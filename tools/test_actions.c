// Per-action bindings (runtime/hle/bindings.c, docs/ACTIONS.md): which host input drives each of the game's actions,
// the answers to the game's hold / pressed queries, the menus falling back to the Switch layer, two actions on one key,
// the capture and savagewind.ini. No window, controller or game files: a made-up mask table stands in for the game's.
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_sites.h"
#include "input_bind.h"

GameSites g_site;  // (no game: the HUD prompt check finds no screens)

static uint64_t now;
Uint64 SDL_GetTicks(void) { return now; }
Uint64 SDL_GetTicksNS(void) { return now * 1000000ull; }
int input_pad_family(void) { return 0; }
bool settings_steam_deck(void) { return false; }

// Actions 2 / 3 / 5 / 9 read X / Y / A / B (the pad mask bits are sead's), the rest A
static uint32_t masks[ACT_COUNT];
static int key_down[BIND_MOUSE_LAST + 1];
static int held(int sc) { return sc > 0 && sc <= BIND_MOUSE_LAST && key_down[sc]; }

static const int JUMP = 2, ATTACK = 3, INTERACT = 5, CANCEL = 9;

static void host(uint64_t key_buttons, uint64_t pad_buttons, uint32_t pad_host) {
    act_publish(act_scan_keys(key_buttons, held), act_scan_pad(pad_buttons, pad_host));
}
static int hold(int id) { return rt_act_query(0, 0, (uint64_t)id, 0x1234, (uint64_t)(uintptr_t)masks); }
static int trig(int id) { return rt_act_query(1, 0, (uint64_t)id, 0x1234, (uint64_t)(uintptr_t)masks); }

static void defaults_are_untouched(void) {
    act_masks(NULL);
    for (int id = 0; id < ACT_COUNT; id++) {
        int b = act_switch_button(id);
        assert(b >= 0 && act_is_default(id));
        assert(g_act_key[id] == g_bind_key[b] && g_act_pad[id] == g_bind_pad[b]);
        key_down[g_act_key[id]] = 1;
        assert(!(act_scan_keys(0, held) >> id & 1));
        key_down[g_act_key[id]] = 0;
        assert(!(act_scan_pad(0, 1u << g_act_pad[id]) >> id & 1));
    }
    act_masks(masks);
    host(1ull << BIND_X, 1ull << BIND_X, 1u << SDL_GAMEPAD_BUTTON_NORTH);
    act_poll(true);
    for (int id = 0; id < ACT_COUNT; id++) assert(hold(id) == -1 && trig(id) == -1);
    host(0, 0, 0);
    assert(rt_act_query(0, 0, 41, 0, 0) == -1 && rt_act_query(1, 0, 4000000000ull, 0, 0) == -1);
}

static void a_bound_action_follows_its_key(void) {
    act_set(BIND_DEV_KEY, JUMP, SDL_SCANCODE_G);
    assert(!act_is_default(JUMP) && act_is_default(ATTACK));
    act_poll(true);
    assert(hold(JUMP) == 0 && trig(JUMP) == 0);
    assert(hold(ATTACK) == -1);  // factory-default actions leave the game in control
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

// ...but not kept for long: while a message is up the game asks about no player action, and the press that closed it
// must not jump once the player is asked again
static void a_press_nobody_asked_about_expires(void) {
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    for (int i = 0; i < 10; i++) now += 50, act_poll(true);  // 500 ms of polls nobody read
    assert(trig(JUMP) == 0);
}

// A conversation answers like the HUD prompt: with Jump on Interact's button, the press that picks an answer doesn't
// jump. Lines only spoken over play (the camera still turns) leave the button to Jump.
static bool dialogue_up, cam_live;
bool framepace_dialogue_ui(void) { return dialogue_up; }
bool input_cam_live(void) { return cam_live; }
static void a_conversation_answer_does_not_jump(void) {
    act_reset();
    int a = g_act_pad[0];  // (Interact's controller button)
    act_set(BIND_DEV_PAD, JUMP, a);
    dialogue_up = true;
    host(0, 0, 0), act_poll(true);
    host(0, 0, 1u << a), now += 16, act_poll(true);
    assert(hold(JUMP) == 0 && trig(JUMP) == 0);
    host(0, 0, 0), now += 16, act_poll(true);
    dialogue_up = false, now += 300, act_poll(true);  // closed, and the prompt's moment is over
    host(0, 0, 1u << a), now += 16, act_poll(true);
    assert(trig(JUMP) == 1);
    host(0, 0, 0), now += 16, act_poll(true);
    dialogue_up = cam_live = true, act_poll(true);
    host(0, 0, 1u << a), now += 16, act_poll(true);
    assert(trig(JUMP) == 1);
    dialogue_up = cam_live = false;
    host(0, 0, 0), act_reset(), act_poll(true);
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

static void the_default_side_keeps_its_factory_binding(void) {
    // Jump has a key of its own; its controller side is left at the default, which is the Switch button X
    assert(g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_NORTH);
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_NORTH);
    act_poll(true);
    assert(hold(JUMP) == 1);
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_WEST);  // Attack's default
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
    assert(!act_is_default(JUMP));  // the changed key still keeps both host sides active
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_NORTH);
    act_poll(true);
    assert(hold(JUMP) == 1);
    act_set(BIND_DEV_KEY, JUMP, ACT_FOLLOW);
    assert(act_is_default(JUMP));
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == -1 && trig(JUMP) == -1);
}

