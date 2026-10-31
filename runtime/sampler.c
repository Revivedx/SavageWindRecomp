// The profiler's sampler: while a capture runs, every interval (default 1000 us, SAVAGEWIND_PROF_SAMPLE=<us>, 0 = off)
// it takes the host call stack of every guest thread and writes it into the capture (PROF_SAMPLE + PROF_STACK records,
// runtime/prof.h). tools/swprof.py splits the samples into game code, the SWR runtime and the driver / system, and
// names the runtime's hot functions. The recompiled game code is one total on purpose: nothing here names its functions.
//
// Windows: the thread is suspended, its context read and the stack walked with the x64 unwind data (the game library
//   and the runtime both have it); nothing allocates or locks while a thread is suspended.
// Linux: a tagged realtime signal reads the interrupted PC. No unwinding, locks or allocation in its handler.
// Elsewhere: no samples (the zones and counters of the capture are all there).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "prof.h"
#include "rt.h"

#define MAX_THREADS 128
#define DEPTH 18  // frames kept per sample (6 records): the top ones decide where the time is; the callers name the cause

static pthread_t g_thread;
static int g_running;
static uint32_t g_interval_us;

// One sample: the head record (time, os thread id, kernel handle, depth), then the addresses three to a record.
static void emit(uint64_t t, uint64_t tid, uint32_t handle, const uint64_t *pc, int n) {
    prof_put(PROF_SAMPLE | ((uint32_t)n << 8), 0, t, tid, handle);
    for (int i = 0; i < n; i += 3)
        prof_put(PROF_STACK, 0, pc[i], i + 1 < n ? pc[i + 1] : 0, i + 2 < n ? pc[i + 2] : 0);
}

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

static uint64_t os_tid(HANDLE h) { return GetThreadId(h); }

// Walks the stack of a suspended thread. The thread can be anywhere (a prologue, a system call, a driver's own code),
// so the unwinder may read what is not there: a fault ends this one sample, never the game (the thread is resumed by
// the caller either way).
static int unwind(CONTEXT *ctx, uint64_t *pc) {
    volatile int d = 0;
    __try {
        for (; d < DEPTH && ctx->Rip; d++) {
            pc[d] = ctx->Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
            if (!f) {  // a leaf without unwind data: the return address is on top of the stack
                if (!ctx->Rsp || IsBadReadPtr((void *)ctx->Rsp, 8)) { d++; break; }
                ctx->Rip = *(DWORD64 *)ctx->Rsp;
                ctx->Rsp += 8;
                continue;
            }
            void *hd = NULL;
            DWORD64 fr = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx, &hd, &fr, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return d;
}

static void *sampler_main(void *arg) {
    (void)arg;
    prof_thread_name("sampler");
    pthread_t host[MAX_THREADS];
    uint32_t handle[MAX_THREADS];
    uint64_t entry[MAX_THREADS];
    for (uint64_t next = plat_now_ns(); __atomic_load_n(&g_running, __ATOMIC_ACQUIRE) && prof_enabled();
         next += (uint64_t)g_interval_us * 1000) {
        int n = kernel_running_threads(host, handle, entry, MAX_THREADS);
        for (int i = 0; i < n; i++) {
            HANDLE h = (HANDLE)pthread_gethandle(host[i]);
            if (!h || h == GetCurrentThread()) continue;
            uint64_t pc[DEPTH], tid = os_tid(h), t = 0;
            int d = 0;
            if (SuspendThread(h) == (DWORD)-1) continue;
            t = prof_ticks();
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx)) d = unwind(&ctx, pc);
            ResumeThread(h);
            if (d) emit(t, tid, handle[i], pc, d);
        }
        uint64_t now = plat_now_ns();
        if (next > now) plat_sleep_until_ns(next);
        else next = now;
    }
    return NULL;
}

