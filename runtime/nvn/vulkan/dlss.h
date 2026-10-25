// Optional NVIDIA DLSS Super Resolution backend (Windows + external NGX SDK).
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
bool vk_dlss_init(void);
void vk_dlss_shutdown(void);
bool vk_dlss_available(void);
// color at the render size; depth and motion at its size too (MVLowRes): motion RG, each pixel's uv in the previous
// frame minus its uv in this one (exec.c cam_motion; zero, with reset, for none). jitter_x / jitter_y: the scene's
// sub-pixel offset in render pixels. reset: no history (a cut). false: not run (the caller falls back).
bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion,
                   TexBackend *output, unsigned quality, float jitter_x, float jitter_y, bool reset);

#ifdef __cplusplus
}
#endif
