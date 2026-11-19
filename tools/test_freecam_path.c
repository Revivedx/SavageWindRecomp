// Synthetic unit test for freecam keyframe spline paths (runtime/hle/freecam.c).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hle/freecam.h"
#include "game_sites.h"

GameSites g_site = {0};

uint64_t plat_now_ns(void) { return 0; }
bool framepace_title_ui(void) { return false; }
bool framepace_loading_ui(void) { return false; }
bool menu_input_captured(void) { return false; }
void photo_reset(bool on) { (void)on; }
void photo_frame(void) {}
uint64_t renderer_present_count(void) { return 0; }

static int checks, fails;
static void check(bool ok, const char *name) {
    checks++;
    if (!ok) {
        fails++;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

int main(void) {
    FreecamKeyframe kf[3] = {
        {{0.0f, 10.0f, 0.0f}, 3.0f, 0.0f},
        {{10.0f, 20.0f, 5.0f}, -3.0f, 0.5f},
        {{20.0f, 10.0f, 0.0f}, -2.5f, -0.5f},
    };
    float eye[3], yaw = 0, pitch = 0;

    // Endpoints match kf[0] and kf[2]
    freecam_eval_path(kf, 3, 0.0f, false, eye, &yaw, &pitch);
    check(near(eye[0], 0.0f, 1e-4f) && near(eye[1], 10.0f, 1e-4f) && near(yaw, 3.0f, 1e-4f), "start endpoint");

    freecam_eval_path(kf, 3, 1.0f, false, eye, &yaw, &pitch);
    check(near(eye[0], 20.0f, 1e-4f) && near(eye[1], 10.0f, 1e-4f) && near(pitch, -0.5f, 1e-4f), "end endpoint");

    // Midpoint (t=0.5) hits kf[1]
    freecam_eval_path(kf, 3, 0.5f, false, eye, &yaw, &pitch);
    check(near(eye[0], 10.0f, 1e-4f) && near(eye[1], 20.0f, 1e-4f) && near(eye[2], 5.0f, 1e-4f), "midpoint eye");

    // Shortest-angle yaw wrap across +/-pi between +3.0 and -3.0 (diff is +0.283185 rad, not -6.0 rad)
    freecam_eval_path(kf, 3, 0.25f, false, eye, &yaw, &pitch);
    check(fabsf(yaw) > 2.8f, "yaw wraps shortest path across pi");

    // Loop continuity: t=0 and t=1 match kf[0]
    float eye0[3], eye1[3], y0, y1, p0, p1;
    freecam_eval_path(kf, 3, 0.0f, true, eye0, &y0, &p0);
    freecam_eval_path(kf, 3, 1.0f, true, eye1, &y1, &p1);
    check(near(eye0[0], eye1[0], 1e-4f) && near(eye0[1], eye1[1], 1e-4f) && near(p0, p1, 1e-4f), "loop wraps to start");

    // Keyframe management API
    freecam_keyframe_clear();
    check(freecam_keyframe_count() == 0 && freecam_path_duration() == 0.0f, "clear resets count and duration");
    freecam_keyframe_add();
    freecam_keyframe_add();
    check(freecam_keyframe_count() == 2 && near(freecam_path_duration(), 3.0f, 1e-4f), "2 keyframes = 3.0s default");
    freecam_path_step_duration(1.5f);
    check(near(freecam_path_duration(), 4.5f, 1e-4f), "step duration +1.5s");
    freecam_path_loop_toggle();
    check(freecam_path_loop() && near(freecam_path_duration(), 9.0f, 1e-4f), "loop doubles 2-kf duration");
    freecam_path_toggle();
    check(freecam_path_playing(), "path starts playing");
    freecam_keyframe_pop();
    check(freecam_keyframe_count() == 1 && !freecam_path_playing(), "pop below 2 stops playback");

    printf("test_freecam_path: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
