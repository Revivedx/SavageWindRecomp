// NVN objects, builders, and state objects (everything except command recording).
#include <stdlib.h>
#include <string.h>

#include "../framepace.h"
#include "../game_sites.h"
#include "../kernel.h"
#include "../perfmon.h"
#include "nvn.h"

void renderer_sync(void);

static uint64_t next_debug_id = 1;

// ================================================================== device
// NVNdeviceInfo -> value, as the NVN 55.15 driver in BOTW 1.8.2's sdk answers (nvnDeviceGetInteger: a constant
// table for most, then the API version and a few per-device values on top). BOTW creates its graphics system
// only when the device reports 55.15 or later (main 0x7100d802e0). The per-device entries (scratch memory
// factors, L2 size, texture limits, memory pool page size) are what a Tegra X1 reports; the rest is the table.
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 55, [1] = 15,
        [2] = 14, [3] = 0x10000, [4] = 0x100, [5] = 8, [6] = 0x10, [7] = 4, [8] = 0x10, [9] = 0x20,
        [10] = 0x10, [11] = 4, [12] = 4, [13] = 4, [14] = 0x10, [15] = 0x20, [16] = 0x20,
        [17] = 0x100, [18] = 0x100, [19] = 4, [20] = 8, [21] = 0x200, [22] = 0x100,
        [23] = 256, [24] = 256,                    // scratch memory scale factors (per device)
        [25] = 0x1000, [26] = 0x20000, [27] = 0x10, [28] = 0x600, [29] = 0x400, [30] = 0x40, [31] = 0x600,
        [32] = 0xffff, [33] = 0xffff, [34] = 0xffff, [35] = 8, [36] = 0x100000, [37] = 0x1000, [38] = 0x10,
        [39] = 0x1000, [40] = 1, [41] = 1, [42] = 1,
        [43] = 0x40000,                            // L2 size (per device)
        [44] = 0xf, [45] = 0x800, [46] = 1, [47] = 1, [48] = 0x10, [49] = 5, [50] = 1, [51] = 8, [52] = 8,
        [53] = 4, [54] = 0x20,
        [55] = 256, [56] = 0x20,                   // (per device)
        [57] = 0x80,
        [58] = 0x1000, [59] = 1,                   // memory pool page size, zero-filled unmapped pages (per device)
        [60] = 4,
        [61] = 16384, [62] = 0x8000000, [63] = 2048, [64] = 16384, [65] = 16384,  // texture limits (per device)
        [66] = 1, [67] = 1, [68] = 1, [69] = 1, [70] = 5, [71] = 1, [72] = 1, [73] = 1, [74] = 1, [75] = 1,
        [76] = 1, [77] = 0x10, [78] = 0x400, [79] = 0x20, [80] = 0x1000, [81] = 0x10000, [82] = 0x10000,
        [83] = 0x100, [84] = 0x4000, [85] = 0x40000, [86] = 0x1000, [87] = 1, [88] = 4, [89] = 2, [90] = 0,
        [91] = 0x1000, [92] = 0x4000, [93] = 0x1000, [94] = 0x80, [95] = 0x20, [96] = 0xffff, [97] = 1,
        [98] = 0x20,                               // (per device)
        [99] = 0x100, [100] = 0x10, [101] = 0xffff, [102] = 0xffff, [103] = 0x100, [104] = 0x3fff,
    };
    if (which < 128) {  // SAVAGEWIND_NVN_LOG: which device integers the game reads
        static uint8_t seen[128];
        static int log = -1;
        if (log < 0) log = getenv("SAVAGEWIND_NVN_LOG") != NULL;
        if (log && !seen[which]) { seen[which] = 1; LOG("[nvn] device integer %u\n", which); }
    }
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) { *(int32_t *)AP(2) = (int32_t)device_integer((uint32_t)A(1)); }
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) { RET(G(uint64_t, A(1), 8)); }  // counter.timestamp (ticks == ns here)
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
// The backend's work (submits, presents, GPU waits) lets the thread's core go (kernel_host_work_begin).
// SAVAGEWIND_HOST_WORK_CORE=1: keep holding it (as before).
static bool host_work(void) {
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_HOST_WORK_CORE");
    return on;
}
// SAVAGEWIND_TIMELINE=<first frame>:<frames>: "[tl]" lines (ms, guest thread, event) for submits, fences, GPU
// waits and presents of those presented frames: who waits for whom within a frame.
uint64_t renderer_present_count(void);
uint64_t vk_completed_seq(void);
uint64_t renderer_fence_seq(void);
bool nvn_timeline(void) {
    static int64_t from = -2, count;
    if (from == -2) {
        const char *e = getenv("SAVAGEWIND_TIMELINE");
        from = -1;
        if (e) { from = atoll(e); const char *c = strchr(e, ':'); count = c ? atoll(c + 1) : 60; }
    }
    if (from < 0) return false;
    uint64_t f = renderer_present_count();
    return f >= (uint64_t)from && f < (uint64_t)(from + count);
}
#define TL(fmt, ...) do { if (nvn_timeline()) LOG("[tl] %.3f %x " fmt "\n", plat_now_ns() / 1e6, kernel_current_thread_handle(), ##__VA_ARGS__); } while (0)

