// A small Horizon kernel: handles, threads, synchronization, memory, and the SVC dispatcher.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"
#include "kernel.h"

// ------------------------------------------------------------------ results
#undef R_OK
#define R_OK 0
#define R_TIMEOUT 0xEA01
#define R_CANCELLED 0xEC01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_STATE 0xFA01
#define R_INVALID_ADDRESS 0xCC01
#define R_NOT_FOUND 0xF201
#define R_INVALID_ENUM 0xF001
#define R_OUT_OF_RESOURCE 0xD001

#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAITERS_BIT 0x40000000u

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;

// g_klock is taken by every syscall of every guest thread (BOTW's two job workers make thousands a second). Contended,
// the winpthreads mutex sleeps in the kernel, ~5-10 us to come back, for a section that takes a microsecond or two: the
// game's main thread spent ~6% of a Kakariko frame blocked here. So a short spin first. SAVAGEWIND_KLOCK_SPIN=<tries>
// (default 64; 0: none).
void klock(void) {
    static int tries = -1;
    if (tries < 0) tries = getenv("SAVAGEWIND_KLOCK_SPIN") ? atoi(getenv("SAVAGEWIND_KLOCK_SPIN")) : 64;
    for (int i = 0; i < tries; i++) {
        if (!pthread_mutex_trylock(&g_klock)) return;
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    pthread_mutex_lock(&g_klock);
}
static __thread KThread *t_cur;

// ------------------------------------------------------------------ handle table
#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

static uint32_t handle_alloc(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x100 + i;
        }
    }
    FATAL("out of handles\n");
}

KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_THREAD) return &t_cur->obj;
    if (h < 0x101 || h >= 0x100 + MAX_HANDLES) return NULL;
    return handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    klock();
    uint32_t h = handle_alloc(o);
    pthread_mutex_unlock(&g_klock);
    return h;
}

static void handle_close(uint32_t h) {
    KObject *o = handle_get(h);
    if (!o || h == PSEUDO_THREAD) return;
    handles[h - 0x100] = NULL;
    o->refs--;
}

// Threads blocked in WaitSynchronization; object signals wake only these (caller holds g_klock).
static KThread *g_syncwaiters;

static void wake(KThread *t) { pthread_cond_signal(&t->cond); }

void kernel_signal(KObject *o) {
    // caller holds g_klock
    o->signaled = true;
    for (KThread *t = g_syncwaiters; t; t = t->snext) wake(t);
}

// ------------------------------------------------------------------ time

uint64_t rt_ticks(void) {
    // 19.2 MHz system tick
    uint64_t ns = plat_now_ns();
    static double scale = -1;
    if (scale < 0) scale = getenv("SAVAGEWIND_TICK_SCALE") ? atof(getenv("SAVAGEWIND_TICK_SCALE")) : 1.0;
    if (scale != 1.0) ns = (uint64_t)(ns * scale);
    return (uint64_t)((__uint128_t)ns * 192 / 10000);
}

static void deadline(struct timespec *ts, int64_t timeout_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ns / 1000000000;
    ts->tv_nsec += timeout_ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

// Sleep on the current thread's condition. Returns false on timeout. Caller holds g_klock.
static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
// ------------------------------------------------------------------ cores
// Horizon runs a thread only on the core its affinity mask allows, and two threads of one core never run at the same
// time: the highest priority runnable thread runs, and equal priorities take turns when one yields or blocks. Games
// rely on that: BOTW 1.8.2 shares a sead heap without a lock between threads pinned to one core, and its free list
// broke when those threads ran in parallel here. So each of the four cores is a token a guest thread must hold to
// run guest code. It is let go while the thread blocks or sleeps and handed to the best waiter (lowest priority
// number, then first come); a thread that sees a better one waiting for its core at a syscall gives it up.
// Generated code has no preemption points of its own, so a long stretch without syscalls keeps the core.
// SAVAGEWIND_CORES=0 lets every thread run at once, as before.
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    KThread *owner;
    KThread *waiting[64];
    int nwaiting;
    uint64_t next_ticket;
} Core;
static Core g_cores[4] = {
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, {0}, 0, 0},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, {0}, 0, 0},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, {0}, 0, 0},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, {0}, 0, 0},
};
static int g_cores_on = -1;
// The best (lowest) priority among the threads waiting for each core, INT_MAX with none: what dispatch.c checks at every
// indirect call (kernel_preempt). SAVAGEWIND_NO_PREEMPT=1: kept at INT_MAX (preemption at syscalls only, as before).
// One cache line per core, written only when it changes: every indirect call of every guest thread reads its core's
// entry, and one shared line rewritten at each core hand-over missed in their caches.
CoreBestWait g_core_best_wait[4] = {{INT_MAX}, {INT_MAX}, {INT_MAX}, {INT_MAX}};
static void core_best_update(Core *k) {  // (k->m held)
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_PREEMPT") != NULL;
    int best = INT_MAX;
    for (int i = 0; i < k->nwaiting && !off; i++)
        if (k->waiting[i]->prio < best) best = k->waiting[i]->prio;
    if (__atomic_load_n(&g_core_best_wait[k - g_cores].v, __ATOMIC_RELAXED) != best)
        __atomic_store_n(&g_core_best_wait[k - g_cores].v, best, __ATOMIC_RELAXED);
}

static bool cores_on(void) {
    if (g_cores_on < 0) g_cores_on = !(getenv("SAVAGEWIND_CORES") && !strcmp(getenv("SAVAGEWIND_CORES"), "0"));
    return g_cores_on;
}

// Whether t is the waiter that gets the core next. Caller holds k->m.
static bool core_next(Core *k, KThread *t) {
    for (int i = 0; i < k->nwaiting; i++) {
        KThread *o = k->waiting[i];
        if (o != t && (o->prio < t->prio || (o->prio == t->prio && o->core_ticket < t->core_ticket))) return false;
    }
    return true;
}

static void core_acquire(KThread *t) {
    if (!t || !cores_on() || t->core_held >= 0 || t->off_cores) return;
    static int one_core = -1;  // (debug) SAVAGEWIND_ONE_CORE=1: every thread on core 0, one guest thread at a time
    if (one_core < 0) one_core = getenv("SAVAGEWIND_ONE_CORE") != NULL;
    int idx = one_core ? 0 : t->core & 3;
    Core *k = &g_cores[idx];
    pthread_mutex_lock(&k->m);
    t->core_ticket = k->next_ticket++;
    if (k->nwaiting < 64) k->waiting[k->nwaiting++] = t;
    core_best_update(k);
    while ((k->owner || !core_next(k, t)) && !g_rt_unwind) pthread_cond_wait(&k->cv, &k->m);
    for (int i = 0; i < k->nwaiting; i++)
        if (k->waiting[i] == t) { k->waiting[i] = k->waiting[--k->nwaiting]; break; }
    core_best_update(k);
    if (!k->owner) k->owner = t;  // (during a reboot the core may stay with another thread: run anyway)
    t->core_held = k->owner == t ? idx : -1;
    pthread_mutex_unlock(&k->m);
}

static void core_release(KThread *t) {
    if (!t || t->core_held < 0) return;
    Core *k = &g_cores[t->core_held];
    pthread_mutex_lock(&k->m);
    if (k->owner == t) k->owner = NULL;
    t->core_held = -1;
    pthread_cond_broadcast(&k->cv);
    pthread_mutex_unlock(&k->m);
}

