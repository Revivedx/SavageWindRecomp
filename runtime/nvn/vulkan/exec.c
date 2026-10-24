// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
#include "../../settings.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_gw_waits, stat_gw_holds;  // draws / dispatches that waited for a storing shader; lists held (gw_check)
static uint64_t stat_uub, stat_uub_same, stat_gcopy_hit, stat_gcopy_miss, stat_gcopy_bytes;  // UpdateUniformBuffer; guest_copy cache
static uint64_t stat_feedback_live;  // depth attachments sampled as they are (read-only feedback, no copy)
static uint64_t stat_feedback_loops, stat_feedback_barriers;  // draws sampling a target they write, in a feedback loop
static bool pass_ds_written;         // a draw of the current pass wrote depth or stencil
static uint8_t pass_color_written;   // the color targets (bits) a draw of the current pass wrote
static uint8_t pass_color_unsynced;  // the color targets drawn or cleared since the pass began or its last feedback barrier
static uint8_t pass_feedback;        // the color targets (bits) the current pass began with a feedback loop enabled
static bool draw_feedback;           // the current draw samples a target it writes (its pipeline allows the loop)
static uint64_t stat_upload_bytes;
static uint64_t stat_sets, stat_sets_reused;  // descriptor sets asked for / shared with an identical one
// SAVAGEWIND_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint64_t cum_xwait;  // (profiling) the part of cum_wait spent inside backend_execute
static __thread bool in_execute;  // (this thread is in backend_execute)
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
// The per-draw timers (draw, pre, key, dyn, sets, tex, vtx) cost ~0.3 us a draw themselves: SAVAGEWIND_VK_PROF=2
// keeps only the whole-list ones (exec, submit, gpu), for timing the draw path itself.
static int profd;
#define PROFD_T0() uint64_t prof_t0_ = profd ? SDL_GetTicksNS() : 0
#define PROFD_ADD(v) do { if (profd) v += SDL_GetTicksNS() - prof_t0_; } while (0)

// Command buffer state draw_ recorded last, so a draw records only what differs (one draw set ~20 pieces of state;
// the open world has ~5000 draws a frame). dyn_epoch changes with each command buffer begun and wherever other code
// binds a graphics pipeline or sets this state (vk_dyn_forget). SAVAGEWIND_VK_NO_DYN_CACHE=1: everything every draw.
static uint64_t dyn_epoch = 1;
static int no_dyn_cache = -1;
static struct {
    uint64_t epoch;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t dt, dw, dop, sten, cull, front, topo, restart, bias_on;
    uint32_t sten_face[2][8];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float blend[4], bias[3], line_w;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ibuf;
    VkDeviceSize ioff;
    uint32_t itype;
    VkBuffer vbuf[17];
    VkDeviceSize voff[17];
} dc;
void vk_dyn_forget(void) { dyn_epoch++; }
static uint64_t setc_gen;  // bumped to drop the shared descriptor sets (set_finish) of the submission in progress
static void dyn_check(void) {  // (after vk_cmd) a new command buffer or foreign state: nothing known
    if (dc.epoch == dyn_epoch) return;
    memset(&dc, 0xFF, sizeof dc);  // matches no value (floats: NaN)
    dc.epoch = dyn_epoch;
}
#define DYN_SAME(field, val) (dc.field == (val) ? true : (dc.field = (val), false))
static _Atomic int stat_compiles;  // programs and pipelines built where first needed
static _Atomic int stat_warm;      // ... found built: by the pre-warm (pso_init) or for other programs with the same stages
static void pso_init(void);
static _Atomic uint64_t prof_c_tr, prof_c_prog, prof_c_pipe, prof_c_comp, prof_c_ntr, prof_c_nprog, prof_c_npipe;  // (VK_PROF) compile ns / counts
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 32  // (several a frame: long command lists go to the GPU in pieces, see begin_pass)
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[64];  // (BOTW 1.8.2 records thousands of draws into one submission)
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static uint32_t chunk_draws;  // draws and dispatches recorded since the last submission
static bool chunk_hold;       // a shader that stores to guest memory was recorded since the last submission

static pthread_mutex_t gpu_mutex;
static pthread_once_t gpu_mutex_once = PTHREAD_ONCE_INIT;
static void gpu_mutex_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gpu_mutex, &a);
    pthread_mutexattr_destroy(&a);
}
void gpu_lock(void) { pthread_once(&gpu_mutex_once, gpu_mutex_init); pthread_mutex_lock(&gpu_mutex); }
void gpu_unlock(void) { pthread_mutex_unlock(&gpu_mutex); }

static void readbacks_apply(void);
uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    // (NVIDIA's vkWaitSemaphores is not cheap even for a value already reached: ~6% of the submission thread went
    // there in the open world, recycling submission slots whose work had long finished)
    if (vk_completed_seq() >= seq) { readbacks_apply(); return; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    if (in_execute) cum_xwait += SDL_GetTicksNS() - t0;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
    readbacks_apply();  // (the CPU may read linear render targets after any wait)
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        dyn_epoch++;
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);

// ---- SAVAGEWIND_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
#define GP_MAX 4096  // (an open-world frame has ~1500 marks)
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// SAVAGEWIND_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with SAVAGEWIND_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("SAVAGEWIND_VK_DEVLOST_TRACE") ? atoi(getenv("SAVAGEWIND_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    chunk_draws = 0;
    chunk_hold = false;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) {
    gpu_lock();
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    gpu_unlock();
}

void renderer_flush(void) {
    gpu_lock();
    renderer_submit();
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    gpu_unlock();
    vk_wait_seq(s);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 64) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per command list: draws binding the same range share the copy, also across the pieces the
// list goes to the GPU in (a shared copy keeps its chunk for the latest piece using it). Each list starts over (the game
// writes the next frame's data in place), and so does any other write to guest memory while a list runs
// (guest_copy_flush). UpdateUniformBuffer (BOTW 1.8.2: ~450 a frame, into ~40 small ranges) drops only the copies over
// the range it wrote: each copy keeps the update count it was made at and which written ranges it covers. (Copies used
// to be shared within one GPU piece of ~300 draws and every UpdateUniformBuffer dropped them all: ~3000 copies, ~15 MB,
// a frame in the open world.)
#define GCOPY_TAB 4096
#define UUB_MAX 64
static struct { uint64_t addr, size, gen, serial, mask; int chunk; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen = 1;
static struct { uint64_t lo, hi, serial; } uubt[UUB_MAX];  // ranges UpdateUniformBuffer wrote, with the last update's count
static int nuub;
static uint64_t uub_serial;
static void guest_copy_flush(void) { __atomic_fetch_add(&gcopy_gen, 1, __ATOMIC_RELAXED); }

// UpdateUniformBuffer wrote [lo, hi)
static void uub_note(uint64_t lo, uint64_t hi) {
    uub_serial++;
    for (int i = 0; i < nuub; i++)
        if (lo < uubt[i].hi && uubt[i].lo < hi) {
            if (lo < uubt[i].lo || hi > uubt[i].hi) {  // (grown: copies made before don't know it covers them)
                if (lo < uubt[i].lo) uubt[i].lo = lo;
                if (hi > uubt[i].hi) uubt[i].hi = hi;
                guest_copy_flush();
            }
            uubt[i].serial = uub_serial;
            return;
        }
    if (nuub < UUB_MAX) uubt[nuub++] = (__typeof__(uubt[0])){lo, hi, uub_serial};
    guest_copy_flush();  // (a range copies made before don't know; or no room: every update drops everything)
}

static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    __typeof__(gcopy[0]) *e = &gcopy[slot];
    uint64_t gen = __atomic_load_n(&gcopy_gen, __ATOMIC_RELAXED);
    if (e->gen == gen && e->addr == addr && e->size == size) {
        bool stale = false;
        for (uint64_t m = e->mask; m && !stale; m &= m - 1) stale = uubt[__builtin_ctzll(m)].serial > e->serial;
        if (!stale) {
            uint64_t pending = vk_pending_seq();
            if (chunks[e->chunk].seq < pending) chunks[e->chunk].seq = pending;
            stat_gcopy_hit++;
            return e->span;
        }
    }
    stat_gcopy_miss++;
    stat_gcopy_bytes += size;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    uint64_t mask = 0;
    for (int i = 0; i < nuub; i++)
        if (addr < uubt[i].hi && uubt[i].lo < addr + size) mask |= 1ull << i;
    e->addr = addr; e->size = size; e->gen = gen; e->serial = uub_serial; e->mask = mask; e->chunk = active; e->span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// SAVAGEWIND_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

// Shaders and pipelines compile where they are first needed, the draw waiting for them (a short stutter the first time
// an effect appears), unless the "Asynchronous shaders" setting is on: then they compile on other threads and draws
// that need them are skipped meanwhile. That loses what the game draws only once: the save file's screenshot (a
// blit into a 256x144 texture, read back right after) came out as whatever the GPU memory held.
// SAVAGEWIND_SYNC_SHADERS / SAVAGEWIND_ASYNC_SHADERS override the setting.
static bool sync_shaders(void) {
    static int env = -1;
    if (env < 0) env = getenv("SAVAGEWIND_SYNC_SHADERS") ? 1 : getenv("SAVAGEWIND_ASYNC_SHADERS") ? 0 : 2;
    return env == 2 ? !g_settings.async_shaders : env;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static Job *tjobs_head, *tjobs_tail;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        while (!tjobs_head) pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        Job *j = tjobs_head;
        tjobs_head = j->next;
        if (!tjobs_head) tjobs_tail = NULL;
        pthread_mutex_unlock(&tjobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) { started = true; pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&tjobs_lock);
    if (tjobs_tail) tjobs_tail->next = j; else tjobs_head = j;
    tjobs_tail = j;
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    float line_width;
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    bool depth_clamp;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (SAVAGEWIND_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
extern uint64_t g_pace_sleep_ns;  // (objects.c present_pace)
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_xwait, l_draws, l_subs, l_comp, hitch_ns;
    if (!hitch_ns) hitch_ns = getenv("SAVAGEWIND_HITCH_MS") ? (uint64_t)atoi(getenv("SAVAGEWIND_HITCH_MS")) * 1000000ull : 50000000ull;
    uint64_t now = SDL_GetTicksNS();
    uint64_t comp = prof_c_tr + prof_c_prog + prof_c_pipe + prof_c_comp;
    if (comp < l_comp) l_comp = 0;  // (the fps line took the counters)
    if (last_t && now - last_t > hitch_ns) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        extern uint64_t g_present_split[6];
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f, compiles %.1f) + present %.1f (acquire %.1f "
            "end %.1f draw %.1f submit %.1f present %.1f slot %.1f) + other %.1f (pace %.1f); %llu draws, %llu submits, compiles %d pending; "
            "exec waits %.1f\n",
            (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, (comp - l_comp) / 1e6, present_ns / 1e6,
            g_present_split[0] / 1e6, g_present_split[1] / 1e6, g_present_split[2] / 1e6, g_present_split[3] / 1e6,
            g_present_split[4] / 1e6, g_present_split[5] / 1e6,
            ((double)dt - ex - present_ns) / 1e6, g_pace_sleep_ns / 1e6, (unsigned long long)(cum_draws - l_draws),
            (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles), (cum_xwait - l_xwait) / 1e6);
    }
    { extern uint64_t g_present_split[6]; memset(g_present_split, 0, sizeof g_present_split); }
    g_pace_sleep_ns = 0;
    l_comp = comp;
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_xwait = cum_xwait; l_draws = cum_draws; l_subs = cum_submits;
}

const char *renderer_stats(void) {
    static int dumps, always = -1;
    if (always < 0) always = getenv("SAVAGEWIND_LOG_CMD_COUNTS") != NULL;  // (debugging) every interval, not just the first 3
    if (dumps++ < 3 || always) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (warm %d), fbcopy %llu (+%llu live), upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_warm, 0), (unsigned long long)stat_feedback_copies, (unsigned long long)stat_feedback_live,
             (double)stat_upload_bytes / 1048576.0);
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", sets %llu (%llu shared), fbloop %llu (%llu barriers), gmem waits %llu holds %llu",
                 (unsigned long long)stat_sets, (unsigned long long)stat_sets_reused, (unsigned long long)stat_feedback_loops,
                 (unsigned long long)stat_feedback_barriers, (unsigned long long)stat_gw_waits, (unsigned long long)stat_gw_holds);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", uub %llu (%llu same), gcopy %llu hit %llu miss (%.1f MB)",
                 (unsigned long long)stat_uub, (unsigned long long)stat_uub_same, (unsigned long long)stat_gcopy_hit,
                 (unsigned long long)stat_gcopy_miss, stat_gcopy_bytes / 1048576.0);
        stat_uub = stat_uub_same = stat_gcopy_hit = stat_gcopy_miss = stat_gcopy_bytes = 0;
        stat_gw_waits = stat_gw_holds = 0;
        stat_sets = stat_sets_reused = 0;
        stat_feedback_loops = stat_feedback_barriers = 0;
    }
    stat_feedback_copies = stat_feedback_live = stat_upload_bytes = 0;
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | compile ms: translate %.0f (%llu) program %.0f (%llu) pipeline %.0f (%llu) compute %.0f",
                 atomic_exchange(&prof_c_tr, 0) / 1e6, (unsigned long long)atomic_exchange(&prof_c_ntr, 0),
                 atomic_exchange(&prof_c_prog, 0) / 1e6, (unsigned long long)atomic_exchange(&prof_c_nprog, 0),
                 atomic_exchange(&prof_c_pipe, 0) / 1e6, (unsigned long long)atomic_exchange(&prof_c_npipe, 0),
                 atomic_exchange(&prof_c_comp, 0) / 1e6);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    st.line_width = 1.0f;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("SAVAGEWIND_VK_PROF") != NULL;
    profd = prof && atoi(getenv("SAVAGEWIND_VK_PROF")) != 2;
    no_dyn_cache = getenv("SAVAGEWIND_VK_NO_DYN_CACHE") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
    pso_init();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
// (debugging) SAVAGEWIND_TRACE_TEX (textures.c): texture write histories
bool vk_trace_tex(NvnTexture *t);
void vk_trace_event(NvnTexture *t, uint64_t key, NvnTexture *const *srcs, int nsrcs, const char *fmt, ...);
void vk_trace_dump(NvnTexture *t);

static int debug_pass_no;
static NvnTexture *pass_dbg[8];  // (debug frame) the color targets of the pass in progress
static int pass_dbg_n, pass_dbg_draws;

// ---- linear render targets (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) go back to guest memory: the CPU reads them.
// BOTW samples its sky into an 8x8 RGBA32F linear target and reads the texels back for its fog and ambient colours
// (and 1x1 / 3x3 ones for exposure); without this it read zeros. After each pass that drew one, its level is copied
// to a host buffer, and once that submission has completed (any wait on the GPU checks) the rows go to the texture's
// storage with its stride.
typedef struct { VkBuffer buf; VmaAllocation mem; const uint8_t *ptr; uint64_t seq, addr, stride, fs, serial; uint32_t row, rows, fmt; } Readback;
static uint64_t g_cur_fs_hash;  // (the fragment shader of the draw in progress: defined with the draw code below)
static Readback readbacks[64];
static int nreadbacks;
static pthread_mutex_t readback_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexture *pass_linear[8];  // the pass's linear color targets (begin_pass), with their views
static NvnTextureView pass_linear_view[8];
static bool pass_linear_has_view[8];
static int pass_nlinear;

// What guest memory held after the last copy back, per linear target, and which image (TexBackend serial) it matches:
// a different hash later means the CPU wrote there, and the image takes those texels before the GPU draws into it
// again (the CPU fills BOTW's height tiles in memory it also has the GPU draw into; copying the whole image back over
// them lost those writes). So does another image: a texture made again at the same memory gets a new one once the old
// one was let go, and it holds nothing yet.
static struct { uint64_t addr, size, hash, serial; int pending; } linear_mem[64];
static int nlinear_mem;
static int linear_mem_find(uint64_t addr, uint64_t size) {  // (readback_lock held)
    for (int i = 0; i < nlinear_mem; i++) if (linear_mem[i].addr == addr && linear_mem[i].size == size) return i;
    if (nlinear_mem == 64) return -1;
    linear_mem[nlinear_mem] = (typeof(linear_mem[0])){addr, size, 0, 0, 0};
    return nlinear_mem++;
}
extern void vk_texture_reload(NvnTexture *t);
extern uint64_t vk_hash_bytes(const void *p, uint64_t n);
static uint64_t linear_target_size(NvnTexture *t) {
    return (t->stride ? t->stride : (uint64_t)t->width * (nvn_format_info(t->format) ? nvn_format_info(t->format)->bpb : 4)) * t->height;
}

