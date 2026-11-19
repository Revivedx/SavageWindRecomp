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
#include "cpu.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"

#define RUPEE_POLL_MS 5000         // (a scan of the heap is long: no one needs the top-up sooner than this)
#define RUPEE_MAX 9999             // (the Infinite rupees top-up target)
#define RUPEE_SET_MAX 999999       // (the game's own wallet limit)
#define RUPEE_HASH 0x23149bf8u     // crc32("CurrentRupee")
#define S32_HASH 0x2906f327u       // crc32("MaxHartValue")
#define HART_CUR_HASH 0xbe9bc993u  // crc32("CurrentHart")
#define HEARTS_MIN 3
#define HEARTS_MAX 30

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool running, stop_req, poked;  // (poked: a command wants the thread now, not at the next poll)
static int32_t want = -1;              // `rupees <n>`: the count to write once (-1: none); under lock
static int32_t last_seen = -1;         // the count the thread last read (-1: not found yet), for `rupees`
static int32_t want_hearts = -1;       // `hearts <3-30>`: heart containers to write (-1: none); under lock
static int32_t want_hearts_tick = -1;  // live hearts update for game thread (Cpu *c in rt_item_tick)
static int32_t last_hearts = -1;       // heart containers last read (-1: not found yet)
static uint64_t vt_s32, rupee_obj, hart_max_obj, hart_cur_obj;

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
static uint64_t find_flag(uint32_t hash) {
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size();
    if (hi <= lo) return 0;
    if (!vt_s32) vt_s32 = s32_class(lo, hi, S32_HASH);
    if (!vt_s32) return 0;
    uint64_t a, e;
    for (uint64_t at = lo; !stopping() && plat_committed_range(at, hi, &a, &e); at = e)
        for (uint64_t p = (a + 7) & ~7ull; p + 0x28 <= e; p += 8)
            if (rd64(p) == vt_s32 && rd32(p + 0x1c) == hash) return p;
    return 0;
}

static bool still_flag(uint64_t obj, uint32_t hash) {
    uint64_t a, e;
    return obj && plat_committed_range(obj, obj + 0x28, &a, &e) && a == obj && e == obj + 0x28 &&
           rd64(obj) == vt_s32 && rd32(obj + 0x1c) == hash;
}

