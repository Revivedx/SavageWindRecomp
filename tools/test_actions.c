// Per-action bindings (runtime/hle/bindings.c, docs/ACTIONS.md): which host input drives each of the game's actions,
// the answers to the game's hold / pressed queries, the menus falling back to the Switch layer, two actions on one key,
// the capture and savagewind.ini. No window, controller or game files: a made-up mask table stands in for the game's.
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_bind.h"

static uint64_t now;
Uint64 SDL_GetTicks(void) { return now; }
int input_pad_family(void) { return 0; }

// Actions 2 / 3 / 5 / 9 read X / Y / A / B (the pad mask bits are sead's), the rest A
static uint32_t masks[ACT_COUNT];
static int key_down[SDL_SCANCODE_COUNT];
static int held(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && key_down[sc]; }

static const int JUMP = 2, ATTACK = 3, INTERACT = 5, CANCEL = 9;

static void host(uint64_t key_buttons, uint64_t pad_buttons, uint32_t pad_host) {
    act_publish(act_scan_keys(key_buttons, held), act_scan_pad(pad_buttons, pad_host));
}
static int hold(int id) { return rt_act_query(0, 0, (uint64_t)id, 0x1234, (uint64_t)(uintptr_t)masks); }
static int trig(int id) { return rt_act_query(1, 0, (uint64_t)id, 0x1234, (uint64_t)(uintptr_t)masks); }

static void defaults_are_untouched(void) {
    for (int id = 0; id < ACT_COUNT; id++) assert(act_is_default(id));
    act_poll(true);
    for (int id = 0; id < ACT_COUNT; id++) assert(hold(id) == -1 && trig(id) == -1);  // the game answers
    assert(rt_act_query(0, 0, 41, 0, 0) == -1 && rt_act_query(1, 0, 4000000000ull, 0, 0) == -1);
}

static void a_bound_action_follows_its_key(void) {
    act_set(BIND_DEV_KEY, JUMP, SDL_SCANCODE_G);
    assert(!act_is_default(JUMP) && act_is_default(ATTACK));
    act_poll(true);
    assert(hold(JUMP) == 0 && trig(JUMP) == 0);
    assert(hold(ATTACK) == -1);  // the other actions stay the game's
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 1 && trig(JUMP) == 1);
    act_poll(true);  // still down; the press was read
    assert(hold(JUMP) == 1 && trig(JUMP) == 0);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 0 && trig(JUMP) == 0);
    // the Switch layer's key for the same button no longer reaches the action: Space is X, but Jump left X
    host(1ull << BIND_X, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 0);
}

static void a_press_is_not_lost_when_nobody_asked(void) {
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);   // pressed here, but the game does not ask before the next poll
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
    assert(trig(JUMP) == 1);  // still delivered once
    act_poll(true);
    assert(trig(JUMP) == 0);
}

static void menus_use_the_switch_layer(void) {
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(false);
    assert(hold(JUMP) == -1 && trig(JUMP) == -1);
    act_poll(true);  // held through the menu closing: held, not newly pressed
    assert(hold(JUMP) == 1 && trig(JUMP) == 0);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
}

static void two_actions_share_a_key(void) {
    act_set(BIND_DEV_KEY, ATTACK, SDL_SCANCODE_G);
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 1 && hold(ATTACK) == 1 && trig(JUMP) == 1 && trig(ATTACK) == 1);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 0 && hold(ATTACK) == 0);
    act_set(BIND_DEV_KEY, ATTACK, ACT_FOLLOW);
    assert(act_is_default(ATTACK));
}