static void a_pad_only_override_uses_both_host_sides(void) {
    act_set(BIND_DEV_PAD, JUMP, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    host(1ull << BIND_X, 1ull << BIND_X, 0);
    act_poll(true);
    assert(hold(JUMP) == 0);  // custom on one device means raw Switch state is ignored on both
    key_down[SDL_SCANCODE_SPACE] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 1 && trig(JUMP) == 1);  // unchanged keyboard side uses its factory host key
    key_down[SDL_SCANCODE_SPACE] = 0;
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    act_poll(true);
    assert(hold(JUMP) == 1);
    bind_reset_slot(BIND_ACTION + JUMP);
    assert(hold(JUMP) == -1 && trig(JUMP) == -1);
    act_set(BIND_DEV_PAD, JUMP, ACT_NONE);
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 0);
    act_reset();
    act_poll(true);
    assert(hold(JUMP) == -1 && trig(JUMP) == -1);
    assert(bind_settings_load("act_key_2", "G"));
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 1 && trig(JUMP) == 1);
    key_down[SDL_SCANCODE_G] = 0;
    act_reset();
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == -1 && trig(JUMP) == -1);
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
    assert(bind_capture_event(&e) && g_act_key[12] == SDL_SCANCODE_G && g_act_pad[12] == SDL_GAMEPAD_BUTTON_SOUTH);
    bind_capture_begin(BIND_DEV_ANY, BIND_ACTION + 12);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    assert(bind_capture_event(&e) && g_act_pad[12] == SDL_GAMEPAD_BUTTON_EAST && g_act_key[12] == SDL_SCANCODE_G);
    bind_reset_slot(BIND_ACTION + 12);  // one row back to its default, both sides
    assert(g_act_key[12] == SDL_SCANCODE_LSHIFT && g_act_pad[12] == SDL_GAMEPAD_BUTTON_SOUTH);
    // a keyboard-only stick direction ignores a controller button but takes a key
    bind_capture_begin(BIND_DEV_ANY, BIND_LS_UP);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    int16_t walk = g_bind_key[BIND_LS_UP];
    assert(bind_capture_event(&e) && g_bind_key[BIND_LS_UP] == walk);
    e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_H;
    assert(bind_capture_event(&e) && g_bind_key[BIND_LS_UP] == SDL_SCANCODE_H);
    bind_reset_slot(BIND_LS_UP);
    assert(g_bind_key[BIND_LS_UP] == walk);
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
    assert(g_act_key[1] == SDL_SCANCODE_LSHIFT && g_act_pad[1] == SDL_GAMEPAD_BUTTON_SOUTH);
    act_reset();
    for (int id = 0; id < ACT_COUNT; id++) assert(act_is_default(id));
}

static char *save_bindings(void) {
    FILE *f = tmpfile();
    assert(f);
    bind_settings_save(f);
    long len = ftell(f);
    assert(len >= 0);
    char *buf = malloc((size_t)len + 1);
    assert(buf);
    rewind(f);
    assert(fread(buf, 1, (size_t)len, f) == (size_t)len);
    buf[len] = 0;
    fclose(f);
    return buf;
}

static void load_bindings(char *buf) {
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char k[64], v[64];
        assert(sscanf(line, "%63s = %63[^\n]", k, v) == 2);
        assert(bind_settings_load(k, v));
    }
}

static void reset_bindings(void) {
    bind_reset(BIND_DEV_KEY);
    bind_reset(BIND_DEV_PAD);
    act_reset();  // fresh ini load: no explicit act_* lines have been seen yet
    assert(bind_settings_load("act_schema", "0"));  // simulate a new process loading a legacy file
}

