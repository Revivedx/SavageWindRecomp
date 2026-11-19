// Cheats > Collectible hints on the map (collectibles.h): the Korok seeds and shrines found in each tower region, and
// which towers are activated. No patch in the game code: as in runtime/rupees.c, a thread of ours finds the game data
// flags among the game's heap objects and reads them, only while the map or the `collectibles` command asks for them
// (collectibles_poll), every COLLECT_POLL_MS.
//
// The flags, by region: runtime/collectibles_table.inc (tools/gen_collectibles.py, from the player's own game files). A
// Korok sets its own bool flag when found; a shrine has Location_DungeonNNN (an s32, not 0 once found) and
// Clear_DungeonNNN (a bool); a tower sets MapTower_NN (a bool) once activated. Each flag is an object on the game's heap
// that starts with its class (vtable, in main) and keeps the crc32 of its name (layout below, as seen in the game's
// memory). One pass over the heap finds the objects by their hash; the class most of a kind's hits share is that kind's,
// so a stray match is dropped. The reading is checked against the game's own count of the Koroks found
// (HiddenKorok_Number, an s32): nothing is shown when they disagree.
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "collectibles.h"
#include "control.h"
#include "game_place.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "platform.h"
#include "rt.h"

typedef struct { uint8_t region; uint32_t found; float x, z; } CollectKorok;
typedef struct { uint8_t region; uint32_t found, cleared; float x, z; } CollectShrine;
#if __has_include("collectibles_table.inc")
#include "collectibles_table.inc"
#define HAVE_TABLE 1
#else  // (no game files: nothing to count)
#define COLLECT_REGIONS 0
#define COLLECT_KOROKS_FOUND 0u
static const CollectKorok korok_table[1];
static const CollectShrine shrine_table[1];
static const uint32_t tower_table[1];
#define HAVE_TABLE 0
#endif

#define COUNT(a) ((int)(sizeof(a) / sizeof *(a)))
#define COLLECT_POLL_MS 1000      // a read while asked
#define COLLECT_ASK_MS 2000       // how long one collectibles_poll keeps it asked
#define COLLECT_RESCAN_MS 10000   // after a failed pass over the heap, the next no sooner; twice as long after each
#define COLLECT_RESCAN_MAX_MS 320000  // failure, up to this (a lasting one, such as an edited save, costs little)
#define COPIES 2                  // the objects kept per flag (one is expected; a second one is read too)
#define CANDIDATES 6              // the hits kept per flag before the vote

// The two kinds of flag object: a bool is 0x18 bytes, its value bit 0 of the byte at +0xb (the rest of that byte and
// +8..+0xa are the flag's properties) and its hash at +0xc; an s32 is 0x28 bytes, its value at +0x18 and its hash at
// +0x1c (runtime/rupees.c)
enum { K_BOOL, K_S32 };
static const struct { uint8_t size, value, hash; } layout[2] = {{0x18, 0x0b, 0x0c}, {0x28, 0x18, 0x1c}};

// Entries: the Koroks, then the shrines' cleared flags and the towers' (bool), the shrines' found flags (s32), and the
// Korok count (s32)
#define N_KOROK COUNT(korok_table)
#define N_SHRINE COUNT(shrine_table)
#define N_TOWER COUNT(tower_table)
#define E_CLEARED N_KOROK
#define E_TOWER (E_CLEARED + N_SHRINE)
#define E_FOUND (E_TOWER + N_TOWER)
#define E_TOTAL (E_FOUND + N_SHRINE)
#define N_ENTRY (E_TOTAL + 1)
static int entry_kind(int e) { return e < E_FOUND ? K_BOOL : K_S32; }
static uint32_t entry_hash(int e) {
    return e < E_CLEARED ? korok_table[e].found : e < E_TOWER ? shrine_table[e - E_CLEARED].cleared
         : e < E_FOUND ? tower_table[e - E_TOWER] : e < E_TOTAL ? shrine_table[e - E_FOUND].found : COLLECT_KOROKS_FOUND;
}

typedef struct { uint32_t hash; uint16_t entry; } Key;
static Key keys[N_ENTRY];  // by hash
typedef struct { uint64_t obj, vt; } Hit;
static Hit hits[N_ENTRY][CANDIDATES];
static uint8_t nhits[N_ENTRY];
static uint64_t objs[N_ENTRY][COPIES];  // the objects read (0: none)
static uint64_t vt_kind[2];
static bool indexed;

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool running, stop_req, poked;
static uint64_t asked_until;                  // plat_now_ns; under lock
static CollectState state = COLLECT_READING;  // under lock
typedef struct {
    CollectCounts counts[COLLECT_REGIONS + 1];  // [COLLECT_REGIONS]: all of them
    bool korok_got[N_KOROK], shrine_found[N_SHRINE];  // (found or cleared)
} CollectRead;
static CollectRead last;  // the last read; under lock

static uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static uint8_t rd8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static bool stopping(void) { return __atomic_load_n(&stop_req, __ATOMIC_RELAXED); }
static bool in_main(uint64_t v) { return v >= 0x7000000000ull && g_site.main_end && v < g_site.main_end && !(v & 7); }

static int key_cmp(const void *a, const void *b) {
    uint32_t x = ((const Key *)a)->hash, y = ((const Key *)b)->hash;
    return x < y ? -1 : x > y;
}

static int key_find(uint32_t hash) {
    int lo = 0, hi = N_ENTRY - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid].hash == hash) return keys[mid].entry;
        if (keys[mid].hash < hash) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

// Still inside the game's heap: its pages stay committed while it keeps its size (runtime/kernel.c SetHeapSize; only a
// reboot resets them, with this thread stopped first). Not a VirtualQuery per object: on Windows' write-watched guest
// window each one took ~0.7 ms, ~1.5 s a read, and held up the renderer's GetWriteWatch (frames of up to 1.5 s).
static bool obj_there(uint64_t obj, int kind) {
    return obj >= HEAP_BASE && obj + layout[kind].size <= HEAP_BASE + kernel_heap_size();
}

// Every object read still the flag it was (a new game or a load can lay the heap out again)
static bool index_holds(void) {
    for (int i = 0; i < N_ENTRY; i++) {
        int k = entry_kind(i);
        for (int c = 0; c < COPIES && objs[i][c]; c++) {
            uint64_t o = objs[i][c];
            if (!obj_there(o, k) || rd64(o) != vt_kind[k] || rd32(o + layout[k].hash) != entry_hash(i)) return false;
        }
    }
    return true;
}

// A flag's value: set in any copy (a bool), the largest (an s32)
static uint32_t flag_value(int e) {
    int k = entry_kind(e);
    uint32_t v = 0;
    for (int c = 0; c < COPIES && objs[e][c]; c++) {
        uint32_t x = k == K_BOOL ? rd8(objs[e][c] + layout[k].value) & 1 : rd32(objs[e][c] + layout[k].value);
        if (x > v) v = x;
    }
    return v;
}

// The class most of a kind's hits share; keeps those hits as the objects. False: a flag of the kind not found.
static bool vote(int kind) {
    typedef struct { uint64_t vt; int n; } Tally;
    Tally t[16];
    int nt = 0, best = -1, flags = 0;
    for (int i = 0; i < N_ENTRY; i++) {
        if (entry_kind(i) != kind) continue;
        flags++;
        for (int h = 0; h < nhits[i]; h++) {
            int j = 0;
            while (j < nt && t[j].vt != hits[i][h].vt) j++;
            if (j == nt && nt < COUNT(t)) t[nt++] = (Tally){hits[i][h].vt, 0};
            if (j < nt) t[j].n++;
        }
    }
    for (int j = 0; j < nt; j++)
        if (best < 0 || t[j].n > t[best].n) best = j;
    if (best < 0) {
        LOG("[collect] %s flags: none found\n", kind == K_BOOL ? "bool" : "s32");
        return false;
    }
    vt_kind[kind] = t[best].vt;
    int missing = 0, extra = 0;
    for (int i = 0; i < N_ENTRY; i++) {
        if (entry_kind(i) != kind) continue;
        int c = 0;
        for (int h = 0; h < nhits[i]; h++)
            if (hits[i][h].vt == t[best].vt) {
                if (c < COPIES) objs[i][c++] = hits[i][h].obj;
                else extra++;
            }
        missing += !c;
    }
    if (missing)
        LOG("[collect] %s flags: class %llx, %d hits for %d flags, %d not found, %d more copies\n",
            kind == K_BOOL ? "bool" : "s32", (unsigned long long)t[best].vt, t[best].n, flags, missing, extra);
    return !missing;
}

