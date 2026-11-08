// Cheats: Infinite rupees (the wallet kept at 9999, from the Savage Wind overlay's Cheats tab or `rupees lock`) and
// `rupees <n>` (the wallet set once). No patch in the game code: a thread of ours finds the rupee count's game data
// flag in the game's memory and writes it: a set at once, the top-up every RUPEE_POLL_MS while the cheat is on.
//
// The game keeps each game data flag as an object on its heap (ksys::gdt, made once as it starts): an s32 flag is
// 0x28 bytes - +0 its class (vtable, in main), +0x18 its value, +0x1c the crc32 of its name (the save's hash). The
// s32 class is found from a flag every version has (MaxHartValue), then the CurrentRupee object among the objects of
// that class. Both are looked for only while there is something to write, and the object is checked again before
// each write (a new game, or a load, may lay the heap out again). From Slop of the Wild, where the same reader also
// fed its achievements.
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "control.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"

#define RUPEE_POLL_MS 5000         // (a scan of the heap is long: no one needs the top-up sooner than this)
#define RUPEE_MAX 9999             // (the wallet's own limit)
#define RUPEE_HASH 0x23149bf8u     // crc32("CurrentRupee")
#define S32_HASH 0x2906f327u       // crc32("MaxHartValue")

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool running, stop_req, poked;  // (poked: a command wants the thread now, not at the next poll)
static int32_t want = -1;              // `rupees <n>`: the count to write once (-1: none); under lock
static int32_t last_seen = -1;         // the count the thread last read (-1: not found yet), for `rupees`
static uint64_t vt_s32, rupee_obj;

static uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static bool stopping(void) { return __atomic_load_n(&stop_req, __ATOMIC_RELAXED); }

static bool in_main(uint64_t v) { return v >= 0x7000000000ull && g_site.main_end && v < g_site.main_end && !(v & 7); }

// The class of the s32 flag named by `hash` (its object: the hash at +0x1c, the class at +0)
static uint64_t s32_class(uint64_t lo, uint64_t hi, uint32_t hash) {
    uint64_t a, e;
    for (uint64_t at = lo; !stopping() && plat_committed_range(at, hi, &a, &e); at = e)  // (only memory that is there)
        for (uint64_t p = ((a + 0x1c + 7) & ~7ull) - 4; p + 4 <= e; p += 8) {  // (p - 0x1c 8-aligned)
            if (rd32(p) != hash) continue;
            uint64_t vt = rd64(p - 0x1c);
            if (in_main(vt)) return vt;
        }
    return 0;
}

// The CurrentRupee flag object, 0: not found (yet)
static uint64_t find_rupees(void) {
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size();
    if (hi <= lo) return 0;
    if (!vt_s32) vt_s32 = s32_class(lo, hi, S32_HASH);
    if (!vt_s32) return 0;
    uint64_t a, e;
    for (uint64_t at = lo; !stopping() && plat_committed_range(at, hi, &a, &e); at = e)
        for (uint64_t p = (a + 7) & ~7ull; p + 0x28 <= e; p += 8)
            if (rd64(p) == vt_s32 && rd32(p + 0x1c) == RUPEE_HASH) return p;
    return 0;
}

static bool still_rupees(uint64_t obj) {
    uint64_t a, e;
    return obj && plat_committed_range(obj, obj + 0x28, &a, &e) && a == obj && e == obj + 0x28 &&
           rd64(obj) == vt_s32 && rd32(obj + 0x1c) == RUPEE_HASH;
}