static void migration_and_independent_defaults(void) {
    reset_bindings();
    char legacy[] = "bind_key_x = H\nbind_pad_x = south\nact_key_11 = G\nact_pad_11 = none\n";
    load_bindings(legacy);
    host(0, 0, 0);
    act_poll(true);
    assert(hold(JUMP) == 0);  // migration refreshed the custom-action gate
    assert(g_bind_key[BIND_X] == SDL_SCANCODE_H && g_bind_pad[BIND_X] == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_act_key[JUMP] == SDL_SCANCODE_H && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_act_key[11] == SDL_SCANCODE_G && g_act_pad[11] == ACT_NONE);
    assert(g_act_key[19] == SDL_SCANCODE_H && g_act_pad[19] == SDL_GAMEPAD_BUTTON_SOUTH);
    char *saved = save_bindings();
    assert(strstr(saved, "act_key_2 = H\n") && strstr(saved, "act_pad_2 = south\n"));
    reset_bindings();
    load_bindings(saved);
    free(saved);
    assert(g_act_key[JUMP] == SDL_SCANCODE_H && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_act_key[11] == SDL_SCANCODE_G && g_act_pad[11] == ACT_NONE);

    // Explicit action lines win in either order, on each device independently (including factory defaults).
    reset_bindings();
    char ordered[] = "act_key_2 = Space\nact_pad_2 = north\nbind_key_x = H\nbind_pad_x = south\n";
    load_bindings(ordered);
    assert(act_is_default(JUMP));
    assert(g_act_key[19] == SDL_SCANCODE_H && g_act_pad[19] == SDL_GAMEPAD_BUTTON_SOUTH);
    // Runtime edits to the raw Switch layer never move actions.
    g_bind_key[BIND_X] = SDL_SCANCODE_J;
    g_bind_pad[BIND_X] = SDL_GAMEPAD_BUTTON_WEST;
    assert(g_act_key[JUMP] == SDL_SCANCODE_SPACE && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_NORTH);
    bind_reset_slot(BIND_X);
    assert(g_act_key[19] == SDL_SCANCODE_H);
    bind_reset_slot(BIND_ACTION + 19);
    assert(act_is_default(19));
    act_set(BIND_DEV_KEY, JUMP, SDL_SCANCODE_G);
    act_set(BIND_DEV_PAD, JUMP, ACT_NONE);
    assert(!bind_is_default_slot(BIND_DEV_KEY, BIND_ACTION + JUMP));
    bind_reset(BIND_DEV_KEY);
    assert(g_act_key[JUMP] == SDL_SCANCODE_SPACE && g_act_pad[JUMP] == ACT_NONE);
    bind_reset(BIND_DEV_PAD);
    assert(act_is_default(JUMP) && bind_is_default(BIND_DEV_PAD) && bind_is_default(BIND_DEV_KEY));
    char *defaults = save_bindings();
    assert(!strcmp(defaults, "act_schema = 2\n"));
    free(defaults);
    reset_bindings();
    assert(bind_settings_load("bind_key_x", "none") && g_act_key[JUMP] == ACT_NONE);
    assert(bind_settings_load("bind_pad_x", "none") && g_act_pad[JUMP] == ACT_NONE);
    reset_bindings();
}

static void schema_migration_is_one_time(void) {
    reset_bindings();
    char legacy[] = "bind_key_x = H\nbind_pad_x = south\n";
    load_bindings(legacy);
    assert(g_act_key[JUMP] == SDL_SCANCODE_H && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_SOUTH);
    bind_reset_slot(BIND_ACTION + JUMP);
    char *saved = save_bindings();
    assert(!strncmp(saved, "act_schema = 2\n", strlen("act_schema = 2\n")));
    assert(!strstr(saved, "act_key_2 =") && !strstr(saved, "act_pad_2 ="));
    assert(strstr(saved, "bind_key_x = H\n") && strstr(saved, "bind_pad_x = south\n"));
    // Fresh reloads keep Jump at its own defaults and preserve all other migrated values byte for byte.
    for (int round = 0; round < 2; round++) {
        char *copy = malloc(strlen(saved) + 1);
        assert(copy);
        strcpy(copy, saved);
        reset_bindings();
        load_bindings(copy);
        free(copy);
        assert(g_act_key[JUMP] == SDL_SCANCODE_SPACE && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_NORTH);
        assert(g_act_key[19] == SDL_SCANCODE_H && g_act_pad[19] == SDL_GAMEPAD_BUTTON_SOUTH);
        host(0, 0, 0);
        act_poll(true);
        assert(hold(JUMP) == -1 && trig(JUMP) == -1 && hold(19) == 0);
        char *again = save_bindings();
        assert(!strcmp(saved, again));
        free(saved);
        saved = again;
    }
    free(saved);
    // Resetting action defaults within a schema-2 session must not re-enable legacy migration.
    act_reset();
    assert(bind_settings_load("bind_key_x", "G") && bind_settings_load("bind_pad_x", "west"));
    assert(g_act_key[JUMP] == SDL_SCANCODE_SPACE && g_act_pad[JUMP] == SDL_GAMEPAD_BUTTON_NORTH);
    reset_bindings();
}

