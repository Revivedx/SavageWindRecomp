// Video > Draw distance (settings.h DRAWDIST_*): how far from the camera the game keeps the actors placed on the map,
// draws its grass and its distant trees. Original leaves every one of the game's values untouched.
//
// Actors.
// Every placed actor (trees, bushes, rocks, buildings, enemies, and the full model of a building that has a "_Far"
// version) exists only within its traverse distance of the camera: the actor's traverseDist from ActorInfo (100 when it
// has none) times a scale that is 1 in the released game (tools/patches.py _traverse_scale). Beyond it a building
// shows its "_Far" model and a tree the terrain's distant trees, or nothing. A longer draw distance multiplies that
// scale: the game itself creates the actors further out and keeps them there, with the same rules as at its own
// distances. Original returns the game's value untouched.
//
// Only the far models (a distance of FAR_DIST or more of their own: "_Far" buildings, Hyrule Castle, the mountains'
// rock models) go past FAR_DIST; everything else stops there. Actors with a nearer distance of their own, made beyond
// it, crashed the game while it loaded the world (2x: distances of 1000-2000 m taken to 2000-4000 m, at the Great
// Plateau; 2x capped at 2000 m, and 2x of only the 2000 m-and-more ones, both loaded and played). The far models go
// at most FAR_MAX times out: at 2x, buildings far off flickered and whole blocks of them went missing (Tabantha, user
// play test 2026-10-29), and 1.5x was fine.
//
// High's factor for the nearer actors, 1.6, is the highest that kept every actor (round 3, 2026-10-29: a copy of a 100%
// save warped to Kakariko, Hyrule Castle's front, Korok Forest, Lurelin, Hateno and Gerudo Town, walking and turning
// for 4700 frames). Past it the game leaves actors out, silently: at the castle's front its wall and banners were
// missing at 1.75x (twice), 2x (twice), 2.25x and 4x, and there at 1x, 1.5x (twice) and 1.6x (twice). Further out it
// crashed while it loaded the world (a null call from 1.9.0 0x7001405cd8, or an access to 0): 2.5x at Gerudo Town,
// 3x at Korok Forest and Gerudo Town, 4x at Kakariko, Hateno and Gerudo Town; 2.25x loaded everywhere.
//
// Grass (tools/patches.py _grass_range, _grass_fade, _grass_blade_*). Each layer (blades: 3 m cells in a 47-cell grid,
// +-69 m; crossed quads: 10 m cells, 59-cell grid, +-290 m) makes grass in the cells of a region in front of the
// camera (blades about 24 m in play, crossed quads 120 m), lists them nearest first (blades: at most 512, crossed quads
// 768), and draws them fading by distance (blades out to about 21.5 m, crossed quads about 46 m). The crossed quads'
// region already reaches past their fade, so only their fade goes further. The blades need both, and their region is
// bounded by their list (holes in the grass by Link when it fills), so their factor follows how full it is. At High
// the blades also get 512 draw slots and a cone-shaped region past the game's own near part, which lets them reach
// about 2.45x (their fade out to about 53 m) within the same list. Every pass that walks the slots goes to the last
// one: the per-frame one that points each slot at this frame's half of its shader block stopped at the 256th, and the
// cells past it flickered at High (round 4, user play test 2026-10-29).
//
// Distant trees (tools/patches.py _tree_ranges): the terrain's far trees fade in where the actor trees end, at the
// actors' factor (the game's hand-off, half way through that fade), and end and fade out the far models' factor times
// further out.
//
// The terrain's own detail levels are not changed: no distance that picks them was found. Its heights and materials
// come from maps rendered around the camera (tera::ApertureMaps) whose extents follow their texture sizes.
//
// (tests) SAVAGEWIND_TRAVERSE_SCALE=<x>: this factor instead of the setting's; SAVAGEWIND_FAR_SCALE=<x>: the far
// models' and the distant trees' factor; SAVAGEWIND_GRASS_BLADES / _QUADS=<x>: the grass layers' factors;
// SAVAGEWIND_GRASS_PROBE=1: their numbers in the log; SAVAGEWIND_DRAWDIST_AT=<frame>:<level>,...: levels in one run.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "settings.h"

#define FAR_DIST 2000.0f  // (metres)
#define FAR_MAX 1.5f

