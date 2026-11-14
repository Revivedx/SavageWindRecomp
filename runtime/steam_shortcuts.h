// Savage Wind in the Steam library (runtime/steam_shortcuts.c): two non-Steam game entries, "Savage Wind" (straight
// into the game, --game) and "Savage Wind Configuration" (--config), written into each Steam user's shortcuts.vdf.
// Steam keeps that file in memory and writes it back when it closes, so Steam is closed first and started again after.
// Entries that are already there (by name) are pointed at this copy instead of added twice; nothing else is changed,
// and the file before is kept as shortcuts.vdf.savagewind-backup.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;     // AppName
    const char *exe;      // quoted, as Steam writes it
    const char *start_dir;
    const char *icon;
    const char *options;  // LaunchOptions
} SteamEntry;

// The new file for in (n bytes, 0: no file yet) with the entries added or updated. false: why says why (in is left
// as it was: a file this can't read is never written over). *out is malloc'd.
bool steam_vdf_upsert(const uint8_t *in, size_t n, const SteamEntry *e, int count, uint8_t **out, size_t *out_n,
                      char *why, size_t why_size);
// How many shortcuts a shortcuts.vdf holds, -1: not one this can read
int steam_vdf_count(const uint8_t *in, size_t n);
// The id Steam gives a non-Steam shortcut (what its artwork files are named after)
uint32_t steam_shortcut_appid(const char *exe, const char *name);

// Everything: find Steam, close it, write, start it again. Runs for a while (waits for Steam): call it on a thread.
// true: message says what was done; false: why not, for the player.
bool steam_add_savage_wind(char *message, size_t message_size);
bool steam_supported(void);  // a release that can add itself (the Windows folder or an AppImage)

#ifdef __cplusplus
}
#endif
