// Intel's XeSS library, downloaded from Intel's repository (xess_fetch.h) through lib_fetch.c. Its licence (Intel
// Simplified Software License) goes next to it as INTEL-XeSS-LICENSE.txt.
#include "xess_fetch.h"
#include "lib_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const LibFile k_files[] = {
    {"bin/libxess.dll", "libxess.dll", 77795704, "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7"},
    {"LICENSE.txt", "INTEL-XeSS-LICENSE.txt", 4194, "784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df"},
};
static const LibFetch k_xess = {"Intel", "/intel/xess/v" XESS_FETCH_VERSION "/", XESS_FETCH_VERSION, k_files,
                                (int)(sizeof k_files / sizeof *k_files)};

#ifdef _WIN32
#include <windows.h>

bool xess_fetch_offered(void) {
    static int intel = -1;
    if (intel >= 0) return intel;
    intel = 0;
    DISPLAY_DEVICEW d = {.cb = sizeof d};
    for (DWORD i = 0; !intel && EnumDisplayDevicesW(NULL, i, &d, 0); i++, d.cb = sizeof d)
        intel = wcsstr(d.DeviceID, L"VEN_8086") != NULL;  // (PCI\VEN_8086&DEV_...: Intel, Arc and integrated)
    if (getenv("SAVAGEWIND_XESS_OFFER")) intel = atoi(getenv("SAVAGEWIND_XESS_OFFER")) != 0;  // (testing elsewhere)
    return intel;
}
#else
bool xess_fetch_offered(void) { return false; }
#endif

bool xess_fetch_present(void) { return lib_fetch_present(&k_xess); }

bool xess_fetch_missing(void) {
    static int missing = -1;
    if (missing < 0) missing = xess_fetch_offered() && !xess_fetch_present();
    return missing;
}

bool xess_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    return lib_fetch_run(&k_xess, progress, why, why_size);
}