// One pass over the heap: every flag object of the table, by its class and hash
static bool build_index(void) {
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size(), a, e;
    indexed = false;
    memset(nhits, 0, sizeof nhits);
    memset(objs, 0, sizeof objs);
    uint64_t t0 = plat_now_ns();
    for (uint64_t at = lo; !stopping() && plat_committed_range(at, hi, &a, &e); at = e)
        for (uint64_t p = (a + 7) & ~7ull; p + layout[K_BOOL].size <= e; p += 8) {
            uint64_t vt = rd64(p);
            if (!in_main(vt)) continue;
            for (int k = 0; k < 2; k++) {
                if (p + layout[k].size > e) continue;
                int i = key_find(rd32(p + layout[k].hash));
                if (i >= 0 && entry_kind(i) == k && nhits[i] < CANDIDATES) hits[i][nhits[i]++] = (Hit){p, vt};
            }
        }
    if (stopping() || !vote(K_BOOL) || !vote(K_S32)) return false;
    uint32_t total = flag_value(E_TOTAL), found = 0;
    for (int i = 0; i < N_KOROK; i++) found += flag_value(i) != 0;
    if (found != total) {
        LOG("[collect] %u Koroks read as found, the game counts %u: not shown\n", (unsigned)found, (unsigned)total);
        return false;
    }
    LOG("[collect] flags read (%.1f s)\n", (double)(plat_now_ns() - t0) / 1e9);
    return indexed = true;
}

static void read_counts(CollectRead *out) {
    memset(out, 0, sizeof *out);
    CollectCounts *all = &out->counts[COLLECT_REGIONS];
    for (int i = 0; i < N_KOROK; i++) {
        CollectCounts *r = &out->counts[korok_table[i].region];
        bool got = out->korok_got[i] = flag_value(i) != 0;
        r->koroks_total++, all->koroks_total++;
        r->koroks += got, all->koroks += got;
    }
    for (int i = 0; i < N_SHRINE; i++) {
        CollectCounts *r = &out->counts[shrine_table[i].region];
        bool cleared = flag_value(E_CLEARED + i) != 0, found = out->shrine_found[i] = cleared || flag_value(E_FOUND + i);
        r->shrines_total++, all->shrines_total++;
        r->shrines_found += found, all->shrines_found += found;
        r->shrines_cleared += cleared, all->shrines_cleared += cleared;
    }
    for (int i = 0; i < N_TOWER && i < COLLECT_REGIONS; i++) out->counts[i].tower = flag_value(E_TOWER + i) != 0;
}

static void set_state(CollectState s, const CollectRead *c) {
    pthread_mutex_lock(&lock);
    state = s;
    if (c) last = *c;
    pthread_mutex_unlock(&lock);
}

