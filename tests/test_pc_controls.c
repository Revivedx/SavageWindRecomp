// Exercise the window-thread cursor policy and ordinary gameplay bindings without opening a window.
// Unrelated guest imports are left out; SDL window operations are replaced below.
#include <assert.h>
#include <stdio.h>
#include "../runtime/hle.h"
// The test calls the host entry points directly; don't root unrelated guest imports in HLE sections.
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/input.c"
#include "../runtime/hle/bindings.c"

Settings g_settings = {.mouse_camera = true, .pc_cursor = true};
static bool pointer_ui, map_pointer, captured, freed, visible = true, relative, rotate_drag;
static bool gyro_active, game_menu, title_menu, motion_tilt, gyro_reset;
static int gyro_pitch, gyro_roll, gyro_yaw;
static float tilt_mouse_x, tilt_mouse_y, tilt_mouse_yaw;
static bool sensor_pad;
static int relative_changes, visibility_changes;
static uint64_t now = 1000000000;

bool pc_cursor_active(void) { return pointer_ui; }
bool pc_cursor_hide_system(void) { return map_pointer; }
bool pc_cursor_map_navigation(void) { return map_pointer; }
bool pc_cursor_rotate_drag(void) { return rotate_drag; }
uint64_t pc_cursor_buttons(uint64_t buttons) { return buttons; }
void pc_cursor_window(SDL_Window *w) { (void)w; }
bool menu_input_captured(void) { return captured; }
bool prof_toggle(void) { return false; }
bool menu_cursor_free(void) { return freed; }
void vibration_update(SDL_Gamepad *p, bool focused) { (void)p; (void)focused; }
void motion_tilt_held(bool held) { motion_tilt = held; }
bool pc_gyro_keyboard_active(void) { return gyro_active; }
void motion_keyboard(int p,int r,int y,bool reset) { gyro_pitch=p;gyro_roll=r;gyro_yaw=y;gyro_reset=reset; }
void motion_poll(void) {}
bool control_enabled(void) { return false; }
bool control_fly_toggle(void) { abort(); }
void control_fly_wheel(float notches) { (void)notches;abort(); }
// (the free camera, hle/freecam.c: off here)
bool freecam_active(void) { return false; }
void freecam_toggle(void) { abort(); }
void freecam_key(int scancode, bool down) { (void)scancode; (void)down; abort(); }
void photo_pad(uint32_t buttons) { (void)buttons; }
void freecam_mouse(float dx, float dy) { (void)dx; (void)dy; abort(); }
void freecam_pad(float lx, float ly, float rx, float ry, float up, float down, bool fast, bool slow) {
    (void)lx; (void)ly; (void)rx; (void)ry; (void)up; (void)down; (void)fast; (void)slow;
}
void control_input_frame(uint64_t frame) { (void)frame; }
void control_input_apply(uint64_t *b,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry) {
    (void)b;(void)lx;(void)ly;(void)rx;(void)ry;
}
uint64_t renderer_present_count(void) { return 0; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w) {
    (void)w;return win_focused?SDL_WINDOW_INPUT_FOCUS:0;
}
uint64_t plat_now_ns(void) { return now; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) {
    (void)w; relative = on; relative_changes++; return true;
}
bool SDL_CursorVisible(void) { return visible; }
bool SDL_ShowCursor(void) { visible = true; visibility_changes++; return true; }
bool SDL_HideCursor(void) { visible = false; visibility_changes++; return true; }
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) { (void)x; (void)y; return 0; }

// Unrelated game/window entry points must never run in this synthetic test.
void pc_cursor_frame(void) { abort(); }
bool framepace_menu_ui(void) { return game_menu; }
bool framepace_title_ui(void) { return title_menu; }
Cpu *kernel_current_cpu(void) { abort(); }
bool pc_cursor_event(const SDL_Event *e) { (void)e; return false; }
void motion_pad_opened(SDL_Gamepad *p) { (void)p; abort(); }
void motion_pad_closed(void) { abort(); }
void motion_sensor_event(const SDL_Event *e) { (void)e; abort(); }
void motion_mouse(float x, float y) { tilt_mouse_x=x;tilt_mouse_y=y; }
void motion_mouse_yaw(float steps) { tilt_mouse_yaw=steps; }
bool motion_has_sensors(void) { return sensor_pad; }
void motion_script(float p, float r, float x, float y) { (void)p; (void)r; (void)x; (void)y; abort(); }
void watch_arm(void) { abort(); }

