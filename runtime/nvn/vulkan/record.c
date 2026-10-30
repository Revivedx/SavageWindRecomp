// The recorder thread: see record.h.
//
// The queue thread translated the game's command lists and made every Vulkan call itself; where it limits the frame
// rate (Kakariko and Gerudo on a Radeon 660M, the GPU at 88-96 %), a quarter of its time was RADV's: building the GPU
// commands behind vkCmd*, writing descriptor sets, the submissions. That work depends on nothing the thread decides
// next. So the translating thread now writes each call's arguments into a packet in a ring, in the order it made them,
// and this thread makes the real calls in that order while the translator goes on. A packet holds everything the call
// needs, by value: the recorder never reads the translator's state or the game's memory.
//
// Shared state and how it is protected (T = the translating thread(s), one at a time under gpu_lock; R = the recorder):
//   ring bytes          written by T before `head` is published (release), read by R after it loads `head`
//                       (acquire); reused by T only past `tail` (R's release store after each packet, T's acquire)
//   head / tail         _Atomic; the only hand-off. `wpos` (T's unpublished end) is T's alone
//   wake-up             R sets `asleep` under `mu` and re-checks `head` before waiting on cv_work (seq_cst stores and
//                       loads on both sides: no lost wake-up); T signals only when it sees `asleep`
//   submitted           _Atomic, stored by R after vkQueueSubmit2 returns (release); anyone reads it
//   cv_done             R broadcasts under `mu` after each submission and whenever it catches up: waiters for a
//                       submission, a drain or ring room sleep on it (timed, so a late broadcast costs at most a tick)
//   command buffers     allocated once, before R starts (exec.c vk_exec_init); from then on only R begins, records,
//   and their pool      ends and submits them. T records into one directly only after rec_drain
//   descriptor sets     allocated and their pools reset by T (exec.c), written and bound by R. Allocation and writes
//                       of other sets of a pool may run at the same time (only the set written is externally
//                       synchronized); a slot's pools are reset once its submission is done on the GPU, which implies
//                       R submitted it, which implies R made every call before it (packets run in order)
//   the queue           R's alone (submissions); T uses it only after rec_drain. Presents go on device.c's own queue
//                       once rec_wait_submitted says the submission that signals their semaphore was made
//   Vulkan objects in   images, views, buffers, samplers, pipelines, layouts, query pools: created by T (or the build
//   packets             workers) before the packet that names them; destroyed by T only once the submission that could
//                       use them is done on the GPU - again after R recorded and submitted it
//   stats               RecStats: T's fields plain, R's and the waits' _Atomic relaxed adds; read by T
#include "record.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../platform.h"

// ================================================================== the ring
typedef void (*PktFn)(const void *args);
typedef struct { PktFn fn; uint32_t size; uint32_t pad; } PktHdr;  // fn NULL: skip to the ring's start
#define PKT_ALIGN 16
#define PUBLISH_EVERY (4u << 10)  // bytes queued before T publishes them (and at every submission and wait)

static uint8_t *ring;
static uint64_t ring_cap;  // (a power of two)
static _Atomic uint64_t head;  // (T) published end
static _Atomic uint64_t tail;  // (R) recorded end
static uint64_t wpos, published;  // (T) written end, the end last published
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
static _Atomic int asleep;
static _Atomic uint64_t submitted;
static int on = -1;  // 1: the thread records, 0: calls at once, -1: not started
static bool prof, prof_enq;  // SAVAGEWIND_VK_PROF; the time queuing each packet only when it isn't 2 (two clock reads a packet)
static RecStats st_t;  // T's counters
static _Atomic uint64_t r_busy_ns, r_wakeups, r_submit_ns;
static _Atomic uint64_t a_seq_waits, a_seq_wait_ns;  // (waits from any thread)

bool rec_on(void) { return on > 0; }

#define NOW() plat_now_ns()
#define ADD(v, x) atomic_fetch_add_explicit(&(v), (x), memory_order_relaxed)

static void timed_wait(pthread_cond_t *cv, uint64_t ns) {  // (mu held)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(cv, &mu, &ts);
}

// T: what it wrote so far is the recorder's to run
static void publish(void) {
    if (published == wpos) return;
    published = wpos;
    atomic_store_explicit(&head, wpos, memory_order_seq_cst);
    if (atomic_load_explicit(&asleep, memory_order_seq_cst)) {
        pthread_mutex_lock(&mu);
        pthread_cond_signal(&cv_work);
        pthread_mutex_unlock(&mu);
    }
}

// T: room for `need` bytes at wpos (the recorder frees room as it records; a full ring means it is behind by its
// whole size, 16 MB of packets, so the wait is rare)
static void wait_room(uint64_t need) {
    if (wpos + need - atomic_load_explicit(&tail, memory_order_acquire) <= ring_cap) return;
    publish();
    uint64_t t0 = NOW();
    pthread_mutex_lock(&mu);
    while (wpos + need - atomic_load_explicit(&tail, memory_order_acquire) > ring_cap) timed_wait(&cv_done, 1000000);
    pthread_mutex_unlock(&mu);
    st_t.full_waits++;
    st_t.full_ns += NOW() - t0;
}