// ---- the submission queue
// On the console a submit hands the GPU its commands and returns; the GPU runs them while the CPU goes on with the next
// frame. Here the commands are translated to Vulkan on the CPU first (~10 ms a frame in the open world), and BOTW 1.8.2
// submits the frame's command list on its render thread (main 0x7000d81158), presents, and only then lets the main
// thread record the next frame: the translation sat on every frame's critical path. So submits, queue fences, presents
// and texture / sync finalizes go, in their order, to one host thread that runs them, and the guest goes on at once. It
// waits where it would wait for the GPU on the console (nvnSyncWait, nvnQueueFinish): for the queue to get past the
// fence, then for the GPU. The commands read guest memory when they run, as the GPU reads it when it gets to them: the
// game fences its CPU writes to memory in use (as it must on the console).
// At most QUEUE_AHEAD presents wait in the queue (SAVAGEWIND_QUEUE_AHEAD, default 1): a guest further ahead waits.
// SAVAGEWIND_ASYNC_EXEC=0: everything runs on the calling thread (as before).
enum { QOP_EXEC, QOP_FENCE, QOP_PRESENT, QOP_TEXTURE_FINALIZE, QOP_SYNC_FREE };
typedef struct { int op; uint64_t a; void *p; } QOp;
#define QCAP 4096
static QOp g_q[QCAP];
static uint64_t g_q_done, g_q_put;  // operations run / queued (operation k, from 1, is done once g_q_done >= k)
static int g_q_presents, g_q_ahead = 1;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_work = PTHREAD_COND_INITIALIZER, g_q_progress = PTHREAD_COND_INITIALIZER;

static bool async_exec(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_ASYNC_EXEC");
        on = !e || atoi(e) != 0;
        if (getenv("SAVAGEWIND_QUEUE_AHEAD")) g_q_ahead = atoi(getenv("SAVAGEWIND_QUEUE_AHEAD")) > 0 ? atoi(getenv("SAVAGEWIND_QUEUE_AHEAD")) : 1;
        LOG("[nvn] command lists run %s\n", on ? "on the submission thread" : "on the submitting thread");
    }
    return on;
}

// SAVAGEWIND_FRAME_LOG=2: an "[fsplit]" line per present with when each step of the frame happened, in ms before the
// present ends: the main thread's VFR update and its GPU wait (nvnSyncWait), the submit, the list's translation, the
// present's turn (cheap enough to leave the timing alone, unlike SAVAGEWIND_TIMELINE)
uint64_t g_fs_vfr, g_fs_sync0, g_fs_sync1, g_fs_submit, g_fs_x0, g_fs_x1, g_fs_pace;
static int frame_split(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FRAME_LOG") && atoi(getenv("SAVAGEWIND_FRAME_LOG")) == 2;
    return on;
}
static void frame_split_log(void) {
    uint64_t now = plat_now_ns();
    #define FS(t) ((t) ? ((double)now - (double)(t)) / 1e6 : -1.0)
    LOG("[fsplit] %.3f vfr %.1f sync %.1f-%.1f submit %.1f exec %.1f-%.1f pace %.1f\n", now / 1e6, FS(g_fs_vfr),
        FS(g_fs_sync0), FS(g_fs_sync1), FS(g_fs_submit), FS(g_fs_x0), FS(g_fs_x1), FS(g_fs_pace));
    #undef FS
}

static void qop_run(const QOp *o) {
    switch (o->op) {
    case QOP_EXEC:
        if (frame_split()) g_fs_x0 = plat_now_ns();
        backend_execute(o->a);
        if (frame_split()) g_fs_x1 = plat_now_ns();
        break;
    case QOP_FENCE: { NvnSync *s = o->p; s->seq = renderer_fence_seq(); s->signaled = 1; break; }
    case QOP_PRESENT:
        backend_present(o->p);
        if (frame_split()) frame_split_log();
        break;
    case QOP_TEXTURE_FINALIZE: backend_texture_destroyed(o->p); ((NvnTexture *)o->p)->finalized = true; break;
    case QOP_SYNC_FREE: free(o->p); break;
    }
}

static void *queue_thread(void *arg) {
    (void)arg;
    kernel_sample_host_thread();
    pthread_mutex_lock(&g_q_lock);
    for (;;) {
        while (g_q_done == g_q_put) pthread_cond_wait(&g_q_work, &g_q_lock);
        QOp o = g_q[g_q_done % QCAP];
        pthread_mutex_unlock(&g_q_lock);
        if (o.op == QOP_EXEC) TL("queue: exec %llx", (unsigned long long)o.a);
        qop_run(&o);
        if (o.op == QOP_EXEC || o.op == QOP_PRESENT) TL("queue: %s done", o.op == QOP_EXEC ? "exec" : "present");
        pthread_mutex_lock(&g_q_lock);
        g_q_done++;
        if (o.op == QOP_PRESENT) g_q_presents--;
        pthread_cond_broadcast(&g_q_progress);
    }
    return NULL;
}