int prof_sampler_modules(ProfModule *out, int max) {
    // (a game with the video libraries loaded has hundreds of modules: ask for the size first)
    DWORD need = 0;
    K32EnumProcessModules(GetCurrentProcess(), NULL, 0, &need);
    if (need > UINT32_MAX - 64 * sizeof(HMODULE)) return 0;
    DWORD capacity = need + 64 * sizeof(HMODULE);
    HMODULE *mods = (HMODULE *)malloc(capacity);
    if (!mods) return 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, capacity, &need)) {
        free(mods);
        return 0;
    }
    int n = 0;
    if (need > capacity) need = capacity;  // DLLs can be loaded between the size query and this snapshot
    HMODULE exe = GetModuleHandleA(NULL);
    for (DWORD i = 0; i < need / sizeof(HMODULE) && n < max; i++) {
        MODULEINFO mi;
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        ProfModule *m = &out[n++];
        memset(m, 0, sizeof *m);
        m->base = m->bias = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
        m->size = mi.SizeOfImage;
        K32GetModuleFileNameExA(GetCurrentProcess(), mods[i], m->name, sizeof m->name - 1);
        m->kind = mods[i] == exe ? PROF_MODULE_RUNTIME : strstr(m->name, "swgame") ? PROF_MODULE_GAME : PROF_MODULE_OTHER;
    }
    free(mods);
    return n;
}

#elif defined(__linux__)
#include <link.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// Reserve an otherwise unused realtime signal for the process lifetime. Restoring SIG_DFL at capture stop is
// unsafe: a blocked thread can still have a queued sample. Tagged, expired requests are harmless in this handler.
// Only the interrupted PC is collected on Linux: backtrace() can lock the loader/allocator from a signal handler.
#define SAMPLE_SIGNAL (SIGRTMIN + 6)
static pthread_once_t signal_once = PTHREAD_ONCE_INIT;
static bool signal_ready;
static struct {
    uint32_t state;  // request id, high bit set while the handler owns the slot; zero means free
    uint32_t answered;
    uint64_t tid, pc, ticks;
} g_slot;
static uint32_t request_id;
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "signal handler needs lock-free request atomics");

// Guest threads name themselves before running guest code. Keep their pthread storage alive while signaling:
// a TLS destructor must pass this lock before the thread can finish, detach or be joined.
static pthread_mutex_t lifetime_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t lifetime_once = PTHREAD_ONCE_INIT;
static pthread_key_t lifetime_key;
static struct LiveThread { pthread_t host; uint64_t serial; } live[MAX_THREADS];
static uint64_t lifetime_serial;

static void unregister_thread(void *p) {
    pthread_mutex_lock(&lifetime_lock);
    ((struct LiveThread *)p)->serial = 0;
    pthread_mutex_unlock(&lifetime_lock);
}

static void init_lifetime_key(void) {
    if (pthread_key_create(&lifetime_key, unregister_thread)) abort();
}

void prof_sampler_register_thread(void) {
    pthread_once(&lifetime_once, init_lifetime_key);
    if (pthread_getspecific(lifetime_key)) return;
    pthread_mutex_lock(&lifetime_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (live[i].serial) continue;
        live[i].host = pthread_self();
        live[i].serial = ++lifetime_serial;
        if (pthread_setspecific(lifetime_key, &live[i])) live[i].serial = 0;
        break;  // unregistered threads are skipped rather than signaled without a lifetime guard
    }
    pthread_mutex_unlock(&lifetime_lock);
}

