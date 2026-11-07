// Photo mode extras for the free camera (hle/freecam.c), only while it is on: a colour filter, the hour of the day,
// and a picture saved to screenshots/. Keys: F8 next filter, [ and ] an hour back and on, F7 a picture; on a
// controller the d-pad: up filter, left / right hour, down picture (the free camera has the controller then, so the
// game sees none of these). Turning the free camera off puts the hour back and drops the filter: the photo's look is
// not left on the game.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "../platform.h"

// Over Video > Colour's own settings, which stay as they are (runtime/color_grade.c color_grade_get)
static const struct { const char *name; int saturation, contrast, temperature, exposure; } filters[] = {
    {"none", 0, 0, 0, 0},      {"black and white", -100, 15, 0, 0}, {"faded", -50, 15, 85, -3},
    {"vivid", 45, 15, 0, 0},   {"golden", 0, 10, 55, -3},           {"cold", -10, 5, -60, 0},
};
#define FILTERS ((int)(sizeof filters / sizeof filters[0]))
static atomic_int filter;

// Requests from the input thread, taken up on the game's own (photo_frame): the clock lives in guest memory
static atomic_int hour_steps, hour_reset;
static float hour_before = -1;  // the hour when the free camera came on and the photo first changed it

float test_warp_hour(bool *held);       // runtime/services/test_warp.c (the control socket's `hour`)
void test_warp_set_hour(float hour);
bool freecam_active(void);

bool photo_filter_get(int *saturation, int *contrast, int *temperature, int *exposure) {
    int f = filter;
    if (!f || !freecam_active()) return false;
    *saturation = filters[f].saturation, *contrast = filters[f].contrast;
    *temperature = filters[f].temperature, *exposure = filters[f].exposure;
    return true;
}

static void filter_next(void) {
    filter = (filter + 1) % FILTERS;
    LOG("[photo] filter: %s\n", filters[filter].name);
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
    LOG("[photo] picture: %s\n", g_window_shot_request);
}

// (freecam.c) on or off
void photo_reset(bool on) {
    filter = 0;
    hour_steps = 0;
    if (!on) hour_reset = 1;
}

// (hle/input.c) keys while the free camera is on; true: taken
bool photo_key(int scancode) {
    switch (scancode) {
    case SDL_SCANCODE_F8: filter_next(); return true;
    case SDL_SCANCODE_F7: shot(); return true;
    case SDL_SCANCODE_LEFTBRACKET: hour_steps--; return true;
    case SDL_SCANCODE_RIGHTBRACKET: hour_steps++; return true;
    }
    return false;
}

// (hle/input.c) the controller's buttons while the free camera is on (SDL_GamepadButton bits): new presses only
void photo_pad(uint32_t buttons) {
    static uint32_t was;
    uint32_t down = buttons & ~was;
    was = buttons;
    if (down >> SDL_GAMEPAD_BUTTON_DPAD_UP & 1) filter_next();
    if (down >> SDL_GAMEPAD_BUTTON_DPAD_DOWN & 1) shot();
    if (down >> SDL_GAMEPAD_BUTTON_DPAD_LEFT & 1) hour_steps--;
    if (down >> SDL_GAMEPAD_BUTTON_DPAD_RIGHT & 1) hour_steps++;
}

// (freecam.c freecam_frame, each frame on the game's thread) the hour changes asked for
void photo_frame(void) {
    if (hour_reset) {
        hour_reset = 0;
        if (hour_before >= 0) test_warp_set_hour(hour_before);
        hour_before = -1;
    }
    int steps = atomic_exchange(&hour_steps, 0);
    if (!steps || !freecam_active()) return;
    float now = test_warp_hour(NULL);
    if (now < 0) return;  // (no world yet)
    if (hour_before < 0) hour_before = now;
    float h = (float)((int)now + steps);
    while (h < 0) h += 24;
    while (h >= 24) h -= 24;
    test_warp_set_hour(h);
    LOG("[photo] hour %.1f -> %.0f\n", now, h);
}