static void *rupee_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!stop_req) {
        int32_t set = want;
        int32_t set_h = want_hearts;
        pthread_mutex_unlock(&lock);
        bool look_rupees = (g_settings.infinite_rupees || set >= 0) && kernel_heap_size();
        bool look_hearts = (set_h >= HEARTS_MIN) && kernel_heap_size();
        if (look_rupees && !still_flag(rupee_obj, RUPEE_HASH)) {
            rupee_obj = find_flag(RUPEE_HASH);
            if (rupee_obj) LOG("[cheats] rupees: the rupee count is at %llx\n", (unsigned long long)rupee_obj);
        }
        if (look_rupees && rupee_obj) {
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
        if (look_hearts) {
            if (!still_flag(hart_max_obj, S32_HASH)) hart_max_obj = find_flag(S32_HASH);
            if (!still_flag(hart_cur_obj, HART_CUR_HASH)) hart_cur_obj = find_flag(HART_CUR_HASH);
            uint32_t units = (uint32_t)(set_h * 4);
            if (hart_max_obj) *(volatile uint32_t *)(uintptr_t)(hart_max_obj + 0x18) = units;
            if (hart_cur_obj) *(volatile uint32_t *)(uintptr_t)(hart_cur_obj + 0x18) = units;
            uint64_t pinfo = g_site.player_info ? *(volatile uint64_t *)(uintptr_t)g_site.player_info : 0;
            if (pinfo) *(volatile float *)(uintptr_t)(pinfo + 0xa0) = (float)units;
            if (hart_max_obj || hart_cur_obj || pinfo) {
                LOG("[cheats] hearts: set to %d containers (%u units) [max_obj=%llx cur_obj=%llx pinfo=%llx]\n",
                    (int)set_h, (unsigned)units, (unsigned long long)hart_max_obj, (unsigned long long)hart_cur_obj, (unsigned long long)pinfo);
                __atomic_store_n(&last_hearts, set_h, __ATOMIC_RELAXED);
            }
        }
        pthread_mutex_lock(&lock);
        if (set >= 0 && want == set && rupee_obj) want = -1;  // (written; not found yet: tried again at the next poll)
        if (set_h >= HEARTS_MIN && want_hearts == set_h && (hart_max_obj || hart_cur_obj || g_site.player_info)) want_hearts = -1;
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

// rupees: show | <0-999999>: set once | lock: keep at 9999 (Infinite rupees) | unlock
static void cmd_rupees(int argc, char **argv, ControlReply *r) {
    if (!running || !kernel_heap_size()) { control_fail(r, "The game isn't running yet"); return; }
    if (argc > 2) { control_fail(r, "rupees: use rupees, rupees <0-%d>, rupees lock or rupees unlock", RUPEE_SET_MAX); return; }
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
        if (end == argv[1] || *end || n < 0 || n > RUPEE_SET_MAX) {
            control_fail(r, "rupees: a count from 0 to %d, lock or unlock", RUPEE_SET_MAX);
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

// hearts: show | <3-30>: set heart containers count once
static void cmd_hearts(int argc, char **argv, ControlReply *r) {
    if (!running || !kernel_heap_size()) { control_fail(r, "The game isn't running yet"); return; }
    if (argc > 2) { control_fail(r, "hearts: use hearts or hearts <%d-%d>", HEARTS_MIN, HEARTS_MAX); return; }
    if (argc == 2) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end || n < HEARTS_MIN || n > HEARTS_MAX) {
            control_fail(r, "hearts: a count from %d to %d", HEARTS_MIN, HEARTS_MAX);
            return;
        }
        pthread_mutex_lock(&lock);
        want_hearts = (int32_t)n;
        pthread_mutex_unlock(&lock);
        __atomic_store_n(&want_hearts_tick, (int32_t)n, __ATOMIC_RELAXED);
        poke();
        control_field_int(r, "set", n);
        control_text(r, "Setting heart containers to %ld (the first time, finding it takes a few seconds)", n);
        return;
    }
    int32_t seen = __atomic_load_n(&last_hearts, __ATOMIC_RELAXED);
    if (seen >= 0) control_field_int(r, "hearts", seen);
    if (seen >= 0) control_text(r, "Hearts: %d containers", (int)seen);
    else control_text(r, "Hearts: not read yet (hearts <%d-%d> finds it)", HEARTS_MIN, HEARTS_MAX);
}

// (runtime/control_game.c: developer mode only - it writes the game's memory)
void rupees_control_init(void) {
    static const ControlCmd cmds[] = {
        {"rupees", "[<0-999999> | lock | unlock]", "Cheat: show, set or lock the rupee count", cmd_rupees, 0},
        {"hearts", "[<3-30>]", "Cheat: show or set the heart containers count", cmd_hearts, 0},
    };
    control_register(cmds, 2);
}

// Set the rupee wallet once from the UI (false if the game isn't running or count is out of range)
bool rupees_set(int32_t count) {
    if (!running || !kernel_heap_size() || count < 0 || count > RUPEE_SET_MAX) return false;
    pthread_mutex_lock(&lock);
    want = count;
    pthread_mutex_unlock(&lock);
    poke();
    return true;
}

// Set heart containers once from the UI (3 to 30; false if not running or count out of range)
bool hearts_set(int32_t count) {
    if (!running || !kernel_heap_size() || count < HEARTS_MIN || count > HEARTS_MAX) return false;
    pthread_mutex_lock(&lock);
    want_hearts = count;
    pthread_mutex_unlock(&lock);
    __atomic_store_n(&want_hearts_tick, count, __ATOMIC_RELAXED);
    poke();
    return true;
}

// Live hearts update on game thread (every frame in play from rt_item_tick):
// sets Link's live current life and PlayerInfo->max_hearts immediately so the in-game HUD reflects the cheat
void rt_hearts_tick(Cpu *c) {
    int32_t h = __atomic_load_n(&want_hearts_tick, __ATOMIC_RELAXED);
    if (h < HEARTS_MIN || h > HEARTS_MAX) return;

    uint64_t pinfo = g_site.player_info ? *(volatile uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (pinfo) {
        *(volatile float *)(uintptr_t)(pinfo + 0xa0) = (float)(h * 4);
    }
    if (c && c->x[1]) {
        *(volatile int32_t *)(uintptr_t)c->x[1] = h * 4;
        __atomic_store_n(&want_hearts_tick, -1, __ATOMIC_RELAXED);
        LOG("[cheats] hearts: applied %d containers (%d units) to live actor\n", (int)h, (int)(h * 4));
    }
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
    vt_s32 = rupee_obj = hart_max_obj = hart_cur_obj = 0;
    want = last_seen = want_hearts = last_hearts = want_hearts_tick = -1;
}
