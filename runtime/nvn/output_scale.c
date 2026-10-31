// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size (the
// final image: the window's resolution, so the HUD and menus are sharp), the 3D scene's (1600x900 docked) at
// (scene_sx, scene_sy) times theirs (vulkan/textures.c RS_OUT / RS_SCENE); the image is then drawn into
// (vx, vy, vw, vh) of the window. Video page "Upscaler" (g_settings.upscaler):
//   Off            the 3D scene renders at the window's own pixel size (its 16:9 part: the game keeps its framing)
//   AMD FSR 1      the 3D scene renders smaller by the quality mode's factor per axis (AMD's: Ultra Quality 1.3,
//                  Quality 1.5, Balanced 1.7, Performance 2.0, Ultra Performance 3.0); FSR 1 brings it to its size in the final image at the
//                  game's composite (vulkan/exec.c scene_upscale)
//   MetalFX        (macOS) metal.m has no scene / final split yet: the modes stay greyed there, and MetalFX scales the
//                  game's image to a bigger window
//   FSR 3, DLSS    listed as coming later (temporal: they need motion vectors, which the game has none of)
// The window size itself is Video > Window size (runtime/window_size.c). On a window that isn't 16:9, Video > Fill wide
// screens widens the game's views to it (rt_persp, below) and its 16:9 render targets with them; off, the 16:9 image
// is fitted into the window with bars. Video > Field of view scales the world's camera in play (rt_persp).
// Test overrides: SAVAGEWIND_SCALE=<f> (render height = 1080 x f), SAVAGEWIND_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../framepace.h"
#include "../hle.h"
#include "../platform.h"
#include "../settings.h"
#include "output_scale.h"

#ifndef __APPLE__
extern bool vk_dlss_available(void);
#endif

#define MAX_TEX 16384.0
#define SCENE_LINES 900.0  // the height of the game's 3D scene targets (docked)

#ifndef __APPLE__
bool fsr3_available(void);  // (vulkan/fsr3.h: built in)
#endif
// FSR 3 upscaling is held back until its cost on the thread that records the frame for Vulkan comes down (the draw
// translation work in progress): today that thread limits 1080p and 1440p, and FSR 3's motion vectors make every mode
// slower than native there. SAVAGEWIND_FSR3=1 offers it anyway (tests); TAA and frame generation stay as they are.
bool fsr3_upscaler_enabled(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FSR3") && atoi(getenv("SAVAGEWIND_FSR3")) != 0;
    return on;
}

bool upscaler_available(int u) {
#ifdef __APPLE__
    return u == UPSCALE_OFF || u == UPSCALE_METALFX;
#else
    return u == UPSCALE_OFF || u == UPSCALE_FSR1 || (u == UPSCALE_FSR3 && fsr3_available() && fsr3_upscaler_enabled()) ||
           (u == UPSCALE_DLSS && vk_dlss_available());
#endif
}

bool rt_fsr3(void) { return g_settings.upscaler == UPSCALE_FSR3 && upscaler_available(UPSCALE_FSR3); }

bool rt_dlss(void) { return g_settings.upscaler == UPSCALE_DLSS && upscaler_available(UPSCALE_DLSS); }

bool rt_temporal(void) { return rt_antialias() == AA_TAA || rt_fsr3() || rt_dlss(); }

double upscale_mode_factor(int q) {
    static const double f[UPQ_COUNT] = {1.3, 1.5, 1.7, 2.0, 3.0};
    return q == UPQ_COUNT ? 1.0 : q >= 0 && q < UPQ_COUNT ? f[q] : 1.5;  // (UPQ_COUNT: SAVAGEWIND_UPSCALE_AT fsr3-native)
}

// Rendering below the window's size: the 3D scene's render targets at their own scale (vulkan/textures.c). Metal has
// that only with SAVAGEWIND_SCALE_TARGETS=1 (untested with BOTW); SAVAGEWIND_NO_SCALE=1 turns it off (game sizes).
bool upscale_modes_available(void) {
    static int on = -1;
#ifdef __APPLE__
    if (on < 0) on = getenv("SAVAGEWIND_SCALE_TARGETS") && !getenv("SAVAGEWIND_NO_SCALE");
#else
    if (on < 0) on = !getenv("SAVAGEWIND_NO_SCALE");
#endif
    return on;
}

double upscale_factor(void) {
    int u = g_settings.upscaler;
    return u != UPSCALE_OFF && upscaler_available(u) && upscale_modes_available()
               ? upscale_mode_factor(g_settings.upscale_quality) : 1.0;
}