static void on_signal(int sig, siginfo_t *si, void *uc_) {
    (void)sig;
    if (si->si_code != SI_QUEUE || si->si_pid != getpid()) return;
    uint32_t req = (uint32_t)si->si_value.sival_int;
    if (!req || req & 0x80000000u) return;
    uint32_t expected = req;
    if (!__atomic_compare_exchange_n(&g_slot.state, &expected, req | 0x80000000u, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    int saved_errno = errno;
    ucontext_t *uc = uc_;
    uint64_t pc = 0;
#if defined(__x86_64__)
    pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (uint64_t)uc->uc_mcontext.pc;
#endif
    (void)uc;
    g_slot.pc = pc;
    g_slot.tid = (uint64_t)syscall(SYS_gettid);
    g_slot.ticks = prof_ticks();
    __atomic_store_n(&g_slot.answered, req, __ATOMIC_RELEASE);
    __atomic_store_n(&g_slot.state, 0, __ATOMIC_RELEASE);
    errno = saved_errno;
}

static void install_signal(void) {
    struct sigaction old, sa;
    if (SAMPLE_SIGNAL > SIGRTMAX || sigaction(SAMPLE_SIGNAL, NULL, &old) || old.sa_handler != SIG_DFL) {
        LOG("[prof] Linux sampler unavailable: realtime signal already in use\n");
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    signal_ready = sigaction(SAMPLE_SIGNAL, &sa, NULL) == 0;
}

static void *sampler_main(void *arg) {
    (void)arg;
    prof_thread_name("sampler");
    pthread_once(&signal_once, install_signal);
    if (!signal_ready) return NULL;
    pthread_t host[MAX_THREADS];
    uint32_t handle[MAX_THREADS];
    uint64_t entry[MAX_THREADS];
    for (uint64_t next = plat_now_ns(); __atomic_load_n(&g_running, __ATOMIC_ACQUIRE) && prof_enabled();
         next += (uint64_t)g_interval_us * 1000) {
        struct LiveThread snapshot[MAX_THREADS];
        pthread_mutex_lock(&lifetime_lock);
        memcpy(snapshot, live, sizeof snapshot);
        pthread_mutex_unlock(&lifetime_lock);
        int n = kernel_running_threads(host, handle, entry, MAX_THREADS);
        for (int i = 0; i < n && prof_enabled(); i++) {
            if (pthread_equal(host[i], pthread_self())) continue;
            // Never reuse a request number: an arbitrarily late signal must not match a future request.
            if (request_id == 0x7fffffffu) return NULL;
            uint32_t req = ++request_id;
            __atomic_store_n(&g_slot.state, req, __ATOMIC_RELEASE);
            union sigval value = {.sival_int = (int)req};
            int sent = ESRCH;
            pthread_mutex_lock(&lifetime_lock);
            for (int j = 0; j < MAX_THREADS; j++) {
                if (snapshot[j].serial && snapshot[j].serial == live[j].serial && host[i] == live[j].host) {
                    sent = pthread_sigqueue(live[j].host, SAMPLE_SIGNAL, value);
                    break;
                }
            }
            pthread_mutex_unlock(&lifetime_lock);
            if (!sent) {
                for (int spin = 0; spin < 250 && __atomic_load_n(&g_slot.state, __ATOMIC_ACQUIRE); spin++) {
                    struct timespec ts = {0, 20000};
                    nanosleep(&ts, NULL);
                }
            }
            uint32_t expected = req;
            __atomic_compare_exchange_n(&g_slot.state, &expected, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
            // A handler that already claimed the request only reads ucontext and writes this slot, never blocks.
            while (__atomic_load_n(&g_slot.state, __ATOMIC_ACQUIRE)) {}
            if (__atomic_load_n(&g_slot.answered, __ATOMIC_ACQUIRE) == req && g_slot.pc)
                emit(g_slot.ticks, g_slot.tid, handle[i], &g_slot.pc, 1);
        }
        uint64_t now = plat_now_ns();
        if (next > now) plat_sleep_until_ns(next);
        else next = now;
    }
    return NULL;
}

static int module_cb(struct dl_phdr_info *info, size_t sz, void *arg) {
    (void)sz;
    struct { ProfModule *out; int n, max; } *a = arg;
    if (a->n >= a->max) return 1;
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uint64_t s = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr, e = s + info->dlpi_phdr[i].p_memsz;
            if (s < lo) lo = s;
            if (e > hi) hi = e;
        }
    if (lo >= hi) return 0;
    ProfModule *m = &a->out[a->n];
    memset(m, 0, sizeof *m);
    m->base = lo;
    m->size = hi - lo;
    m->bias = info->dlpi_addr;
    snprintf(m->name, sizeof m->name, "%s", info->dlpi_name[0] ? info->dlpi_name : "(runtime)");
    m->kind = a->n == 0 ? PROF_MODULE_RUNTIME : strstr(m->name, "swgame") ? PROF_MODULE_GAME : PROF_MODULE_OTHER;
    a->n++;
    return 0;
}

int prof_sampler_modules(ProfModule *out, int max) {
    struct { ProfModule *out; int n, max; } a = {out, 0, max};
    dl_iterate_phdr(module_cb, &a);
    return a.n;
}

#else  // no sampler on this platform

static void *sampler_main(void *arg) { (void)arg; return NULL; }
int prof_sampler_modules(ProfModule *out, int max) { (void)out; (void)max; return 0; }

#endif

#ifndef __linux__
void prof_sampler_register_thread(void) {}
#endif

void prof_sampler_begin(void) {
    const char *e = getenv("SAVAGEWIND_PROF_SAMPLE");
    long us = e ? atol(e) : 1000;
    if (us <= 0 || __atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) return;
    if (us < 100) us = 100;
    g_interval_us = (uint32_t)us;
    __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_thread, NULL, sampler_main, NULL) != 0) __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
}

void prof_sampler_end(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    pthread_join(g_thread, NULL);
}
