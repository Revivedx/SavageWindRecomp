// Free camera / photo mode (Home). The game's cameras are sead::LookAtCamera; their view comes from eye (+0x38), at
// (+0x44) and up (+0x50) in doUpdateMatrix (tools/patches.py _lookat_update calls rt_lookat first). The view cameras
// (in play: the main camera and a copy of it) have an eye off the origin and up near +y; the shadow cameras are built
// at the origin. While the free camera is on, the view cameras take its eye and look direction, the game's clock
// nearly stops (framepace.c) and the keyboard and mouse move the camera instead of Link (hle/input.c):
// W/S/A/D forward, back, left, right; E/Q up, down; Shift faster, Ctrl slower; the mouse turns it. On a controller
// both stick clicks turn it on and off; left stick moves, right stick turns, ZR / ZL up / down, R faster, L slower.
// Its photo extras (a filter, the hour, a picture) are in hle/photo.c.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

static bool active;
static float eye[3], yaw, pitch;   // yaw around +y from +z, pitch up from the horizon (radians)
static float seen_eye[3], seen_at[3];
static bool seen;
static bool keys[8];               // forward, back, left, right, up, down, fast, slow
static uint64_t last_ns;
static float pad_l[2], pad_r[2], pad_up, pad_down;  // the controller while it is on (hle/input.c recompute_pad)
static bool pad_fast, pad_slow;

bool freecam_active(void) { return active; }

static bool view_camera(const float *f) {
    return (fabsf(f[0]) + fabsf(f[1]) + fabsf(f[2])) > 1.0f && f[7] > 0.5f;  // eye off the origin, up near +y
}

// (debugging) SAVAGEWIND_LOG_CAMERAS=<frame>: the cameras updated during that presented frame, with eye and at
static void log_cameras(uint64_t cam) {
    static long at = -2;
    if (at == -2) at = getenv("SAVAGEWIND_LOG_CAMERAS") ? atol(getenv("SAVAGEWIND_LOG_CAMERAS")) : -1;
    if (at < 0) return;
    extern uint64_t renderer_present_count(void);
    if ((long)renderer_present_count() != at) return;
    const float *f = (const float *)(uintptr_t)(cam + 0x38);
    LOG("[cam] %llx eye %.2f %.2f %.2f at %.2f %.2f %.2f up %.2f %.2f %.2f\n", (unsigned long long)cam, f[0], f[1], f[2],
        f[3], f[4], f[5], f[6], f[7], f[8]);
}

void rt_lookat(uint64_t cam) {
    log_cameras(cam);
    float *f = (float *)(uintptr_t)(cam + 0x38);
    if (!view_camera(f)) return;
    if (!active) {
        memcpy(seen_eye, f, sizeof seen_eye);
        memcpy(seen_at, f + 3, sizeof seen_at);
        seen = true;
        return;
    }
    float cp = cosf(pitch);
    f[0] = eye[0]; f[1] = eye[1]; f[2] = eye[2];
    f[3] = eye[0] + sinf(yaw) * cp; f[4] = eye[1] + sinf(pitch); f[5] = eye[2] + cosf(yaw) * cp;
    f[6] = 0; f[7] = 1; f[8] = 0;
}

void freecam_toggle(void) {
    if (!active && !seen) return;  // (no view camera yet: the title screen)
    active = !active;
    extern void photo_reset(bool on);  // (hle/photo.c: its filter and hour go with the free camera)
    photo_reset(active);
    if (active) {
        memcpy(eye, seen_eye, sizeof eye);
        float dx = seen_at[0] - seen_eye[0], dy = seen_at[1] - seen_eye[1], dz = seen_at[2] - seen_eye[2];
        float h = sqrtf(dx * dx + dz * dz);
        yaw = atan2f(dx, dz);
        pitch = atan2f(dy, h > 1e-6f ? h : 1e-6f);
        memset(keys, 0, sizeof keys);
        last_ns = 0;
    }
    LOG("[freecam] %s\n", active ? "on" : "off");
}

// Keys while it is on (hle/input.c): SDL scancodes of W S A D E Q, Shift and Ctrl
void freecam_key(int scancode, bool down) {
    static const int map[][2] = {{26, 0}, {22, 1}, {4, 2}, {7, 3}, {8, 4}, {20, 5}, {225, 6}, {229, 6}, {224, 7}, {228, 7}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i][0] == scancode) keys[map[i][1]] = down;
}

// The controller (hle/input.c): left stick moves, right stick turns, ZR / ZL up / down, R faster, L slower
void freecam_pad(float lx, float ly, float rx, float ry, float up, float down, bool fast, bool slow) {
    pad_l[0] = lx, pad_l[1] = ly, pad_r[0] = rx, pad_r[1] = ry;
    pad_up = up > 0.1f ? up : 0, pad_down = down > 0.1f ? down : 0;
    pad_fast = fast, pad_slow = slow;
}

void freecam_mouse(float dx, float dy) {
    yaw -= dx * 0.003f;
    pitch -= dy * 0.003f;
    if (pitch > 1.5f) pitch = 1.5f;
    if (pitch < -1.5f) pitch = -1.5f;
}

// Each frame (framepace.c rt_vfr_update): moves the eye by the keys held, 8 m/s (x5 with Shift, /5 with Ctrl)
void freecam_frame(void) {
    {   // (testing) SAVAGEWIND_FREECAM_TEST=<frame>: at that presented frame it turns on, 30 m higher, turned 90 degrees
        static long at = -2;
        if (at == -2) at = getenv("SAVAGEWIND_FREECAM_TEST") ? atol(getenv("SAVAGEWIND_FREECAM_TEST")) : -1;
        extern uint64_t renderer_present_count(void);
        if (at >= 0 && (long)renderer_present_count() >= at && !active && seen) {
            freecam_toggle();
            eye[1] += 30.0f;
            yaw += 1.5708f;
            pitch = -0.35f;
            at = -1;
        }
    }
    extern void photo_frame(void);  // (hle/photo.c) the hour asked for, or put back
    photo_frame();
    if (!active) return;
    uint64_t now = plat_now_ns();
    float dt = last_ns ? (float)((now - last_ns) / 1e9) : 0;
    last_ns = now;
    if (dt > 0.1f) dt = 0.1f;
    yaw -= pad_r[0] * 2.0f * dt;  // (full tilt: about 115 degrees a second)
    pitch += pad_r[1] * 1.5f * dt;
    if (pitch > 1.5f) pitch = 1.5f;
    if (pitch < -1.5f) pitch = -1.5f;
    float v = 8.0f * (keys[6] || pad_fast ? 5.0f : 1.0f) / (keys[7] || pad_slow ? 5.0f : 1.0f) * dt;
    float fx = sinf(yaw) * cosf(pitch), fy = sinf(pitch), fz = cosf(yaw) * cosf(pitch);
    float rx = -cosf(yaw), rz = sinf(yaw);  // (right of the look direction, on the horizon)
    float f = (keys[0] ? 1.0f : 0) - (keys[1] ? 1.0f : 0) + pad_l[1], r = (keys[3] ? 1.0f : 0) - (keys[2] ? 1.0f : 0) + pad_l[0];
    float u = (keys[4] ? 1.0f : 0) - (keys[5] ? 1.0f : 0) + pad_up - pad_down;
    eye[0] += (fx * f + rx * r) * v;
    eye[1] += fy * f * v + u * v;
    eye[2] += (fz * f + rz * r) * v;
}
