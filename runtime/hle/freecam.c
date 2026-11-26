// Free camera / photo mode (Home). The game's cameras are sead::LookAtCamera; their view comes from eye (+0x38), at
// (+0x44) and up (+0x50) in doUpdateMatrix (tools/patches.py _lookat_update calls rt_lookat first). The view cameras
// (in play: the main camera and a copy of it) have an eye off the origin and up near +y; the shadow cameras are built
// at the origin. While the free camera is on, the view cameras take its eye and look direction, the game's clock
// nearly stops (framepace.c) and the keyboard and mouse move the camera instead of Link (hle/input.c):
// W/S/A/D forward, back, left, right; E/Q up, down; Shift faster, Ctrl slower; the mouse turns it. On a controller
// both stick clicks turn it on and off; left stick moves, right stick turns, ZR / ZL up / down, R faster, L slower.
// Its photo extras (a filter, the hour, a picture) are in hle/photo.c.
#include "freecam.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../framepace.h"
#include "../game_sites.h"

static bool active;
static float eye[3], yaw, pitch;   // yaw around +y from +z, pitch up from the horizon (radians)
static float seen_eye[3], seen_at[3];
static bool seen;
static bool keys[8];               // forward, back, left, right, up, down, fast, slow
static uint64_t last_ns;
static float pad_l[2], pad_r[2], pad_up, pad_down;  // the controller while it is on (hle/input.c recompute_pad)
static bool pad_fast, pad_slow;

static pthread_mutex_t path_mu = PTHREAD_MUTEX_INITIALIZER;
static FreecamKeyframe keyframes[FREECAM_MAX_KEYFRAMES];
static int nkeyframes;
static bool path_playing;
static bool path_loop;
static float path_sec_per_seg = 3.0f;
static float path_t;

RT_ON_RESET(freecam_reset) {
    pthread_mutex_lock(&path_mu);
    active = false;
    seen = false;
    path_playing = false;
    path_t = 0;
    memset(keys, 0, sizeof keys);
    pthread_mutex_unlock(&path_mu);
}

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
    // Only in play: not while the game is paused (its menus, the map, a pause for loading), on the start screen or
    // while the overlay has the controls. Turning it off always works.
    extern bool menu_input_captured(void);
    bool paused = g_site.pause_state && LD32(g_site.pause_state) != 0;
    if (!active && (paused || framepace_title_ui() || framepace_loading_ui() || menu_input_captured())) {
        LOG("[freecam] not now: the game is paused or in a menu\n");
        return;
    }
    active = !active;
    if (!active) {
        pthread_mutex_lock(&path_mu);
        path_playing = false;
        pthread_mutex_unlock(&path_mu);
    }
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

static float catmull_rom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static float wrap_pi(float a) {
    const float two_pi = 6.28318530718f, pi = 3.14159265359f;
    a = fmodf(a + pi, two_pi);
    if (a < 0) a += two_pi;
    return a - pi;
}

void freecam_eval_path(const FreecamKeyframe *kf, int n, float t_norm, bool loop, float out_eye[3], float *out_yaw,
                       float *out_pitch) {
    if (!kf || n <= 0) return;
    if (n == 1) {
        memcpy(out_eye, kf[0].eye, sizeof(float) * 3);
        *out_yaw = kf[0].yaw;
        *out_pitch = kf[0].pitch;
        return;
    }
    if (t_norm < 0.0f) t_norm = 0.0f;
    if (t_norm > 1.0f) t_norm = 1.0f;
    int segs = loop ? n : n - 1;
    float scaled = t_norm * (float)segs;
    int seg = (int)floorf(scaled);
    if (seg >= segs) seg = segs - 1;
    float u = scaled - (float)seg;
    int i0, i1, i2, i3;
    if (loop) {
        i0 = (seg - 1 + n) % n;
        i1 = seg % n;
        i2 = (seg + 1) % n;
        i3 = (seg + 2) % n;
    } else {
        i0 = seg > 0 ? seg - 1 : 0;
        i1 = seg;
        i2 = seg + 1 < n ? seg + 1 : n - 1;
        i3 = seg + 2 < n ? seg + 2 : n - 1;
    }
    for (int c = 0; c < 3; c++)
        out_eye[c] = catmull_rom(kf[i0].eye[c], kf[i1].eye[c], kf[i2].eye[c], kf[i3].eye[c], u);
    float y1 = kf[i1].yaw;
    float y0 = y1 + wrap_pi(kf[i0].yaw - y1);
    float y2 = y1 + wrap_pi(kf[i2].yaw - y1);
    float y3 = y2 + wrap_pi(kf[i3].yaw - y2);
    *out_yaw = catmull_rom(y0, y1, y2, y3, u);
    float p = catmull_rom(kf[i0].pitch, kf[i1].pitch, kf[i2].pitch, kf[i3].pitch, u);
    if (p > 1.5f) p = 1.5f;
    if (p < -1.5f) p = -1.5f;
    *out_pitch = p;
}

