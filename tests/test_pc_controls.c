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
static bool pointer_ui, map_pointer, captured, freed, visible = true, relative, rotate_drag, apparatus_ui;
static double gyro_yaw_deg, gyro_pitch_deg;
static bool gyro_cleared;
static int relative_changes, visibility_changes;
static uint64_t now = 1000000000;

bool pc_cursor_active(void) { return pointer_ui; }
bool pc_cursor_hide_system(void) { return map_pointer; }
bool pc_cursor_map_navigation(void) { return map_pointer; }
bool pc_cursor_rotate_drag(void) { return rotate_drag; }
uint64_t pc_cursor_buttons(uint64_t buttons) { return buttons; }
void pc_cursor_window(SDL_Window *w) { (void)w; }
bool menu_input_captured(void) { return captured; }
bool menu_cursor_free(void) { return freed; }
void vibration_update(SDL_Gamepad *p, bool focused) { (void)p; (void)focused; }
void motion_tilt_held(bool held) { (void)held; }
bool pc_gyro_keyboard_active(void) { return apparatus_ui; }
void pc_gyro_scene_tick(void) {}
void motion_keyboard(int p,int r,int y,bool reset) { (void)p;(void)r;(void)y;(void)reset; }
void motion_poll(void) {}
bool motion_has_sensors(void) { return true; }
bool motion_cam_consume(double *yaw, double *pitch) {
    if (gyro_yaw_deg == 0 && gyro_pitch_deg == 0) return false;
    *yaw = gyro_yaw_deg; *pitch = gyro_pitch_deg;
    gyro_yaw_deg = gyro_pitch_deg = 0;
    return true;
}
void motion_cam_clear(void) { gyro_yaw_deg = gyro_pitch_deg = 0; gyro_cleared = true; }
void motion_dualstick(float lx, float ly, float rx, float ry, double dt, bool r) {
    (void)lx; (void)ly; (void)rx; (void)ry; (void)dt; (void)r; abort();
}
bool control_enabled(void) { return false; }
bool control_fly_toggle(void) { abort(); }
void control_fly_wheel(float notches) { (void)notches;abort(); }
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

// Unrelated game/window entry points in this synthetic test.
void pc_cursor_frame(void) {}
bool framepace_menu_ui(void) { return false; }
bool framepace_title_ui(void) { return false; }
Cpu *kernel_current_cpu(void) { abort(); }
bool pc_cursor_event(const SDL_Event *e) { (void)e; return false; }
void motion_pad_opened(SDL_Gamepad *p) { (void)p; abort(); }
void motion_pad_closed(void) { abort(); }
void motion_sensor_event(const SDL_Event *e) { (void)e; abort(); }
void motion_mouse(float x, float y) { (void)x; (void)y; abort(); }
void motion_mouse_yaw(float y) { (void)y; abort(); }
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

    // Controller motion camera modes (Off / Exploration / Hold button / Always) and simultaneous right-stick mixing.
    pointer_ui=false;win_focused=true;g_settings.motion_camera_sens=1.0f;
    float cv[5]={0};
    g_settings.motion_camera=GYRO_CAM_OFF;gyro_yaw_deg=2.0;gyro_pitch_deg=1.0;gyro_cleared=false;
    input_cam_frame(0.5f);assert(gyro_cleared&&!rt_cam_stick((uintptr_t)cv,0));

    g_settings.motion_camera=GYRO_CAM_EXPLORE;gyro_yaw_deg=2.0;gyro_pitch_deg=1.0;
    input_cam_frame(0.5f);assert(rt_cam_stick((uintptr_t)cv,0)==1&&cv[0]>0&&cv[1]>0);
    float py=cv[1];assert(rt_cam_pitch(&py)==1&&fabsf(py-cv[1]*CAM_PITCH_LINEAR)<1e-5f);
    // Exploration mode yields during shrine apparatus puzzles and bow/rune aiming (no double-rotation).
    apparatus_ui=true;gyro_yaw_deg=2.0;input_cam_frame(0.5f);assert(!rt_cam_stick((uintptr_t)cv,0));
    apparatus_ui=false;
    (void)rt_cam_pitch_scale(0x3f800000u,0x3f333333u);  // CameraAiming read
    gyro_yaw_deg=2.0;input_cam_frame(0.5f);assert(!rt_cam_stick((uintptr_t)cv,0));
    // Always mode keeps turning even while CameraAiming is active (when in-game motion aiming is off).
    g_settings.motion_camera=GYRO_CAM_ALWAYS;gyro_yaw_deg=2.0;
    input_cam_frame(0.5f);assert(rt_cam_stick((uintptr_t)cv,0)==1&&cv[0]>0);
    now+=200000000;  // expire aiming window

    // Hold-button mode turns only while BIND_GYRO_CAM is held, and coexists simultaneously with right stick deflection.
    g_settings.motion_camera=GYRO_CAM_HOLD;pad_gyro_cam_held=false;
    gyro_yaw_deg=2.0;input_cam_frame(0.5f);assert(!rt_cam_stick((uintptr_t)cv,0));
    pad_gyro_cam_held=true;gyro_yaw_deg=2.0;gyro_pitch_deg=1.0;
    input_cam_frame(0.5f);assert(rt_cam_stick((uintptr_t)cv,0)==1);
    float gyro_only_x=cv[0];
    pad_rx=16384;pad_ry=16384;gyro_yaw_deg=2.0;gyro_pitch_deg=1.0;
    input_cam_frame(0.5f);assert(rt_cam_stick((uintptr_t)cv,0)==1&&cv[0]>gyro_only_x);
    py=cv[1];assert(rt_cam_pitch(&py)==1&&!cam_only_lin);
    pad_rx=pad_ry=0;pad_gyro_cam_held=false;
    puts("PASS: controller motion camera modes, aiming/shrine suppression, and simultaneous right-stick + gyro mixing");
}