// (g_q_lock held; returns with it held) until done(): the guest thread's core let go meanwhile
static void queue_block(bool (*done)(uint64_t), uint64_t arg) {
    while (!done(arg)) {
        pthread_mutex_unlock(&g_q_lock);
        kernel_host_work_begin();
        pthread_mutex_lock(&g_q_lock);
        while (!done(arg)) pthread_cond_wait(&g_q_progress, &g_q_lock);
        pthread_mutex_unlock(&g_q_lock);
        kernel_host_work_end();  // (taking the core back without the queue's lock: its holder may be queueing)
        pthread_mutex_lock(&g_q_lock);
    }
}
static bool q_reached(uint64_t k) { return g_q_done >= k; }
static bool q_room(uint64_t present) { return g_q_put - g_q_done < QCAP && (!present || g_q_presents < g_q_ahead); }

static void queue_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, queue_thread, NULL);
}
static uint64_t queue_put(int op, uint64_t a, void *p) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, queue_start);
    pthread_mutex_lock(&g_q_lock);
    queue_block(q_room, op == QOP_PRESENT);
    g_q[g_q_put % QCAP] = (QOp){op, a, p};
    uint64_t k = ++g_q_put;
    if (op == QOP_PRESENT) g_q_presents++;
    pthread_cond_signal(&g_q_work);
    pthread_mutex_unlock(&g_q_lock);
    return k;
}

// until the queue has run operation k (0: nothing to wait for; ~0: everything queued so far)
static void queue_wait(uint64_t k) {
    if (!k) return;
    pthread_mutex_lock(&g_q_lock);
    if (k == ~0ull) k = g_q_put;
    queue_block(q_reached, k);
    pthread_mutex_unlock(&g_q_lock);
}
void nvn_queue_idle(void) { if (async_exec()) queue_wait(~0ull); }  // (runtime/main.c reboot: guest threads stopped)

NVN(nvnQueueSubmitCommands) {
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    TL("submit %d lists (lr %llx)", n, (unsigned long long)c->x[30]);
    if (frame_split()) g_fs_submit = plat_now_ns();
    if (async_exec()) {
        for (int i = 0; i < n; i++) queue_put(QOP_EXEC, handles[i], NULL);
        return;
    }
    if (host_work()) kernel_host_work_begin();
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
    if (host_work()) kernel_host_work_end();
    TL("submitted (seq %llu)", (unsigned long long)renderer_fence_seq());
}
NVN(nvnQueueFlush) { (void)c; }
NVN(nvnQueueFinish) {
    (void)c;
    if (async_exec()) queue_wait(~0ull);
    if (host_work()) kernel_host_work_begin();
    renderer_sync();
    if (host_work()) kernel_host_work_end();
}

static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnSync *sync_of(uint64_t guest) {  // (the submission thread creates one too: a list fencing a sync never initialized)
    NvnSync *s = HOST(NvnSync, guest);
    if (s) return s;
    pthread_mutex_lock(&g_sync_lock);
    s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    pthread_mutex_unlock(&g_sync_lock);
    return s;
}
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    if (async_exec()) {
        s->ticket = queue_put(QOP_FENCE, 0, s);
        TL("fence %llx queued as %llu (lr %llx)", (unsigned long long)A(1), (unsigned long long)s->ticket, (unsigned long long)c->x[30]);
        return;
    }
    s->signaled = 1; s->seq = renderer_fence_seq();
    TL("fence %llx = seq %llu (lr %llx)", (unsigned long long)A(1), (unsigned long long)s->seq, (unsigned long long)c->x[30]);
}
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) { NvnSync *s = sync_of(guest); s->signaled = 1; s->seq = seq; }
void nvn_sync_in_list(uint64_t guest) { if (guest) sync_of(guest)->in_list = 1; }  // (cmdbuf.c: recording a list fence)
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) {
    NvnSync *s = HOST(NvnSync, A(0));
    SET_HOST(A(0), 0);
    if (s && async_exec()) queue_put(QOP_SYNC_FREE, 0, s);  // (a queued fence may still set it)
    else free(s);
}
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = getenv("SAVAGEWIND_SYNC_FLUSH") ? 1 : 0;
    if (getenv("SAVAGEWIND_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // SAVAGEWIND_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = getenv("SAVAGEWIND_SYNC_ALL") ? 1 : 0;
    NvnSync *s = sync_of(A(0));
    TL("syncwait %llx seq %llu ticket %llu (fence seq now %llu, done %llu, lr %llx)", (unsigned long long)A(0),
       (unsigned long long)s->seq, (unsigned long long)s->ticket, (unsigned long long)renderer_fence_seq(),
       (unsigned long long)vk_completed_seq(), (unsigned long long)c->x[30]);
    // the queue first: up to the fence that sets it (a fence inside a command list: everything submitted so far)
    if (frame_split() && s->ticket) g_fs_sync0 = plat_now_ns();
    if (async_exec()) queue_wait(flush || whole || s->in_list ? ~0ull : s->ticket);
    if (host_work()) kernel_host_work_begin();
    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else renderer_wait_seq(s->seq);  // only the submission that holds this fence
    if (host_work()) kernel_host_work_end();
    if (frame_split() && s->ticket) g_fs_sync1 = plat_now_ns();
    TL("syncwait done");
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(*(uint32_t *)(uintptr_t)event_addr(A(0))); }
NVN(nvnEventSignal) { *(uint32_t *)(uintptr_t)event_addr(A(0)) = (uint32_t)A(2); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    SET_HOST(A(0), p);
    RET(1);
}
NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
    if (p) {
        nvn_cmdbuf_pool_finalize(p->storage, p->size);
        free(p);
    }
    SET_HOST(A(0), 0);
}
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetBufferAddress) { RET(HOST(NvnPool, A(0))->storage); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
// (pool, count, const NVNmappingRequest[]: {physical pool, physical offset, virtual offset, size, storage class}).
// GPU addresses are CPU addresses here, so a virtual pool can only stand for memory that already exists: when the
// requests map it onto one physical pool at a single displacement (BOTW 1.8.2 maps its 0x7300000-byte texture pool
// onto a physical pool of that size, whole, once), the virtual pool's storage becomes that physical memory.
// Other mappings (pages scattered over pools) would need aliased memory: logged, left unmapped.
NVN(nvnMemoryPoolMapVirtual) {
    NvnPool *vp = HOST(NvnPool, A(0));
    const uint8_t *rq = AP(2);
    int n = (int)A(1);
    uint64_t base = 0;
    bool alias = n > 0;
    for (int i = 0; i < n && alias; i++) {
        const uint64_t *q = (const uint64_t *)(rq + i * 40);
        NvnPool *pp = q[0] ? HOST(NvnPool, q[0]) : NULL;
        if (!pp || !pp->storage) { alias = false; break; }  // (unmapping, or a physical pool without memory)
        uint64_t b = pp->storage + q[1] - q[2];
        if (i == 0) base = b;
        else if (b != base) alias = false;
    }
    if (alias) vp->storage = base;
    static int logged;
    if (!alias && logged++ < 8)
        LOG("[nvn] MapVirtual pool %llx: %d requests not one contiguous alias (not mapped)\n", (unsigned long long)A(0), n);
    RET(1);
}

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures (BNTX) were swizzled offline with their own block height, which can differ
    // from the driver default: NVNpackagedTextureLayout's low word is log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
    if (pl && getenv("SAVAGEWIND_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %016llx fmt %#x %ux%u lv %u -> default bh %u\n", pl, G(uint32_t, b, 0x20), t->width, t->height, t->levels, t->block_height_log2);
    }
}

