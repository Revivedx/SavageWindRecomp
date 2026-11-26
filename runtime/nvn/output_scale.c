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
#include "../window_size.h"  // (winsize_refresh_rate: the display's rate, the dynamic resolution's default target)
#include "output_scale.h"

#ifndef SW_METAL
extern bool vk_dlssg_available(void);
extern int vk_dlssg_max_mult(void);
extern bool vk_dlss_available(void);
#endif

#define MAX_TEX 16384.0
#define SCENE_LINES 900.0  // the height of the game's 3D scene targets (docked)

#ifndef SW_METAL
bool fsr3_available(void);  // (vulkan/fsr3.h: built in)
bool fsr4_available(void);  // (vulkan/fsr_api.h: AMD's DLLs, on an AMD Radeon)
bool xess_available(void);  // (vulkan/xess.h: libxess.dll next to the game)
bool xess_usable(void);     // (the context made, and not failed this run)
#endif
// FSR 3 and FSR 4 upscaling were held back from 0.5.0 until the motion vectors left the render thread (recording them
// there cost 1.2 ms a frame: RTX 3060, 1440p Quality, 130 fps native, 104 with FSR 3). Offered again since they are
// worked out on a thread of their own (vulkan/mvec.c) and the shadow cascades follow the upscaler's factor
// (shadow_map_size): same PC, Savage preset, 1440p, FSR 3 Quality over native at the Great Plateau 106 / 102,
// Kakariko 106 / 88, Korok Forest 67 / 40. SAVAGEWIND_FSR3=0 hides them again (a settings file with fsr3 or fsr4 then
// loads as FSR 1 with the same mode).
bool fsr3_upscaler_enabled(void) {
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_FSR3") || atoi(getenv("SAVAGEWIND_FSR3")) != 0;
    return on;
}

bool upscaler_available(int u) {
#ifdef SW_METAL
    return u == UPSCALE_OFF || u == UPSCALE_METALFX;
#else
    return u == UPSCALE_OFF || u == UPSCALE_FSR1 || (u == UPSCALE_FSR3 && fsr3_available() && fsr3_upscaler_enabled())
        || (u == UPSCALE_FSR4 && fsr4_available() && fsr3_upscaler_enabled())  // (FSR 4: the same switch as FSR 3)
        || (u == UPSCALE_DLSS && vk_dlss_available())
        || (u == UPSCALE_XESS && xess_available());
#endif
}

// FSR 4 is on this PC (an AMD Radeon, AMD's DLLs found), whether or not it is held back (the menu says which)
bool fsr4_offered(void) {
#ifdef SW_METAL
    return false;
#else
    return fsr4_available();
#endif
}

bool frame_gen_available(int fg) {
#ifdef SW_METAL
    return fg == FG_OFF;
#else
    return fg == FG_OFF || fg == FG_FSR3 || (fg == FG_DLSS && vk_dlssg_available());
#endif
}

bool frame_gen_mult_available(int fg, int mult) {
#ifdef SW_METAL
    return mult == FG_MULT_MIN;
#else
    if (mult == FG_MULT_MIN) return true;
    return fg == FG_DLSS && frame_gen_available(FG_DLSS) && mult <= vk_dlssg_max_mult();
#endif
}

int frame_gen_mult(int fg) {
    static int forced = -1;  // SAVAGEWIND_FG_MULT=2|3|4 overrides the setting (tests)
    if (forced < 0) forced = getenv("SAVAGEWIND_FG_MULT") ? atoi(getenv("SAVAGEWIND_FG_MULT")) : 0;
    int m = forced ? forced : g_settings.frame_gen_mult;
    if (m < FG_MULT_MIN) m = FG_MULT_MIN;
    if (m > FG_MULT_MAX) m = FG_MULT_MAX;
    while (m > FG_MULT_MIN && !frame_gen_mult_available(fg, m)) m--;
    return m;
}

bool rt_fsr4(void) { return g_settings.upscaler == UPSCALE_FSR4 && upscaler_available(UPSCALE_FSR4); }
// FSR 4 takes the same inputs and does the same jobs (anti-aliasing, sharpening) as FSR 3: everything that asks for
// FSR 3 asks for either; vulkan/exec.c fsr3_scene picks the one to run
bool rt_fsr3(void) { return (g_settings.upscaler == UPSCALE_FSR3 && upscaler_available(UPSCALE_FSR3)) || rt_fsr4(); }