// A thread that does a coprocessor's work runs beside the four cores, as on the console: the in-process audio
// renderer (runtime/services/audio.c) is the audio DSP's. Waiting for a core token behind the game's threads, it
// fell tens of milliseconds behind and the sound broke up.
void kernel_thread_off_cores(void) {
    KThread *t = t_cur;
    if (!t) return;
    core_release(t);
    t->off_cores = true;
}

// Host code running on a guest thread (an HLE call) waits until t without holding the thread's core, in slices of
// at most 50 ms so a reboot doesn't wait it out (as svcSleepThread).
void kernel_sleep_until_ns(uint64_t t) {
    KThread *me = t_cur;
    core_release(me);
    for (uint64_t now; !g_rt_unwind && (now = plat_now_ns()) < t;)
        plat_sleep_until_ns(t - now < 50000000 ? t : now + 50000000);
    core_acquire(me);
}

// The GPU backend's work on a guest thread (runtime/nvn/objects.c: submits, presents, GPU waits). On the console that
// work is the GPU's: the thread that submits returns at once, and one that waits for the GPU gives its core up. Here
// it is host code on the submitting thread, ~10 ms a frame in the open world plus shader compiles: holding the core,
// it kept the game's other threads of that core waiting. It reads guest memory the GPU would read anyway.
static __thread int host_work_depth;
void kernel_host_work_begin(void) {
    if (host_work_depth++ == 0) core_release(t_cur);
}
void kernel_host_work_end(void) {
    if (--host_work_depth == 0) core_acquire(t_cur);
}

// At a syscall: give the core to a better thread waiting for it, if there is one.
static void core_preempt_point(KThread *t) {
    if (!t || t->core_held < 0) return;
    Core *k = &g_cores[t->core_held];
    if (!__atomic_load_n(&k->nwaiting, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&k->m);
    bool better = false;
    for (int i = 0; i < k->nwaiting; i++)
        if (k->waiting[i]->prio < t->prio) better = true;
    pthread_mutex_unlock(&k->m);
    if (better) { core_release(t); core_acquire(t); }
}

// Horizon runs a better (lower priority number) thread the moment it can run, taking the core from a worse one.
// Generated code has no preemption points of its own, so the core went over only at the holder's next syscall: in
// BOTW's open world a prio-48 job thread ran ~5 ms of guest code without one once a second, while a prio-44 worker of
// the same core, which the main thread was waiting for, waited for the core (a ~10 ms longer frame every second).
// dispatch.c calls this at every indirect call (a safe point: the guest state is all in Cpu).
void kernel_preempt(KThread *t) {
    if (t->core_held < 0 || __atomic_load_n(&g_core_best_wait[t->core_held].v, __ATOMIC_RELAXED) >= t->prio) return;
    core_release(t);
    core_acquire(t);
}

// svcSleepThread(0) and friends: let the threads of the same priority waiting for this core go first.
static void core_yield(KThread *t) {
    if (!t || t->core_held < 0) { sched_yield(); return; }
    core_release(t);
    core_acquire(t);
}

// Moves t to another core (svcSetThreadCoreMask). The running thread changes cores at once; another one picks the
// new core up the next time it acquires one.
static void core_move(KThread *t, int core) {
    if (core < 0 || core > 3 || t->core == core) return;
    if (t == t_cur && t->core_held >= 0) {
        core_release(t);
        t->core = core;
        core_acquire(t);
    } else {
        t->core = core;
    }
}

static bool kwait(int64_t timeout_ns, const struct timespec *ts) {
    pthread_cond_t *cv = t_cur ? &t_cur->cond : &g_nothread_cond;
    KThread *t = t_cur;
    bool held = t && t->core_held >= 0;
    if (held) core_release(t);  // (lock order: g_klock, then a core's m; core_acquire never runs under g_klock)
    bool r;
    if (timeout_ns < 0) {
        pthread_cond_wait(cv, &g_klock);
        r = true;
    } else {
        r = pthread_cond_timedwait(cv, &g_klock, ts) == 0;
    }
    if (held) {
        pthread_mutex_unlock(&g_klock);
        core_acquire(t);
        klock();
    }
    return r;
}

// ------------------------------------------------------------------ memory map
typedef struct { uint64_t addr, size; uint32_t type, perm; } Region;
static Region regions[1024];
static int nregions;
static uint64_t heap_size;
static uint64_t tls_next = TLS_BASE;
uint64_t kernel_heap_size(void) { return heap_size; }  // bytes from HEAP_BASE set by SetHeapSize
// Writable guest data regions (heap, mapped/physical memory, module .data): for debug/runtime memory scans.
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max) {
    int n = 0;
    for (int i = 0; i < nregions && n < max; i++)
        if ((regions[i].perm & 2) && regions[i].type != 0xB && regions[i].type != 0xC) {
            addr[n] = regions[i].addr; size[n] = regions[i].size; n++;
        }
    return n;
}

static void region_add_impl(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm, bool commit) {
    if (nregions == 1024) FATAL("too many regions\n");
    if (commit) plat_commit(addr, size);  // back it before the OS writes there (Windows: file reads into the region)
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm};
}

static void region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    region_add_impl(addr, size, type, perm, true);
}

static void region_remove(uint64_t addr) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
            nregions--;
            return;
        }
    }
}

static void query_memory(uint64_t addr, uint64_t out) {
    uint64_t prev_end = 0;
    uint64_t info[5] = {0};  // addr, size, type|attr<<32, perm|ipc<<32, device|pad<<32
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info[0] = r->addr;
            info[1] = r->size;
            info[2] = r->type;
            info[3] = r->perm;
            memcpy((void *)out, info, 0x28);
            return;
        }
        if (addr < r->addr) {
            info[0] = prev_end;
            info[1] = r->addr - prev_end;
            memcpy((void *)out, info, 0x28);
            return;
        }
        prev_end = r->addr + r->size;
    }
    info[0] = prev_end;
    info[1] = (1ull << 39) - prev_end;
    memcpy((void *)out, info, 0x28);
}

// ------------------------------------------------------------------ threads
static uint64_t next_thread_id = 0x50;

// ------------------------------------------------------------------ in-place reboot
// Recompiled guest code can't be killed from outside, so a reboot (runtime/main.c) sets g_rt_unwind, wakes every
// waiting thread, and each guest thread longjmps back to thread_main at its next safe point: rt_svc (entry and
// exit), an HLE call (runtime/dispatch.c), or a kernel wait (which returns early while g_rt_unwind is set). The
// frames dropped are recompiled code (its state lives in guest memory, which is wiped next) and HLE functions;
// none of those hold a host lock across a call into the guest, so nothing stays locked. svcExitThread ends a
// thread the same way.
volatile int g_rt_unwind;
static KThread *g_threads;  // every thread of this boot (g_klock)