static void capture_pad(int dev, int slot, int button) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = (Uint8)button;
    bind_capture_begin(dev, slot);
    assert(bind_capture_active(NULL, NULL, NULL));
    assert(bind_capture_event(&e));
    assert(!bind_capture_active(NULL, NULL, NULL) && g_bind_pad[slot] == button);
}

static void host_pad_slots(void) {
    reset_bindings();
    for (int slot = BIND_WALK; slot <= BIND_GYRO_CENTER; slot++) {
        assert(bind_pad_slot(slot) && g_bind_pad[slot] == -1);
        assert(bind_is_default_slot(BIND_DEV_PAD, slot));
        capture_pad(BIND_DEV_PAD, slot, SDL_GAMEPAD_BUTTON_MISC2);
        assert(!bind_is_default_slot(BIND_DEV_PAD, slot));
        bind_reset_slot(slot);
        capture_pad(BIND_DEV_ANY, slot, SDL_GAMEPAD_BUTTON_MISC2);
        bind_reset_slot(slot);
        assert(g_bind_pad[slot] == -1 && bind_is_default_slot(BIND_DEV_PAD, slot));
    }
    for (int slot = BIND_LS_UP; slot <= BIND_RS_RIGHT; slot++) assert(!bind_pad_slot(slot));
    // Host actions share inputs without taking raw menu buttons or another action's binding.
    capture_pad(BIND_DEV_ANY, BIND_WALK, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_bind_pad[BIND_B] == SDL_GAMEPAD_BUTTON_SOUTH && g_bind_pad[BIND_LS_UP] == 0);
    assert(g_act_pad[12] == SDL_GAMEPAD_BUTTON_SOUTH);  // independent action, no swap
    capture_pad(BIND_DEV_PAD, BIND_TILT, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_bind_pad[BIND_WALK] == SDL_GAMEPAD_BUTTON_SOUTH);
    capture_pad(BIND_DEV_PAD, BIND_GYRO_UP, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_bind_pad[BIND_TILT] == SDL_GAMEPAD_BUTTON_SOUTH);
    capture_pad(BIND_DEV_ANY, BIND_GYRO_DOWN, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(g_bind_pad[BIND_GYRO_UP] == SDL_GAMEPAD_BUTTON_SOUTH && g_bind_pad[BIND_TILT] == SDL_GAMEPAD_BUTTON_SOUTH);
    // A trigger is also capturable on the new slots.
    SDL_Event e = {0}; e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER; e.gaxis.value = 20000;
    bind_capture_begin(BIND_DEV_ANY, BIND_WALK);
    assert(bind_capture_event(&e) && g_bind_pad[BIND_WALK] == PADB_LTRIGGER && g_bind_pad[BIND_ZL] == PADB_LTRIGGER);

    // Every added ini name round-trips. Unbinding an existing raw pad slot also survives a save.
    static const char *const names[] = {"walk", "tilt", "gyro_up", "gyro_down", "gyro_left", "gyro_right",
                                        "gyro_turn_left", "gyro_turn_right", "gyro_center"};
    for (int i = 0; i < 9; i++) {
        char key[64]; snprintf(key, sizeof key, "bind_pad_%s", names[i]);
        assert(bind_settings_load(key, "misc2"));
        assert(g_bind_pad[BIND_WALK + i] == SDL_GAMEPAD_BUTTON_MISC2);
    }
    assert(bind_settings_load("bind_pad_zl", "none"));
    char *saved = save_bindings();
    assert(strstr(saved, "bind_pad_zl = none\n"));
    for (int i = 0; i < 9; i++) {
        char line[64]; snprintf(line, sizeof line, "bind_pad_%s = misc2\n", names[i]);
        assert(strstr(saved, line));
    }
    reset_bindings();
    load_bindings(saved);
    free(saved);
    for (int i = 0; i < 9; i++) assert(g_bind_pad[BIND_WALK + i] == SDL_GAMEPAD_BUTTON_MISC2);
    assert(g_bind_pad[BIND_ZL] == -1);
    bind_settings_load("bind_pad_walk", "none");
    bind_settings_load("bind_pad_tilt", "bad-button");
    assert(g_bind_pad[BIND_WALK] == -1 && g_bind_pad[BIND_TILT] == SDL_GAMEPAD_BUTTON_MISC2);
    reset_bindings();
}

static void captures_allow_shared_action_inputs(void) {
    const int captures[] = {BIND_DEV_PAD, BIND_DEV_KEY, BIND_DEV_ANY};
    for (unsigned c = 0; c < sizeof captures / sizeof *captures; c++) {
        for (int dev = BIND_DEV_PAD; dev <= BIND_DEV_KEY; dev++) {
            if (captures[c] != BIND_DEV_ANY && captures[c] != dev) continue;
            reset_bindings();
            int16_t *raw = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
            int16_t *actions = dev == BIND_DEV_PAD ? g_act_pad : g_act_key;
            const int host = raw[BIND_B];
            SDL_Event e = {0};
            e.type = dev == BIND_DEV_PAD ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_KEY_DOWN;
            if (dev == BIND_DEV_PAD) e.gbutton.button = (Uint8)host;
            else e.key.scancode = (SDL_Scancode)host;
            // Every host action and game action can share the menu-back input. Only the chosen slot changes.
            for (int n = BIND_WALK; n <= BIND_GYRO_CENTER + ACT_COUNT; n++) {
                int slot = n <= BIND_GYRO_CENTER ? n : BIND_ACTION + n - BIND_GYRO_CENTER - 1;
                int16_t expected_raw[BIND_KEY_COUNT], expected_actions[ACT_COUNT];
                memcpy(expected_raw, raw, sizeof expected_raw);
                memcpy(expected_actions, actions, sizeof expected_actions);
                if (bind_slot_is_action(slot)) expected_actions[slot - BIND_ACTION] = host;
                else expected_raw[slot] = host;
                bind_capture_begin(captures[c], slot);
                assert(bind_capture_event(&e));
                assert(!bind_capture_active(NULL, NULL, NULL));
                assert(!memcmp(raw, expected_raw, sizeof expected_raw));
                assert(!memcmp(actions, expected_actions, sizeof expected_actions));
                int actual_dev, actual_slot, other;
                assert(bind_last_result(&actual_dev, &actual_slot, &other, NULL) == 1);
                assert(actual_dev == dev && actual_slot == slot && other == -1);
            }
            // Direct action edits also leave all existing bindings in place.
            for (int id = 0; id < ACT_COUNT; id++) act_set(dev, id, raw[BIND_A]);
            for (int id = 0; id < ACT_COUNT; id++) assert(actions[id] == raw[BIND_A]);
            assert(raw[BIND_B] == host);
            // Raw Switch slots still swap with each other without displacing any shared host actions.
            int old_a = raw[BIND_A];
            bind_capture_begin(dev, BIND_A);
            assert(bind_capture_event(&e));
            assert(raw[BIND_A] == host && raw[BIND_B] == old_a);
            for (int slot = BIND_WALK; slot <= BIND_GYRO_CENTER; slot++) assert(raw[slot] == host);
            for (int id = 0; id < ACT_COUNT; id++) assert(actions[id] == old_a);
            int other;
            assert(bind_last_result(NULL, NULL, &other, NULL) == 2 && other == BIND_B);
        }
    }
    reset_bindings();
}

static void names_and_groups(void) {
    static const char *const names[] = {"Move forward", "Move back", "Move left", "Move right",
        "Camera up", "Camera down", "Camera left", "Camera right", "Walk", "Tilt (hold)",
        "Gyro tilt up", "Gyro tilt down", "Gyro tilt left", "Gyro tilt right", "Gyro turn left", "Gyro turn right", "Gyro recenter"};
    for (int i = 0; i < 17; i++) assert(!strcmp(bind_input_name(BIND_LS_UP + i), names[i]));
    for (int slot = -1; slot <= BIND_KEY_COUNT; slot++) {
        if (bind_key_gyro(slot)) assert(!strcmp(bind_input_group(slot), "gyro"));
        else assert(!bind_input_group(slot));
    }
    static const char *const menu_names[] = {"Menu: confirm", "Menu: back", "Menu: Hero's Path", "Menu: sort / sensor"};
    for (int i = 0; i < 4; i++) assert(!strcmp(bind_input_name(BIND_A + i), menu_names[i]));
    assert(!strcmp(bind_input_name(BIND_PLUS), "Pause menu") && !strcmp(bind_input_name(BIND_MINUS), "Map (Sheikah Slate)"));
    assert(!strcmp(bind_input_name(BIND_UP), "Quick menu: runes") && !strcmp(bind_input_name(BIND_LEFT), "Quick menu: shields"));
    assert(!strcmp(bind_input_name(BIND_RIGHT), "Quick menu: weapons"));
    assert(!strcmp(bind_input_name(BIND_DOWN), "Magnesis: pull closer"));
    // no listed input is called by its Switch button
    static const char *const raw[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-pad Left", "D-pad Up",
                                      "D-pad Right", "D-pad Down"};
    for (int slot = 0; slot < BIND_PAD_COUNT; slot++)
        for (unsigned i = 0; i < sizeof raw / sizeof *raw; i++) assert(strcmp(bind_input_name(slot), raw[i]));
}

static void swapped_jump_sprint_masks_keep_keyboard_bindings(void) {
    act_reset();
    masks[2] = 0x2;   // BOTW JumpButtonChange: Jump -> B
    masks[12] = 0x8;  // Sprint -> X
    assert(act_is_default(2) && act_is_default(12));
    act_poll(true);
    assert(hold(2) == 0 && hold(12) == 0);
    // Keyboard still follows its configured bindings (Space = Jump, Left Shift = Sprint)
    key_down[SDL_SCANCODE_SPACE] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(2) == 1 && trig(2) == 1 && hold(12) == 0 && trig(12) == 0);
    key_down[SDL_SCANCODE_SPACE] = 0;
    key_down[SDL_SCANCODE_LSHIFT] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(2) == 0 && trig(2) == 0 && hold(12) == 1 && trig(12) == 1);
    key_down[SDL_SCANCODE_LSHIFT] = 0;
    // Uncustomized controller still follows the game's swapped Switch layout (B = Jump, X = Sprint)
    host(0, 0, 1u << g_bind_pad[BIND_B]);
    act_poll(true);
    assert(hold(2) == 1 && trig(2) == 1 && hold(12) == 0 && trig(12) == 0);
    host(0, 0, 1u << g_bind_pad[BIND_X]);
    act_poll(true);
    assert(hold(2) == 0 && trig(2) == 0 && hold(12) == 1 && trig(12) == 1);
    host(0, 0, 0);
    // Restoring the 1.9.0 masks returns both to game fallthrough (-1)
    masks[2] = 0x8;
    masks[12] = 0x2;
    act_poll(true);
    assert(hold(2) == -1 && hold(12) == -1);
}