// Intel XeSS (vulkan/xess.h): the same temporal inputs as FSR 3 (mvec's vectors, the depth, a jittered camera). It
// runs on any GPU with DP4a, with libxess.dll next to the game.
bool rt_xess(void) { return g_settings.upscaler == UPSCALE_XESS && xess_usable(); }

bool rt_dlss(void) { return g_settings.upscaler == UPSCALE_DLSS && upscaler_available(UPSCALE_DLSS); }

bool rt_temporal(void) { return rt_antialias() == AA_TAA || rt_fsr3() || rt_dlss() || rt_xess(); }

// quality modes, AMD's scale factors (per axis): Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0,
// Ultra Performance 3.0, Native 1.0 (FSR 3 / FSR 4). Dynamic is the frame-rate-following mode (see the governor
// below).
double dynamic_factor(void);  // the factor the dynamic governor has settled on

double upscale_mode_factor(int q) {
    static const double f[UPQ_COUNT] = {1.3, 1.5, 1.7, 2.0, 3.0, 1.0, 1.0};  // (Dynamic: never used, see upscale_factor)
    return q >= 0 && q < UPQ_COUNT ? f[q] : 1.5;
}

// Rendering below the window's size: the 3D scene's render targets at their own scale (vulkan/textures.c). Metal has
// that only with SAVAGEWIND_SCALE_TARGETS=1 (untested with BOTW); SAVAGEWIND_NO_SCALE=1 turns it off (game sizes).
bool upscale_modes_available(void) {
    static int on = -1;
#ifdef SW_METAL
    if (on < 0) on = getenv("SAVAGEWIND_SCALE_TARGETS") && !getenv("SAVAGEWIND_NO_SCALE");
#else
    if (on < 0) on = !getenv("SAVAGEWIND_NO_SCALE");
#endif
    return on;
}

// Ultra Performance renders a third of the window's size on each side. A temporal upscaler (FSR 3 / 4, XeSS, DLSS) is
// made to bring that to 2160 lines or more; at 1440 the scene is 853 x 480 and the whole image comes out blurred, for
// hardly more frames than Performance where the CPU is the limit. So it is offered from a 4K window on, and below that
// a stored Ultra Performance runs as Performance (the setting is kept: it comes back on a 4K window). FSR 1 keeps it at
// any size. SAVAGEWIND_UPSCALE_ANY=1: offered at any size, as before.
// DLSS's Native mode is NVIDIA's DLAA, and the menus call it so (as other games list it among DLSS's modes)
bool upscale_mode_is_dlaa(int q) { return q == UPQ_NATIVE && g_settings.upscaler == UPSCALE_DLSS; }

static volatile int g_out_lines;  // the window's height (output_scale; 0: not known yet)
bool upscale_mode_offered(int q) {
    if (q == UPQ_DYNAMIC) return true;  // (the dynamic resolution: one of every upscaler's modes)
    // (every temporal upscaler: XeSS reconstructs from the same inputs, so its 1x is anti-aliasing too - #820 knew only
    //  of FSR 3 and FSR 4, which were the ones there then; DLSS's 1x is NVIDIA's DLAA, and the menus name it so)
    if (q == UPQ_NATIVE) return rt_temporal_upscaler();
    static int any = -1;
    if (any < 0) any = getenv("SAVAGEWIND_UPSCALE_ANY") && atoi(getenv("SAVAGEWIND_UPSCALE_ANY")) != 0;
    if (q != UPQ_ULTRA_PERFORMANCE || any || !(rt_fsr3() || rt_xess() || rt_dlss())) return true;
    return !g_out_lines || g_out_lines >= 2160;
}

int upscale_quality_now(void) {
    int q = g_settings.upscale_quality;
    return upscale_mode_offered(q) ? q : (q == UPQ_NATIVE || q == UPQ_DYNAMIC ? UPQ_QUALITY : UPQ_PERFORMANCE);
}

// The temporal upscalers. They anti-alias the scene themselves (from several frames): while one is on, Video >
// Anti-aliasing is off, the game's FXAA and SMAA included (vulkan/exec.c), and the menus show the row greyed with the
// upscaler's name. The setting is kept.
bool rt_temporal_upscaler(void) { return rt_fsr3() || rt_xess() || rt_dlss(); }
const char *rt_temporal_upscaler_name(void) {
    return rt_fsr4() ? "FSR 4" : rt_fsr3() ? "FSR 3" : rt_xess() ? "XeSS" : rt_dlss() ? "DLSS" : NULL;
}

