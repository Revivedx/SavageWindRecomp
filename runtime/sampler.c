// (profiling) Sampling profiler for Windows. SAVAGEWIND_SAMPLE=<start s>:<seconds>[:<interval us>] suspends every
// guest thread each interval (default 1000 us) from <start> seconds after boot, for <seconds>, and records its host
// call stack (x64 unwind data: the game library and the runtime both have it). The samples go to
// build/samples.txt; tools/profile_samples.py turns them into self / inclusive time per function, recompiled game
// functions as f_<guest address>. A thread waiting in the kernel shows its wait; filter with the tool.
// Each round of samples starts with "R <ns> <presented frame>", so a stretch of frames can be looked at alone.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "rt.h"

#ifdef _WIN32
#include <windows.h>

#define MAX_THREADS 128
#define MAX_DEPTH 48

enum { SAMPLE_IDLE, SAMPLE_STARTING, SAMPLE_SAMPLING, SAMPLE_WRITING, SAMPLE_DONE,
       SAMPLE_MEMORY_ERROR, SAMPLE_FILE_ERROR, SAMPLE_THREAD_ERROR };
static atomic_int sample_state;
static struct { double start, seconds, us; } request;

const char *sampler_status(void) {
    static const char *const names[] = {"idle", "starting", "sampling", "writing", "done",
        "failed: allocation; retry a shorter capture", "failed: output; create a writable build folder and retry",
        "failed: worker creation; retry"};
    return names[atomic_load(&sample_state)];
}

static void *sampler_main(void *arg) {
    (void)arg;
    double start = request.start, secs = request.seconds, us = request.us;
    plat_sleep_until_ns(plat_now_ns() + (uint64_t)(start * 1e9));
    size_t cap = 12u << 20, used = 0;  // words (96 MB), allocated up front: nothing may allocate while a thread is suspended
    uint64_t *buf = malloc(cap * sizeof *buf);
    if (!buf) { atomic_store(&sample_state, SAMPLE_MEMORY_ERROR); return NULL; }
    atomic_store(&sample_state, SAMPLE_SAMPLING);
    pthread_t host[MAX_THREADS];
    uint32_t handle[MAX_THREADS];
    uint64_t entry[MAX_THREADS];
    uint64_t end = plat_now_ns() + (uint64_t)(secs * 1e9), nsamples = 0;
    LOG("[sample] sampling every %.0f us for %.0f s\n", us, secs);
    for (uint64_t next = plat_now_ns(); plat_now_ns() < end; next += (uint64_t)(us * 1000)) {
        int n = kernel_running_threads(host, handle, entry, MAX_THREADS);
        if (used + 4 < cap) {  // a round marker: "R <ns> <presented frame>" in the file
            extern uint64_t renderer_present_count(void);
            buf[used++] = ~0ull;
            buf[used++] = 2;
            buf[used++] = plat_now_ns();
            buf[used++] = renderer_present_count();
        }
        for (int i = 0; i < n && used + MAX_DEPTH + 4 < cap; i++) {
            HANDLE h = (HANDLE)pthread_gethandle(host[i]);
            if (!h || SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx)) {
                size_t at = used;
                buf[used++] = handle[i];
                buf[used++] = 0;  // depth, filled below
                int d = 0;
                for (; d < MAX_DEPTH && ctx.Rip; d++) {
                    buf[used++] = ctx.Rip;
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (!f) {  // a leaf without unwind data: the return address is on top of the stack
                        if (!ctx.Rsp || IsBadReadPtr((void *)ctx.Rsp, 8)) break;
                        ctx.Rip = *(DWORD64 *)ctx.Rsp;
                        ctx.Rsp += 8;
                        continue;
                    }
                    void *hd = NULL;
                    DWORD64 fr = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &hd, &fr, NULL);
                }
                buf[at + 1] = (uint64_t)d;
                nsamples++;
            }
            ResumeThread(h);
        }
        uint64_t now = plat_now_ns();
        if (next > now) plat_sleep_until_ns(next);
        else next = now;
    }
    atomic_store(&sample_state, SAMPLE_WRITING);
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        fprintf(f, "M exe %llx\nM swgame %llx\n", (unsigned long long)(uintptr_t)GetModuleHandleA(NULL),
                (unsigned long long)(uintptr_t)GetModuleHandleA("swgame.dll"));
        int n = kernel_running_threads(host, handle, entry, MAX_THREADS);
        for (int i = 0; i < n; i++) fprintf(f, "T %u %llx\n", handle[i], (unsigned long long)entry[i]);
        for (size_t p = 0; p < used;) {
            uint64_t th = buf[p], d = buf[p + 1];
            if (th == ~0ull) {
                fprintf(f, "R %llu %llu\n", (unsigned long long)buf[p + 2], (unsigned long long)buf[p + 3]);
                p += 2 + d;
                continue;
            }
            fprintf(f, "S %llu", (unsigned long long)th);
            for (uint64_t k = 0; k < d; k++) fprintf(f, " %llx", (unsigned long long)buf[p + 2 + k]);
            fputc('\n', f);
            p += 2 + d;
        }
        bool failed = ferror(f) != 0;
        if (fclose(f)) failed = true;
        if (failed) { free(buf); atomic_store(&sample_state, SAMPLE_FILE_ERROR); return NULL; }
    }
    if (!f) { free(buf); atomic_store(&sample_state, SAMPLE_FILE_ERROR); return NULL; }
    LOG("[sample] %llu samples written to build/samples.txt\n", (unsigned long long)nsamples);
    free(buf);
    atomic_store(&sample_state, SAMPLE_DONE);
    return NULL;
}

// Own the request until the worker has finished writing. No join, sleep or file I/O on the caller.
static const char *sampler_launch(double start, double seconds, double us) {
    int state = atomic_load(&sample_state);
    do {
        if (state >= SAMPLE_STARTING && state <= SAMPLE_WRITING)
            return "already active; use sample status and wait for completion";
    } while (!atomic_compare_exchange_weak(&sample_state, &state, SAMPLE_STARTING));
    request.start = start;
    request.seconds = seconds;
    request.us = us;
    pthread_t th;
    if (pthread_create(&th, NULL, sampler_main, NULL)) {
        atomic_store(&sample_state, SAMPLE_THREAD_ERROR);
        return "cannot start worker; retry";
    }
    pthread_detach(th);
    return NULL;
}

const char *sampler_request(double seconds, unsigned interval_us) {
    if (!isfinite(seconds) || seconds < 0.001 || seconds > 3600 || interval_us < 100 || interval_us > 1000000)
        return "use seconds 0.001-3600 and interval_us 100-1000000";
    return sampler_launch(0, seconds, interval_us);
}

__attribute__((constructor)) static void sampler_start(void) {
    const char *e = getenv("SAVAGEWIND_SAMPLE");
    if (!e) return;
    double start = 0, secs = 0, us = 1000;
    sscanf(e, "%lf:%lf:%lf", &start, &secs, &us);
    if (!isfinite(start) || !isfinite(secs) || !isfinite(us) || start < 0 || secs <= 0 || us <= 0 ||
        start >= 1e9 || secs >= 1e9 || us >= 1e12) return;
    (void)sampler_launch(start, secs, us);
}
#else
const char *sampler_status(void) { return "unsupported: Windows only"; }
const char *sampler_request(double seconds, unsigned interval_us) {
    (void)seconds; (void)interval_us;
    return "runtime sampling needs Windows; run the Windows build and use sample start <seconds> [interval_us]";
}
#endif