static pthread_t g_host_threads[4];  // (profiling) host threads the sampler takes too: handles 0xf000 + i
static int g_nhost_threads;
void kernel_sample_host_thread(void) {
    klock();
    if (g_nhost_threads < 4) g_host_threads[g_nhost_threads++] = pthread_self();
    pthread_mutex_unlock(&g_klock);
}
int kernel_running_threads(pthread_t *host, uint32_t *handle, uint64_t *entry, int max) {
    int n = 0;
    klock();
    for (KThread *t = g_threads; t && n < max; t = t->anext)
        if (t->started && !t->finished) host[n] = t->host, handle[n] = t->handle, entry[n] = t->entry, n++;
    for (int i = 0; i < g_nhost_threads && n < max; i++) host[n] = g_host_threads[i], handle[n] = 0xf000 + i, entry[n] = 0, n++;
    pthread_mutex_unlock(&g_klock);
    return n;
}

void rt_unwind_point(void) {
    if (__builtin_expect(g_rt_unwind, 0) && t_cur) longjmp(t_cur->unwind, 1);
}

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_cur = t;
    crash_handler_install();
    rt_set_fpcr(0);
    core_acquire(t);
    if (!setjmp(t->unwind)) guest_call(&t->cpu, t->entry);
    core_release(t);
    klock();
    t->finished = true;
    kernel_signal(&t->obj);
    pthread_mutex_unlock(&g_klock);
    t_cur = NULL;
    return NULL;
}

static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core) {
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cond, NULL);
    t->obj.type = KOBJ_THREAD;
    t->entry = entry;
    t->prio = t->base_prio = prio;
    t->core = core < 0 ? 0 : core & 3;  // (-2: the process's default core, 0)
    t->core_held = -1;
    t->id = next_thread_id++;
    t->cpu.x[0] = arg;
    t->cpu.sp = stack_top & ~15ull;
    t->cpu.excl_addr = ~0ull;
    t->tlr = tls_next;
    tls_next += 0x200;
    t->cpu.tpidrro = t->tlr;
    t->cpu.thread = t;
    klock();
    t->anext = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_klock);
    return t;
}

static void thread_start(KThread *t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);  // guest recursion runs on the host stack
    t->started = true;
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
}

KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio) {
    uint64_t stack = STACK_BASE + STACK_SIZE - ((stack_size + 0xFFFF) & ~0xFFFFull);
    region_add(stack, stack_size, 0xB, 3);
    KThread *t = thread_new(entry, 0, stack + stack_size, prio, 0);
    t->handle = kernel_new_handle(&t->obj);
    return t;
}

void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4) {
    t->cpu.x[0] = x0; t->cpu.x[1] = x1; t->cpu.x[2] = x2; t->cpu.x[3] = x3; t->cpu.x[4] = x4;
    thread_start(t);  // the host main thread goes on to run the window event loop
}

uint32_t kernel_current_thread_handle(void) { return t_cur ? t_cur->handle : 0; }
Cpu *kernel_current_cpu(void) { return t_cur ? &t_cur->cpu : NULL; }
KThread *kernel_current_thread(void) { return t_cur; }

// ------------------------------------------------------------------ mutex / condvar (Horizon semantics)
// Mutex word: owner handle | WAITERS_BIT. Waiting threads sleep on the global cond and are handed ownership.

static uint32_t *u32p(uint64_t a) { return (uint32_t *)(uintptr_t)a; }

// Try to take mutex `m` for `tag`, or mark it contended. Returns true if acquired. Uses CAS so a
// concurrent user-space unlock (owner storing 0) is never overwritten.
static bool mutex_acquire_or_mark(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = __atomic_load_n(u32p(m), __ATOMIC_SEQ_CST);
        if (v == 0) {
            if (__atomic_compare_exchange_n(u32p(m), &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
        } else {
            if ((v & WAITERS_BIT) ||
                __atomic_compare_exchange_n(u32p(m), &v, v | WAITERS_BIT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return false;
        }
    }
}

// Pick a thread waiting on mutex `addr` and give it ownership. Caller holds g_klock.
// Priority inheritance, as Horizon does for its mutexes: a thread holding a mutex runs at the best priority of the
// threads waiting for it (and that passes on along a chain of mutex waits). BOTW's main thread waits once a second for
// a mutex a job thread of priority 48 holds; the job thread then waited for its core behind threads the main thread
// is better than. SAVAGEWIND_NO_PRIO_INHERIT=1: off. (g_klock held for all of these.)
static bool prio_inherit_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_NO_PRIO_INHERIT") == NULL;
    return on;
}

static void prio_set(KThread *t, int p) {
    if (t->prio == p) return;
    t->prio = p;
    if (!cores_on() || t->off_cores) return;
    Core *k = &g_cores[t->core & 3];  // (waiting for its core or holding it: the waiters' order and the best wait change)
    pthread_mutex_lock(&k->m);
    core_best_update(k);
    pthread_cond_broadcast(&k->cv);
    pthread_mutex_unlock(&k->m);
}

static uint32_t mutex_owner(uint64_t addr) { return __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST) & ~WAITERS_BIT; }

// The priority t should run at: its own, or the best of the threads waiting for a mutex it holds.
static void prio_refresh(KThread *t) {
    int p = t->base_prio;
    if (prio_inherit_on())
        for (KThread *w = g_waitlist; w; w = w->wnext)
            if (w->wait_kind == WAIT_MUTEX && w->prio < p && mutex_owner(w->wait_addr) == t->handle) p = w->prio;
    prio_set(t, p);
}

// waiter starts waiting for the mutex at addr: its owner (and the owner of a mutex that one waits for...) runs at least
// at waiter's priority.
static void prio_boost_owner(KThread *waiter, uint64_t addr) {
    if (!prio_inherit_on()) return;
    for (int depth = 0; depth < 8; depth++) {
        KObject *o = handle_get(mutex_owner(addr));
        if (!o || o->type != KOBJ_THREAD) return;
        KThread *owner = (KThread *)o;
        if (owner == waiter || owner->prio <= waiter->prio) return;
        prio_set(owner, waiter->prio);
        if (owner->wait_kind != WAIT_MUTEX) return;
        addr = owner->wait_addr;
    }
}

static void mutex_release(uint64_t addr) {
    KThread *next = NULL;
    int count = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (t->wait_kind == WAIT_MUTEX && t->wait_addr == addr) {
            if (!next || t->prio < next->prio) next = t;
            count++;
        }
    }
    if (!next) {
        __atomic_store_n(u32p(addr), 0, __ATOMIC_SEQ_CST);
        if (prio_inherit_on() && t_cur && t_cur->prio != t_cur->base_prio) prio_refresh(t_cur);
        return;
    }
    __atomic_store_n(u32p(addr), next->handle | (count > 1 ? WAITERS_BIT : 0), __ATOMIC_SEQ_CST);
    next->wait_kind = WAIT_NONE;
    next->wait_result = R_OK;
    if (prio_inherit_on()) {
        prio_refresh(next);  // (the other waiters now wait for it)
        if (t_cur && t_cur != next) prio_refresh(t_cur);  // (the one letting go: back to its own, or another mutex's)
    }
    wake(next);
}

KThread *g_waitlist;

static void waitlist_add(KThread *t) { t->wnext = g_waitlist; g_waitlist = t; }
static void waitlist_remove(KThread *t) {
    for (KThread **p = &g_waitlist; *p; p = &(*p)->wnext)
        if (*p == t) { *p = t->wnext; return; }
}