// FSR 3 / 4, XeSS and DLSS take no scene larger than the window (vulkan/exec.c scene_post skips them then, and the
// camera's jitter, there for them, would reach the screen unresolved): with one of them the render scale goes up to
// 100%, and in its Native mode (DLAA with DLSS), the window's own size by definition, it is 100%. Picking one of them
// or Native in a menu sets the render scale to 100% (render_scale_fit), and leaving them for a choice that offers the
// value it had puts it back (so stepping through the rows on the way to another value loses nothing); a stored value
// they don't offer otherwise (the ini edited, DLSS's library added later) runs as 100%.
bool render_scale_offered(int r) {
    if (r == RSCALE_100 || !rt_temporal_upscaler()) return true;
    return r < RSCALE_100 && upscale_quality_now() != UPQ_NATIVE;
}

static int fitted_from = -1;  // the render scale render_scale_fit replaced with 100% (this run only; -1: none)

void render_scale_fit(void) {
    int r = g_settings.render_scale;
    if (!render_scale_offered(r)) {
        if (fitted_from < 0) fitted_from = r;
        g_settings.render_scale = RSCALE_100;
    } else if (fitted_from >= 0 && r == RSCALE_100 && render_scale_offered(fitted_from)) {
        g_settings.render_scale = fitted_from, fitted_from = -1;
    }
}

void render_scale_chosen(void) { fitted_from = -1; }

int render_scale_now(void) {
    int r = g_settings.render_scale;
    return r < 0 || r >= RSCALE_COUNT ? RSCALE_100 : render_scale_offered(r) ? r : RSCALE_100;
}

double upscale_factor(void) {
    int u = g_settings.upscaler;
    if (u == UPSCALE_OFF || !upscaler_available(u) || !upscale_modes_available()) return 1.0;
    // (the Dynamic mode: the factor follows the frame rate, dynamic_res_tick below)
    if (upscale_quality_now() == UPQ_DYNAMIC) return dynamic_factor();
    return upscale_mode_factor(upscale_quality_now());
}

// ---- Dynamic resolution: the upscaler's Dynamic mode
//
// One place only: Video > Upscaler mode > Dynamic, with any upscaler (FSR 1 runs on every graphics card). It moves the
// upscaler's factor (upscale_factor above), so the HUD and menus stay at the window's resolution. With no upscaler
// there is no dynamic resolution: a separate switch that scaled the scene on its own, and offered the Dynamic mode
// only while on, was two controls for one thing (removed 2026-11-18). Off by default; the Potato preset picks it.
//
// The game's own mechanism (exec.c CMD_REPORT_COUNTER) is the other way of doing this: it times its GPU work with the
// console's counters and picks between two fixed sizes from that. On a PC those counters measure our command
// translation, not the GPU's work, so they are frozen by default and the setting had nothing to do unless it was
// unfrozen by hand. This one is driven by the frame rate instead - real frames with frame generation off, presented
// frames (the generated ones included) with it on, since that is what the player sees and what the upscaler's output
// is paced at.
//
// Drawing fewer pixels only helps where the GPU is the limit. On a CPU-bound machine (most of this project's 1080p
// and 1440p runs) it changes nothing and only makes the picture softer. GPU load readings are unreliable across
// vendors, so instead of guessing the limiter this measures it: each step's settled frame rate is kept in a small
// table, and a step smaller is only kept if it actually raised the rate over the step above it. The first step that
// does not is undone, and the scale goes back to full resolution and stays there for the rest of the run. That is
// what makes this a feature on a GPU-bound machine and a no-op on the machine that withdrew #852, without a hardware
// whitelist. A step is judged only after DYN_RES_HOLD frames, so a load spike or a shader compile cannot shrink the
// picture. SAVAGEWIND_DYNAMIC_LOG=1: every step and what it decided.
#define DYN_RES_STEPS 8        // 1.0 (Native) down to 3.0x, the upscaler's scale factors
#define DYN_RES_HOLD 60        // presented frames a step is held before its rate is judged (1 s at 60 fps)
#define DYN_RES_SHRINK 0.92    // under this share of the target, try one step smaller
#define DYN_RES_GROW 1.10      // over it, one step sharper
#define DYN_RES_GAIN 1.05      // a step smaller has to gain at least this share of the rate to be kept
static const double k_dyn_res_factors[DYN_RES_STEPS] = {1.0, 1.3, 1.5, 1.7, 2.0, 2.4, 2.7, 3.0};
static volatile int dyn_res_step;         // 0: Native, dyn_res_max_step(): smallest