static void *collect_thread(void *arg) {
    (void)arg;
    static CollectRead read;
    uint64_t failed_at = 0, retry_ms = COLLECT_RESCAN_MS;
    pthread_mutex_lock(&lock);
    while (!stop_req) {
        bool asked = plat_now_ns() < asked_until;
        pthread_mutex_unlock(&lock);
        if (asked && kernel_heap_size()) {
            if (indexed && !index_holds()) indexed = false;  // (the flags moved: read the heap again)
            if (!indexed && (!failed_at || plat_now_ns() - failed_at >= retry_ms * 1000000ull)) {
                if (!failed_at) set_state(COLLECT_READING, NULL);  // (a retry keeps saying it failed until it works)
                else if (retry_ms < COLLECT_RESCAN_MAX_MS) retry_ms *= 2;
                failed_at = build_index() ? 0 : plat_now_ns();
                if (failed_at && !stopping()) set_state(COLLECT_NOT_FOUND, NULL);
                if (!failed_at) retry_ms = COLLECT_RESCAN_MS;
            }
            if (indexed) {
                read_counts(&read);
                set_state(COLLECT_READY, &read);
            }
        }
        // asked: the next read in COLLECT_POLL_MS; not asked: asleep until collectibles_poll asks (or a reboot)
        pthread_mutex_lock(&lock);
        if (plat_now_ns() < asked_until) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            long long ns = until.tv_nsec + COLLECT_POLL_MS * 1000000LL;
            until.tv_sec += (time_t)(ns / 1000000000LL), until.tv_nsec = (long)(ns % 1000000000LL);
            while (!stop_req && !poked && pthread_cond_timedwait(&wake, &lock, &until) != ETIMEDOUT) {}
        } else
            while (!stop_req && !poked) pthread_cond_wait(&wake, &lock);
        poked = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

CollectState collectibles_poll(void) {
    if (!HAVE_TABLE || !running || COLLECT_REGIONS != game_place_region_count()) return COLLECT_NO_TABLE;
    pthread_mutex_lock(&lock);
    bool idle = plat_now_ns() >= asked_until;
    asked_until = plat_now_ns() + COLLECT_ASK_MS * 1000000ull;
    if (idle) poked = true, pthread_cond_signal(&wake);  // (shown again: read now, not at the next poll)
    CollectState s = state;
    pthread_mutex_unlock(&lock);
    return s;
}

int collectibles_regions(void) { return COLLECT_REGIONS; }

bool collectibles_counts(int region, CollectCounts *out) {
    if (region < -1 || region >= COLLECT_REGIONS) return false;
    pthread_mutex_lock(&lock);
    bool ok = state == COLLECT_READY;
    if (ok) *out = last.counts[region < 0 ? COLLECT_REGIONS : region];
    pthread_mutex_unlock(&lock);
    return ok;
}

int collectibles_missing(CollectPoint *out, int max) {
    int n = 0;
    pthread_mutex_lock(&lock);
    if (state == COLLECT_READY) {
        for (int i = 0; i < N_KOROK && n < max; i++) {
            const CollectKorok *k = &korok_table[i];
            if (!last.korok_got[i] && last.counts[k->region].tower)
                out[n++] = (CollectPoint){k->x, k->z, COLLECT_KOROK, k->found};
        }
        for (int i = 0; i < N_SHRINE && n < max; i++) {
            const CollectShrine *sh = &shrine_table[i];
            if (!last.shrine_found[i] && last.counts[sh->region].tower)
                out[n++] = (CollectPoint){sh->x, sh->z, COLLECT_SHRINE, sh->found};
        }
    }
    pthread_mutex_unlock(&lock);
    return n;
}

// collectibles: the counts, all of Hyrule and by region, and the towers activated (the first time: a few seconds to read
// them)
static void cmd_collectibles(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    CollectState s = collectibles_poll();
    if (s == COLLECT_NO_TABLE) { control_fail(r, "collectibles: none to count (a build without the game files)"); return; }
    control_field_str(r, "state", s == COLLECT_READY ? "ready" : s == COLLECT_READING ? "reading" : "not_found");
    if (s == COLLECT_READING) { control_text(r, "Reading the game's records: ask again in a moment"); return; }
    if (s == COLLECT_NOT_FOUND) { control_text(r, "The game's records couldn't be read (the log says why)"); return; }
    char towers[512] = "";  // the regions whose tower is activated (English names)
    for (int i = -1; i < COLLECT_REGIONS; i++) {
        CollectCounts c;
        if (!collectibles_counts(i, &c)) break;
        if (i >= 0 && c.tower) {
            size_t at = strlen(towers);
            snprintf(towers + at, sizeof towers - at, "%s%s", at ? ", " : "", game_place_region_name(i, NULL));
        }
        if (i < 0) {
            control_field_int(r, "koroks", c.koroks);
            control_field_int(r, "shrines_found", c.shrines_found);
            control_field_int(r, "shrines_cleared", c.shrines_cleared);
        }
        control_text(r, "%s: %d/%d Koroks, %d/%d shrines found, %d cleared%s", i < 0 ? "All of Hyrule" :
                     game_place_region_name(i, NULL), c.koroks, c.koroks_total, c.shrines_found, c.shrines_total,
                     c.shrines_cleared, i >= 0 && c.tower ? ", tower activated" : "");
    }
    control_field_str(r, "towers", towers);
}

// (runtime/control_game.c: developer mode only, as the other cheats' commands)
void collectibles_control_init(void) {
    static const ControlCmd cmds[] = {
        {"collectibles", "", "Cheat: the Korok seeds and shrines found, all of Hyrule and by region, and the towers activated",
         cmd_collectibles, 0},
    };
    control_register(cmds, 1);
}

void collectibles_start(void) {
    if (!HAVE_TABLE || running) return;
    for (int i = 0; i < N_ENTRY; i++) keys[i] = (Key){entry_hash(i), (uint16_t)i};
    qsort(keys, N_ENTRY, sizeof *keys, key_cmp);
    stop_req = poked = false;
    if (pthread_create(&thread, NULL, collect_thread, NULL) == 0) running = true;
}

// Reboot: the thread is gone before the guest memory is reset under it; the next boot starts it again
RT_ON_RESET(collectibles_reset) {
    if (running) {
        pthread_mutex_lock(&lock);
        __atomic_store_n(&stop_req, true, __ATOMIC_RELAXED);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        pthread_join(thread, NULL);
        running = false;
    }
    indexed = false;
    vt_kind[0] = vt_kind[1] = 0;
    pthread_mutex_lock(&lock);  // (the render thread may be in collectibles_poll)
    asked_until = 0;
    state = COLLECT_READING;
    pthread_mutex_unlock(&lock);
}
