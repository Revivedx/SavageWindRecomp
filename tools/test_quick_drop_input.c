// Real HID fill pipeline and equipped action gate, without game/renderer.
#include <assert.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/input.c"
#include "test_freecam_disabled.h"
#include "../runtime/quick_drop_action.c"

Settings g_settings;
void act_poll(bool live) { (void)live; }
uint64_t g_watch_lo, g_watch_hi;
static uint64_t test_frame = 100, pointer_injection, control_injection;
static bool game_menu = true;
bool menu_input_captured(void) { return false; }
bool menu_cursor_free(void) { return false; }
bool framepace_title_ui(void) { return false; }
bool framepace_menu_ui(void) { return game_menu; }
bool pc_cursor_map_navigation(void) { return false; }
bool pc_cursor_rotate_drag(void) { return false; }
uint64_t pc_cursor_buttons(uint64_t buttons) { return buttons | pointer_injection; }
bool pc_gyro_keyboard_active(void) { return false; }
bool motion_has_sensors(void) { return false; }
uint64_t renderer_present_count(void) { return test_frame; }
uint64_t plat_now_ns(void) { return test_frame * 16666667; }
void control_input_apply(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)lx; (void)ly; (void)rx; (void)ry; *buttons |= control_injection;
}
static uint64_t sample(void) {
    uint64_t state[5]; uint32_t id = 0; Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)state; cpu.x[1] = (uintptr_t)&id;
    fill_state(&cpu);
    assert(*(int32_t *)((uint8_t *)state + 16) == 22000);
    assert(*(int32_t *)((uint8_t *)state + 24) == -22000);
    return state[1];
}
int main(void) {
    uint64_t back = 1ull << BTN_B, right = 1ull << BTN_RIGHT, attack = 1ull << BTN_Y;
    assert(unconsumed_buttons(right | attack) == (right | attack));
    input_consume_buttons(right | back);
    assert(unconsumed_buttons(right | back | attack) == attack);
    assert(unconsumed_buttons(right | back) == 0);
    assert(unconsumed_buttons(right) == 0);
    assert(unconsumed_buttons(back | right) == back);
    assert(unconsumed_buttons(0) == 0);
    assert(unconsumed_buttons(right | back) == (right | back));
    unconsumed_buttons(0);

    script_lx = 22000; script_rx = -22000;
    const uint64_t directions = (1ull << 18) | (1ull << 20);
    script_buttons = attack; key_buttons = 1ull << BTN_MINUS; pad_buttons = 1ull << BTN_ZR;
    pointer_injection = 1ull << BTN_A;
    control_injection = (1ull << BTN_L) | right;
    menu_confirm = true; g_inject_plus = 1;
    const uint64_t actions = attack | (1ull << BTN_MINUS) | (1ull << BTN_ZR) |
        (1ull << BTN_A) | (1ull << BTN_PLUS) | control_injection;
    assert(sample() == (actions | directions)); // baseline exercises all injection stages

    atomic_store(&equipped_drop.active, true);
    g_settings.quick_drop = false; // admitted cleanup/gate survives settings changes
    for (int frame = 0; frame < 5; frame++, test_frame++)
        for (int poll = 0; poll < 7; poll++) assert(sample() == directions);
    for (unsigned bit = 0; bit < 32; bit++) {
        control_injection = 1ull << bit;
        uint64_t expected = directions | (control_injection & UINT64_C(0xff0000));
        assert(sample() == expected);
    }
    // New injections arriving after admission are not in the original raw
    // snapshot. Exercise each late producer while the real gate stays active.
    script_buttons = key_buttons = pad_buttons = pointer_injection = control_injection = 0;
    menu_confirm = false; test_frame += 20; assert(sample() == directions);
    pointer_injection = 1ull << BTN_A; assert(sample() == directions);
    pointer_injection = 0; assert(sample() == directions);
    menu_confirm = true; assert(sample() == directions);
    menu_confirm = false; assert(sample() == directions);
    g_inject_plus = 1; assert(sample() == directions); assert(sample() == directions);
    control_injection = (1ull << BTN_MINUS) | (1ull << BTN_R); assert(sample() == directions);
    script_buttons = attack; key_buttons = 1ull << BTN_MINUS; pad_buttons = 1ull << BTN_ZR;
    pointer_injection = 1ull << BTN_A; menu_confirm = true;
    control_injection = (1ull << BTN_L) | right;
    assert(sample() == directions);
    atomic_store(&equipped_drop.active, false);
    // Inputs observed while busy remain consumed until released, including
    // pointer/Enter/latched Plus/control inputs absent from the raw snapshot.
    assert(sample() == directions);
    script_buttons = key_buttons = pad_buttons = pointer_injection = control_injection = 0;
    menu_confirm = false; test_frame += 20; assert(sample() == directions);
    script_buttons = actions;
    assert(sample() == (actions | directions)); // release allows a genuinely new press
    atomic_store(&equipped_drop.active, true);
    assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_C(0xff0000));
    atomic_store(&equipped_drop.active, false);
    puts("input consumption and equipped HID gate tests passed");
    return 0;
}
