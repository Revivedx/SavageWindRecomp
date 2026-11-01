// Controller-state checks without a window or game files. Unused runtime functions are removed by the linker.
// Linux: clang -O1 -ffunction-sections -fdata-sections -Iruntime -isystem /tmp/sdl3-src/include \
// tools/test_menu_arrows.c -Wl,--gc-sections -lm -lpthread -o /tmp/test_menu_arrows
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../runtime/hle/input.c"

Settings g_settings;
int16_t g_bind_key[BIND_KEY_COUNT];
uint64_t g_watch_lo, g_watch_hi;
static bool captured, title, paused;

bool menu_input_captured(void) { return captured; }
bool framepace_title_ui(void) { return title; }
bool framepace_menu_ui(void) { return paused; }
void motion_tilt_held(bool held) { (void)held; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t plat_now_ns(void) { return 0; }

static void sample_state(int32_t x, int32_t y, int32_t rx, int32_t ry, uint64_t buttons) {
    uint64_t state[5] = {0};
    uint32_t id = 0;
    Cpu c = {0};
    c.x[0] = (uintptr_t)state;
    c.x[1] = (uintptr_t)&id;
    fill_state(&c);
    assert(*(int32_t *)((uint8_t *)state + 16) == x);
    assert(*(int32_t *)((uint8_t *)state + 20) == y);
    // The sticks also produce direction bits; check them along with the bound buttons.
    if (x < -16000) buttons |= 1ull << 16;
    if (y > 16000) buttons |= 1ull << 17;
    if (x > 16000) buttons |= 1ull << 18;
    if (y < -16000) buttons |= 1ull << 19;
    if (rx < -16000) buttons |= 1ull << 20;
    if (ry > 16000) buttons |= 1ull << 21;
    if (rx > 16000) buttons |= 1ull << 22;
    if (ry < -16000) buttons |= 1ull << 23;
    if (state[1] != buttons)
        fprintf(stderr, "buttons: expected %#llx, got %#llx\n", (unsigned long long)buttons, (unsigned long long)state[1]);
    assert(state[1] == buttons);
    assert(*(int32_t *)((uint8_t *)state + 24) == rx);
    assert(*(int32_t *)((uint8_t *)state + 28) == ry);
}

static void sample(int32_t x, int32_t y, uint64_t buttons) { sample_state(x, y, 0, 0, buttons); }

// Run with -fsanitize=thread too: keyboard events and guest polls belong to different threads.
static _Atomic bool poll_ready, poll_start;
static void *poll_enter(void *unused) {
    (void)unused;
    uint64_t state[5] = {0};
    uint32_t id = 0;
    Cpu c = {0};
    c.x[0] = (uintptr_t)state;
    c.x[1] = (uintptr_t)&id;
    poll_ready = true;
    while (!poll_start) {}
    for (int i = 0; i < 50000; i++) {
        fill_state(&c);
        assert((state[1] & ~(1ull << BTN_A)) == 0);
    }
    return NULL;
}

int main(void) {
    g_bind_key[BIND_LS_UP] = SDL_SCANCODE_W;
    g_bind_key[BIND_LS_DOWN] = SDL_SCANCODE_S;
    g_bind_key[BIND_LS_LEFT] = SDL_SCANCODE_A;
    g_bind_key[BIND_LS_RIGHT] = SDL_SCANCODE_D;
    const int arrows[] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN};
    const int32_t xs[] = {-32767, 32767, 0, 0}, ys[] = {0, 0, 32767, -32767};
    for (int i = 0; i < 4; i++) {
        keys_down[arrows[i]] = 1;
        recompute_keyboard();
        sample(0, 0, 0);  // gameplay, including dialogue and quick menus outside the two menu signals
        title = true;
        sample(xs[i], ys[i], 0);
        title = false; paused = true;
        sample(xs[i], ys[i], 0);
        captured = true;
        sample(0, 0, 0);  // overlay, name entry or binding capture owns input
        captured = false; paused = false;
        sample(0, 0, 0);  // closing a menu while holding an arrow must not move the player
        keys_down[arrows[i]] = 0;
        recompute_keyboard();
    }

    paused = true;
    keys_down[SDL_SCANCODE_UP] = keys_down[SDL_SCANCODE_RIGHT] = 1;
    recompute_keyboard();
    sample(32767, 32767, 0);
    keys_down[SDL_SCANCODE_DOWN] = keys_down[SDL_SCANCODE_LEFT] = 1;
    recompute_keyboard();
    sample(0, 0, 0);
    input_reset();
    sample(0, 0, 0);

    // A binding in any slot claims its arrow, including buttons, sticks, walk and tilt.
    for (int a = 0; a < 4; a++) for (int slot = 0; slot < BIND_KEY_COUNT; slot++) {
        int16_t old = g_bind_key[slot];
        g_bind_key[slot] = arrows[a];
        keys_down[arrows[a]] = 1;
        recompute_keyboard();
        int32_t x = slot == BIND_LS_LEFT ? -32767 : slot == BIND_LS_RIGHT ? 32767 : 0;
        int32_t y = slot == BIND_LS_DOWN ? -32767 : slot == BIND_LS_UP ? 32767 : 0;
        uint64_t buttons = slot < BIND_PAD_COUNT ? 1ull << slot : 0;
        int32_t rx = slot == BIND_RS_LEFT ? -32767 : slot == BIND_RS_RIGHT ? 32767 : 0;
        int32_t ry = slot == BIND_RS_DOWN ? -32767 : slot == BIND_RS_UP ? 32767 : 0;
        sample_state(x, y, rx, ry, buttons);
        paused = false;
        sample_state(x, y, rx, ry, buttons);  // the binding also works in gameplay
        paused = true;
        g_bind_key[slot] = old;
        input_reset();
    }

    keys_down[SDL_SCANCODE_W] = 1;
    keys_down[SDL_SCANCODE_UP] = 1;
    recompute_keyboard();
    sample(0, 32767, 0);  // two sources in the same direction stay in range
    keys_down[SDL_SCANCODE_W] = 0;
    keys_down[SDL_SCANCODE_S] = 1;
    recompute_keyboard();
    sample(0, 0, 0);  // opposing keyboard directions cancel
    input_reset();
    test_keys[SDL_SCANCODE_LEFT] = 1;
    recompute_keyboard();
    sample(-32767, 0, 0);  // scripted host keys follow the same route
    test_keys[SDL_SCANCODE_LEFT] = 0;
    recompute_keyboard();
    sample(0, 0, 0);

    pad_lx = 12000; script_ly = -8000;
    sample(12000, -8000, 0);
    paused = false;
    sample(12000, -8000, 0);  // existing controller and scripted input are unchanged
    pad_lx = script_ly = 0;
    g_settings.dpad_move = true;
    paused = true;
    keys_down[SDL_SCANCODE_RIGHT] = 1;
    recompute_keyboard();
    sample(32767, 0, 0);  // the d-pad option must not add a second arrow action
    paused = false;
    sample(0, 0, 0);
    keys_down[SDL_SCANCODE_RIGHT] = 0;
    recompute_keyboard();
    g_settings.dpad_move = false;

    // Test each Enter on its own: holding Return must not hide a broken keypad fallback.
    const int enters[] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER};
    const int modifiers[] = {SDL_SCANCODE_LALT, SDL_SCANCODE_RALT, SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI};
    for (int k = 0; k < 2; k++) {
        keys_down[enters[k]] = 1;
        recompute_keyboard();
        sample(0, 0, 0);  // gameplay
        title = true;
        sample(0, 0, 1ull << BTN_A);
        title = false; paused = true;
        sample(0, 0, 1ull << BTN_A);
        for (int m = 0; m < 4; m++) {
            keys_down[modifiers[m]] = 1;
            recompute_keyboard();
            sample(0, 0, 0);  // fullscreen modifiers must not also confirm
            keys_down[modifiers[m]] = 0;
            recompute_keyboard();
            sample(0, 0, 1ull << BTN_A);
        }
        captured = true;
        sample(0, 0, 0);  // overlay, name entry or binding capture owns input
        captured = false;
        paused = false;
        sample(0, 0, 0);  // closing a menu while holding Enter releases A
        paused = true;
        sample(0, 0, 1ull << BTN_A);  // held behavior matches E when a menu opens again
        input_reset();
        sample(0, 0, 0);
        test_keys[enters[k]] = 1;
        recompute_keyboard();
        sample(0, 0, 1ull << BTN_A);  // scripted keys publish through the same path
        test_keys[enters[k]] = 0;
        recompute_keyboard();
        sample(0, 0, 0);
        paused = false;
    }

    // A binding claims only that Enter; the other Enter must keep its menu fallback.
    for (int k = 0; k < 2; k++) for (int slot = 0; slot < BIND_KEY_COUNT; slot++) {
        int16_t old = g_bind_key[slot];
        g_bind_key[slot] = enters[k];
        keys_down[enters[k]] = 1;
        recompute_keyboard();
        int32_t x = slot == BIND_LS_LEFT ? -32767 : slot == BIND_LS_RIGHT ? 32767 : 0;
        int32_t y = slot == BIND_LS_DOWN ? -32767 : slot == BIND_LS_UP ? 32767 : 0;
        int32_t rx = slot == BIND_RS_LEFT ? -32767 : slot == BIND_RS_RIGHT ? 32767 : 0;
        int32_t ry = slot == BIND_RS_DOWN ? -32767 : slot == BIND_RS_UP ? 32767 : 0;
        uint64_t buttons = slot < BIND_PAD_COUNT ? 1ull << slot : 0;
        paused = true;
        sample_state(x, y, rx, ry, buttons);
        paused = false;
        sample_state(x, y, rx, ry, buttons);  // the binding also works in gameplay
        paused = true;
        keys_down[enters[1 - k]] = 1;
        recompute_keyboard();
        sample_state(x, y, rx, ry, buttons | (1ull << BTN_A));
        keys_down[enters[k]] = 0;
        recompute_keyboard();
        sample(0, 0, 1ull << BTN_A);  // only the unbound Enter is held
        paused = false;
        sample(0, 0, 0);
        g_bind_key[slot] = old;
        keys_down[enters[1 - k]] = 0;
        input_reset();
    }

    // Polls consume published state, not keyboard arrays owned by the event producer.
    paused = true;
    keys_down[SDL_SCANCODE_RETURN] = 1;
    sample(0, 0, 0);
    recompute_keyboard();
    sample(0, 0, 1ull << BTN_A);
    keys_down[SDL_SCANCODE_RETURN] = 0;
    sample(0, 0, 1ull << BTN_A);
    recompute_keyboard();
    sample(0, 0, 0);

    pthread_t poller;
    int err = pthread_create(&poller, NULL, poll_enter, NULL);
    assert(err == 0);
    while (!poll_ready) {}
    poll_start = true;
    for (int i = 0; i < 50000; i++) {
        keys_down[SDL_SCANCODE_RETURN] = i & 1;
        keys_down[SDL_SCANCODE_LALT] = (i >> 1) & 1;
        recompute_keyboard();
    }
    err = pthread_join(poller, NULL);
    assert(err == 0);
    input_reset();
    sample(0, 0, 0);
    paused = false;
    puts("menu arrows and Enter: gating, independent bindings, reset and concurrent publication passed");
    return 0;
}