// T: a packet for fn with `args` bytes of arguments (+ `extra` bytes of arrays after them, each caller aligns its
// own); returns the arguments' place. Committed with commit() once filled.
static void *push(PktFn fn, size_t args, size_t extra) {
    uint64_t need = (sizeof(PktHdr) + ((args + PKT_ALIGN - 1) & ~(size_t)(PKT_ALIGN - 1)) + extra + PKT_ALIGN - 1) & ~(uint64_t)(PKT_ALIGN - 1);
    if (need > ring_cap / 4) FATAL("[vk] recorder: a packet of %llu bytes does not fit the ring\n", (unsigned long long)need);
    uint64_t at = wpos & (ring_cap - 1);
    if (at + need > ring_cap) {  // no room before the end: a skip packet, the real one at the start
        uint64_t skip = ring_cap - at;
        wait_room(skip);
        PktHdr *s = (PktHdr *)(ring + at);
        s->fn = NULL;
        s->size = (uint32_t)skip;
        wpos += skip;
        at = 0;
    }
    wait_room(need);
    PktHdr *h = (PktHdr *)(ring + at);
    h->fn = fn;
    h->size = (uint32_t)need;
    wpos += need;
    st_t.packets++;
    st_t.bytes += need;
    return h + 1;
}
static void commit(void) {
    if (wpos - published >= PUBLISH_EVERY) publish();
}
// (the arrays of a packet: after its arguments, 16-aligned)
#define AL16(n) (((n) + 15) & ~(size_t)15)
static inline void *after(const void *args, size_t args_size) { return (uint8_t *)args + AL16(args_size); }

// ---- the thread
static void *run(void *arg) {
    (void)arg;
    uint64_t t = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&head, memory_order_acquire);
        if (h == t) {
            pthread_mutex_lock(&mu);
            atomic_store_explicit(&asleep, 1, memory_order_seq_cst);
            while ((h = atomic_load_explicit(&head, memory_order_seq_cst)) == t) pthread_cond_wait(&cv_work, &mu);
            atomic_store_explicit(&asleep, 0, memory_order_relaxed);
            pthread_mutex_unlock(&mu);
            ADD(r_wakeups, 1);
        }
        uint64_t t0 = prof ? NOW() : 0;
        while (t != h) {
            const PktHdr *p = (const PktHdr *)(ring + (t & (ring_cap - 1)));
            if (p->fn) p->fn(p + 1);
            t += p->size;
            atomic_store_explicit(&tail, t, memory_order_release);
        }
        if (prof) ADD(r_busy_ns, NOW() - t0);
        pthread_mutex_lock(&mu);  // caught up: drains and waiters
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

void rec_init(void) {
    if (on >= 0) return;
    prof = getenv("SAVAGEWIND_VK_PROF") != NULL;
    prof_enq = prof && atoi(getenv("SAVAGEWIND_VK_PROF")) != 2;
    const char *e = getenv("SAVAGEWIND_VK_RECORDER");
    int want = !e || atoi(e) != 0;
    // ARM's Mali drivers: LAS Recomp lost the GPU a few seconds into its title screen with the recorder thread on
    // (Mali-G610, LAS #358) and ran fine with it off; SAVAGEWIND_VK_RECORDER=1 still turns it on there
    bool mali = g_vk.props.vendorID == 0x13B5;
    if (mali && !e) want = 0;
    on = 0;
    if (want) {
        const char *mb = getenv("SAVAGEWIND_VK_RECORDER_MB");
        uint64_t cap = (uint64_t)(mb && atoi(mb) > 0 ? atoi(mb) : 16) << 20;
        while (cap & (cap - 1)) cap &= cap - 1;  // (a power of two)
        ring = malloc(cap);
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1 << 20);
        ring_cap = cap;  // (before the thread exists)
        if (!ring || pthread_create(&th, &attr, run, NULL) != 0) {
            LOG("[vk] recorder thread: could not start (%s): Vulkan calls on the queue thread\n", ring ? "thread" : "memory");
            free(ring);
            ring = NULL;
            ring_cap = 0;
        } else {
            pthread_detach(th);
            on = 1;
        }
        pthread_attr_destroy(&attr);
    }
    LOG("[vk] recorder thread %s%s\n", on ? "on" : "off", mali && !e ? " (Mali driver)" : "");
}

// ================================================================== waits
// A wait on the recorder this long (a quarter second) is logged: the recorder was stuck in a driver call (a slow
// vkQueueSubmit2 is logged too, just before) or the whole process stalled
static void slow_wait(const char *what, uint64_t ns) {
    if (ns <= 250000000) return;
    static _Atomic int n;
    if (atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 20) LOG("[vk] recorder: a wait for %s took %llu ms\n", what, (unsigned long long)(ns / 1000000));
}

