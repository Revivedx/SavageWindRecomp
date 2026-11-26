#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOD_ID_CAP           256
#define MOD_NAME_CAP         256
#define MOD_VER_CAP          64
#define MOD_AUTHOR_CAP       128
#define MOD_DESC_CAP         512
#define MOD_PATH_CAP         1024
#define MOD_CONFLICT_SAMPLES 8
#define MOD_SAMPLE_CAP       256

typedef struct {
    char     id[MOD_ID_CAP];
    char     name[MOD_NAME_CAP];
    char     version[MOD_VER_CAP];
    char     author[MOD_AUTHOR_CAP];
    char     description[MOD_DESC_CAP];
    char     mod_dir[MOD_PATH_CAP];
    char     host_dir[MOD_PATH_CAP];
    char     rel_dir[MOD_PATH_CAP];
    bool     uses_romfs_subdir;
    bool     enabled;
    int      file_count;
    uint64_t total_bytes;
    int      wins_conflicts;
    int      loses_conflicts;
    int      sample_count;
    char     samples[MOD_CONFLICT_SAMPLES][MOD_SAMPLE_CAP];
} ModEntry;

typedef struct {
    ModEntry *items;
    int       count;
    int       cap;
    int       enabled_count;
    int       total_conflicts;
} ModList;

// True for non-RomFS documentation/script files at the root of a flat mod
bool mods_is_ignored_top_file(const char *name);

// Scan <root>/mods/* (root == NULL or "" uses "."), reconcile with <root>/mods.txt,
// parse optional metadata, count files/bytes, and optionally analyze conflicts across enabled mods.
bool mods_scan(const char *root, ModList *out, bool analyze_conflicts);

// Free all heap allocations owned by `out` and reset it to zero.
void mods_list_free(ModList *out);

// Atomically persist `list` order and enabled state to <root>/mods.txt.
bool mods_save(const char *root, const ModList *list);

// Toggle one mod's enabled state in <root>/mods.txt.
bool mods_set_enabled(const char *root, const char *id, bool enabled);

// Enable or disable all discovered mods in <root>/mods.txt.
bool mods_set_all_enabled(const char *root, bool enabled);

// Move `id` in load order by `delta` (< 0 moves higher toward #1 / index 0; > 0 moves lower).
bool mods_move(const char *root, const char *id, int delta);

// Read SAVAGEWIND_ROMFS_OVERLAY from environment if non-empty.
bool mods_env_override(char *out, size_t cap);

// Snapshot active overlay layers for the current boot: env override first, then enabled mods #1..#N.
void mods_session_begin(const char *root);
// The mods this run loaded, as the log counts them: each one an overlay's savagewind-mods.txt names, each Mods tab mod
int mods_session_mod_count(void);

// True if saved enabled mod order in <root>/mods.txt differs from the booted session snapshot.
bool mods_session_pending_restart(const char *root);

// Active overlay layers for the running session (falls back to SAVAGEWIND_ROMFS_OVERLAY before session_begin).
int         mods_active_count(void);
const char *mods_active_dir(int index);
const char *mods_active_name(int index);

// The problem report's line about mods ("Mods: none\n", or the active layers' names), written to `out`; its length
int mods_report_line(char *out, size_t cap);

// Locate the highest-priority regular file matching `<ov_sub>/<rel>` across active layers.
bool mods_find_file(const char *ov_sub, const char *rel,
                    char *out, size_t cap,
                    char *mod_name_out, size_t mod_name_cap);

#ifdef __cplusplus
}
#endif