// "Swap A and B (Xbox: A confirms)" sets that convention: the Switch's A and its actions on the bottom button, B and
// its actions on the right one. A second press keeps it (#934: it swapped back), and A / B actions a swap kept from
// before left on the other of the two follow; other actions stay where they are.
static void swap_ab_sets_the_xbox_convention(void) {
    const int south = SDL_GAMEPAD_BUTTON_SOUTH, east = SDL_GAMEPAD_BUTTON_EAST;
    bind_reset(BIND_DEV_PAD);
    for (int press = 0; press < 3; press++) {
        if (press == 2) act_set(BIND_DEV_PAD, 14, south), act_set(BIND_DEV_PAD, 0, east);  // (Cancel, Interact left behind)
        bind_swap_ab();
        assert(g_bind_pad[BIND_A] == south && g_bind_pad[BIND_B] == east);
        for (int id = 0; id < ACT_COUNT; id++) {
            int sb = act_switch_button(id);
            if (sb == BIND_A) assert(g_act_pad[id] == south);
            else if (sb == BIND_B) assert(g_act_pad[id] == east);
            else assert(g_act_pad[id] != south && g_act_pad[id] != east);
        }
    }
    host(0, 0, 1u << east);  // Cancel (let go, close the paraglider) on the right button, Interact not
    act_poll(true);
    assert(hold(14) == 1 && hold(0) == 0);
    host(0, 0, 0);
    act_poll(true);
    bind_reset(BIND_DEV_PAD);
}

