// NVIDIA's DLSS libraries for Video > Upscaler and Video > Frame generation's NVIDIA DLSS (nvn/vulkan/dlss.c), which
// Savage Wind doesn't ship: downloaded by the setup from NVIDIA's own repository, NVIDIA/DLSS on GitHub, at a pinned
// release, checked by size and SHA-256, and written with NVIDIA's licence. Windows: next to savagewind.exe (WinHTTP).
// Linux x86-64: into the executable's folder when that folder can be written (a source build's build/), otherwise the
// data folder, with the system's curl. Elsewhere: unavailable.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLSS_FETCH_VERSION "310.9.16"  // (NVIDIA/DLSS's release tag v310.9.16)
#ifdef _WIN32
#define DLSS_FETCH_MB 66               // (both Windows libraries, for the setup's text)
#elif defined(__linux__) && defined(__x86_64__)
#define DLSS_FETCH_MB 59               // (the Linux Super Resolution library)
#else
#define DLSS_FETCH_MB 0
#endif

// An NVIDIA driver with NGX is installed (the libraries are of use here)
bool dlss_fetch_offered(void);
// The libraries are in dlss_fetch_dir at their recorded size
bool dlss_fetch_present(void);
// An NVIDIA driver here but the libraries not (the game's menus: NVIDIA DLSS greyed, pointing at the download); read
// once, as the files don't change while the game runs
bool dlss_fetch_missing(void);
// Downloads what is missing (progress: bytes done of total, any thread). false: why says what failed (nothing kept).
bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);
// The folder the libraries are written to, and where NGX looks for them. false: no path.
bool dlss_fetch_dir(char *out, size_t n);
// Linux x86-64: a path dlopen() accepts for the driver's libnvidia-ngx.so.1. Elsewhere NULL.
const char *dlss_fetch_core(void);

#ifdef __cplusplus
}
#endif
