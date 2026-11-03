// Import of Wii U / Cemu saves (runtime/services/save_convert.c), the C version of tools/botw_save_convert.py (which
// stays as its reference and test oracle: tools/test_save_convert.py compares the two file by file).
//
// A save is a folder of small binary files; the Wii U's are big endian, the Switch's little endian, and each game
// revision has its own flag list. The flag list (name hash, type, length, default) comes from the player's own
// Pack/Bootup.pack, so nothing of the game is embedded. Everything here is SDL-free and has no state of its own: the
// start screen's Saves tab (runtime/ui/saves_tab.mm) calls it from a worker thread.
//
// An import is: sc_format_load -> sc_convert into a new staging folder (the saves stay as they are) -> the player
// confirms -> sc_stage_install, which moves the current save folder to a dated backup folder and puts the staged one
// in its place. Nothing the player had is deleted, and a failure at any step leaves the saves as they were.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SC_OK = 0,
    SC_NO_SAVE,       // no BotW save in the folder (no numbered folder with a game_data.sav)
    SC_SWITCH_SAVE,   // the save is already in the Switch byte order: nothing to convert
    SC_BAD_SAVE,      // a save file is damaged or incomplete (detail: which)
    SC_NO_BOOTUP,     // Pack/Bootup.pack can't be opened
    SC_BAD_BOOTUP,    // Pack/Bootup.pack can't be read or lacks the flag tables (detail: why)
    SC_IO,            // a file or folder couldn't be read, written or moved (detail: which and why)
    SC_LINKED,        // the save folder is a link: moving it would move the link
    SC_MOVED,         // sc_stage_install stopped halfway and could not undo it: the saves are in the backup (detail: it)
    SC_SEARCH_LIMIT,  // the folder search ran out of time or visits: choose a closer folder
    SC_CANCELLED,     // the caller requested cancellation before installation
    SC_IN_USE,        // a running game holds the save folder's lock (see sc_lock_take): close it first
} ScCode;

// One converted flag file: what it held and what became of it
typedef struct {
    char file[64];                    // "0/game_data.sav", "0/caption.sav", "option.sav"
    uint32_t src_rev, dst_rev;        // the game revision in the file's header, before and after
    uint32_t kept, defaulted, dropped, resized;  // flags carried over / not in the save (default) / not in the game /
                                                 // carried over with another length (resized flags count in kept too)
} ScFlagFile;

typedef struct {
    ScCode code;
    char detail[1200];                // English, for the log and the message's variable part: a path or a reason
    char root[1100];                  // the profile folder the slots were found in
    char **slots;                     // the slot folders converted ("0", "1", ...)
    int nslots;
    ScFlagFile *files;
    int nfiles;
    uint32_t kept, defaulted, dropped, resized;  // the sums over files
    int n_tracker, n_album, n_pict;   // Hero's Path blocks, album and compendium pictures
    int n_written;                    // files written (the staging folder; a backup: the files copied)
    int warnings;                     // things that didn't stop the import (install: files not carried over)
} ScReport;

void sc_report_free(ScReport *r);

// ---- the game's flag tables
typedef struct ScFormat ScFormat;
// Reads GameData/gamedata.ssarc and savedataformat.ssarc of a Pack/Bootup.pack. SC_NO_BOOTUP / SC_BAD_BOOTUP.
ScCode sc_format_load(const char *bootup_path, ScFormat **out, ScReport *r);
void sc_format_free(ScFormat *f);
uint32_t sc_format_revision(const ScFormat *f);

// ---- converting
enum { SC_ALLOW_SWITCH = 1 };  // (tests) a little-endian source is converted for its revision instead of refused

// The profile folder in dir: dir itself if it holds numbered slot folders with a game_data.sav, else the first folder
// below it (up to 10 levels, in name order) that does; one in the Wii U's byte order is preferred to any other. SC_NO_SAVE
// otherwise; SC_SEARCH_LIMIT if the four-second or 100,000-folder budget is exhausted.
ScCode sc_find_profile(const char *dir, char *root, size_t cap);

