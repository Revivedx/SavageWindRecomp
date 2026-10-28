// AMD FidelityFX Super Resolution 3.1's upscaler (fsr3.cpp: the vendored FidelityFX SDK's, third_party/FidelityFX) for
// the 3D scene at its composite (exec.c scene_post, Video > Upscaler FSR 3): this frame's scene, depth, motion vectors
// (vulkan/mvec.c) and reactive mask at the internal resolution in, the scene at its size in the window out. Built in
// with SW_FSR3 (Windows and Linux: the Makefile); elsewhere fsr3_available is false.
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
    uint32_t width, height;
} Fsr3Image;

typedef struct {
    VkDevice dev;
    VkPhysicalDevice phys;
    VkCommandBuffer cb;
    // inputs at the render size (every image in VK_IMAGE_LAYOUT_GENERAL, as they are left): the scene (LDR), its depth
    // (R32F, [0, 1], not inverted), the motion vectors (.xy: see motion_scale) and the reactive mask (R8); the output
    // (storage) at the upscaled size
    Fsr3Image color, depth, motion, reactive, output;
    float jitter[2];        // the camera's jitter this frame, render pixels (FSR's convention: x right, y down)
    float motion_scale[2];  // .xy times this (over the render size) is the UV offset to where the pixel was
    float dt_ms;            // time since the last frame
    float near_, far_, fov_y;  // the camera (fov_y: radians)
    float sharpness;        // RCAS, 0..1 (0: off)
    bool reset;             // no usable history (a cut, the first frame)
} Fsr3Dispatch;

bool fsr3_available(void);
// Records FSR 3.1 into d->cb (the context is made, and made again, for the sizes given); false when it failed
bool fsr3_dispatch(const Fsr3Dispatch *d);
// The jitter sequence FSR wants for this render / output width: its phase count and phase i's offset in pixels
int fsr3_jitter_phases(uint32_t render_w, uint32_t output_w);
void fsr3_jitter_offset(int i, int phases, float *x, float *y);
void fsr3_destroy(void);

#ifdef __cplusplus
}
#endif
