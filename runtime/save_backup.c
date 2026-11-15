// Saves > Back up at each start: a copy of the save folder as the game starts, into save-backups/auto-<date and time>
// next to it (sc_backup, the copy the Saves tab's "Back up saves" makes), the newest AUTO_BACKUPS of them kept. Only
// when something in the save folder changed since the last one (the newest file's time, in AUTO_LAST; not option.sav,
// which the game writes at every start), once per run of
// the program, and not for tests with a scratch save folder (SAVAGEWIND_SAVE_DIR). The player's own backups
// (save-backups/save-*) are never removed. From Slop of the Wild.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"
#include "services/save_convert.h"
#include "settings.h"

#define SAVE_DIR "save"
#define BACKUP_DIR SAVE_DIR "-backups"            // (sc_backup_path's folder)
#define AUTO_PREFIX "auto-"
#define AUTO_LAST BACKUP_DIR "/auto-last.txt"
#define AUTO_BACKUPS 5

static SDL_EnumerationResult SDLCALL newest(void *out, const char *dir, const char *name) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo in;
    if (!SDL_GetPathInfo(p, &in)) return SDL_ENUM_CONTINUE;
    if (in.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, newest, out);
    else if (strcmp(name, "option.sav") && in.modify_time > *(SDL_Time *)out) *(SDL_Time *)out = in.modify_time;
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL remove_tree(void *unused, const char *dir, const char *name) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo in;
    if (SDL_GetPathInfo(p, &in) && in.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, remove_tree, unused);
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

typedef struct { char names[64][48]; int n; } List;

static SDL_EnumerationResult SDLCALL list_auto(void *out, const char *dir, const char *name) {
    (void)dir;
    List *l = out;
    if (l->n < 64 && !strncmp(name, AUTO_PREFIX, strlen(AUTO_PREFIX)))
        snprintf(l->names[l->n++], sizeof l->names[0], "%s", name);
    return SDL_ENUM_CONTINUE;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }  // (dated names: oldest first)

// (runtime/main.c: before each game start takes the save folder's lock)
void save_backup_at_start(void) {
    static bool done;  // (once a run: a reboot starts the game again in the same process)
    if (done || !g_settings.save_backups || getenv("SAVAGEWIND_SAVE_DIR")) return;
    done = true;
    const char *save = g_settings.save_dir[0] ? g_settings.save_dir : SAVE_DIR;
    SDL_Time t = 0, last = 0;
    SDL_EnumerateDirectory(save, newest, &t);
    if (!t) return;  // (no saves yet)
    char backup_dir[1024], auto_last[1024];
    sc_backup_path(save, backup_dir, sizeof backup_dir);
    snprintf(auto_last, sizeof auto_last, "%s/auto-last.txt", backup_dir);
    FILE *f = fopen(auto_last, "r");
    if (f) {
        long long v = 0;
        if (fscanf(f, "%lld", &v) == 1) last = v;
        fclose(f);
    }
    if (t == last) {
        LOG("[saves] no backup at start: nothing changed since the last one\n");
        return;
    }
    char when[32] = "0", to[1024];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
    snprintf(to, sizeof to, "%s/" AUTO_PREFIX "%s", backup_dir, when);
    static ScReport r;
    if (sc_backup(save, to, &r) != SC_OK) {
        LOG("[saves] backup at start failed (code %d)\n", (int)r.code);
        return;
    }
    f = fopen(auto_last, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)t);
        fclose(f);
    }
    static List l;
    l.n = 0;
    SDL_EnumerateDirectory(backup_dir, list_auto, &l);
    qsort(l.names, l.n, sizeof l.names[0], by_name);
    for (int i = 0; i + AUTO_BACKUPS < l.n; i++) {
        snprintf(to, sizeof to, "%s/%s", backup_dir, l.names[i]);
        SDL_EnumerateDirectory(to, remove_tree, NULL);
        SDL_RemovePath(to);
    }
    LOG("[saves] backed up at start to %s/" AUTO_PREFIX "%s\n", backup_dir, when);
}
