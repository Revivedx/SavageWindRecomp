// Intel XeSS super resolution over Vulkan (xess.cpp): libxess.dll loaded at run time from the game's folder
// (Windows; Intel ships no others), like AMD's FSR DLLs (fsr_api.h). Video > Upscaler Intel XeSS: the scene at its
// composite (exec.c fsr3_scene's path), with the same inputs as FSR 3 (fsr3.h). Without the DLL, or where the device
// or driver can't run it, xess_available is false and the rest of the game is as before.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "fsr3.h"

#ifdef __cplusplus
extern "C" {
#endif

// The instance extensions and Vulkan API version XeSS needs (asked before the instance is made: device.c enables
// them only then); NULL / 0 without the DLL
void xess_required_instance_extensions(const char *const **names, uint32_t *count, uint32_t *min_api);
// The device extensions and features XeSS needs (asked after the instance, before the device is made: device.c
// enables them only then); false without the DLL. *features receives a writable feature-structure chain the library
// patches (its own memory): pass it as VkDeviceCreateInfo::pNext.
bool xess_required_device_extensions(VkInstance inst, VkPhysicalDevice phys, const char *const **names,
                                     uint32_t *count, void **features);
// After the device is made: makes the XeSS context (logged). False without the DLL, or when the library refused the
// device or driver. A context that came with an old-driver warning still counts (it runs, only slower).
bool xess_probe(VkInstance inst, VkDevice dev, VkPhysicalDevice phys);
// Whether libxess.dll was found and made a context (Video > Upscaler's Intel XeSS entry is offered when so)
bool xess_available(void);
// False once a dispatch failed this run (the scene goes on without XeSS from there)
bool xess_usable(void);
// Records XeSS into d->cb (fsr3.h's Fsr3Dispatch: the scene, depth R32F [0, 1] not inverted, the motion vectors at
// the render size, the reactive mask R8, the output at the window's size; the images are left in GENERAL). XeSS wants
// its inputs in SHADER_READ_ONLY_OPTIMAL and the output in GENERAL: the transitions are recorded here, around the
// library's own commands. False when it failed (logged there).
bool xess_dispatch(const Fsr3Dispatch *d);
void xess_destroy(void);
// XeSS's recommended jitter sequence length for these sizes (8 x the upscale ratio squared)
uint32_t xess_jitter_phases(uint32_t render_w, uint32_t output_w);

#ifdef __cplusplus
}
#endif
