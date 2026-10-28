// The thread that watches the save folder for game_data.sav changes and feeds them to ach_update (runtime/achievements.h).
//
// The game writes a save in pieces, so a file is only read once its size and time stayed the same for two looks (2 s
// apart). Every game_data.sav under the save folder is read; each keeps its own achievements (runtime/achievements.h).
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "achievements.h"

#ifdef ACH_STANDALONE  // (tools/test_achievements.sh)
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#include "rt.h"
#endif

extern const char *g_save_dir;  // runtime/services/fs.c
bool test_warp_active(void);    // runtime/services/test_warp.c

#define POLL_MS 2000
#define MAX_FILES 32
#define MAX_SAVE (64u << 20)

typedef struct {
    char path[1024];
    time_t proc_mtime, pend_mtime;
    off_t proc_size, pend_size;
    bool seen, present, was_present;
} Watched;

static Watched files[MAX_FILES];
static int n_files;
static pthread_t thread;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;
static bool running, stop_req;

static Watched *slot_for(const char *path) {
    for (int i = 0; i < n_files; i++)
        if (!strcmp(files[i].path, path)) return &files[i];
    if (n_files >= MAX_FILES) return NULL;
    Watched *w = &files[n_files++];
    memset(w, 0, sizeof *w);
    snprintf(w->path, sizeof w->path, "%s", path);
    return w;
}

static void scan(const char *dir, int depth, void (*found)(const char *path, const struct stat *st)) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) scan(p, depth - 1, found);
        } else if (!strcmp(e->d_name, "game_data.sav")) {
            found(p, &st);
        }
    }
    closedir(d);
}

static bool read_all(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (unsigned long)sz > MAX_SAVE) { fclose(f); return false; }
    uint8_t *b = malloc((size_t)sz);
    bool ok = b && fread(b, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!ok) { free(b); return false; }
    *out = b, *n = (size_t)sz;
    return true;
}

static void on_file(const char *path, const struct stat *st) {
    Watched *w = slot_for(path);
    if (!w) return;
    w->present = w->was_present = true;
    if (ach_wants_reread(path)) w->seen = false, w->pend_mtime = st->st_mtime, w->pend_size = st->st_size;  // (read it now)
    if (w->seen && st->st_mtime == w->proc_mtime && st->st_size == w->proc_size) return;  // nothing new
    if (st->st_mtime != w->pend_mtime || st->st_size != w->pend_size) {  // changed since the last look: wait one more
        w->pend_mtime = st->st_mtime, w->pend_size = st->st_size;
        return;
    }
    uint8_t *data;
    size_t n;
    w->proc_mtime = st->st_mtime, w->proc_size = st->st_size, w->seen = true;  // (tried: a bad file isn't retried until it changes)
    if (!read_all(path, &data, &n)) return;
    AchSave s = {data, n};
    if (ach_save_ok(&s)) {
        int fresh = ach_update_at(path, &s, time(NULL), st->st_mtime);
        if (fresh) LOG("[achievements] %d new from %s\n", fresh, path);
    }
    free(data);
}

// A save file that has vanished was deleted: its achievements go with it.
static void after_scan(void) {
    for (int i = 0; i < n_files; i++) {
        Watched *w = &files[i];
        if (w->present || !w->was_present) continue;
        w->was_present = false;
        w->seen = false, w->pend_mtime = 0, w->pend_size = 0;  // (a file that comes back is read again)
        ach_game_gone(w->path);
    }
    for (int i = 0; i < n_files; i++) files[i].present = false;  // (set again by the next look)
}

static void *watch_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&wlock);
    while (!stop_req) {
        pthread_mutex_unlock(&wlock);
        scan(g_save_dir && *g_save_dir ? g_save_dir : "save", 4, on_file);
        after_scan();
        pthread_mutex_lock(&wlock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += POLL_MS / 1000;
        while (!stop_req && pthread_cond_timedwait(&wcond, &wlock, &until) != ETIMEDOUT) {}
    }
    pthread_mutex_unlock(&wlock);
    return NULL;
}

void ach_start(void) {
    if (running) return;
    ach_state_load("achievements.txt");  // (the data root is the working directory)
    ach_set_blocked(test_warp_active());  // test warps / flag edits would hand them out
    stop_req = false;
    if (pthread_create(&thread, NULL, watch_thread, NULL) == 0) running = true;
    ach_live_start();
}

void ach_stop(void) {
    if (!running) return;
    ach_live_stop();
    pthread_mutex_lock(&wlock);
    stop_req = true;
    pthread_cond_signal(&wcond);
    pthread_mutex_unlock(&wlock);
    pthread_join(thread, NULL);
    running = false;
}