static void guard_rebind_updates_hidden_lock_on(void) {
    // Rebinding action 13 ("Guard / lock on") to R2 (SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER / rtrigger)
    // must also make hidden action 29 (lock-on) follow R2 instead of staying on L2 (#1016).
    act_reset();
    int r2 = SDL_GAMEPAD_BUTTON_EAST;
    int l2 = g_act_pad[13];
    assert(l2 != r2);
    act_set(BIND_DEV_PAD, 13, r2);
    host(0, 0, 1u << r2);
    act_poll(true);
    assert(hold(13) == 1 && hold(29) == 1);
    host(0, 0, 1u << l2);
    act_poll(true);
    assert(hold(13) == 0 && hold(29) == 0);
    host(0, 0, 0);
    act_poll(true);
    act_reset();
}

// Magnesis: pull closer (the raw D-pad Down, #967) moved to G: Call horse and Throw distance, at their factory
// defaults on D-pad Down, keep 4 (they answer from their own bindings now), while the raw input, which the game reads
// for Magnesis and for the hidden actions on D-pad Down, is G. The same for an action on L when Menu: previous tab moves.
static void a_moved_menu_input_leaves_its_actions(void) {
    act_reset();
    bind_reset(BIND_DEV_KEY);
    const int CALL_HORSE = 35, THROW_DISTANCE = 37, HIDDEN_DOWN = 22, AIM_RUNE = 33;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(CALL_HORSE) == -1 && hold(THROW_DISTANCE) == -1 && hold(HIDDEN_DOWN) == -1);
    bind_settings_load("act_schema", "2");  // (a raw line is not a legacy one to migrate into the actions)
    bind_settings_load("bind_key_down", "G");  // (the ini path: refreshes the actions)
    assert(g_bind_key[BIND_DOWN] == SDL_SCANCODE_G && act_is_default(CALL_HORSE) && act_is_default(THROW_DISTANCE));
    key_down[SDL_SCANCODE_4] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(CALL_HORSE) == 1 && trig(CALL_HORSE) == 1 && hold(THROW_DISTANCE) == 1);
    assert(hold(HIDDEN_DOWN) == -1);  // (no row of its own: the game goes on reading the raw input)
    key_down[SDL_SCANCODE_4] = 0;
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(CALL_HORSE) == 0 && hold(THROW_DISTANCE) == 0);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
    // L on the controller: Aim rune keeps the left bumper when Menu: previous tab moves to the right one
    assert(hold(AIM_RUNE) == -1);
    bind_settings_load("bind_pad_l", "rshoulder");
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    act_poll(true);
    assert(hold(AIM_RUNE) == 1);
    host(0, 0, 1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    act_poll(true);
    assert(hold(AIM_RUNE) == 0);
    host(0, 0, 0);
    act_poll(true);
    // ZL on the keyboard (#1103): Menu: previous tab 2 moved to G, Guard / lock on and the hidden lock-on check keep
    // right click, and G does neither
    const int GUARD = 13, HIDDEN_LOCK_ON = 29, RIGHT_CLICK = BIND_MOUSE + SDL_BUTTON_RIGHT;
    assert(g_bind_key[BIND_ZL] == RIGHT_CLICK && act_is_default(GUARD));
    bind_settings_load("bind_key_zl", "G");
    assert(g_bind_key[BIND_ZL] == SDL_SCANCODE_G && act_is_default(GUARD));
    key_down[RIGHT_CLICK] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(GUARD) == 1 && hold(HIDDEN_LOCK_ON) == 1);
    key_down[RIGHT_CLICK] = 0;
    key_down[SDL_SCANCODE_G] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(GUARD) == 0 && hold(HIDDEN_LOCK_ON) == 0);
    key_down[SDL_SCANCODE_G] = 0;
    host(0, 0, 0);
    act_poll(true);
    // back to the defaults: the game's own answers again
    bind_reset(BIND_DEV_KEY);
    bind_reset(BIND_DEV_PAD);
    assert(hold(CALL_HORSE) == -1 && hold(AIM_RUNE) == -1);
    act_reset();
}