static bool is_linear(uint64_t b) { return nvn_texture_flags_pitch(G(uint32_t, b, 8)) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10) {  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    }
    if (is_linear(b)) return G(uint64_t, b, 0x30) * G(uint32_t, b, 0x14);
    TexLayout t = {0};
    builder_layout(b, &t);
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? 32 : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = t->pool ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = next_debug_id++;
    return t;
}

// Every host texture of this boot (they are only freed by a reboot: see nvnTextureFinalize), so a guest NVNtexture
// whose first word is not one of them (never initialized, or not a texture) is told apart.
static uintptr_t *g_tex_set;
static uint32_t g_tex_cap, g_tex_n;
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t tex_slot(uintptr_t p) { return (uint32_t)((p >> 4) * 0x9E3779B97F4A7C15ull >> 32) & (g_tex_cap - 1); }
static void tex_set_put(uintptr_t p) {  // (lock held)
    if ((g_tex_n + 1) * 2 > g_tex_cap) {
        uintptr_t *old = g_tex_set;
        uint32_t oc = g_tex_cap;
        g_tex_cap = g_tex_cap ? g_tex_cap * 2 : 4096;
        g_tex_set = calloc(g_tex_cap, sizeof *g_tex_set);
        g_tex_n = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i]) tex_set_put(old[i]);
        free(old);
    }
    uint32_t i = tex_slot(p);
    while (g_tex_set[i]) i = (i + 1) & (g_tex_cap - 1);
    g_tex_set[i] = p;
    g_tex_n++;
}
static bool tex_set_has(uintptr_t p) {  // (lock held)
    if (!g_tex_cap) return false;
    for (uint32_t i = tex_slot(p); g_tex_set[i]; i = (i + 1) & (g_tex_cap - 1))
        if (g_tex_set[i] == p) return true;
    return false;
}

// (debugging) log the textures whose storage holds guest address a
void nvn_debug_textures_at(uint64_t a) {
    pthread_mutex_lock(&g_tex_lock);
    for (uint32_t i = 0; i < g_tex_cap; i++) {
        NvnTexture *t = (NvnTexture *)g_tex_set[i];
        if (!t || !t->addr) continue;
        uint64_t size = tegra_total_size(&t->layout);
        if (t->stride * t->height > size) size = t->stride * t->height;
        if (a >= t->addr && a < t->addr + size)
            LOG("[debug]   texture #%llu '%s' %ux%u format %#x flags %#x stride %llu at %llx (+%llx)%s\n",
                (unsigned long long)t->debug_id, t->label, t->width, t->height, t->format, t->flags,
                (unsigned long long)t->stride, (unsigned long long)t->addr, (unsigned long long)(a - t->addr),
                t->finalized ? " finalized" : "");
    }
    pthread_mutex_unlock(&g_tex_lock);
}

