// Settings > Camera while the game runs. The game reads its camera program once, into its own parameter objects: one
// per camera (action) number, 0x20 bytes or more each (vtable, name crc, the next of the camera's, then the value), the
// cameras' chains next to each other on the heap, and the cameras read them as they go (the field of view when they
// start again: aiming, a menu...). So a style that only changes numbers goes in there at once: found by the numbers the
// game has now (the program served), then written. A style that turns cameras into other kinds needs them made again:
// a restart. Either way the actor is served with the new style from then on (fs.c camera_serve_style).
#include "camera_tune.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "control_mem.h"
#include "rt.h"
#include "settings.h"

#define ROOM 0x8000           // (as fs.c's CAMERA_GROW: what a style can add to the program)
#define MAX_VALUES 8192
#define WINDOW (1u << 20)     // (bytes each side of the first camera found: the others are near it)
#define MAX_FOUND 4096
#define MAX_HEADS 8192
#define P_CRC 8
#define P_NEXT 0x10
#define P_VALUE 0x18

static uint8_t *vanilla;
static size_t vanilla_n;
static int booted = -1;
static bool same_kinds[CAMERA_STYLES];  // the style's cameras are the booted style's kinds
static int in_game = -1;                 // the style the game's parameter objects have, -1: not known (or a stand-in)
#define MAX_KINDS 512
static char booted_kinds[MAX_KINDS][32];  // each camera's kind as the game made it
static int nkinds;
static int served = -1;                  // the style the actor is served with
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int wanted = -1;
static atomic_bool running;

// Where each number of the program is in the game: (action, crc) -> its parameter object, sorted
typedef struct { uint64_t key, addr; } Where;
static Where *where;
static int nwhere;

static uint8_t *program_for(int style, size_t *n) {
    uint8_t *p = malloc(vanilla_n + ROOM);
    if (!p) return NULL;
    memcpy(p, vanilla, vanilla_n);
    *n = vanilla_n;
    camera_style_program(p, n, vanilla_n + ROOM, style);
    return p;
}

static CameraValue *values_for(int style, int *n) {
    size_t size;
    uint8_t *p = program_for(style, &size);
    CameraValue *v = p ? malloc(sizeof *v * MAX_VALUES) : NULL;
    *n = v ? camera_program_values(p, size, v, MAX_VALUES) : 0;
    free(p);
    return v;
}

// The game makes its cameras from the style's program: their kinds, and which styles keep them
static void made_with(int style) {
    booted = style;
    size_t bn;
    uint8_t *b = vanilla ? program_for(style, &bn) : NULL;
    nkinds = b ? camera_program_kinds(b, bn, booted_kinds, MAX_KINDS) : 0;
    for (int s = 0; s < CAMERA_STYLES; s++) {
        size_t sn;
        uint8_t *p = b ? program_for(s, &sn) : NULL;
        same_kinds[s] = p && camera_program_same_kinds(b, bn, p, sn);
        free(p);
    }
    free(b);
}

void camera_live_init(const uint8_t *program, size_t n, int style) {
    pthread_mutex_lock(&lock);
    free(vanilla);
    vanilla = malloc(n);
    if (vanilla) memcpy(vanilla, program, n);
    vanilla_n = n;
    in_game = served = vanilla ? style : -1;
    nwhere = 0;
    booted = -1, nkinds = 0;
    if (vanilla) made_with(style);
    pthread_mutex_unlock(&lock);
}

bool camera_pending(void) {
    int s = g_settings.camera_style;
    return booted >= 0 && s >= 0 && s < CAMERA_STYLES && !same_kinds[s];
}

static uint64_t key_of(int action, uint32_t crc) { return (uint64_t)action << 32 | crc; }
static int by_key(const void *a, const void *b) {
    uint64_t x = ((const Where *)a)->key, y = ((const Where *)b)->key;
    return x < y ? -1 : x > y;
}
static uint64_t where_is(int action, uint32_t crc) {
    Where k = {key_of(action, crc), 0}, *w = nwhere ? bsearch(&k, where, (size_t)nwhere, sizeof k, by_key) : NULL;
    return w ? w->addr : 0;
}

// A parameter object at window offset o: a vtable in the game's code, a name, the next one in the window or none
static bool object_at(const uint8_t *w, size_t o, uint64_t base) {
    uint64_t vt, next;
    uint32_t crc;
    memcpy(&vt, w + o, 8), memcpy(&crc, w + o + P_CRC, 4), memcpy(&next, w + o + P_NEXT, 8);
    return vt >> 32 == 0x70 && crc && (!next || (next >= base && next < base + 2ull * WINDOW && !(next & 7)));
}

// The value bytes the game holds (a bool: one byte)
static size_t held_size(const CameraValue *v) { return v->type == 0 ? 1 : v->size; }