// the level in use: the setting's, or (tests) SAVAGEWIND_DRAWDIST_AT=<script frame>:<level 0-2>,... from that frame of
// SAVAGEWIND_SCRIPT_CLOCK on, so one run (one weather, one camera) shows several levels
static int level(void) {
    static const char *at;
    static int init, forced = -1;
    if (!init) init = 1, at = getenv("SAVAGEWIND_DRAWDIST_AT");
    if (at && *at) {
        extern uint64_t framepace_script_now(void);
        char *end;
        unsigned long long fr = strtoull(at, &end, 10);
        if (*end == ':' && framepace_script_now() >= fr) {
            forced = (int)strtol(end + 1, &end, 10);
            at = *end == ',' ? end + 1 : end;
        }
    }
    return forced >= 0 ? forced : g_settings.draw_distance;
}

static const float k_factor[DRAWDIST_COUNT] = {1.0f, 1.5f, 1.6f};  // (DRAWDIST_*)

float draw_distance_factor(void) {
    static float env = -1;
    if (env < 0) {
        const char *s = getenv("SAVAGEWIND_TRAVERSE_SCALE");
        float f = s ? (float)atof(s) : 0;
        env = f >= 0.25f && f <= 8.0f ? f : 0;
    }
    if (env > 0) return env;
    int d = level();
    return d > DRAWDIST_ORIGINAL && d < DRAWDIST_COUNT ? k_factor[d] : 1.0f;
}

// the far models' and the distant trees' factor: the actors' one, at most FAR_MAX
static float far_factor(void) {
    static float env = -1;
    if (env < 0) {
        const char *s = getenv("SAVAGEWIND_FAR_SCALE");
        float f = s ? (float)atof(s) : 0;
        env = f >= 0.25f && f <= 8.0f ? f : 0;
    }
    if (env > 0) return env;
    float f = draw_distance_factor();
    return f > FAR_MAX ? FAR_MAX : f;
}

// The grass's own factors, by layer (1: the game's own; see the top).
// - Blades: their near region (the cells that get grass made and drawn) and their fade, at most these factors, and
//   only while their cell list has room: it holds 512 cells (inline in their manager, +0x878), the game's own play
//   fills 335-342 of it at the Great Plateau and in Hyrule Field, and a full list leaves holes in the grass by Link
//   (seen at 3x). The factor in use follows the list (blades_factor).
// - Crossed quads: their fade only. Their region (about 120 m in play) already holds the cells, and its list is full
//   in the game's own play (718-768 of 768).
static const float k_blades[DRAWDIST_COUNT] = {1.0f, 1.15f, 2.5f};  // (DRAWDIST_*)
static const float k_quads[DRAWDIST_COUNT] = {1.0f, 1.6f, 2.5f};
#define BLADE_LIST 512
#define BLADE_LIST_HIGH 485  // (listed cells past this: the factor goes down)
#define BLADE_LIST_LOW 455   // (below this: back up towards the level's)

// (tests) SAVAGEWIND_GRASS_BLADES=<x>, SAVAGEWIND_GRASS_QUADS=<x>: these factors instead of the setting's
static float env_factor(const char *name, float *cache) {
    if (*cache < 0) {
        const char *s = getenv(name);
        float f = s ? (float)atof(s) : 0;
        *cache = f >= 0.25f && f <= 8.0f ? f : 0;
    }
    return *cache;
}
static float grass_target(int quads) {
    static float env[2] = {-1, -1};
    float e = env_factor(quads ? "SAVAGEWIND_GRASS_QUADS" : "SAVAGEWIND_GRASS_BLADES", &env[quads]);
    if (e > 0) return e;
    int d = level();
    return d > DRAWDIST_ORIGINAL && d < DRAWDIST_COUNT ? (quads ? k_quads : k_blades)[d] : 1.0f;
}

static int grass_probe(void) {  // (tests) SAVAGEWIND_GRASS_PROBE=1: the grass layers' numbers in the log
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_GRASS_PROBE") != NULL;
    return on;
}

// The blades' factor in use this frame: set once a frame by their fade store (grass_fade, which runs before their view
// context in the same update), from the level's target and how full their list was last frame: it climbs slowly
// while the list has room, drops fast when it nears full, and starts from the game's own where there is no grass (an
// empty list says nothing about how full the next field's would be).
static float g_blades = 1.0f;

