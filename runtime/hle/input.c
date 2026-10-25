// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c has the defaults: keyboard and mouse laid out for
// BOTW, controller by position, Switch A = east). Keyboard bindings can name mouse buttons; the mouse's motion turns
// the camera (the right stick) while the cursor is held in the window, or tilts the controller while the tilt key is
// held (motion controls: runtime/hle/motion.c, which also takes the controller's gyroscope and accelerometer).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../motion.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static uint8_t mouse_down[8];  // SDL_BUTTON_LEFT .. X2 (keyboard bindings BIND_MOUSE + button)
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;

// Host keys / buttons held by a test script (SAVAGEWIND_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in SAVAGEWIND_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)
static _Atomic int last_device;  // DEVICE_*: what the player used last
static bool tilt_held;  // the tilt key: the mouse tilts the controller instead of turning the camera
extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

static int held_key(int sc) {
    if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) return mouse_down[sc - BIND_MOUSE];
    return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]);
}

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    int tilt = held_key(g_bind_key[BIND_WALK]) ? 13107 : 32767;  // walk: 40% (BOTW walks below about half)
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * tilt;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * tilt;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
    bool tilt_key = held_key(g_bind_key[BIND_TILT]) && !menu_input_captured();
    if (tilt_key != tilt_held) motion_tilt_held(tilt_held = tilt_key);
}

// A controller's stick with a radial dead zone (Controls page, a fraction of full tilt): within it the stick reads 0,
// past it the tilt is rescaled so it still starts from 0 and reaches full. The Switch's input service has one, and
// the game counts on it: its menus mute a stick when they open until the stick is back within 1% of center
// (FUN_7100fe8dc0, 1.8.2), so a stick resting a few percent off center (most do) stayed muted in the map and menus,
// and a worn one drifted in play.
static void dead_zone(_Atomic int32_t *x, _Atomic int32_t *y, int32_t rx, int32_t ry, float dz) {
    double m = sqrt((double)rx * rx + (double)ry * ry) / 32767;
    if (m <= dz || m <= 0) { *x = 0, *y = 0; return; }
    double k = (m >= 1 ? 1 : (m - dz) / (1 - dz)) / m;
    *x = (int32_t)lround(rx * k), *y = (int32_t)lround(ry * k);
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    dead_zone(&pad_lx, &pad_ly, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
              -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), g_settings.stick_deadzone[0]);
    dead_zone(&pad_rx, &pad_ry, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
              -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), g_settings.stick_deadzone[1]);
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    memset(mouse_down, 0, sizeof mouse_down);
    recompute_keyboard();
}

// ---- mouse camera
// While the game has the window (focused, no overlay menu, no scripted run) the cursor is held in it (SDL relative
// mode). Its motion turns the camera through the game's own camera input (below) when the game code has the patches
// for it (tools/patches.py _cam_stick, _cam_pitch), else, and wherever no camera reads it (the map), it becomes the
// right stick: speed in pixels per second, scaled so that about 1200 px/s is a full tilt at sensitivity 1, plus a
// small offset past the game's own stick dead zone so slow motion still turns it. SAVAGEWIND_MOUSE_SCALE (px/s for
// a full tilt) and SAVAGEWIND_MOUSE_DEADZONE (0..1) tune that.
static _Atomic int64_t mouse_dx_milli, mouse_dy_milli;  // motion since the last stick sample, 1/1000 pixel
static _Atomic int64_t cam_dx_milli, cam_dy_milli;      // motion since the last camera frame (input_cam_frame)
static _Atomic bool mouse_held;                         // relative mode on (main thread decides, game thread reads)

