// Downloadable content check (runtime/dlc_check.c). The game unlocks The Master Trials and The Champions' Ballad only
// when aoc:u lists both add-on contents (extracted/aoc1 and extracted/aoc2) and fsp-srv can mount aoc1, which in DLC
// 3.0 holds the data of both while aoc2 only marks the second pack.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_DLC_MAX 8
#define RT_DLC_VERSION "3.0"

typedef enum {
    RT_DLC_NONE = 0,        // no add-on content folders, and none misplaced or merged
    RT_DLC_READY,           // aoc1 (complete 3.0 data) and aoc2 (second pack's RomFS) are both valid
    RT_DLC_ONLY_ONE,        // only one of the two DLC folders is present (#470: the game needs both to unlock DLC)
    RT_DLC_SWAPPED,         // aoc1 and aoc2 are swapped (the 1.4 GB data pack is in aoc2 instead of aoc1)
    RT_DLC_WRONG_VERSION,   // aoc1's System/AocVersion.txt isn't "3.0" (older DLC, or Wii U's "0300")
    RT_DLC_INCOMPLETE,      // an aoc folder lacks System/AocVersion.txt (or is nested a folder deeper), or aoc1 lacks folders / files
    RT_DLC_MERGED,          // no aoc folders, and romfs/ has the DLC merged into it (#188, #242)
    RT_DLC_MISNAMED,        // no aoc folders, and another folder in root (e.g. dlc1) holds a DLC RomFS (#383)
} RtDlcState;

typedef struct {
    RtDlcState state;
    unsigned present_mask;  // bit (i - 1) set when root/aoc<i> is a directory (1..RT_DLC_MAX)
    int only_pack;          // RT_DLC_ONLY_ONE: 1 = the data pack, 2 = the second pack's marker
    bool nested;            // RT_DLC_INCOMPLETE: System/AocVersion.txt was found a folder deeper (e.g. aoc1/romfs/)
    char version[32];       // aoc1's System/AocVersion.txt (printable ASCII, '%' replaced), when present
    char detail[128];       // missing item ("aoc1/Movie"), misnamed folder ("dlc1"), or unreadable path
    unsigned files;         // rt_dlc_verify: files across aoc1 + aoc2
    uint64_t bytes;         // rt_dlc_verify: total bytes across aoc1 + aoc2
} RtDlcCheck;

// Fast metadata check of root (no full tree walk: safe on the UI thread when opening or refreshing the page).
RtDlcCheck rt_dlc_check(const char *root);

// Deep verification of root (lays out both RomFS images as the game does and checks key files for reading).
RtDlcCheck rt_dlc_verify(const char *root);

// Short log tag for a state ("ready", "only-one", ...).
const char *rt_dlc_state_name(RtDlcState state);

#ifdef __cplusplus
}
#endif