// Does the chain from object o hold the action's numbers as they are? Those it has go to out; how many, 0 for no. (A
// number the program has and the camera's kind doesn't read isn't in it: a few the mod adds, and those of the kind a
// camera was before the mod made it another.)
#define MAY_MISS 4
static int chain_matches(const uint8_t *w, uint64_t base, size_t o, const CameraValue *v, int n, Where *out) {
    // the chain's objects by name, at most 256
    uint32_t crcs[256];
    uint64_t at[256];
    int m = 0;
    for (uint64_t a = base + o; a && m < 256;) {
        size_t off = (size_t)(a - base);
        if (a < base || off + P_VALUE + 12 > 2ull * WINDOW || !object_at(w, off, base)) return 0;
        memcpy(&crcs[m], w + off + P_CRC, 4);
        at[m++] = a;
        memcpy(&a, w + off + P_NEXT, 8);
    }
    int got = 0;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < m && crcs[j] != v[i].crc) j++;
        if (j == m) continue;
        if (memcmp(w + (at[j] - base) + P_VALUE, v[i].v, held_size(&v[i]))) return 0;
        out[got++] = (Where){key_of(v[i].action, v[i].crc), at[j]};
    }
    return got;
}

// The game's objects for every number of the program: found by the values it has now (cur). False if not found (no
// camera yet: the title screen).
static bool find_objects(const CameraValue *cur, int ncur) {
    nwhere = 0;
    if (!ncur) return false;
    // the action with the most numbers, by its first one's name: there it is in the game
    int best = 0, best_n = 0;
    for (int i = 0; i < ncur;) {
        int j = i;
        while (j < ncur && cur[j].action == cur[i].action) j++;
        if (j - i > best_n) best = i, best_n = j - i;
        i = j;
    }
    static uint64_t found[MAX_FOUND];
    size_t nf = control_mem_find_u32(cur[best].crc, HEAP_BASE, HEAP_BASE + HEAP_MAX, found, MAX_FOUND);
    uint8_t *w = malloc(2ull * WINDOW);
    Where *out = malloc(sizeof *out * (size_t)ncur), *tmp = malloc(sizeof *tmp * (size_t)ncur);
    bool ok = false;
    for (size_t f = 0; f < nf && w && out && tmp && !ok; f++) {
        uint64_t obj = found[f] - P_CRC, base = obj > HEAP_BASE + WINDOW ? obj - WINDOW : HEAP_BASE;
        memset(w, 0, 2ull * WINDOW);
        for (size_t o = 0; o < 2ull * WINDOW; o += 0x10000) control_mem_peek(base + o, w + o, 0x10000);  // (gaps stay 0)
        if ((obj & 7) || !object_at(w, (size_t)(obj - base), base) ||
            chain_matches(w, base, (size_t)(obj - base), &cur[best], best_n, tmp) < best_n - MAY_MISS)
            continue;
        // the chains: objects no other object leads to
        static uint8_t led[2 * WINDOW / 64];
        static uint32_t heads[MAX_HEADS];
        int nheads = 0;
        memset(led, 0, sizeof led);
        for (size_t o = 0; o + P_VALUE + 12 <= 2ull * WINDOW; o += 8) {
            uint64_t next;
            memcpy(&next, w + o + P_NEXT, 8);
            if (next && object_at(w, o, base)) led[(next - base) / 8 / 8] |= (uint8_t)(1 << ((next - base) / 8 % 8));
        }
        for (size_t o = 0; o + P_VALUE + 12 <= 2ull * WINDOW && nheads < MAX_HEADS; o += 8)
            if (!(led[o / 8 / 8] >> (o / 8 % 8) & 1) && object_at(w, o, base)) heads[nheads++] = (uint32_t)o;
        // each action to the chain holding most of its numbers, all as they are (the first such, and one chain each:
        // two cameras alike are told apart by order)
        static bool taken[MAX_HEADS];
        memset(taken, 0, sizeof taken);
        int nout = 0, mapped = 0, actions = 0;
        for (int i = 0; i < ncur;) {
            int j = i, top = 0, at = -1;
            while (j < ncur && cur[j].action == cur[i].action) j++;
            for (int h = 0; h < nheads; h++) {
                int g = taken[h] ? 0 : chain_matches(w, base, heads[h], &cur[i], j - i, tmp);
                if (g > top) top = g, at = h;
            }
            actions++;
            if (at >= 0 && top >= (j - i < 3 ? j - i : 3)) {
                taken[at] = true;
                nout += chain_matches(w, base, heads[at], &cur[i], j - i, out + nout);
                mapped++;
            }
            i = j;
        }
        qsort(out, (size_t)nout, sizeof *out, by_key);
        free(where);
        where = out, nwhere = nout, out = NULL;
        LOG("[camera] the game's cameras: %d of %d found\n", mapped, actions);
        ok = true;
    }
    free(w), free(out), free(tmp);
    return ok;
}

// The objects found before are still the game's (each still has its name)
static bool objects_hold(void) {
    for (int i = 0; i < nwhere; i += 37) {
        uint32_t crc;
        if (!control_mem_peek(where[i].addr + P_CRC, &crc, 4) || crc != (uint32_t)where[i].key) return false;
    }
    return nwhere > 0;
}


