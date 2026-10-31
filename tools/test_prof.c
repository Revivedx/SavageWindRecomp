// Test of the profiler's recorder (runtime/prof.c): several threads write zones, added-up work and counters, the
// capture file is read back and counted. `make test-prof`.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "prof.h"

// what sampler.c takes from the kernel: the threads to sample (the workers of the test)
#include <pthread.h>
#include "kernel.h"
static pthread_t g_sampled[8];
static int g_nsampled;
static pthread_mutex_t g_sampled_lock = PTHREAD_MUTEX_INITIALIZER;
int kernel_running_threads(pthread_t *host, uint32_t *handle, uint64_t *entry, int max) {
    pthread_mutex_lock(&g_sampled_lock);
    int n = g_nsampled < max ? g_nsampled : max;
    for (int i = 0; i < n; i++) host[i] = g_sampled[i], handle[i] = 0x100 + (uint32_t)i, entry[i] = 0;
    pthread_mutex_unlock(&g_sampled_lock);
    return n;
}

// what prof.c takes from the platform layer
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) {
    while (plat_now_ns() < t) {
        struct timespec ts = {0, 200000};
        nanosleep(&ts, NULL);
    }
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void spin_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    while (plat_now_ns() < end) {}
}

static void work_unit(void) {
    SW_ZONE("work");
    {
        SW_ZONE("inner");
        spin_ns(20000);
    }
    for (int i = 0; i < 10; i++) {
        SW_ACC("draw");
        { SW_ACC("draw/pre"); spin_ns(500); }
    }
    SW_COUNT("queue", 7);
}

#define WORKERS 3
#define ROUNDS 40
static pthread_barrier_t bar;
static int g_rounds;
static void *worker(void *arg) {
    pthread_mutex_lock(&g_sampled_lock);
    g_sampled[g_nsampled++] = pthread_self();
    pthread_mutex_unlock(&g_sampled_lock);
    char name[16];
    snprintf(name, sizeof name, "worker%ld", (long)(intptr_t)arg);
    prof_thread_name(name);
    for (int f = 0; f < g_rounds; f++) {
        pthread_barrier_wait(&bar);
        work_unit();
    }
    return NULL;
}

typedef struct { uint64_t zones, accs, acc_calls, counters, frames, records, samples, stack_recs; uint32_t threads, sites, modules; int ok; } Counts;

static Counts read_file(const char *path) {
    Counts c = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return c;
    unsigned char h[72];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "SWPROF01", 8)) { fclose(f); return c; }
    uint32_t version, recsize, nsites, nthreads, nchunks;
    memcpy(&version, h + 8, 4);
    memcpy(&recsize, h + 12, 4);
    memcpy(&nsites, h + 56, 4);
    memcpy(&nthreads, h + 60, 4);
    memcpy(&nchunks, h + 64, 4);
    CHECK(version == 1 && recsize == sizeof(ProfRec), "version %u rec %u", version, recsize);
    for (uint32_t i = 0; i < nsites; i++) {
        uint32_t id; uint16_t len; char buf[256];
        assert(fread(&id, 4, 1, f) == 1 && fread(&len, 2, 1, f) == 1 && len < sizeof buf && fread(buf, 1, len, f) == len);
    }
    unsigned char th[48];
    for (uint32_t i = 0; i < nthreads; i++) assert(fread(th, 1, sizeof th, f) == sizeof th);
    uint32_t nmodules;
    memcpy(&nmodules, h + 68, 4);
    unsigned char mod[24 + 4 + 196];
    for (uint32_t i = 0; i < nmodules; i++) assert(fread(mod, 1, sizeof mod, f) == sizeof mod);
    c.modules = nmodules;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t hdr[2];
        assert(fread(hdr, 4, 2, f) == 2);
        CHECK(hdr[0] < nthreads, "chunk thread %u of %u", hdr[0], nthreads);
        ProfRec *r = malloc(hdr[1] * sizeof *r + 1);
        assert(fread(r, sizeof *r, hdr[1], f) == hdr[1]);
        for (uint32_t j = 0; j < hdr[1]; j++) {
            switch (r[j].kind & 0xff) {
            case PROF_ZONE: c.zones++; CHECK(r[j].t1 >= r[j].t0, "zone ends before it starts"); break;
            case PROF_ACC: c.accs++; c.acc_calls += r[j].kind >> 8; break;
            case PROF_COUNTER: c.counters++; CHECK(r[j].t1 == 7, "counter value %llu", (unsigned long long)r[j].t1); break;
            case PROF_FRAME: c.frames++; break;
            case PROF_SAMPLE: c.samples++; CHECK((r[j].kind >> 8) > 0 && (r[j].kind >> 8) <= 18, "sample depth %u", r[j].kind >> 8); break;
            case PROF_STACK: c.stack_recs++; break;
            default: CHECK(0, "kind %u", r[j].kind);
            }
        }
        c.records += hdr[1];
        free(r);
    }
    c.threads = nthreads;
    c.sites = nsites;
    c.ok = 1;
    fclose(f);
    return c;
}