// The smallest step allowed: the scene never smaller than a third of the window's lines, Ultra Performance's size at
// a render scale of 100%. Below 100% that stops it earlier: the Potato preset (50%, Dynamic) goes from half the lines
// down to its fixed size before (Quality, 1.5x), never under it; 213x120 at 1280x720 otherwise.
static int dyn_res_max_step(void) {
    static const double ss_f[RSCALE_COUNT] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};  // (output_scale's Render scale)
    int rsi = render_scale_now();
    double most = 3.0 * (rsi >= 0 && rsi < RSCALE_COUNT ? ss_f[rsi] : 1.0) + 1e-6;
    int s = 0;
    while (s + 1 < DYN_RES_STEPS && k_dyn_res_factors[s + 1] <= most) s++;
    return s;
}
// The factor the governor has settled on: upscale_factor and the F10 overlay read it.
double dynamic_factor(void) {
    int s = dyn_res_step;
    return s >= 0 && s < DYN_RES_STEPS ? k_dyn_res_factors[s] : 1.0;
}
static int dyn_res_hold;
static float dyn_res_rate[DYN_RES_STEPS];  // the settled rate measured at each step (0: not measured)
static int dyn_res_probe_step = -1;        // the step whose gain is being measured (-1: none)
static bool dyn_res_no_gain;               // a step smaller gained nothing: the CPU is the limit, stay at Native
static int dyn_res_floor = -1;             // the last step that gained frames: never smaller (-1: not found yet)
static int dyn_res_logged;                 // the "stay at Native" decision is logged once, not every hold
static bool dyn_fg_disabled;               // frame generation had no net gain: turned off for this run
static bool dyn_fg_no_gain;                // the same verdict, outside the Dynamic mode (a hint, not an action)
static float dyn_fg_baseline;              // real fps measured with frame generation off, before it was enabled
static int dyn_fg_hold;                    // frames frame generation has been on and measured
static bool dyn_fg_was_on;

// Whether frame generation was switched off by the dynamic mode for having no net gain. Never touches the setting:
// fg_wanted() simply reads this, so the menu still shows what the player chose and a reboot gets a fresh chance.
bool dynamic_fg_disabled(void) { return dyn_fg_disabled; }
// The same verdict, outside the Dynamic mode: nothing is switched off, but the frame generation row and the F10 overlay can say that it is costing frames without a net gain (see dynamic_fg_check).
bool dynamic_fg_no_gain(void) { return dyn_fg_no_gain; }

// Whether the Dynamic mode is in use: an upscaler running with it as its mode. Read by the F10 overlay
// (perf_overlay_draw) and the frame generation verdict.
bool dynamic_res_active(void) {
    int u = g_settings.upscaler;
    return u != UPSCALE_OFF && upscaler_available(u) && upscale_modes_available() &&
           upscale_quality_now() == UPQ_DYNAMIC;
}

// The target frame rate: the display's, or the game's own limit when that is lower (a 60 limit on a 160 Hz screen
// aims at 60, not 160). Uncapped with no display rate: 60, the console's own. The target only decides when to try
// a smaller step and when there is headroom to go sharper; whether a step is kept is decided by the measured gain,
// so a target the machine cannot reach does not walk the scale down forever.
double dynamic_res_target(void) {
    int lim = g_settings.fps_limit;  // (0: no limit)
    float hz = winsize_refresh_rate();
    double target = lim > 0 ? (double)lim : hz > 1 ? (double)hz : 60.0;
    if (lim > 0 && hz > 1 && (double)hz < target) target = (double)hz;
    return target;
}

// Keeps the governor's step in line with the setting, once per presented frame (dynamic_res_tick). Returns true while
// SAVAGEWIND_DYNAMIC_RES_STEP=<0..7> holds one step, for testing sizes without waiting for the frame rate.
static bool dynamic_res_sync(void) {
    // (the mode was left and picked again, or the upscaler changed: the rates measured before belong to another
    // configuration, so start over at full size with a fresh chance to measure the limiter)
    static bool last_active;
    bool active = dynamic_res_active();
    if (last_active && !active) dynamic_res_reset();
    last_active = active;
    if (!active) { dyn_res_step = 0; return false; }
    static int forced = -2;
    if (forced == -2) {
        const char *e = getenv("SAVAGEWIND_DYNAMIC_RES_STEP");
        forced = e ? atoi(e) : -1;
        if (forced >= DYN_RES_STEPS) forced = DYN_RES_STEPS - 1;
    }
    if (forced < 0) return false;
    dyn_res_step = forced < dyn_res_max_step() ? forced : dyn_res_max_step();
    return true;
}