// Block until wait_kind is cleared by a waker, timeout, or cancel. Caller holds g_klock.
static uint32_t block_current(int64_t timeout_ns) {
    KThread *t = t_cur;
    struct timespec ts;
    WaitKind kind = t->wait_kind;
    extern bool nvn_timeline(void);
    uint64_t tl_t0 = nvn_timeline() ? plat_now_ns() : 0, tl_addr = t->wait_addr;
    if (timeout_ns >= 0) deadline(&ts, timeout_ns);
    waitlist_add(t);
    while (t->wait_kind != WAIT_NONE) {
        if (g_rt_unwind) {  // reboot: give up the wait (the thread unwinds when its svc returns)
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (t->cancel) {
            t->cancel = false;
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (!kwait(timeout_ns, &ts)) {
            // The timeout can fire while a waker already ran: a thread it woke (or handed a mutex) did not time
            // out, and a condvar waiter that a signal moved to its mutex waits for that without a timeout, as on
            // the console. Timing out anyway, a condvar waiter would go on to take the mutex it was just given
            // (svc_wait_process_wide_key) and wait for itself.
            if (t->wait_kind == WAIT_NONE) break;
            if (kind == WAIT_CONDVAR && t->wait_kind == WAIT_MUTEX) {
                timeout_ns = -1;
                continue;
            }
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_TIMEOUT;
            break;
        }
    }
    waitlist_remove(t);
    if (tl_t0 && plat_now_ns() - tl_t0 > 200000)
        LOG("[tl] %.3f %x block %.3f ms (kind %d addr %llx lr %llx)\n", plat_now_ns() / 1e6, t->handle,
            (plat_now_ns() - tl_t0) / 1e6, kind, (unsigned long long)tl_addr, (unsigned long long)t->cpu.x[30]);
    return t->wait_result;
}

static uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    klock();
    uint32_t v = __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_klock);
        return R_OK;
    }
    KThread *t = t_cur;
    t->wait_kind = WAIT_MUTEX;
    t->wait_addr = addr;
    t->wait_tag = tag;
    prio_boost_owner(t, addr);
    uint32_t r = block_current(-1);
    pthread_mutex_unlock(&g_klock);
    return r == R_CANCELLED ? R_OK : r;
}

static uint32_t svc_arbitrate_unlock(uint64_t addr) {
    klock();
    mutex_release(addr);
    pthread_mutex_unlock(&g_klock);
    return R_OK;
}

static uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    klock();
    KThread *t = t_cur;
    // The key says "a thread waits" before the mutex is let go, as the real kernel orders it: a signaller takes
    // the mutex in user mode the moment it is free and skips svcSignalProcessWideKey when the key reads 0. Released
    // first, the wakeup could be lost and both sides wait for good (BOTW 1.8.2's job queues hung that way).
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
    t->wait_kind = WAIT_CONDVAR;
    t->wait_addr = key;
    t->wait_mutex = mutex;
    t->wait_tag = tag;
    mutex_release(mutex);
    uint32_t r = block_current(timeout);
    // A wait that timed out returns without the mutex, as on the console: user mode takes it again (BOTW 1.8.2's
    // InternalConditionVariableImplByHorizon::TimedWait calls Enter on 0xea01, sdk 0x71001492bc). Taken here too,
    // the thread then waited for the mutex it held, and every thread after it (the game's audio thread, waiting
    // for the in-process audio renderer's event).
    if (r != R_OK && r != R_TIMEOUT) {
        // cancelled: still must reacquire the mutex
        if (!mutex_acquire_or_mark(mutex, tag)) {
            t->wait_kind = WAIT_MUTEX;
            t->wait_addr = mutex;
            prio_boost_owner(t, mutex);
            block_current(-1);
        }
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void svc_signal_process_wide_key(uint64_t key, int32_t count) {
    klock();
    int woken = 0;
    bool any_left = false;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key && (!best || t->prio < best->prio)) best = t;
        if (!best) break;
        woken++;
        // the woken thread must reacquire its mutex
        uint64_t m = best->wait_mutex;
        best->wait_result = R_OK;
        if (mutex_acquire_or_mark(m, best->wait_tag)) {
            best->wait_kind = WAIT_NONE;
            wake(best);
        } else {
            best->wait_kind = WAIT_MUTEX;
            best->wait_addr = m;
            prio_boost_owner(best, m);
        }
    }
    for (KThread *t = g_waitlist; t; t = t->wnext)
        if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key) any_left = true;
    if (!any_left) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_klock);
}

// ------------------------------------------------------------------ address arbiter
enum { ARB_WAIT_LESS = 0, ARB_DEC_WAIT_LESS = 1, ARB_WAIT_EQ = 2 };
enum { ARB_SIGNAL = 0, ARB_SIGNAL_INC_EQ = 1, ARB_SIGNAL_MOD_EQ = 2 };

static uint32_t svc_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    klock();
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    bool wait;
    if (type == ARB_WAIT_LESS) wait = cur < value;
    else if (type == ARB_DEC_WAIT_LESS) {
        wait = cur < value;
        if (wait) __atomic_store_n(u32p(addr), (uint32_t)(cur - 1), __ATOMIC_SEQ_CST);
    } else if (type == ARB_WAIT_EQ) wait = cur == value;
    else { pthread_mutex_unlock(&g_klock); return R_INVALID_ENUM; }
    if (!wait) { pthread_mutex_unlock(&g_klock); return R_INVALID_STATE; }
    if (timeout == 0) { pthread_mutex_unlock(&g_klock); return R_TIMEOUT; }
    t_cur->wait_kind = WAIT_ARBITER;
    t_cur->wait_addr = addr;
    uint32_t r = block_current(timeout);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static int arbiter_wake(uint64_t addr, int32_t count) {
    int woken = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (count > 0 && woken >= count) break;
        if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) {
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_OK;
            wake(t);
            woken++;
        }
    }
    return woken;
}