void freecam_keyframe_add(void) {
    pthread_mutex_lock(&path_mu);
    if (nkeyframes < FREECAM_MAX_KEYFRAMES) {
        FreecamKeyframe *k = &keyframes[nkeyframes++];
        memcpy(k->eye, eye, sizeof k->eye);
        k->yaw = yaw;
        k->pitch = pitch;
        LOG("[freecam] keyframe %d added (%.1f, %.1f, %.1f)\n", nkeyframes, eye[0], eye[1], eye[2]);
    }
    pthread_mutex_unlock(&path_mu);
}

void freecam_keyframe_pop(void) {
    pthread_mutex_lock(&path_mu);
    if (nkeyframes > 0) {
        nkeyframes--;
        if (nkeyframes < 2) path_playing = false;
        LOG("[freecam] keyframe removed (%d left)\n", nkeyframes);
    }
    pthread_mutex_unlock(&path_mu);
}

void freecam_keyframe_clear(void) {
    pthread_mutex_lock(&path_mu);
    nkeyframes = 0;
    path_playing = false;
    path_t = 0;
    LOG("[freecam] keyframes cleared\n");
    pthread_mutex_unlock(&path_mu);
}

void freecam_path_toggle(void) {
    pthread_mutex_lock(&path_mu);
    if (nkeyframes >= 2) {
        path_playing = !path_playing;
        if (path_playing) path_t = 0;
        LOG("[freecam] path %s (%d keyframes)\n", path_playing ? "playing" : "stopped", nkeyframes);
    }
    pthread_mutex_unlock(&path_mu);
}

void freecam_path_loop_toggle(void) {
    pthread_mutex_lock(&path_mu);
    path_loop = !path_loop;
    LOG("[freecam] path loop %s\n", path_loop ? "on" : "off");
    pthread_mutex_unlock(&path_mu);
}

void freecam_path_step_duration(float delta_sec) {
    pthread_mutex_lock(&path_mu);
    path_sec_per_seg += delta_sec;
    if (path_sec_per_seg < 0.5f) path_sec_per_seg = 0.5f;
    if (path_sec_per_seg > 30.0f) path_sec_per_seg = 30.0f;
    pthread_mutex_unlock(&path_mu);
}

bool freecam_path_playing(void) {
    pthread_mutex_lock(&path_mu);
    bool r = path_playing;
    pthread_mutex_unlock(&path_mu);
    return r;
}

bool freecam_path_loop(void) {
    pthread_mutex_lock(&path_mu);
    bool r = path_loop;
    pthread_mutex_unlock(&path_mu);
    return r;
}

int freecam_keyframe_count(void) {
    pthread_mutex_lock(&path_mu);
    int r = nkeyframes;
    pthread_mutex_unlock(&path_mu);
    return r;
}

float freecam_path_duration(void) {
    pthread_mutex_lock(&path_mu);
    int segs = nkeyframes >= 2 ? (path_loop ? nkeyframes : nkeyframes - 1) : 0;
    float r = (float)segs * path_sec_per_seg;
    pthread_mutex_unlock(&path_mu);
    return r;
}

bool freecam_path_key(int scancode) {
    switch (scancode) {
    case SDL_SCANCODE_K: freecam_keyframe_add(); return true;
    case SDL_SCANCODE_BACKSPACE: freecam_keyframe_pop(); return true;
    case SDL_SCANCODE_C: freecam_keyframe_clear(); return true;
    case SDL_SCANCODE_P: freecam_path_toggle(); return true;
    case SDL_SCANCODE_O: freecam_path_loop_toggle(); return true;
    case SDL_SCANCODE_MINUS: freecam_path_step_duration(-0.5f); return true;
    case SDL_SCANCODE_EQUALS: freecam_path_step_duration(0.5f); return true;
    }
    return false;
}

void freecam_pad_buttons(uint32_t buttons) {
    static uint32_t was;
    uint32_t down = buttons & ~was;
    was = buttons;
    if (down >> SDL_GAMEPAD_BUTTON_NORTH & 1) freecam_keyframe_add();
    if (down >> SDL_GAMEPAD_BUTTON_WEST & 1) freecam_path_toggle();
    if (down >> SDL_GAMEPAD_BUTTON_EAST & 1) freecam_keyframe_pop();
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
    pthread_mutex_lock(&path_mu);
    if (path_playing && nkeyframes >= 2) {
        int segs = path_loop ? nkeyframes : nkeyframes - 1;
        float total = (float)segs * path_sec_per_seg;
        path_t += dt;
        if (path_loop && total > 0) {
            path_t = fmodf(path_t, total);
        } else if (path_t >= total) {
            path_t = total;
            path_playing = false;
        }
        float tn = total > 0 ? path_t / total : 1.0f;
        freecam_eval_path(keyframes, nkeyframes, tn, path_loop, eye, &yaw, &pitch);
        pthread_mutex_unlock(&path_mu);
        return;
    }
    pthread_mutex_unlock(&path_mu);
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
