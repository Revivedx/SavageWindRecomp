// Optional NVIDIA DLSS Super Resolution and Frame Generation backend (Windows + external NGX SDK).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "vk.h"

bool vk_dlss_compiled(void);
void vk_dlss_required_instance_extensions(const char ***names, unsigned *count);
void vk_dlss_required_device_extensions(const char ***names, unsigned *count);
bool vk_dlss_init(void);  // NGX on g_vk.dev (false: no SDK, DLL or NVIDIA driver for it)
void vk_dlss_shutdown(void);

// NVIDIA DLSS Super Resolution (Video > Upscaler)
bool vk_dlss_available(void);
// color at the render size; depth and motion at its size too (MVLowRes): motion RG times mv_scale_x / mv_scale_y is
// each pixel's offset to where it was in the previous frame, in render pixels (exec.c cam_motion's uv differences:
// the motion's size; mvec.c's NDC differences: -w / 2, h / 2; zero, with reset, for none). jitter_x / jitter_y: the
// scene's sub-pixel offset in render pixels. reset: no history (a cut). false: not run (the caller falls back).
bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion,
                   TexBackend *output, unsigned quality, float jitter_x, float jitter_y, float mv_scale_x,
                   float mv_scale_y, bool reset);

// NVIDIA DLSS Frame Generation (Video > Frame generation: NVIDIA DLSS; NGX's own, 2x-4x): exec.c vk_fg_generate's
// inputs, the ones AMD FSR 3 takes (framegen.h FgFrame).
typedef struct {
    TexBackend *color;    // this frame as presented (with the HUD), read through view_format
    TexBackend *hudless;  // ... without it, read through view_format
    VkFormat view_format; // (the UNORM format of the two: the interpolated frame's)
    TexBackend *depth;    // the 3D scene's depth
    TexBackend *motion;   // camera motion vectors at depth's size (RG16F, uv: where each pixel was - here)
    TexBackend *out[3];   // the interpolated frames, in order (storage images, color's size and view_format)
    int count;            // how many (1 .. vk_dlssg_max_mult() - 1): multi frame generation beyond 1
    // the world camera: view-projection now and in the previous frame, and projection (row-major, column vectors,
    // OpenGL's clip space: z -1..1)
    double vp[16], prev_vp[16], proj[16];
    float ysign;          // the depth image's rows: uv.y = (ndc.y * ysign + 1) / 2 (exec.c cam_motion)
    bool gl_depth;        // depth values are (z / w + 1) / 2, else z / w
    float near_, far_, fovy;  // (fovy in radians)
    float cam_pos[3], cam_up[3], cam_right[3], cam_fwd[3];
    bool reset;           // a cut: no history
} DlssgFrame;
bool vk_dlssg_available(void);
int vk_dlssg_max_mult(void);  // frames on screen for each drawn it can make here (2: one generated frame)
// Records the f->count frames between the previous call's and this one into f->out[]. false: not made (logged); NGX
// unable to make the feature at all: vk_dlssg_available false from then on.
bool vk_dlssg_dispatch(VkCommandBuffer cb, const DlssgFrame *f);
bool vk_dlssg_live(void);     // the feature is made (its VRAM held)
void vk_dlssg_release(void);  // (the GPU idle first: the caller's)

#ifdef __cplusplus
}
#endif
