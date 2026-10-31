// Focus changes with a real SDL virtual gamepad, without game files or a renderer.
// Linux: clang -O1 -ffunction-sections -fdata-sections -Iruntime $(pkg-config --cflags sdl3) \
// tools/test_input_focus.c -Wl,--gc-sections $(pkg-config --libs sdl3) -lm -o /tmp/test_input_focus
#include <assert.h>
#include <stdio.h>
#include "../runtime/hle.h"
// PE/COFF retains `used` registration entries. This harness calls the HID reader directly.
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/input.c"

Settings g_settings;
int16_t g_bind_key[BIND_KEY_COUNT], g_bind_pad[BIND_PAD_COUNT];
uint64_t g_watch_lo, g_watch_hi;
static bool captured;
static int sensor_events;
static bool vibrating;

bool menu_input_captured(void) { return captured; }
bool menu_cursor_free(void) { return false; }
void motion_tilt_held(bool held) { (void)held; }
void motion_mouse(float x, float y) { (void)x; (void)y; }
void motion_pad_opened(SDL_Gamepad *p) { (void)p; }
void motion_pad_closed(void) {}
void motion_sensor_event(const SDL_Event *e) { (void)e; sensor_events++; }
void vibration_update(SDL_Gamepad *p, bool focused) { (void)p; vibrating = focused; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t plat_now_ns(void) { return 0; }

// PE linkers also keep exported helpers from input.c. These paths are outside this harness.
static _Noreturn void unexpected_helper(void) { fputs("unexpected runtime helper\n", stderr); abort(); }
Cpu *kernel_current_cpu(void) { unexpected_helper(); }
void watch_arm(void) { unexpected_helper(); }
int bind_pad_from_name(const char *name) { (void)name; unexpected_helper(); }
int bind_slot_from_name(const char *name) { (void)name; unexpected_helper(); }
bool bind_capture_event(const SDL_Event *e) { (void)e; unexpected_helper(); }
void bind_capture_begin(int dev, int slot) { (void)dev; (void)slot; unexpected_helper(); }
void bind_reset(int dev) { (void)dev; unexpected_helper(); }
void motion_script(float p, float r, float ps, float ys) { (void)p; (void)r; (void)ps; (void)ys; unexpected_helper(); }

static void focus(bool on) {
    SDL_Event e = {0};
    e.type = on ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(!input_user_blocked(&e));
    input_handle_event(&e);
}

static void pump(void) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (!input_user_blocked(&e)) input_handle_event(&e);
}

static void sample(uint64_t buttons, bool tilted) {
    uint64_t state[5] = {0};
    uint32_t id = 0;
    Cpu c = {0};
    c.x[0] = (uintptr_t)state;
    c.x[1] = (uintptr_t)&id;
    fill_state(&c);
    uint64_t directions = tilted ? (1ull << 18) | (1ull << 17) | (1ull << 20) | (1ull << 23) : 0;
    if (state[1] != (buttons | directions))
        fprintf(stderr, "buttons: expected %#llx, got %#llx\n",
                (unsigned long long)(buttons | directions), (unsigned long long)state[1]);
    assert(state[1] == (buttons | directions));
    assert(input_buttons_now() == buttons);
    int32_t *sticks = (int32_t *)((uint8_t *)state + 16);
    if (tilted) assert(sticks[0] > 16000 && sticks[1] > 16000 && sticks[2] < -16000 && sticks[3] < -16000);
    else assert(!sticks[0] && !sticks[1] && !sticks[2] && !sticks[3]);
    assert(*(uint32_t *)((uint8_t *)state + 32) == 1);  // still connected while unfocused
}

static void controls(SDL_Joystick *joy, bool held) {
    assert(SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, held));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, held ? 32767 : -32768));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, held ? 32767 : -32768));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, held ? 25000 : 0));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, held ? -25000 : 0));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, held ? -25000 : 0));
    assert(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTY, held ? 25000 : 0));
    pump();
}

int main(void) {
    // Force SDL to deliver background events: the runtime must enforce its own focus policy.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc = {0};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "Focus test";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id);
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    assert(joy);
    pad = SDL_OpenGamepad(id);  // use the virtual pad even when a physical controller is connected
    assert(pad);
    for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = -1;
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_EAST;
    g_bind_pad[BIND_ZL] = PADB_LTRIGGER;
    g_bind_pad[BIND_ZR] = PADB_RTRIGGER;
    g_settings.stick_deadzone[0] = g_settings.stick_deadzone[1] = 0.1f;
    const uint64_t held = (1ull << BTN_A) | (1ull << BTN_ZL) | (1ull << BTN_ZR);
    pump();
    assert(pad);
    focus(true);
    controls(joy, true);
    sample(held, true);

    // Losing focus must release cached input even if no further controller event arrives.
    focus(false);
    sample(0, false);
    assert(!vibrating);
    controls(joy, false);
    last_device = DEVICE_KEYBOARD;
    controls(joy, true);
    sample(0, false);
    assert(input_last_device() == DEVICE_KEYBOARD);
    SDL_Event sensor = {0};
    sensor.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
    sensor.gsensor.which = id;
    if (!input_user_blocked(&sensor)) input_handle_event(&sensor);
    assert(sensor_events == 0);

    // Regaining focus samples the current controls, without needing a new button/axis event.
    focus(true);
    sample(held, true);
    input_handle_event(&sensor);
    assert(sensor_events == 1);
    controls(joy, false);
    sample(0, false);
    captured = true;
    controls(joy, true);
    sample(0, false);
    focus(false);
    captured = false;
    sample(0, false);

    // Scripted input remains usable for unattended tests while the window is unfocused.
    script_buttons = 1ull << BTN_B;
    sample(1ull << BTN_B, false);
    script_buttons = 0;
    host_test = true;
    test_pad = 1u << SDL_GAMEPAD_BUTTON_EAST;
    recompute_pad();
    sample(1ull << BTN_A, false);
    test_pad = 0;
    recompute_pad();
    sample(0, false);
    host_test = false;
    SDL_Event synthetic = {0};
    synthetic.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    synthetic.gbutton.which = TEST_DEVICE;
    assert(!input_user_blocked(&synthetic));

    // An initially unfocused window need not receive a FOCUS_LOST event.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    assert(SDL_InitSubSystem(SDL_INIT_VIDEO));
    SDL_Window *hidden = SDL_CreateWindow("Focus test", 64, 64, SDL_WINDOW_HIDDEN);
    assert(hidden);
    focus(true);
    sample(held, true);
    input_focus_update(hidden);
    sample(0, false);
    SDL_DestroyWindow(hidden);

    assert(SDL_DetachVirtualJoystick(id));
    pump();
    sample(0, false);
    SDL_CloseJoystick(joy);
    SDL_Quit();
    puts("input focus checks passed");
}