uint64_t rec_submitted(void) { return atomic_load_explicit(&submitted, memory_order_acquire); }

void rec_wait_submitted(uint64_t seq) {
    if (on <= 0 || rec_submitted() >= seq) return;
    uint64_t t0 = NOW();
    pthread_mutex_lock(&mu);
    while (rec_submitted() < seq) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
    uint64_t dt = NOW() - t0;
    ADD(a_seq_waits, 1);
    ADD(a_seq_wait_ns, dt);
    slow_wait("a submission", dt);
}

void rec_drain(void) {
    if (on <= 0) return;
    publish();
    uint64_t upto = wpos;
    if (atomic_load_explicit(&tail, memory_order_acquire) >= upto) return;
    uint64_t t0 = NOW();
    pthread_mutex_lock(&mu);
    while (atomic_load_explicit(&tail, memory_order_acquire) < upto) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
    uint64_t dt = NOW() - t0;
    st_t.drains++;
    st_t.drain_ns += dt;
    slow_wait("a drain", dt);
}

// ================================================================== stats
void rec_stats_take(RecStats *o) {
    *o = st_t;
    memset(&st_t, 0, sizeof st_t);
    o->busy_ns = atomic_exchange_explicit(&r_busy_ns, 0, memory_order_relaxed);
    o->wakeups = atomic_exchange_explicit(&r_wakeups, 0, memory_order_relaxed);
    o->submit_ns = atomic_exchange_explicit(&r_submit_ns, 0, memory_order_relaxed);
    o->seq_waits = atomic_exchange_explicit(&a_seq_waits, 0, memory_order_relaxed);
    o->seq_wait_ns = atomic_exchange_explicit(&a_seq_wait_ns, 0, memory_order_relaxed);
}

// (T) time spent queuing: around every rec_* that queues, when profiling in detail
#define ENQ_T0() uint64_t enq_t0_ = prof_enq ? NOW() : 0
#define ENQ_END() do { if (prof_enq) st_t.enqueue_ns += NOW() - enq_t0_; } while (0)

// ================================================================== descriptor writes
static bool write_is_image(const VkWriteDescriptorSet *w) {
    switch (w->descriptorType) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return true;
    default: return false;
    }
}
typedef union { VkDescriptorBufferInfo b; VkDescriptorImageInfo i; } WriteInfo;
typedef struct { uint32_t n; } UpdateArgs;  // then: n VkWriteDescriptorSet, then n WriteInfo
static void update_sets_run(const void *p) {
    const UpdateArgs *a = p;
    VkWriteDescriptorSet *w = after(a, sizeof *a);
    const WriteInfo *inf = after(w, a->n * sizeof *w);
    for (uint32_t i = 0; i < a->n; i++) {
        if (write_is_image(&w[i])) w[i].pImageInfo = &inf[i].i;
        else w[i].pBufferInfo = &inf[i].b;
    }
    vkUpdateDescriptorSets(g_vk.dev, a->n, w, 0, NULL);
}
void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w) {
    if (on <= 0) { vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL); return; }
    ENQ_T0();
    UpdateArgs *a = push(update_sets_run, sizeof *a, AL16(n * sizeof *w) + n * sizeof(WriteInfo));
    a->n = n;
    VkWriteDescriptorSet *pw = after(a, sizeof *a);
    WriteInfo *inf = after(pw, n * sizeof *pw);
    for (uint32_t i = 0; i < n; i++) {
        if (w[i].descriptorCount != 1 || w[i].pNext || w[i].pTexelBufferView)
            FATAL("[vk] recorder: a descriptor write it can't carry (%u descriptors)\n", w[i].descriptorCount);
        pw[i] = w[i];
        if (write_is_image(&w[i])) inf[i].i = *w[i].pImageInfo;
        else inf[i].b = *w[i].pBufferInfo;
        pw[i].pImageInfo = NULL; pw[i].pBufferInfo = NULL;  // (set again when run)
    }
    commit();
    ENQ_END();
}

// ================================================================== command buffers, submissions
static void begin_cb_run(const void *p) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*(const VkCommandBuffer *)p, &bi));
}
void rec_BeginCommandBuffer(VkCommandBuffer cb) {
    if (on <= 0) { begin_cb_run(&cb); return; }
    ENQ_T0();
    *(VkCommandBuffer *)push(begin_cb_run, sizeof cb, 0) = cb;
    commit();
    ENQ_END();
}
static void end_cb_run(const void *p) { VK_CHECK(vkEndCommandBuffer(*(const VkCommandBuffer *)p)); }
void rec_EndCommandBuffer(VkCommandBuffer cb) {
    if (on <= 0) { end_cb_run(&cb); return; }
    ENQ_T0();
    *(VkCommandBuffer *)push(end_cb_run, sizeof cb, 0) = cb;
    commit();
    ENQ_END();
}

