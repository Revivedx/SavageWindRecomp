// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render-target scale (relative to the game's 1920x1080 sizes)
    double scene_sx, scene_sy; // the 3D scene's render targets' scale (vulkan/textures.c RS_SCENE)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
    double crop[4];            // the part of the image drawn there (uv origin, size): its 16:9 middle in menus
} OutScale;

// dw x dh: the window's drawable size in pixels
void output_scale(double dw, double dh, bool reshape_allowed, OutScale *o);
// Fullscreen on a size the display has no mode for (runtime/window_size.c): output_scale ran on that iw x ih image
// size; this moves its placement onto the sw x sh screen (scaled to fit, centred).
void output_scale_screen(double sw, double sh, int iw, int ih, OutScale *o);
// The upscaler (Video > Upscaler): whether this build has it (UPSCALE_*), a quality mode's factor (UPQ_*: render size
// = window / factor per axis), and the factor in use (1: Off)
bool upscaler_available(int u);
bool fsr4_offered(void);  // AMD FSR 4 usable on this PC (an AMD Radeon and AMD's DLLs; held back or not)
bool frame_gen_available(int fg);  // FG_*: Video > Frame generation's choices this GPU can run
// Video > Frame generation multiplier: whether frame generation fg (FG_*) makes mult - 1 frames for each drawn here,
// and the multiplier in use with it (the setting, or the most it makes below that)
bool frame_gen_mult_available(int fg, int mult);
int frame_gen_mult(int fg);
bool fsr3_upscaler_enabled(void);  // FSR 3 / 4 upscaling offered (SAVAGEWIND_FSR3=0: hidden)
// The world camera's projection as last made (rt_persp, game thread): tan(fovy / 2), aspect, near, far; zeros before
void rt_world_projection(float out[4]);
// The scene's render size in pixels (vulkan/exec.c: its motion target), for the jitter's sub-pixel offsets
void rt_jitter_set_size(uint32_t w, uint32_t h);
// SAVAGEWIND_JITTER=1: the world camera's projection moved by a sub-pixel jitter each game frame (rt_persp_offset)
bool rt_jitter_on(void);
int rt_antialias(void);  // Video > Anti-aliasing (settings.h AA_*), or SAVAGEWIND_AA_AT's
bool rt_fsr3(void);      // Video > Upscaler FSR 3, built in, or FSR 4 (both temporal, with the same inputs)
bool rt_fsr4(void);      // Video > Upscaler FSR 4 (vulkan/fsr_api.h)
bool rt_dlss(void);      // Video > Upscaler DLSS, and NGX can run it
bool rt_xess(void);      // Video > Upscaler Intel XeSS (vulkan/xess.h: libxess.dll, any DP4a GPU)
bool rt_temporal(void);  // TAA, FSR 3 / 4, DLSS or XeSS: motion vectors and the jitter on (vulkan/mvec.c, rt_jitter_on)
void rt_jitter_set_phases(uint32_t n);  // (FSR 3: the jitter sequence's length, fsr3_jitter_phases)
bool upscale_modes_available(void);  // (rendering below the window's size: needs render-target scaling)
double upscale_mode_factor(int q);
double upscale_factor(void);
bool upscale_mode_offered(int q);   // mode q can be chosen now (Ultra Performance with FSR 3 / 4 or DLSS: a 4K window)
bool upscale_mode_is_dlaa(int q);   // mode q is named DLAA in the menus: Native with DLSS chosen
int upscale_quality_now(void);      // the mode in use: the setting, or Performance where it is not offered
bool rt_temporal_upscaler(void);  // FSR 3 / 4, XeSS or DLSS: the upscalers that rebuild the image from several frames
const char *rt_temporal_upscaler_name(void);  // its name ("FSR 4", "FSR 3", "XeSS", "DLSS"), or NULL
// Video > Render scale: whether value r (RSCALE_*) can be chosen now (with a temporal upscaler up to 100%, in its Native
// or DLAA mode 100%), and the one in use
bool render_scale_offered(int r);
int render_scale_now(void);
// The stored render scale to 100% when the upscaler and mode now chosen don't offer it, and back to the value it had
// when they offer it again; render_scale_chosen: the player set it in its row (that value is never undone)
void render_scale_fit(void);
void render_scale_chosen(void);
// Video > Shadow resolution: the width the game's shadow maps are made at from presented frame `frame` on (0: their
// own size; vulkan/textures.c RS_SHADOW)
unsigned shadow_map_size(unsigned long long frame);
// The world camera's projection as last built (rt_persp: tan(fovy / 2) and aspect as drawn, near, far), for frame
// generation's motion vectors (vulkan/framegen.c). false before the first one.
typedef struct { float tan_half_fovy, aspect, near_, far_; unsigned long long serial; } WorldProj;
void output_world_proj_set(float t, float a, float n, float f);
bool output_world_proj(WorldProj *p);

float output_view_shape(void);
// The window's shape over 16:9 whether or not the view is widened to it (the loading screen keeps 16:9)
float output_window_shape(void);
bool output_tall_map(float shape);
// Extra layout space above and below the visible world map, in native 1280x720 units.
float output_map_extra_y(void);

// Extra pause header/footer space, and the full visible pointer canvas (map or pause).
float output_pause_extra_y(void);
float output_ui_extra_y(void);

// Dynamic resolution: the upscaler's Dynamic mode (nvn/output_scale.c). A frame-rate governor moves the upscaler's
// factor, and a step smaller that does not actually raise the rate is undone (so a CPU-bound machine keeps full
// resolution instead of softening the picture for nothing). The tick runs once per presented frame; reset on a new
// window or boot.
void dynamic_res_tick(float fps, float fps_generated, bool fg_on);
double dynamic_res_target(void);
void dynamic_res_reset(void);
// For the F10 overlay: whether the Dynamic mode is in use, the factor it has settled on, and whether frame generation
// has been turned off for having no net gain.
bool dynamic_res_active(void);
double dynamic_factor(void);
bool dynamic_fg_disabled(void);  // Dynamic mode: FG was switched off for no net gain
bool dynamic_fg_no_gain(void);   // another mode: the same verdict, as a hint only (nothing switched off)
