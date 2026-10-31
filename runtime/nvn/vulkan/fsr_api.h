// AMD's FSR API (fsr_api.cpp): the FSR SDK 2.4.0's signed amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll, loaded at run time from the game's folder (Windows only: AMD ships no others;
// tools/fetch_fsr_dlls.py downloads them). They hold AMD FSR 4 (Video > Upscaler FSR 4: AMD Radeon graphics cards)
// and AMD's own build of the FSR 3.1 upscaler (3.1.6; SAVAGEWIND_FSR3_API=1 has Upscaler FSR 3 use it in place of
// the built-in one, fsr3.cpp). Same inputs as fsr3.h's Fsr3Dispatch. Without the DLLs, or elsewhere, nothing is
// available and the rest of the game is as before.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "fsr3.h"

#ifdef __cplusplus
extern "C" {
#endif

// The loader DLL loads (asked before the device is made: device.c enables FSR 4's device extensions only then)
bool fsr_api_present(void);
// After the device is made: which upscaler versions AMD's DLL offers for it (logged)
void fsr_api_probe(VkDevice dev, VkPhysicalDevice phys);
// A version with this major number (4: FSR 4, 3: FSR 3.1) was offered for this device
bool fsr_api_has(int major);
// ...and has not failed yet this run (fsr_api_dispatch gives up on a version after its first failure)
bool fsr_api_usable(int major);
// Records that version's upscale into d->cb (its context made, and made again, for the sizes given); false when it
// failed (the scene goes on without it, and that version is not tried again this run)
bool fsr_api_dispatch(const Fsr3Dispatch *d, int major);
void fsr_api_destroy(void);

bool fsr4_available(void);  // fsr_api_has(4)
bool fsr3_api_wanted(void);  // SAVAGEWIND_FSR3_API=1 and fsr_api_has(3)

#ifdef __cplusplus
}
#endif
