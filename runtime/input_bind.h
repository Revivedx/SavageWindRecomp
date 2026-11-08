// Control remapping (runtime/hle/bindings.c): per-action host bindings and the raw Switch layer. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in savagewind.ini.
// The runtime overlay and launcher keep fixed navigation shortcuts. The game's menus read the raw Switch layer.
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
    BIND_WALK,                                           // held: left stick at 40%, keyboard or controller
    BIND_TILT,                                           // held: mouse / sensorless controller sticks tilt (motion)
    BIND_GYRO_UP, BIND_GYRO_DOWN, BIND_GYRO_LEFT, BIND_GYRO_RIGHT,  // key or controller button: virtual gyro during a shrine
    BIND_GYRO_TURN_LEFT, BIND_GYRO_TURN_RIGHT, BIND_GYRO_CENTER,    // apparatus (runtime/hle/motion.c motion_keyboard)
    BIND_FLY,                                            // toggles fly mode (control endpoint builds, runtime/control_fly.c)
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks + walk + tilt + gyro + fly
};
// BIND_DEV_ANY (a capture only): the next press of either device is taken, a controller button for the controller side,
// a key or mouse button for the keyboard side (only the stick-direction slots ignore the controller)
enum { BIND_DEV_PAD, BIND_DEV_KEY, BIND_DEV_ANY };
// Gyro grouping is for the UI. Walk, Tilt and gyro bindings allow duplicates on both devices and never take part
// in swaps, including when a legacy slot is rebound to their input. Other host slots keep their shared context.
static inline bool bind_key_gyro(int slot) { return slot >= BIND_GYRO_UP && slot <= BIND_GYRO_CENTER; }
static inline bool bind_key_same_context(int a, int b) {
    return (a < BIND_WALK || a > BIND_GYRO_CENTER) && (b < BIND_WALK || b > BIND_GYRO_CENTER);
}
// A keyboard binding can also be a mouse button: BIND_MOUSE + SDL_BUTTON_LEFT (1) .. SDL_BUTTON_X2 (5), past every
// SDL scancode.
#define BIND_MOUSE 0x400
#define BIND_MOUSE_LAST (BIND_MOUSE + 5)
// What the player uses (runtime/hle/input.c input_last_device)
enum { DEVICE_KEYBOARD, DEVICE_PAD };
int input_last_device(void);
int input_pad_family(void);  // the open controller: 0 Nintendo / other / none, 1 Xbox, 2 PlayStation, 3 Steam Deck
bool input_window_focused(void);
#define BIND_VALVE_VENDOR 0x28de  // (USB vendor of the Steam Deck's controls and of Steam Input's virtual pads)
#define BIND_DECK_PRODUCT 0x1205  // (the Steam Deck's controls, read directly)
// A controller's family for its icons and drawings (the numbers above), from what SDL says of it (type, USB vendor and
// product, name; first: the first Valve pad opened); runtime/hle/bindings.c
int bind_pad_family(int type, int vendor, int product, const char *name, bool first);

// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };

extern int16_t g_bind_pad[BIND_KEY_COUNT];  // PADB value, -1 = none; use bind_pad_slot (stick directions stay keyboard-only)
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode or BIND_MOUSE + button per input (0 = none)
bool bind_pad_slot(int slot);               // raw buttons, walk, tilt, gyro and fly; false for stick directions

const char *bind_input_name(int slot);         // "Pause menu", "Move forward", "Camera up"...
const char *bind_input_group(int slot);        // "gyro" for the seven gyro slots, NULL otherwise
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int scancode);
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev);
void bind_reset_slot(int slot);                // one input (or action: BIND_ACTION + id) back to its defaults, both devices
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

// ---- Per-action bindings
// The game asks "is action N held / pressed" through two functions (tools/patches.py _act_query); each action reads one
// Switch button (a table in the game: several actions share a button). Every action owns an explicit host key and
// controller binding, initially copied from that button's factory defaults for 1.9.0. An action differing on either
// device reads both host bindings directly. A factory-default action leaves the game to answer from its Switch layer,
// preserving JumpButtonChange and raw Switch injection. Before act_schema = 2, legacy ini raw-button rebinds migrate
// for action/device pairs without an act_* line. Menus also use the Switch layer. input.c publishes host state.
#define ACT_COUNT 41
enum { ACT_FOLLOW = -1, ACT_NONE = -2 };
extern int16_t g_act_key[ACT_COUNT];  // SDL_Scancode / BIND_MOUSE + button, or ACT_NONE
extern int16_t g_act_pad[ACT_COUNT];  // PADB value or ACT_NONE
// An action as a capture slot: bind_capture_begin(dev, BIND_ACTION + id)
#define BIND_ACTION 0x100
static inline bool bind_slot_is_action(int slot) { return slot >= BIND_ACTION && slot < BIND_ACTION + ACT_COUNT; }

void act_set(int dev, int id, int host);      // ACT_FOLLOW = restore factory default (not stored); ACT_NONE = nothing
bool act_is_default(int id);                  // both sides equal their factory defaults
bool act_hidden(int id);                      // no code reaches it in the game (or only by a computed number): not on the page
const char *act_name(int id);                 // what the game does with it, or NULL while unknown
int act_switch_button(int id);                // the Switch input (BIND_*) in the game table (1.9.0 fallback before boot)
void act_masks(const uint32_t *table);        // the game's table of one mask per action (its address, from the patch)
void act_reset(void);                         // every action back to the default

// Host state in, from runtime/hle/input.c. Main thread, on every key / button event:
uint64_t act_scan_keys(uint64_t key_buttons, int (*held)(int scancode));    // actions the keyboard side holds
uint64_t act_scan_pad(uint64_t pad_buttons, uint32_t host_buttons);         // ... the controller side holds
void act_publish(uint64_t keys, uint64_t pads);                             // (bit per action)
// The controller poll (game thread, once per GetNpadStates): freezes the held / pressed sets the game's queries see.
// live = false in the game's menus, with the window unfocused or the overlay up: the Switch layer answers then.
void act_poll(bool live);
// The hook: the answer to hold (kind 0) / trigger (kind 1) of action `id` on game pad object `pad`, or -1 = the game
// answers (factory defaults, a menu or an id out of range). `masks` is the game's mask table.
int rt_act_query(int kind, uint64_t pad, uint64_t id, uint64_t lr, uint64_t masks);

// Logging build (SAVAGEWIND_ACTLOG=1): a live list of what the game says is pressed / held, drawn over the game by the menu
// layer (ui/menu.mm act_overlay_draw) next to the "[act] poll N" lines in the log.
typedef struct {
    int id;
    unsigned pressed, held;  // times the game has said "pressed" / started saying "held" since the start
    uint32_t last_ms;        // SDL_GetTicks of the latest of either
    bool holding;            // the game says held right now
} ActLogRow;
bool act_log_enabled(void);
int act_log_rows(ActLogRow *out, int max);  // actions seen, the most recent first

#ifdef __cplusplus
}
#endif
