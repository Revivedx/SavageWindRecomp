// Mods (docs/MODS.md): what is in mods/ is what the game plays with. If the files there changed since the last time, the
// game runs swmods (tools/mods.py frozen, BCML in it) in a window of its own before it boots; the result is the overlay
// folder runtime/services/fs.c finds. Nothing changed: nothing runs. It fails: the log says so and the game plays with
// the last result. Windows only for now; elsewhere tools/mods.py does it.
// Settings turn a mod off by moving its file to mods/disabled/, where nothing applies it, and on by moving it back.
#include "mods.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef MODS_TEST
#include "paths.h"
#include "rt.h"
#else
#define LOG printf
#endif

static bool mod_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name && (!strcasecmp(dot, ".bnp") || !strcasecmp(dot, ".zip") || !strcasecmp(dot, ".rar") || !strcasecmp(dot, ".7z"));
}

// ---- on and off (Settings)
#define MODS_MAX 64
typedef struct { char file[256], title[256]; bool on, was_on; } ModFile;
static ModFile mods[MODS_MAX];
static int nmods;

static void mods_mkdir(const char *path) {
#if defined(_WIN32) && defined(MODS_TEST)  // (rt.h makes it the same everywhere)
    mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void scan_folder(const char *folder, bool on) {
    DIR *d = opendir(folder);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && nmods < MODS_MAX;) {
        if (strlen(e->d_name) >= sizeof mods[0].file || !mod_file(e->d_name)) continue;
        ModFile *m = &mods[nmods++];
        snprintf(m->file, sizeof m->file, "%s", e->d_name);
        snprintf(m->title, sizeof m->title, "%s", e->d_name);
        *strrchr(m->title, '.') = 0;
        m->on = m->was_on = on;
    }
    closedir(d);
}

static int by_title(const void *a, const void *b) { return strcasecmp(((const ModFile *)a)->title, ((const ModFile *)b)->title); }

void mods_scan(void) {
    nmods = 0;
    scan_folder("mods", true);
    scan_folder("mods/disabled", false);
    qsort(mods, (size_t)nmods, sizeof mods[0], by_title);
}

int mods_count(void) { return nmods; }
const char *mods_title(int i) { return mods[i].title; }
bool mods_on(int i) { return mods[i].on; }

void mods_set(int i, bool on) {
    ModFile *m = &mods[i];
    if (m->on == on) return;
    char on_path[300], off_path[300];
    snprintf(on_path, sizeof on_path, "mods/%s", m->file);
    snprintf(off_path, sizeof off_path, "mods/disabled/%s", m->file);
    mods_mkdir("mods/disabled");
    if (rename(on ? off_path : on_path, on ? on_path : off_path)) {
        LOG("[mods] can't move %s\n", m->file);
        return;
    }
    m->on = on;
}

bool mods_pending(void) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].on != mods[i].was_on) return true;
    return false;
}

#ifdef _WIN32
#include <windows.h>

#define STAMP "mods/.bcml/stamp"

// mods/'s mod files as text (name, size, time, in the order sorted): false if it can't be read
static bool mods_list(char *out, size_t cap) {
    DIR *d = opendir("mods");
    size_t n = 0;
    out[0] = 0;
    if (!d) return true;
    char names[256][256];
    int count = 0;
    for (struct dirent *e; (e = readdir(d)) && count < 256;) {
        if (strlen(e->d_name) < 256 && mod_file(e->d_name)) strcpy(names[count++], e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < count; i++) {
        char path[300];
        struct stat sb;
        snprintf(path, sizeof path, "mods/%s", names[i]);
        if (stat(path, &sb)) return false;
        n += (size_t)snprintf(out + n, cap - n, "%s %lld %lld\n", names[i], (long long)sb.st_size, (long long)sb.st_mtime);
        if (n >= cap) return false;
    }
    return true;
}

// What was last applied; false: nothing yet
static bool read_stamp(char *out, size_t cap) {
    FILE *f = fopen(STAMP, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    return true;
}

#ifndef MODS_TEST
void mods_sync(void) {
    static char now[1 << 16], last[1 << 16];
    if (getenv("SAVAGEWIND_NO_MODS") || !mods_list(now, sizeof now)) return;
    struct stat sb;
    if (read_stamp(last, sizeof last) ? !strcmp(now, last) : !now[0] && stat("mods/.merged", &sb)) return;
    char tool[1100];
    if (!rt_resource("swmods\\swmods.exe", "build/helper/swmods/swmods.exe", tool, sizeof tool)) {
        LOG("[mods] mods/ changed, but there is no swmods here: run tools/mods.py\n");
        return;
    }
    LOG("[mods] mods/ changed: applying\n");
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"%s\"", tool);
    STARTUPINFOA si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(tool, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        LOG("[mods] can't start %s (error %lu)\n", tool, GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code) { LOG("[mods] swmods failed (%lu), see mods/.bcml/errors.txt: the rest of mods/ is applied\n", code); return; }
    FILE *f = fopen(STAMP, "wb");
    if (f) { fputs(now, f); fclose(f); }
    LOG("[mods] applied\n");
}
#endif

#elif !defined(MODS_TEST)

void mods_sync(void) {}

#endif
