// Intel's XeSS library for Video > Upscaler's Intel XeSS (nvn/vulkan/xess.cpp), which Savage Wind doesn't ship: as
// NVIDIA DLSS (dlss_fetch.h), downloaded by the setup from Intel's own repository, intel/xess on GitHub, at a pinned
// release, checked by size and SHA-256, and written next to savagewind.exe with Intel's licence. Windows only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XESS_FETCH_VERSION "3.0.2"  // (intel/xess's release tag v3.0.2: XeSS-SR 2.0.2)

// An Intel graphics card (vendor 8086) is in this PC: XeSS is offered there, where it gains (on other cards it ran
// as fast as FSR 3 or slower, docs/XESS.md)
bool xess_fetch_offered(void);
// The library and its licence are next to savagewind.exe at their recorded size
bool xess_fetch_present(void);
// Offered here but the library not there (the game's menus: Intel XeSS greyed, pointing at the download); read once
bool xess_fetch_missing(void);
// Downloads what is missing (progress: bytes done of total, any thread). false: why says what failed (nothing kept).
bool xess_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
