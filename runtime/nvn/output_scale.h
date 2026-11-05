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
bool fsr3_upscaler_enabled(void);  // FSR 3 upscaling offered (held back for now; SAVAGEWIND_FSR3=1)
// The world camera's projection as last made (rt_persp, game thread): tan(fovy / 2), aspect, near, far; zeros before
void rt_world_projection(float out[4]);
// The scene's render size in pixels (vulkan/exec.c: its motion target), for the jitter's sub-pixel offsets
void rt_jitter_set_size(uint32_t w, uint32_t h);
// SAVAGEWIND_JITTER=1: the world camera's projection moved by a sub-pixel jitter each game frame (rt_persp_offset)
bool rt_jitter_on(void);
int rt_antialias(void);  // Video > Anti-aliasing (settings.h AA_*), or SAVAGEWIND_AA_AT's
bool rt_fsr3(void);      // Video > Upscaler FSR 3, built in, or FSR 4 (both temporal, with the same inputs)
bool rt_fsr4(void);      // Video > Upscaler FSR 4 (vulkan/fsr_api.h)
bool rt_temporal(void);  // TAA or FSR 3: motion vectors and the jitter on (vulkan/mvec.c, rt_jitter_on)
void rt_jitter_set_phases(uint32_t n);  // (FSR 3: the jitter sequence's length, fsr3_jitter_phases)
bool upscale_modes_available(void);  // (rendering below the window's size: needs render-target scaling)
double upscale_mode_factor(int q);
double upscale_factor(void);
// Video > Shadow resolution: the width the game's shadow maps are made at from presented frame `frame` on (0: their
// own size; vulkan/textures.c RS_SHADOW)
unsigned shadow_map_size(unsigned long long frame);
// The world camera's projection as last built (rt_persp: tan(fovy / 2) and aspect as drawn, near, far), for frame
// generation's motion vectors (vulkan/framegen.c). false before the first one.
typedef struct { float tan_half_fovy, aspect, near_, far_; unsigned long long serial; } WorldProj;
void output_world_proj_set(float t, float a, float n, float f);
bool output_world_proj(WorldProj *p);
