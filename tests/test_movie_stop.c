// Regression tests for movie::Decoder stop/acquire races in runtime/hle/movie.c.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle.h"
#include "../runtime/settings.h"

Settings g_settings;
const char *g_romfs_dir = "";
volatile int g_inject_plus;

static atomic_int g_work_depth;
static atomic_int g_work_enters;
static atomic_int g_clear_event_calls;
static pthread_mutex_t g_test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_test_cv = PTHREAD_COND_INITIALIZER;

uint64_t hle_register(const char *name, HostFn fn) {
    (void)name;
    return (uint64_t)(uintptr_t)fn;
}
void *hle_alloc(uint64_t size) { return calloc(1, (size_t)size); }
uint64_t guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os10ClearEventEPNS0_9EventTypeE")) return 0x1001ull;
    if (!strcmp(name, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE")) return 0x1002ull;
    return 0;
}
void guest_call(Cpu *c, uint64_t addr) {
    (void)c;
    if (addr == 0x1001ull) atomic_fetch_add(&g_clear_event_calls, 1);
}
void kernel_host_work_begin(void) {
    pthread_mutex_lock(&g_test_mu);
    atomic_fetch_add(&g_work_depth, 1);
    atomic_fetch_add(&g_work_enters, 1);
    pthread_cond_broadcast(&g_test_cv);
    pthread_mutex_unlock(&g_test_mu);
}
void kernel_host_work_end(void) { atomic_fetch_sub(&g_work_depth, 1); }
uint64_t input_buttons_now(void) { return 0; }
void rt_fatal_trace(void) { abort(); }

#include "../runtime/hle/movie.c"

void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    (void)t;
    (void)rgba;
    (void)w;
    (void)h;
    (void)stride;
}

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct {
    uint64_t obj[2];
    Cpu cpu;
    int32_t out_idx;
    int64_t out_pts;
    uint32_t out_flags;
    int32_t out_rem;
} AcqArgs;

static void *worker_acquire(void *arg) {
    AcqArgs *a = (AcqArgs *)arg;
    memset(&a->cpu, 0, sizeof a->cpu);
    a->out_idx = -1;
    a->out_pts = -1;
    a->out_flags = 0xdeadbeefu;
    a->out_rem = -1;
    a->cpu.x[0] = (uint64_t)(uintptr_t)a->obj;
    a->cpu.x[1] = (uint64_t)(uintptr_t)&a->out_idx;
    a->cpu.x[2] = (uint64_t)(uintptr_t)&a->out_pts;
    a->cpu.x[3] = (uint64_t)(uintptr_t)&a->out_flags;
    a->cpu.x[4] = (uint64_t)(uintptr_t)&a->out_rem;
    dec_acquire_output(&a->cpu);
    return NULL;
}

static Decoder *make_video_decoder(uint64_t obj[2]) {
    Cpu c = {0};
    c.x[0] = (uint64_t)(uintptr_t)obj;
    movie_decoder_ctor(&c);
    Decoder *d = (Decoder *)(uintptr_t)obj[1];
    d->video = true;
    d->fps = 30;
    d->events[0] = 0x2001ull;
    d->events[1] = 0x2002ull;
    return d;
}

static int test_stop_rejects_already_stopped_acquire(void) {
    uint64_t obj[2] = {0};
    Decoder *d = make_video_decoder(obj);
    d->ready[0] = (OutReady){2, 33333, 0};
    d->nready = 1;

    Cpu stop_cpu = {0};
    stop_cpu.x[0] = (uint64_t)(uintptr_t)obj;
    dec_stop(&stop_cpu);
    CHECK(d->stopped, "dec_stop sets stopped");

    AcqArgs a = {.obj = {obj[0], obj[1]}};
    worker_acquire(&a);
    CHECK(a.cpu.x[0] == 1, "dec_acquire_output must fail after dec_stop, got x[0]=%llu", (unsigned long long)a.cpu.x[0]);
    CHECK(a.out_idx == -1, "out_idx must not be written when stopped");
    return 0;
}

