// Host-side screenshot capture (PrintScreen during play)
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "../platform.h"

static atomic_bool shot_pending;
static char saved_path[256];
static atomic_int saved_seq;

// (vulkan/device.c, the presenter) the picture asked for has been written to path
void photo_saved(const char *path) {
    if (!atomic_exchange(&shot_pending, false)) return;
    snprintf(saved_path, sizeof saved_path, "%s", path);
    atomic_fetch_add(&saved_seq, 1);
}

// (ui/menu.mm) a number that changes with each picture saved, and its path
int photo_saved_seq(const char **path) {
    *path = saved_path;
    return atomic_load(&saved_seq);
}

static void shot(void) {
    extern char g_window_shot_request[256];  // (hle/input.c: the presenter saves the next frame there)
    char when[32] = "0";
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%Y%m%d-%H%M%S", tm);
    SDL_CreateDirectory("screenshots");  // (in the data folder: the working directory, runtime/paths.h)
    static int same_second;
    static char last[32];
    same_second = strcmp(last, when) ? 0 : same_second + 1;
    snprintf(last, sizeof last, "%s", when);
    if (same_second) snprintf(g_window_shot_request, sizeof g_window_shot_request, "screenshots/savagewind-%s-%d.png", when, same_second);
    else snprintf(g_window_shot_request, sizeof g_window_shot_request, "screenshots/savagewind-%s.png", when);
    atomic_store(&shot_pending, true);
    LOG("[photo] picture: %s\n", g_window_shot_request);
}

// Request a screenshot to be saved by the presenter on the next frame
void photo_take(void) {
    shot();
}

RT_ON_RESET(photo_reset) {
    atomic_store(&shot_pending, false);
}
