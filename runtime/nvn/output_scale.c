// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size (the
// final image: the window's resolution, so the HUD and menus are sharp), the 3D scene's (1600x900 docked) at
// (scene_sx, scene_sy) times theirs (vulkan/textures.c RS_OUT / RS_SCENE); the image is then drawn into
// (vx, vy, vw, vh) of the window. Video page "Upscaler" (g_settings.upscaler):
//   Off            the 3D scene renders at the window's own pixel size (its 16:9 part: the game keeps its framing)
//   AMD FSR 1      the 3D scene renders smaller by the quality mode's factor per axis (AMD's: Ultra Quality 1.3,
//                  Quality 1.5, Balanced 1.7, Performance 2.0); FSR 1 brings it to its size in the final image at the
//                  game's composite (vulkan/exec.c scene_upscale)
//   AMD FSR 3.1    (Vulkan, with amd_fidelityfx_vk.dll) the same render sizes, plus Native AA (1x) and Ultra
//                  Performance (3x); the jittered scene, its depth and motion vectors are resolved into the final
//                  image's size at the composite (vulkan/exec.c scene_resolve)
//   MetalFX        (macOS) metal.m has no scene / final split yet: the modes stay greyed there, and MetalFX scales the
//                  game's image to a bigger window
//   DLSS           listed as coming later
// The window size itself is Video > Window size (runtime/window_size.c).
// Test overrides: SAVAGEWIND_SCALE=<f> (render height = 1080 x f), SAVAGEWIND_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"

#define MAX_TEX 16384.0
#define SCENE_LINES 900.0  // the height of the game's 3D scene targets (docked)

#ifndef __APPLE__
bool fsr3_available(void);  // (vulkan/fsr3.cpp: the AMD FidelityFX DLL loaded)
#endif

bool upscaler_available(int u) {
#ifdef __APPLE__
    return u == UPSCALE_OFF || u == UPSCALE_METALFX;
#else
    return u == UPSCALE_OFF || u == UPSCALE_FSR1 || (u == UPSCALE_FSR3 && fsr3_available());
#endif
}

// whether upscaler u offers mode q (Native AA and Ultra Performance: FSR 3.1 only)
bool upscale_mode_offered(int u, int q) {
    return q >= 0 && q < UPQ_COUNT && (u == UPSCALE_FSR3 || (q != UPQ_NATIVE && q != UPQ_ULTRA_PERF));
}

// The modes from the sharpest to the fastest (the menus' order)
const int upscale_mode_order[UPQ_COUNT] = {UPQ_NATIVE, UPQ_ULTRA, UPQ_QUALITY, UPQ_BALANCED, UPQ_PERFORMANCE, UPQ_ULTRA_PERF};

double upscale_mode_factor(int q) {
    static const double f[UPQ_COUNT] = {1.3, 1.5, 1.7, 2.0, 1.0, 3.0};
    return q >= 0 && q < UPQ_COUNT ? f[q] : 1.5;
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
    int u = g_settings.upscaler, q = g_settings.upscale_quality;
    if (!upscale_mode_offered(u, q)) q = UPQ_QUALITY;  // (FSR 1 with a mode of FSR 3.1's: its nearest, Quality)
    return u != UPSCALE_OFF && upscaler_available(u) && upscale_modes_available() ? upscale_mode_factor(q) : 1.0;
}

void output_scale(double dw, double dh, bool reshape_allowed, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("SAVAGEWIND_SCALE") ? atof(getenv("SAVAGEWIND_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("SAVAGEWIND_WIDE_TEST") ? atof(getenv("SAVAGEWIND_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
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
}

void output_scale_screen(double sw, double sh, int iw, int ih, OutScale *o) {
    double k = fmin(sw / iw, sh / ih), ox = (sw - iw * k) / 2, oy = (sh - ih * k) / 2;
    o->vx = ox + o->vx * k; o->vy = oy + o->vy * k;
    o->vw *= k; o->vh *= k;
}