RT_ON_RESET(tex_set_reset) {  // (after the renderer's reset: no image is left)
    for (uint32_t i = 0; i < g_tex_cap; i++) free((void *)g_tex_set[i]);
    free(g_tex_set);
    g_tex_set = NULL;
    g_tex_cap = g_tex_n = 0;
}

NVN(nvnTextureInitialize) {
    NvnTexture *t = texture_from_builder(A(1));
    if (t->flags & 2)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", t, t->width, t->height,
            t->format, t->flags, t->target, t->addr, t->stride);
    pthread_mutex_lock(&g_tex_lock);
    tex_set_put((uintptr_t)t);
    pthread_mutex_unlock(&g_tex_lock);
    SET_HOST(A(0), t);
    RET(1);
}
// The host texture a guest NVNtexture holds now (finalized ones too), or NULL (never initialized, or not a texture).
// Commands take it when they are recorded: on hardware a command buffer holds the texture's address and shape as they
// were then. BOTW initializes one NVNtexture object again for each step of a chain (exposure: 192x192 down to 3x3)
// within one command buffer; resolved when executed, every step drew into the last one.
NvnTexture *nvn_texture_host(uint64_t guest) {
    if (guest < GUEST_LO || guest + 8 > GUEST_HI) return NULL;
    uintptr_t h = (uintptr_t)*(uint64_t *)(uintptr_t)guest;
    if (!h) return NULL;
    pthread_mutex_lock(&g_tex_lock);
    bool known = tex_set_has(h);
    pthread_mutex_unlock(&g_tex_lock);
    return known ? (NvnTexture *)h : NULL;
}