static uint32_t svc_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    klock();
    uint32_t r = R_OK;
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (type == ARB_SIGNAL) {
        arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_INC_EQ) {
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)(cur + 1), __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else if (type == ARB_SIGNAL_MOD_EQ) {
        int waiting = 0;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) waiting++;
        int32_t nv;
        if (count <= 0) nv = waiting > 0 ? value - 2 : value + 1;
        else nv = waiting == 0 ? value + 1 : (waiting <= count ? value - 1 : value);
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)nv, __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else {
        r = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ wait synchronization
static uint32_t svc_wait_synchronization(uint64_t handles_ptr, int32_t count, int64_t timeout, int32_t *index) {
    klock();
    struct timespec ts;
    if (timeout > 0) deadline(&ts, timeout);
    KThread *t = t_cur;
    uint32_t *hs = (uint32_t *)(uintptr_t)handles_ptr;
    uint32_t r;
    if (ENV("SAVAGEWIND_VSYNC_BT")) {  // (debug) who waits for the display's vsync event: the game's frame loop
        extern uint32_t vi_vsync_handle(void);
        static int shown;
        for (int i = 0; i < count && shown < 2; i++)
            if (hs[i] && hs[i] == vi_vsync_handle()) {
                shown++;
                LOG("[vsync-bt] wait on vsync (%d handles, timeout %lld) lr %llx:\n", count, (long long)timeout,
                    (unsigned long long)(t ? t->cpu.x[30] : 0));
                plat_print_backtrace();
            }
    }
    for (;;) {
        for (int i = 0; i < count; i++) {
            KObject *o = handle_get(hs[i]);
            if (!o) {
                static int reported;  // (a game that polls an unknown handle repeats this every frame)
                if (reported++ < 4)
                    LOG("[kernel] WaitSynchronization on invalid handle %x (%d of %d) lr %llx\n", hs[i], i, count,
                        (unsigned long long)(t ? t->cpu.x[30] : 0));
                r = R_INVALID_HANDLE;
                goto out;
            }
            if (o->signaled) { *index = i; r = R_OK; goto out; }
        }
        if (t->cancel) { t->cancel = false; r = R_CANCELLED; goto out; }
        if (g_rt_unwind) { r = R_CANCELLED; goto out; }
        if (timeout == 0) { r = R_TIMEOUT; goto out; }
        t->in_sync_wait = true;
        t->snext = g_syncwaiters;
        g_syncwaiters = t;
        extern bool nvn_timeline(void);
        uint64_t tl_t0 = nvn_timeline() ? plat_now_ns() : 0;
        bool ok = kwait(timeout < 0 ? -1 : timeout, &ts);
        if (tl_t0 && plat_now_ns() - tl_t0 > 200000)
            LOG("[tl] %.3f %x waitsync %.3f ms (handle %x lr %llx)\n", plat_now_ns() / 1e6, t->handle,
                (plat_now_ns() - tl_t0) / 1e6, hs[0], (unsigned long long)t->cpu.x[30]);
        for (KThread **p = &g_syncwaiters; *p; p = &(*p)->snext)
            if (*p == t) { *p = t->snext; break; }
        t->in_sync_wait = false;
        if (!ok) { r = R_TIMEOUT; goto out; }
    }
out:
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ debug output
static void debug_string(uint64_t p, uint64_t len) {
    fprintf(stderr, "[guest] %.*s", (int)len, (const char *)(uintptr_t)p);
    if (len && ((const char *)(uintptr_t)p)[len - 1] != '\n') fputc('\n', stderr);
}

// ------------------------------------------------------------------ SVC dispatch
static int trace_svc = -1;

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc);

static _Atomic uint64_t svc_wait_ns[32][0x80], svc_calls[32][0x80];
static int prof_svc = -1;

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    rt_unwind_point();
    // ArbitrateUnlock and SignalProcessWideKey give the core up after they let go, not before: a thread that yielded
    // first kept its waiters waiting until it got its core back (the game's audio thread held the audio renderer's
    // mutex up to 40 ms that way).
    bool release = imm == 0x1B || imm == 0x1D;
    if (!release) core_preempt_point(t_cur);
    svc_entry(c, imm, pc);
    if (release) core_preempt_point(t_cur);
    rt_unwind_point();
}

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc) {
    if (prof_svc < 0) prof_svc = ENV("SAVAGEWIND_PROF_SVC") != NULL;
    if (prof_svc && imm < 0x80) {
        uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
        uint32_t h0 = imm == 0x18 && a2 ? *(uint32_t *)(uintptr_t)a1 : 0;
        uint64_t t0 = plat_now_ns();
        svc_impl(c, imm, pc);
        uint64_t dt = plat_now_ns() - t0;
        static _Atomic int nslow;
        if (((t_cur && t_cur->id == 80 && (imm == 0x18 || imm == 0x34)) || (dt > 10000000 && imm != 0x0b && imm != 0x1c)) && nslow < 20000) {
            nslow++;
            LOG("[slow] t%llu svc%02x %llums a=%llx,%llx,%llx,%llx h0=%x lr=%llx\n", (unsigned long long)(t_cur ? t_cur->id : 0), imm,
                (unsigned long long)(dt / 1000000), (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, h0, (unsigned long long)lr);
        }
        unsigned tid = t_cur ? (unsigned)(t_cur->id & 31) : 31;
        svc_wait_ns[tid][imm] += dt;
        svc_calls[tid][imm]++;
        static _Atomic uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 2000000000ull) {
            last = now;
            for (int t = 0; t < 32; t++) {
                int any = 0;
                for (int i = 0; i < 0x80; i++)
                    if (svc_calls[t][i]) {
                        if (!any) { LOG("[svcprof] t%d", t); any = 1; }
                        LOG(" %02x:%llu/%llums", i, (unsigned long long)svc_calls[t][i], (unsigned long long)(svc_wait_ns[t][i] / 1000000));
                        svc_calls[t][i] = 0; svc_wait_ns[t][i] = 0;
                    }
                if (any) LOG("\n");
            }
        }
        return;
    }
    if (trace_svc < 0) trace_svc = getenv("SAVAGEWIND_TRACE_SVC") != NULL;
    if (!trace_svc) return svc_impl(c, imm, pc);
    uint64_t in[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    svc_impl(c, imm, pc);
    LOG("[svc] %02x(%llx, %llx, %llx, %llx) -> %llx %llx %llx  @%llx\n", imm, in[0], in[1], in[2], in[3], c->x[0],
        c->x[1], c->x[2], pc);
}

void backend_heap_set(uint64_t size);  // renderer: the game set its heap's size (runtime/nvn)

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x01: {  // SetHeapSize
        uint64_t size = x[1];
        LOG("[kernel] SetHeapSize request %llu MB (limit %llu MB)\n",
            (unsigned long long)(size >> 20), (unsigned long long)(HEAP_MAX >> 20));
        if (size > HEAP_MAX) {
            LOG("[kernel] SetHeapSize rejected\n");
            x[0] = R_OUT_OF_RESOURCE;
            break;
        }
        heap_size = size;
        backend_heap_set(size);  // (the renderer may back the heap with GPU memory: Linux, exec.c)
        region_remove(HEAP_BASE);
        if (size) {
            #ifdef _WIN32
            bool eager = !plat_guest_sparse_heap_enabled();

            if (plat_guest_sparse_heap_enabled())
                plat_guest_sparse_heap_range(HEAP_BASE, size);

            LOG("[kernel] guest heap backing: %s commit (sparse opt-in %s)\n",
                eager ? "eager" : "lazy",
                eager ? "off" : "on");
            #else
            bool eager = true;
            #endif
            region_add_impl(HEAP_BASE, size, 5, 3, eager);
        }
        LOG("[kernel] SetHeapSize accepted: %llu MB at %llx\n",
            (unsigned long long)(size >> 20), (unsigned long long)HEAP_BASE);
        x[0] = R_OK;
        x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;  // SetMemoryPermission / SetMemoryAttribute
    case 0x04:  // MapMemory(dst, src, size): src is inaccessible while mapped, so copying is equivalent
        kernel_watch_write("MapMemory", x[0], x[2]);
        memcpy((void *)x[0], (void *)x[1], x[2]);
        region_add(x[0], x[2], 0xB, 3);
        x[0] = R_OK;
        break;
    case 0x05:  // UnmapMemory(dst, src, size): copy back
        kernel_watch_write("UnmapMemory", x[1], x[2]);
        memcpy((void *)x[1], (void *)x[0], x[2]);
        region_remove(x[0]);
        x[0] = R_OK;
        break;
    case 0x06:  // QueryMemory(out_info, _, addr)
        query_memory(x[2], x[0]);
        x[0] = R_OK;
        x[1] = 0;
        break;
    case 0x07: LOG("ExitProcess\n"); exit(0);
    case 0x08: {  // CreateThread(_, entry, arg, stack_top, prio, core)
        KThread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        t->handle = kernel_new_handle(&t->obj);
        LOG("[kernel] CreateThread entry=%llx stack=%llx prio=%d core %d -> %x\n", x[1], x[3], (int)x[4], (int)(int32_t)x[5],
            t->handle);
        x[0] = R_OK;
        x[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) { x[0] = R_INVALID_HANDLE; break; }
        thread_start((KThread *)o);
        x[0] = R_OK;
        break;
    }
    case 0x0A: longjmp(t_cur->unwind, 1);  // ExitThread: back to thread_main, which signals the thread object
    case 0x0B: {  // SleepThread (in slices of at most 50 ms: a reboot doesn't wait out a long sleep)
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) { core_yield(t_cur); break; }
        core_release(t_cur);
        for (uint64_t end = plat_now_ns() + (uint64_t)ns, now; !g_rt_unwind && (now = plat_now_ns()) < end;) {
            uint64_t d = end - now < 50000000 ? end - now : 50000000;
            struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
            nanosleep(&ts, NULL);
        }
        core_acquire(t_cur);
        break;
    }
    case 0x0C: {  // GetThreadPriority(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->prio : 0;
        break;
    }
    case 0x0D: {  // SetThreadPriority
        KObject *o = handle_get((uint32_t)x[0]);
        if (o) {
            klock();
            ((KThread *)o)->base_prio = (int)x[1];
            prio_refresh((KThread *)o);
            pthread_mutex_unlock(&g_klock);
        }
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        break;
    }
    case 0x0E: {  // GetThreadCoreMask(_, _, handle)
        KObject *o = handle_get((uint32_t)x[2]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->core : 0;
        x[2] = 0xF;
        break;
    }
    case 0x0F: {  // SetThreadCoreMask(handle, ideal core, affinity mask)
        KObject *o = handle_get((uint32_t)x[0] == 0xFFFF8000u ? kernel_current_thread_handle() : (uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) { x[0] = R_INVALID_HANDLE; break; }
        int32_t ideal = (int32_t)x[1];
        uint64_t mask = x[2] & 0xF;
        int core = ideal >= 0 ? ideal : -1;
        if (ENV("SAVAGEWIND_LOG_CORES"))
            LOG("[kernel] SetThreadCoreMask %x: ideal %d mask %llx (prio %d)\n", ((KThread *)o)->handle, ideal,
                (unsigned long long)mask, ((KThread *)o)->prio);
        if (core < 0 && mask && !(mask & (mask - 1))) core = __builtin_ctzll(mask);  // pinned to one core
        if (core >= 0) core_move((KThread *)o, core);
        x[0] = R_OK;
        break;
    }
    case 0x10: x[0] = t_cur->core; break;  // GetCurrentProcessorNumber
    case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
        klock();
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o) x[0] = R_INVALID_HANDLE;
        else {
            KObject *ev = o->type == KOBJ_EVENT ? o : NULL;
            if (!ev) x[0] = R_INVALID_HANDLE;
            else {
                if (imm == 0x11) kernel_signal(ev);
                else if (imm == 0x17 && !ev->signaled) { x[0] = R_INVALID_STATE; pthread_mutex_unlock(&g_klock); break; }
                else ev->signaled = false;
                x[0] = R_OK;
            }
        }
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_SHMEM) { x[0] = R_INVALID_HANDLE; break; }
        KSharedMem *s = (KSharedMem *)o;
        kernel_watch_write("MapSharedMemory", x[1], x[2]);
        memset((void *)x[1], 0, x[2]);
        s->guest_addr = x[1];  // HLE services write straight into the guest's mapping
        region_add(x[1], x[2], 6, 3);
        if (s->on_map) s->on_map(s);
        x[0] = R_OK;
        break;
    }
    case 0x14: region_remove(x[1]); x[0] = R_OK; break;  // UnmapSharedMemory
    case 0x15: {  // CreateTransferMemory(_, addr, size, perm)
        KTransferMem *t = calloc(1, sizeof *t);
        t->obj.type = KOBJ_TMEM;
        t->addr = x[1];
        t->size = x[2];
        x[0] = R_OK;
        x[1] = kernel_new_handle(&t->obj);
        break;
    }
    case 0x16:  // CloseHandle
        klock();
        handle_close((uint32_t)x[0]);
        pthread_mutex_unlock(&g_klock);
        x[0] = R_OK;
        break;
    case 0x18: {  // WaitSynchronization(_, handles, count, timeout)
        int32_t idx = -1;
        x[0] = svc_wait_synchronization(x[1], (int32_t)x[2], (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {  // CancelSynchronization
        klock();
        KObject *o = handle_get((uint32_t)x[0]);
        if (o && o->type == KOBJ_THREAD) {
            ((KThread *)o)->cancel = true;
            wake((KThread *)o);
            x[0] = R_OK;
        } else x[0] = R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x1A: x[0] = svc_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;
    case 0x1B: x[0] = svc_arbitrate_unlock(x[0]); break;
    case 0x1C: x[0] = svc_wait_process_wide_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;
    case 0x1D: svc_signal_process_wide_key(x[0], (int32_t)x[1]); x[0] = R_OK; break;
    case 0x1E: x[0] = rt_ticks(); break;
    case 0x1F: {  // ConnectToNamedPort(_, name)
        uint32_t h = ipc_connect_named_port((const char *)(uintptr_t)x[1]);
        x[0] = h ? R_OK : R_NOT_FOUND;
        x[1] = h;
        break;
    }
    case 0x21: x[0] = ipc_handle_request(c, (uint32_t)x[0]); break;  // SendSyncRequest
    case 0x22: {  // SendSyncRequestWithUserBuffer(buf, size, handle)
        uint64_t saved = c->tpidrro;
        c->tpidrro = x[0];
        x[0] = ipc_handle_request(c, (uint32_t)x[2]);
        c->tpidrro = saved;
        break;
    }
    case 0x24: x[0] = R_OK; x[1] = 0x81; break;  // GetProcessId
    case 0x25: {  // GetThreadId(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? ((KThread *)o)->id : 0;
        break;
    }
    case 0x26:  // Break
        if (x[0] & 0x80000000u) { LOG("[kernel] Break notification %llx\n", x[0]); x[0] = R_OK; break; }
        FATAL("svcBreak(reason=%llx, info=%llx, size=%llx) at %llx\n", x[0], x[1], x[2], pc);
    case 0x27: debug_string(x[0], x[1]); x[0] = R_OK; break;
    case 0x29: {  // GetInfo(_, id0, handle, id1)
        uint32_t id0 = (uint32_t)x[1];
        uint64_t v = 0;
        uint32_t r = R_OK;
        switch (id0) {
        case 0: v = 0xF; break;
        case 1: v = 0xFFFFFFFFFFFFFFFFull; break;
        case 2: v = ALIAS_BASE; break;
        case 3: v = ALIAS_SIZE; break;
        case 4: v = HEAP_BASE; break;
        case 5: v = HEAP_MAX; break;
        case 6: case 21: v = 0xCD500000ull; break;  // application memory (3.2 GB, as on hardware)
        case 7: case 22: v = heap_size + 0x4000000; break;
        case 8: v = 0; break;
        case 9: v = 0; break;
        case 11: v = ((uint64_t)arc4random() << 32) | arc4random(); break;
        case 12: v = GUEST_LO; break;
        case 13: v = GUEST_HI - GUEST_LO; break;
        case 14: v = STACK_BASE; break;
        case 15: v = STACK_SIZE; break;
        case 16: case 17: v = 0; break;
        case 18: v = g_program_id; break;  // ProgramId (main.npdm)
        // UserExceptionContextAddress: a zeroed page at the top of the TLS region. BOTW 1.8.2's
        // nn::os::SetDyingMessageRegion stores its dying-message pointer at +0x1c0 there.
        case 20: v = TLS_BASE + TLS_SIZE - 0x1000; break;
        case 28: v = 0; break;  // AliasRegionExtraSize (HOS 18+; BOTW 1.8.2's SDK asks): none
        default: r = R_INVALID_ENUM; LOG("[kernel] GetInfo unknown id %u/%llu\n", id0, x[3]);
        }
        x[0] = r;
        x[1] = v;
        break;
    }
    case 0x2C:  // MapPhysicalMemory(addr, size)
        LOG("[kernel] MapPhysicalMemory %llx+%llx\n", (unsigned long long)x[0], (unsigned long long)x[1]);
        memset((void *)x[0], 0, x[1]);
        region_add(x[0], x[1], 0x7, 3);
        x[0] = R_OK;
        break;
    case 0x2D:  // UnmapPhysicalMemory
        region_remove(x[0]);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = svc_wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]); break;
    case 0x35: x[0] = svc_signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {  // CreateEvent -> w1 writable, w2 readable
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        x[0] = R_OK;
        x[1] = kernel_new_handle(ev);
        x[2] = kernel_new_handle(ev);
        if (ENV("SAVAGEWIND_PROF_SVC")) LOG("[evt] guest w=%llx r=%llx lr=%llx\n", (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[30]);
        break;
    }
    default:
        FATAL("unimplemented svc 0x%x at %llx (x0=%llx x1=%llx x2=%llx)\n", imm, pc, x[0], x[1], x[2]);
    }
}

// Reboot, step 1: stop every guest thread (see rt_unwind_point) and join it. `idle` keeps the window responsive.
void kernel_stop_threads(void (*idle)(void)) {
    g_rt_unwind = 1;
    uint64_t t0 = plat_now_ns(), logged = t0;
    for (;;) {
        int live = 0;
        klock();
        for (KThread *t = g_threads; t; t = t->anext)
            if (t->started && !t->finished) { live++; wake(t); }
        pthread_cond_broadcast(&g_nothread_cond);
        pthread_mutex_unlock(&g_klock);
        for (int i = 0; i < 4; i++) {  // threads waiting for a core run on to their next safe point
            pthread_mutex_lock(&g_cores[i].m);
            pthread_cond_broadcast(&g_cores[i].cv);
            pthread_mutex_unlock(&g_cores[i].m);
        }
        if (!live) break;
        uint64_t now = plat_now_ns();
        if (now - logged > 3000000000ull) {  // (a thread that never reaches a safe point: who and where)
            logged = now;
            LOG("[reboot] still waiting for %d guest threads after %llums:\n", live, (unsigned long long)((now - t0) / 1000000));
            for (KThread *t = g_threads; t; t = t->anext)
                if (t->started && !t->finished)
                    LOG("[reboot]   thread %llu (entry %llx) pc %llx lr %llx wait %d sync %d\n", (unsigned long long)t->id,
                        (unsigned long long)t->entry, (unsigned long long)t->cpu.pc, (unsigned long long)t->cpu.x[30],
                        t->wait_kind, t->in_sync_wait);
        }
        if (idle) idle();
        usleep(2000);
    }
    int n = 0;
    for (KThread *t = g_threads; t; t = t->anext)
        if (t->started) { pthread_join(t->host, NULL); n++; }
    LOG("[reboot] %d guest threads stopped in %llums\n", n, (unsigned long long)((plat_now_ns() - t0) / 1000000));
}

// Reboot, step 2 (threads stopped): a fresh kernel. Kernel objects are leaked rather than freed: host statics
// can still point at some (vi.c's vsync event, signalled by its host timer thread).
void kernel_reset(void) {
    klock();
    for (KThread *t = g_threads, *next; t; t = next) {
        next = t->anext;
        pthread_cond_destroy(&t->cond);
        free(t);
    }
    g_threads = NULL;
    g_waitlist = NULL;
    g_syncwaiters = NULL;
    memset(handles, 0, sizeof handles);
    nregions = 0;
    heap_size = 0;
    tls_next = TLS_BASE;
    next_thread_id = 0x50;
    for (int i = 0; i < 4; i++) {
        g_cores[i].owner = NULL;
        g_cores[i].nwaiting = 0;
    }
    g_rt_unwind = 0;
    pthread_mutex_unlock(&g_klock);
}

// (debug) SAVAGEWIND_THREAD_DUMP_AT=<seconds>[,<seconds>...]: at those times after start-up, print every guest
// thread's wait state and its guest call chain, walked through the AArch64 frame records (x29 -> {fp, lr}) in
// guest memory. For hangs: map the return addresses to functions with tools/ghidra/gq.py.
static bool guest_mapped(uint64_t a, uint64_t n) {
    for (int i = 0; i < nregions; i++)
        if (a >= regions[i].addr && a + n <= regions[i].addr + regions[i].size) return true;
    return false;
}

// The current guest thread's lr and frame-record chain as of its last call boundary (dispatch.c, SAVAGEWIND_WATCH:
// who called the memcpy that wrote a watched range).
void kernel_log_guest_chain(void) {
    KThread *t = t_cur;
    if (!t) return;
    fprintf(stderr, "[watch]   guest lr %llx chain", (unsigned long long)t->cpu.x[30]);
    uint64_t fp = t->cpu.x[29];
    for (int i = 0; i < 10 && fp && !(fp & 7) && guest_mapped(fp, 16); i++) {
        const uint64_t *rec = (const uint64_t *)(uintptr_t)fp;
        if (!rec[1]) break;
        fprintf(stderr, " %llx", (unsigned long long)rec[1]);
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    fprintf(stderr, "\n");
}

static void dump_threads(void) {
    static const char *const kinds[] = {"-", "mutex", "condvar", "arbiter"};
    klock();
    for (KThread *t = g_threads; t; t = t->anext) {
        LOG("[threads] handle %x id %llx prio %d core %d %s wait %s %llx%s lr %llx\n", t->handle,
            (unsigned long long)t->id, t->prio, t->core, t->finished ? "finished" : t->started ? "running" : "created",
            kinds[t->wait_kind & 3], (unsigned long long)t->wait_addr, t->in_sync_wait ? " (WaitSynchronization)" : "",
            (unsigned long long)t->cpu.x[30]);
        if (t->wait_kind == WAIT_MUTEX || t->wait_kind == WAIT_CONDVAR) {  // the mutex word: its owner's handle
            uint64_t m = t->wait_kind == WAIT_MUTEX ? t->wait_addr : t->wait_mutex;
            LOG("[threads]   mutex %llx = %x\n", (unsigned long long)m, guest_mapped(m, 4) ? *u32p(m) : 0);
        }
        if (t->started && !t->finished && t->wait_kind == WAIT_NONE && !t->in_sync_wait) {
            // not in a kernel wait: the registers as of the last call boundary (a leaf loop keeps them in host registers)
            LOG("[threads]   regs");
            for (int r = 0; r < 31; r++) LOG(" x%d=%llx", r, (unsigned long long)t->cpu.x[r]);
            LOG("\n");
        }
        LOG("[threads]   chain");
        uint64_t fp = t->cpu.x[29];
        for (int i = 0; i < 24 && fp && !(fp & 7) && guest_mapped(fp, 16); i++) {
            const uint64_t *rec = (const uint64_t *)(uintptr_t)fp;
            if (!rec[1]) break;
            LOG(" %llx", (unsigned long long)rec[1]);
            if (rec[0] <= fp) break;  // frame records move up the stack
            fp = rec[0];
        }
        LOG("\n");
    }
    pthread_mutex_unlock(&g_klock);
}

static void *thread_dump_timer(void *arg) {
    const char *p = arg;
    double start = plat_now_ns() / 1e9;
    while (*p) {
        double at = strtod(p, (char **)&p);
        while (*p == ',') p++;
        for (double now; (now = plat_now_ns() / 1e9 - start) < at;) {
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
        LOG("[threads] --- %.0f s\n", at);
        dump_threads();
    }
    return NULL;
}

// (debug) SAVAGEWIND_WATCH=<addr>:<len> (hex, len up to 64): report when those guest bytes change, polled every
// millisecond, and when an HLE copy (kernel_watch_write) writes over them. For memory corruption: a change with
// no HLE report next to it came from guest code; SAVAGEWIND_TRACE_SVC around it shows what the guest was doing.
// SAVAGEWIND_WATCH_DEREF=<off>:<len> (hex) also watches len bytes at the pointer stored at watch_addr + off (for a
// list head: the first node), following it whenever that pointer changes.
static uint64_t watch_addr, watch_len, deref_off, deref_len;
static _Atomic uint64_t deref_addr;

void kernel_watch_write(const char *what, uint64_t addr, uint64_t len) {
    uint64_t d = deref_addr;
    if ((watch_len && addr < watch_addr + watch_len && watch_addr < addr + len) ||
        (d && deref_len && addr < d + deref_len && d < addr + len))
        LOG("[watch] %s writes %llx+%llx over a watched range (thread %x)\n", what, (unsigned long long)addr,
            (unsigned long long)len, t_cur ? t_cur->handle : 0);
}

// Logs the bytes at [addr, addr+len) when they differ from prev (len up to 64); returns whether they did.
static bool watch_sample(const char *tag, uint64_t addr, uint64_t len, uint8_t *prev, bool *have, double t) {
    uint8_t cur[64];
    if (!guest_mapped(addr, len)) return false;
    memcpy(cur, (const void *)(uintptr_t)addr, len);
    if (*have && !memcmp(cur, prev, len)) return false;
    LOG("[watch] %.3f s %s %llx:", t, tag, (unsigned long long)addr);
    for (uint64_t i = 0; i + 8 <= len; i += 8) LOG(" %016llx", (unsigned long long)*(uint64_t *)(cur + i));
    LOG("\n");
    memcpy(prev, cur, len);
    *have = true;
    return true;
}

static void *watch_poll(void *arg) {
    (void)arg;
    uint8_t prev[64], dprev[64];
    bool have = false, dhave = false;
    double start = plat_now_ns() / 1e9;
    for (;;) {
        double t = plat_now_ns() / 1e9 - start;
        static bool head_trapped;  // SAVAGEWIND_WATCH_TRAP without a DEREF: the page trap sits on the head itself
        if (!head_trapped && !deref_len && getenv("SAVAGEWIND_WATCH_TRAP") && guest_mapped(watch_addr, watch_len)) {
            extern void watch_retarget(uint64_t lo, uint64_t hi);  // runtime/dispatch.c
            head_trapped = true;
            watch_retarget(watch_addr, watch_addr + watch_len);
        }
        watch_sample("head", watch_addr, watch_len, prev, &have, t);
        if (deref_len && guest_mapped(watch_addr + deref_off, 8)) {
            uint64_t d = *(const uint64_t *)(uintptr_t)(watch_addr + deref_off);
            static int trap = -1;  // SAVAGEWIND_WATCH_TRAP=1: name the functions touching the node's links instead
            if (trap < 0) trap = getenv("SAVAGEWIND_WATCH_TRAP") != NULL;
            if (d != deref_addr) {
                deref_addr = d;
                dhave = false;
                extern void watch_retarget(uint64_t lo, uint64_t hi);  // runtime/dispatch.c
                if (trap && d && d != watch_addr) watch_retarget(d, d + 16);
            }
            if (d && !trap) watch_sample("node", d, deref_len, dprev, &dhave, t);
        }
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void kernel_init(void) {
    static bool watch_started;
    const char *w = getenv("SAVAGEWIND_WATCH");
    if (!watch_started && w) {
        watch_started = true;
        watch_addr = strtoull(w, (char **)&w, 16);
        watch_len = *w == ':' ? strtoull(w + 1, NULL, 16) : 8;
        if (watch_len > 64) watch_len = 64;
        const char *dw = getenv("SAVAGEWIND_WATCH_DEREF");
        if (dw) {
            deref_off = strtoull(dw, (char **)&dw, 16);
            deref_len = *dw == ':' ? strtoull(dw + 1, NULL, 16) : 0x20;
            if (deref_len > 64) deref_len = 64;
        }
        LOG("[watch] %llx+%llx (and the %llx bytes at [+%llx])\n", (unsigned long long)watch_addr,
            (unsigned long long)watch_len, (unsigned long long)deref_len, (unsigned long long)deref_off);
        pthread_t th;
        pthread_create(&th, NULL, watch_poll, NULL);
    }
    static bool dump_started;
    if (!dump_started && getenv("SAVAGEWIND_THREAD_DUMP_AT")) {
        dump_started = true;
        pthread_t th;
        pthread_create(&th, NULL, thread_dump_timer, (void *)getenv("SAVAGEWIND_THREAD_DUMP_AT"));
    }
    // Reserve the heap, alias, stack, and TLS regions.
    region_add(TLS_BASE, TLS_SIZE, 0xC, 3);
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        region_add(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        region_add(m->base + m->data_off, m->size - m->data_off, 4, 3);
    }
}

KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    KObject *ev = calloc(1, sizeof *ev);
    ev->type = KOBJ_EVENT;
    *w = kernel_new_handle(ev);
    *r = kernel_new_handle(ev);
    if (ENV("SAVAGEWIND_PROF_SVC")) { const char *s = plat_symbol((uint64_t)(uintptr_t)__builtin_return_address(0), NULL); LOG("[evt] hle w=%x r=%x from %s\n", *w, *r, s ? s : "?"); }
    return ev;
}

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    KSharedMem *s = calloc(1, sizeof *s);
    s->obj.type = KOBJ_SHMEM;
    s->size = size;
    s->on_map = on_map;
    *handle = kernel_new_handle(&s->obj);
    return s;
}
