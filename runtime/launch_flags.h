// Launch options: environment flags (SAVAGEWIND_*) a tester sets from the start screen instead of a shell.
//
// A developer asks a tester to "try SAVAGEWIND_VK_NO_FEEDBACK_LOOP=1"; most players can't set an environment variable.
// The start screen keeps a list of NAME=value flags in <data root>/launch-flags.txt (a separate file, not savagewind.ini:
// they are read before settings_load() and before the data root's other files, by a process that may restart itself,
// and a file the player can delete is the recovery from a bad entry). They start empty. This file is the part
// without any UI or platform code: checking, the file, suggestions, and the environment (runtime/launch_flags_boot.c
// does the start-up and restart, runtime/ui/launch_flags_ui.mm the screen; tools/test_launch_flags.c tests this).
//
// Flags read once and cached, or read before main() (runtime/sampler.c, runtime/fnprof.c), must be in the environment
// when the process starts. So the saved flags are applied by restarting the process once with them set (the restart
// is skipped when nothing is saved, which is the normal case) and the process remembers what it applied in
// SAVAGEWIND_LAUNCH_FLAGS_APPLIED ("<hash>:<name>,<name>").
//
// A flag that is on by being set (getenv() != NULL) can't be turned off by a value: 0 would still turn it on. Such a
// flag ("switch" kind, found by tools/gen_launch_flags.py) is only ever on or removed, and a hand-written 0 is
// ignored when applying.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LF_MAX_FLAGS 48
#define LF_NAME_MAX 64   // with the terminator
#define LF_VALUE_MAX 128 // with the terminator
#define LF_FILE "launch-flags.txt"
#define LF_APPLIED_ENV "SAVAGEWIND_LAUNCH_FLAGS_APPLIED"
#define LF_OFF_SWITCH "--no-launch-flags"  // start without the saved flags (recovery)

typedef enum { LF_KIND_SWITCH, LF_KIND_NUMBER, LF_KIND_TEXT } LfKind;
// play: suggested first; other: a runtime flag without a description; dev: dumps, traces, logs, profiling, tests;
// internal / launcher / build: can't be added (see tools/launch_flags_curated.txt)
typedef enum { LF_GROUP_PLAY, LF_GROUP_OTHER, LF_GROUP_DEV, LF_GROUP_INTERNAL, LF_GROUP_LAUNCHER, LF_GROUP_BUILD } LfGroup;

typedef struct {
    const char *name;
    LfKind kind;
    LfGroup group;
    const char *example;  // the value a suggestion fills in ("" = none)
    const char *desc;     // one line ("" = none)
} LfInfo;

extern const LfInfo LF_TABLE[];  // runtime/launch_flags_table.c (generated), sorted by name
extern const int LF_TABLE_N;

typedef struct {
    char name[LF_NAME_MAX];
    char value[LF_VALUE_MAX];
} LfFlag;

typedef struct {
    LfFlag flag[LF_MAX_FLAGS];
    int n;
} LfList;

typedef enum {
    LF_OK = 0,
    LF_ERR_EMPTY,           // no name
    LF_ERR_BAD_NAME,        // not letters, digits and underscores
    LF_ERR_NAME_LONG,
    LF_ERR_BAD_VALUE,       // a line break or other control character
    LF_ERR_VALUE_LONG,
    LF_ERR_INTERNAL,        // Savage Wind's own, or needed before the saved flags can be found
    LF_ERR_LAUNCHER,        // would skip or lock the start screen, or hide the window
    LF_ERR_BUILD,           // belongs to the game code build
    LF_ERR_SYSTEM,          // a system variable (PATH, HOME, LD_PRELOAD, ...)
    LF_ERR_SWITCH_OFF,      // a 0 for a flag that is on whenever it is set: remove it instead
    LF_ERR_FULL,
} LfError;

// Plain-words English text for an error (the UI translates it)
const char *lf_error_text(LfError e);

const LfInfo *lf_find(const char *name);  // exact name, or NULL
// A typed name tidied up: spaces trimmed, a name that starts like SAVAGEWIND_ in any case upper-cased whole
void lf_normalize_name(char *name);
// A typed value tidied up: spaces trimmed
void lf_normalize_value(char *value);
// Whether this name and value can be added. An empty value is fine (it means 1).
LfError lf_check(const char *name, const char *value);
// A value that leaves a switch flag off (0, off, false, no, or nothing)
bool lf_value_is_off(const char *value);
// Is the value, for this flag, one that applying skips (a switch with an off value)?
bool lf_skipped(const LfFlag *f);

void lf_clear(LfList *l);
int lf_index(const LfList *l, const char *name);  // -1 = not in the list
// Adds NAME=value (an empty value becomes 1) or replaces the value of NAME. The strings are tidied first.
LfError lf_set(LfList *l, const char *name, const char *value);
void lf_remove(LfList *l, int index);

// The file's text: one NAME=value per line; a bare NAME means NAME=1; blank lines and # lines are skipped; lines that
// don't pass lf_check are skipped too. Returns how many lines were skipped for a problem.
int lf_parse(LfList *l, const char *text);
// The text for a file (starts with a comment saying what the file is). Returns the length (0 on a too small buffer).
size_t lf_format(const LfList *l, char *out, size_t cap);
bool lf_load(LfList *l, const char *path);  // false = no file (the list is then empty)
bool lf_save(const LfList *l, const char *path);  // written whole and swapped in; an empty list removes the file

// What applying a list sets: its hash and names ("<hash>:<name>,<name>"; "" for nothing to apply)
void lf_signature(const LfList *l, char *out, size_t cap);
// The signature of what this process was started with (LF_APPLIED_ENV; "" = nothing)
const char *lf_applied(void);
// Make the process environment hold exactly this list: variables of an earlier apply that are no longer wanted are
// removed, the list's are set, LF_APPLIED_ENV is updated. Returns how many variables were set.
int lf_apply(const LfList *l);
// Does the environment already hold this list?
bool lf_in_effect(const LfList *l);

// Matches for what is being typed, best first: names that start with it, then that contain it (case ignored, with or
// without the SAVAGEWIND_ prefix); play flags before other ones before developer ones. Nothing for a blocked flag.
// Empty text lists the play flags. Returns how many were written to out.
int lf_suggest(const char *typed, const LfInfo **out, int max);

#ifdef __cplusplus
}
#endif