// "Reset keyboard to defaults" then rebinding Guard: the reset must not leave the hidden lock-on check (29) a binding
// of its own, or Guard's new key would stop moving it (#1016) and the old one would go on targeting (#1057).
static void a_reset_leaves_the_hidden_lock_on_check_following_guard(void) {
    act_reset();
    bind_reset(BIND_DEV_KEY);
    act_set(BIND_DEV_KEY, 13, SDL_SCANCODE_F);
    host(0, 0, 0);
    act_poll(true);
    key_down[BIND_MOUSE + SDL_BUTTON_RIGHT] = 1;  // its factory-default input, now nobody's
    host(0, 0, 0);
    act_poll(true);
    assert(hold(13) == 0 && hold(29) == 0);
    key_down[BIND_MOUSE + SDL_BUTTON_RIGHT] = 0;
    key_down[SDL_SCANCODE_F] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(13) == 1 && hold(29) == 1);
    key_down[SDL_SCANCODE_F] = 0;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(13) == 0 && hold(29) == 0);
    bind_reset(BIND_DEV_KEY);
    act_reset();
}

// The player's report (#1057): Bow on the right mouse button and Guard / lock on on F, the other way round from the
// factory defaults. The right mouse button must aim the bow only, and the hidden lock-on check (29) must follow
// Guard's F, so pressing it must not target as well.
static void bow_and_guard_swapped_on_mouse_and_key_leaves_lock_on_on_the_guard_key(void) {
    act_reset();
    host(0, 0, 0);
    act_poll(true);
    assert(g_act_key[18] == SDL_SCANCODE_F && g_act_key[13] == BIND_MOUSE + SDL_BUTTON_RIGHT);  // factory defaults
    // The swap the Controls page makes: Bow takes the guard's right mouse button, Guard takes the bow's F.
    act_set(BIND_DEV_KEY, 18, BIND_MOUSE + SDL_BUTTON_RIGHT);
    act_set(BIND_DEV_KEY, 13, SDL_SCANCODE_F);
    host(0, 0, 0);
    act_poll(true);
    assert(hold(13) == 0 && hold(18) == 0 && hold(29) == 0);
    key_down[BIND_MOUSE + SDL_BUTTON_RIGHT] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(18) == 1 && trig(18) == 1);   // the bow aims
    assert(hold(13) == 0 && hold(29) == 0);   // ...and nothing locks on
    key_down[BIND_MOUSE + SDL_BUTTON_RIGHT] = 0;
    key_down[SDL_SCANCODE_F] = 1;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(18) == 0);
    assert(hold(13) == 1 && hold(29) == 1);  // Guard, and the hidden lock-on check with it
    key_down[SDL_SCANCODE_F] = 0;
    host(0, 0, 0);
    act_poll(true);
    assert(hold(13) == 0 && hold(29) == 0);
    act_reset();
}