typedef struct {
    VkCommandBufferSubmitInfo cbs[2];
    VkSemaphoreSubmitInfo wait, sig[2];
    uint32_t ncb, nwait, nsig;
    uint64_t seq;
} SubmitArgs;
static void submit_run(const void *p) {
    const SubmitArgs *a = p;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = a->ncb, .pCommandBufferInfos = a->cbs,
                        .signalSemaphoreInfoCount = a->nsig, .pSignalSemaphoreInfos = a->sig,
                        .waitSemaphoreInfoCount = a->nwait, .pWaitSemaphoreInfos = &a->wait};
    uint64_t t0 = NOW();
    VkResult r = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    uint64_t dt = NOW() - t0;
    if (prof) ADD(r_submit_ns, dt);
    if (dt > 50000000) {  // (a driver stall: a slow wait on the recorder shows where it was)
        static int n;
        if (n++ < 20) LOG("[vk] vkQueueSubmit2 of submission %llu took %llu ms\n", (unsigned long long)a->seq, (unsigned long long)(dt / 1000000));
    }
    if (r == VK_ERROR_DEVICE_LOST) { extern void vk_devlost_dump(void); vk_devlost_dump(); }
    VK_CHECK(r);
    atomic_store_explicit(&submitted, a->seq, memory_order_release);
    if (on > 0) {
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
}
void rec_QueueSubmit2(const VkSubmitInfo2 *si, uint64_t seq) {
    SubmitArgs a = {.ncb = si->commandBufferInfoCount, .nwait = si->waitSemaphoreInfoCount, .nsig = si->signalSemaphoreInfoCount, .seq = seq};
    if (a.ncb > 2 || a.nwait > 1 || a.nsig > 2 || si->pNext) FATAL("[vk] recorder: a submission it can't carry\n");
    for (uint32_t i = 0; i < a.ncb; i++) a.cbs[i] = si->pCommandBufferInfos[i];
    if (a.nwait) a.wait = si->pWaitSemaphoreInfos[0];
    for (uint32_t i = 0; i < a.nsig; i++) a.sig[i] = si->pSignalSemaphoreInfos[i];
    if (on <= 0) { submit_run(&a); return; }
    ENQ_T0();
    *(SubmitArgs *)push(submit_run, sizeof a, 0) = a;
    publish();  // (whoever waits for the submission must not wait for the next batch)
    ENQ_END();
}

// ================================================================== commands
// Scalar arguments: a struct of the command buffer and the values, made by a macro from the parameter list.
#define CMD_ARGS(name, ...) typedef struct { VkCommandBuffer cb; __VA_ARGS__ } name##Args;
#define CMD_QUEUE(name, ...) \
    do { if (on <= 0) { vk##name(__VA_ARGS__); return; } ENQ_T0(); name##Args *a_ = push(name##_run, sizeof *a_, 0); \
         *a_ = (name##Args){__VA_ARGS__}; commit(); ENQ_END(); } while (0)

CMD_ARGS(CmdDispatch, uint32_t x, y, z;)
static void CmdDispatch_run(const void *p) { const CmdDispatchArgs *a = p; vkCmdDispatch(a->cb, a->x, a->y, a->z); }
void rec_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { CMD_QUEUE(CmdDispatch, cb, x, y, z); }

CMD_ARGS(CmdDraw, uint32_t count, inst, first, first_inst;)
static void CmdDraw_run(const void *p) { const CmdDrawArgs *a = p; vkCmdDraw(a->cb, a->count, a->inst, a->first, a->first_inst); }
void rec_CmdDraw(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst) {
    CMD_QUEUE(CmdDraw, cb, count, inst, first, first_inst);
}

CMD_ARGS(CmdDrawIndexed, uint32_t count, inst, first; int32_t base; uint32_t first_inst;)
static void CmdDrawIndexed_run(const void *p) {
    const CmdDrawIndexedArgs *a = p;
    vkCmdDrawIndexed(a->cb, a->count, a->inst, a->first, a->base, a->first_inst);
}
void rec_CmdDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst) {
    CMD_QUEUE(CmdDrawIndexed, cb, count, inst, first, base, first_inst);
}

CMD_ARGS(CmdBindPipeline, VkPipelineBindPoint bp; VkPipeline p;)
static void CmdBindPipeline_run(const void *p) { const CmdBindPipelineArgs *a = p; vkCmdBindPipeline(a->cb, a->bp, a->p); }
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) { CMD_QUEUE(CmdBindPipeline, cb, bp, p); }

CMD_ARGS(CmdBindIndexBuffer, VkBuffer buf; VkDeviceSize off; VkIndexType type;)
static void CmdBindIndexBuffer_run(const void *p) { const CmdBindIndexBufferArgs *a = p; vkCmdBindIndexBuffer(a->cb, a->buf, a->off, a->type); }
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) { CMD_QUEUE(CmdBindIndexBuffer, cb, buf, off, type); }

