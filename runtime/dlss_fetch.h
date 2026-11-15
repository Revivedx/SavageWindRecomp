// NVIDIA's DLSS library for Video > Upscaler's NVIDIA DLSS (nvn/vulkan/dlss.c), which Savage Wind doesn't ship:
// downloaded by the setup from NVIDIA's own repository, NVIDIA/DLSS on GitHub, at a pinned release, checked by size and
// SHA-256, and written next to savagewind.exe with NVIDIA's licence. Windows only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLSS_FETCH_VERSION "310.9.16"  // (NVIDIA/DLSS's release tag v310.9.16)

// An NVIDIA driver with NGX is installed (the library is of use here)
bool dlss_fetch_offered(void);
// The library and its licence are next to savagewind.exe at their recorded size
bool dlss_fetch_present(void);
// An NVIDIA driver here but the library not (the game's menus: NVIDIA DLSS greyed, pointing at the download); read
// once, as the files don't change while the game runs
bool dlss_fetch_missing(void);
// Downloads what is missing (progress: bytes done of total, any thread). false: why says what failed (nothing kept).
bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