// The window's shape over 16:9 while the game's views are widened to fill it, else 1; the field of view's factor in
// use (rt_persp)
static volatile float g_view_shape = 1, g_fov = 1;
static uint32_t g_world_proj[4];  // tan(fovy / 2), aspect, near, far of the world camera's last matrix (rt_persp), float bits

void output_scale(double dw, double dh, bool reshape_allowed, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("SAVAGEWIND_SCALE") ? atof(getenv("SAVAGEWIND_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("SAVAGEWIND_WIDE_TEST") ? atof(getenv("SAVAGEWIND_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    // A wider window (21:9, 32:9) sees more to the sides, a taller one (16:10, 4:3: shape < 1) more above and below
    // (#125); the HUD keeps its 16:9 layout in the middle either way. The start screen and its menus, a 16:9 layout
    // over 16:9 pictures with no world to show more of, keep the 16:9 picture with bars, and so does the loading screen
    // (#98: widened, its tip went to the window's edges and its logo stayed in the 16:9 middle).
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005 && !framepace_title_ui() && !framepace_loading_ui();
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines: the window's (Metal: divided by the upscaler's factor)
#ifdef __APPLE__
    double out_div = upscale_factor(), scene_div = 1.0;
#else
    double out_div = 1.0, scene_div = upscale_factor();
#endif
    double h = forced > 0 ? forced : (reshape ? dh / 1080.0 : fit) / out_div;
    h = fmax(h, 0.1);  // (a tiny window)
    double sx = h * shape, sy = h;
    double cap = fmin(MAX_TEX / (1920.0 * sx), MAX_TEX / (1080.0 * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // the 3D scene (BOTW docked: 1600x900, composited into the 1920x1080 final image) at the final image's
    // resolution, divided by the upscaler's factor
    double k = 1080.0 / SCENE_LINES / scene_div;
    o->scene_sx = sx * k; o->scene_sy = sy * k;
    double scap = fmin(MAX_TEX / (1920.0 * o->scene_sx), MAX_TEX / (1080.0 * o->scene_sy));
    if (scap < 1.0) { o->scene_sx *= scap; o->scene_sy *= scap; }
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
    o->crop[0] = o->crop[1] = 0, o->crop[2] = o->crop[3] = 1;
    // Pause pages keep their 16:9 layout centered while the wider or taller image remains visible around them.
    g_view_shape = (float)o->shape;
    // Video > Field of view: in play only (cutscenes and talks keep the game's framing), eased over about 1/4 s so
    // that a talk's start and end don't jump
    bool event = framepace_in_event();
    float want = event ? 1.0f : g_settings.fov / 100.0f;
    static float logged = -1;
    static int log = -1;
    if (log < 0) log = getenv("SAVAGEWIND_LOG_PROJ") != NULL;
    if (log && want != logged) {
        logged = want;
        LOG("[fov] %.2f (setting %d%%, event %d)\n", want, g_settings.fov, (int)event);
    }
    static uint64_t last;
    uint64_t now = plat_now_ns();
    double dt = last ? (now - last) / 1e9 : 1;
    last = now;
    float ease = dt >= 0.25 ? 1.0f : (float)(dt / 0.25);
    g_fov = fabsf(want - g_fov) < 0.001f ? want : g_fov + (want - g_fov) * ease;
}

float output_view_shape(void) { return g_view_shape; }

// The game's perspective views, as each one's matrix is built (tools/patches.py _persp_matrix: sead's
// PerspectiveProjection; tan_fovy = tan(fovy / 2) at +0xac, aspect at +0xb0, as float bits; near at +0x98). Every 16:9
// view is widened to the window's shape, as every 16:9 render target is (vulkan/textures.c): a wider window sees more
// to the sides, the vertical view kept; a taller one (16:10, 4:3) more above and below, the horizontal view kept
// (tan(fovy / 2) / shape, #125). The HUD's and menus' layers are 16:9 views too, so they keep their shape in
// the middle. The field of view scales the world's
// camera only: its far plane is 25000 (its near plane moves with the camera's distance), the HUD's layers', the
// menus' 3D models' and the start screen's are 10000. SAVAGEWIND_LOG_PROJ=1: each kind of view the first time.
//
// The world's camera (far plane over 10000) also gets the widened values written back into its projection: widening
// only the matrix left tan(fovy / 2) and the aspect there 16:9 (and at the game's field of view), and the game's own
// uses of them - the sun's shadow cascades are fitted to that view - saw a narrower view than the one drawn. On an
// ultrawide window the shadows outside the 16:9 middle were missing or in the wrong place and slid as the camera
// turned (issue #103). What the game set is kept per projection (wide_cam), so the next build, a change of the window
// or of the field of view, and the screen-to-camera loads (tools/patches.py _persp_unproject) start from it again
// instead of widening twice. SAVAGEWIND_NO_WIDE_CAMERA_FIX=1: the matrix only, as before.
// The field of view also writes fovy and the sine and cosine of its half (+0xa0 / +0xa4 / +0xa8): the game copies the
// camera's projection with getFovy and setFovy (which makes tan from the angle), and the cascades are fitted to that
// copy; with tan alone a band of the far ground went dark and shadows were missing at the screen's edges above 100%
// (#162, #203). The game sets fovy again every frame, so it doesn't build up. SAVAGEWIND_NO_FOV_CAMERA_FIX=1: tan and
// aspect only.
static WorldProj g_world;
static pthread_mutex_t world_lock = PTHREAD_MUTEX_INITIALIZER;

void output_world_proj_set(float t, float a, float n, float f) {
    pthread_mutex_lock(&world_lock);
    g_world = (WorldProj){t, a, n, f, g_world.serial + 1};
    pthread_mutex_unlock(&world_lock);
}

bool output_world_proj(WorldProj *p) {
    pthread_mutex_lock(&world_lock);
    *p = g_world;
    pthread_mutex_unlock(&world_lock);
    return p->serial != 0;
}

static struct { uint64_t proj; uint32_t set_t, set_a, wrote_t, wrote_a; } wide_cam[16];
static unsigned wide_cam_next;
static pthread_mutex_t wide_cam_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_persp(uint64_t proj, uint32_t *tan_fovy, uint32_t *aspect) {
    float t, a, nf[2];
    memcpy(nf, (const void *)(uintptr_t)(proj + 0x98), 8);
    static int wide_fix = -1;
    if (wide_fix < 0) wide_fix = getenv("SAVAGEWIND_NO_WIDE_CAMERA_FIX") == NULL;
    static int wide_fov = -1;
    if (wide_fov < 0) wide_fov = getenv("SAVAGEWIND_NO_FOV_CAMERA_FIX") == NULL;
    bool world = nf[1] > 10001;
    if (world) {  // (values this wrote back: the game's own instead)
        pthread_mutex_lock(&wide_cam_lock);
        for (unsigned i = 0; i < 16; i++)
            if (wide_cam[i].proj == proj && wide_cam[i].wrote_t == *tan_fovy && wide_cam[i].wrote_a == *aspect) {
                *tan_fovy = wide_cam[i].set_t, *aspect = wide_cam[i].set_a;
                break;
            }
        pthread_mutex_unlock(&wide_cam_lock);
    }
    uint32_t set_t = *tan_fovy, set_a = *aspect;
    memcpy(&t, tan_fovy, 4), memcpy(&a, aspect, 4);
    static int log = -1;
    if (log < 0) log = getenv("SAVAGEWIND_LOG_PROJ") != NULL;
    if (log) {  // (by what it is, not where: many are copies on a stack)
        static float seen[128][4];
        static int n;
        float key[4] = {a, t, nf[0], nf[1]};
        bool known = false;
        for (int i = 0; i < n && !known; i++) known = !memcmp(seen[i], key, sizeof key);
        if (!known && n < 128) {
            memcpy(seen[n++], key, sizeof key);
            LOG("[proj] %#llx: aspect %.4f, fovy %.2f deg, near %g far %g\n", (unsigned long long)proj, a,
                2 * atanf(t) * 57.29578f, nf[0], nf[1]);
        }
    }
    if (fabsf(a - 16.0f / 9.0f) >= 0.01f) return;
    float fov = g_fov, shape = g_view_shape;
    if (fov != 1 && nf[1] > 10001) t = tanf(fminf(atanf(t) * fov, 1.4f));  // (under 160 degrees)
    a *= shape;
    if (shape < 1) t /= shape;  // (a taller window: the horizontal view kept, more above and below)
    memcpy(tan_fovy, &t, 4), memcpy(aspect, &a, 4);
    if (world) output_world_proj_set(t, a, nf[0], nf[1]);
    if (world && wide_fix && (*tan_fovy != set_t || *aspect != set_a)) {  // (the world's camera: written back)
        memcpy((void *)(uintptr_t)(proj + 0xac), tan_fovy, 4), memcpy((void *)(uintptr_t)(proj + 0xb0), aspect, 4);
        if (*tan_fovy != set_t && wide_fov) {  // fovy, sin and cos of its half too
            float h = atanf(t), fsc[3] = {2 * h, sinf(h), cosf(h)};
            memcpy((void *)(uintptr_t)(proj + 0xa0), fsc, 12);
        }
        pthread_mutex_lock(&wide_cam_lock);
        unsigned k = 16;
        for (unsigned i = 0; i < 16 && k == 16; i++)
            if (wide_cam[i].proj == proj) k = i;
        if (k == 16) k = wide_cam_next++ & 15;
        wide_cam[k].proj = proj, wide_cam[k].set_t = set_t, wide_cam[k].set_a = set_a;
        wide_cam[k].wrote_t = *tan_fovy, wide_cam[k].wrote_a = *aspect;
        pthread_mutex_unlock(&wide_cam_lock);
    }
    if (nf[1] > 10001) {  // the world camera's projection as made (vulkan/mvec.c checks the scene's matrices with it)
        float w[4] = {t, a, nf[0], nf[1]};
        uint32_t u[4];
        memcpy(u, w, sizeof u);
        for (int i = 0; i < 4; i++) __atomic_store_n(&g_world_proj[i], u[i], __ATOMIC_RELAXED);
    }
}

void rt_world_projection(float out[4]) {
    uint32_t u[4];
    for (int i = 0; i < 4; i++) u[i] = __atomic_load_n(&g_world_proj[i], __ATOMIC_RELAXED);
    memcpy(out, u, sizeof u);
}

// The game's frustum views (tools/patches.py _frustum_load: sead's FrustumProjection; left, right at +0xa8 / +0xac,
// top, bottom at +0xa0 / +0xa4): f = {left, right, top, bottom}, a 16:9 one widened to the window's shape as rt_persp
// does (some of the HUD's layers are these: the menus' cursor).
void rt_frustum(uint64_t proj, float *f) {
    float lr[2], tb[2];
    memcpy(lr, (const void *)(uintptr_t)(proj + 0xa8), 8);
    memcpy(tb, (const void *)(uintptr_t)(proj + 0xa0), 8);
    f[0] = lr[0], f[1] = lr[1], f[2] = tb[0], f[3] = tb[1];
    float w = lr[1] - lr[0], h = tb[0] - tb[1], shape = g_view_shape;
    if (shape == 1 || h == 0 || fabsf(fabsf(w / h) - 16.0f / 9.0f) >= 0.01f) return;
    if (shape > 1) {
        float c = (lr[0] + lr[1]) / 2;
        f[0] = c + (lr[0] - c) * shape, f[1] = c + (lr[1] - c) * shape;
    } else {  // (a taller window: top and bottom moved out)
        float c = (tb[0] + tb[1]) / 2;
        f[2] = c + (tb[0] - c) / shape, f[3] = c + (tb[1] - c) / shape;
    }
}

// Temporal anti-aliasing / upscaling jitter (Video > Anti-aliasing TAA, Upscaler FSR 3; SAVAGEWIND_JITTER=1 / 0 whatever
// the setting): the world camera's projection moved by a sub-pixel offset, a Halton (2, 3) sequence over 8 presented
// frames (FSR 3: its own count, rt_jitter_set_phases) (framepace_shown_frame: every matrix made in one frame gets the
// same, and the paused game's two updates a frame one phase), [-0.5, 0.5] pixels of the scene's render size
// (rt_jitter_set_size). It lands in the projection of the frame's
// camera block, where vulkan/mvec.c reads it back and takes it out of the motion vectors.
static uint32_t jitter_size[2];
void rt_jitter_set_size(uint32_t w, uint32_t h) {
    __atomic_store_n(&jitter_size[0], w, __ATOMIC_RELAXED);
    __atomic_store_n(&jitter_size[1], h, __ATOMIC_RELAXED);
}

bool rt_jitter_on(void) {
    static int env = -2;
    if (env == -2) env = getenv("SAVAGEWIND_JITTER") ? atoi(getenv("SAVAGEWIND_JITTER")) != 0 : -1;
    return env >= 0 ? env : rt_temporal();
}

// The jitter sequence's length: 8, or the temporal upscaler's (FSR 3's fsr3_jitter_phases, DLSS's; set by vulkan/exec.c)
static uint32_t jitter_phases = 8;
void rt_jitter_set_phases(uint32_t n) { __atomic_store_n(&jitter_phases, n ? n : 8, __ATOMIC_RELAXED); }

// Video > Anti-aliasing (settings.h AA_*). Test override: SAVAGEWIND_AA_AT=<frame>:<off|smaa|taa>,... from that
// presented frame on (same-view A/B in one run).
uint64_t renderer_present_count(void);
int rt_antialias(void) {
    static int n = -1;
    static unsigned long long at[8];
    static int mode[8];
    if (n < 0) {
        n = 0;
        for (const char *e = getenv("SAVAGEWIND_AA_AT"); e && *e && n < 8;) {
            char name[16] = "";
            if (sscanf(e, "%llu:%15[^,]", &at[n], name) != 2) break;
            mode[n++] = !strcmp(name, "taa") ? AA_TAA : !strcmp(name, "smaa") ? AA_SMAA : AA_OFF;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    int r = g_settings.antialias;
    uint64_t frame = n ? renderer_present_count() : 0;
    for (int i = 0; i < n; i++)
        if (frame >= at[i]) r = mode[i];
    return r;
}

static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

// A perspective view's offset (x, y: fractions of its width and height, as float bits; tools/patches.py
// _persp_matrix_offset), for a view rt_persp widens: the same distance in its 16:9 part (x / shape; on a taller window
// y x shape), so what the game moves to a side lines up with the 16:9 layout around it. The world camera's also gets
// the jitter.
void rt_persp_offset(uint64_t proj, uint32_t *x, uint32_t *y) {
    float a, ox, oy, shape = g_view_shape, nf[2];
    memcpy(&a, (const void *)(uintptr_t)(proj + 0xb0), 4);
    memcpy(nf, (const void *)(uintptr_t)(proj + 0x98), 8);
    memcpy(&ox, x, 4), memcpy(&oy, y, 4);
    if (shape != 1 && fabsf(a - 16.0f / 9.0f) < 0.01f) {
        if (shape > 1) ox /= shape;
        else oy *= shape;  // (a taller window: the vertical one, a fraction of the taller height)
    }
    uint32_t w = __atomic_load_n(&jitter_size[0], __ATOMIC_RELAXED), h = __atomic_load_n(&jitter_size[1], __ATOMIC_RELAXED);
    if (nf[1] > 10001 && rt_jitter_on() && w && h) {
        uint32_t n = __atomic_load_n(&jitter_phases, __ATOMIC_RELAXED);
        uint32_t i = (uint32_t)(framepace_shown_frame() % n) + 1;  // (from 1: Halton's 0 is 0)
        ox += (halton(i, 2) - 0.5f) / (float)w;
        oy += (halton(i, 3) - 0.5f) / (float)h;  // (the projection gets twice the offset: NDC is 2 wide)
    }
    memcpy(x, &ox, 4), memcpy(y, &oy, 4);
}

void output_scale_screen(double sw, double sh, int iw, int ih, OutScale *o) {
    double k = fmin(sw / iw, sh / ih), ox = (sw - iw * k) / 2, oy = (sh - ih * k) / 2;
    o->vx = ox + o->vx * k; o->vy = oy + o->vy * k;
    o->vw *= k; o->vh *= k;
}

// Console: the game's own size (BOTW: 896 x 896 per cascade); High 2048, Ultra 4096, Extreme 8192. Test override:
// SAVAGEWIND_SHADOW_AT=<frame>:<console|high|ultra|extreme|width>,... from that presented frame on (same-view A/B in one
// run).
unsigned shadow_map_size(unsigned long long frame) {
    static const unsigned size[SHADOW_COUNT] = {0, 2048, 4096, 8192};
    static int n = -1;
    static unsigned long long at[8];
    static unsigned w[8];
    if (n < 0) {
        n = 0;
        for (const char *e = getenv("SAVAGEWIND_SHADOW_AT"); e && *e && n < 8;) {
            char name[16] = "";
            if (sscanf(e, "%llu:%15[^,]", &at[n], name) != 2) break;
            w[n++] = !strcmp(name, "console") ? 0 : !strcmp(name, "high") ? 2048 : !strcmp(name, "ultra") ? 4096
                   : !strcmp(name, "extreme") ? 8192 : (unsigned)atoi(name);
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    unsigned r = g_settings.shadow_res >= 0 && g_settings.shadow_res < SHADOW_COUNT ? size[g_settings.shadow_res] : 0;
    for (int i = 0; i < n; i++)
        if (frame >= at[i]) r = w[i];
    return r > 8192 ? 8192 : r;
}