typedef struct ScStage ScStage;
// Optional cooperative cancellation, checked during the search and between files. The callback must be thread-safe.
typedef struct { bool (*requested)(void *); void *data; } ScCancel;
// Converts the profile found in src into the NEW folder stage_dir (it must not exist): stage_dir/save_0/<slot>/...,
// option.sav, tracker/, album/, pict_book/, for the Switch byte order and the revision of fmt. The Hero's Path, album
// and compendium pictures are always taken. On failure everything it made is removed again (*stage is NULL).
ScCode sc_convert(const char *src, const char *stage_dir, const ScFormat *fmt, unsigned flags, ScStage **stage,
                  ScReport *r);
ScCode sc_convert_cancel(const char *src, const char *stage_dir, const ScFormat *fmt, unsigned flags,
                         const ScCancel *cancel, ScStage **stage, ScReport *r);
// Puts the staged folder in place of save_dir: save_dir (if there) is first moved whole to backup_dir (which must not
// exist; its parent folders are made), then stage_dir becomes save_dir, and what the old one held besides save_0
// (sdcard, ...) is copied over. If the second step fails the first is undone. Consumes the stage only on success.
// After SC_MOVED, use sc_stage_keep to preserve the new saves for recovery; other failures can discard the stage.
ScCode sc_stage_install(ScStage *stage, const char *save_dir, const char *backup_dir, ScReport *r);
// Removes the files and folders the conversion made (and only those) and frees the stage.
void sc_stage_discard(ScStage *stage);
// Frees the stage without removing its files: keep the new saves for recovery after SC_MOVED.
void sc_stage_keep(ScStage *stage);

typedef struct { char imported[1100], backup[1100]; } ScRecovery;  // empty when none; backup can be absent
// Removes dated save-import-* folders beside save_dir, except keep_dir (a live or unconfirmed stage, or NULL).
// If save_dir is missing, keeps ALL of them and reports the newest import and backup folders for manual recovery.
// Does not follow links. Call only while no other job can create, install or discard a stage.
ScCode sc_cleanup_stages(const char *save_dir, const char *keep_dir, ScRecovery *recovery, ScReport *r);

// ---- backups
// A dated folder name that doesn't exist yet: <save_dir>-backups/save-YYYYMMDD-HHMMSS (a -2, -3... if taken), and
// the staging folder's: <save_dir>-import-YYYYMMDD-HHMMSS.
void sc_backup_path(const char *save_dir, char *out, size_t cap);
void sc_stage_path(const char *save_dir, char *out, size_t cap);
// Copies save_dir whole into the new folder backup_dir (made, with its parents). SC_NO_SAVE: nothing to copy. On
// failure the copy made so far is removed again.
ScCode sc_backup(const char *save_dir, const char *backup_dir, ScReport *r);

// ---- the lock on the save folder
// While a game runs it holds the lock of its save folder: the file <save_dir>.lock, next to the folder. sc_backup and
// sc_stage_install take it for their run and fail with SC_IN_USE when somebody else has it. The operating system drops
// a lock when its process ends, so a crashed game leaves none behind.
typedef struct ScLock ScLock;
typedef enum {
    SC_LOCK_OK,           // taken: *out is the lock (sc_lock_take)
    SC_LOCK_BUSY,         // another process holds it
    SC_LOCK_UNAVAILABLE,  // the lock file can't be made or locked (read-only folder...): nothing to wait for, *out is NULL
} ScLockResult;
ScLockResult sc_lock_take(const char *save_dir, ScLock **out);
void sc_lock_drop(ScLock *lock);  // NULL is fine

// ---- looking at a save folder (<save>/save_0)
typedef struct {
    char name[16];          // the slot folder
    int number;
    time_t saved;           // when the game wrote it: the modification time of caption.sav (else game_data.sav)
    int64_t play_seconds;   // from caption.sav's PlayTime flag when it has one, else -1
    bool has_picture;       // <slot>/caption.jpg
} ScSlot;
// The slots of save_0_dir, by number; the count (0: none, or the folder isn't there). Free the list with free().
int sc_list_slots(const char *save_0_dir, ScSlot **out);

#ifdef __cplusplus
}
#endif
