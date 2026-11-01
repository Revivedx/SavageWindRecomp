// A capture starts while the game's confirm input is held. Its release must reach input.c, or confirm is still
// held when the capture ends and the game's Options screen immediately starts another capture (#175).
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>

#include "input_bind.h"

static uint64_t now;
Uint64 SDL_GetTicks(void) { return now; }
int input_pad_family(void) { return 1; }

// Model the held state on the other side of the same event routing as the window loop. No window or controller.
static bool held;
static void release_to_game(SDL_Event e) {
    assert(!bind_capture_event(&e));
    held = false;
}

static void waiting(void) {
    int dev, slot, ms;
    assert(bind_capture_active(&dev, &slot, &ms));
}

static void ended(int expected) {
    int dev, slot, other, serial, ms;
    assert(!bind_capture_active(&dev, &slot, &ms));
    assert(bind_last_result(&dev, &slot, &other, &serial) == expected);
    assert(!held);
}

int main(void) {
    SDL_Event e = {0};
    held = true;
    bind_capture_begin(BIND_DEV_KEY, BIND_RIGHT);
    e.type = SDL_EVENT_KEY_UP; e.key.scancode = SDL_SCANCODE_E;
    release_to_game(e); waiting();  // releasing confirm must not assign a binding
    e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_B;
    assert(bind_capture_event(&e));
    assert(g_bind_key[BIND_RIGHT] == SDL_SCANCODE_B);
    ended(1);

    held = true;
    bind_capture_begin(BIND_DEV_KEY, BIND_RIGHT);
    e = (SDL_Event){0}; e.type = SDL_EVENT_MOUSE_BUTTON_UP; e.button.button = SDL_BUTTON_LEFT;
    release_to_game(e); waiting();
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button = SDL_BUTTON_X1;
    assert(bind_capture_event(&e));
    assert(g_bind_key[BIND_RIGHT] == BIND_MOUSE + SDL_BUTTON_X1);
    ended(1);

    held = true;
    bind_capture_begin(BIND_DEV_PAD, BIND_RIGHT);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_BUTTON_UP; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    release_to_game(e); waiting();
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    assert(bind_capture_event(&e)); ended(2);  // B already uses the south button: swap

    const int devices[] = {BIND_DEV_KEY, BIND_DEV_PAD};
    for (unsigned i = 0; i < sizeof devices / sizeof *devices; i++) {
        int dev = devices[i];
        for (int axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER; axis <= SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; axis++) {
            held = true;
            bind_capture_begin(dev, BIND_A);
            e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
            e.gaxis.axis = axis; e.gaxis.value = 0;
            release_to_game(e); waiting();
            e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_ESCAPE;
            assert(bind_capture_event(&e)); ended(3);
        }
    }

    held = true;
    bind_capture_begin(BIND_DEV_KEY, BIND_A);
    e = (SDL_Event){0}; e.type = SDL_EVENT_KEY_UP; e.key.scancode = SDL_SCANCODE_E;
    release_to_game(e); now += BIND_CAPTURE_MS;
    ended(4);  // idle timeout also leaves confirm released

    // A new binding still needs a press, and sticks must not navigate the remapping UI during capture.
    bind_capture_begin(BIND_DEV_PAD, BIND_A);
    e = (SDL_Event){0}; e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX; e.gaxis.value = 20000;
    assert(bind_capture_event(&e)); waiting();
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    assert(bind_capture_event(&e));
    assert(g_bind_pad[BIND_A] == PADB_LTRIGGER);
    ended(2);  // ZL already uses the left trigger
    // Optional gyro bindings may overlap normal keys without mutating the
    // camera defaults; duplicates within the gyro context still swap.
    bind_reset(BIND_DEV_KEY);
    int camera_up=g_bind_key[BIND_RS_UP];
    bind_capture_begin(BIND_DEV_KEY,BIND_GYRO_DOWN);
    e=(SDL_Event){0};e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_I;
    assert(bind_capture_event(&e));assert(g_bind_key[BIND_GYRO_DOWN]==SDL_SCANCODE_I);
    assert(g_bind_key[BIND_GYRO_UP]==SDL_SCANCODE_K&&g_bind_key[BIND_RS_UP]==camera_up);
    bind_capture_begin(BIND_DEV_KEY,BIND_GYRO_CENTER);
    e.key.scancode=SDL_SCANCODE_H;assert(bind_capture_event(&e));
    assert(g_bind_key[BIND_GYRO_CENTER]==SDL_SCANCODE_H&&g_bind_key[BIND_RS_UP]==camera_up);
    puts("binding capture: confirm releases, assignments, cancellation and timeout passed");
    return 0;
}
