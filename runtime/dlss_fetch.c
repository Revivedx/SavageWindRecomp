// NVIDIA's DLSS libraries, downloaded from NVIDIA's repository (dlss_fetch.h) through lib_fetch.c.
#include "dlss_fetch.h"
#include "lib_fetch.h"

#include <stdio.h>

static const LibFile k_files[] = {
    {"lib/Windows_x86_64/rel/nvngx_dlss.dll", "nvngx_dlss.dll", 58956912,
     "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"},
    {"lib/Windows_x86_64/rel/nvngx_dlssg.dll", "nvngx_dlssg.dll", 7506544,
     "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"},
    {"LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt", 26620, "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"},
};
static const LibFetch k_dlss = {"NVIDIA", "/NVIDIA/DLSS/v" DLSS_FETCH_VERSION "/", DLSS_FETCH_VERSION, k_files,
                                (int)(sizeof k_files / sizeof *k_files)};

#ifdef _WIN32
#include <windows.h>

bool dlss_fetch_offered(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}
#else
bool dlss_fetch_offered(void) { return false; }
#endif

bool dlss_fetch_present(void) { return lib_fetch_present(&k_dlss); }

bool dlss_fetch_missing(void) {
    static int missing = -1;
    if (missing < 0) missing = dlss_fetch_offered() && !dlss_fetch_present();
    return missing;
}

bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    return lib_fetch_run(&k_dlss, progress, why, why_size);
}
