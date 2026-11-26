// The game's own options (the console game's pause menu > System > Options), for the F1 menu's Gameplay page: game data
// flags kept in option.sav (save format 7 in the player's Bootup.pack: GameData/savedataformat), with their kinds,
// defaults and ranges from GameData/gamedata. While the game runs they are read and written in its memory
// (achievements_live.c ach_live_flag), where the game reads them as it goes (camera, jump button, minimap...); and in
// option.sav, so the next start has them too (the game itself saves that file only when its Options screen closes).
//
// option.sav: u32 version (0x4730), u32 0xffffffff, u32 1, then (u32 crc32 of the flag's name, u32 value) pairs.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "achievements.h"
#include "game_flags.h"
#include "services/save_profiles.h"

typedef struct { const char *name; bool s32; int def, lo, hi; } GameOpt;
static const GameOpt k_opts[GOPT_COUNT] = {
    [GOPT_INVERT_Y] = {"CameraUpDownReverse", false, 0, 0, 1},
    [GOPT_INVERT_X] = {"CameraRLReverse", false, 0, 0, 1},
    [GOPT_JUMP_SWAP] = {"JumpButtonChange", false, 0, 0, 1},
    [GOPT_MOTION_AIM] = {"GyroOnOff", false, 1, 0, 1},
    [GOPT_RUBY] = {"TextRubyOnOff", false, 1, 0, 1},
    [GOPT_MINIMAP_NORTH] = {"MiniMapDirection", false, 0, 0, 1},
    [GOPT_CAMERA_SPEED] = {"StickSensitivity", true, 2, 0, 4},
    [GOPT_AMIIBO] = {"AmiiboItemOnOff", false, 0, 0, 1},
    [GOPT_HUD_PRO] = {"MainScreenOnOff", false, 0, 0, 1},
    [GOPT_MESSAGE_OPAQUE] = {"TextWindowChange", true, 1, 1, 2},
};

static uint32_t crc32_of(const char *s) {
    uint32_t c = 0xffffffffu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

static uint32_t hash_of(int id) {
    static uint32_t h[GOPT_COUNT];
    if (!h[id]) h[id] = crc32_of(k_opts[id].name);
    return h[id];
}

// <save root>/save_0/option.sav: the game's session's (what the game mounted), or the active profile's before a game
static void option_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/save_0/option.sav", sp_session_save_dir());
}

// The file's value for a hash (false: no file, or the flag isn't in it)
static bool file_get(uint32_t hash, uint32_t *v) {
    char path[1100];
    option_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[3], pair[2];
    bool found = false;
    if (fread(head, 4, 3, f) == 3)
        while (fread(pair, 4, 2, f) == 2)
            if (pair[0] == hash) { *v = pair[1], found = true; break; }
    fclose(f);
    return found;
}

static bool file_set(uint32_t hash, uint32_t v) {
    char path[1100];
    option_path(path, sizeof path);
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    uint32_t head[3], pair[2];
    bool done = false;
    if (fread(head, 4, 3, f) == 3)
        for (long at = 12; fread(pair, 4, 2, f) == 2; at += 8)
            if (pair[0] == hash) {
                done = fseek(f, at + 4, SEEK_SET) == 0 && fwrite(&v, 4, 1, f) == 1;
                break;
            }
    fclose(f);
    return done;
}

static int clamp(int id, int v) {
    return v < k_opts[id].lo ? k_opts[id].lo : v > k_opts[id].hi ? k_opts[id].hi : v;
}

bool game_opt_live(int id) {
    uint64_t obj;
    bool s32;
    return id >= 0 && id < GOPT_COUNT && ach_live_flag(hash_of(id), &obj, &s32);
}

int game_opt_get(int id) {
    if (id < 0 || id >= GOPT_COUNT) return 0;
    uint64_t obj;
    bool s32;
    if (ach_live_flag(hash_of(id), &obj, &s32))
        return clamp(id, s32 ? (int)*(volatile int32_t *)(uintptr_t)(obj + 0x18) : *(volatile uint8_t *)(uintptr_t)(obj + 0xb) & 1);
    uint32_t v;
    if (file_get(hash_of(id), &v)) return clamp(id, (int)v);
    return k_opts[id].def;
}

void game_opt_set(int id, int v) {
    if (id < 0 || id >= GOPT_COUNT) return;
    v = clamp(id, v);
    uint64_t obj;
    bool s32;
    if (ach_live_flag(hash_of(id), &obj, &s32)) {
        if (s32) *(volatile int32_t *)(uintptr_t)(obj + 0x18) = v;
        else {
            volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)(obj + 0xb);
            *b = (uint8_t)((*b & ~1u) | (v & 1));
        }
    }
    if (!file_set(hash_of(id), (uint32_t)v))
        fprintf(stderr, "[options] %s = %d: in the game's memory only (no option.sav yet: the game writes it)\n",
                k_opts[id].name, v);
}

int game_opt_default(int id) { return id >= 0 && id < GOPT_COUNT ? k_opts[id].def : 0; }

void game_opt_defaults(void) {
    for (int i = 0; i < GOPT_COUNT; i++) game_opt_set(i, k_opts[i].def);
}