static void update(void) { input_mouse_update((SDL_Window *)(uintptr_t)1); }
int main(void) {
    unsetenv("SAVAGEWIND_INPUT"); unsetenv("SAVAGEWIND_HOST_INPUT");
    assert(g_bind_key[BIND_RIGHT] == SDL_SCANCODE_1);
    assert(g_bind_key[BIND_LEFT] == SDL_SCANCODE_2);
    assert(g_bind_key[BIND_UP] == SDL_SCANCODE_3);
    assert(g_bind_key[BIND_DOWN] == SDL_SCANCODE_4);
    update(); assert(relative && !visible);
    pointer_ui = true;
    update(); assert(!relative && visible);  // ordinary menus still need the host cursor
    int changes = relative_changes;
    map_pointer = true;
    update(); assert(!relative && !visible && relative_changes == changes);
    // This used to return early because relative mode was already off.
    map_pointer = false;
    update(); assert(visible && relative_changes == changes);
    map_pointer = true; update(); assert(!visible);
    freed = true; update(); assert(visible && !relative);
    freed = false; update(); assert(!visible);
    captured = true; update(); assert(visible);
    captured = false; win_focused = false; update(); assert(visible);
    win_focused = true; update(); assert(!visible);
    pointer_ui = map_pointer = false; update(); assert(relative && !visible);
    g_settings.mouse_camera = false; update(); assert(!relative && visible);
    changes = visibility_changes; update(); assert(visibility_changes == changes);
    puts("PASS: camera/menu/map transitions, absolute map input, F2, focus, overlay and idle cursor visibility");

    // Pointer support must leave normal play bindings alone: F draws the bow,
    // right mouse raises the shield/locks on, and left mouse attacks.
    mouse_held=true;
    SDL_Event e={0};e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_RIGHT;
    input_handle_event(&e);assert(input_buttons_now()&(1ull<<BTN_ZL));
    assert(!(input_buttons_now()&(1ull<<BTN_ZR)));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);
    e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_F;
    input_handle_event(&e);assert(input_buttons_now()&(1ull<<BTN_ZR));
    e.type=SDL_EVENT_KEY_UP;input_handle_event(&e);
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_LEFT;
    input_handle_event(&e);assert(input_buttons_now()&(1ull<<BTN_Y));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);
    puts("PASS: F bow, right-mouse shield/lock-on and left-mouse attack use ordinary bindings");

    // Controller Walk scales both left-stick axes and returns to full speed on release (never cumulative).
    host_test = true;
    g_bind_pad[BIND_WALK] = SDL_GAMEPAD_BUTTON_MISC2;
    test_lx = 32767; test_ly = -32767;
    test_pad = 1u << SDL_GAMEPAD_BUTTON_MISC2;
    recompute_pad(); assert(pad_lx == 13107 && pad_ly == -13107);
    recompute_pad(); assert(pad_lx == 13107 && pad_ly == -13107);
    test_pad = 0; recompute_pad(); assert(pad_lx == 32767 && pad_ly == -32767);
    // Keyboard Walk retains its factory behavior.
    keys_down[SDL_SCANCODE_C] = keys_down[SDL_SCANCODE_W] = 1;
    recompute_keyboard(); assert(key_ly == 13107);
    keys_down[SDL_SCANCODE_C] = 0; recompute_keyboard(); assert(key_ly == 32767);
    input_reset();

    // Tilt combines devices: releasing the key while the controller button is held keeps the mouse clutch held.
    g_bind_pad[BIND_TILT] = SDL_GAMEPAD_BUTTON_MISC3;
    test_pad = 1u << SDL_GAMEPAD_BUTTON_MISC3;
    recompute_pad(); assert(tilt_held && motion_tilt);
    keys_down[SDL_SCANCODE_T] = 1; recompute_keyboard();
    test_pad = 0; recompute_pad(); assert(tilt_held && motion_tilt);
    keys_down[SDL_SCANCODE_T] = 0; recompute_keyboard(); assert(!tilt_held && !motion_tilt);
    test_pad = 1u << SDL_GAMEPAD_BUTTON_MISC3; recompute_pad();
    keys_down[SDL_SCANCODE_T] = 1; recompute_keyboard();
    keys_down[SDL_SCANCODE_T] = 0; recompute_keyboard(); assert(motion_tilt);
    captured = true; recompute_motion(); assert(!motion_tilt);
    captured = false; recompute_motion(); assert(motion_tilt);
    host_test = false; win_focused = false; recompute_pad(); assert(!motion_tilt && host_buttons == 0);
    win_focused = true; host_test = true;
    test_pad = 0; recompute_pad();
    // A controller Tilt hold also routes the mouse into motion and enables sensorless dual-stick tilt.
    test_pad = 1u << SDL_GAMEPAD_BUTTON_MISC3; recompute_pad();
    e = (SDL_Event){0}; e.type = SDL_EVENT_MOUSE_MOTION; e.motion.xrel = 5; e.motion.yrel = -3;
    input_handle_event(&e); assert(tilt_mouse_x == 5 && tilt_mouse_y == -3);
    e = (SDL_Event){0}; e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.y = 2;
    input_handle_event(&e); assert(tilt_mouse_yaw == 2);
    host_test = false; pad = (SDL_Gamepad *)(uintptr_t)1;
    assert(dualstick_on());
    sensor_pad = true; assert(!dualstick_on()); sensor_pad = false;
    pad = NULL; host_test = true; test_pad = 0; recompute_pad();
    assert(!dualstick_on());

    // Each gyro button feeds the same pitch/roll/yaw/reset state as the keyboard, only in the live context.
    g_settings.pc_gyro = true;
    for (int slot = BIND_GYRO_UP; slot <= BIND_GYRO_CENTER; slot++) {
        g_bind_pad[slot] = SDL_GAMEPAD_BUTTON_MISC4;
        test_pad = 1u << SDL_GAMEPAD_BUTTON_MISC4;
        gyro_active = false; recompute_pad();
        assert(!gyro_pitch && !gyro_roll && !gyro_yaw && !gyro_reset);
        gyro_active = true; recompute_keyboard();  // context change without a new pad event
        assert(gyro_pitch == (slot == BIND_GYRO_UP ? -1 : slot == BIND_GYRO_DOWN ? 1 : 0));
        assert(gyro_roll == (slot == BIND_GYRO_LEFT ? -1 : slot == BIND_GYRO_RIGHT ? 1 : 0));
        assert(gyro_yaw == (slot == BIND_GYRO_TURN_LEFT ? -1 : slot == BIND_GYRO_TURN_RIGHT ? 1 : 0));
        assert(gyro_reset == (slot == BIND_GYRO_CENTER));
        game_menu = true; recompute_motion(); assert(!gyro_pitch && !gyro_roll && !gyro_yaw && !gyro_reset);
        game_menu = false; title_menu = true; recompute_motion(); assert(!gyro_reset && !gyro_pitch && !gyro_roll && !gyro_yaw);
        title_menu = false; captured = true; recompute_motion(); assert(!gyro_reset && !gyro_pitch && !gyro_roll && !gyro_yaw);
        captured = false; win_focused = false; recompute_motion(); assert(!gyro_reset && !gyro_pitch && !gyro_roll && !gyro_yaw);
        win_focused = true;
        g_bind_pad[slot] = -1;
    }
    g_bind_pad[BIND_GYRO_UP] = SDL_GAMEPAD_BUTTON_MISC4;
    recompute_pad(); assert(gyro_pitch == -1);
    keys_down[SDL_SCANCODE_I] = 1; recompute_keyboard(); assert(gyro_pitch == -1);  // OR, not doubled
    keys_down[SDL_SCANCODE_I] = 0; recompute_keyboard(); assert(gyro_pitch == -1);
    keys_down[SDL_SCANCODE_K] = 1; recompute_keyboard(); assert(gyro_pitch == 0);  // opposite devices cancel
    keys_down[SDL_SCANCODE_K] = 0; test_pad = 0; recompute_pad(); assert(gyro_pitch == 0);
    puts("PASS: controller Walk/Tilt and all gyro bindings, mixed-device holds, menu/focus/context gates");

    // With the PC pointer in a menu, left (click) and right (back) belong to the pointer, but middle click stays the
    // right stick press: Rotate in the inventory (#346).
    pointer_ui=true;mouse_held=false;  // (the menu state update() gives: no relative mode, mouse free)
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_MIDDLE;
    input_handle_event(&e);assert(input_buttons_now()&(1ull<<BTN_RSTICK));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);assert(!(input_buttons_now()&(1ull<<BTN_RSTICK)));
    // Released after its menu closed (mouse camera off: no relative mode to reset the buttons): never stuck in play.
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;input_handle_event(&e);assert(input_buttons_now()&(1ull<<BTN_RSTICK));
    pointer_ui=false;e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);
    assert(!(input_buttons_now()&(1ull<<BTN_RSTICK)));pointer_ui=true;
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_LEFT;
    input_handle_event(&e);assert(!(input_buttons_now()&(1ull<<BTN_Y)));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_RIGHT;
    input_handle_event(&e);assert(!(input_buttons_now()&(1ull<<BTN_ZL)));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;input_handle_event(&e);
    // A left-button drag in Rotate mode turns Link through the right stick, even with the mouse camera off.
    int32_t sx,sy;g_settings.mouse_sens=g_settings.mouse_sens_y=1;  // (the defaults: unset in this synthetic Settings)
    input_pc_rotate_motion(40,0);mouse_stick(&sx,&sy);assert(sx==0&&sy==0);  // no drag: discarded
    rotate_drag=true;now+=16000000;
    input_pc_rotate_motion(40,0);mouse_stick(&sx,&sy);assert(sx>0&&sy==0);
    now+=16000000;input_pc_rotate_motion(-40,0);mouse_stick(&sx,&sy);assert(sx<0);
    rotate_drag=false;now+=16000000;mouse_stick(&sx,&sy);assert(sx==0&&sy==0);
    puts("PASS: pointer menus keep middle click bound; Rotate-mode drag turns Link through the right stick");
}
