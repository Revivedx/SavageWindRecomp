// Achievements (runtime/achievements.c, achievements_defs.c, achievements_watch.c).
//
// How it works: the game itself is never touched. A small thread (achievements_watch.c) looks at the game's
// game_data.sav files in the save folder; when one changes (the game saved) it is read and each achievement is checked
// against the flags in it (flag names are stored as crc32 hashes: see test_warp.c for the format). What has been
// earned is kept per save in achievements.txt in the data root: a new game starts with none, and each save shows its own. The overlay (ui/achievements_ui.mm) shows a toast on each unlock and has an "Achievements" tab.
//
// Adding one: a line in g_ach_defs (achievements_defs.c). `tools/ach_tool verify <game_data.sav>` says which of the flag
// names the table uses really are in a save.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACH_FLAG,   // one flag: value >= min (booleans: min 1); `flag` may be "A,B": alternative names, the highest present counts
    ACH_COUNT,  // how many of flag pattern(first..last) are >= 1; `flag` is a printf pattern with one %d (e.g. "Clear_Dungeon%03d")
    ACH_ALL,    // every flag in the comma-separated list `flag` is >= 1
    ACH_SET,    // how many of the names in `list` are >= 1 (long lists: dishes, Compendium entries); min 0 = all of them
    ACH_SUM,    // the values of the names in `list` added up (enemy defeat counters): reach min
    ACH_STR,    // a string array in the save (horse names): how many of its slots are filled, or (with `list`) equal one of the
                // names in `list` (ignoring case); `flag` names the array (or "#<hex crc32>"), `first` = entries per slot
    ACH_ANY,    // one of the groups in `list` complete: each is a comma-separated list of names, all >= 1 (an armor set's
                // three pieces); progress is the most of one group
    ACH_META,   // every other achievement unlocked: not from the save, from what the playthrough has (ach_update)
} AchKind;

typedef struct {
    const char *id;    // stable, saved in achievements.txt: never change one that has shipped
    const char *name;  // English; the overlay runs it through tr()
    const char *desc;
    AchKind kind;
    const char *flag;
    int first, last;  // ACH_COUNT: the numbers the pattern runs over (inclusive); ACH_FLAG: `last` (when > 0) is the
                      // value's unit (MaxHartValue counts quarter hearts: 4, so progress and goal are in hearts)
    uint32_t min;     // ACH_FLAG: the value to reach; ACH_COUNT / ACH_SET: how many flags; ACH_SUM: the total; ACH_STR: slots
    const char *const *list;  // ACH_SET / ACH_SUM: the flag names; ACH_STR: the texts that count (none: any non-empty slot)
    int n_list;
} AchDef;

extern const AchDef g_ach_defs[];
extern const int g_ach_count;

// ---- the save image (game_data.sav: 0xC-byte header, then 8-byte entries: crc32 of the name, 32-bit value) ----
typedef struct {
    const uint8_t *data;
    size_t size;
} AchSave;

uint32_t ach_crc32(const char *s);
bool ach_save_ok(const AchSave *s);                                         // looks like a game_data.sav
bool ach_save_get(const AchSave *s, uint32_t hash, uint32_t *value);        // first entry with that hash
bool ach_save_get_named(const AchSave *s, const char *name, uint32_t *value);  // name, or "#<hex crc32>"
// A string array (the horse names): a run of entries with the same hash, 4 characters each; `per` entries make one slot
// (a horse name is 16 = 64 characters, 6 slots). Fills out[i] with the text of slot i ("" when empty; trailing/leading
// blanks cut; plain UTF-8, or ASCII stored as UTF-16LE) and returns how many slots the save holds (at most max).
#define ACH_STR_MAX 257
int ach_save_strings(const AchSave *s, uint32_t hash, int per, char (*out)[ACH_STR_MAX], int max);
// Progress of one achievement in a save: *cur of *target; true when reached.
bool ach_eval(const AchDef *d, const AchSave *s, uint32_t *cur, uint32_t *target);
// Calls cb for each flag name the achievement looks at (a pattern: every number of its range).
void ach_def_flags(const AchDef *d, void (*cb)(const char *name, void *user), void *user);

// ---- what has been earned ----
void ach_state_load(const char *path);         // reads path (missing = nothing earned yet); later changes are saved there (per save file)
void ach_set_blocked(bool blocked);            // true: progress is tracked but nothing unlocks (test warps, future assists)
// Achievements belong to a save: each game_data.sav has those its contents meet (a playthrough's progress only adds up).
// What the menu and ach_info show is the save being played: the one the game loaded (opened to read) or saved last, or
// before it opens one, the one written last. A save the game writes is announced against the one that was being played:
// what it has that that one hadn't gets a notice. A new game's first save meets nothing; loading an older save shows its
// achievements again, without notices.
void ach_game_key(const char *save_path, char *out, size_t n);  // the profile folder of a game_data.sav
void ach_game_gone(const char *save_path);   // that save was deleted: its achievements go with it
void ach_set_active(const char *save_path);  // the game opened this game_data.sav to write it (it is being saved)
void ach_game_loaded(const char *save_path);  // ... to read it: loaded, it is the one being played now
// The watcher, for each game_data.sav it looks at: read it again even unchanged (a save the game has loaded: its next
// read is quiet)
bool ach_wants_reread(const char *save_path);
// Checks every achievement against the contents of the save at save_path (time `now`): the save's own achievements, and
// when it is the play's save (the game opened it to write, or it is the one being played), toasts for what the save
// being played until now hadn't. Returns how many toasts that queued.
int ach_update(const char *save_path, const AchSave *s, time_t now);
// The same, given when the save was written (its file time: the save shown before the game opens one is the newest)
int ach_update_at(const char *save_path, const AchSave *s, time_t now, time_t written);

typedef struct {
    const AchDef *def;
    bool unlocked;
    time_t at;  // when (unlocked only)
    uint32_t cur, target;  // progress from the latest save looked at (0 / 0 before the first)
} AchInfo;
void ach_info(int index, AchInfo *out);
int ach_unlocked_total(void);
// ---- the sound of an unlock ----
// A short "ya-ha-ha!" made from sine waves into out (at most max samples, mono 16-bit at `rate`); returns the sample count.
size_t ach_synth_jingle(int16_t *out, size_t max, int rate);
// The sound `name` of a game sound bank (.bars, `n` bytes at b), decoded to mono 16-bit (malloc'd, `*count` samples at
// `*rate` Hz), or NULL when the bank has no such sound or it isn't DSP-ADPCM (runtime/achievements_bars.c).
int16_t *ach_bars_sound(const uint8_t *b, size_t n, const char *name, int *rate, size_t *count);
// Plays the notice's sound on the default output: the hidden Korok's "Yahaha!" from the player's RomFS, else
// achievement.wav from the data root, else the made-up jingle; render thread (runtime/achievements_sound.c).
void ach_sound_play(void);
void ach_sound_play_final(void);  // ... the last achievement's (ACH_META): a quest completed's jingle from the RomFS

int ach_toast_pending(void);          // unlocks not yet shown
bool ach_toast_take(int *index);      // the oldest one not yet shown

// ---- the watcher thread ----
void ach_start(void);  // loads achievements.txt (data root) and starts watching the save folder
void ach_stop(void);

#ifdef __cplusplus
}
#endif