// Before a pass draws into linear target t: guest memory -> image when the CPU changed it since the last copy back, or
// when the memory came from another image. BOTW makes the height tiles its collision reads again at the same memory
// as terrain streams in, then draws only part of them: the copy back of a new, empty image put zeros and stale texels
// into the rest, and Link stood in the ground there.
static void linear_target_sync(NvnTexture *t) {
    uint64_t size = linear_target_size(t);
    if (t->addr < GUEST_LO || t->addr + size > GUEST_HI) return;
    uint64_t serial = vk_tex(t, true)->serial;
    pthread_mutex_lock(&readback_lock);
    int n = nlinear_mem, i = linear_mem_find(t->addr, size);
    bool reload = false;
    if (i >= 0 && !linear_mem[i].pending) {  // (a copy back still in flight: the memory is the GPU's until it lands)
        uint64_t h = vk_hash_bytes((const void *)(uintptr_t)t->addr, size);
        reload = i >= n || h != linear_mem[i].hash || serial != linear_mem[i].serial;  // (first use: whatever the CPU put there)
        linear_mem[i].hash = h;
        linear_mem[i].serial = serial;
    }
    pthread_mutex_unlock(&readback_lock);
    if (reload) vk_texture_reload(t);
}

// Whether a copy back from another image is still on its way to linear target t's memory: t's image takes those
// contents before it is drawn into, so they have to land first (begin_pass waits).
static bool linear_target_behind(NvnTexture *t) {
    uint64_t size = linear_target_size(t);
    if (t->addr < GUEST_LO || t->addr + size > GUEST_HI) return false;
    uint64_t serial = vk_tex(t, true)->serial;
    bool behind = false;
    pthread_mutex_lock(&readback_lock);
    for (int i = 0; i < nlinear_mem; i++)
        if (linear_mem[i].addr == t->addr && linear_mem[i].size == size)
            behind = linear_mem[i].pending && linear_mem[i].serial != serial;
    pthread_mutex_unlock(&readback_lock);
    return behind;
}

static void readbacks_apply(void) {
    pthread_mutex_lock(&readback_lock);
    if (nreadbacks) {
        uint64_t done = vk_completed_seq();
        int k = 0;
        for (int i = 0; i < nreadbacks; i++) {
            Readback *r = &readbacks[i];
            if (r->seq > done) { readbacks[k++] = *r; continue; }
            kernel_watch_write("linear render target", r->addr, r->stride * r->rows);
            static int dump = -1;  // (debugging) SAVAGEWIND_DUMP_LINEAR=1: the first copies of each target as raw files
            if (dump < 0) dump = getenv("SAVAGEWIND_DUMP_LINEAR") != NULL;
            if (dump) {
                static struct { uint64_t addr; int n; } dumped[32];
                int d = 0;
                while (d < 32 && dumped[d].addr && dumped[d].addr != r->addr) d++;
                if (d < 32 && dumped[d].n < 3) {
                    dumped[d].addr = r->addr;
                    char path[160];
                    snprintf(path, sizeof path, "build/linrt_%llx_%u_%ux%u_f%x.raw", (unsigned long long)r->addr, dumped[d].n++,
                             r->row, r->rows, r->fmt);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(r->ptr, 1, (size_t)r->row * r->rows, f); fclose(f); }
                    LOG("[vk] linear render target %llx (last fs %016llx) saved as %s\n", (unsigned long long)r->addr,
                        (unsigned long long)r->fs, path);
                }
            }
            for (uint32_t y = 0; y < r->rows; y++)
                memcpy((void *)(uintptr_t)(r->addr + y * r->stride), r->ptr + (uint64_t)y * r->row, r->row);
            guest_copy_flush();
            vmaDestroyBuffer(g_vk.vma, r->buf, r->mem);
            int m = linear_mem_find(r->addr, r->stride * r->rows);
            if (m >= 0) {
                if (linear_mem[m].pending > 0) linear_mem[m].pending--;
                linear_mem[m].hash = vk_hash_bytes((const void *)(uintptr_t)r->addr, r->stride * r->rows);
                linear_mem[m].serial = r->serial;
            }
        }
        nreadbacks = k;
    }
    pthread_mutex_unlock(&readback_lock);
}