// The camera's own input. Every camera action reads the right stick through one function, which the runtime answers
// while the mouse has the camera (rt_cam_stick), and turns by what it gives x its speeds x the frame's length (the
// VFR ratio: 1 per 30th of a second). The mouse gives its motion in the frame (input_cam_frame, at the frame's VFR
// update) in degrees - 0.022 per count x the sensitivity, as Quake and Source count it - over the turn a full tilt
// makes in that frame: CAM_DEG_PER_TILT per 30th of a second (CameraChase's yaw at the game's default camera speed:
// 1.6 x 1.78 x its lngStickScale, 1). Neither clamped to the stick's circle (no top speed) nor curved (no dead zone,
// no square), so a frame's turn is the mouse's motion in it, at any frame rate.
// Both axes alike: the cameras turn the horizontal part CAM_YAW_PER_PITCH times as fast as the vertical one (the
// game's 1.78 for the horizontal stick speed), so the vertical part gets that much more of the mouse's motion. The
// chase-like cameras square the vertical part once more: rt_cam_pitch leaves that out for the mouse, scaled to turn
// as far as the horizontal part (CAM_PITCH_LINEAR: unsquared, CameraChase's vertical part turns 2.089 degrees a 30th
// of a second for 1, measured; the yaw 2.848). The bow's camera (CameraAiming) and a held item's (CameraAiming2)
// scale the two apart again (lngStickScale 0.7 and latStickScale 1; 0.8 and 1.5): rt_cam_pitch_scale gives their
// vertical part the horizontal part's scale. Each camera's own speed stays (the bow's 0.7, its zoom's 0.42, the
// scope's 0.5...), and so do the game's camera speed setting and its Camera X-axis / Y-axis options (Inverted: the
// cameras' controller turns the stick around for them, the runtime the mouse's motion, rt_cam_reverse).
// SAVAGEWIND_MOUSE_NATIVE=0 moves the right stick instead; SAVAGEWIND_MOUSE_LOG=<n> logs n camera input reads (the
// caller, the input and, from CameraChase's main update, the angles it starts from).
#define CAM_DEG_PER_COUNT 0.022
#define CAM_YAW_PER_PITCH 1.78
#define CAM_DEG_PER_TILT (1.6 * CAM_YAW_PER_PITCH)
#define CAM_PITCH_LINEAR (float)(CAM_DEG_PER_TILT / (2.0892 * CAM_YAW_PER_PITCH))
static bool test_mouse;               // SAVAGEWIND_HOST_INPUT moves the mouse ("mouse:"): taken as held
static _Atomic uint64_t cam_read_ns;  // the last camera input read (rt_cam_stick): a camera runs, through the patch
static _Atomic bool cam_mouse;        // the mouse gives this frame's camera input ...
static float cam_vx, cam_vy;          // ... this one (game thread, input_cam_frame)
static _Atomic int cam_reverse;       // the game's Camera X-axis / Y-axis options: Inverted (bits 0 / 1, rt_cam_reverse)

static int cam_log_left(void) {
    static int n = -1;
    if (n < 0) n = getenv("SAVAGEWIND_MOUSE_LOG") ? atoi(getenv("SAVAGEWIND_MOUSE_LOG")) : 0;
    return n > 0 ? n-- : 0;
}

static bool cam_native_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_MOUSE_NATIVE") || atoi(getenv("SAVAGEWIND_MOUSE_NATIVE")) != 0;
    return on;
}

// A camera read its input through the patch in the last quarter second: the mouse is the camera's, not the stick's
static bool cam_reading(void) {
    uint64_t t = cam_read_ns;
    return cam_native_on() && t && plat_now_ns() - t < 250000000;
}

// runtime/hle/framepace.c rt_vfr_update, once a frame: the mouse's motion since the last frame, over the frame's
// length (ratio: its VFR ratio, which the cameras multiply their turn by)
void input_cam_frame(float ratio) {
    double dx = (double)atomic_exchange(&cam_dx_milli, 0) / 1000.0, dy = (double)atomic_exchange(&cam_dy_milli, 0) / 1000.0;
    if ((dx != 0 || dy != 0) && cam_log_left())
        LOG("[mouse] frame: %.1f %.1f counts, ratio %.3f, held %d/%d\n", dx, dy, ratio, (int)mouse_held, (int)test_mouse);
    if (!cam_native_on() || !(mouse_held || test_mouse) || !g_settings.mouse_camera || tilt_held || (dx == 0 && dy == 0)) {
        cam_mouse = false;
        return;
    }
    double k = CAM_DEG_PER_COUNT * g_settings.mouse_sens / (CAM_DEG_PER_TILT * (ratio > 0.05f ? ratio : 0.05f));
    cam_vx = (float)(dx * k);
    cam_vy = (float)(-dy * k * CAM_YAW_PER_PITCH * (g_settings.mouse_invert_y ? -1 : 1));
    cam_mouse = true;
}

