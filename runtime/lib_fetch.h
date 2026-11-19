// A vendor's library that Savage Wind doesn't ship but the player's PC downloads (NVIDIA DLSS: dlss_fetch.h, Intel
// XeSS: xess_fetch.h): from the vendor's own repository on GitHub at a pinned release, each file checked by size and
// SHA-256 and written next to savagewind.exe with the vendor's licence. Windows only (WinHTTP).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *src;     // the path in the repository at the tag
    const char *name;    // next to savagewind.exe
    uint64_t size;
    const char *sha256;
} LibFile;

typedef struct {
    const char *vendor;  // "NVIDIA", "Intel": in the messages
    const char *path;    // on raw.githubusercontent.com, up to the tag: "/NVIDIA/DLSS/v310.9.16/"
    const char *version;
    const LibFile *files;
    int nfiles;
} LibFetch;

// Every file is next to savagewind.exe at its recorded size
bool lib_fetch_present(const LibFetch *l);
// Downloads what is missing (progress: bytes done of total, any thread). false: why says what failed (nothing kept).
bool lib_fetch_run(const LibFetch *l, void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