static void readback_schedule(VkCommandBuffer cb, NvnTexture *t, const NvnTextureView *v, bool has_view) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_LINEAR_READBACK") != NULL;  // (A/B tests)
    TexBackend *b = t->backend;
    const FormatInfo *fi = nvn_format_info(t->format);
    if (off || !b || !b->image || !fi || !fi->bpb || fi->bpb == 3 || fi->bw != 1 || b->samples > 1 || b->scalable || b->depth > 1 ||
        (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->format != vk_format(t->format))
        return;
    uint32_t lv = has_view && (v->setmask & 1) ? v->base_level : 0, ly = has_view && (v->setmask & 2) ? v->min_layer : 0;
    if (lv || ly) return;  // (linear targets have one level and one layer)
    uint32_t w = b->width, h = b->height, row = w * fi->bpb;
    uint64_t stride = t->stride ? t->stride : row;
    if (stride < row || t->addr < GUEST_LO || t->addr + stride * h > GUEST_HI) return;
    {
        static uint64_t seen[16];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) known |= seen[i] == t->addr;
        if (!known && nseen < 16) {
            seen[nseen++] = t->addr;
            LOG("[vk] linear render target %ux%u format %#x stride %llu at %llx: copied back to guest memory\n", w, h, t->format,
                (unsigned long long)stride, (unsigned long long)t->addr);
        }
    }
    pthread_mutex_lock(&readback_lock);
    if (nreadbacks == 64) { pthread_mutex_unlock(&readback_lock); return; }
    Readback *r = &readbacks[nreadbacks];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (uint64_t)row * h, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &r->buf, &r->mem, &info) != VK_SUCCESS) { pthread_mutex_unlock(&readback_lock); return; }
    r->ptr = info.pMappedData;
    r->seq = vk_pending_seq();
    r->addr = t->addr;
    r->stride = stride;
    r->row = row;
    r->rows = h;
    r->fmt = t->format;
    r->fs = g_cur_fs_hash;
    r->serial = b->serial;
    nreadbacks++;
    int m = linear_mem_find(r->addr, stride * h);
    if (m >= 0) linear_mem[m].pending++;
    pthread_mutex_unlock(&readback_lock);
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferRowLength = w, .bufferImageHeight = h, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, r->buf, 1, &c);
}

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    for (int i = 0; i < pass_nlinear; i++) readback_schedule(vk_cmd(), pass_linear[i], &pass_linear_view[i], pass_linear_has_view[i]);
    pass_nlinear = 0;
    // (debug frame) what the pass drew: its targets, as begin_pass recorded them (st.rt may name the next pass's)
    if (g_debug_frame && pass_dbg_n && pass_dbg[0] && pass_dbg[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("SAVAGEWIND_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        LOG("[pass] %s: %d targets, %d draws\n", path, pass_dbg_n, pass_dbg_draws);
        // a frame saves hundreds of images (minutes, the window stalls meanwhile): SAVAGEWIND_DEBUG_PASS_FROM=<n> /
        // SAVAGEWIND_DEBUG_PASS_TO=<n> save only those passes, SAVAGEWIND_DEBUG_MAIN_ONLY only each pass's first target
        static int from = -1, to = -1, main_only = -1;
        if (from < 0) from = getenv("SAVAGEWIND_DEBUG_PASS_FROM") ? atoi(getenv("SAVAGEWIND_DEBUG_PASS_FROM")) : 0;
        if (to < 0) to = getenv("SAVAGEWIND_DEBUG_PASS_TO") ? atoi(getenv("SAVAGEWIND_DEBUG_PASS_TO")) : 1 << 30;
        if (main_only < 0) main_only = getenv("SAVAGEWIND_DEBUG_MAIN_ONLY") != NULL;
        if (debug_pass_no - 1 < from || debug_pass_no - 1 > to) return;
        renderer_save_texture_tex(pass_dbg[0]->backend, path);
        for (int i = 1; i < pass_dbg_n && !main_only; i++) {  // the other targets of a multi-target pass: pass_NNN_c<i>.png
            if (!pass_dbg[i] || !pass_dbg[i]->backend) continue;
            char p2[300];
            snprintf(p2, sizeof p2, "%.*s_c%d.png", (int)strlen(path) - 4, path, i);
            renderer_save_texture_tex(pass_dbg[i]->backend, p2);
        }
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    pass_ds_written = false;
    pass_color_written = pass_color_unsynced = pass_feedback = 0;
    if (dl_on == 2) renderer_flush();  // (SAVAGEWIND_VK_DEVLOST_TRACE=2: each pass its own submission)
    // BOTW records a whole frame into one command list. Submitted only at its end, the GPU sat idle while the list was
    // translated (~13 ms in the open world) and only then rendered it, and the game waits for the GPU to finish a frame
    // (nvnSyncWait) before it goes on: the two times added up, a fifth of the frames ran over 31.7 ms, and the game
    // went to 20 fps. A submission at a pass boundary every ~300 draws lets the GPU work on the frame meanwhile.
    // SAVAGEWIND_VK_SUBMIT_DRAWS=<n> sets the count (0: one submission per command list, as before).
    static int submit_draws = -1;
    if (submit_draws < 0) submit_draws = getenv("SAVAGEWIND_VK_SUBMIT_DRAWS") ? atoi(getenv("SAVAGEWIND_VK_SUBMIT_DRAWS")) : 300;
    // Not once the list has recorded a shader that stores to guest memory (global memory, the imported heap): run
    // on the GPU while the rest of the list is translated, it wrote memory this thread was copying for later draws and
    // dispatches (uniforms, vertices), they got half-written data, and the GPU faulted on a write (~1 world load in
    // 7). The rest of such a list goes in one piece at its end and sees the memory as it was, as before. (Waiting for
    // the piece instead cost more: the GPU is ~13 ms behind the translation at that point of BOTW's frame.)
    static int no_hold = -1;  // (debugging) SAVAGEWIND_VK_NO_HOLD=1: pieces go on after such a shader too
    if (no_hold < 0) no_hold = getenv("SAVAGEWIND_VK_NO_HOLD") != NULL;
    if (submit_draws > 0 && chunk_draws >= (uint32_t)submit_draws && (!chunk_hold || no_hold))
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    st.rt_dirty = false;
    for (int i = 0; i < st.rt.ncolor && i < 8; i++)  // (before the pass's command buffer is taken: a flush ends it)
        if (st.rt.color[i] && (st.rt.color[i]->flags & 0x40) && linear_target_behind(st.rt.color[i])) {
            renderer_flush();  // (its wait applies the copies back)
            break;
        }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    {   // one scale for the pass: attachments the size of a scaled one are scaled too (BOTW's G-buffer mixes
        // targets the scaling heuristic takes and ones it does not; the unscaled ones got only part of the pass)
        NvnTexture *all[9];
        int n = 0;
        for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) all[n++] = st.rt.color[i];
        if (st.rt.depth) all[n++] = st.rt.depth;
        for (int i = 0; i < n; i++) {
            if (!renderer_texture_scalable(all[i])) continue;
            for (int k = 0; k < n; k++)
                if (all[k]->width == all[i]->width && all[k]->height == all[i]->height) vk_make_scalable_like(all[k], all[i]);
            break;
        }
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
#ifdef VK_KHR_unified_image_layouts  // (Vulkan headers 1.4.321+; older ones never enable feedback loops: device.c)
    VkAttachmentFeedbackLoopInfoEXT ca_fb[8];
#endif
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_dbg_n = st.rt.ncolor > 8 ? 8 : st.rt.ncolor;
    for (int i = 0; i < pass_dbg_n; i++) pass_dbg[i] = st.rt.color[i];
    {   // (debugging) SAVAGEWIND_LOG_RENDER_TEX=<w>x<h>: passes that draw into textures of that size
        static int lw = -1, lh;
        if (lw < 0) { lw = 0; if (getenv("SAVAGEWIND_LOG_RENDER_TEX")) sscanf(getenv("SAVAGEWIND_LOG_RENDER_TEX"), "%dx%d", &lw, &lh); }
        static int n;
        for (int i = 0; lw && i < pass_dbg_n && n < 5000; i++)
            if (st.rt.color[i] && (int)st.rt.color[i]->width == lw && (int)st.rt.color[i]->height == lh)
                LOG("[rt] frame %llu: #%llu %ux%ux%u f%#x target %u drawn at level %u layer %u (view set %#x)\n",
                    (unsigned long long)renderer_present_count(), (unsigned long long)st.rt.color[i]->debug_id, lw, lh,
                    st.rt.color[i]->depth, st.rt.color[i]->format, st.rt.color[i]->target, st.rt.cview[i].base_level,
                    st.rt.cview[i].min_layer, st.rt.has_cview[i] ? st.rt.cview[i].setmask : 0), n++;
    }
    pass_nlinear = 0;
    for (int i = 0; i < pass_dbg_n; i++)
        if (st.rt.color[i] && (st.rt.color[i]->flags & 0x40)) {  // NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET
            pass_linear[pass_nlinear] = st.rt.color[i];
            pass_linear_view[pass_nlinear] = st.rt.cview[i];
            pass_linear_has_view[pass_nlinear++] = st.rt.has_cview[i];
            linear_target_sync(st.rt.color[i]);
        }
    pass_dbg_draws = 0;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (b->feedback_seen && b->feedback_ok) {  // a target some draw samples while writing it (resolve_feedback)
#ifdef VK_KHR_unified_image_layouts
            ca_fb[i] = (VkAttachmentFeedbackLoopInfoEXT){.sType = VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT,
                                                         .feedbackLoopEnable = VK_TRUE};
            ca[i].pNext = &ca_fb[i];
#endif
            pass_feedback |= (uint8_t)(1u << i);
        }
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    dyn_check();
    if (!memcmp(&dc.sc, &r, sizeof r)) return;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
    dc.sc = r;
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    dyn_check();
    if (!memcmp(&dc.vp, &vp, sizeof vp)) return;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
    dc.vp = vp;
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_LINE_WIDTH};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetLineWidth(cb, 1.0f);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            pass_color_unsynced |= (uint8_t)(1u << index);
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    if (index >= 0) pass_color_unsynced |= (uint8_t)(1u << index);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vk_dyn_forget();
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(const VkBindingInfo *binds, int nbinds, VkDescriptorSetLayout *set_layout, int nsets,
                                    VkShaderStageFlags s0, VkShaderStageFlags s1) {
    set_layout[0] = vk_set_layout(binds, nbinds, 0, s0);
    if (nsets > 1) set_layout[1] = vk_set_layout(binds, nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("SAVAGEWIND_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

// One stage's SPIR-V module and bindings, per translated shader: programs share their vertex and fragment shaders
// (the same ShaderInfo, vk_translate_cached), so a stage seen in another program costs no SPIR-V load, reflection or
// module creation again.
typedef struct { const ShaderInfo *si; VkShaderModule m; int nbinds; VkBindingInfo binds[MAX_BIND]; } StageBuild;
#define STAGE_TAB 8192
static StageBuild *stage_tab[STAGE_TAB];
static pthread_mutex_t stage_lock = PTHREAD_MUTEX_INITIALIZER;

static const StageBuild *stage_build(const ShaderInfo *si, int stage, const char *name) {
    uint32_t slot = (uint32_t)(((uintptr_t)si >> 4) * 0x9E3779B1u) & (STAGE_TAB - 1);
    pthread_mutex_lock(&stage_lock);
    for (int probe = 0; probe < STAGE_TAB && stage_tab[slot]; probe++, slot = (slot + 1) & (STAGE_TAB - 1))
        if (stage_tab[slot]->si == si) { StageBuild *b = stage_tab[slot]; pthread_mutex_unlock(&stage_lock); return b; }
    pthread_mutex_unlock(&stage_lock);
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(si->msl, stage, name, &n);
    if (!spv) return NULL;
    StageBuild *b = calloc(1, sizeof *b);
    b->si = si;
    b->nbinds = vk_reflect(spv, n, b->binds, MAX_BIND);
    b->m = vk_module(spv, n);
    free(spv);
    if (!b->m) { free(b); return NULL; }
    pthread_mutex_lock(&stage_lock);
    for (int probe = 0; probe < STAGE_TAB; probe++, slot = (slot + 1) & (STAGE_TAB - 1)) {
        if (stage_tab[slot] && stage_tab[slot]->si == si) {  // (another thread built it meanwhile)
            StageBuild *o = stage_tab[slot];
            pthread_mutex_unlock(&stage_lock);
            vkDestroyShaderModule(g_vk.dev, b->m, NULL);
            free(b);
            return o;
        }
        if (!stage_tab[slot]) { stage_tab[slot] = b; break; }
    }
    pthread_mutex_unlock(&stage_lock);
    return b;  // (a full table keeps it out: built again next time, nothing goes wrong)
}

// A program's modules, bindings and layout, per pair of translated shaders: Progs with the same stages (the same
// shaders at other addresses, after an in-place reboot) and the pipeline pre-warm share them, and the pipelines
// made for one fit the others (same layout object).
typedef struct ProgBuild {
    const ShaderInfo *vi, *fi;
    VkShaderModule vs, fs;
    int nbinds;
    VkBindingInfo binds[MAX_BIND];
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    struct ProgBuild *next;
} ProgBuild;
#define PB_TAB 4096
static ProgBuild *pb_tab[PB_TAB];
static pthread_mutex_t pb_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t pair_slot(const void *a, const void *b) {
    uint64_t h = ((uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)b * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)(h ^ (h >> 29));
}

// NULL when a stage does not compile. *made: built by this call (else found).
static const ProgBuild *prog_build(const ShaderInfo *vi, const ShaderInfo *fi, bool *made) {
    *made = false;
    uint32_t slot = pair_slot(vi, fi) & (PB_TAB - 1);
    pthread_mutex_lock(&pb_lock);
    ProgBuild *b = pb_tab[slot];
    while (b && !(b->vi == vi && b->fi == fi)) b = b->next;
    pthread_mutex_unlock(&pb_lock);
    if (b) return b;
    const StageBuild *v = stage_build(vi, STAGE_VERTEX, "vs");
    const StageBuild *f = v ? stage_build(fi, STAGE_FRAGMENT, "fs") : NULL;
    if (!v || !f) return NULL;
    b = calloc(1, sizeof *b);
    b->vi = vi;
    b->fi = fi;
    b->nbinds = v->nbinds;
    memcpy(b->binds, v->binds, sizeof v->binds[0] * (size_t)v->nbinds);
    int nf = f->nbinds < MAX_BIND - b->nbinds ? f->nbinds : MAX_BIND - b->nbinds;
    memcpy(b->binds + b->nbinds, f->binds, sizeof f->binds[0] * (size_t)nf);
    b->nbinds += nf;
    b->vs = v->m;
    b->fs = f->m;
    b->layout = make_layout(b->binds, b->nbinds, b->set_layout, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    pthread_mutex_lock(&pb_lock);
    ProgBuild *o = pb_tab[slot];
    while (o && !(o->vi == vi && o->fi == fi)) o = o->next;
    if (!o) { b->next = pb_tab[slot]; pb_tab[slot] = b; }
    pthread_mutex_unlock(&pb_lock);
    if (o) {  // (another thread built it meanwhile)
        vkDestroyPipelineLayout(g_vk.dev, b->layout, NULL);
        for (int i = 0; i < 2; i++) vkDestroyDescriptorSetLayout(g_vk.dev, b->set_layout[i], NULL);
        free(b);
        return o;
    }
    *made = true;
    return b;
}

static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t pt0 = SDL_GetTicksNS();
    bool made;
    const ProgBuild *b = prog_build(p->vi, p->fi, &made);
    if (!b) { atomic_store(&p->state, 2); return; }
    p->nbinds = b->nbinds;
    memcpy(p->binds, b->binds, sizeof b->binds[0] * (size_t)b->nbinds);
    p->vs = b->vs;
    p->fs = b->fs;
    p->set_layout[0] = b->set_layout[0];
    p->set_layout[1] = b->set_layout[1];
    p->layout = b->layout;
    if (made) {
        atomic_fetch_add(&stat_compiles, 1);
        atomic_fetch_add(&prof_c_prog, SDL_GetTicksNS() - pt0); atomic_fetch_add(&prof_c_nprog, 1);
    } else {
        atomic_fetch_add(&stat_warm, 1);
    }
    atomic_store(&p->state, 1);
}

// SAVAGEWIND_SHADER_DUMP=1: the Maxwell code of shaders that fail to translate -> build/shaders/<stage>_<addr>.bin
// (from the shader's first word, at most 64 KB: past its end is other data)
static void dump_failed_shader(uint64_t code, uint64_t max, const char *stage) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_SHADER_DUMP") != NULL;
    if (!on || !code) return;
    mkdir("build/shaders", 0755);
    char path[96];
    snprintf(path, sizeof path, "build/shaders/%s_%llx.bin", stage, (unsigned long long)code);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite((const void *)(uintptr_t)code, 1, max && max < 0x10000 ? max : 0x10000, f);
    fclose(f);
}

// (debugging) Fragment shaders named in an env list (comma-separated): an fs hash (the [draw] log's), or
// w<pc hex>=<word hex>, the shaders whose instruction at that pc (as the saved GLSL numbers them) is that word; the
// latter does not need a run to learn the hash first.
typedef struct { uint64_t v; uint32_t pc; bool word; } FsSel;

static int fs_sel_parse(const char *p, FsSel *out, int max) {
    int n = 0;
    while (p && *p && n < max) {
        const char *q = p;
        FsSel f = {0};
        if (*p == 'w') { f.word = true; f.pc = (uint32_t)strtoul(p + 1, (char **)&p, 16); if (*p == '=') p++; }
        f.v = strtoull(p, (char **)&p, 16);
        out[n++] = f;
        while (*p == ',') p++;
        if (p == q) break;
    }
    return n;
}

static bool fs_sel_match(const FsSel *f, uint64_t code, uint64_t hash) {
    if (!f->word) return f->v == hash;
    return code && f->pc % 32 && f->pc < 0x10000 && ((const uint64_t *)(uintptr_t)(code + 0x80))[f->pc / 8] == f->v;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    uint64_t pt0 = SDL_GetTicksNS();
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_mutex_lock(&translate_lock);
    {   // SAVAGEWIND_DBG_FS=<fs code addr hex>:<pc hex>:<reg>: that register after that instruction, as every output
        // (pc ffffffff: 1.0, where the shader runs at all), as in renderer.m. The shader can also be named
        // w<pc hex>=<word hex> (see FsSel) in place of its address, which changes from run to run.
        static unsigned long long dbg_addr;
        static unsigned dbg_pc;
        static int dbg_reg, parsed;
        static FsSel sel;
        if (!parsed) {
            parsed = 1;
            const char *e = getenv("SAVAGEWIND_DBG_FS");
            if (e && *e == 'w') { fs_sel_parse(e, &sel, 1); e = strchr(e, ':'); if (e) sscanf(e, ":%x:%d", &dbg_pc, &dbg_reg); }
            else if (e) sscanf(e, "%llx:%x:%d", &dbg_addr, &dbg_pc, &dbg_reg);
        }
        if (sel.word) dbg_addr = fs_sel_match(&sel, p->code[1], 0) ? p->code[1] : 0;
        maxwell_dbg_code = dbg_addr ? (const uint8_t *)(uintptr_t)dbg_addr : NULL;
        maxwell_dbg_pc = dbg_pc;
        maxwell_dbg_reg = dbg_reg;
    }
    p->vi = vk_translate_cached((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = vk_translate_cached((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    atomic_fetch_add(&prof_c_tr, SDL_GetTicksNS() - pt0); atomic_fetch_add(&prof_c_ntr, 1);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        dump_failed_shader(p->code[0], p->cmax[0], "vs");
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s (code %llx)\n", p->fi->error, (unsigned long long)p->code[1]);
        dump_failed_shader(p->code[1], p->cmax[1], "fs");
        atomic_store(&p->state, 2);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    run_job(compile_program, p);
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// The last program and pipeline draw_ looked up (consecutive draws mostly repeat them; cleared by vk_exec_reset).
static struct { uint64_t code[2]; ShaderKey vk; uint8_t rt[sizeof ((ShaderKey *)0)->rt_kind]; Prog *p; } prog_memo;

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey);
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    if (prog_memo.p && prog_memo.code[0] == np->stages[0].code_addr && prog_memo.code[1] == np->stages[1].code_addr &&
        !memcmp(&prog_memo.vk, vkey, sizeof *vkey) && !memcmp(prog_memo.rt, fkey->rt_kind, sizeof prog_memo.rt))
        return prog_memo.p;
    Prog *p = get_program_(np, vkey, fkey);
    prog_memo.code[0] = np->stages[0].code_addr;
    prog_memo.code[1] = np->stages[1].code_addr;
    prog_memo.vk = *vkey;
    memcpy(prog_memo.rt, fkey->rt_kind, sizeof prog_memo.rt);
    prog_memo.p = p;
    return p;
}

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_translate(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30: case 0x6A:
        return 1;
    // integer-to-float formats (R8_UI2F 0x51 .. RGBA32_I2F 0x68, RGB10A2_UI2F / _I2F): fetched as integers, the
    // shader converts (USCALED / SSCALED vertex formats are optional in Vulkan)
    case 0x51: case 0x53: case 0x55: case 0x57: case 0x59: case 0x5B: case 0x5D: case 0x5F: case 0x61: case 0x63:
    case 0x65: case 0x67: case 0x6B:
        return 4;
    case 0x52: case 0x54: case 0x56: case 0x58: case 0x5A: case 0x5C: case 0x5E: case 0x60: case 0x62: case 0x64:
    case 0x66: case 0x68: case 0x6C:
        return 3;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    // integer-to-float: the integer format (attr_kind 3 / 4 makes the shader convert)
    case 0x51: return VK_FORMAT_R8_UINT;
    case 0x52: return VK_FORMAT_R8_SINT;
    case 0x53: return VK_FORMAT_R16_UINT;
    case 0x54: return VK_FORMAT_R16_SINT;
    case 0x55: return VK_FORMAT_R32_UINT;
    case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_UINT;
    case 0x58: return VK_FORMAT_R8G8_SINT;
    case 0x59: return VK_FORMAT_R16G16_UINT;
    case 0x5A: return VK_FORMAT_R16G16_SINT;
    case 0x5B: return VK_FORMAT_R32G32_UINT;
    case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_UINT;
    case 0x5E: return VK_FORMAT_R8G8B8_SINT;
    case 0x5F: return VK_FORMAT_R16G16B16_UINT;
    case 0x60: return VK_FORMAT_R16G16B16_SINT;
    case 0x61: return VK_FORMAT_R32G32B32_UINT;
    case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x64: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x65: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x66: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x6B: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x6C: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    default: {
        static uint8_t told[256];
        if (f != 0x2E && !told[f & 0xFF]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread: the program, and the
// fixed state (PipeState, also what the pipeline list stores; zeroed before it is filled: compared and hashed whole).
#define ZERO_BINDING 16
typedef struct {
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
    uint8_t feedback;  // samples a color target it writes (VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
    uint8_t depth_clamp;  // nvnCommandBufferSetDepthClamp (in what was padding: older pipeline lists read it as off)
} PipeState;
typedef struct { Prog *prog; PipeState s; } PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];
static struct { PipeKey k; Pipe *pp; } pipe_memo;  // (see prog_memo)

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// The driver's verdict on the pipeline cache for a pipeline just made (creation feedback): to the cache saver
// (device.c), and counted for the pre-warm's log.
static _Atomic int pso_cache_hits, pso_cache_misses;
static void cache_feedback(const VkPipelineCreationFeedback *fb) {
    bool hit = (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
               (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
    atomic_fetch_add(hit ? &pso_cache_hits : &pso_cache_misses, 1);
    extern void vk_pipeline_cache_note(bool hit);
    vk_pipeline_cache_note(hit);
}

static VkResult create_pipeline(const PipeState *k, VkShaderModule vsm, VkShaderModule fsm, VkPipelineLayout layout, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .depthClampEnable = k->depth_clamp && g_vk.depth_clamp,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                .pPipelineCreationFeedback = &fb};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &fci,
                                        .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->color,
                                        .depthAttachmentFormat = k->depth, .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout,
                                       .flags = k->feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0};
    *out = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, out);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    } else {
        cache_feedback(&fb);
    }
    return r;
}

// Graphics pipelines by stages and fixed state, whoever made them first: a draw (build_pipeline, for its Prog's
// Pipe) or the pre-warm. One being built is waited for; the pre-warm skips those a draw made first.
typedef struct WarmPipe {
    const ShaderInfo *vi, *fi;
    PipeState s;
    uint64_t hash;
    VkPipeline pipe;
    _Atomic int state;  // 1 being built, 2 ready, 3 failed
    struct WarmPipe *next;
} WarmPipe;
#define WARM_TAB 16384
static WarmPipe *warm_tab[WARM_TAB];
static pthread_mutex_t warm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t warm_cond = PTHREAD_COND_INITIALIZER;

// true: made here, state 1 (the caller builds it, then warm_done); false: there already, and built.
static bool warm_get(const ShaderInfo *vi, const ShaderInfo *fi, const PipeState *s, WarmPipe **out) {
    uint64_t h = hash_words((uint64_t)pair_slot(vi, fi) * 0x9E3779B97F4A7C15ull, s, sizeof *s);
    pthread_mutex_lock(&warm_lock);
    WarmPipe *w = warm_tab[h & (WARM_TAB - 1)];
    while (w && !(w->hash == h && w->vi == vi && w->fi == fi && !memcmp(&w->s, s, sizeof *s))) w = w->next;
    if (!w) {
        w = calloc(1, sizeof *w);
        w->vi = vi;
        w->fi = fi;
        w->s = *s;
        w->hash = h;
        w->state = 1;
        w->next = warm_tab[h & (WARM_TAB - 1)];
        warm_tab[h & (WARM_TAB - 1)] = w;
        pthread_mutex_unlock(&warm_lock);
        *out = w;
        return true;
    }
    while (atomic_load(&w->state) == 1) pthread_cond_wait(&warm_cond, &warm_lock);
    pthread_mutex_unlock(&warm_lock);
    *out = w;
    return false;
}

static void warm_done(WarmPipe *w, VkPipeline p, VkResult r) {
    pthread_mutex_lock(&warm_lock);
    w->pipe = p;
    atomic_store(&w->state, r == VK_SUCCESS ? 2 : 3);
    pthread_cond_broadcast(&warm_cond);
    pthread_mutex_unlock(&warm_lock);
}

static void pso_record(int kind, uint64_t id0, uint64_t id1, const void *state, size_t n);

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    uint64_t pt0 = SDL_GetTicksNS();
    PipeKey *k = &pp->k;
    WarmPipe *w;
    if (warm_get(k->prog->vi, k->prog->fi, &k->s, &w)) {
        VkPipeline p;
        VkResult r = create_pipeline(&k->s, k->prog->vs, k->prog->fs, k->prog->layout, &p);
        warm_done(w, p, r);
        if (r == VK_SUCCESS) pso_record(1, vk_translate_id(k->prog->vi), vk_translate_id(k->prog->fi), &k->s, sizeof k->s);
        atomic_fetch_add(&stat_compiles, 1);
        atomic_fetch_add(&prof_c_pipe, SDL_GetTicksNS() - pt0); atomic_fetch_add(&prof_c_npipe, 1);
    } else {
        atomic_fetch_add(&stat_warm, 1);
    }
    pp->pipe = w->pipe;
    atomic_store(&pp->state, atomic_load(&w->state) == 2 ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static bool feedback_any_type[4];  // the view fits every lookup type (a restricted view of the image itself)
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static const NvnTexEntry *shader_texture_entry(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return &((NvnTexEntry *)st.tpool->entries)[tic];
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debugging) SAVAGEWIND_FEEDBACK_DUMP=<frame>: the color targets that draws of that frame sampled while writing them,
// raw (level 0, layer 0), once the targets change after them: build/fbdump_<n>_<w>x<h>_f<vkformat>.bin. Compares
// the copies with the feedback loop pixel for pixel.
static NvnTexture *fbrun_tex[4];
static int nfbrun;
static bool fbdump_on(void) {
    static long at = -2;
    if (at == -2) at = getenv("SAVAGEWIND_FEEDBACK_DUMP") ? atol(getenv("SAVAGEWIND_FEEDBACK_DUMP")) : -1;
    return at >= 0 && renderer_present_count() == (uint64_t)at;
}
static void fbdump_note(NvnTexture *t) {
    if (!fbdump_on()) return;
    for (int i = 0; i < nfbrun; i++) if (fbrun_tex[i] == t) return;
    if (nfbrun < 4) fbrun_tex[nfbrun++] = t;
}
static void fbdump_flush(void) {
    static int n;
    for (int i = 0; i < nfbrun; i++) {
        TexBackend *b = fbrun_tex[i]->backend;
        if (!b || !b->image || b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) continue;
        uint32_t bpp = b->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        VkSpan out = vk_readback((uint64_t)b->width * b->height * bpp);
        vk_end_pass();
        st.rt_dirty = true;
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_flush();
        char path[160];
        snprintf(path, sizeof path, "build/fbdump_%d_%ux%u_f%d.bin", n++, b->width, b->height, b->format);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(out.ptr, 1, (size_t)b->width * b->height * bpp, f); fclose(f); }
        LOG("[feedback] dumped %s\n", path);
    }
    nfbrun = 0;
}

// The draw writes color target i (the fragment shader outputs it and its channel mask lets some channel through).
static bool draw_writes_color(Prog *pr, int i) {
    return ((pr->fi->out_mask >> (i * 4)) & 0xF) && ((st.channel_mask >> (i * 4)) & 0xF);
}

// The draw writes neither depth nor stencil: its depth attachment is only read.
static bool depth_read_only(void) {
    if (st.ds[0] && st.ds[1]) return false;
    if (!st.ds[3]) return true;
    uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
    for (int f = 0; f < 2; f++) {
        uint32_t o = f ? ops >> 12 : ops;
        if (!(st.stencil_mask[f] & 0xFF)) continue;
        for (int k = 0; k < 3; k++) {
            uint32_t v = (o >> (4 * k)) & 0xF;
            if (v >= 2 && v <= 8) return false;  // (stencil_op: 2..8 write, KEEP otherwise)
        }
    }
    return true;
}

static long logfb = -1, fbskip;  // (SAVAGEWIND_LOG_FEEDBACK)
static unsigned fb_draws;
static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    draw_feedback = false;
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("SAVAGEWIND_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            // A depth attachment the draw only reads is sampled as it is: Vulkan allows that read-only feedback in the
            // GENERAL layout. (BOTW's light composites, fog and particles sample the depth they test against; a copy
            // each time also ended the pass: ~500 copies a frame in the Shrine of Resurrection.) Depth written earlier
            // in this pass needs a barrier first: the pass ends, without a copy.
            static int nolive = -1;
            if (nolive < 0) nolive = getenv("SAVAGEWIND_NO_LIVE_DEPTH") != NULL;
            if (!nolive && t == st.rt.depth && depth_read_only()) {
                if (pass_ds_written) { vk_end_pass(); st.rt_dirty = true; }
                stat_feedback_live++;
                feedback_tex[nfeedback] = t;
                feedback_any_type[nfeedback] = false;
                feedback_view[nfeedback++] = vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            // A color target sampled at other mip levels: BOTW builds mip chains by rendering level n from level n-1
            // of the same (array) texture, sampling it without a view. The lookup gets a view of the levels below
            // the one being written: different subresources, no feedback loop, no copy. (The copy cost a pass and a
            // 1024x1024 copy per level, and for array lookups it gave a black texture.) The level below was written
            // by an earlier pass, which ended with a barrier.
            if (!nolive && t != st.rt.depth) {
                const NvnTexEntry *e = shader_texture_entry(sg, &infos[sg]->tex[i]);
                int ci = -1;
                for (int k = 0; k < st.rt.ncolor; k++) if (st.rt.color[k] == t) ci = k;
                const NvnTextureView *cv = ci >= 0 && st.rt.has_cview[ci] ? &st.rt.cview[ci] : NULL;
                uint32_t nl = t->levels ? t->levels : 1, tl = cv && (cv->setmask & 1) ? cv->base_level : 0;
                bool sv = e && e->has_view && (e->view.setmask & 1);
                uint32_t sb = sv ? e->view.base_level : 0, sn = sv && e->view.num_levels ? e->view.num_levels : nl - sb;
                // A color target this draw doesn't write (BOTW's light composites keep the G-buffer bound with its
                // channel masks off and sample it): read as it is, like a read-only depth attachment. Written earlier
                // in this pass: the pass ends first (a barrier), still no copy.
                if (ci >= 0 && !draw_writes_color(pr, ci)) {
                    if (pass_color_written & (1u << ci)) { vk_end_pass(); st.rt_dirty = true; }
                    stat_feedback_live++;
                    continue;
                }
                if (tl > sb) {  // (tl inside the sampled range: keep the levels under it; past it: all of them)
                    NvnTextureView v;
                    memset(&v, 0, sizeof v);
                    if (e && e->has_view) v = e->view;
                    v.setmask |= 1;
                    v.base_level = (uint8_t)sb;
                    v.num_levels = (uint8_t)(tl < sb + sn ? tl - sb : sn);
                    VkImageView view = vk_view(t, &v, true, false, infos[sg]->tex[i].type);
                    if (view) {
                        stat_feedback_live++;
                        feedback_tex[nfeedback] = t;
                        feedback_any_type[nfeedback] = true;
                        feedback_view[nfeedback++] = view;
                        continue;
                    }
                }
                // The same for layers: the terrain fills layer n of its height and water arrays from layer n-1 (the
                // coarser level of detail) wherever no tile of its own is loaded, sampling the array it renders into.
                // The lookup gets the layers below the one being written. (The snapshot gave array lookups a black
                // dummy: those parts of the height array stayed 0, pits with stretched walls in the ground.)
                uint32_t tly = cv && (cv->setmask & 2) ? cv->min_layer : 0;
                bool svl = e && e->has_view && (e->view.setmask & 2);
                uint32_t sbl = svl ? e->view.min_layer : 0;
                if (tly > sbl && infos[sg]->tex[i].type == TEXTYPE_2D_ARRAY) {
                    NvnTextureView v;
                    memset(&v, 0, sizeof v);
                    if (e && e->has_view) v = e->view;
                    v.setmask |= 2;
                    v.min_layer = (uint16_t)sbl;
                    v.num_layers = (uint16_t)(tly - sbl);
                    VkImageView view = vk_view(t, &v, true, false, infos[sg]->tex[i].type);
                    if (view) {
                        stat_feedback_live++;
                        feedback_tex[nfeedback] = t;
                        feedback_any_type[nfeedback] = true;
                        feedback_view[nfeedback++] = view;
                        continue;
                    }
                }
            }
            // A color target the draw samples where it writes it: BOTW's terrain ORs bits into the G channel of G-buffer
            // target 0 (fs 50fc37edcd79cf02 reads it at its own pixel), ~200 draws a frame in a row without a barrier.
            // A copy of the 1600x900 target before each draw cost a third of the frame's GPU time (copy, full barrier,
            // the pass ended and begun again). Instead the pass runs with the target as a feedback loop, and a barrier
            // inside it makes the earlier draws' writes visible to this one's reads at the same pixel; each draw sees
            // what the ones before it wrote, as with the copies. Fragment-shader reads only (a barrier inside the pass
            // reaches no vertex shader). The first such draw ends the pass: it begins again with the loop enabled, and
            // from then on every pass with that target does.
            static int nofbl = -1;
            if (nofbl < 0) nofbl = getenv("SAVAGEWIND_VK_FEEDBACK_COPY") != NULL;  // (debugging) copies, as before
            if (!nofbl && sg == 1 && t != st.rt.depth && src->feedback_ok) {
                int ci = -1;
                for (int k = 0; k < st.rt.ncolor; k++) if (st.rt.color[k] == t) ci = k;
                if (ci >= 0) {
                    src->feedback_seen = true;
                    fbdump_note(t);
                    if (in_pass && !st.rt_dirty && (pass_feedback & (1u << ci))) {
                        if (pass_color_unsynced & (1u << ci)) {
                            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                   .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                   .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                   .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                   .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
                            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                   .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT, .memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &mb};
                            vkCmdPipelineBarrier2(vk_cmd(), &di);
                            pass_color_unsynced = 0;  // (one barrier covers every target)
                            stat_feedback_barriers++;
                        }
                    } else {
                        vk_end_pass();
                        st.rt_dirty = true;
                    }
                    draw_feedback = true;
                    stat_feedback_loops++;
                    continue;
                }
            }
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (t != st.rt.depth) fbdump_note(t);
            // SAVAGEWIND_LOG_FEEDBACK=<n>: 200 snapshots after the first n (what is sampled while bound)
            if (logfb < 0) logfb = getenv("SAVAGEWIND_LOG_FEEDBACK") ? (fbskip = atol(getenv("SAVAGEWIND_LOG_FEEDBACK")), 200) : 0;
            if (logfb > 0 && fbskip > 0) fbskip--;
            else if (logfb > 0) {
                logfb--;
                const NvnTexEntry *e = shader_texture_entry(sg, &infos[sg]->tex[i]);
                int ci = -1;
                for (int k = 0; k < st.rt.ncolor; k++) if (st.rt.color[k] == t) ci = k;
                const NvnTextureView *cv = ci >= 0 && st.rt.has_cview[ci] ? &st.rt.cview[ci] : NULL;
                LOG("[feedback] fs %016llx @%llx %s %ux%u fmt %#x levels %u | target view mask %#x level %u layer %u+%u | sampled view %d mask %#x "
                    "level %u+%u layer %u+%u | stage %d t%d target %d of %d out %#x mask %#x blend %d draws since %u\n", (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[1].code_addr, t == st.rt.depth ? "depth" : "color", src->width, src->height, t->format,
                    t->levels, cv ? cv->setmask : 0, cv ? cv->base_level : 0, cv ? cv->min_layer : 0, cv ? cv->num_layers : 0,
                    e ? e->has_view : -1, e && e->has_view ? e->view.setmask : 0, e && e->has_view ? e->view.base_level : 0,
                    e && e->has_view ? e->view.num_levels : 0, e && e->has_view ? e->view.min_layer : 0,
                    e && e->has_view ? e->view.num_layers : 0, sg, i, ci, st.rt.ncolor, pr->fi->out_mask, st.channel_mask,
                    ci >= 0 ? (int)((st.color_state[0] >> ci) & 1) : -1, fb_draws);
                fb_draws = 0;
            }
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_any_type[nfeedback] = false;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

static bool gw_ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    VkDescriptorBufferInfo direct;
    if (gw_ubo_direct(r.addr, size, &direct)) return direct;  // (written by a storing shader: read in place, see gw_note)
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// SAVAGEWIND_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vk_dyn_forget();  // (a new buffer may get the old handle: nothing recorded may be taken as still bound)
        setc_gen++;
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; SAVAGEWIND_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("SAVAGEWIND_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

// ---- shaders that store to global memory, and the CPU's copies of guest memory
// A shader with STG writes guest memory (the imported heap) on the GPU, while this thread goes on copying guest memory
// for later draws and dispatches (constant buffers; vertices and indices outside the heap). Run on the GPU while the
// rest of its list was translated, such a shader wrote memory being copied, the copy came out half-written, and the
// GPU faulted on a write (~1 world load in 7). So the rest of such a list went to the GPU in one piece at its end: in
// BOTW's open world ~10 compute dispatches a frame store results this way, the first ~100 draws into the frame, and
// the GPU sat idle until the whole frame was translated (~10 ms) and then rendered it (~8 ms), while the game waits
// for that frame's fence early in the next one.
// Now the memory such a shader may write - its storage buffers - goes into a set of ranges (they come back frame after
// frame). A constant buffer in it is read in place from the imported heap, as the console's GPU reads it (after the
// shader: each pass starts with a full barrier), not copied. Any other CPU copy of memory in the set (vertices or
// indices outside the heap, a constant buffer that can't be bound in place) first waits for the GPU to finish what
// was submitted. The list goes on to the GPU in pieces. A storing shader without storage buffers (it writes through
// other pointers), or a full set, holds the rest of its list as before.
// SAVAGEWIND_VK_GMEM_HOLD=1: hold after every storing shader (as before).
#define GW_MAX 4096
static struct { uint64_t lo, hi; } gw[GW_MAX];  // sorted, disjoint, not touching
static int ngw;
static uint64_t gw_lo = ~0ull, gw_hi;  // (bounds of the set: most reads are nowhere near it)

static int gw_find(uint64_t addr) {  // the first range ending after addr
    int a = 0, b = ngw;
    while (a < b) {
        int m = (a + b) / 2;
        if (gw[m].hi <= addr) a = m + 1; else b = m;
    }
    return a;
}

static bool gw_overlaps(uint64_t lo, uint64_t hi) {
    if (!ngw || hi <= gw_lo || lo >= gw_hi) return false;
    int i = gw_find(lo);
    return i < ngw && gw[i].lo < hi;
}

static bool gw_add(uint64_t lo, uint64_t hi) {  // false: no room
    int i = gw_find(lo);
    if (i < ngw && gw[i].lo <= lo && hi <= gw[i].hi) return true;  // (already in)
    if (i > 0 && gw[i - 1].hi == lo) i--;  // (touching on the left: merge)
    int j = i;
    while (j < ngw && gw[j].lo <= hi) j++;  // [i, j): overlapping or touching
    if (j == i && ngw == GW_MAX) return false;
    if (j > i) {
        if (gw[i].lo < lo) lo = gw[i].lo;
        if (gw[j - 1].hi > hi) hi = gw[j - 1].hi;
        memmove(&gw[i + 1], &gw[j], (ngw - j) * sizeof gw[0]);
        ngw -= j - i - 1;
    } else {
        memmove(&gw[i + 1], &gw[i], (ngw - i) * sizeof gw[0]);
        ngw++;
    }
    gw[i].lo = lo;
    gw[i].hi = hi;
    if (lo < gw_lo) gw_lo = lo;
    if (hi > gw_hi) gw_hi = hi;
    return true;
}

static void gw_note(int stage) {  // a shader of this stage that stores to global memory was recorded
    static int hold = -1;
    if (hold < 0) hold = getenv("SAVAGEWIND_VK_GMEM_HOLD") != NULL;
    if (chunk_hold) return;  // (the rest of the list goes in one piece anyway)
    bool ok = !hold;
    int n = 0;
    for (int i = 0; i < 16 && ok; i++) {
        Range r = st.sbuf[stage][i];
        if (!r.addr || !r.size) continue;
        ok = gw_add(r.addr, r.addr + r.size);
        n++;
    }
    if (!ok || !n) { chunk_hold = true; stat_gw_holds++; }
}

// A constant buffer bound in place (in the set, in the heap, aligned for a uniform buffer descriptor), or false.
static bool gw_ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!gw_overlaps(addr, addr + size) || addr % g_vk.props.limits.minUniformBufferOffsetAlignment) return false;
    VkBuffer b;
    VkDeviceSize off;
    if (!guest_direct(addr, size, &b, &off)) return false;
    *out = (VkDescriptorBufferInfo){b, off, size};
    return true;
}

static bool gw_ubo_copied(int stage, uint32_t cbuf_mask) {  // a constant buffer the stage reads would be copied from the set
    for (uint32_t m = cbuf_mask >> 3; m; m &= m - 1) {  // (cbuf k is bound slot k - 3)
        int i = __builtin_ctz(m);
        if (i >= 18) break;
        Range r = st.ubuf[stage][i];
        uint64_t size = r.size ? (r.size > 0x10000 ? 0x10000 : r.size) : 0x10000;
        VkDescriptorBufferInfo d;
        if (r.addr && gw_overlaps(r.addr, r.addr + size) && !gw_ubo_direct(r.addr, size, &d)) return true;
    }
    return false;
}

// Before a draw's or dispatch's copies (and before its pass begins): copies of memory in the set wait for the GPU.
// cbufs[stage]: the constant buffers each stage reads (ShaderInfo.cbuf_mask; 0: none).
static void gw_check(const uint32_t cbufs[6], bool vertices, uint64_t iaddr, uint64_t isize) {
    if (!ngw) return;
    bool wait = false;
    for (int s = 0; s < 6 && !wait; s++)
        if (cbufs[s]) wait = gw_ubo_copied(s, cbufs[s]);
    VkBuffer b;
    VkDeviceSize off;
    for (int i = 0; i < 16 && vertices && !wait; i++) {
        Range v = st.vbuf[i];
        if (v.addr && !guest_direct(v.addr, v.size ? v.size : 256, &b, &off)) wait = gw_overlaps(v.addr, v.addr + (v.size ? v.size : 256));
    }
    if (iaddr && !wait && !guest_direct(iaddr, isize, &b, &off)) wait = gw_overlaps(iaddr, iaddr + isize);
    if (!wait) return;
    stat_gw_waits++;
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_wait_seq(submit_seq);
    guest_copy_flush();  // (the list's shared copies may hold what was there before)
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    ngw = 0;
    nuub = 0;
    guest_copy_flush();
    gw_lo = ~0ull;
    gw_hi = 0;
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    memset(&prog_memo, 0, sizeof prog_memo);
    memset(&pipe_memo, 0, sizeof pipe_memo);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    st_defaults();
}

// The 3D scene upscaled for this draw (scene_upscale): sampled instead of the scene texture, at its scale.
static struct { NvnTexture *t; VkImageView view; float sx, sy; } scene_up;
static void scene_upscale(Prog *pr, CmdRec *r);

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        NvnTexture *et = e->tex;  // (a finalized one too: textures.c gives it the image of its memory and shape)
        if (et) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. SAVAGEWIND_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("SAVAGEWIND_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)et >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == et && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(et, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = et; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(et, &sc[0], &sc[1]);
            if (et == scene_up.t && stage == 1) { view = scene_up.view; sc[0] = scene_up.sx; sc[1] = scene_up.sy; }
            wh[0] = et->width; wh[1] = et->height;
            for (int k = 0; k < nfeedback; k++)
                if (et == feedback_tex[k] && feedback_any_type[k]) view = feedback_view[k];
                else if (et == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Descriptor sets of identical contents within one submission are one set (the open world draws many objects with
// the same material textures and constants): saves the allocation and the update. SAVAGEWIND_VK_NO_SET_CACHE=1: off.
// Entries are told apart by two independent 64-bit hashes of the contents (no stored key: comparing ~700-byte keys
// from a 3 MB table cost more than the hashing).
#define SETC_TAB 4096
typedef struct { uint64_t seq, hash, hash2; VkDescriptorSetLayout layout; VkDescriptorSet set; uint32_t n; } SetEnt;
static SetEnt *setc;

static VkDescriptorSet set_finish(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, int n) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_SET_CACHE") != NULL;
    stat_sets++;
    SetEnt *e = NULL;
    uint64_t seq = vk_pending_seq() + (setc_gen << 40), h = 0, h2 = 0;
    if (!off) {
        h = (uint64_t)(uintptr_t)layout;
        h2 = h ^ 0x6A09E667F3BCC909ull;
        for (int i = 0; i < n; i++) {
            uint64_t v[4];
            v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
            if (w[i].pImageInfo) {
                v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
                v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
                v[3] = w[i].pImageInfo->imageLayout;
            } else {
                v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
                v[2] = w[i].pBufferInfo->offset;
                v[3] = w[i].pBufferInfo->range;
            }
            for (int k = 0; k < 4; k++) {
                h = (h ^ v[k]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
                h2 = (h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; h2 ^= h2 >> 31;
            }
        }
        h ^= h >> 32;
        if (!setc) setc = calloc(SETC_TAB, sizeof *setc);
        e = &setc[(h ^ (h >> 31)) & (SETC_TAB - 1)];
        if (e->seq == seq && e->hash == h && e->hash2 == h2 && e->layout == layout && e->n == (uint32_t)n) {
            stat_sets_reused++;
            return e->set;
        }
    }
    VkDescriptorSet ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    if (e) {
        e->seq = vk_pending_seq() + (setc_gen << 40);  // (the set lives in this submission's pools)
        e->hash = h; e->hash2 = h2; e->layout = layout; e->set = ds; e->n = (uint32_t)n;
    }
    return ds;
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[65][4] = {{1, 1, 0, 0}};
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    return set_finish(layout, w, n);
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

// Compute pipelines by translated shader and group size, whoever made them first (a dispatch or the pre-warm), as
// WarmPipe for graphics.
typedef struct CsBuild {
    const ShaderInfo *si;
    uint32_t local[3];
    int nbinds;
    VkBindingInfo binds[MAX_BIND];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipe;
    _Atomic int state;  // 1 being built, 2 ready, 3 failed
    struct CsBuild *next;
} CsBuild;
static CsBuild *cs_builds;

static bool cs_get(const ShaderInfo *si, const uint32_t local[3], CsBuild **out) {
    pthread_mutex_lock(&warm_lock);
    CsBuild *c = cs_builds;
    while (c && !(c->si == si && !memcmp(c->local, local, sizeof c->local))) c = c->next;
    if (!c) {
        c = calloc(1, sizeof *c);
        c->si = si;
        memcpy(c->local, local, sizeof c->local);
        c->state = 1;
        c->next = cs_builds;
        cs_builds = c;
        pthread_mutex_unlock(&warm_lock);
        *out = c;
        return true;
    }
    while (atomic_load(&c->state) == 1) pthread_cond_wait(&warm_cond, &warm_lock);
    pthread_mutex_unlock(&warm_lock);
    *out = c;
    return false;
}

static void cs_make(CsBuild *c) {
    VkResult r = VK_ERROR_UNKNOWN;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(c->si->msl, STAGE_COMPUTE, "cs", &n);
    if (spv) {
        c->nbinds = vk_reflect(spv, n, c->binds, MAX_BIND);
        VkShaderModule m = vk_module(spv, n);
        free(spv);
        VkDescriptorSetLayout sl[2] = {0};
        c->layout = make_layout(c->binds, c->nbinds, sl, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
        c->set_layout = sl[0];
        VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
        VkSpecializationInfo spec = {3, me, 12, c->local};
        VkPipelineCreationFeedback fb = {0};
        VkPipelineCreationFeedbackCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                    .pPipelineCreationFeedback = &fb};
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .pNext = &fci,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                    .pSpecializationInfo = &spec},
                                          .layout = c->layout};
        r = m ? vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &c->pipe) : VK_ERROR_INVALID_SHADER_NV;
        if (r == VK_SUCCESS) cache_feedback(&fb);
        else LOG("[compute] pipeline failed: %d\n", r);
    }
    pthread_mutex_lock(&warm_lock);
    atomic_store(&c->state, r == VK_SUCCESS ? 2 : 3);
    pthread_cond_broadcast(&warm_cond);
    pthread_mutex_unlock(&warm_lock);
}

static void build_compute(void *arg) {
    CsProg *cp = arg;
    uint64_t pt0 = SDL_GetTicksNS();
    CsBuild *c;
    if (cs_get(cp->si, cp->local, &c)) {
        cs_make(c);
        if (atomic_load(&c->state) == 2) pso_record(2, vk_translate_id(cp->si), 0, cp->local, sizeof cp->local);
        atomic_fetch_add(&stat_compiles, 1);
        atomic_fetch_add(&prof_c_comp, SDL_GetTicksNS() - pt0);
    } else {
        atomic_fetch_add(&stat_warm, 1);
    }
    cp->p.nbinds = c->nbinds;
    memcpy(cp->p.binds, c->binds, sizeof c->binds[0] * (size_t)c->nbinds);
    cp->p.set_layout[0] = c->set_layout;
    cp->p.layout = c->layout;
    cp->pipe = c->pipe;
    atomic_store(&cp->p.state, atomic_load(&c->state) == 2 ? 1 : 2);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    k.shared_size = ss->control ? *(const uint32_t *)(ss->control + 0x754) : 0;  // (next to the group size at 0x748)
    pthread_mutex_lock(&translate_lock);
    cp->si = vk_translate_cached((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        dump_failed_shader(ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, "cs");
        if (ss->control) dump_failed_shader((uint64_t)(uintptr_t)ss->control, 0x800, "cs_control");
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

// ================================================================== pipeline list and pre-warm
// Every pipeline a draw or a dispatch builds is recorded in cache/vk_pipelines.list: its translated stages (by
// translation id, vk_translate_id) and the fixed state it bakes in (PipeState; compute: the group size). At the next
// start, worker threads build the listed pipelines while the game boots - translations from cache/xlate, SPIR-V from
// cache/spirv, the driver's code from the pipeline cache (device.c) - into the tables draws look in (ProgBuild,
// WarmPipe, CsBuild). The first frame that looks like gameplay (PSO_WAIT_DRAWS draws or more) waits for the rest;
// device.c shows a bar when that takes more than a moment. A draw needing a pipeline a worker is building waits for
// that one; one not reached yet the draw builds itself, as without the list. Pipelines missing from the list build
// where first needed, synchronously as before, and join it.
// One record per pipeline: damaged and repeated records are dropped when the list is read (and the file rewritten).
// New records go to the end of the file every second (vk_pso_flush, from device.c's saver thread) and at exit. The
// header names the GPU and the shader translator build the list is for (each record's check covers them too):
// another, and it starts over. Not the driver: a new driver empties the pipeline cache, and the list is what rebuilds
// it before the game needs it. SAVAGEWIND_NO_SHADER_WARM=1: no pre-warm (pipelines are still recorded).
#define PSO_LIST_VERSION 1
#define PSO_WAIT_DRAWS 800
typedef struct { char magic[8]; uint32_t version, rec_size, vendor, device; char shaders[64]; } PsoHeader;
typedef struct {
    uint32_t kind;   // 1 graphics, 2 compute
    uint32_t check;  // low half of pso_hash
    uint64_t id[2];  // translation ids: vertex and fragment, or compute and 0
    union { PipeState g; uint32_t local[3]; } u;
} PsoRec;

static pthread_mutex_t pso_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pso_seed;    // GPU, translator build, list version
static uint64_t *pso_known;  // pso_hash of the records in the file and pending (open addressing, 0 = free)
static size_t pso_known_cap, pso_known_n;
static PsoRec *pso_pend;     // records not written yet
static int pso_npend, pso_pend_cap;
static PsoRec *pso_recs;     // the list as read at start: what the pre-warm builds
static int pso_nrecs;
static _Atomic int pso_next, pso_finished, pso_workers, pso_built, pso_found, pso_skipped;
static _Atomic bool pso_running;
static uint64_t pso_t0;

static void pso_header(PsoHeader *h) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "SWPSOLST", 8);
    h->version = PSO_LIST_VERSION;
    h->rec_size = sizeof(PsoRec);
    h->vendor = g_vk.props.vendorID;
    h->device = g_vk.props.deviceID;
    snprintf(h->shaders, sizeof h->shaders, "%s", vk_shader_version());
}

static uint64_t pso_hash(const PsoRec *r) {
    uint64_t h = hash_words(pso_seed ^ r->kind, r->id, sizeof r->id);
    h = hash_words(h, &r->u, sizeof r->u);
    return h ? h : 1;
}

static bool pso_known_add(uint64_t h) {  // (pso_lock held) false: there already
    if ((pso_known_n + 1) * 2 > pso_known_cap) {
        size_t cap = pso_known_cap ? pso_known_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < pso_known_cap; i++) {
            if (!pso_known[i]) continue;
            size_t j = pso_known[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = pso_known[i];
        }
        free(pso_known);
        pso_known = t;
        pso_known_cap = cap;
    }
    size_t j = h & (pso_known_cap - 1);
    for (; pso_known[j]; j = (j + 1) & (pso_known_cap - 1))
        if (pso_known[j] == h) return false;
    pso_known[j] = h;
    pso_known_n++;
    return true;
}

static void pso_record(int kind, uint64_t id0, uint64_t id1, const void *state, size_t n) {
    if (!id0 || (kind == 1 && !id1)) return;  // (translations not cached: debugging switches)
    PsoRec r;
    memset(&r, 0, sizeof r);
    r.kind = (uint32_t)kind;
    r.id[0] = id0;
    r.id[1] = id1;
    memcpy(&r.u, state, n < sizeof r.u ? n : sizeof r.u);
    uint64_t h = pso_hash(&r);
    r.check = (uint32_t)h;
    pthread_mutex_lock(&pso_lock);
    if (pso_known_add(h)) {
        if (pso_npend == pso_pend_cap) {
            pso_pend_cap = pso_pend_cap ? pso_pend_cap * 2 : 256;
            pso_pend = realloc(pso_pend, (size_t)pso_pend_cap * sizeof *pso_pend);
        }
        pso_pend[pso_npend++] = r;
    }
    pthread_mutex_unlock(&pso_lock);
}

void vk_pso_flush(void) {
    pthread_mutex_lock(&pso_lock);
    if (pso_npend && pso_seed) {
        char path[512];
        vk_cache_path(path, sizeof path, "vk_pipelines.list");
        FILE *f = fopen(path, "ab");
        if (f) {
            fseek(f, 0, SEEK_END);
            bool ok = true;
            if (ftell(f) == 0) {
                PsoHeader h;
                pso_header(&h);
                ok = fwrite(&h, sizeof h, 1, f) == 1;
            }
            ok = ok && fwrite(pso_pend, sizeof *pso_pend, (size_t)pso_npend, f) == (size_t)pso_npend;
            if (fclose(f)) ok = false;
            if (ok) pso_npend = 0;  // (else tried again next time; a part written is dropped by its check when read)
        }
    }
    pthread_mutex_unlock(&pso_lock);
}

static void pso_load(void) {
    PsoHeader want;
    pso_header(&want);
    pso_seed = hash_words(0xcbf29ce484222325ull, &want, sizeof want);
    char path[512];
    vk_cache_path(path, sizeof path, "vk_pipelines.list");
    FILE *f = fopen(path, "rb");
    const char *why = f ? NULL : "no file yet";
    int bad = 0, dup = 0;
    bool rewrite = false;
    if (f) {
        PsoHeader h;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < (long)sizeof h || fread(&h, sizeof h, 1, f) != 1) why = "too short";
        else if (memcmp(h.magic, want.magic, 8) || h.version != want.version || h.rec_size != want.rec_size) why = "older format";
        else if (h.vendor != want.vendor || h.device != want.device) why = "made on another GPU";
        else if (memcmp(h.shaders, want.shaders, sizeof h.shaders)) why = "made by another shader translator build";
        if (why) {
            rewrite = true;
        } else {
            long n = (size - (long)sizeof h) / (long)sizeof(PsoRec);
            if ((size - (long)sizeof h) % (long)sizeof(PsoRec)) bad++;  // (cut short)
            pso_recs = malloc((size_t)(n ? n : 1) * sizeof *pso_recs);
            n = pso_recs ? (long)fread(pso_recs, sizeof *pso_recs, (size_t)n, f) : 0;
            pthread_mutex_lock(&pso_lock);
            for (long i = 0; i < n; i++) {
                PsoRec *r = &pso_recs[i];
                uint64_t rh = pso_hash(r);
                if ((r->kind != 1 && r->kind != 2) || r->check != (uint32_t)rh) bad++;
                else if (!pso_known_add(rh)) dup++;
                else pso_recs[pso_nrecs++] = *r;
            }
            pthread_mutex_unlock(&pso_lock);
            rewrite = bad || dup;
        }
        fclose(f);
    }
    if (rewrite) {  // (the good records only, or just the header)
        char tmp[540];
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
        FILE *o = fopen(tmp, "wb");
        bool ok = o && fwrite(&want, sizeof want, 1, o) == 1 &&
                  fwrite(pso_recs, sizeof *pso_recs, (size_t)pso_nrecs, o) == (size_t)pso_nrecs;
        if (o && fclose(o)) ok = false;
        if (!ok || !vk_replace_file(tmp, path)) remove(tmp);
    }
    if (why) LOG("[pso] pipeline list: starts empty (%s: %s)\n", path, why);
    else LOG("[pso] pipeline list: %d pipelines (%d damaged and %d repeated records dropped)\n", pso_nrecs, bad, dup);
}

// The fixed state of a record is something this device can build (the list is not keyed by driver).
static bool pso_state_ok(const PipeState *s) {
    if (s->ncolor > 8 || s->topo_class > 2 || !s->samples || s->samples > 64 || (s->samples & (s->samples - 1))) return false;
    if (s->feedback && !g_vk.feedback_loop) return false;
    for (int i = 0; i < 17; i++)
        if (s->divisor[i] > 1 && !g_vk.divisor) return false;
    for (int i = 0; i < 32; i++)
        if (((s->attr_mask >> i) & 1) && (s->attr_bind[i] > ZERO_BINDING || s->attr_fmt[i] >= 256 ||
                                          vertex_format_supported(s->attr_fmt[i]) != s->attr_fmt[i]))
            return false;
    return true;
}

static void pso_warm_one(const PsoRec *r) {
    if (r->kind == 1) {
        ShaderInfo *vi = vk_translate_load_id(r->id[0], STAGE_VERTEX);
        ShaderInfo *fi = vi ? vk_translate_load_id(r->id[1], STAGE_FRAGMENT) : NULL;
        bool made;
        const ProgBuild *b = fi && pso_state_ok(&r->u.g) ? prog_build(vi, fi, &made) : NULL;
        if (!b) { atomic_fetch_add(&pso_skipped, 1); return; }
        WarmPipe *w;
        if (!warm_get(vi, fi, &r->u.g, &w)) { atomic_fetch_add(&pso_found, 1); return; }
        VkPipeline p;
        VkResult res = create_pipeline(&r->u.g, b->vs, b->fs, b->layout, &p);
        warm_done(w, p, res);
    } else {
        ShaderInfo *si = vk_translate_load_id(r->id[0], STAGE_COMPUTE);
        if (!si) { atomic_fetch_add(&pso_skipped, 1); return; }
        CsBuild *c;
        if (!cs_get(si, r->u.local, &c)) { atomic_fetch_add(&pso_found, 1); return; }
        cs_make(c);
    }
    atomic_fetch_add(&pso_built, 1);
}

static void *pso_worker(void *arg) {
    (void)arg;
    // (debugging) SAVAGEWIND_PSO_SLOW_MS=<n>: each pipeline takes n ms longer, to see the wait and its bar (device.c
    // then saves the first bar frame as build/pso_wait.png)
    int slow = getenv("SAVAGEWIND_PSO_SLOW_MS") ? atoi(getenv("SAVAGEWIND_PSO_SLOW_MS")) : 0;
    for (int i; (i = atomic_fetch_add(&pso_next, 1)) < pso_nrecs;) {
        pso_warm_one(&pso_recs[i]);
        if (slow > 0) SDL_Delay((Uint32)slow);
        atomic_fetch_add(&pso_finished, 1);
    }
    if (atomic_fetch_sub(&pso_workers, 1) == 1) {
        LOG("[pso] pre-warm done in %.0f ms: %d pipelines built, %d built first by draws, %d skipped (shaders missing or "
            "state unsupported); pipeline cache %d hits, %d misses\n", (SDL_GetTicksNS() - pso_t0) / 1e6, atomic_load(&pso_built),
            atomic_load(&pso_found), atomic_load(&pso_skipped), atomic_load(&pso_cache_hits), atomic_load(&pso_cache_misses));
        atomic_store(&pso_running, false);
    }
    return NULL;
}

static void pso_init(void) {
    pso_load();
    // (without the translation cache, draws translate anew and never meet what the pre-warm loads)
    if (!pso_nrecs || getenv("SAVAGEWIND_NO_SHADER_WARM") || getenv("SAVAGEWIND_NO_XLATE_CACHE") || maxwell_translation_uncacheable())
        return;
    // most of the CPU (as run_job), leaving some to the game booting meanwhile
    int n = SDL_GetNumLogicalCPUCores() - 4;
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    pso_t0 = SDL_GetTicksNS();
    atomic_store(&pso_workers, n);
    atomic_store(&pso_running, true);
    LOG("[pso] pre-warm: %d pipelines on %d threads\n", pso_nrecs, n);
    for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, pso_worker, NULL); }
}

// device.c, once per frame presented: whether this frame waits for the pre-warm (it looks like gameplay and the
// pre-warm is still going), and the wait itself in steps of ms (true once done; done: the part built).
bool vk_pso_wait_needed(void) {
    static uint64_t mark;
    uint64_t d = cum_draws - mark;
    mark = cum_draws;
    return atomic_load(&pso_running) && d >= PSO_WAIT_DRAWS;
}

bool vk_pso_wait(int ms, float *done) {
    for (int t = 0; t < ms && atomic_load(&pso_running); t += 5) SDL_Delay(5);
    *done = pso_nrecs ? (float)atomic_load(&pso_finished) / (float)pso_nrecs : 1.0f;
    return !atomic_load(&pso_running);
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    gw_check((const uint32_t[6]){0, 0, 0, 0, 0, cp->si->cbuf_mask}, false, 0, 0);
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    if (g_debug_frame) {
        LOG("[dispatch] %ux%ux%u cs %llx gmem %d cb %#x sbuf:", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr,
            cp->si->uses_gmem, cp->si->cbuf_mask);
        for (int i = 0; i < 16; i++)
            if (st.sbuf[5][i].addr) LOG(" %d=%llx+%llu", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
        LOG("\n");
    }
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
    chunk_draws++;
    if (cp->si->writes_gmem) {
        if (getenv("SAVAGEWIND_LOG_HOLD")) {
            LOG("[hold] frame %llu draw %llu: cs %llx sbuf:", (unsigned long long)renderer_present_count(),
                (unsigned long long)stat_draws, (unsigned long long)st.program->stages[5].code_addr);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" %llx+%llx", (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
        gw_note(5);
    }
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// (debugging) SAVAGEWIND_DEBUG_PICK=<x>,<y>[;<x>,<y>...] (up to 4 points, first-target pixels): in a debug frame each
// draw copies those pixels of its first target afterwards (its own pass), and vk_pick_report logs, per point, the
// draws that changed it ("[pick]"): the last one is what the frame shows there.
typedef struct { uint64_t vs, fs, hash; void *img; uint32_t w, h, fmt, no; } PickRec;
static struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; int n, npt, on; int pt[4][2]; uint32_t draws; PickRec rec[4096]; } pick = {.on = -1};

static void pick_after_draw(void) {
    if (pick.on < 0) {
        pick.on = 0;
        for (const char *e = getenv("SAVAGEWIND_DEBUG_PICK"); e && *e && pick.npt < 4;) {
            if (sscanf(e, "%d,%d", &pick.pt[pick.npt][0], &pick.pt[pick.npt][1]) != 2) break;
            pick.npt++;
            e = strchr(e, ';');
            if (e) e++;
        }
        pick.on = pick.npt > 0;
    }
    pick.draws++;
    if (!pick.on || !st.rt.ncolor || !st.rt.color[0] || !st.rt.color[0]->backend || pick.n >= 4096) return;
    TexBackend *b = st.rt.color[0]->backend;
    if ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->samples > 1 || b->width < 64) return;
    if (!pick.buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 * 4 * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &pick.buf, &pick.mem, &info));
        pick.ptr = info.pMappedData;
    }
    uint32_t layer = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) ? st.rt.cview[0].min_layer : 0;
    uint32_t level = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
    if (level >= b->levels || layer >= b->layers) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int k = 0; k < pick.npt; k++) {
        uint32_t x = (uint32_t)pick.pt[k][0] * b->width / 1600 >> level, y = (uint32_t)pick.pt[k][1] * b->height / 900 >> level;
        memset(pick.ptr + ((size_t)pick.n * 4 + k) * 16, 0, 16);
        if (x >= level_extent(b->width, level) || y >= level_extent(b->height, level)) continue;
        VkBufferImageCopy c = {.bufferOffset = ((VkDeviceSize)pick.n * 4 + k) * 16,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                               .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, pick.buf, 1, &c);
    }
    PickRec *r = &pick.rec[pick.n++];
    r->vs = st.program->stages[0].code_addr;
    r->fs = st.program->stages[1].code_addr;
    r->hash = g_cur_fs_hash;
    r->img = (void *)b->image;
    r->w = b->width; r->h = b->height; r->fmt = st.rt.color[0]->format; r->no = pick.draws;
}

void vk_pick_report(void) {
    if (!pick.n) return;
    renderer_flush();
    for (int k = 0; k < pick.npt; k++) {
        LOG("[pick] point %d (%d,%d of 1600x900): draws that changed it\n", k, pick.pt[k][0], pick.pt[k][1]);
        for (int i = 0; i < pick.n; i++) {
            const uint8_t *v = pick.ptr + ((size_t)i * 4 + k) * 16;
            int j = i - 1;  // the previous draw into the same image
            while (j >= 0 && pick.rec[j].img != pick.rec[i].img) j--;
            if (j >= 0 && !memcmp(v, pick.ptr + ((size_t)j * 4 + k) * 16, 16)) continue;
            const uint32_t *u = (const uint32_t *)v;
            LOG("[pick]   draw %u fs %llx hash %016llx vs %llx | target %ux%u f%#x [%p] | %08x %08x %08x %08x\n", pick.rec[i].no,
                (unsigned long long)pick.rec[i].fs, (unsigned long long)pick.rec[i].hash, (unsigned long long)pick.rec[i].vs,
                pick.rec[i].w, pick.rec[i].h, pick.rec[i].fmt, pick.rec[i].img, u[0], u[1], u[2], u[3]);
        }
    }
    pick.n = 0;
    pick.draws = 0;
}

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROFD_T0();
    draw_(r, indexed);
    PROFD_ADD(prof_draw);
}
static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (profd) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { stat_skipped++; return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr || atomic_load(&pr->state) != 1) {
        if (st.rt.ncolor && vk_trace_tex(st.rt.color[0]))
            vk_trace_event(st.rt.color[0], 2, NULL, 0, "draw skipped: program %s", pr ? "not compiled yet" : "missing");
        stat_skipped++;
        return;
    }
    {   // fragment-shader hash: skip lists (SAVAGEWIND_SKIP_FSHASH), debug logs, GPU profile labels
        static uint64_t last_fs, last_hash;
        static bool last_skip;
        static FsSel skip[16];
        static int nskip = -1;
        if (nskip < 0) nskip = fs_sel_parse(getenv("SAVAGEWIND_SKIP_FSHASH"), skip, 16);
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            // (by code address, like the program table: 512 bytes hashed byte by byte at every fragment shader change
            // cost 1% of the translation)
            static struct { uint64_t fs, hash; } memo[512];
            uint32_t ms = (uint32_t)(fs >> 4) & 511;
            if (fs && memo[ms].fs == fs) last_hash = memo[ms].hash;
            else {
                last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
                memo[ms].fs = fs; memo[ms].hash = last_hash;
            }
            last_skip = false;
            for (int i = 0; i < nskip; i++) last_skip |= fs_sel_match(&skip[i], fs, last_hash);
        }
        extern volatile int g_skip_fs_off;
        if (last_skip && !g_skip_fs_off) { stat_skipped++; return; }
        if (g_debug_frame && getenv("SAVAGEWIND_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    if (g_debug_frame) { debug_draw(pr, r, indexed); pass_dbg_draws++; }
    {   // (debugging) SAVAGEWIND_LOG_FS_CB=<fs hash>:<k>:<first float>:<count>: each frame, those floats of fragment cbuf k
        // (c[k], as the debug frame's fcb<k>) at the first draw of that shader: a parameter's course over a scripted walk
        static int on = -1;
        static uint64_t want, last_frame = ~0ull;
        static unsigned k, first, count;
        if (on < 0) {
            const char *e = getenv("SAVAGEWIND_LOG_FS_CB");
            unsigned long long h;
            on = e && sscanf(e, "%llx:%u:%u:%u", &h, &k, &first, &count) == 4 && k >= 3 && k < 21;
            want = on ? h : 0;
        }
        if (on && g_cur_fs_hash == want && renderer_present_count() != last_frame) {
            last_frame = renderer_present_count();
            Range u = st.ubuf[1][k - 3];
            char buf[1024];
            int o = snprintf(buf, sizeof buf, "[fscb] frame %llu c%u@%llx:", (unsigned long long)last_frame, k, (unsigned long long)u.addr);
            for (unsigned i = 0; i < count && u.addr && (first + i + 1) * 4 <= u.size && o < (int)sizeof buf - 16; i++)
                o += snprintf(buf + o, sizeof buf - o, " %g", ((const float *)(uintptr_t)u.addr)[first + i]);
            LOG("%s\n", buf);
        }
    }
    {   // (SAVAGEWIND_TRACE_TEX) a draw into a traced texture: its fragment shader and the textures it samples
        bool any = false;
        for (int i = 0; i < st.rt.ncolor && i < 8 && !any; i++) any = vk_trace_tex(st.rt.color[i]);
        if (any) {
            NvnTexture *srcs[16];
            int ns = 0;
            char buf[240];
            int o = 0;
            for (int sg = 0; sg < 2; sg++) {
                ShaderInfo *si = sg ? pr->fi : pr->vi;
                for (int i = 0; i < si->ntex && ns < 16; i++) {
                    NvnTexture *t = shader_texture_object(sg, &si->tex[i]);
                    srcs[ns++] = t;
                    if (t && o < (int)sizeof buf - 12) o += snprintf(buf + o, sizeof buf - o, " #%llu", (unsigned long long)t->debug_id);
                }
            }
            {   // the vertex stage's first uniform buffer (c[3]): its words 0x50..0x70 (BOTW's 2D quads: uv transform)
                Range u = st.ubuf[0][0];
                if (u.addr && u.size >= 0x70 && o < (int)sizeof buf - 60) {
                    const float *f = (const float *)(uintptr_t)(u.addr + 0x50);
                    o += snprintf(buf + o, sizeof buf - o, " | vs %llx c3@%llx %g %g %g %g / %g %g %g", (unsigned long long)
                                  st.program->stages[0].code_addr, (unsigned long long)u.addr, f[0], f[1], f[2], f[3], f[4],
                                  f[5], f[6]);
                }
            }
            buf[o] = 0;
            for (int i = 0; i < st.rt.ncolor && i < 8; i++)
                if (st.rt.color[i])
                    vk_trace_event(st.rt.color[i], g_cur_fs_hash + (uint64_t)i, srcs, ns,
                                   "draw as target %d (of %d)%s, fs %llx hash %016llx out %#x, mask %#x, samples%s", i,
                                   st.rt.ncolor, draw_writes_color(pr, i) ? "" : " NOT WRITTEN",
                                   (unsigned long long)st.program->stages[1].code_addr, (unsigned long long)g_cur_fs_hash,
                                   pr->fi->out_mask, st.channel_mask, buf);
        }
    }
    fb_draws++;
    resolve_feedback(pr);
    scene_upscale(pr, r);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    {
        uint32_t sm = st.program->stage_mask, all = ~0u;  // (geometry and tessellation stages: every bound buffer)
        uint32_t cbufs[6] = {pr->vi->cbuf_mask, pr->fi->cbuf_mask, sm & 4 ? all : 0, sm & 8 ? all : 0, sm & 16 ? all : 0, 0};
        gw_check(cbufs, true, indexed ? r->x[3] : 0, indexed ? (uint64_t)r->x[2] * 4 : 0);
    }
    begin_pass();
    if (st.rt.depth && !depth_read_only()) pass_ds_written = true;
    for (int i = 0; i < st.rt.ncolor && i < 8; i++)
        if (st.rt.color[i] && draw_writes_color(pr, i)) {
            pass_color_written |= (uint8_t)(1u << i);
            pass_color_unsynced |= (uint8_t)(1u << i);
        }
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }

    uint64_t prof_tk_ = profd ? SDL_GetTicksNS() : 0;
    if (profd) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.s.ncolor = pass_ncolor;
    // only the pass's formats: the slots past them hold earlier passes' (not part of the pipeline, and different
    // leftovers made duplicate pipelines)
    memcpy(k.s.color, pass_color, sizeof k.s.color[0] * (pass_ncolor < 8 ? pass_ncolor : 8));
    k.s.depth = pass_depth;
    k.s.stencil = pass_stencil;
    k.s.samples = pass_samples;
    k.s.feedback = draw_feedback;
    k.s.depth_clamp = st.depth_clamp && g_vk.depth_clamp;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.s.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.s.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.s.attr_bind[i] = ZERO_BINDING;
            k.s.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.s.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.s.attr_off[i] = off;
        k.s.attr_bind[i] = stream;
        k.s.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.s.used_streams & (1u << s))) continue;
        k.s.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.s.divisor[s] = s < st.nstream && k.s.stride[s] ? st.stream[s][1] : 0;
        if (k.s.divisor[s] > 1 && !g_vk.divisor) k.s.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.s.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.s.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.s.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.s.blend_on |= (uint8_t)(1u << i);
            memcpy(k.s.blend[i] + 1, st.blend[i] + 1, 6);  // (the factors and ops; 0 is the target index, 7 padding)
        }
    }
    k.s.a2c = st.ms[0xC] != 0;
    k.s.topo_class = cls;
    k.s.poly_fill = st.poly[2] != 1;
    Pipe *pp = pipe_memo.pp;
    if (!pp || memcmp(&pipe_memo.k, &k, sizeof k)) {
        uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
        if (!ph) ph = 1;
        uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
        while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
        pp = pipe_tab[pslot].pipe;
        if (!pp) {
            pp = calloc(1, sizeof *pp);
            pp->k = k;
            pipe_tab[pslot].key = ph;
            pipe_tab[pslot].pipe = pp;
            run_job(build_pipeline, pp);
        }
        pipe_memo.k = k;
        pipe_memo.pp = pp;
    }
    if (atomic_load(&pp->state) != 1) {
        if (st.rt.ncolor && vk_trace_tex(st.rt.color[0]))
            vk_trace_event(st.rt.color[0], 3, NULL, 0, "draw skipped: pipeline %s", pp->state ? "failed" : "compiling");
        stat_skipped++;
        return;
    }
    if (no_dyn_cache) vk_dyn_forget();
    dyn_check();
    if (!DYN_SAME(pipe, pp->pipe)) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = profd ? SDL_GetTicksNS() : 0;
    if (profd) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("SAVAGEWIND_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (!DYN_SAME(dt, (uint32_t)dt)) vkCmdSetDepthTestEnable(cb, dt);
    if (!DYN_SAME(dw, (uint32_t)(dt && st.ds[1]))) vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). SAVAGEWIND_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("SAVAGEWIND_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!DYN_SAME(dop, (uint32_t)dop)) vkCmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    if (!DYN_SAME(sten, (uint32_t)sten)) vkCmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t *c = dc.sten_face[f];
            uint32_t fail = stencil_op(o & 0xF), pass = stencil_op((o >> 8) & 0xF), zfail = stencil_op((o >> 4) & 0xF);
            uint32_t cmp = compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
            if (c[0] != fail || c[1] != pass || c[2] != zfail || c[3] != cmp) {
                vkCmdSetStencilOp(cb, face, fail, pass, zfail, cmp);
                c[0] = fail; c[1] = pass; c[2] = zfail; c[3] = cmp;
            }
            if (c[4] != (st.stencil_vmask[f] & 0xFF)) vkCmdSetStencilCompareMask(cb, face, c[4] = st.stencil_vmask[f] & 0xFF);
            if (c[5] != (st.stencil_mask[f] & 0xFF)) vkCmdSetStencilWriteMask(cb, face, c[5] = st.stencil_mask[f] & 0xFF);
            if (c[6] != (uint32_t)st.stencil_ref[f]) vkCmdSetStencilReference(cb, face, c[6] = st.stencil_ref[f]);
        }
    }
    if (memcmp(dc.blend, st.blend_color, sizeof dc.blend)) {
        vkCmdSetBlendConstants(cb, st.blend_color);
        memcpy(dc.blend, st.blend_color, sizeof dc.blend);
    }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (!DYN_SAME(cull, (uint32_t)cm)) vkCmdSetCullMode(cb, cm);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // SAVAGEWIND_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("SAVAGEWIND_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (!DYN_SAME(front, (uint32_t)ff)) vkCmdSetFrontFace(cb, ff);
    VkPrimitiveTopology pt = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (!DYN_SAME(topo, (uint32_t)pt)) vkCmdSetPrimitiveTopology(cb, pt);
    bool restart = st.prim_restart && native && (mode == 3 || mode == 5);
    if (!DYN_SAME(restart, (uint32_t)restart)) vkCmdSetPrimitiveRestartEnable(cb, restart);
    {   // nvnCommandBufferSetLineWidth, at the target's scale (BOTW's noise meter strokes its wave at 3 px, then 1.5 px)
        float lw = 1.0f;
        if (cls == 1 && g_vk.wide_lines) {
            const float *range = g_vk.props.limits.lineWidthRange;
            lw = st.line_width * pass_sy;
            lw = lw < range[0] ? range[0] : lw > range[1] ? range[1] : lw;
        }
        if (dc.line_w != lw) {  // (NaN after dyn_check: always set)
            vkCmdSetLineWidth(cb, lw);
            dc.line_w = lw;
        }
    }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("SAVAGEWIND_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (!DYN_SAME(bias_on, (uint32_t)on)) vkCmdSetDepthBiasEnable(cb, on);
        if (memcmp(dc.bias, f, sizeof f)) {
            vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
            memcpy(dc.bias, f, sizeof f);
        }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // per-draw tint (translate.c `ltint`): no game-specific highlight sets one yet
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (profd) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROFD_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROFD_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = profd ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (profd) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {
        float k = 1.0f;  // (horizontal stretch of a camera-attached effect: none, the view keeps its 16:9 shape)
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f | depth range %.7f %.7f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy, zn, zf);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = profd ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (profd) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    if (dc.layout != pr->layout || dc.sets[0] != vset || dc.sets[1] != fset) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);
        dc.layout = pr->layout; dc.sets[0] = vset; dc.sets[1] = fset;
    }

    uint64_t prof_t2_ = profd ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && (dc.ibuf != ib.buf || dc.ioff != ib.off || dc.itype != (uint32_t)itype_vk)) {
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
        dc.ibuf = ib.buf; dc.ioff = ib.off; dc.itype = itype_vk;
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.s.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (dc.vbuf[s] != dbuf || dc.voff[s] != doff) {
                    vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                    dc.vbuf[s] = dbuf; dc.voff[s] = doff;
                }
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.s.stride[s], need;
            if (!stride) need = 256;
            else if (k.s.divisor[s]) need = ((uint64_t)base_inst + inst + k.s.divisor[s] - 1) / k.s.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (dc.vbuf[s] != sp.buf || dc.voff[s] != off) {
            vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
            dc.vbuf[s] = sp.buf; dc.voff[s] = off;
        }
    }

    if (profd) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    stat_draws++;
    chunk_draws++;
    if (pr->vi->writes_gmem || pr->fi->writes_gmem) {
        if (pr->vi->writes_gmem) gw_note(0);
        if (pr->fi->writes_gmem) gw_note(1);
        if (getenv("SAVAGEWIND_LOG_HOLD")) LOG("[hold] frame %llu draw %llu: vs %llx (%d) fs %llx (%d) fs hash %016llx\n",
                                               (unsigned long long)renderer_present_count(), (unsigned long long)stat_draws,
                                               (unsigned long long)st.program->stages[0].code_addr, pr->vi->writes_gmem,
                                               (unsigned long long)st.program->stages[1].code_addr, pr->fi->writes_gmem,
                                               (unsigned long long)g_cur_fs_hash);
    }
    cum_draws++;
    if (g_debug_frame) pick_after_draw();
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_save_channels(TexBackend *b, const char *path);

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) LOG(" %ux%u/f%#x/fl%#x@%llx[%p]", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format,
                                st.rt.color[i]->flags, (unsigned long long)st.rt.color[i]->addr,
                                st.rt.color[i]->backend ? (void *)((TexBackend *)st.rt.color[i]->backend)->image : NULL);
    if (st.rt.depth) LOG(" d%#x %ux%u@%llx[%p]", st.rt.depth->format, st.rt.depth->width, st.rt.depth->height,
                         (unsigned long long)st.rt.depth->addr,
                         st.rt.depth->backend ? (void *)((TexBackend *)st.rt.depth->backend)->image : NULL);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    for (int i = 0; i < st.rt.ncolor && i < 8; i++)  // blend func per enabled target: srcC dstC srcA dstA eqC eqA
        if ((st.color_state[0] >> i) & 1)
            LOG(" | bf%d %x %x %x %x %x %x", i, st.blend[i][1], st.blend[i][2], st.blend[i][3], st.blend[i][4], st.blend[i][5], st.blend[i][6]);
    LOG(" | fs out %#x | poly3 %#x offset %g %g %g | clamp %d\n", pr->fi->out_mask, st.poly[3], st.poly_offset[0], st.poly_offset[1],
        st.poly_offset[2], st.depth_clamp);
    for (int i = 0; i < st.rt.ncolor; i++)  // target views (layer / level selections)
        if (st.rt.color[i] && st.rt.has_cview[i])
            LOG("[draw]   rt%d view: set %#x levels %u+%u layers %u+%u (texture target %u depth %u)\n", i, st.rt.cview[i].setmask,
                st.rt.cview[i].base_level, st.rt.cview[i].num_levels, st.rt.cview[i].min_layer, st.rt.cview[i].num_layers,
                st.rt.color[i]->target, st.rt.color[i]->depth);
    if (getenv("SAVAGEWIND_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        if (r->x[2] <= 6 && (pr->vi->cbuf_mask & 2)) {  // vertex compiler constants (c1, after the code)
            NvnShaderStage *ss = &st.program->stages[0];
            const float *u = (const float *)(uintptr_t)(ss->code_addr + ss->const_offset);
            LOG("[draw]   vcb1:");
            for (int k = 0; k < 16 && k * 4 < (int)(ss->const_size ? ss->const_size : 16); k++) LOG(" %.9g", u[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18 && r->x[2] <= 6; c++) {  // vertex cbufs of full-screen draws (k - 3: binding k)
            if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
            LOG("[draw]   vcb%d %llx size %u:", c + 3, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
            for (int k = 0; k < 128 && k * 4 < (int)st.ubuf[0][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {  // (binding k is ubuf k - 3)
            if (!((pr->fi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c + 3, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 128 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s%s addr %llx [%p]\n", i, (void *)t, t->width, t->height,
            t->format, b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0,
            b && b->scalable ? " scalable" : "", t->finalized ? " FINALIZED" : "", (unsigned long long)t->addr,
            b ? (void *)b->image : NULL);
        LOG("[draw]   shape%d: target %u depth %u levels %u -> layers %u%s\n", i, t->target, t->depth, t->levels, b ? b->layers : 0,
            b && b->base_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? " (array)" : "");
        {   // its sampler and swizzle (view swizzle if the pool entry overrides it)
            const NvnTexEntry *e = shader_texture_entry(1, &pr->fi->tex[i]);
            if (e && e->has_view)
                LOG("[draw]   view%d: set %#x levels %u+%u layers %u+%u format %#x target %u\n", i, e->view.setmask, e->view.base_level,
                    e->view.num_levels, e->view.min_layer, e->view.num_layers, e->view.format, e->view.target);
            int slot = ((int)pr->fi->tex[i].handle_offset - 0x20) / 8;
            uint64_t h = (slot >= 0 && slot < 32) ? st.tex[1][slot] : 0;
            uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
            const NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
            const uint8_t *sw = e && e->has_view && (e->view.setmask & 8) ? e->view.swizzle : t->swizzle;
            LOG("[draw]   smp%d: type %d swz %u%u%u%u%s h %llx tsc %u", i, pr->fi->tex[i].type, sw[0], sw[1], sw[2], sw[3],
                e && e->has_view ? " (view)" : "", (unsigned long long)h, tsc);
            if (sd) LOG(" filt %u/%u wrap %u %u %u lod %g..%g bias %g aniso %g cmp %u/%u", sd->min_filter, sd->mag_filter,
                        sd->wrap[0], sd->wrap[1], sd->wrap[2], sd->lod_min, sd->lod_max, sd->lod_bias, sd->max_aniso,
                        sd->compare_mode, sd->compare_func);
            LOG("\n");
        }
    }
    {   // SAVAGEWIND_DEBUG_TEXDUMP=<fs>[,...] (see FsSel): the first such draw of a debug frame saves its fragment
        // and vertex textures (build/dtex_<hash>_<i>... / _v<i>..., see debug_save_channels; depth as .pgm) and logs
        // its vertex layout, its first vertices and its uniform buffers
        static FsSel want[8];
        static uint64_t done[8];
        static int nwant = -1;
        static int all = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_ALL=1: every such draw also saves its textures whole (RGB),
                              // build/dtex_<hash>_d<draw>_<i>.png, <draw> counting the frame's draws as [pick] does
        if (nwant < 0) nwant = fs_sel_parse(getenv("SAVAGEWIND_DEBUG_TEXDUMP"), want, 8);
        if (all < 0) all = getenv("SAVAGEWIND_DEBUG_TEXDUMP_ALL") != NULL;
        for (int k = 0; k < nwant && all; k++) {
            if (!fs_sel_match(&want[k], st.program->stages[1].code_addr, g_cur_fs_hash)) continue;
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                if (!b || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_d%u_%d.png", (unsigned long long)g_cur_fs_hash, pick.draws + 1, i);
                renderer_save_texture_tex(b, path);
                LOG("[draw]   tex%d %ux%u f%#x saved as %s\n", i, t->width, t->height, t->format, path);
            }
        }
        static long only_draw = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_DRAW=<n>: that draw (numbered as [pick] does), not the first
        if (only_draw < 0) only_draw = getenv("SAVAGEWIND_DEBUG_TEXDUMP_DRAW") ? atol(getenv("SAVAGEWIND_DEBUG_TEXDUMP_DRAW")) : 0;
        static long nth = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_NTH=<n>: the n-th draw of that shader in the frame (from 1)
        if (nth < 0) nth = getenv("SAVAGEWIND_DEBUG_TEXDUMP_NTH") ? atol(getenv("SAVAGEWIND_DEBUG_TEXDUMP_NTH")) : 0;
        static uint64_t seen_frame[8];
        static long seen[8];
        for (int k = 0; k < nwant; k++) {
            if (!fs_sel_match(&want[k], st.program->stages[1].code_addr, g_cur_fs_hash) || done[k] == renderer_present_count() + 1)
                continue;
            if (only_draw && pick.draws + 1 != (uint32_t)only_draw) continue;
            if (seen_frame[k] != renderer_present_count() + 1) seen_frame[k] = renderer_present_count() + 1, seen[k] = 0;
            if (nth && ++seen[k] != nth) continue;
            done[k] = renderer_present_count() + 1;
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                if (!b) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_%d.%s", (unsigned long long)g_cur_fs_hash, i,
                         b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? "pgm" : "png");
                vk_end_pass();
                if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) debug_save_depth(b, path);
                else debug_save_channels(b, path);
                LOG("[draw]   tex%d saved as %s\n", i, path);
            }
            for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // the vertex shader's textures too (vtex<i>)
                NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                const NvnTexEntry *e = shader_texture_entry(0, &pr->vi->tex[i]);
                if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
                LOG("[draw]   vtex%d: %ux%ux%u f%#x target %u levels %u flags %#x stride %u addr %llx%s", i, t->width, t->height, t->depth,
                    t->format, t->target, t->levels, t->flags, (unsigned)t->stride, (unsigned long long)t->addr, t->finalized ? " FINALIZED" : "");
                if (e && e->has_view)
                    LOG(" view set %#x levels %u+%u layers %u+%u format %#x", e->view.setmask, e->view.base_level, e->view.num_levels,
                        e->view.min_layer, e->view.num_layers, e->view.format);
                {
                    int slot = ((int)pr->vi->tex[i].handle_offset - 0x20) / 8;
                    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[0][slot] : 0;
                    uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
                    const NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                    LOG(" | type %d h %llx tsc %u", pr->vi->tex[i].type, (unsigned long long)h, tsc);
                    if (sd) LOG(" filt %u/%u wrap %u %u %u lod %g..%g", sd->min_filter, sd->mag_filter, sd->wrap[0], sd->wrap[1],
                                sd->wrap[2], sd->lod_min, sd->lod_max);
                }
                LOG("\n");
                if (!b) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_v%d.%s", (unsigned long long)g_cur_fs_hash, i,
                         b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? "pgm" : "png");
                vk_end_pass();
                if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) debug_save_depth(b, path);
                else debug_save_channels(b, path);
            }
            LOG("[draw]   attribs:");  // the vertex layout: format / stream / offset, and each stream's first bytes
            for (int a = 0; a < 16; a++)
                if ((pr->vi->attr_mask >> a) & 1)
                    LOG(" a%d=%#x/s%u/+%u", a, a < st.nattrib ? st.attrib[a] & 0xFF : 0, a < st.nattrib ? (st.attrib[a] >> 8) & 0x1F : 99,
                        a < st.nattrib ? st.attrib[a] >> 13 : 0);
            LOG("\n");
            for (int s = 0; s < st.nstream && s < 16; s++) {
                if (!st.vbuf[s].addr) continue;
                const float *f = (const float *)(uintptr_t)st.vbuf[s].addr;
                LOG("[draw]   stream%d stride %u divisor %u %llx size %llu:", s, st.stream[s][0], st.stream[s][1],
                    (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
                for (uint64_t k = 0; k < 64 && k * 4 < st.vbuf[s].size; k++) LOG("%s%.6g", k % 8 ? " " : "\n[draw]     ", f[k]);
                LOG("\n");
            }
            if (indexed) {
                LOG("[draw]   indices (type %llu, base vertex %lld):", (unsigned long long)r->x[1], (long long)(int32_t)r->x[4]);
                for (uint64_t k = 0; k < r->x[2] && k < 64; k++)
                    LOG(" %u", r->x[1] == 0 ? ((const uint8_t *)(uintptr_t)r->x[3])[k] : r->x[1] == 1 ? ((const uint16_t *)(uintptr_t)r->x[3])[k]
                                                                                       : ((const uint32_t *)(uintptr_t)r->x[3])[k]);
                LOG("\n");
            }
            for (uint64_t k = 0; k < r->x[2] && k < 24; k++) {  // the first vertices' attributes, raw bytes in hex
                uint64_t v = indexed ? (r->x[1] == 0 ? ((const uint8_t *)(uintptr_t)r->x[3])[k] : r->x[1] == 1 ? ((const uint16_t *)(uintptr_t)r->x[3])[k]
                                                                                                 : ((const uint32_t *)(uintptr_t)r->x[3])[k]) + (int32_t)r->x[4]
                                     : r->x[1] + k;
                LOG("[draw]   vertex %llu:", (unsigned long long)v);
                for (int a = 0; a < 16 && a < st.nattrib; a++) {
                    if (!((pr->vi->attr_mask >> a) & 1)) continue;
                    uint32_t fmt = st.attrib[a] & 0xFF, s = (st.attrib[a] >> 8) & 0x1F, off = st.attrib[a] >> 13;
                    if (s >= 16 || !st.vbuf[s].addr) continue;
                    uint32_t bpb = nvn_format_info(fmt)->bpb;
                    uint64_t at = v * st.stream[s][0] + off;
                    if (at + bpb > st.vbuf[s].size) { LOG(" a%d=(out)", a); continue; }
                    const uint8_t *p = (const uint8_t *)(uintptr_t)(st.vbuf[s].addr + at);
                    LOG(" a%d=", a);
                    for (uint32_t b = 0; b < bpb && b < 16; b++) LOG("%02x", p[b]);
                }
                LOG("\n");
            }
            for (int sg = 0; sg < 2; sg++)  // and every uniform buffer either stage reads, whole (up to 4 KiB)
                for (int c = 0; c < 18; c++) {
                    ShaderInfo *si = sg ? pr->fi : pr->vi;
                    Range u = st.ubuf[sg][c];
                    if (!((si->cbuf_mask >> (c + 3)) & 1) || !u.addr) continue;
                    const float *f = (const float *)(uintptr_t)u.addr;
                    LOG("[draw]   %ccb%d %llx size %u:", sg ? 'f' : 'v', c + 3, (unsigned long long)u.addr, (unsigned)u.size);
                    for (uint32_t k = 0; k < 1024 && k * 4 < u.size; k++)
                        LOG("%s%.6g", k % 16 == 0 ? "\n[draw]     " : k % 4 ? " " : " | ", f[k]);
                    LOG("\n");
                }
        }
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    vk_trace_event(t, 0, NULL, 0, "copy from buffer %llx", (unsigned long long)src);
    if (getenv("SAVAGEWIND_LOG_COPIES")) {  // (debugging)
        static int n;
        if (n++ < 200)
            LOG("[copy] buffer %llx -> #%llu %ux%ux%u target %u f%#x level %d region %d,%d,%d %dx%dx%d view set %#x layer %u\n",
                (unsigned long long)src, (unsigned long long)t->debug_id, t->width, t->height, t->depth, t->target, t->format,
                level, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], dst->has_view ? dst->view.setmask : 0,
                dst->has_view ? dst->view.min_layer : 0);
    }
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

// (debugging) the last texture-to-buffer copies: backend_debug_copies_at logs those that wrote a guest address
static struct { uint64_t dst, size, frame; NvnTexture *t; int32_t rg[6]; } t2b_log[16];
static unsigned t2b_next;
void backend_debug_copies_at(uint64_t a) {
    for (int i = 0; i < 16; i++)
        if (t2b_log[i].t && a >= t2b_log[i].dst && a < t2b_log[i].dst + t2b_log[i].size)
            LOG("[debug]   CopyTextureToBuffer frame %llu from #%llu %ux%u format %#x region %d,%d %dx%d -> %llx+%llx\n",
                (unsigned long long)t2b_log[i].frame, (unsigned long long)t2b_log[i].t->debug_id, t2b_log[i].t->width,
                t2b_log[i].t->height, t2b_log[i].t->format, t2b_log[i].rg[0], t2b_log[i].rg[1], t2b_log[i].rg[3],
                t2b_log[i].rg[4], (unsigned long long)t2b_log[i].dst, (unsigned long long)t2b_log[i].size);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    kernel_watch_write("CopyTextureToBuffer", dst, total);
    vk_trace_dump(t);
    {
        unsigned k = t2b_next++ & 15;
        t2b_log[k].dst = dst;
        t2b_log[k].size = total;
        t2b_log[k].frame = renderer_present_count();
        t2b_log[k].t = t;
        memcpy(t2b_log[k].rg, rg, sizeof rg);
    }
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (SAVAGEWIND_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    if (getenv("SAVAGEWIND_LOG_COPIES")) {  // (debugging)
        static int n;
        if (n++ < 300)
            LOG("[copy] tex #%llu %ux%ux%u t%u f%#x lv%u (%d,%d,%d %dx%dx%d) view %#x/%u/%u -> #%llu %ux%ux%u t%u f%#x lv%u "
                "(%d,%d,%d %dx%dx%d) view %#x/%u/%u flags %llx\n",
                (unsigned long long)st_->debug_id, st_->width, st_->height, st_->depth, st_->target, st_->format, st_->levels,
                src->region[0], src->region[1], src->region[2], src->region[3], src->region[4], src->region[5],
                src->has_view ? src->view.setmask : 0, src->view.base_level, src->view.min_layer, (unsigned long long)dt->debug_id,
                dt->width, dt->height, dt->depth, dt->target, dt->format, dt->levels, dst->region[0], dst->region[1],
                dst->region[2], dst->region[3], dst->region[4], dst->region[5], dst->has_view ? dst->view.setmask : 0,
                dst->view.base_level, dst->view.min_layer, (unsigned long long)flags);
    }
    {
        NvnTexture *src1 = st_;
        vk_trace_event(dt, 0, &src1, 1, "copy from #%llu %ux%u f%#x region %d,%d %dx%d -> region %d,%d %dx%d flags %llx",
                (unsigned long long)st_->debug_id, st_->width, st_->height, st_->format, src->region[0], src->region[1],
                src->region[3], src->region[4], dst->region[0], dst->region[1], dst->region[3], dst->region[4],
                (unsigned long long)flags);
    }
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs, VkFormat fmt) {
    static struct { const char *fs; VkFormat f; VkPipeline p; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].p && cache[i].fs == fs && cache[i].f == fmt) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 16; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h in format f (kept until the size or format changes). 0, 1: SMAA edges / weights, 2: SMAA
// output, 3, 4: FSR EASU / RCAS of the presented image; 5, 6: FSR EASU / RCAS of the 3D scene (scene_upscale).
enum { POST_IMAGES = 7 };
static TexBackend *post_image(int i, uint32_t w, uint32_t h, VkFormat f) {
    static TexBackend img[POST_IMAGES];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h || d->format != f)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs, dst->format);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vk_dyn_forget();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        const VkFormat f8 = VK_FORMAT_R8G8B8A8_UNORM;
        TexBackend *edges = post_image(0, b->width, b->height, f8), *weights = post_image(1, b->width, b->height, f8);
        TexBackend *out = post_image(2, b->width, b->height, f8);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh, VK_FORMAT_R8G8B8A8_UNORM), *r = post_image(4, ow, oh, VK_FORMAT_R8G8B8A8_UNORM);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// ---- FSR 1 at the scene -> final image composite (Video > Upscaler: AMD FSR 1). The 3D scene's targets are at the
// internal resolution (textures.c RS_SCENE), the final image at the window's (RS_OUT), and the game's composite draw
// would scale the scene up bilinearly. Before that draw, EASU brings the scene to its size in the final image and
// RCAS sharpens it, in the scene's own format (10-bit: no banding); the draw then samples that at 1:1 (texture_info).
// SAVAGEWIND_SCENE_FSR=0: the game's own bilinear composite (A/B).
static void scene_upscale(Prog *pr, CmdRec *r) {
    scene_up.t = NULL;
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_SCENE_FSR") ? atoi(getenv("SAVAGEWIND_SCENE_FSR")) : 1;
    if (!on || g_settings.upscaler != UPSCALE_FSR1) return;
    NvnTexture *rt = st.rt.ncolor ? st.rt.color[0] : NULL;
    if (!rt || r->x[2] > 6 || !pr->fi->ntex || vk_texture_scale_group(rt) != RS_OUT) return;
    const float *vp = st.viewport[0];
    if (vp[2] < rt->width * 0.5f || vp[3] < rt->height * 0.5f) return;  // (a composite fills most of the image)
    float osx, osy;
    renderer_texture_scale(rt, &osx, &osy);
    uint32_t ow = (uint32_t)lroundf(vp[2] * osx), oh = (uint32_t)lroundf(vp[3] * osy);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        // the scene: a full-size scene target (not one of its reduced steps: bloom, blur)
        if (!t || t->target != 1 || vk_texture_scale_group(t) != RS_SCENE || t->width * 2 < vp[2] || t->height * 2 < vp[3]) continue;
        TexBackend *b = vk_tex(t, false);
        if (!b->image || b->layers != 1 || b->samples != 1 || b->depth != 1 || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        if (ow <= b->width + 1 && oh <= b->height + 1) return;  // (at its size in the window already: the upscaler is off)
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, b->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) continue;
        vk_end_pass();
        VkCommandBuffer cb = vk_cmd();
        TexBackend *e = post_image(5, ow, oh, b->format), *o = post_image(6, ow, oh, b->format);
        float iw = (float)b->width, ih = (float)b->height;
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        static const VkComponentMapping id = {0};
        VkImageView in[3] = {vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT)};
        post_pass(cb, "easu.frag", in, 1, e, pe, false);
        float pr_[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri, 1, o, pr_, false);
        vk_full_barrier(cb);
        scene_up.t = t;
        scene_up.view = post_view(o);
        scene_up.sx = (float)ow / (float)t->width;
        scene_up.sy = (float)oh / (float)t->height;
        static uint32_t lw, lh, low, loh;
        if (b->width != lw || b->height != lh || ow != low || oh != loh)
            LOG("[post] scene %ux%u -> fsr1 %ux%u (format %d)\n", b->width, b->height, ow, oh, b->format);
        lw = b->width; lh = b->height; low = ow; loh = oh;
        return;
    }
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vk_dyn_forget();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

static void save_view_png(TexBackend *b, VkImageView src, const char *path);

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    save_view_png(b, raw_view(b), path);
}

// (debug frames) each channel of a colour texture's level 0 as grey, <path> minus .png + _<channel>.png, and
// _L<layer> before that for the layers of an array (up to 8).
static void debug_save_channels(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->base_type == VK_IMAGE_VIEW_TYPE_3D) return;
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    uint32_t ly0 = 0, ly1 = 7;  // SAVAGEWIND_DEBUG_TEXDUMP_LAYERS=<first>-<last> (default 0-7)
    uint32_t lv = getenv("SAVAGEWIND_DEBUG_TEXDUMP_LEVEL") ? (uint32_t)atoi(getenv("SAVAGEWIND_DEBUG_TEXDUMP_LEVEL")) : 0;  // (stretched)
    if (getenv("SAVAGEWIND_DEBUG_TEXDUMP_LAYERS")) sscanf(getenv("SAVAGEWIND_DEBUG_TEXDUMP_LAYERS"), "%u-%u", &ly0, &ly1);
    for (uint32_t ly = ly0; ly < b->layers && ly <= ly1; ly++)
        for (int ch = 0; ch < 4; ch++) {
            VkComponentSwizzle c = (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + ch);
            VkComponentMapping m = {c, c, c, VK_COMPONENT_SWIZZLE_ONE};
            char p2[300], lay[16] = "";
            if (b->layers > 1) snprintf(lay, sizeof lay, "_L%u", ly);
            snprintf(p2, sizeof p2, "%.*s%s_%c.png", (int)strlen(path) - 4, path, lay, "rgba"[ch]);
            save_view_png(b, vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, lv < b->levels ? lv : 0, 1, ly, 1, m, VK_IMAGE_ASPECT_COLOR_BIT), p2);
        }
}

