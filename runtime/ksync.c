// Futex-style locks and conditions (ksync.h).
#include "ksync.h"

#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef BOOL(WINAPI *WaitOnAddressFn)(volatile VOID *, PVOID, SIZE_T, DWORD);
typedef VOID(WINAPI *WakeFn)(PVOID);
static WaitOnAddressFn p_wait;
static WakeFn p_wake1, p_wakeall;
static bool park_init(void) {
    HMODULE h = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    if (!h) h = LoadLibraryA("kernelbase.dll");
    if (!h) return false;
    p_wait = (WaitOnAddressFn)(void *)GetProcAddress(h, "WaitOnAddress");
    p_wake1 = (WakeFn)(void *)GetProcAddress(h, "WakeByAddressSingle");
    p_wakeall = (WakeFn)(void *)GetProcAddress(h, "WakeByAddressAll");
    return p_wait && p_wake1 && p_wakeall;
}
// Sleeps while *a == expect, for at most rel_ns (< 0: no limit). Returns at any wake-up, spurious ones included.
static void park(uint32_t *a, uint32_t expect, int64_t rel_ns) {
    DWORD ms = INFINITE;
    if (rel_ns >= 0) {
        int64_t m = (rel_ns + 999999) / 1000000;
        ms = m >= 0xFFFFFFFELL ? 0xFFFFFFFEu : (DWORD)m;
    }
    p_wait((volatile VOID *)a, &expect, sizeof expect, ms);
}
static void wake(uint32_t *a, bool all) {
    if (all) p_wakeall(a);
    else p_wake1(a);
}
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
static bool park_init(void) { return true; }
static void park(uint32_t *a, uint32_t expect, int64_t rel_ns) {
    struct timespec ts, *tp = NULL;
    if (rel_ns >= 0) {
        ts.tv_sec = rel_ns / 1000000000;
        ts.tv_nsec = rel_ns % 1000000000;
        tp = &ts;
    }
    syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, expect, tp, NULL, 0);
}
static void wake(uint32_t *a, bool all) { syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, all ? 0x7fffffff : 1, NULL, NULL, 0); }
#else
static bool park_init(void) { return false; }
static void park(uint32_t *a, uint32_t expect, int64_t rel_ns) { (void)a, (void)expect, (void)rel_ns; }
static void wake(uint32_t *a, bool all) { (void)a, (void)all; }
#endif

static int g_mode = -1;  // 1: the futex words, 0: the pthread members
static int g_mtries = 64;
static uint64_t g_spin_ns;

static void ksync_setup(void) {
    int mode = 1;
    const char *e = getenv("SAVAGEWIND_KSYNC");
    if (e && !strcmp(e, "0")) mode = 0;
    if (mode && !park_init()) mode = 0;
    if (getenv("SAVAGEWIND_KLOCK_SPIN")) g_mtries = atoi(getenv("SAVAGEWIND_KLOCK_SPIN"));
    long us = 20;
    if (getenv("SAVAGEWIND_KSYNC_SPIN")) us = atol(getenv("SAVAGEWIND_KSYNC_SPIN"));
    int cpus = plat_cpu_count();
    g_spin_ns = cpus < 6 || us < 0 ? 0 : (uint64_t)us * 1000;
    __atomic_store_n(&g_mode, mode, __ATOMIC_RELEASE);
}
static inline int mode(void) {
    int m = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
    if (__builtin_expect(m < 0, 0)) {
        ksync_setup();
        m = g_mode;
    }
    return m;
}

static inline void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ------------------------------------------------------------------ mutex
void kmutex_init(KMutex *m) {
    pthread_mutex_init(&m->pm, NULL);
    m->s = 0;
}

bool kmutex_trylock(KMutex *m) {
    if (!mode()) return pthread_mutex_trylock(&m->pm) == 0;
    uint32_t z = 0;
    return __atomic_compare_exchange_n(&m->s, &z, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void kmutex_lock(KMutex *m) {
    if (!mode()) {  // (a short spin first: the section is a microsecond or two, a sleep is 5-10)
        for (int i = 0; i < g_mtries; i++) {
            if (!pthread_mutex_trylock(&m->pm)) return;
            relax();
        }
        pthread_mutex_lock(&m->pm);
        return;
    }
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->s, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    for (int i = 0; i < g_mtries; i++) {
        if (__atomic_load_n(&m->s, __ATOMIC_RELAXED) == 0) {
            c = 0;
            if (__atomic_compare_exchange_n(&m->s, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        }
        relax();
    }
    if (c != 2) c = __atomic_exchange_n(&m->s, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        park(&m->s, 2, -1);
        c = __atomic_exchange_n(&m->s, 2, __ATOMIC_ACQUIRE);
    }
}

void kmutex_unlock(KMutex *m) {
    if (!mode()) {
        pthread_mutex_unlock(&m->pm);
        return;
    }
    if (__atomic_exchange_n(&m->s, 0, __ATOMIC_RELEASE) == 2) wake(&m->s, false);
}

// ------------------------------------------------------------------ condition
void kcond_init(KCond *c) {
    pthread_cond_init(&c->pc, NULL);
    c->seq = 0;
    c->parked = 0;
}

void kcond_destroy(KCond *c) { pthread_cond_destroy(&c->pc); }

static void cond_wake(KCond *c, bool all) {
    if (!mode()) {
        if (all) pthread_cond_broadcast(&c->pc);
        else pthread_cond_signal(&c->pc);
        return;
    }
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->parked, __ATOMIC_SEQ_CST)) wake(&c->seq, all);  // (nobody asleep: no kernel call)
}
void kcond_signal(KCond *c) { cond_wake(c, false); }
void kcond_broadcast(KCond *c) { cond_wake(c, true); }

// abs: NULL waits without a limit. Returns false if the time ran out.
static bool cond_wait(KCond *c, KMutex *m, const struct timespec *abs) {
    if (!mode()) {
        if (!abs) {
            pthread_cond_wait(&c->pc, &m->pm);
            return true;
        }
        return pthread_cond_timedwait(&c->pc, &m->pm, abs) == 0;
    }
    uint32_t s = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
    kmutex_unlock(m);
    bool ok = true;
    if (g_spin_ns) {  // (the signal usually comes within a few microseconds: no sleep, no wake-up call for it)
        uint64_t t0 = now_ns();
        for (unsigned i = 1; __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == s; i++) {
            relax();
            if (!(i & 31) && now_ns() - t0 > g_spin_ns) break;
        }
    }
    if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == s) {
        __atomic_add_fetch(&c->parked, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&c->seq, __ATOMIC_SEQ_CST) == s) {
            int64_t rel = -1;
            if (abs) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                rel = ((int64_t)abs->tv_sec - (int64_t)now.tv_sec) * 1000000000ll + ((int64_t)abs->tv_nsec - (int64_t)now.tv_nsec);
                if (rel <= 0) {
                    ok = false;
                    break;
                }
            }
            park(&c->seq, s, rel);
        }
        __atomic_sub_fetch(&c->parked, 1, __ATOMIC_SEQ_CST);
    }
    kmutex_lock(m);
    return ok;
}
void kcond_wait(KCond *c, KMutex *m) { cond_wait(c, m, NULL); }
bool kcond_timedwait(KCond *c, KMutex *m, const struct timespec *abs) { return cond_wait(c, m, abs); }