CMD_ARGS(CmdEndRendering, char pad_;)
static void CmdEndRendering_run(const void *p) { vkCmdEndRendering(((const CmdEndRenderingArgs *)p)->cb); }
void rec_CmdEndRendering(VkCommandBuffer cb) {
    if (on <= 0) { vkCmdEndRendering(cb); return; }
    ENQ_T0();
    ((CmdEndRenderingArgs *)push(CmdEndRendering_run, sizeof(CmdEndRenderingArgs), 0))->cb = cb;
    commit();
    ENQ_END();
}

CMD_ARGS(CmdSetLineWidth, float w;)
static void CmdSetLineWidth_run(const void *p) { const CmdSetLineWidthArgs *a = p; vkCmdSetLineWidth(a->cb, a->w); }
void rec_CmdSetLineWidth(VkCommandBuffer cb, float w) { CMD_QUEUE(CmdSetLineWidth, cb, w); }

CMD_ARGS(CmdSetDepthBias, float c, k, s;)
static void CmdSetDepthBias_run(const void *p) { const CmdSetDepthBiasArgs *a = p; vkCmdSetDepthBias(a->cb, a->c, a->k, a->s); }
void rec_CmdSetDepthBias(VkCommandBuffer cb, float c, float k, float s) { CMD_QUEUE(CmdSetDepthBias, cb, c, k, s); }

#define CMD_ONE(name, T) \
    CMD_ARGS(name, T v;) \
    static void name##_run(const void *p) { const name##Args *a = p; vk##name(a->cb, a->v); } \
    void rec_##name(VkCommandBuffer cb, T v) { CMD_QUEUE(name, cb, v); }
CMD_ONE(CmdSetDepthBiasEnable, VkBool32)
CMD_ONE(CmdSetDepthTestEnable, VkBool32)
CMD_ONE(CmdSetDepthWriteEnable, VkBool32)
CMD_ONE(CmdSetStencilTestEnable, VkBool32)
CMD_ONE(CmdSetPrimitiveRestartEnable, VkBool32)
CMD_ONE(CmdSetDepthCompareOp, VkCompareOp)
CMD_ONE(CmdSetCullMode, VkCullModeFlags)
CMD_ONE(CmdSetFrontFace, VkFrontFace)
CMD_ONE(CmdSetPrimitiveTopology, VkPrimitiveTopology)

#define CMD_FACE_U32(name) \
    CMD_ARGS(name, VkStencilFaceFlags face; uint32_t v;) \
    static void name##_run(const void *p) { const name##Args *a = p; vk##name(a->cb, a->face, a->v); } \
    void rec_##name(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v) { CMD_QUEUE(name, cb, face, v); }
CMD_FACE_U32(CmdSetStencilCompareMask)
CMD_FACE_U32(CmdSetStencilWriteMask)
CMD_FACE_U32(CmdSetStencilReference)

CMD_ARGS(CmdSetStencilOp, VkStencilFaceFlags face; VkStencilOp fail, pass, dfail; VkCompareOp cmp;)
static void CmdSetStencilOp_run(const void *p) {
    const CmdSetStencilOpArgs *a = p;
    vkCmdSetStencilOp(a->cb, a->face, a->fail, a->pass, a->dfail, a->cmp);
}
void rec_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp) {
    CMD_QUEUE(CmdSetStencilOp, cb, face, fail, pass, dfail, cmp);
}

CMD_ARGS(CmdResetQueryPool, VkQueryPool pool; uint32_t first, n;)
static void CmdResetQueryPool_run(const void *p) { const CmdResetQueryPoolArgs *a = p; vkCmdResetQueryPool(a->cb, a->pool, a->first, a->n); }
void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n) { CMD_QUEUE(CmdResetQueryPool, cb, pool, first, n); }

CMD_ARGS(CmdWriteTimestamp2, VkPipelineStageFlags2 stage; VkQueryPool pool; uint32_t query;)
static void CmdWriteTimestamp2_run(const void *p) { const CmdWriteTimestamp2Args *a = p; vkCmdWriteTimestamp2(a->cb, a->stage, a->pool, a->query); }
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    CMD_QUEUE(CmdWriteTimestamp2, cb, stage, pool, query);
}

CMD_ARGS(CmdBeginQuery, VkQueryPool pool; uint32_t query; VkQueryControlFlags flags;)
static void CmdBeginQuery_run(const void *p) { const CmdBeginQueryArgs *a = p; vkCmdBeginQuery(a->cb, a->pool, a->query, a->flags); }
void rec_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
    CMD_QUEUE(CmdBeginQuery, cb, pool, query, flags);
}

CMD_ARGS(CmdEndQuery, VkQueryPool pool; uint32_t query;)
static void CmdEndQuery_run(const void *p) { const CmdEndQueryArgs *a = p; vkCmdEndQuery(a->cb, a->pool, a->query); }
void rec_CmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query) { CMD_QUEUE(CmdEndQuery, cb, pool, query); }

