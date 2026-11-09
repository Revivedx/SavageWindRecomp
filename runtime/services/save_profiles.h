// Save profiles (runtime/services/save_profiles.c): several complete save folders side by side, one of them active.
//
// A profile is a whole save root, the same layout the game has always used: <save root>/save_0/{0..5,tracker,option.sav}.
// The player's existing `save` folder is the profile called "Default" and is never moved or renamed, so an install
// that has never heard of profiles keeps working with no action. The others live in `save-profiles/<name>/save`:
//
//   <data root>/save                          the Default profile (as before)
//   <data root>/save.lock, save-backups/      its lock and its backups (as before, see save_convert.h)
//   <data root>/save-profiles/<name>/save     every other profile, with its own save.lock and save-backups beside it
//   <data root>/save_profile.txt              the active profile's name (one line); missing or unknown = Default
//
// Which profile is active is one small file of its own rather than a key in savagewind.ini: it is read before and
// independently of the settings, a half-written ini can't lose it, and it is written whole by a rename.
//
// SAVAGEWIND_SAVE_DIR (tests, scratch runs) still wins over everything: when it is set the active profile is ignored.
//
// Like save_convert.h this is SDL-free and has no state of its own except the per-boot session path below. Every
// function that changes a profile takes the game's lock on it (sc_lock_take), so a profile in use by a running Savage
// Wind is never switched to, copied, renamed or deleted from under it. `root` is the data root, NULL or "" for the
// working directory (which rt_paths_init makes the data root).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SP_OK = 0,
    SP_NAME_EMPTY,     // no name, or only spaces
    SP_NAME_LONG,      // longer than SP_NAME_MAX
    SP_NAME_CHARS,     // a character other than letters, digits, space, - _ . ( ), a leading / trailing dot or space, or ".."
    SP_NAME_RESERVED,  // "Default", or a name Windows reserves for devices (CON, NUL, COM1, ...)
    SP_EXISTS,         // a profile with that name exists (upper / lower case don't count)
    SP_NOT_FOUND,      // no such profile
    SP_IS_DEFAULT,     // the Default profile can't be renamed or deleted
    SP_IS_ACTIVE,      // the active profile can't be renamed or deleted: switch to another first
    SP_IN_USE,         // a running game holds the profile's lock
    SP_NO_SPACE,       // not enough free space for the copy
    SP_LINKED,         // the profile folder is a link: deleting it would not stay inside Savage Wind's folder
    SP_IO,             // a file or folder couldn't be read, written or moved (see sp_detail)
} SpCode;

enum { SP_NAME_MAX = 32 };  // (short: the data root, the name and save_0/tracker/... have to fit Windows' 260 characters)
#define SP_DEFAULT_NAME "Default"

// Why the last failing call failed, in English, for the log (a path and the system's reason). Per thread.
const char *sp_detail(void);

// ---- names
// SP_OK, or SP_NAME_*. Name rules: 1..SP_NAME_MAX characters of A-Z a-z 0-9 space - _ . ( ); no leading or trailing
// space or dot, no "..", not a Windows device name (with or without an extension), not "Default" in any case. That keeps
// a name a single plain folder name on Windows and Linux, with no separator, drive or parent reference possible.
SpCode sp_validate_name(const char *name);

// ---- looking
typedef struct {
    char name[SP_NAME_MAX + 1];
    bool is_default;
    bool active;
    int slots;        // slot folders in it
    time_t saved;     // when the newest slot was saved (the game's file times), 0: none
} SpProfile;
// Default first, then the others by name (upper / lower case alike). Folders whose name isn't a valid profile name, and
// the temporary ones the copy and delete make (a leading dot), are not listed. The count; free the list with free().
int sp_list(const char *root, SpProfile **out);
// The profile's save root (the folder with save_0 in it), as game and Saves tab use it. false: invalid name.
// NULL, "" and "Default" give <root>/save.
bool sp_save_dir(const char *root, const char *name, char *out, size_t cap);
// The active profile's name: from save_profile.txt, "Default" when the file is missing or names a profile that is gone.
void sp_active(const char *root, char *name, size_t cap);
bool sp_override_active(void);  // SAVAGEWIND_SAVE_DIR is set: profiles don't apply
// The save root to use now: SAVAGEWIND_SAVE_DIR if set, else the active profile's.
void sp_active_save_dir(const char *root, char *out, size_t cap);

// ---- the game's session
// main.c calls sp_session_begin() once per boot, after the start screen (where the player may have switched profile),
// and the lock, the game's file system and the log all use sp_session_save_dir() from then on: one answer for the
// whole boot, even if save_profile.txt changes meanwhile. Without a session it is sp_active_save_dir(NULL, ...).
void sp_session_begin(void);
const char *sp_session_save_dir(void);

// ---- changing
SpCode sp_set_active(const char *root, const char *name);       // SP_NOT_FOUND, SP_IN_USE (a game runs in it)
SpCode sp_create(const char *root, const char *name);           // an empty profile
// A complete copy of profile `from` as the new profile `to`: made under a temporary name and renamed into place only
// when whole, so a failure (no space, a file in use, a power cut) leaves no half profile. `from` can be Default.
// SP_NO_SPACE before anything is copied when the drive is too full. *bytes: the size copied (may be NULL).
SpCode sp_copy(const char *root, const char *from, const char *to, uint64_t *bytes);
SpCode sp_rename(const char *root, const char *from, const char *to);
// Deletes the profile whole. Refuses Default, the active profile, a profile in use and a linked folder. The folder
// is renamed out of the list first, so an interrupted delete never leaves a half profile showing.
SpCode sp_delete(const char *root, const char *name);
// Removes what a crash left: temporary copies older than a day and half-deleted profiles. Call from a worker.
void sp_cleanup(const char *root);

// Free bytes where `dir` is; tests replace it to fake a full disk. Default: the operating system's answer (UINT64_MAX
// when it can't tell, which skips the check).
extern uint64_t (*sp_free_space_hook)(const char *dir);

#ifdef __cplusplus
}
#endif