static int test_stop_wakes_and_aborts_paced_acquire(void) {
    uint64_t obj[2] = {0};
    Decoder *d = make_video_decoder(obj);
    d->playing = true;
    d->clock_ns = av_gettime_relative() * 1000;
    d->clock_us = 0;
    d->ready[0] = (OutReady){4, 90000, 0};  // 90 ms ahead -> enters paced wait
    d->nready = 1;

    atomic_store(&g_work_depth, 0);
    atomic_store(&g_work_enters, 0);

    AcqArgs a = {.obj = {obj[0], obj[1]}};
    pthread_t th;
    CHECK(pthread_create(&th, NULL, worker_acquire, &a) == 0, "pthread_create");

    // Wait until dec_acquire_output has released d->lock and entered the pacing wait.
    pthread_mutex_lock(&g_test_mu);
    while (atomic_load(&g_work_enters) == 0) pthread_cond_wait(&g_test_cv, &g_test_mu);
    pthread_mutex_unlock(&g_test_mu);

    int64_t t0 = av_gettime_relative();
    Cpu stop_cpu = {0};
    stop_cpu.x[0] = (uint64_t)(uintptr_t)obj;
    dec_stop(&stop_cpu);
    CHECK(pthread_join(th, NULL) == 0, "pthread_join");
    int64_t elapsed_us = av_gettime_relative() - t0;

    CHECK(a.cpu.x[0] == 1, "paced dec_acquire_output woken by dec_stop must return 1, got %llu",
          (unsigned long long)a.cpu.x[0]);
    CHECK(a.out_idx == -1, "stopped dec_acquire_output must not hand out frame index");
    CHECK(atomic_load(&g_work_depth) == 0, "kernel_host_work_begin/end must stay balanced");
    CHECK(elapsed_us < 50000, "dec_stop should wake paced wait immediately (took %lld us)", (long long)elapsed_us);
    return 0;
}

static int test_stopped_guards_input_and_release(void) {
    uint64_t obj[2] = {0};
    Decoder *d = make_video_decoder(obj);
    d->out_busy[0] = true;
    d->eos_pending = true;
    d->eos_time = 100000;

    Cpu stop_cpu = {0};
    stop_cpu.x[0] = (uint64_t)(uintptr_t)obj;
    dec_stop(&stop_cpu);

    int32_t idx = -1;
    Cpu acq_in = {0};
    acq_in.x[0] = (uint64_t)(uintptr_t)obj;
    acq_in.x[1] = (uint64_t)(uintptr_t)&idx;
    dec_acquire_input(&acq_in);
    CHECK(acq_in.x[0] == 1 && idx == -1, "dec_acquire_input must fail when stopped");

    Cpu send_in = {0};
    send_in.x[0] = (uint64_t)(uintptr_t)obj;
    send_in.x[1] = 0;
    send_in.x[5] = MOVIE_EOS;
    dec_send_input(&send_in);
    CHECK(send_in.x[0] == 1 && d->nready == 0, "dec_send_input must fail and not queue when stopped");

    Cpu rel_out = {0};
    rel_out.x[0] = (uint64_t)(uintptr_t)obj;
    rel_out.x[1] = 0;
    dec_release_output(&rel_out);
    CHECK(rel_out.x[0] == 0 && d->nready == 0, "dec_release_output must not flush pending frames when stopped");
    return 0;
}

static int test_normal_paced_acquire_succeeds(void) {
    uint64_t obj[2] = {0};
    Decoder *d = make_video_decoder(obj);
    d->clock_ns = av_gettime_relative() * 1000;
    d->clock_us = 0;
    d->ready[0] = (OutReady){3, 20000, 0};  // 20 ms ahead
    d->nready = 1;

    int64_t t0 = av_gettime_relative();
    AcqArgs a = {.obj = {obj[0], obj[1]}};
    worker_acquire(&a);
    int64_t elapsed_us = av_gettime_relative() - t0;

    CHECK(a.cpu.x[0] == 0, "normal paced dec_acquire_output must succeed, got %llu",
          (unsigned long long)a.cpu.x[0]);
    CHECK(a.out_idx == 3 && a.out_pts == 20000 && a.out_flags == 0 && a.out_rem == 0,
          "unexpected output fields idx=%d pts=%lld flags=%u rem=%d", a.out_idx, (long long)a.out_pts, a.out_flags,
          a.out_rem);
    CHECK(elapsed_us >= 10000, "normal paced acquire should wait until due time (elapsed %lld us)",
          (long long)elapsed_us);
    return 0;
}

int main(void) {
    if (test_stop_rejects_already_stopped_acquire()) return 1;
    if (test_stop_wakes_and_aborts_paced_acquire()) return 1;
    if (test_stopped_guards_input_and_release()) return 1;
    if (test_normal_paced_acquire_succeeds()) return 1;
    printf("movie stop tests passed\n");
    return 0;
}