CMD_ARGS(CmdSetBlendConstants, float c[4];)
static void CmdSetBlendConstants_run(const void *p) { const CmdSetBlendConstantsArgs *a = p; vkCmdSetBlendConstants(a->cb, a->c); }
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (on <= 0) { vkCmdSetBlendConstants(cb, c); return; }
    ENQ_T0();
    CmdSetBlendConstantsArgs *a = push(CmdSetBlendConstants_run, sizeof *a, 0);
    a->cb = cb;
    memcpy(a->c, c, sizeof a->c);
    commit();
    ENQ_END();
}

// ---- arrays: the packet's arguments, then the array(s) after them
CMD_ARGS(CmdBindVertexBuffers, uint32_t first, n;)  // then VkBuffer[n], VkDeviceSize[n]
static void CmdBindVertexBuffers_run(const void *p) {
    const CmdBindVertexBuffersArgs *a = p;
    const VkBuffer *b = after(a, sizeof *a);
    const VkDeviceSize *o = after(b, a->n * sizeof *b);
    vkCmdBindVertexBuffers(a->cb, a->first, a->n, b, o);
}
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs) {
    if (on <= 0) { vkCmdBindVertexBuffers(cb, first, n, bufs, offs); return; }
    ENQ_T0();
    CmdBindVertexBuffersArgs *a = push(CmdBindVertexBuffers_run, sizeof *a, AL16(n * sizeof *bufs) + n * sizeof *offs);
    *a = (CmdBindVertexBuffersArgs){cb, first, n};
    VkBuffer *b = after(a, sizeof *a);
    memcpy(b, bufs, n * sizeof *bufs);
    memcpy(after(b, n * sizeof *b), offs, n * sizeof *offs);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdBindDescriptorSets, VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t first, n, ndyn;)  // then sets[n], dyn[ndyn]
static void CmdBindDescriptorSets_run(const void *p) {
    const CmdBindDescriptorSetsArgs *a = p;
    const VkDescriptorSet *s = after(a, sizeof *a);
    const uint32_t *d = after(s, a->n * sizeof *s);
    vkCmdBindDescriptorSets(a->cb, a->bp, a->layout, a->first, a->n, s, a->ndyn, a->ndyn ? d : NULL);
}
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                               const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn) {
    if (on <= 0) { vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn); return; }
    ENQ_T0();
    CmdBindDescriptorSetsArgs *a = push(CmdBindDescriptorSets_run, sizeof *a, AL16(n * sizeof *sets) + ndyn * sizeof *dyn);
    *a = (CmdBindDescriptorSetsArgs){cb, bp, layout, first, n, ndyn};
    VkDescriptorSet *s = after(a, sizeof *a);
    memcpy(s, sets, n * sizeof *sets);
    if (ndyn) memcpy(after(s, n * sizeof *s), dyn, ndyn * sizeof *dyn);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdPushConstants, VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t off, size;)  // then the bytes
static void CmdPushConstants_run(const void *p) {
    const CmdPushConstantsArgs *a = p;
    vkCmdPushConstants(a->cb, a->layout, a->stages, a->off, a->size, after(a, sizeof *a));
}
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size, const void *data) {
    if (on <= 0) { vkCmdPushConstants(cb, layout, stages, off, size, data); return; }
    ENQ_T0();
    CmdPushConstantsArgs *a = push(CmdPushConstants_run, sizeof *a, size);
    *a = (CmdPushConstantsArgs){cb, layout, stages, off, size};
    memcpy(after(a, sizeof *a), data, size);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdSetViewport, uint32_t first, n;)  // then VkViewport[n]
static void CmdSetViewport_run(const void *p) { const CmdSetViewportArgs *a = p; vkCmdSetViewport(a->cb, a->first, a->n, after(a, sizeof *a)); }
void rec_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    if (on <= 0) { vkCmdSetViewport(cb, first, n, v); return; }
    ENQ_T0();
    CmdSetViewportArgs *a = push(CmdSetViewport_run, sizeof *a, n * sizeof *v);
    *a = (CmdSetViewportArgs){cb, first, n};
    memcpy(after(a, sizeof *a), v, n * sizeof *v);
    commit();
    ENQ_END();
}
CMD_ARGS(CmdSetScissor, uint32_t first, n;)  // then VkRect2D[n]
static void CmdSetScissor_run(const void *p) { const CmdSetScissorArgs *a = p; vkCmdSetScissor(a->cb, a->first, a->n, after(a, sizeof *a)); }
void rec_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    if (on <= 0) { vkCmdSetScissor(cb, first, n, r); return; }
    ENQ_T0();
    CmdSetScissorArgs *a = push(CmdSetScissor_run, sizeof *a, n * sizeof *r);
    *a = (CmdSetScissorArgs){cb, first, n};
    memcpy(after(a, sizeof *a), r, n * sizeof *r);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdClearAttachments, uint32_t na, nr;)  // then VkClearAttachment[na], VkClearRect[nr]