static void the_unbound_side_keeps_the_switch_button(void) {
    // Jump has a key of its own; its controller side is left at the default, which is the Switch button X
    assert(g_act_pad[JUMP] == ACT_FOLLOW);
    host(0, 1ull << BIND_X, 0);
    act_poll(true);
    assert(hold(JUMP) == 1);
    host(0, 1ull << BIND_Y, 0);  // Y is Attack's button
    act_poll(true);
    assert(hold(JUMP) == 0);
    // ... or a controller button of its own
    act_set(BIND_DEV_PAD, JUMP, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    host(0, 1ull << BIND_X, 0);
    act_poll(true);
    assert(hold(JUMP) == 0);  // X no longer jumps
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    act_poll(true);
    assert(hold(JUMP) == 1);
    // ... or none
    act_set(BIND_DEV_PAD, JUMP, ACT_NONE);
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    act_poll(true);
    assert(hold(JUMP) == 0);
    act_set(BIND_DEV_PAD, JUMP, ACT_FOLLOW);
    act_set(BIND_DEV_KEY, JUMP, ACT_FOLLOW);
    assert(act_is_default(JUMP));
    host(0, 0, 0);
}

static void the_switch_button_comes_from_the_game_table(void) {
    assert(act_switch_button(JUMP) == BIND_X && act_switch_button(ATTACK) == BIND_Y);
    assert(act_switch_button(INTERACT) == BIND_A && act_switch_button(CANCEL) == BIND_B);
    assert(act_switch_button(ACT_COUNT) == -1 && act_switch_button(-1) == -1);
}

static void capture_and_settings(void) {
    SDL_Event e = {0};
    act_reset();
    bind_capture_begin(BIND_DEV_KEY, BIND_ACTION + JUMP);
    int dev, slot, ms;
    assert(bind_capture_active(&dev, &slot, &ms) && dev == BIND_DEV_KEY && slot == BIND_ACTION + JUMP);
    e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_E;  // E is the Switch layer's A key: nothing swaps
    assert(bind_capture_event(&e));
    assert(g_act_key[JUMP] == SDL_SCANCODE_E && g_bind_key[BIND_A] == SDL_SCANCODE_E);
    int other, serial;
    assert(bind_last_result(&dev, &slot, &other, &serial) == 1 && slot == BIND_ACTION + JUMP && other == -1);
    assert(!strcmp(act_name(JUMP), "Jump") && !strcmp(bind_input_name(BIND_ACTION + JUMP), "Jump"));
    assert(act_name(ATTACK + 20) == NULL && !strcmp(bind_input_name(BIND_ACTION + 7), "Action"));
    assert(act_hidden(7) && act_hidden(29) && !act_hidden(JUMP) && !act_hidden(13) && !act_hidden(40));
    for (int i = 0; i < ACT_COUNT; i++) assert(act_hidden(i) || act_name(i));  // every action on the page has a name
    // a second action on the same key
    bind_capture_begin(BIND_DEV_KEY, BIND_ACTION + ATTACK);
    assert(bind_capture_event(&e) && g_act_key[ATTACK] == SDL_SCANCODE_E && g_act_key[JUMP] == SDL_SCANCODE_E);
    // a controller button, and a mouse button
    bind_capture_begin(BIND_DEV_PAD, BIND_ACTION + INTERACT);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    assert(bind_capture_event(&e) && g_act_pad[INTERACT] == SDL_GAMEPAD_BUTTON_SOUTH);
    bind_capture_begin(BIND_DEV_KEY, BIND_ACTION + CANCEL);
    e = (SDL_Event){0}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button = SDL_BUTTON_X1;
    assert(bind_capture_event(&e) && g_act_key[CANCEL] == BIND_MOUSE + SDL_BUTTON_X1);
    // Escape cancels and keeps the old one
    bind_capture_begin(BIND_DEV_KEY, BIND_ACTION + CANCEL);
    e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_ESCAPE;
    assert(bind_capture_event(&e) && g_act_key[CANCEL] == BIND_MOUSE + SDL_BUTTON_X1);
    // BIND_DEV_ANY (the in-game Options page): a key binds the keyboard side, a controller button the other side
    bind_capture_begin(BIND_DEV_ANY, BIND_ACTION + 12);
    e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_G;
    assert(bind_capture_event(&e) && g_act_key[12] == SDL_SCANCODE_G && g_act_pad[12] == ACT_FOLLOW);
    bind_capture_begin(BIND_DEV_ANY, BIND_ACTION + 12);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    assert(bind_capture_event(&e) && g_act_pad[12] == SDL_GAMEPAD_BUTTON_EAST && g_act_key[12] == SDL_SCANCODE_G);
    bind_reset_slot(BIND_ACTION + 12);  // one row back to its default, both sides
    assert(g_act_key[12] == ACT_FOLLOW && g_act_pad[12] == ACT_FOLLOW);
    // a keyboard-only input (the walk key) ignores a controller button but takes a key
    bind_capture_begin(BIND_DEV_ANY, BIND_WALK);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    int16_t walk = g_bind_key[BIND_WALK];
    assert(bind_capture_event(&e) && g_bind_key[BIND_WALK] == walk);
    e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_H;
    assert(bind_capture_event(&e) && g_bind_key[BIND_WALK] == SDL_SCANCODE_H);
    bind_reset_slot(BIND_WALK);
    assert(g_bind_key[BIND_WALK] == walk);
    act_set(BIND_DEV_PAD, ATTACK, ACT_NONE);

    // savagewind.ini: only what differs, and back
    FILE *f = tmpfile();  // not open_memstream: that is POSIX only and this also runs on Windows
    bind_settings_save(f);
    long len = ftell(f);
    char *buf = malloc((size_t)len + 1);
    rewind(f);
    assert(fread(buf, 1, (size_t)len, f) == (size_t)len);
    buf[len] = 0;
    fclose(f);
    assert(strstr(buf, "act_key_2 = E\n") && strstr(buf, "act_key_3 = E\n") && strstr(buf, "act_pad_5 = south\n"));
    assert(strstr(buf, "act_key_9 = Mouse Back\n") && strstr(buf, "act_pad_3 = none\n"));
    assert(!strstr(buf, "act_key_0") && !strstr(buf, "act_pad_2"));
    int16_t key[ACT_COUNT], pad[ACT_COUNT];
    memcpy(key, g_act_key, sizeof key);
    memcpy(pad, g_act_pad, sizeof pad);
    act_reset();
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char k[64], v[64];
        if (sscanf(line, "%63s = %63[^\n]", k, v) == 2) bind_settings_load(k, v);
    }
    free(buf);
    assert(!memcmp(key, g_act_key, sizeof key) && !memcmp(pad, g_act_pad, sizeof pad));
    // junk is ignored
    assert(bind_settings_load("act_key_99", "E") && bind_settings_load("act_key_x", "E") &&
           bind_settings_load("act_pad_1", "nonsense"));
    assert(g_act_key[1] == ACT_FOLLOW && g_act_pad[1] == ACT_FOLLOW);
    act_reset();
    for (int id = 0; id < ACT_COUNT; id++) assert(act_is_default(id));
}

int main(void) {
    for (int i = 0; i < ACT_COUNT; i++) masks[i] = 0x1;
    masks[JUMP] = 0x8, masks[ATTACK] = 0x10, masks[INTERACT] = 0x1, masks[CANCEL] = 0x2;
    act_masks(masks);
    defaults_are_untouched();
    the_switch_button_comes_from_the_game_table();
    a_bound_action_follows_its_key();
    a_press_is_not_lost_when_nobody_asked();
    menus_use_the_switch_layer();
    two_actions_share_a_key();
    the_unbound_side_keeps_the_switch_button();
    capture_and_settings();
    puts("action bindings: defaults, held / pressed answers, menus, shared keys, capture and settings passed");
    return 0;
}