static void blades_factor(uint64_t mgr) {
    float target = grass_target(0);
    if (target == 1.0f) { g_blades = 1.0f; return; }  // (Original)
    if (getenv("SAVAGEWIND_GRASS_NOGUARD")) { g_blades = target; return; }  // (tests: the target, list or not)
    uint32_t listed = *(const uint32_t *)(uintptr_t)(mgr + 0x878);  // (last frame's: the update clears it later)
    float f = g_blades < 1.0f ? 1.0f : g_blades;
    if (listed == 0) f = 1.0f;
    else if (listed >= BLADE_LIST_HIGH) f = 1.0f + (f - 1.0f) * 0.8f;
    else if (listed < BLADE_LIST_LOW) f += 0.005f;
    if (f > target) f = target;
    g_blades = f;
}

// The blades' region is two slabs along the view (near, far), each as wide as the view is at its end. Grown 2.5 times
// it lists more cells than their list holds, so at High the part past the game's own near distance is a cone instead,
// CONE_WIDEN times as wide as the view (tan(fovy / 2) x aspect) plus CONE_MARGIN m (rt_grass_blade_lateral).
#define CONE_WIDEN 1.0f
#define CONE_MARGIN 4.0f
#define CONE_AHEAD 6.0f  // (the far part keeps this much past the near one: cells made just before they are needed)
static int g_cone;
static float g_cone_from, g_cone_to, g_cone_k;

// a blade cell's distance from the view's axis is checked against `lim` (part 0 near, 1 far: its half width + 1.5 m):
// x, z the cell's offset from the camera, d2 its squared distance, a, b the view direction (float bits) -> the half
// width to use. The far part, which lists cells ahead of the near one to make them early, keeps only CONE_AHEAD m.
float rt_grass_blade_lateral(int part, float lim, float x, float z, float d2, uint32_t a_bits, uint32_t b_bits) {
    if (!g_cone || d2 <= g_cone_from * g_cone_from) return lim;  // (Original, Medium, and the game's own near cells)
    float ahead = g_cone_to + CONE_AHEAD;
    if (part == 1 && d2 > ahead * ahead) return -1.0f;  // (never inside)
    union { uint32_t u; float f; } a = {.u = a_bits}, b = {.u = b_bits};
    float axis = a.f * x + b.f * z;  // (along the view; the half width is |a z - b x|)
    float w = axis * g_cone_k + CONE_MARGIN;
    return w < lim ? w : lim;
}

// the grass's view context (x2 the camera, s3 the layer's cell size: blades 3 m, crossed quads 10 m): s0 the least
// tan(fovy / 2) it takes (tan(10 deg)) and its two screen-size limits (float bits), as the game passes them; the
// region its cells are made in reaches 1 / (max(tan, s0) * limit) -> 1 when they change. For the blades the limits go
// down by their factor and s0 up by it: the near region reaches that many times further in a normal view, and the
// furthest one (a narrow view) stays the game's own, inside their grid.
int rt_grass_range(uint64_t cam, uint32_t cell_bits, uint32_t *s0, uint32_t *a, uint32_t *b) {
    union { uint32_t u; float f; } cell = {.u = cell_bits};
    int quads = !(cell.f < 5);
    if (grass_probe()) {
        static long n;
        if ((++n & 511) < 2) {
            union { uint32_t u; float f; } x = {.u = *s0}, p = {.u = *a}, q = {.u = *b};
            float t = *(float *)(uintptr_t)(cam + 0x134), tt = t > x.f ? t : x.f;
            LOG("[grass] %s: tan(fovy/2) %g, region %g m (the game's), furthest %g m\n", quads ? "crossed quads" : "blades",
                t, 1 / (tt * p.f) - 1 / (tt * q.f), 1 / (x.f * p.f));
        }
    }
    float f = quads ? 1.0f : g_blades;
    if (!quads) {  // (the blades' cone, for rt_grass_blade_lateral: at High only, past the game's own near distance)
        union { uint32_t u; float f; } x = {.u = *s0}, p = {.u = *a}, q = {.u = *b};
        float tn = *(float *)(uintptr_t)(cam + 0x134), asp = *(float *)(uintptr_t)(cam + 0x12c);
        float tt = tn > x.f ? tn : x.f;
        g_cone = f > 1.0f && level() == DRAWDIST_HIGH && !getenv("SAVAGEWIND_GRASS_NOCONE");
        g_cone_from = 1 / (tt * p.f) - 1 / (tt * q.f);  // (the game's near distance this frame)
        g_cone_k = tn * (asp > 0 ? asp : 1.0f) * CONE_WIDEN;
    }
    if (f == 1.0f) return 0;  // (Original, and the crossed quads: the game's own)
    union { uint32_t u; float f; } t = {.u = *s0};
    if (t.f > 0 && t.f < 3.0e38f) t.f *= f;
    *s0 = t.u;
    uint32_t *v[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        union { uint32_t u; float f; } x = {.u = *v[i]};
        if (x.f > 0 && x.f < 3.0e38f) x.f /= f;  // (0 or less, or FLT_MAX: the game's "no limit")
        *v[i] = x.u;
    }
    if (!quads) {  // (the near part's distance now, for the far part's cut)
        union { uint32_t u; float f; } x = {.u = *s0}, p = {.u = *a}, q = {.u = *b};
        float tn = *(float *)(uintptr_t)(cam + 0x134), tt = tn > x.f ? tn : x.f;
        g_cone_to = 1 / (tt * p.f) - 1 / (tt * q.f);
    }
    return 1;
}