static void CmdClearAttachments_run(const void *p) {
    const CmdClearAttachmentsArgs *a = p;
    const VkClearAttachment *at = after(a, sizeof *a);
    vkCmdClearAttachments(a->cb, a->na, at, a->nr, after(at, a->na * sizeof *at));
}
void rec_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    if (on <= 0) { vkCmdClearAttachments(cb, na, at, nr, r); return; }
    ENQ_T0();
    CmdClearAttachmentsArgs *a = push(CmdClearAttachments_run, sizeof *a, AL16(na * sizeof *at) + nr * sizeof *r);
    *a = (CmdClearAttachmentsArgs){cb, na, nr};
    VkClearAttachment *pa = after(a, sizeof *a);
    memcpy(pa, at, na * sizeof *at);
    memcpy(after(pa, na * sizeof *pa), r, nr * sizeof *r);
    commit();
    ENQ_END();
}

// Image clears, copies, blits, resolves: two handles, two layouts, a clear value, a count of regions and a filter;
// the regions follow.
typedef struct {
    VkCommandBuffer cb;
    uint64_t h1, h2;
    VkImageLayout l1, l2;
    uint32_t n;
    VkFilter filter;
    union { VkClearColorValue color; VkClearDepthStencilValue ds; } clear;
} XferArgs;
#define XFER_REGIONS(a) after((a), sizeof *(a))
static void copy_buffer_run(const void *p) { const XferArgs *a = p; vkCmdCopyBuffer(a->cb, (VkBuffer)a->h1, (VkBuffer)a->h2, a->n, XFER_REGIONS(a)); }
static void copy_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void blit_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdBlitImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a), a->filter);
}
static void resolve_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdResolveImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void buffer_to_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyBufferToImage(a->cb, (VkBuffer)a->h1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void image_to_buffer_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyImageToBuffer(a->cb, (VkImage)a->h1, a->l1, (VkBuffer)a->h2, a->n, XFER_REGIONS(a));
}
static void clear_color_run(const void *p) {
    const XferArgs *a = p;
    vkCmdClearColorImage(a->cb, (VkImage)a->h1, a->l1, &a->clear.color, a->n, XFER_REGIONS(a));
}
static void clear_ds_run(const void *p) {
    const XferArgs *a = p;
    vkCmdClearDepthStencilImage(a->cb, (VkImage)a->h1, a->l1, &a->clear.ds, a->n, XFER_REGIONS(a));
}
// queues fn with the arguments and n regions of `size` bytes each (the recorder on)
static void xfer(PktFn fn, const XferArgs *x, const void *regions, size_t size) {
    ENQ_T0();
    XferArgs *a = push(fn, sizeof *a, x->n * size);
    *a = *x;
    memcpy(XFER_REGIONS(a), regions, x->n * size);
    commit();
    ENQ_END();
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    if (on <= 0) { vkCmdCopyBuffer(cb, src, dst, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .n = n};
    xfer(copy_buffer_run, &x, r, sizeof *r);
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    if (on <= 0) { vkCmdCopyImage(cb, src, sl, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n};
    xfer(copy_image_run, &x, r, sizeof *r);
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    if (on <= 0) { vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n, .filter = f};
    xfer(blit_image_run, &x, r, sizeof *r);
}
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    if (on <= 0) { vkCmdResolveImage(cb, src, sl, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n};
    xfer(resolve_image_run, &x, r, sizeof *r);
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) {
    if (on <= 0) { vkCmdCopyBufferToImage(cb, src, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l2 = dl, .n = n};
    xfer(buffer_to_image_run, &x, r, sizeof *r);
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    if (on <= 0) { vkCmdCopyImageToBuffer(cb, src, sl, dst, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .n = n};
    xfer(image_to_buffer_run, &x, r, sizeof *r);
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    if (on <= 0) { vkCmdClearColorImage(cb, img, l, c, n, rg); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)img, .l1 = l, .n = n, .clear.color = *c};
    xfer(clear_color_run, &x, rg, sizeof *rg);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n,
                                   const VkImageSubresourceRange *rg) {
    if (on <= 0) { vkCmdClearDepthStencilImage(cb, img, l, c, n, rg); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)img, .l1 = l, .n = n, .clear.ds = *c};
    xfer(clear_ds_run, &x, rg, sizeof *rg);
}

// ---- barriers: the dependency with its three barrier arrays after it (nothing chained: pNext must be NULL)
CMD_ARGS(CmdPipelineBarrier2, VkDependencyInfo di;)  // then memory[], buffer[], image[] barriers
static void CmdPipelineBarrier2_run(const void *p) {
    const CmdPipelineBarrier2Args *a = p;
    VkDependencyInfo di = a->di;
    const VkMemoryBarrier2 *m = after(a, sizeof *a);
    const VkBufferMemoryBarrier2 *b = after(m, di.memoryBarrierCount * sizeof *m);
    const VkImageMemoryBarrier2 *im = after(b, di.bufferMemoryBarrierCount * sizeof *b);
    di.pMemoryBarriers = di.memoryBarrierCount ? m : NULL;
    di.pBufferMemoryBarriers = di.bufferMemoryBarrierCount ? b : NULL;
    di.pImageMemoryBarriers = di.imageMemoryBarrierCount ? im : NULL;
    vkCmdPipelineBarrier2(a->cb, &di);
}
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    if (on <= 0) { vkCmdPipelineBarrier2(cb, di); return; }
    if (di->pNext) FATAL("[vk] recorder: a chained dependency\n");
    for (uint32_t i = 0; i < di->memoryBarrierCount; i++) if (di->pMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) if (di->pBufferMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) if (di->pImageMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    ENQ_T0();
    size_t sm = di->memoryBarrierCount * sizeof(VkMemoryBarrier2), sb = di->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2);
    size_t si = di->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2);
    CmdPipelineBarrier2Args *a = push(CmdPipelineBarrier2_run, sizeof *a, AL16(sm) + AL16(sb) + si);
    a->cb = cb;
    a->di = *di;
    a->di.pMemoryBarriers = NULL; a->di.pBufferMemoryBarriers = NULL; a->di.pImageMemoryBarriers = NULL;
    uint8_t *m = after(a, sizeof *a);
    if (sm) memcpy(m, di->pMemoryBarriers, sm);
    uint8_t *b = after(m, sm);
    if (sb) memcpy(b, di->pBufferMemoryBarriers, sb);
    if (si) memcpy(after(b, sb), di->pImageMemoryBarriers, si);
    commit();
    ENQ_END();
}