// #1103: Menu: previous tab 2 (ZL) on another key leaves Guard / lock on on its own in play, the game's raw ZL too
static void a_moved_menu_tab_leaves_guard_in_play(void) {
    act_reset();
    bind_reset(BIND_DEV_KEY);
    const int GUARD = 13, BOW = 18;
    uint64_t zl = 1ull << BIND_ZL, zr = 1ull << BIND_ZR;
    int guard_key = g_act_key[GUARD];
    key_down[guard_key] = 1;  // factory defaults: the raw input as it comes
    assert(act_gameplay_buttons(zl, held, 0) == zl);
    key_down[guard_key] = 0;
    bind_settings_load("act_schema", "2");
    bind_settings_load("bind_key_zl", "G");
    assert(g_bind_key[BIND_ZL] == SDL_SCANCODE_G && g_act_key[GUARD] == guard_key);
    // the menu's key gives the raw ZL (key_buttons): in play it no longer reaches the game
    key_down[SDL_SCANCODE_G] = 1;
    assert(!(act_gameplay_buttons(zl, held, 0) & zl));
    key_down[SDL_SCANCODE_G] = 0;
    key_down[guard_key] = 1;
    assert(act_gameplay_buttons(0, held, 0) & zl);
    key_down[guard_key] = 0;
    // Bow on another key: ZR in play from it, not from Menu: next tab 2's
    act_set(BIND_DEV_KEY, BOW, SDL_SCANCODE_H);
    key_down[SDL_SCANCODE_H] = 1;
    assert(act_gameplay_buttons(0, held, 0) & zr);
    key_down[SDL_SCANCODE_H] = 0;
    assert(!(act_gameplay_buttons(zr, held, 0) & zr));
    act_reset();
    bind_reset(BIND_DEV_KEY);
}

int main(void) {
    static const uint32_t k_masks_190[ACT_COUNT] = {
        0x1, 0x2, 0x8, 0x10, 0x8, 0x1, 0x8, 0x2, 0x8, 0x2, 0x8, 0x8, 0x2, 0x4, 0x2, 0x40, 0x1, 0x80, 0x20, 0x8, 0x2,
        0x10000, 0x20000, 0x10000, 0x20000, 0x2, 0x1, 0x2000, 0x4000, 0x4, 0x2, 0x4000, 0x4000, 0x2000, 0x1, 0x20000,
        0x10, 0x20000, 0x1, 0x2, 0x2000};
    memcpy(masks, k_masks_190, sizeof masks);
    act_masks(masks);
    defaults_are_untouched();
    the_switch_button_comes_from_the_game_table();
    swapped_jump_sprint_masks_keep_keyboard_bindings();
    a_bound_action_follows_its_key();
    a_press_is_not_lost_when_nobody_asked();
    a_press_nobody_asked_about_expires();
    menus_use_the_switch_layer();
    two_actions_share_a_key();
    the_default_side_keeps_its_factory_binding();
    a_pad_only_override_uses_both_host_sides();
    capture_and_settings();
    migration_and_independent_defaults();
    schema_migration_is_one_time();
    captures_allow_shared_action_inputs();
    host_pad_slots();
    swap_ab_sets_the_xbox_convention();
    names_and_groups();
    a_conversation_answer_does_not_jump();
    guard_rebind_updates_hidden_lock_on();
    a_reset_leaves_the_hidden_lock_on_check_following_guard();
    bow_and_guard_swapped_on_mouse_and_key_leaves_lock_on_on_the_guard_key();
    a_moved_menu_input_leaves_its_actions();
    a_moved_menu_tab_leaves_guard_in_play();
    puts("action bindings: default fallthrough, custom host bindings, schema migration, held / pressed, menus, shared keys, pad capture, contexts, A / B swap, reset and ini passed");
    return 0;
}