static void *rupee_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!stop_req) {
        int32_t set = want;
        pthread_mutex_unlock(&lock);
        bool look = (g_settings.infinite_rupees || set >= 0) && kernel_heap_size();
        if (look && !still_rupees(rupee_obj)) {
            rupee_obj = find_rupees();
            if (rupee_obj) LOG("[cheats] rupees: the rupee count is at %llx\n", (unsigned long long)rupee_obj);
        }
        if (look && rupee_obj) {
            if (set >= 0) {
                *(volatile uint32_t *)(uintptr_t)(rupee_obj + 0x18) = (uint32_t)set;
                LOG("[cheats] rupees: wallet set to %d\n", (int)set);
            }
            if (g_settings.infinite_rupees && (int32_t)rd32(rupee_obj + 0x18) < RUPEE_MAX) {
                *(volatile uint32_t *)(uintptr_t)(rupee_obj + 0x18) = RUPEE_MAX;
                static bool said;
                if (!said) said = true, LOG("[cheats] infinite rupees: wallet filled\n");
            }
            __atomic_store_n(&last_seen, (int32_t)rd32(rupee_obj + 0x18), __ATOMIC_RELAXED);
        }
        pthread_mutex_lock(&lock);
        if (set >= 0 && want == set && rupee_obj) want = -1;  // (written; not found yet: tried again at the next poll)
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        long long ns = until.tv_nsec + RUPEE_POLL_MS * 1000000LL;
        until.tv_sec += (time_t)(ns / 1000000000LL), until.tv_nsec = (long)(ns % 1000000000LL);
        while (!stop_req && !poked && pthread_cond_timedwait(&wake, &lock, &until) != ETIMEDOUT) {}
        poked = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void poke(void) {
    pthread_mutex_lock(&lock);
    poked = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

// rupees: show | <0-9999>: set once | lock: keep at 9999 (Infinite rupees) | unlock
static void cmd_rupees(int argc, char **argv, ControlReply *r) {
    if (!running || !kernel_heap_size()) { control_fail(r, "The game isn't running yet"); return; }
    if (argc > 2) { control_fail(r, "rupees: use rupees, rupees <0-%d>, rupees lock or rupees unlock", RUPEE_MAX); return; }
    if (argc == 2 && (!strcmp(argv[1], "lock") || !strcmp(argv[1], "unlock"))) {
        g_settings.infinite_rupees = argv[1][0] == 'l';
        poke();
        control_field_bool(r, "lock", g_settings.infinite_rupees);
        if (g_settings.infinite_rupees) control_text(r, "Infinite rupees on: the wallet is kept at %d", RUPEE_MAX);
        else control_text(r, "Infinite rupees off");
        return;
    }
    if (argc == 2) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end || n < 0 || n > RUPEE_MAX) {
            control_fail(r, "rupees: a count from 0 to %d, lock or unlock", RUPEE_MAX);
            return;
        }
        pthread_mutex_lock(&lock);
        want = (int32_t)n;
        pthread_mutex_unlock(&lock);
        poke();
        control_field_int(r, "set", n);
        control_text(r, "Setting the wallet to %ld rupees (the first time, finding it takes a few seconds)", n);
        if (g_settings.infinite_rupees) control_text(r, "Infinite rupees is on: it tops the wallet up to %d again", RUPEE_MAX);
        return;
    }
    int32_t seen = __atomic_load_n(&last_seen, __ATOMIC_RELAXED);
    control_field_bool(r, "lock", g_settings.infinite_rupees);
    if (seen >= 0) control_field_int(r, "rupees", seen);
    control_text(r, "Infinite rupees: %s", g_settings.infinite_rupees ? "on" : "off");
    if (seen >= 0) control_text(r, "Wallet: %d rupees", (int)seen);
    else control_text(r, "Wallet: not read yet (rupees <n> or rupees lock finds it)");
}

// (runtime/control_game.c: developer mode only - it writes the game's memory)
void rupees_control_init(void) {
    static const ControlCmd cmds[] = {
        {"rupees", "[<0-9999> | lock | unlock]", "Cheat: show, set or lock the rupee count", cmd_rupees, 0},
    };
    control_register(cmds, 1);
}

// (runtime/main.c boot: each game start)
void rupees_start(void) {
    if (running) return;
    stop_req = poked = false;
    if (pthread_create(&thread, NULL, rupee_thread, NULL) == 0) running = true;
}

// Reboot: the thread is gone before the guest memory is reset under it; the next boot starts it again
RT_ON_RESET(rupees_reset) {
    if (running) {
        pthread_mutex_lock(&lock);
        __atomic_store_n(&stop_req, true, __ATOMIC_RELAXED);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        pthread_join(thread, NULL);
        running = false;
    }
    vt_s32 = rupee_obj = 0;
    want = last_seen = -1;
}