// a grass layer's fade scale (float bits; its fades go as 1 / (k * distance)) as the game stores it once a frame (x: its
// manager) -> the one to store: k / the layer's factor, so its grass fades that many times further out
static uint32_t grass_fade(uint64_t mgr, uint32_t k, int quads) {
    if (!quads) blades_factor(mgr);
    float f = quads ? grass_target(1) : g_blades;
    if (grass_probe()) {
        static long n[2];
        if ((++n[quads] & (getenv("SAVAGEWIND_GRASS_PROBE_ALL") ? 0 : 255)) == (getenv("SAVAGEWIND_GRASS_PROBE_ALL") ? 0 : 1)) {
            const uint32_t *m = (const uint32_t *)(uintptr_t)mgr;
            // (each list entry: u16 cell, u16 squared distance in m^2; the furthest one of each list)
            uint32_t dl = quads ? 0x288 : 0x68, ll = quads ? 0xe98 : 0x878, nd = m[dl / 4], nl = m[ll / 4], fd = 0, fl = 0;
            for (uint32_t i = 0; i < nd && i < 1024; i++) { uint32_t d = m[dl / 4 + 1 + i] >> 16; if (d > fd) fd = d; }
            for (uint32_t i = 0; i < nl && i < 1024; i++) { uint32_t d = m[ll / 4 + 1 + i] >> 16; if (d > fl) fl = d; }
            if (quads) LOG("[grass] crossed quads: %u cells drawn (furthest %.0f m), %u listed (of 768, furthest %.0f m), "
                           "factor %g\n", nd, sqrt((double)fd), nl, sqrt((double)fl), f);
            else {
                // (the draw slots, 0x78 bytes each at +0x37388: +0x41 the half of their shader block in use; slots
                // the per-frame pass skipped keep another half than slot 0's)
                const uint8_t *slots = *(const uint8_t *const *)(uintptr_t)(mgr + 0x37388);
                uint32_t ns = m[0x37380 / 4], stale = 0;
                for (uint32_t i = 1; slots && i < nd && i < ns; i++) stale += slots[i * 0x78 + 0x41] != slots[0x41];
                LOG("[grass] blades: %u cells drawn (of %u, furthest %.0f m, %u on another half), %u listed (of %d, "
                    "furthest %.0f m), factor %g (level's %g)\n", nd, ns, sqrt((double)fd), stale, nl, BLADE_LIST,
                    sqrt((double)fl), f, grass_target(0));
            }
        }
    }
    if (f == 1.0f) return k;  // (Original: the game's own, bit for bit)
    union { uint32_t u; float f; } x = {.u = k};
    if (x.f > 0 && x.f < 3.0e38f) x.f /= f;
    return x.u;
}
// The blades' draw slots, made once when the grass is set up at boot (the game's 256). At High there are 512, for a
// region up to 2.5 times the game's; the two passes that stop at the 256th cell stop at the last slot instead.
// (Original and Medium: the game's 256 and 255, so a game started at those levels draws as the game does.)
#define BLADE_SLOTS_HIGH 512
static uint32_t g_blade_slots = 0x100;
uint32_t rt_grass_blade_slots(uint32_t game) {
    const char *e = getenv("SAVAGEWIND_GRASS_SLOTS");  // (tests: this many instead)
    g_blade_slots = e && atoi(e) >= 16 && atoi(e) <= 4096 ? (uint32_t)atoi(e) :
                    level() == DRAWDIST_HIGH ? BLADE_SLOTS_HIGH : game;
    if (g_blade_slots != game) LOG("[draw distance] grass blades: %u draw slots (game %u)\n", g_blade_slots, game);
    return g_blade_slots;
}
uint32_t rt_grass_blade_cap(void) { return g_blade_slots - 1; }
// ... and the per-frame pass that points each slot at this frame's half of its shader block (tools/patches.py
// _grass_blade_cap_swap): every slot, or a slot past the 256th keeps another frame's half and its cell flickers.
// (tests) SAVAGEWIND_GRASS_OLDSWAP=1: the game's 256 there, as round 3 had it.
uint32_t rt_grass_blade_swap_cap(void) {
    static int old = -1;
    if (old < 0) old = getenv("SAVAGEWIND_GRASS_OLDSWAP") != NULL;
    return old ? 0xff : g_blade_slots - 1;
}

