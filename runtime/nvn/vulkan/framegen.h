// AMD FSR 3 frame generation (framegen.cpp: the FidelityFX SDK's frame interpolation and optical flow), used by
// exec.c vk_fg_generate. Plain C: the SDK is C++.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t w, h;
} FgImage;

// What the contexts are made for (a change makes them again)
typedef struct {
    uint32_t display_w, display_h;  // the game's final image (the interpolated frame's size)
    uint32_t render_w, render_h;    // the 3D scene's (depth and motion vectors)
    VkFormat color_format, hudless_format;
    bool depth_inverted;
} FgConfig;

typedef struct {
    FgImage color;     // this frame as presented (with the HUD)
    FgImage hudless;   // ... without it (the 3D scene composited, before the HUD's first draw)
    FgImage depth;     // the 3D scene's depth (D32)
    FgImage motion;    // motion vectors (.xy: where each pixel was in the previous frame - here, times motion_scale)
    float motion_scale[2];  // .xy times this over the render size is that uv offset (0, 0: .xy are uv units)
    FgImage out;       // the interpolated frame (storage image)
    float near_, far_, fovy;  // the world camera's (fovy in radians)
    float cam_pos[3], cam_up[3], cam_right[3], cam_fwd[3];
    float frame_ms;    // time since the previous real frame
    uint64_t frame_id; // +1 per real frame (a gap resets the interpolation)
    bool reset;        // the camera cut: no history
    bool debug_view;   // (SAVAGEWIND_FG_DEBUG_VIEW) the SDK's debug view instead of the interpolated frame
} FgFrame;

typedef struct {
    VkPhysicalDevice phys;
    VkDevice dev;
    void (*wait_idle)(void);  // the GPU idle (before the contexts are made again)
} FgVk;
extern FgVk g_fg_vk;

// Records the interpolated frame between the previous call's frame and this one into f->out. false: failed (logged).
bool fg_dispatch(VkCommandBuffer cb, const FgConfig *c, const FgFrame *f);
void fg_destroy(void);

#ifdef __cplusplus
}
#endif
