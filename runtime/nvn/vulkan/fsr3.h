// AMD FSR 3.1 upscaling (runtime/nvn/vulkan/fsr3.cpp): the AMD FidelityFX DLL, loaded at run time.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage color, depth, mv, output;     // color (gamma-encoded), depth R32F, mv RGBA16F, output (storage)
    VkFormat color_fmt, output_fmt;
    uint32_t rw, rh, ow, oh, mvw, mvh;    // render size, output size, the motion vectors' size
    float jx, jy;                         // this frame's jitter, in render pixels (fsr3_jitter)
    float mv_scale_x, mv_scale_y;         // the motion vectors to pixels, previous - current
    float near_, far_, fov_y, meters_per_unit, frame_ms, sharpness;
    bool reset;                           // no history (a cut, the first frame)
} Fsr3Dispatch;

bool fsr3_available(void);
// the jitter sequence for render width rw upscaled to ow: frame's offset in pixels
void fsr3_jitter(uint32_t rw, uint32_t ow, uint64_t frame, float *jx, float *jy);
// records the upscale into cb (outside any rendering); false: not done (the caller falls back)
bool fsr3_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *d);
void fsr3_shutdown(void);
