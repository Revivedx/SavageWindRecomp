// The parts of SW Update that are plain logic (runtime/update_core.c; tools/test_update_core.c tests them on any OS):
// release versions, choosing a release from GitHub's list, the release's file list (sw-manifest.txt), safe paths.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "0.5.1-alpha.5" (a leading v is accepted). Valid: three numbers, optionally -alpha.N / -beta.N / -rc.N.
bool uc_version_valid(const char *v);
// <0, 0, >0 as a is older than, the same as or newer than b. A final version is newer than its own pre-releases.
int uc_version_cmp(const char *a, const char *b);

typedef struct {
    char version[64];    // no leading v
    char tag[64];
    char page_url[512];  // the release's page
    char zip_name[256], zip_url[1024], sig_url[1024];
    char digest[65];     // sha256 of the zip as GitHub reports it (lowercase hex), "" if it didn't
    uint64_t zip_size;
} UcRelease;

typedef enum {
    UC_PICK_UPDATE,      // out: the newest release newer than current
    UC_PICK_CURRENT,     // nothing newer that can be installed
    UC_PICK_BAD_JSON,    // not the list of releases GitHub sends
} UcPick;

// json: GET /repos/<owner>/<repo>/releases (a JSON array, newest first or not). suffix: the end of this platform's
// asset ("-windows.zip", "-x86_64.AppImage"...). Drafts, tags that aren't versions and releases without both that asset
// and its ".sig" are passed over; *skipped_unsigned counts newer releases passed over for lack of a signature (may be NULL).
UcPick uc_pick_release(const char *json, const char *current, const char *suffix, UcRelease *out, int *skipped_unsigned);

typedef struct {
    char sha256[65];
    uint64_t size;
    char *path;  // relative, forward slashes
} UcFile;

typedef struct {
    char version[64];
    UcFile *files;
    int count;
} UcManifest;

// text: sw-manifest.txt, NUL-terminated. false: why says what is wrong with it. Every path is checked by uc_path_safe.
bool uc_manifest_parse(const char *text, UcManifest *out, char *why, size_t why_size);
void uc_manifest_free(UcManifest *m);
const UcFile *uc_manifest_find(const UcManifest *m, const char *path);  // case-insensitive, as Windows' files are

// A relative path made only of ordinary names: nothing absolute, no "..", "." or empty parts, no drive or stream
// (":"), no backslashes, no name Windows reserves (CON, NUL, COM1...), none ending in a dot or a space.
bool uc_path_safe(const char *path);

// The folder a release's zip keeps everything in, "Savage Wind/": path without it, NULL if path isn't inside it.
const char *uc_strip_top(const char *zip_path);

// hex (2n digits, either case) to n bytes. false: anything else.
bool uc_hex_decode(const char *hex, uint8_t *out, size_t n);

// Whether the program at exe (a real path) is inside the folder appdir (a real path): an AppImage's own program runs
// from its runtime's mount ($APPDIR). Another AppImage's $APPIMAGE / $APPDIR, inherited, fail it.
bool uc_path_inside(const char *exe, const char *appdir);

#ifdef __cplusplus
}
#endif