// Only the renderer's image goes; the host object stays, and so does the guest object's pointer to it. BOTW copies
// NVNtexture objects and finalizes one copy while it keeps drawing with another, and keeps texture pool entries
// registered with finalized textures: on hardware those just go on reading the texture's memory. (A texture
// initialized again in the same object gets a new host object.)
NVN(nvnTextureFinalize) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (!t) return;
    pthread_mutex_lock(&g_tex_lock);
    bool known = tex_set_has((uintptr_t)t);
    pthread_mutex_unlock(&g_tex_lock);
    if (!known) {
        static int logged;
        if (logged++ < 8) LOG("[nvn] TextureFinalize(%llx): %llx is not a texture (lr %llx)\n",
                              (unsigned long long)A(0), (unsigned long long)(uintptr_t)t, (unsigned long long)c->x[30]);
        return;
    }
    if (async_exec()) { queue_put(QOP_TEXTURE_FINALIZE, 0, t); return; }  // (after the lists submitted before it)
    backend_texture_destroyed(t);
    t->finalized = true;
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TEX->swizzle[i]; }
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(0); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) {
    NvnTexture *t = TEX;
    RET(nvn_texture_flags_pitch(t->flags) && t->stride ? t->stride * t->height : tegra_total_size(&t->layout));
}
NVN(nvnTextureGetDebugID) { RET(TEX->debug_id); }
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }
// Whether two textures are the same texture for the hardware (shape, format, swizzle, storage): NVN compares the
// objects' state, not their addresses. agl builds a texture's NVNtexture again whenever its data is set and keeps
// the texture pool slot when the new one compares equal (its slot holder, 0x7000ec33f0 in 1.9.0). BOTW's cuttable
// trees take that slot id once for the trunk material (0x7000f75dcc) and set the data again later on another
// thread; comparing addresses gave each rebuild a new slot and freed the old one, which the trunks went on binding
// after the allocator gave it to other textures (#16: an exposure step as the cut profile threw trees across the sky).
NVN(nvnTextureCompare) {
    const NvnTexture *a = nvn_texture_host(A(0)), *b = nvn_texture_host(A(1));
    if (a == b) { RET(a || A(0) == A(1)); return; }  // (copies of one texture; or two objects never initialized)
    if (!a || !b) { RET(0); return; }
    RET(a->target == b->target && a->format == b->format && a->flags == b->flags && a->width == b->width &&
        a->height == b->height && a->depth == b->depth && a->levels == b->levels && a->samples == b->samples &&
        a->ds_mode == b->ds_mode && !memcmp(a->swizzle, b->swizzle, sizeof a->swizzle) && a->stride == b->stride &&
        a->addr == b->addr && a->pool == b->pool && a->pool_offset == b->pool_offset &&
        a->layout.layers == b->layout.layers && a->layout.block_height_log2 == b->layout.block_height_log2 &&
        a->layout.block_depth_log2 == b->layout.block_depth_log2);
}
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views: an NvnTextureView (nvn.h) packed into the guest's 16-byte NVNtextureView. Clearing NVN 53's 0x28
// bytes here zeroed the next sead heap block's list link in BOTW 1.8.2, and its free list then looped.
#define TV ((NvnTextureView *)AP(0))
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, sizeof(NvnTextureView)); }
NVN(nvnTextureViewSetLevels) { TV->setmask |= 1; TV->base_level = (uint8_t)A(1); TV->num_levels = (uint8_t)A(2); }
NVN(nvnTextureViewSetLayers) { TV->setmask |= 2; TV->min_layer = (uint16_t)A(1); TV->num_layers = (uint16_t)A(2); }
NVN(nvnTextureViewSetFormat) { TV->setmask |= 4; TV->format = (uint16_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    TV->setmask |= 8;
    for (int i = 0; i < 4; i++) TV->swizzle[i] = (uint8_t)A(1 + i);
}
NVN(nvnTextureViewSetDepthStencilMode) { TV->setmask |= 16; TV->ds_mode = (uint8_t)A(1); }
NVN(nvnTextureViewSetTarget) { TV->setmask |= 32; TV->target = (uint8_t)A(1); }
NVN(nvnTextureViewGetLevels) { *(int32_t *)AP(1) = TV->base_level; *(int32_t *)AP(2) = TV->num_levels; RET(TV->setmask & 1); }
NVN(nvnTextureViewGetLayers) { *(int32_t *)AP(1) = TV->min_layer; *(int32_t *)AP(2) = TV->num_layers; RET(!!(TV->setmask & 2)); }
NVN(nvnTextureViewGetFormat) { *(uint32_t *)AP(1) = TV->format; RET(!!(TV->setmask & 4)); }
NVN(nvnTextureViewGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TV->swizzle[i]; RET(!!(TV->setmask & 8)); }
NVN(nvnTextureViewGetDepthStencilMode) { *(uint32_t *)AP(1) = TV->ds_mode; RET(!!(TV->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { *(uint32_t *)AP(1) = TV->target; RET(!!(TV->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), sizeof(NvnTextureView)) == 0); }

NVN(nvnTextureWriteTexels) {
    // (texture, view, region, data): tightly packed linear texels into the storage (block-linear, or rows at the stride
    // for pitch textures) of one whole level (the view's base level), for the region's slices (3D) or layers (arrays:
    // z, plus the view's first layer; 1D arrays: y).
    // The renderer uploads the written levels at the texture's next use, even when the GPU has drawn into the texture
    // since: BOTW writes level 0 of its 3D noise this way and renders the other levels from it.
    NvnTexture *t = HOST(NvnTexture, A(0));
    const NvnTextureView *v = A(1) ? (const NvnTextureView *)AP(1) : NULL;
    const int32_t *r = AP(2);  // xoffset, yoffset, zoffset, width, height, depth
    if (!t || !r || !t->addr || !t->layout.fmt) return;
    if (getenv("SAVAGEWIND_LOG_COPIES")) {  // (debugging)
        static int n;
        if (n++ < 200)
            LOG("[copy] WriteTexels #%llu %ux%ux%u target %u f%#x region %d,%d,%d %dx%dx%d view set %#x level %u layer %u\n",
                (unsigned long long)t->debug_id, t->width, t->height, t->depth, t->target, t->format, r[0], r[1], r[2], r[3],
                r[4], r[5], v ? v->setmask : 0, v ? v->base_level : 0, v ? v->min_layer : 0);
    }
    uint32_t level = v && (v->setmask & 1) ? v->base_level : 0;
    if (level >= t->layout.levels) return;
    LevelLayout l = tegra_level(&t->layout, level);
    bool is3d = t->target == 2, array1d = t->target == 3;
    uint32_t y0 = array1d ? 0 : (uint32_t)r[1], h = array1d ? 1 : (uint32_t)r[4];
    uint32_t z0 = array1d ? (uint32_t)r[1] : (uint32_t)r[2];
    uint32_t nz = array1d ? (uint32_t)(r[4] > 0 ? r[4] : 1) : (uint32_t)(r[5] > 0 ? r[5] : 1);
    if (!is3d && v && (v->setmask & 2)) z0 += v->min_layer;
    uint32_t nslices = is3d ? l.d : (t->layout.layers ? t->layout.layers : 1);
    if (r[0] || y0 || (uint32_t)r[3] != l.w || h != l.h || (is3d && (z0 || nz != l.d)) || z0 + nz > nslices) {
        static int n;
        if (n++ < 8) LOG("[nvn] partial WriteTexels not implemented (#%llu %ux%u level %u, region %d,%d,%d %dx%dx%d)\n",
                         (unsigned long long)t->debug_id, t->width, t->height, level, r[0], r[1], r[2], r[3], r[4], r[5]);
        return;
    }
    uint32_t bpb = t->layout.fmt->bpb;
    uint8_t *base = (uint8_t *)(uintptr_t)t->addr;
    if (nvn_texture_flags_pitch(t->flags)) {  // pitch storage: rows at the stride (one level, one layer)
        uint64_t row = (uint64_t)l.wb * bpb, stride = t->stride ? t->stride : row;
        kernel_watch_write("WriteTexels", t->addr, stride * l.hb);
        for (uint32_t y = 0; y < l.hb; y++) memcpy(base + y * stride, (const uint8_t *)AP(3) + y * row, row);
    } else if (is3d) {
        kernel_watch_write("WriteTexels", t->addr + l.offset, tegra_total_size(&t->layout) - l.offset);
        tegra_swizzle(AP(3), base + l.offset, l.wb, l.hb, l.d, bpb, l.block_height_log2, l.block_depth_log2);
    } else {
        uint64_t layer_size = tegra_layer_size(&t->layout), slice = (uint64_t)l.wb * l.hb * bpb;
        for (uint32_t i = 0; i < nz; i++) {
            uint64_t dst = (uint64_t)(z0 + i) * layer_size + l.offset;
            kernel_watch_write("WriteTexels", t->addr + dst, slice);
            tegra_swizzle((const uint8_t *)AP(3) + i * slice, base + dst, l.wb, l.hb, 1, bpb, l.block_height_log2,
                          l.block_depth_log2);
        }
    }
    __atomic_fetch_or(&t->cpu_levels, 1u << level, __ATOMIC_RELEASE);
    backend_pool_written(t->addr, tegra_total_size(&t->layout));
}
NVN(nvnTextureWriteTexelsStrided) { LOG("[nvn] WriteTexelsStrided not implemented\n"); (void)c; }
NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\n"); (void)c; }
NVN(nvnTextureReadTexelsStrided) { (void)c; }

// ================================================================== samplers
// SamplerBuilder: the game's NVNsamplerBuilder is only 64 bytes in NVN 55 (BOTW 1.8.2 builds one on the stack
// right below its saved frame pointer, main 0x71000156f0), so the builder state is packed to fit; an
// NvnSamplerDesc (72 bytes) after the device pointer overwrote the caller's frame.
typedef struct {
    uint64_t device;
    uint8_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
} NvnSamplerBuilderState;
_Static_assert(sizeof(NvnSamplerBuilderState) <= 64, "NVNsamplerBuilder is 64 bytes");
#define SD ((NvnSamplerBuilderState *)AP(0))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerBuilderState *d = SD;
    uint64_t device = d->device;
    memset(d, 0, sizeof *d);
    d->device = device;
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_min = 0; d->lod_max = 1000.0f; d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

static NvnSamplerDesc sampler_desc_from_builder(const NvnSamplerBuilderState *b) {
    NvnSamplerDesc d = {0};
    d.min_filter = b->min_filter; d.mag_filter = b->mag_filter;
    for (int i = 0; i < 3; i++) d.wrap[i] = b->wrap[i];
    d.compare_mode = b->compare_mode; d.compare_func = b->compare_func; d.reduction = b->reduction;
    d.lod_min = b->lod_min; d.lod_max = b->lod_max; d.lod_bias = b->lod_bias; d.max_aniso = b->max_aniso;
    memcpy(d.border, b->border, sizeof d.border);
    return d;
}

// An NVNsampler holds a pointer to its description, and the game copies sampler objects by value (registering the
// copy later): descriptions are shared per distinct setting and never freed, so a copy made before the original
// was finalized still names its own settings. (Each object owning a malloc'd description gave the copy a dangling
// pointer; once the memory went to another sampler, the pool slot got that sampler's wrap and filters, and the
// Shrine of Resurrection's walkway texture was clamped instead of repeated in some runs.)
static pthread_mutex_t sampler_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnSamplerDesc *sampler_intern(const NvnSamplerDesc *want) {
    static NvnSamplerDesc **tab;
    static size_t n, cap;
    pthread_mutex_lock(&sampler_lock);
    NvnSamplerDesc *d = NULL;
    for (size_t i = 0; i < n && !d; i++) if (!memcmp(tab[i], want, sizeof *want)) d = tab[i];
    if (!d) {
        if (n == cap) { cap = cap ? cap * 2 : 64; tab = realloc(tab, cap * sizeof *tab); }
        d = malloc(sizeof *d);
        *d = *want;
        tab[n++] = d;
    }
    pthread_mutex_unlock(&sampler_lock);
    return d;
}
NVN(nvnSamplerInitialize) {
    NvnSamplerDesc d = sampler_desc_from_builder(AP(1));
    SET_HOST(A(0), sampler_intern(&d));
    RET(1);
}
NVN(nvnSamplerFinalize) { SET_HOST(A(0), 0); }
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) { RET(memcmp(HOST(NvnSamplerDesc, A(0)), HOST(NvnSamplerDesc, A(1)), sizeof(NvnSamplerDesc)) == 0); }
NVN(nvnSamplerGetDebugID) { RET(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];
RT_ON_RESET(desc_pools_reset) { memset(g_texture_pools, 0, sizeof g_texture_pools); memset(g_sampler_pools, 0, sizeof g_sampler_pools); }

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg) {
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = calloc(count, entry);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    return d;
}

NVN(nvnTexturePoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools);
    RET(1);
}
NVN(nvnTexturePoolFinalize) { (void)c; }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    e->tex = HOST(NvnTexture, A(2));
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count && HOST(NvnSamplerDesc, A(2))) {
        NvnSamplerDesc s = *HOST(NvnSamplerDesc, A(2));
        s.backend = NULL;  // (the pool entry gets its own VkSampler)
        ((NvnSamplerDesc *)d->entries)[id] = s;
    }
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = sampler_desc_from_builder(AP(2));
}