uint32_t rt_grass_fade_blades(uint64_t mgr, uint32_t k) { return grass_fade(mgr, k, 0); }
uint32_t rt_grass_fade_quads(uint64_t mgr, uint32_t k) { return grass_fade(mgr, k, 1); }

// the terrain's distant trees (x0 of their per-frame shader setup): their five ranges at +0x14cb0 (where they fade in,
// that fade, how far they go, the fade out, where their blocks change detail) from the game's values times the
// factor. The game writes them again as it plays (about twice a frame in play; the end distance is 4000 m, sometimes
// 4444.5), so ranges that aren't the ones last set here are the game's new ones and are scaled again. (Each object
// keeps its own: one object's ranges are never taken from another's, already scaled ones.)
void rt_tree_ranges(uint64_t tree) {
    static struct { uint64_t obj; float game[5], set[5]; } t[4];
    static int n, next;
    static float logged = 1.0f;
    float *r = (float *)(uintptr_t)(tree + 0x14cb0);
    int i = 0;
    while (i < n && t[i].obj != tree) i++;
    if (i == n) {
        i = n < 4 ? n++ : next++ & 3;
        t[i].obj = tree;
        memcpy(t[i].game, r, sizeof t[i].game);
        memcpy(t[i].set, r, sizeof t[i].set);
    } else if (memcmp(r, t[i].set, sizeof t[i].set)) {
        memcpy(t[i].game, r, sizeof t[i].game);
        memcpy(t[i].set, r, sizeof t[i].set);
    }
    // Where they fade in (the first two) stays the game's (100-200 m) at every level. Actor trees made farther out
    // overlap them for longer, but there is always a tree drawn while the game makes the actors entering range a few at
    // a time: with the fade-in moved out with the actors (High: 160-320 m), a tree inside 160 m whose actor was not made
    // yet was not drawn at all, then popped in (the user's play test, #88). How far they go (the last three) takes
    // the far models' factor.
    float f = far_factor(), fnear = 1.0f, want[5];
    static float env_near = -1, env_far = -1;  // (tests) SAVAGEWIND_TREE_NEAR / _FAR=<x>: the first two / last three
    float fn = env_factor("SAVAGEWIND_TREE_NEAR", &env_near), ff = env_factor("SAVAGEWIND_TREE_FAR", &env_far);
    for (int k = 0; k < 5; k++)  // (Original: x 1, the game's own bits)
        want[k] = t[i].game[k] * (k < 2 ? (fn > 0 ? fn : fnear) : (ff > 0 ? ff : f));
    if (!memcmp(want, r, sizeof want)) return;  // (nothing to write)
    memcpy(r, want, sizeof want);
    memcpy(t[i].set, want, sizeof want);
    if (f != logged) LOG("[draw distance] terrain's distant trees: to %.0f m (game %.0f m)\n", want[2], t[i].game[2]);
    logged = f;
}

// the traverse distance scale (float bits) as the game has it, and the actor's own distance (float bits; 0 or less:
// the game takes 100) -> the scale to use
uint32_t rt_traverse_scale(uint32_t scale, uint32_t dist) {
    float f = draw_distance_factor();
    if (f == 1.0f) return scale;  // (Original: the game's own, bit for bit)
    union { uint32_t u; float f; } s = {.u = scale}, d = {.u = dist};
    float game = (d.f > 0 ? d.f : 100.0f) * s.f;  // (the game's distance)
    if (game >= FAR_DIST) f = far_factor();
    else if (f > 1.0f && game * f > FAR_DIST) f = FAR_DIST / game;
    s.f *= f;
    return s.u;
}