static uint32_t crc_of(const char *name) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *name; name++) {
        c ^= (uint8_t)*name;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

// Write the value into the game's object for (action, crc) if it holds another; 1 if it wrote
static int put(int action, uint32_t crc, const void *v, size_t n) {
    uint64_t a = where_is(action, crc);
    uint8_t now[12];
    if (!a || !control_mem_peek(a + P_VALUE, now, n) || !memcmp(now, v, n)) return 0;
    return control_mem_poke(a + P_VALUE, v, n);
}

static bool target_float(const CameraValue *v, int n, int action, const char *name, float *out) {
    uint32_t crc = crc_of(name);
    for (int i = 0; i < n; i++)
        if (v[i].action == action && v[i].crc == crc && v[i].type == 1) { memcpy(out, v[i].v, 4); return true; }
    return false;
}

// A camera the game made of another kind than the style's (until a restart makes it again): what of the style it can
// show. The over-the-shoulder styles' cameras (CameraAiming, CameraLockOnAimingAt) put back behind Link, as far and as
// wide as the style's own; the style's numbers it shares by name, except the limits that mean another thing there.
static int stand_in(int k, const CameraValue *want, int n) {
    int written = 0;
    bool aiming = !strcmp(booted_kinds[k], "CameraAiming");
    uint32_t rmin = crc_of("radiusMin"), rmax = crc_of("radiusMax");
    for (int i = 0; i < n; i++)
        if (want[i].action == k && !(aiming && (want[i].crc == rmin || want[i].crc == rmax)))
            written += put(k, want[i].crc, want[i].v, held_size(&want[i]));
    float zero = 0, f;
    if (aiming) {
        written += put(k, crc_of("sideOffset"), &zero, 4);
        if (target_float(want, n, k, "radius", &f) || target_float(want, n, k, "Radius", &f))
            written += put(k, rmin, &f, 4) + put(k, rmax, &f, 4);
        if (target_float(want, n, k, "Fovy", &f)) f *= 57.2957795f, written += put(k, crc_of("fovy"), &f, 4);
    }
    if (!strcmp(booted_kinds[k], "CameraLockOnAimingAt")) written += put(k, crc_of("OffsetX"), &zero, 4);
    if (!strcmp(booted_kinds[k], "CameraChase") && target_float(want, n, k, "fovy", &f))
        f /= 57.2957795f, written += put(k, crc_of("Fovy"), &f, 4);
    return written;
}

static void apply(int style) {
    int ncur = 0, nnew = 0, written = 0, nk = 0;
    CameraValue *cur = in_game >= 0 ? values_for(in_game, &ncur) : NULL, *want = values_for(style, &nnew);
    size_t tn;
    uint8_t *tp = program_for(style, &tn);
    static char kinds[MAX_KINDS][32];
    nk = tp ? camera_program_kinds(tp, tn, kinds, MAX_KINDS) : 0;
    free(tp);
    if (want && nk == nkinds && (objects_hold() || find_objects(cur, ncur))) {
        for (int i = 0; i < nnew; i++)
            if (!strcmp(kinds[want[i].action], booted_kinds[want[i].action]))
                written += put(want[i].action, want[i].crc, want[i].v, held_size(&want[i]));
        int stand = 0;
        for (int k = 0; k < nk; k++)
            if (strcmp(kinds[k], booted_kinds[k])) stand++, written += stand_in(k, want, nnew);
        // a camera standing still reads some only when it starts again (the field of view; right after a load, all):
        // a tap of ZL, which puts it back behind Link, once the player is back in play
        extern volatile int g_inject_camera_reset;
        if (written) g_inject_camera_reset = 1;
        LOG("[camera] style %d in the game's cameras: %d numbers%s\n", style, written,
            stand ? " (cameras of another kind: as close as they can, exact after a restart)" : "");
        in_game = stand ? -1 : style;
    } else {
        // (not made yet, the title screen: the game makes them of the served style's kinds, whatever they are)
        LOG("[camera] style %d: the game's cameras aren't made yet, they will be with it\n", style);
        if (!same_kinds[style]) made_with(style);
        in_game = style;
    }
    free(cur), free(want);
    if (camera_serve_style(style)) served = style;
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        int s = atomic_load(&wanted);
        pthread_mutex_lock(&lock);
        if (vanilla && s >= 0 && s < CAMERA_STYLES && (s != in_game || s != served)) apply(s);
        pthread_mutex_unlock(&lock);
        atomic_store(&running, false);
        if (atomic_load(&wanted) == s) break;
        atomic_store(&running, true);  // (changed meanwhile: once more)
    }
    return NULL;
}

void camera_live_apply(int style) {
    atomic_store(&wanted, style);
    if (atomic_exchange(&running, true)) return;  // (the worker takes the new one when it ends)
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL)) atomic_store(&running, false);
    else pthread_detach(t);
}