NvnDescPool *nvn_find_pool(NvnDescPool **reg, uint64_t guest_pool) {
    return guest_pool ? HOST(NvnDescPool, guest_pool) : NULL;
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); }
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
static void set_shaders(Cpu *c, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(uint64_t *)(data + i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (uint64_t *)(data + i * stride + 8);
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = *(uint32_t *)(uintptr_t)(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (*(uint32_t *)(uintptr_t)code != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = *(const uint32_t *)(control + 0x700);
        p->stages[stage].const_size = *(const uint32_t *)(control + 0x6FC);
        p->stage_mask |= 1u << stage;
    }
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
// BOTW's frame loop (sead GameFrameworkNx, 1.8.2 main 0x700101287c) sets the interval here every frame (lr
// frame_loop_interval_ret, 1.8.2 0x7001012a34; x19 = the framework). It times each frame ([fw+0x250], 19.2 MHz system
// ticks, from the frame's start to the end of its calc and draw) and goes to interval 3 (20 fps) after three frames in
// a row over 1.9 vsyncs (31.7 ms), back to 2 after a few fast ones; [fw+0x234] is the highest interval it may pick (3).
// SAVAGEWIND_LOG_FRAMETIME=<ms>: frames whose game time is over that, and every change of interval (tests).
// SAVAGEWIND_MAX_INTERVAL=2: the game never picks 3, so its step per frame is fixed and a scripted run takes the same
// path every time (benchmarks; with 3 allowed, the path depends on the frame times). The default while Savage Wind
// paces the frames and sets the game's clock (runtime/hle/framepace.c): the interval no longer sets either.
#define FRAME_LOOP_LR g_site.frame_loop_interval_ret
NVN(nvnWindowSetPresentInterval) {
    int want = (int)A(1);
    if (c->x[30] == FRAME_LOOP_LR) {
        uint64_t fw = c->x[19];
        static int log_ms = -2, max_iv = -1;
        if (log_ms == -2) { const char *e = getenv("SAVAGEWIND_LOG_FRAMETIME"); log_ms = e ? atoi(e) : -1; }
        if (max_iv < 0) {
            const char *e = getenv("SAVAGEWIND_MAX_INTERVAL");
            max_iv = e ? atoi(e) : framepace_on() ? 2 : 0;
        }
        uint32_t *max_field = (uint32_t *)(uintptr_t)(fw + 0x234);
        if (max_iv > 0 && *max_field > (uint32_t)max_iv) *max_field = (uint32_t)max_iv;
        if (log_ms >= 0) {
            static uint64_t n, last_ns;
            uint64_t now = plat_now_ns();
            double ms = (double)*(uint64_t *)(uintptr_t)(fw + 0x250) / 19200.0;
            if (ms > log_ms || WIN->interval != want)
                LOG("[frame] #%llu game %.1f ms, since the last %.1f ms, interval %d -> %d\n", (unsigned long long)n, ms,
                    (now - last_ns) / 1e6, WIN->interval, want);
            n++;
            last_ns = now;
        }
    }
    WIN->interval = want;
}
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
// Presentation pace. On the console a frame reaches the screen at a vsync, and the window's present interval
// (vsyncs per frame) sets the pace: the next acquire waits for a free texture. The HLE presented at once, so the
// game ran as fast as the host let it (BOTW's main menu at 130 fps, its animations too fast). A present now waits
// for its frame's turn on a steady grid: 1 / the frame rate (Video > Frame rate, 30 in the menus; runtime/hle/
// framepace.c) after the last one, and tells framepace.c when it went out (the game's clock follows it).
// SAVAGEWIND_FRAMEPACE=0 (the game's own pacing): interval x 1/60 s after the last one, and never sooner than
// SAVAGEWIND_FPS_CAP allows (default 60; 0 = no cap).
uint64_t g_pace_sleep_ns;  // (VK_PROF hitch lines) time present_pace slept
static void present_pace(int interval) {
    static int cap = -1;
    static uint64_t last;
    if (cap < 0) cap = getenv("SAVAGEWIND_FPS_CAP") ? atoi(getenv("SAVAGEWIND_FPS_CAP")) : 60;
    uint64_t period;
    if (framepace_on()) period = framepace_period_ns();
    else {
        period = 16666667ull * (uint64_t)(interval > 0 ? interval : 0);
        if (cap > 0 && period < 1000000000ull / (uint64_t)cap) period = 1000000000ull / (uint64_t)cap;
        if (!period) return;
    }
    uint64_t target = last + period, now = plat_now_ns();
    if (last && now < target) kernel_sleep_until_ns(target);
    uint64_t t1 = plat_now_ns();
    g_pace_sleep_ns += t1 - now;
    now = t1;
    last = last && now < target + period ? target : now;  // (steady cadence; after a long frame, start again)
    framepace_presented(last, last == target);
}

NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    static int logged = -1;
    if (logged != w->interval) { logged = w->interval; LOG("[nvn] window present interval %d\n", w->interval); }
    TL("present (pace)");
    present_pace(w->interval);
    if (frame_split()) g_fs_pace = plat_now_ns();
    perfmon_frame();
    TL("present (backend)");
    if (async_exec()) {
        if (i >= 0 && i < w->num) queue_put(QOP_PRESENT, 0, HOST(NvnTexture, w->textures[i]));
        TL("present queued");
        return;
    }
    if (host_work()) kernel_host_work_begin();
    if (i >= 0 && i < w->num) backend_present(HOST(NvnTexture, w->textures[i]));
    if (host_work()) kernel_host_work_end();
    TL("presented");
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