// ---- rendering: the info, its colour attachments, then the depth and stencil ones. Chained: only a colour
// attachment's VkAttachmentFeedbackLoopInfoEXT (exec.c begin_pass, VK_KHR_unified_image_layouts), carried as a mask
CMD_ARGS(CmdBeginRendering, VkRenderingInfo ri; VkRenderingAttachmentInfo depth, stencil; uint8_t has_depth, has_stencil; uint16_t fbl;)  // then colour[]
static void CmdBeginRendering_run(const void *p) {
    const CmdBeginRenderingArgs *a = p;
    VkRenderingInfo ri = a->ri;
    VkRenderingAttachmentInfo ca[16];
#ifdef VK_KHR_unified_image_layouts
    VkAttachmentFeedbackLoopInfoEXT fb[16];
#endif
    ri.pColorAttachments = ri.colorAttachmentCount ? after(a, sizeof *a) : NULL;
    if (a->fbl) {
        memcpy(ca, ri.pColorAttachments, ri.colorAttachmentCount * sizeof *ca);
#ifdef VK_KHR_unified_image_layouts
        for (uint32_t i = 0; i < ri.colorAttachmentCount; i++)
            if (a->fbl & (1u << i)) {
                fb[i] = (VkAttachmentFeedbackLoopInfoEXT){.sType = VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT,
                                                         .feedbackLoopEnable = VK_TRUE};
                ca[i].pNext = &fb[i];
            }
#endif
        ri.pColorAttachments = ca;
    }
    ri.pDepthAttachment = a->has_depth ? &a->depth : NULL;
    ri.pStencilAttachment = a->has_stencil ? &a->stencil : NULL;
    vkCmdBeginRendering(a->cb, &ri);
}
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (on <= 0) { vkCmdBeginRendering(cb, ri); return; }
    if (ri->pNext) FATAL("[vk] recorder: a chained rendering info\n");
    if (ri->colorAttachmentCount > 16) FATAL("[vk] recorder: %u colour attachments\n", ri->colorAttachmentCount);
    uint16_t fbl = 0;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
        const VkBaseInStructure *n = ri->pColorAttachments[i].pNext;
        if (!n) continue;
#ifdef VK_KHR_unified_image_layouts
        if (n->sType == VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT && !n->pNext &&
            ((const VkAttachmentFeedbackLoopInfoEXT *)n)->feedbackLoopEnable) { fbl |= (uint16_t)(1u << i); continue; }
#endif
        FATAL("[vk] recorder: a chained attachment\n");
    }
    if ((ri->pDepthAttachment && ri->pDepthAttachment->pNext) || (ri->pStencilAttachment && ri->pStencilAttachment->pNext))
        FATAL("[vk] recorder: a chained attachment\n");
    ENQ_T0();
    size_t sc = ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo);
    CmdBeginRenderingArgs *a = push(CmdBeginRendering_run, sizeof *a, sc);
    a->cb = cb;
    a->ri = *ri;
    a->ri.pColorAttachments = NULL; a->ri.pDepthAttachment = NULL; a->ri.pStencilAttachment = NULL;
    a->has_depth = ri->pDepthAttachment != NULL;
    a->has_stencil = ri->pStencilAttachment != NULL;
    if (a->has_depth) a->depth = *ri->pDepthAttachment;
    if (a->has_stencil) a->stencil = *ri->pStencilAttachment;
    a->fbl = fbl;
    if (sc) memcpy(after(a, sizeof *a), ri->pColorAttachments, sc);
    if (fbl) for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) ((VkRenderingAttachmentInfo *)after(a, sizeof *a))[i].pNext = NULL;
    commit();
    ENQ_END();
}