// tools/patches.py _cam_stick: the camera's stick input (two floats at out) when the mouse gives it; 0: the stick's
int rt_cam_stick(uint64_t out, uint64_t lr) {
    cam_read_ns = plat_now_ns();
    bool mouse = cam_mouse && cam_native_on();
    if (cam_log_left()) {
        const float *p = (const float *)(uintptr_t)out;  // (CameraChase's main update: radius, pitch, yaw after it)
        LOG("[mouse] camera input from %llx: %s %.3f %.3f inverted %d (chase: pitch %.3f yaw %.3f)\n",
            (unsigned long long)lr, mouse ? "mouse" : "stick", mouse ? cam_vx : 0.0f, mouse ? cam_vy : 0.0f,
            (int)cam_reverse, p[3], p[4]);
        const char *o = getenv("SAVAGEWIND_MOUSE_LOG_X19");  // (debug) <caller lr>:<off>,...: floats of its x19
        if (o && strtoull(o, (char **)&o, 16) == lr) {
            const uint8_t *obj = (const uint8_t *)(uintptr_t)kernel_current_cpu()->x[19];
            char line[256];
            int n = 0;
            while (*o == ':' || *o == ',') {
                unsigned long off = strtoul(o + 1, (char **)&o, 16);
                n += snprintf(line + n, sizeof line - n, " +%lx %.3f", off, *(const float *)(obj + off));
                if (n >= (int)sizeof line) break;
            }
            LOG("[mouse]   x19%s\n", line);
        }
    }
    if (!mouse) return 0;
    float *v = (float *)(uintptr_t)out;
    int rev = cam_reverse;
    v[0] = rev & 1 ? -cam_vx : cam_vx, v[1] = rev & 2 ? -cam_vy : cam_vy;
    return 1;
}

// tools/patches.py _cam_reverse: the game's Camera X-axis (0) / Y-axis (1) option, Inverted or not, read every frame
// by the cameras' controller, which turns its right stick around for it; the mouse's motion turns around alike
void rt_cam_reverse(int axis, int on) {
    if (on) cam_reverse |= 1 << axis;
    else cam_reverse &= ~(1 << axis);
}

// tools/patches.py _cam_pitch: the chase-like cameras' vertical part, to be squared (0) or, from the mouse, scaled (1)
int rt_cam_pitch(float *y) {
    if (!cam_mouse || !cam_native_on()) return 0;
    *y *= CAM_PITCH_LINEAR;
    return 1;
}

// tools/patches.py _cam_pitch_aim: CameraAiming's scale for the vertical part (float bits): its latStickScale, or,
// from the mouse, its lngStickScale (the horizontal part's)
uint32_t rt_cam_pitch_scale(uint32_t lat, uint32_t lng) {
    return cam_mouse && cam_native_on() ? lng : lat;
}

static void mouse_stick(int32_t *x, int32_t *y) {
    static uint64_t last_ns;
    static int32_t hx, hy;
    *x = *y = 0;
    if (!(mouse_held || test_mouse) || !g_settings.mouse_camera || cam_reading()) {
        mouse_dx_milli = mouse_dy_milli = 0;
        hx = hy = 0;
        return;
    }
    uint64_t now = plat_now_ns();
    if (last_ns && now - last_ns < 4000000) { *x = hx; *y = hy; return; }  // (several polls in one frame: one sample)
    double dt = last_ns ? (double)(now - last_ns) / 1e9 : 1 / 60.0;
    last_ns = now;
    if (dt > 0.1) dt = 0.1;
    static double full = -1, dz = -1;
    if (full < 0) full = getenv("SAVAGEWIND_MOUSE_SCALE") ? atof(getenv("SAVAGEWIND_MOUSE_SCALE")) : 1200.0;
    if (dz < 0) dz = getenv("SAVAGEWIND_MOUSE_DEADZONE") ? atof(getenv("SAVAGEWIND_MOUSE_DEADZONE")) : 0.12;
    double dx = (double)atomic_exchange(&mouse_dx_milli, 0) / 1000.0, dy = (double)atomic_exchange(&mouse_dy_milli, 0) / 1000.0;
    double vx = dx / dt / (full / g_settings.mouse_sens), vy = -dy / dt / (full / g_settings.mouse_sens);
    if (g_settings.mouse_invert_y) vy = -vy;
    double m = sqrt(vx * vx + vy * vy);
    if (m > 1e-3) {
        double out = dz + m * (1 - dz);  // past the dead zone, then linear
        if (out > 1) out = 1;
        vx *= out / m, vy *= out / m;
    }
    hx = *x = (int32_t)lround(vx * 32767);
    hy = *y = (int32_t)lround(vy * 32767);
}