static void wait_idle(void) {
    char s[256];
    for (int i = 0; i < 400; i++) {
        prof_status(s, sizeof s);
        if (!strncmp(s, "idle", 4)) return;
        usleep(10000);
    }
    CHECK(0, "capture file was not finished: %s", s);
}

static void run_capture(int frames, Counts *out) {
    pthread_t th[WORKERS];
    g_nsampled = 0;
    g_rounds = frames;
    pthread_barrier_init(&bar, NULL, WORKERS + 1);
    for (long i = 0; i < WORKERS; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    prof_thread_name("main");
    CHECK(prof_start(), "start");
    CHECK(!prof_start(), "a second start while capturing must be refused");
    for (int f = 0; f < frames; f++) {
        prof_frame((uint64_t)f);
        pthread_barrier_wait(&bar);
        work_unit();
        usleep(500);
    }
    for (int i = 0; i < WORKERS; i++) pthread_join(th[i], NULL);
    prof_stop();
    wait_idle();
    pthread_barrier_destroy(&bar);
    *out = read_file(prof_last_file());
}

int main(void) {
    setenv("SAVAGEWIND_PROF_MB", "8", 1);  // small buffer: the full-buffer test below fills it
    char dir[] = "/tmp/test_prof_XXXXXX";
    assert(mkdtemp(dir));
    assert(chdir(dir) == 0);

    // names made at run time: the same text gives the same id, another text another
    {
        char n1[32] = "pass 1920x1080", n2[32] = "pass 1280x720";
        uint32_t a = prof_site_dyn(n1), b = prof_site_dyn(n2);
        n1[0] = 'X';  // (the name is copied)
        CHECK(a && b && a != b, "dynamic ids %u %u", a, b);
        CHECK(prof_site_dyn("pass 1920x1080") == a && prof_site_dyn("pass 1280x720") == b, "dynamic ids are stable");
    }

    // off: markers record nothing and a capture started later sees none of this
    work_unit();
    char s[256];
    prof_status(s, sizeof s);
    CHECK(!strncmp(s, "idle", 4) && !prof_last_file()[0], "status when off: %s", s);

    // one capture, 3 workers + the main thread
    Counts c;
    run_capture(ROUNDS, &c);
    CHECK(c.ok, "file %s", prof_last_file());
    printf("capture 1: %llu events, %u threads, %u sites: zones %llu, acc %llu (calls %llu), counters %llu, frames %llu\n",
           (unsigned long long)c.records, c.threads, c.sites, (unsigned long long)c.zones, (unsigned long long)c.accs,
           (unsigned long long)c.acc_calls, (unsigned long long)c.counters, (unsigned long long)c.frames);
    CHECK(c.frames == ROUNDS + 1, "frames including the closing boundary: %llu", (unsigned long long)c.frames);
    CHECK(c.zones == 2ull * ROUNDS * (WORKERS + 1), "zones %llu", (unsigned long long)c.zones);
    CHECK(c.counters == 1ull * ROUNDS * (WORKERS + 1), "counters %llu", (unsigned long long)c.counters);
    CHECK(c.threads >= WORKERS + 1, "threads %u", c.threads);  // (the sampler thread is one more)
    printf("sampler: %llu samples, %llu stack records, %u modules\n", (unsigned long long)c.samples,
           (unsigned long long)c.stack_recs, c.modules);
#if defined(__linux__) || defined(_WIN32)
    CHECK(c.samples > 20 && c.stack_recs >= c.samples, "samples %llu stack records %llu", (unsigned long long)c.samples,
          (unsigned long long)c.stack_recs);
    CHECK(c.modules >= 1, "modules %u", c.modules);
#endif
    // Each work unit calls draw and draw/pre 10 times. The writer must preserve the final totals too.
    CHECK(c.acc_calls == 2ull * 10 * ROUNDS * (WORKERS + 1),
          "acc calls %llu", (unsigned long long)c.acc_calls);

    // a second capture works (threads register again) and does not contain the first one's events
    Counts c2;
    run_capture(10, &c2);
    CHECK(c2.ok && c2.frames == 11 && c2.zones == 2ull * 10 * (WORKERS + 1), "capture 2: frames %llu zones %llu",
          (unsigned long long)c2.frames, (unsigned long long)c2.zones);

    // the command: the words a front end hands over
    {
        char o[300];
        char *st[] = {"prof", "status"}, *start[] = {"prof", "start"}, *stop[] = {"prof", "stop"}, *rep[] = {"prof", "report"},
             *bad[] = {"prof", "nonsense"}, *many[] = {"prof", "start", "now"};
        CHECK(prof_command(1, st, o, sizeof o) && !strncmp(o, "idle", 4), "bare prof is status: %s", o);
        CHECK(prof_command(2, rep, o, sizeof o) && strstr(o, "swprof.py") && strstr(o, ".swprof"), "report: %s", o);
        CHECK(!prof_command(2, stop, o, sizeof o) && strstr(o, "no capture"), "stop when idle: %s", o);
        CHECK(prof_command(2, start, o, sizeof o) && prof_active(), "start: %s", o);
        CHECK(!prof_command(2, start, o, sizeof o) && strstr(o, "cannot start"), "second start: %s", o);
        CHECK(prof_command(2, stop, o, sizeof o) && !prof_active(), "stop: %s", o);
        wait_idle();
        CHECK(!prof_command(2, bad, o, sizeof o) && strstr(o, "Usage"), "unknown: %s", o);
        CHECK(!prof_command(3, many, o, sizeof o) && strstr(o, "Usage"), "too many: %s", o);
    }

    // cost, for the record
    enum { N = 2000000 };
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < N; i++) { SW_ZONE("bench"); }
    double off = (double)(plat_now_ns() - t0) / N;
    printf("marker cost: off %.1f ns per zone", off);
    CHECK(prof_start(), "start");
    prof_frame(0);
    t0 = plat_now_ns();
    for (int i = 0; i < 100000; i++) { SW_ZONE("bench"); }
    double on = (double)(plat_now_ns() - t0) / 100000;
    t0 = plat_now_ns();
    for (int i = 0; i < 100000; i++) { SW_ACC("bench_acc"); }
    double acc = (double)(plat_now_ns() - t0) / 100000;
    printf(", on %.1f ns per zone, %.1f ns per added-up zone\n", on, acc);
    prof_stop();
    wait_idle();

    // a full buffer ends the capture by itself at the next frame, and the file still reads
    prof_stop();  // (not capturing: nothing happens)
    CHECK(prof_start(), "start");
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 400000; i++) { SW_ZONE("fill"); }
        prof_frame((uint64_t)f);
        if (!prof_active()) break;
    }
    CHECK(!prof_active(), "the capture should have ended itself when the buffer was full");
    wait_idle();
    Counts full = read_file(prof_last_file());
    printf("full buffer: %llu events in the file\n", (unsigned long long)full.records);
    CHECK(full.ok && full.records > 0, "file after a full buffer");

    // F6 uses prof_toggle: acknowledge a full buffer before allowing another capture.
    uint32_t full_gen = prof_generation();
    CHECK(!prof_toggle() && !prof_active(), "F6 after a full buffer must only acknowledge the stop");
    CHECK(prof_generation() == full_gen, "acknowledgment must not start a capture");
    CHECK(prof_toggle() && prof_active(), "the next F6 must start a capture");
    CHECK(!prof_toggle() && !prof_active(), "F6 must still stop a running capture");
    wait_idle();
    CHECK(prof_toggle() && prof_active(), "a manual stop must not require acknowledgment");
    prof_stop();
    wait_idle();

    printf(failures ? "test_prof: %d FAILED\n" : "test_prof: ok\n", failures);
    return failures != 0;
}