// Once per presented frame, from objects.c: fps is the real rendered rate, fps_generated the generated frames per
// second, fg_on whether frame generation is in use. The rate the governor acts on is the one the player sees: real
// frames with frame generation off, presented frames (the generated ones included) with it on - the upscaler's output
// is paced at that rate, so its Dynamic mode has to be too.
//
// Frame generation is not free: on a CPU-bound machine it can lower the real frame rate enough that even doubled
// presentation loses. Its net gain is judged against the real rate measured while it was off: when it is on, the
// presented rate (real + generated) has to beat that baseline by DYN_FG_GAIN.
//
// The verdict is the same whatever the upscaler's mode is, but what is done with it is not: in the Dynamic mode, frame
// generation is switched off for the rest of the run (dynamic_fg_disabled, read by device.c fg_wanted) - that is part
// of what the mode is for. In any other mode nothing is switched off; dynamic_fg_no_gain is just a hint shown next to
// the frame generation setting and in the F10 overlay. Either way g_settings.frame_gen is untouched:
// the setting stays as the player left it, a reboot starts a fresh measurement, frame generation that was on from
// the very first frame has no baseline and is left alone, and turning it off by hand is not a verdict.
#define DYN_FG_GAIN 1.15       // presented rate has to beat the no-FG rate by at least this to be worth it
#define DYN_FG_HOLD 90         // presented frames FG is left on before its net gain is judged (1.5 s at 60 fps)
static void dynamic_fg_check(float fps, float fps_generated, bool fg_on) {
    if (dyn_fg_disabled || dyn_fg_no_gain) return;  // (the verdict is for the run; a reboot clears it)
    if (!fg_on) {
        if (fps > 0) dyn_fg_baseline = fps;  // (the rate without FG: the reference for the next time it is on)
        dyn_fg_hold = 0;
        dyn_fg_was_on = false;
        return;
    }
    bool first = !dyn_fg_was_on;
    dyn_fg_was_on = true;
    if (first && dyn_fg_baseline <= 0) return;  // (FG was on from the start: no baseline, leave it alone)
    if (++dyn_fg_hold < DYN_FG_HOLD) return;
    dyn_fg_hold = 0;
    float presented = fps + fps_generated;
    if (dyn_fg_baseline > 0 && presented < dyn_fg_baseline * DYN_FG_GAIN) {
        if (dynamic_res_active()) {
            dyn_fg_disabled = true;
            LOG("[dynamic] frame generation off: %.0f presented fps against %.0f without it (no net gain)\n", presented,
                dyn_fg_baseline);
        } else {
            dyn_fg_no_gain = true;  // (not the Dynamic mode: a hint in the menu and the overlay, nothing switched off)
            LOG("[dynamic] frame generation has no net gain: %.0f presented fps against %.0f without it (hint only)\n",
                presented, dyn_fg_baseline);
        }
    } else {
        // (it is paying for itself: keep the comparison fresh, but never above what the game can really draw)
        dyn_fg_baseline = fmaxf(dyn_fg_baseline, fps);
    }
}