// Main thread, every window loop turn (runtime/nvn/vulkan/device.c ui_run): hold or free the cursor.
extern bool menu_cursor_free(void);     // runtime/ui/menu.mm: Esc freed the cursor (a click in the window holds it)
static bool win_focused = true;         // (main thread: window events and input_mouse_update)
void input_mouse_update(SDL_Window *w) {
    static bool cur;
    bool want = w && win_focused && g_settings.mouse_camera && !menu_input_captured() && !menu_cursor_free() &&
                !getenv("SAVAGEWIND_INPUT") && !getenv("SAVAGEWIND_HOST_INPUT");
    if (want == cur) return;
    cur = want;
    SDL_SetWindowRelativeMouseMode(w, want);
    mouse_held = want;
    if (!want) { memset(mouse_down, 0, sizeof mouse_down); recompute_keyboard(); }
    LOG("[input] mouse %s\n", want ? "held in the window (camera)" : "free");
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log
volatile int g_record_frames;        // F12: (debugging) save every other game image to build/frames/ until F12 again
volatile int g_skip_fs_off;          // F8: (debugging) draw the shaders SAVAGEWIND_SKIP_FSHASH names, until F8 again

// Events a test pushes (SAVAGEWIND_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives the player spoils it):
// SAVAGEWIND_NO_USER_INPUT=1, on by default whenever SAVAGEWIND_INPUT is set (or SAVAGEWIND_HOST_INPUT /
// SAVAGEWIND_BIND_TEST; SAVAGEWIND_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("SAVAGEWIND_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("SAVAGEWIND_INPUT") || getenv("SAVAGEWIND_HOST_INPUT") || getenv("SAVAGEWIND_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (SAVAGEWIND_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// The device the player used last (button icons, runtime/nvn/glyphs.c): a key, a mouse button or a mouse turn, or a
// controller's button or stick
int input_last_device(void) { return pad ? last_device : DEVICE_KEYBOARD; }

// Controller family for the Controls tab labels (runtime/hle/bindings.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// SAVAGEWIND_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("SAVAGEWIND_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        if (pad) motion_pad_opened(pad);
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F12 && !e->key.repeat)
            g_record_frames = !g_record_frames;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F8 && !e->key.repeat) {
            g_skip_fs_off = !g_skip_fs_off;
            LOG("[input] F8: SAVAGEWIND_SKIP_FSHASH shaders %s\n", g_skip_fs_off ? "drawn" : "skipped");
        }
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        if (e->type == SDL_EVENT_KEY_DOWN) last_device = DEVICE_KEYBOARD;
        recompute_keyboard();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if ((tilt_held || mouse_held) && fabsf(e->motion.xrel) + fabsf(e->motion.yrel) > 4) last_device = DEVICE_KEYBOARD;
        if (tilt_held) {
            motion_mouse(e->motion.xrel, e->motion.yrel);
        } else if (mouse_held) {
            mouse_dx_milli += (int64_t)lround(e->motion.xrel * 1000);
            mouse_dy_milli += (int64_t)lround(e->motion.yrel * 1000);
            cam_dx_milli += (int64_t)lround(e->motion.xrel * 1000);
            cam_dy_milli += (int64_t)lround(e->motion.yrel * 1000);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button < sizeof mouse_down && mouse_held) {
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) last_device = DEVICE_KEYBOARD;
            mouse_down[e->button.button] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            recompute_keyboard();
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: case SDL_EVENT_WINDOW_FOCUS_GAINED:
        win_focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) {
            pad = SDL_OpenGamepad(e->gdevice.which);
            LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
            note_family();
            if (pad) motion_pad_opened(pad);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = NULL;
            note_family();
            motion_pad_closed();
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (pad && !host_test && SDL_GetGamepadID(pad) == e->gsensor.which) motion_sensor_event(e);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (pad && !host_test && (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                                  (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 16000)))
            last_device = DEVICE_PAD;
        recompute_pad();
        break;
    }
}

