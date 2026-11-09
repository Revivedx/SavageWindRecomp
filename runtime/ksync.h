// Locks and condition variables for the guest kernel layer (kernel.c's g_klock, the per-thread wait conditions and the
// core tokens; nvn/cmdbuf.c's shadow lock).
//
// A winpthreads mutex or condition costs a kernel call each time it sleeps or wakes someone (a semaphore or event), and
// a frame of the game's main thread has ~30 hand-offs between threads. These are futex-style instead: a word in
// memory, a kernel call only when a thread really has to sleep or to be woken, and a short spin before sleeping (the
// other side usually answers within ~20 us). The sleep is the OS's address wait (Windows 8+ WaitOnAddress, Linux
// futex). Where there is none (macOS, Windows 7) or SAVAGEWIND_KSYNC=0, every call goes to the pthread members
// instead, as before.
//   SAVAGEWIND_KSYNC_SPIN=<us>  how long a waiting thread spins before it sleeps (default 20, 0: never; and 0 on
//                               machines with fewer than 6 logical processors)
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t pm;  // (the fallback)
    uint32_t s;          // 0 free, 1 held, 2 held with sleepers
} KMutex;
typedef struct {
    pthread_cond_t pc;  // (the fallback)
    uint32_t seq;       // changed by every signal
    uint32_t parked;    // threads asleep (or about to be) on seq
} KCond;

#define KMUTEX_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
#define KCOND_INIT {PTHREAD_COND_INITIALIZER, 0, 0}

void kmutex_init(KMutex *m);
void kmutex_lock(KMutex *m);
bool kmutex_trylock(KMutex *m);
void kmutex_unlock(KMutex *m);
void kcond_init(KCond *c);
void kcond_destroy(KCond *c);
void kcond_signal(KCond *c);
void kcond_broadcast(KCond *c);
// Wait on c with m held; m is let go while waiting and held again on return. May return early without a signal (as
// pthread_cond_wait). The timed one takes an absolute CLOCK_REALTIME time; false if it ran out.
void kcond_wait(KCond *c, KMutex *m);
bool kcond_timedwait(KCond *c, KMutex *m, const struct timespec *abs);