void dynamic_res_tick(float fps, float fps_generated, bool fg_on) {
    dynamic_fg_check(fps, fps_generated, fg_on);
    if (dynamic_res_sync()) return;  // (keep dyn_res_step in line with the upscaler's mode)
    // (the first seconds are loading, shader compilation and pipeline builds: the frame rate there says nothing
    // about the scene's cost. Skip them, and everything after a reboot, which resets the counter.)
    if (framepace_shown_frame() < 180) { dyn_res_hold = 0; return; }
    if (!dynamic_res_active() || fps <= 0) { dyn_res_hold = 0; return; }
    // (only in play: the title screen, loading screens, menus and cutscenes cost the GPU little or nothing of the
    // scene's, so a step judged there gains nothing. On the title it was judged "CPU-bound" at 141 fps and held at
    // full size for the whole run, while the world at 1440p, render scale 200%, ran GPU-bound at 58 fps)
    if (framepace_title_ui() || framepace_loading_ui() || framepace_menu_ui() || framepace_map_ui() ||
        framepace_in_event()) {
        dyn_res_hold = 0;
        if (dyn_res_probe_step >= 0) dyn_res_step = dyn_res_probe_step - 1, dyn_res_probe_step = -1;  // (judged later)
        return;
    }
    // (the rate to hold: what is presented. With frame generation on that includes the generated frames.)
    float presented = fps + (fg_on ? fps_generated : 0.0f);
    double target = dynamic_res_target();
    if (target <= 0) return;
    if (++dyn_res_hold < DYN_RES_HOLD) return;
    dyn_res_hold = 0;
    float ratio = (float)(presented / target);
    int before = dyn_res_step, max_step = dyn_res_max_step();
    const char *why = "";
    dyn_res_rate[dyn_res_step] = presented;  // (settled rate at the step we have been holding)
    if (dyn_res_step > max_step) {  // (Render scale lowered since: back to the smallest size it allows)
        dyn_res_step = max_step;
        dyn_res_probe_step = -1;
        why = " (render scale: the smallest allowed)";
    } else if (dyn_res_probe_step >= 0) {
        // (a step smaller was taken DYN_RES_HOLD frames ago: this is its settled rate. Keep it only if it gained
        // frames over the step it came from - that is the whole test of whether the GPU is the limit here.)
        int probe = dyn_res_probe_step;
        dyn_res_probe_step = -1;
        if (dyn_res_rate[probe] < dyn_res_rate[probe - 1] * DYN_RES_GAIN) {
            // (no gain: the step before is where the GPU stopped being the limit. Back to it and never smaller -
            // all the way back to Native threw away the steps that had gained: 1440p at 200%, 53 fps at 1.0x, 75 at
            // 1.3x, 73 at 1.5x, and it went back to 53. Only when the first step gains nothing is it the CPU.)
            dyn_res_step = dyn_res_floor = probe - 1;
            dyn_res_no_gain = dyn_res_floor == 0;
            why = dyn_res_no_gain ? " (no gain: CPU-bound, back to Native)"
                                  : " (no gain smaller: back one step, the smallest from now on)";
        } else {
            why = " (kept: it gained frames)";
        }
    } else if (dyn_res_no_gain) {
        if (dyn_res_step != 0) dyn_res_step = 0, why = " (CPU-bound: Native)";
        // (stay at Native for the rest of the run: drawing smaller cannot help here. No more log lines for it.)
    } else if (ratio > DYN_RES_GROW && dyn_res_step > 0) {
        dyn_res_step--;  // (clear of the target: room to be sharper)
        dyn_res_probe_step = -1;
        why = " (headroom)";
    } else if (ratio >= DYN_RES_SHRINK && dyn_res_step > 0 && dyn_res_probe_step < 0) {
        // (at the target, or close enough that a smaller step is not called for: drift back towards full resolution
        // one step at a time. The start-up transients may have taken a few steps down; this walks them back once the
        // scene has settled, without a gain test that would need another step to compare against.)
        dyn_res_step--;
        why = " (at target: sharper)";
    } else if (dyn_res_step < max_step && (dyn_res_floor < 0 || dyn_res_step < dyn_res_floor) &&
               ratio < DYN_RES_SHRINK) {
        dyn_res_probe_step = ++dyn_res_step;  // (well under the target: try one step smaller, judge it next time)
        why = " (trying smaller)";
    }
    if ((dyn_res_step != before || (dyn_res_no_gain && !dyn_res_logged)) && getenv("SAVAGEWIND_DYNAMIC_LOG")) {
        LOG("[dynamic-res] %.0f fps (%s) at a target of %.0f: %.2fx -> %.2fx%s\n", presented,
            fg_on ? "presented" : "real", target, k_dyn_res_factors[before], k_dyn_res_factors[dyn_res_step], why);
        if (dyn_res_no_gain) dyn_res_logged = 1;
    }
}

// A new window or boot: the scale starts over at full size, with a fresh chance to measure the limiter. The frame
// generation decision is NOT reset by a resize - only a reboot (backend reset) should give it another chance.
void dynamic_res_reset(void) {
    dyn_res_step = 0;
    dyn_res_hold = 0;
    dyn_res_probe_step = -1;
    dyn_res_no_gain = false;
    dyn_res_floor = -1;
    dyn_res_logged = 0;
    memset((void *)dyn_res_rate, 0, sizeof dyn_res_rate);
}

