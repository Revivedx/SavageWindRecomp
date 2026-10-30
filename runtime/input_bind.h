// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in savagewind.ini.
// Menus (the overlay, the start screen) never use these: they keep the fixed default layout, so no binding can lock
// the player out.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Switch inputs, in nn::hid button-bit order (runtime/hle/input.c BTN_*), then the keyboard's stick directions.
enum {
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_LSTICK, BIND_RSTICK, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_LEFT, BIND_UP, BIND_RIGHT, BIND_DOWN,
    BIND_PAD_COUNT,                                      // controller: the 16 buttons (its sticks stay the sticks)
    BIND_LS_UP = BIND_PAD_COUNT, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT,
    BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT,
    BIND_WALK,                                           // keyboard only: held, the left stick keys tilt it part way
    BIND_TILT,                                           // keyboard only: held, the mouse tilts the controller (motion)
    BIND_GYRO_UP, BIND_GYRO_DOWN, BIND_GYRO_LEFT, BIND_GYRO_RIGHT,
    BIND_GYRO_TURN_LEFT, BIND_GYRO_TURN_RIGHT, BIND_GYRO_CENTER,
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks + walk + tilt
};
enum { BIND_DEV_PAD, BIND_DEV_KEY };
// Gyro keys form a separate, optional input context. Rebinding one must not
// silently swap the normal camera/movement keys that work with gyro disabled.
static inline bool bind_key_same_context(int a, int b) {
    return (a >= BIND_GYRO_UP) == (b >= BIND_GYRO_UP);
}
// A keyboard binding can also be a mouse button: BIND_MOUSE + SDL_BUTTON_LEFT (1) .. SDL_BUTTON_X2 (5), past every
// SDL scancode.
#define BIND_MOUSE 0x400
#define BIND_MOUSE_LAST (BIND_MOUSE + 5)
// What the player uses (runtime/hle/input.c input_last_device)
enum { DEVICE_KEYBOARD, DEVICE_PAD };
int input_last_device(void);
int input_pad_family(void);  // the open controller: 0 Nintendo / other / none, 1 Xbox, 2 PlayStation

// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };

extern int16_t g_bind_pad[BIND_PAD_COUNT];  // PADB value per Switch button
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode or BIND_MOUSE + button per input (0 = none)

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int scancode);
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev);
bool bind_is_default(int dev);
bool bind_is_default_slot(int dev, int slot);

// "Click a binding, then press": begun from the UI; the next press (any thread's SDL event) is taken. Escape cancels,
// as does BIND_CAPTURE_MS without a press. On a conflict the two inputs swap.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int slot);
void bind_capture_cancel(void);
// capturing: dev / slot (else -1), ms left
bool bind_capture_active(int *dev, int *slot, int *ms_left);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset; *serial bumps
int bind_last_result(int *dev, int *slot, int *other, int *serial);

// savagewind.ini: bind_pad_<input> / bind_key_<input> (only the ones that differ from the defaults are written)
bool bind_settings_load(const char *key, const char *val);
void bind_settings_save(FILE *f);

// bind_* names (savagewind.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);

#ifdef __cplusplus
}
#endif
