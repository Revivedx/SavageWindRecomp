// Gameplay > Keep crouch after camera (settings.h camera_keep_stealth): remember whether Link was crouched
// when opening the Camera rune and return to crouch idle instead of standing idle when Camera finishes normally.
// Patches in the Player AI selector and Camera::leave() (tools/patches.py _camera_enter_start,
// _camera_exit_finished, _camera_exit_active, _camera_leave) record entry, choose the exit state, and clear stale
// state on any non-normal Camera exit.
#include <stdbool.h>
#include <stdint.h>

#include "hle.h"
#include "settings.h"

static uint64_t g_camera_stealth_player;
static bool g_camera_stealth_crouched;

void rt_camera_stealth_enter(uint64_t player, int is_squat) {
    g_camera_stealth_player = player;
    g_camera_stealth_crouched = player != 0 && is_squat != 0;
}

int rt_camera_stealth_exit(uint64_t player) {
    bool restore = g_settings.camera_keep_stealth && g_camera_stealth_crouched &&
                   player != 0 && player == g_camera_stealth_player;
    g_camera_stealth_crouched = false;
    g_camera_stealth_player = 0;
    return restore ? 1 : 0;
}

void rt_camera_stealth_leave(void) {
    g_camera_stealth_crouched = false;
    g_camera_stealth_player = 0;
}

RT_ON_RESET(camera_stealth_reset) { rt_camera_stealth_leave(); }