// Scripted input for testing: SAVAGEWIND_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up; motion: TILT_UP/DOWN/LEFT/RIGHT hold the controller tilted 30 degrees that way,
// TURN_LEFT/RIGHT turn it 90 degrees per second, TURN_UP/DOWN 60).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// SAVAGEWIND_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in savagewind.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
//   "frame:mouse:<dx>/<dy>:frames" moves the mouse by that many counts each frame (the mouse is then taken as held
//   in the window throughout: the mouse camera).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// SAVAGEWIND_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
char g_window_shot_request[256];  // the presenter saves the next frame (game + menus) here, then clears it
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) {
        init = 1;
        hspec = getenv("SAVAGEWIND_HOST_INPUT");
        bspec = getenv("SAVAGEWIND_BIND_TEST");
        host_test = hspec;
        test_mouse = hspec && strstr(hspec, ":mouse:");
    }
    char buf[2048];
    static uint64_t mouse_done;  // ("mouse:" moved it for the frames up to this one)
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (!strcmp(dev, "mouse")) {  // once per frame of the script's clock (SAVAGEWIND_SCRIPT_CLOCK: one may
                double mx = 0, my = 0;    // last two presents, or be skipped)
                sscanf(name, "%lf/%lf", &mx, &my);
                uint64_t lo = mouse_done + 1 > start ? mouse_done + 1 : start, hi = frame + 1 < start + len ? frame + 1 : start + len;
                double n = hi > lo ? (double)(hi - lo) : 0;
                mouse_dx_milli += llround(n * mx * 1000), mouse_dy_milli += llround(n * my * 1000);
                cam_dx_milli += llround(n * mx * 1000), cam_dy_milli += llround(n * my * 1000);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
        if (frame > mouse_done) mouse_done = frame;
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip a skippable movie)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    if (g_pad_obj && getenv("SAVAGEWIND_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("SAVAGEWIND_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    float tilt_p = 0, tilt_r = 0, turn_p = 0, turn_y = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strcmp(name, "TILT_UP")) tilt_p = 30;
        else if (!strcmp(name, "TILT_DOWN")) tilt_p = -30;
        else if (!strcmp(name, "TILT_LEFT")) tilt_r = -30;
        else if (!strcmp(name, "TILT_RIGHT")) tilt_r = 30;
        else if (!strcmp(name, "TURN_UP")) turn_p = 60;
        else if (!strcmp(name, "TURN_DOWN")) turn_p = -60;
        else if (!strcmp(name, "TURN_LEFT")) turn_y = 90;
        else if (!strcmp(name, "TURN_RIGHT")) turn_y = -90;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
    motion_script(tilt_p, tilt_r, turn_p, turn_y);
}

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("SAVAGEWIND_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("SAVAGEWIND_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t b = menu ? script_buttons : key_buttons | pad_buttons | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t mx = 0, my = 0;
    if (!menu) mouse_stick(&mx, &my);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + my) + script_ry);
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("SAVAGEWIND_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// int GetNpadStates(State *out, int count, const NpadIdType &id): the newest `count` samples, newest first (BOTW
// 1.8.2 reads its controllers only this way). Every sample is the current state, with sampling numbers counting
// back from the newest.
static void fill_states(Cpu *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint64_t id_ptr = c->x[2];
    if (!out || count <= 0) { c->x[0] = 0; return; }
    uint64_t saved = c->x[1];
    c->x[1] = id_ptr;  // (fill_state's arguments: state, &id)
    fill_state(c);
    c->x[1] = saved;
    int64_t newest = *(int64_t *)out;
    for (int i = 1; i < count; i++) {
        memcpy(out + 0x28 * i, out, 0x28);
        *(int64_t *)(out + 0x28 * i) = newest - i;
    }
    c->x[0] = (uint64_t)count;
}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_fullkey) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_dual) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_left) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_right) { fill_states(c); }

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration devices: accepted but inert (the SDK's vibration node graph stays recompiled).
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}