// The window's shape over 16:9 while the game's views are widened to fill it, else 1; the field of view's factor in
// use (rt_persp)
static volatile float g_view_shape = 1, g_fov = 1, g_map_extra_y;
static volatile float g_window_shape = 1;  // the window's shape over 16:9, widened or not (output_window_shape)
static uint32_t g_world_proj[4];  // tan(fovy / 2), aspect, near, far of the world camera's last matrix (rt_persp), float bits

void output_scale(double dw, double dh, bool reshape_allowed, OutScale *o) {
    {   // (the window's size changed: the dynamic resolution's steps were chosen for the old one, start over)
        static double last_w, last_h;
        if (last_w > 0 && (dw != last_w || dh != last_h)) dynamic_res_reset();
        last_w = dw, last_h = dh;
    }
    g_out_lines = (int)dh;
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("SAVAGEWIND_SCALE") ? atof(getenv("SAVAGEWIND_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("SAVAGEWIND_WIDE_TEST") ? atof(getenv("SAVAGEWIND_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    g_window_shape = (float)shape;
    // A wider window (21:9, 32:9) sees more to the sides, a taller one (16:10, 4:3: shape < 1) more above and below
    // (#125); the HUD keeps its 16:9 layout in the middle either way. Taller title screens fill the window too.
    // Fit wider title/loading layouts to keep their artwork and text aligned (#98). Taller views keep their shape
    // through the loading overlay so the world doesn't change aspect while fading out or back in.
    // The frames before the start screen first comes up count as it too (#805): the game makes its layout projection
    // there, once, and widened then (rt_persp) it kept the window's aspect on the 16:9 start screen, whose artwork came
    // out 1/shape as wide (a strip in the middle on 32:9).
    static bool title_seen;
    if (framepace_title_ui()) title_seen = true;
    bool title_fit = framepace_title_ui() || !title_seen;
#ifndef SW_METAL
    title_fit = title_fit && shape >= 1;  // Vulkan cover-crops the artwork on taller title screens.
#endif
    extern volatile int g_movie_playing;  // runtime/hle/movie.c: pre-rendered 16:9 cutscenes
    bool movie = __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED) > 0;
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005 && !title_fit && !movie &&
                   (shape < 1 || !framepace_loading_ui());
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines: the window's (Metal: divided by the upscaler's factor)
    // Video > Render scale: above 100% everything renders bigger than the window and is scaled down to it
    // (supersampling: the cleanest anti-aliasing, at the GPU's cost); below it smaller, scaled up (faster, blurrier).
    // MAX_TEX below still caps it. On Vulkan, only the 3D scene (RS_SCENE) scales with render_scale so the UI/HUD
    // stays sharp at the output resolution (#746).
    static const double ss_f[RSCALE_COUNT] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    int rsi = render_scale_now();
    double ss = rsi >= 0 && rsi < RSCALE_COUNT ? ss_f[rsi] : 1.0;
#ifdef SW_METAL
    double out_div = upscale_factor(), scene_div = 1.0, ui_ss = ss, scene_ss = 1.0;
#else
    double out_div = 1.0, scene_div = upscale_factor(), ui_ss = 1.0, scene_ss = ss;
#endif
    // Near-16:9 integer resolutions like 1366x768 (768 * 16/9 = 1365.33) would otherwise render 1365px wide and
    // resample at a sub-pixel horizontal offset into the 1366px window (#770); snap <2px rounding to exact 1:1.
    bool near_fit = !reshape && forced <= 0 && wide_test <= 0 &&
                    fabs(dw - 1920.0 * fit) < 2.0 && fabs(dh - 1080.0 * fit) < 2.0;
    double h = forced > 0 ? forced : (reshape ? dh / 1080.0 : fit) / out_div * ui_ss;
    h = fmax(h, 0.1);  // (a tiny window)
    double sx = near_fit ? fmax((dw / 1920.0) / out_div * ui_ss, 0.1) : h * shape;
    double sy = near_fit ? fmax((dh / 1080.0) / out_div * ui_ss, 0.1) : h;
    double cap = fmin(MAX_TEX / (1920.0 * sx), MAX_TEX / (1080.0 * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // the 3D scene (BOTW docked: 1600x900, composited into the 1920x1080 final image) at the final image's
    // resolution, scaled by Video > Render scale (on Vulkan) and divided by the upscaler's factor (in its Dynamic mode,
    // the one the frame rate sets: upscale_factor)
    double k = 1080.0 / SCENE_LINES / scene_div * scene_ss;
    o->scene_sx = sx * k; o->scene_sy = sy * k;
    double scap = fmin(MAX_TEX / (1920.0 * o->scene_sx), MAX_TEX / (1080.0 * o->scene_sy));
    if (scap < 1.0) { o->scene_sx *= scap; o->scene_sy *= scap; }
    // where the image goes in the window: fills it when reshaped (or near-16:9 snapped), else a fitted 16:9 box
    if (reshape || near_fit) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
    o->crop[0] = o->crop[1] = 0, o->crop[2] = o->crop[3] = 1;
    bool tall_map = output_tall_map((float)shape);
    g_map_extra_y = tall_map ? (float)(360 * (1 / shape - 1)) : 0;
    // Renderers and aspect ratios without the taller map layout keep the centered 16:9 frame (#303).
    if (reshape && shape < 1 && !tall_map && framepace_map_ui()) {
        double map_h = o->vh * shape;
        o->vy += (o->vh - map_h) / 2;
        o->vh = map_h;
        o->crop[1] = (1 - shape) / 2;
        o->crop[3] = shape;
    }
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
float output_window_shape(void) { return g_window_shape; }
float output_map_extra_y(void) { return g_map_extra_y; }

float output_pause_extra_y(void) {
#ifdef SW_METAL
    return 0;
#else
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_HUD_FIX") != NULL;
    float shape = g_view_shape;
    return !off && g_settings.hud_edges && shape >= 0.75f && shape < 1 &&
           framepace_menu_ui() && !framepace_map_ui() ? 360 * (1 / shape - 1) : 0;
#endif
}

float output_ui_extra_y(void) { return fmaxf(g_map_extra_y, output_pause_extra_y()); }

// The taller world-map layout is implemented by Vulkan. Very narrow windows keep the console frame.
bool output_tall_map(float shape) {
#ifdef SW_METAL
    return false;
#else
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_HUD_FIX") != NULL;
    return !off && shape >= 0.75f && shape < 1 && framepace_map_ui();
#endif
}

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

// Console: the game's own size (BOTW: 896 x 896 per cascade); High 2048, Ultra 4096, Extreme 8192; Potato 448, half the
// game's (softer edges, a quarter of the pixels to draw: for weak GPUs). Test override:
// SAVAGEWIND_SHADOW_AT=<frame>:<console|high|ultra|extreme|width>,... from that presented frame on (same-view A/B in one
// run).
unsigned shadow_map_size(unsigned long long frame) {
    static const unsigned size[SHADOW_COUNT] = {0, 2048, 4096, 8192, 448};
    static int n = -1;
    static unsigned long long at[8];
    static unsigned w[8];
    if (n < 0) {
        n = 0;
        for (const char *e = getenv("SAVAGEWIND_SHADOW_AT"); e && *e && n < 8;) {
            char name[16] = "";
            if (sscanf(e, "%llu:%15[^,]", &at[n], name) != 2) break;
            w[n++] = !strcmp(name, "console") ? 0 : !strcmp(name, "high") ? 2048 : !strcmp(name, "ultra") ? 4096
                   : !strcmp(name, "extreme") ? 8192 : !strcmp(name, "potato") ? 448 : (unsigned)atoi(name);
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    unsigned r = g_settings.shadow_res >= 0 && g_settings.shadow_res < SHADOW_COUNT ? size[g_settings.shadow_res] : 0;
    for (int i = 0; i < n; i++)
        if (frame >= at[i]) r = w[i];
    if (r > 8192) r = 8192;
    // With a temporal upscaler (FSR 3 / 4, XeSS, DLSS) the scene is drawn smaller by the quality mode's factor, and
    // the cascades above the console's size follow it: as many shadow texels per drawn pixel as without the upscaler
    // (Extreme at Quality: 5440 instead of 8192). The cascades cost the same at any scene resolution otherwise, and
    // where they are most of the frame the upscaler gained nothing. Never under the console's own 896.
    // SAVAGEWIND_SHADOW_FOLLOW=0: the setting's size, as before.
    static int follow = -1;
    if (follow < 0) follow = !getenv("SAVAGEWIND_SHADOW_FOLLOW") || atoi(getenv("SAVAGEWIND_SHADOW_FOLLOW")) != 0;
    if (follow && r > 896 && (rt_fsr3() || rt_xess() || rt_dlss())) {
        r = (unsigned)(r / upscale_factor() / 64.0 + 0.5) * 64;
        if (r < 896) r = 896;
    }
    return r;
}