static void save_view_png(TexBackend *b, VkImageView src, const char *path) {
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vk_dyn_forget();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        // (the records were written by another core: the first touch of each one missed the cache)
        __builtin_prefetch((const char *)r + 256);
        __builtin_prefetch((const char *)r + 320);
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (g_debug_frame && getenv("SAVAGEWIND_LOG_OPS"))
            LOG("[op] %u depth %d: %llx %llx %llx %llx %llx\n", r->op, depth, (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                (unsigned long long)r->x[2], (unsigned long long)r->x[3], (unsigned long long)r->x[4]);
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            if (g_debug_frame && getenv("SAVAGEWIND_LOG_OPS"))
                for (uint64_t i = 0; i < r->x[0] && i < 8; i++) LOG("[call] depth %d -> %llx\n", depth, (unsigned long long)h[i]);
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            if (g_debug_frame && getenv("SAVAGEWIND_LOG_BINDS")) {
                LOG("[bind] ubuf stage %llu slots %llu+%llu:", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                    (unsigned long long)r->x[2]);
                for (uint64_t i = 0; i < r->x[2] && i < 18; i++) {
                    const uint64_t *rg = (const uint64_t *)((uint8_t *)x + i * 16);
                    LOG(" %llx+%llu", (unsigned long long)rg[0], (unsigned long long)rg[1]);
                }
                LOG("\n");
            }
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            if (g_debug_frame && getenv("SAVAGEWIND_LOG_BINDS")) {
                LOG("[bind] stage %llu slots %llu+%llu:", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                    (unsigned long long)r->x[2]);
                for (uint64_t i = 0; i < r->x[2] && i < 32; i++) {
                    uint64_t h;
                    memcpy(&h, (uint8_t *)x + i * 8, 8);
                    uint32_t tic = h & 0xFFFFF;
                    NvnTexture *t = h && st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
                    if (t) LOG(" %ux%u/t%u/f%#x", t->width, t->height, t->target, t->format);
                    else LOG(" %llx", (unsigned long long)h);
                }
                LOG("\n");
            }
            break;
        case CMD_BIND_TEXTURES_DEFERRED:  // (stage, first, count, handles): the handles as they are now
            if (r->x[3] >= GUEST_LO && r->x[3] + r->x[2] * 8 <= GUEST_HI)
                for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                    memcpy(&st.tex[r->x[0]][r->x[1] + i], (const uint8_t *)(uintptr_t)r->x[3] + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("SAVAGEWIND_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            if (nfbrun && memcmp(&prev, &st.rt, sizeof prev)) fbdump_flush();
            if (logfb > 0 && !fbskip && memcmp(&prev, &st.rt, sizeof prev))
                LOG("[feedback] targets %d (%ux%u fmt %#x) depth %d after %u draws\n", st.rt.ncolor, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0,
                    st.rt.depth != NULL, fb_draws);
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_LINE_WIDTH: st.line_width = r->f[0]; break;
        case CMD_SET_DEPTH_CLAMP: {
            // BOTW draws its sun shadow cascades (and its sun occlusion query) with depth clamp: casters nearer the
            // light than a cascade's near plane (the cliff over the Shrine of Resurrection, tree tops above the view)
            // are flattened onto it, not clipped. Clipped, the sun lit cave interiors, and what got clipped changed
            // with the cascades as the camera turned (tree shadows). Pipeline state (PipeState.depth_clamp).
            static int off = -1;  // SAVAGEWIND_NO_DEPTH_CLAMP=1: ignored (A/B tests)
            if (off < 0) off = getenv("SAVAGEWIND_NO_DEPTH_CLAMP") != NULL;
            static bool logged;
            if (r->x[0] && !logged) { logged = true; LOG("[nvn] SetDepthClamp on (depth clamp %s)\n", g_vk.depth_clamp ? "supported" : "not supported"); }
            st.depth_clamp = r->x[0] != 0 && !off;
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("SAVAGEWIND_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            if (g_debug_frame && getenv("SAVAGEWIND_LOG_BINDS"))
                LOG("[bind] update ubuf %llx (size %llu) +%llu: %llu bytes\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                    (unsigned long long)r->x[2], (unsigned long long)r->x[3]);
            kernel_watch_write("UpdateUniformBuffer", r->x[0] + r->x[2], r->x[3]);
            stat_uub++;
            // The same bytes again (~1 update in 5 in the open world): the copies over the range stay good.
            if (!memcmp((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3])) { stat_uub_same++; break; }
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            uub_note(r->x[0] + r->x[2], r->x[0] + r->x[2] + r->x[3]);  // (the shared copies over it are stale now)
            break;
        case CMD_BARRIER:
            if (logfb > 0 && !fbskip) LOG("[feedback] barrier %#llx after %u draws\n", (unsigned long long)r->x[0], fb_draws);
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("SAVAGEWIND_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and picks its 3D resolution from that (docked:
            // 1600x900 down to 1280x720, a scene target of its own per size). Timed here they measured our command
            // translation, not GPU work, and the open world dropped to 1280x720 times the internal-resolution
            // scale. A PC GPU does that work in a fraction of the Switch's time: every counter of a presented frame
            // gets the same timestamp (no GPU time), so the game keeps 1600x900. SAVAGEWIND_VK_COUNTER=live: the
            // time of each counter (as before).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            if (g_debug_frame && type != 0 && type != 16)
                LOG("[counter] type %u -> %llx after draw %llu (fs hash %016llx, rt %d %ux%u)\n", type, (unsigned long long)r->x[1],
                    (unsigned long long)stat_draws, (unsigned long long)g_cur_fs_hash, st.rt.ncolor,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : st.rt.depth ? st.rt.depth->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : st.rt.depth ? st.rt.depth->height : 0);
            static int frozen = -1;
            if (frozen < 0) { const char *e = getenv("SAVAGEWIND_VK_COUNTER"); frozen = !e || strcmp(e, "live"); }
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (frozen && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); }
            kernel_watch_write("ReportCounter", r->x[1], 16);
            if (out) { out[0] = 0; out[1] = frozen ? frame_ts : SDL_GetTicksNS(); }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            kernel_watch_write("CopyBufferToBuffer", r->x[1], r->x[2]);
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            guest_copy_flush();
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    readbacks_apply();
    gpu_lock();
    if (g_debug_frame && getenv("SAVAGEWIND_LOG_OPS")) LOG("[submit] %llx\n", (unsigned long long)handle);
    PROF_T0();
    in_execute = true;
    guest_copy_flush();  // (a new list: the game may have written the memory of the last one's copies since)
    exec_list(handle, 0);
    in_execute = false;
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("SAVAGEWIND_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
    gpu_unlock();
}
