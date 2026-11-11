// Updating a Linux AppImage (runtime/update_appimage.c): the new file replaces the old one in place, under the same
// name, so shortcuts and Steam entries keep working. Linux allows that while the program runs, so no separate updater
// is needed. Downloads with the system's curl, and checks the file against a signature before anything changes.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "update_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char code[32];    // stable, for tests
    char title[200];  // what happened
    char what[900];   // what it means and what to do
} UpdateProblem;

// The running AppImage's file, NULL when not running from one
const char *appimage_path(void);
// work: a writable folder for temporary files. true: *available says if out holds a newer release.
bool appimage_find_update(const char *work, const char *current, UcRelease *out, bool *available, UpdateProblem *p);
// Downloads, checks, starts the new file once to see that it runs, and replaces the old one, kept as <file>.old until
// the new version has finished setting up. *progress goes 0..1 as it works.
bool appimage_install(const char *work, const UcRelease *r, volatile float *progress, UpdateProblem *p);
bool appimage_go_back(UpdateProblem *p);        // <file>.old back in place
bool appimage_has_old(void);
void appimage_remove_old(void);
// Replaces this process with the AppImage on disk, opening Savage Wind Configuration. Returns only if it couldn't.
void appimage_relaunch(UpdateProblem *p);

#ifdef __cplusplus
}
#endif
