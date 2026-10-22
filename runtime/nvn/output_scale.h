// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render-target scale (relative to the game's 1920x1080 sizes)
    double scene_sx, scene_sy; // the 3D scene's render targets' scale (vulkan/textures.c RS_SCENE)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
} OutScale;

// dw x dh: the window's drawable size in pixels
void output_scale(double dw, double dh, bool reshape_allowed, OutScale *o);
// Fullscreen on a size the display has no mode for (runtime/window_size.c): output_scale ran on that iw x ih image
// size; this moves its placement onto the sw x sh screen (scaled to fit, centred).
void output_scale_screen(double sw, double sh, int iw, int ih, OutScale *o);
// The upscaler (Video > Upscaler): whether this build has it (UPSCALE_*), a quality mode's factor (UPQ_*: render size
// = window / factor per axis), and the factor in use (1: Off)
bool upscaler_available(int u);
bool upscale_modes_available(void);  // (rendering below the window's size: needs render-target scaling)
double upscale_mode_factor(int q);
bool upscale_mode_offered(int u, int q);    // (Native AA and Ultra Performance: FSR 3.1 only)
extern const int upscale_mode_order[];      // UPQ_COUNT modes, from the sharpest (Native AA) to the fastest
double upscale_factor(void);
