// Control remapping: tables runtime/hle/input.c reads on every input event (so a change applies at once), the
// capture used by the "click a binding, then press" UI, and their savagewind.ini lines. See runtime/input_bind.h.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../game_sites.h"
#include "../settings.h"
#include "../framepace.h"

_Static_assert(PADB_LTRIGGER == SDL_GAMEPAD_BUTTON_COUNT, "trigger pseudo-buttons follow SDL's buttons");

// Defaults: Nintendo layout by position (east = A, south = B, north = X, west = Y), triggers = ZL / ZR.
// Keyboard and mouse, laid out for BOTW the way PC action games are: WASD move (C held: walk), the mouse turns the
// camera (I / J / K / L too, runtime/hle/input.c), Space = X (jump), Left Shift = B (sprint, back), E = A
// (interact, confirm), left click = Y (attack), right click = ZL (shield, lock on), F = ZR (bow), Q = L (rune),
// R = R (throw), Left Ctrl = left stick press (crouch), middle click = right stick press (look), Tab = Plus
// (inventory), M = Minus (map), 1 / 2 / 3 / 4 = d-pad right / left / up / down (weapons, shields, runes, whistle:
// the quick menus), T held = the mouse tilts the controller (motion puzzles, runtime/hle/motion.c). Esc and F1 stay the overlay menu's (runtime/ui/menu.mm).
#define DEF_PAD { \
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST, \
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, \
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PADB_LTRIGGER, PADB_RTRIGGER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, \
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN, \
    [BIND_WALK ... BIND_GYRO_CENTER] = -1, \
    [BIND_FLY] = SDL_GAMEPAD_BUTTON_MISC1,  /* fly mode (test builds): the Capture / Share / Mute button, which the game never uses */ \
    [BIND_CONSOLE] = -1}
#define DEF_KEY { \
    SDL_SCANCODE_E, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, BIND_MOUSE + SDL_BUTTON_LEFT, SDL_SCANCODE_LCTRL, \
    BIND_MOUSE + SDL_BUTTON_MIDDLE, SDL_SCANCODE_Q, SDL_SCANCODE_R, BIND_MOUSE + SDL_BUTTON_RIGHT, SDL_SCANCODE_F, \
    SDL_SCANCODE_TAB, SDL_SCANCODE_M, \
    SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_1, SDL_SCANCODE_4, \
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_I, SDL_SCANCODE_K, \
    SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_C, SDL_SCANCODE_T, \
    SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_U, SDL_SCANCODE_O, SDL_SCANCODE_P, \
    SDL_SCANCODE_F7, SDL_SCANCODE_F12}
static const int16_t k_def_pad[BIND_KEY_COUNT] = DEF_PAD, k_def_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_pad[BIND_KEY_COUNT] = DEF_PAD, g_bind_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_pad2[BIND_KEY_COUNT], g_bind_key2[BIND_KEY_COUNT];

bool bind_pad_slot(int slot) { return (slot >= 0 && slot < BIND_PAD_COUNT) || (slot >= BIND_WALK && slot <= BIND_FLY); }

static const char *const k_ini[BIND_KEY_COUNT] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right", "walk", "tilt",
    "gyro_up", "gyro_down", "gyro_left", "gyro_right", "gyro_turn_left", "gyro_turn_right", "gyro_center", "fly",
    "console",
};
// What each raw Switch input does apart from the 41 actions: the Key bindings page and the F1 overlay list these by
// function, never by the Switch button. Left stick press has no function of its own (the action Crouch covers it): it
// is not listed and keeps its Switch name. D-pad Down's own is Magnesis pulling the object closer (#967; Up, the runes'
// quick menu, pushes it away).
static const char *const k_names[BIND_KEY_COUNT] = {
    "Menu: confirm", "Menu: back", "Menu: Hero's Path", "Menu: sort / sensor",
    "Left stick press", "Menu: inspect / zoom", "Menu: previous tab", "Menu: next tab",
    "Menu: previous tab 2", "Menu: next tab 2", "Pause menu", "Map (Sheikah Slate)",
    "Quick menu: shields", "Quick menu: runes", "Quick menu: weapons", "Magnesis: pull closer",
    "Move forward", "Move back", "Move left",
    "Move right", "Camera up", "Camera down", "Camera left", "Camera right", "Walk",
    "Tilt (hold)",
    "Gyro tilt up", "Gyro tilt down", "Gyro tilt left", "Gyro tilt right",
    "Gyro turn left", "Gyro turn right", "Gyro recenter",
    "Fly mode (test builds)",
    "Console",
};
// controller buttons by position (savagewind.ini)
static const char *const k_pad_ini[PADB_HOST_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger",
};

int bind_slot_from_name(const char *name) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (!strcmp(name, k_ini[i])) return i;
    return -1;
}
int bind_pad_from_name(const char *name) {
    for (int i = 0; i < PADB_HOST_COUNT; i++) if (!strcmp(name, k_pad_ini[i])) return i;
    return -1;
}

const char *bind_input_name(int slot) {
    if (bind_slot_is_action(slot)) return act_name(slot - BIND_ACTION) ? act_name(slot - BIND_ACTION) : "Action";  // (the page numbers the unnamed)
    return slot >= 0 && slot < BIND_KEY_COUNT ? k_names[slot] : "?";
}

const char *bind_input_group(int slot) { return bind_key_gyro(slot) ? "gyro" : NULL; }

// fam: 0 Nintendo (and pads SDL doesn't name), 1 Xbox, 2 PlayStation
const char *bind_pad_label(int host, int fam) {
    static const char *const nin[PADB_HOST_COUNT] = {
        "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Capture", "Right paddle 1", "Left paddle 1",
        "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "ZL", "ZR",
    };
    static const char *const xb[PADB_HOST_COUNT] = {
        "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS (press)", "RS (press)", "LB", "RB",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Share", "Paddle P1", "Paddle P3",
        "Paddle P2", "Paddle P4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT", "RT",
    };
    static const char *const ps[PADB_HOST_COUNT] = {
        "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mute", "Right back button", "Left back button",
        "Right Fn", "Left Fn", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2",
    };
    static const char *const deck[PADB_HOST_COUNT] = {
        "A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "...", "R4", "L4",
        "R5", "L5", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2",
    };
    if (host < 0 || host >= PADB_HOST_COUNT) return "(none)";
    return fam == 1 ? xb[host] : fam == 2 ? ps[host] : fam == 3 ? deck[host] : nin[host];
}

static const char *const k_mouse[5] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse Back", "Mouse Forward"};

const char *bind_key_label(int sc) {
    if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) return k_mouse[sc - BIND_MOUSE - 1];
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "(unnamed key)";
}

// Steam Input (a Deck's Gaming Mode) shows every controller as a virtual one of Valve's, an "Xbox 360" pad; SDL gives
// the controller behind it (type, name) when Steam says, often it doesn't (an 8BitDo: "Steam Virtual Gamepad"). So:
// PlayStation by its type; Nintendo's by theirs; a controller not Valve's by its type (Xbox, or none of these); a Valve pad is the Deck's own controls when it says so (the Deck's product, its
// name) or, saying nothing, when it is the first one opened (Steam Input's for the Deck's controls, there from boot) in
// Steam Deck mode; Steam Input's for another controller, an Xbox one. (A Valve vendor alone made each the Deck's.)
int bind_pad_family(int type, int vendor, int product, const char *name, bool first) {
    if (type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5) return 2;
    if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
        type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)
        return 0;
    if (vendor != BIND_VALVE_VENDOR) return type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE ? 1 : 0;
    if (product == BIND_DECK_PRODUCT || (name && strstr(name, "Steam Deck"))) return 3;
    bool unnamed = !name || !*name || strstr(name, "Steam Virtual") || strstr(name, "Steam Controller");
    return unnamed && first && settings_steam_deck() ? 3 : 1;
}

int bind_label_family(void) {
    extern int input_pad_family(void);  // runtime/hle/input.c (the game's open pad, or SAVAGEWIND_PAD_TYPE)
    int f = input_pad_family();
    if (f) return f;
    int n = 0;  // start screen: input.c hasn't opened a pad yet
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0)
        f = bind_pad_family(SDL_GetGamepadTypeForID(ids[0]), SDL_GetGamepadVendorForID(ids[0]),
                            SDL_GetGamepadProductForID(ids[0]), SDL_GetGamepadNameForID(ids[0]), true);
    SDL_free(ids);
    return f;
}

static void result(int kind, int dev, int slot, int other);
static int act_default(int dev, int id);
static void act_refresh(void);
static void act_reset_following_guard(int dev);

void bind_reset(int dev) {
    if (dev == BIND_DEV_PAD) {
        memcpy(g_bind_pad, k_def_pad, sizeof g_bind_pad);
        for (int i = 0; i < BIND_KEY_COUNT; i++) g_bind_pad2[i] = -1;
    } else {
        memcpy(g_bind_key, k_def_key, sizeof g_bind_key);
        memset(g_bind_key2, 0, sizeof g_bind_key2);
    }
    for (int id = 0; id < ACT_COUNT; id++) {
        act_set(dev, id, ACT_FOLLOW);
        act_set2(dev, id, ACT_NONE);
    }
    // The hidden lock-on check (29) has no row of its own and follows Guard (#1016), which works while it is nobody's
    // binding: a reset must leave it following, not make the factory default a binding of its own, or rebinding Guard
    // afterwards would stop moving it and the old input would go on targeting (#1057).
    act_reset_following_guard(dev);
    LOG("[input] %s bindings reset to the defaults\n", dev == BIND_DEV_PAD ? "controller" : "keyboard");
    result(5, dev, -1, -1);
}

void bind_reset_slot(int slot) {
    if (bind_slot_is_action(slot)) {
        act_set(BIND_DEV_PAD, slot - BIND_ACTION, ACT_FOLLOW);
        act_set(BIND_DEV_KEY, slot - BIND_ACTION, ACT_FOLLOW);
        act_set2(BIND_DEV_PAD, slot - BIND_ACTION, ACT_NONE);
        act_set2(BIND_DEV_KEY, slot - BIND_ACTION, ACT_NONE);
        return;
    }
    if (slot < 0 || slot >= BIND_KEY_COUNT) return;
    g_bind_key[slot] = k_def_key[slot];
    g_bind_key2[slot] = 0;
    if (bind_pad_slot(slot)) g_bind_pad[slot] = k_def_pad[slot];
    g_bind_pad2[slot] = -1;
    act_refresh();
}

// The controller's confirm and back buttons in the Xbox convention, as the button says ("Xbox: A confirms"): the
// Switch's A on the bottom button and its B on the right one. From Nintendo's layout the two, and every action on
// either of the two host buttons, take each other's. Then the game's own A and B actions still on the other one of the
// two follow them: bindings made one by one, or an earlier swap, can leave some behind. The button used to swap back
// and forth, so a press after a swap kept from the last session put confirm back on the right button and left Cancel
// (letting go of a wall, closing the paraglider, leaving a talk) on the confirm button (#934). Already in the Xbox
// convention, the Switch's buttons stay; Reset controller to defaults goes back to Nintendo's.
void bind_swap_ab(void) {
    const int south = SDL_GAMEPAD_BUTTON_SOUTH, east = SDL_GAMEPAD_BUTTON_EAST;
    int a = g_bind_pad[BIND_A], b = g_bind_pad[BIND_B];
    if (a < 0 || b < 0 || a == b) return;
    if (a != south || b != east) {
        g_bind_pad[BIND_A] = (int16_t)b, g_bind_pad[BIND_B] = (int16_t)a;
        for (int id = 0; id < ACT_COUNT; id++) {
            if (g_act_pad[id] == a) act_set(BIND_DEV_PAD, id, b);
            else if (g_act_pad[id] == b) act_set(BIND_DEV_PAD, id, a);
        }
        a = g_bind_pad[BIND_A], b = g_bind_pad[BIND_B];
    }
    if (a == south && b == east)
        for (int id = 0; id < ACT_COUNT; id++) {
            int sb = act_switch_button(id);
            if (sb == BIND_A && g_act_pad[id] == east) act_set(BIND_DEV_PAD, id, south);
            else if (sb == BIND_B && g_act_pad[id] == south) act_set(BIND_DEV_PAD, id, east);
        }
    LOG("[input] controller: confirm and back in the Xbox convention (A on %s, B on %s)\n", k_pad_ini[a], k_pad_ini[b]);
    result(6, BIND_DEV_PAD, -1, -1);
}

bool bind_ab_swapped(void) {
    return g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_SOUTH && g_bind_pad[BIND_B] == SDL_GAMEPAD_BUTTON_EAST;
}

bool bind_is_default(int dev) {
    for (int id = 0; id < ACT_COUNT; id++)
        if (!bind_is_default_slot(dev, BIND_ACTION + id)) return false;
    if (dev == BIND_DEV_PAD) {
        if (memcmp(g_bind_pad, k_def_pad, sizeof g_bind_pad)) return false;
        for (int i = 0; i < BIND_KEY_COUNT; i++) if (g_bind_pad2[i] != -1) return false;
        return true;
    }
    if (memcmp(g_bind_key, k_def_key, sizeof g_bind_key)) return false;
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (g_bind_key2[i] != 0) return false;
    return true;
}

bool bind_is_default_slot(int dev, int slot) {
    if (bind_slot_is_action(slot)) {
        int id = slot - BIND_ACTION;
        return act_default(dev, id) == (dev == BIND_DEV_PAD ? g_act_pad[id] : g_act_key[id]) &&
               (dev == BIND_DEV_PAD ? g_act_pad2[id] : g_act_key2[id]) == ACT_NONE;
    }
    if (slot < 0 || slot >= BIND_KEY_COUNT) return true;
    return dev == BIND_DEV_PAD ? (g_bind_pad[slot] == k_def_pad[slot] && g_bind_pad2[slot] == -1) :
                                 (g_bind_key[slot] == k_def_key[slot] && g_bind_key2[slot] == 0);
}


// ------------------------------------------------------------------ capture
static _Atomic int cap_dev = -1, cap_slot = -1;
static _Atomic bool cap_alt;
static _Atomic uint64_t cap_deadline;
static _Atomic int res_kind, res_dev, res_slot, res_other, res_serial;

static void result(int kind, int dev, int slot, int other) {
    res_kind = kind, res_dev = dev, res_slot = slot, res_other = other;
    res_serial++;
}

void bind_capture_begin_alt(int dev, int slot, bool alt) {
    if (!bind_slot_is_action(slot) &&
        (slot < 0 || slot >= BIND_KEY_COUNT || (dev == BIND_DEV_PAD && !bind_pad_slot(slot)))) return;
    if (dev == BIND_DEV_ANY && !bind_slot_is_action(slot) && slot >= BIND_KEY_COUNT) return;
    cap_deadline = SDL_GetTicks() + BIND_CAPTURE_MS;
    cap_alt = alt;
    cap_slot = slot;
    cap_dev = dev;
}
void bind_capture_begin(int dev, int slot) { bind_capture_begin_alt(dev, slot, false); }
bool bind_capture_is_alt(void) { return cap_dev >= 0 && cap_alt; }

void bind_capture_cancel(void) {
    if (cap_dev < 0) return;
    result(3, cap_dev, cap_slot, -1);
    cap_dev = -1;
}

bool bind_capture_active(int *dev, int *slot, int *ms_left) {
    int d = cap_dev;
    if (d >= 0 && SDL_GetTicks() >= cap_deadline) {
        // (a mouse whose own software sends a side button as something else arrives as nothing at all: #1036)
        LOG("[input] binding %s: nothing pressed in %d s\n", bind_input_name(cap_slot), BIND_CAPTURE_MS / 1000);
        result(4, d, cap_slot, -1);
        cap_dev = d = -1;
    }
    if (dev) *dev = d;
    if (slot) *slot = d >= 0 ? cap_slot : -1;
    if (ms_left) *ms_left = d >= 0 ? (int)(cap_deadline - SDL_GetTicks()) : 0;
    return d >= 0;
}

int bind_last_result(int *dev, int *slot, int *other, int *serial) {
    if (dev) *dev = res_dev;
    if (slot) *slot = res_slot;
    if (other) *other = res_other;
    if (serial) *serial = res_serial;
    return res_kind;
}

void bind_set2(int dev, int slot, int host) {
    if (slot < 0 || slot >= BIND_KEY_COUNT) return;
    if (dev == BIND_DEV_PAD) {
        if (bind_pad_slot(slot)) g_bind_pad2[slot] = (int16_t)host;
    } else {
        g_bind_key2[slot] = (int16_t)host;
    }
    act_refresh();
}

// Bind `host` to `slot`; only legacy host slots swap. Actions, Walk, Tilt and gyro allow shared inputs.
static void assign(int dev, int slot, int host) {
    if (cap_alt) {
        if (bind_slot_is_action(slot)) {
            act_set2(dev, slot - BIND_ACTION, host);
            result(1, dev, slot, -1);
            return;
        }
        bind_set2(dev, slot, host);
        result(1, dev, slot, -1);
        return;
    }
    if (bind_slot_is_action(slot)) {  // an action may share its key with any other: nothing swaps
        act_set(dev, slot - BIND_ACTION, host);
        LOG("[input] %s action %d -> %s\n", dev == BIND_DEV_PAD ? "controller" : "keyboard", slot - BIND_ACTION,
            dev == BIND_DEV_PAD ? k_pad_ini[host] : bind_key_label(host));
        result(1, dev, slot, -1);
        return;
    }
    int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int other = -1;
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (i != slot && (dev == BIND_DEV_KEY ? bind_key_same_context(i, slot) : (bind_pad_slot(i) && bind_key_same_context(i, slot))) && t[i] == host) {
            other = i;
            t[i] = t[slot];
        }
    t[slot] = (int16_t)host;
    LOG("[input] %s %s -> %s%s%s%s\n", dev == BIND_DEV_PAD ? "controller" : "keyboard", k_names[slot],
        dev == BIND_DEV_PAD ? k_pad_ini[host] : bind_key_label(host), other >= 0 ? " (swapped with " : "",
        other >= 0 ? k_names[other] : "", other >= 0 ? ")" : "");
    if (other >= 0) LOG("[input]   %s -> %s\n", k_names[other], dev == BIND_DEV_PAD ? bind_pad_label(t[other], 0) : bind_key_label(t[other]));
    result(other >= 0 ? 2 : 1, dev, slot, other);
    act_refresh();
}

bool bind_capture_event(const SDL_Event *e) {
    int dev = cap_dev;
    if (dev < 0) return false;
    // which sides this capture takes (BIND_DEV_ANY: both, the controller only where the input has a controller side)
    const bool kd = dev != BIND_DEV_PAD;
    const bool pd = dev == BIND_DEV_PAD || (dev == BIND_DEV_ANY && (bind_slot_is_action(cap_slot) || bind_pad_slot(cap_slot)));
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) return true;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { bind_capture_cancel(); return true; }
        // A key SDL has no scancode for (a mouse button its software turned into a key: #1036) would bind "(none)"
        if (kd && e->key.scancode == SDL_SCANCODE_UNKNOWN) {
            LOG("[input] binding %s: a key with no scancode (key code %#x) can't be bound\n", bind_input_name(cap_slot),
                (unsigned)e->key.key);
            return true;
        }
        // F1 (menu), F9 (frame dump), F11 (fullscreen), PrintScreen (screenshot) and the active Console key stay
        // the overlay's; a controller capture ignores keys. F12 / Grave can be bound once Console moves elsewhere (#1020).
        bool is_console = cap_slot != BIND_CONSOLE &&
                          ((g_bind_key[BIND_CONSOLE] > 0 && e->key.scancode == g_bind_key[BIND_CONSOLE]) ||
                           (g_bind_key2[BIND_CONSOLE] > 0 && e->key.scancode == g_bind_key2[BIND_CONSOLE]) ||
                           (g_bind_key[BIND_CONSOLE] == SDL_SCANCODE_F12 && e->key.scancode == SDL_SCANCODE_GRAVE));
        if (!kd || e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_F9 ||
            e->key.scancode == SDL_SCANCODE_F11 || e->key.scancode == SDL_SCANCODE_PRINTSCREEN ||
            is_console || e->key.scancode >= SDL_SCANCODE_COUNT) return true;
        cap_dev = -1;
        assign(BIND_DEV_KEY, cap_slot, e->key.scancode);
        return true;
    // Keep host held-state current even while capturing. Swallowing the release of the confirm key / button left
    // it down in input.c, so ending a capture pressed confirm again and reopened the binding row (#175).
    case SDL_EVENT_KEY_UP:
        return false;
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:  // a keyboard binding can be a mouse button (a controller capture ignores it)
        if (kd && (e->button.button < SDL_BUTTON_LEFT || e->button.button > SDL_BUTTON_X2))
            LOG("[input] binding %s: mouse button %d can't be bound\n", bind_input_name(cap_slot), e->button.button);
        if (!kd || e->button.button < SDL_BUTTON_LEFT || e->button.button > SDL_BUTTON_X2) return true;
        cap_dev = -1;
        assign(BIND_DEV_KEY, cap_slot, BIND_MOUSE + e->button.button);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Home / Guide opens the menu; a keyboard capture ignores the controller (it times out, or Esc)
        if (!pd || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) return true;
        cap_dev = -1;
        assign(BIND_DEV_PAD, cap_slot, e->gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return false;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            e->gaxis.value <= 16000) return false;  // a trigger may also have been bound to confirm
        if (pd && e->gaxis.value > 16000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            cap_dev = -1;
            assign(BIND_DEV_PAD, cap_slot, e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER);
        }
        return true;  // (sticks: no menu movement while waiting)
    default:
        return false;
    }
}

// ------------------------------------------------------------------ savagewind.ini
static bool act_settings_load(const char *key, const char *val);
static void act_migrate(int dev, int slot);
bool bind_settings_load(const char *key, const char *val) {
    if (!strncmp(key, "act_", 4)) return act_settings_load(key, val);
    bool pad2 = !strncmp(key, "bind_pad2_", 10), kb2 = !strncmp(key, "bind_key2_", 10);
    if (pad2 || kb2) {
        for (int i = 0; i < BIND_KEY_COUNT; i++) {
            if (strcmp(key + 10, k_ini[i]) || (pad2 && !bind_pad_slot(i))) continue;
            if (pad2) {
                int h = !strcmp(val, "none") ? -1 : bind_pad_from_name(val);
                if (h >= -1) g_bind_pad2[i] = (int16_t)h;
            } else {
                int sc = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : SDL_SCANCODE_UNKNOWN;
                for (int m = 0; m < 5; m++)
                    if (!strcmp(val, k_mouse[m])) sc = BIND_MOUSE + 1 + m;
                if (sc != SDL_SCANCODE_UNKNOWN || !strcmp(val, "none")) g_bind_key2[i] = (int16_t)sc;
            }
        }
        act_refresh();
        return true;
    }
    bool pad = !strncmp(key, "bind_pad_", 9), kb = !strncmp(key, "bind_key_", 9);
    if (!pad && !kb) return false;
    for (int i = 0; i < BIND_KEY_COUNT; i++) {
        if (strcmp(key + 9, k_ini[i]) || (pad && !bind_pad_slot(i))) continue;
        if (pad) {
            int h = !strcmp(val, "none") ? -1 : bind_pad_from_name(val);
            if (h < 0 && strcmp(val, "none")) continue;
            g_bind_pad[i] = (int16_t)h;
        } else {
            int sc = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : SDL_SCANCODE_UNKNOWN;
            for (int m = 0; m < 5; m++)
                if (!strcmp(val, k_mouse[m])) sc = BIND_MOUSE + 1 + m;
            if (sc == SDL_SCANCODE_UNKNOWN && strcmp(val, "none")) continue;
            g_bind_key[i] = (int16_t)sc;
        }
        if (i < BIND_PAD_COUNT) act_migrate(pad ? BIND_DEV_PAD : BIND_DEV_KEY, i);
    }
    act_refresh();
    return true;
}

static void act_settings_save(FILE *f);
void bind_settings_save(FILE *f) {
    act_settings_save(f);
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (bind_pad_slot(i) && g_bind_pad[i] != k_def_pad[i] && g_bind_pad[i] >= -1 && g_bind_pad[i] < PADB_HOST_COUNT)
            fprintf(f, "bind_pad_%s = %s\n", k_ini[i], g_bind_pad[i] < 0 ? "none" : k_pad_ini[g_bind_pad[i]]);
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (g_bind_key[i] != k_def_key[i])
            fprintf(f, "bind_key_%s = %s\n", k_ini[i], g_bind_key[i] ? bind_key_label(g_bind_key[i]) : "none");
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (bind_pad_slot(i) && g_bind_pad2[i] >= 0 && g_bind_pad2[i] < PADB_HOST_COUNT)
            fprintf(f, "bind_pad2_%s = %s\n", k_ini[i], k_pad_ini[g_bind_pad2[i]]);
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (g_bind_key2[i] > 0)
            fprintf(f, "bind_key2_%s = %s\n", k_ini[i], bind_key_label(g_bind_key2[i]));
}

// ------------------------------------------------------------------ per-action bindings
// Each action owns its host defaults, copied from the factory Switch layout. ACT_FOLLOW remains an API reset
// command for the existing UI, never a stored binding. Only actions differing from their defaults use host answers.
int16_t g_act_key[ACT_COUNT], g_act_pad[ACT_COUNT];
int16_t g_act_key2[ACT_COUNT], g_act_pad2[ACT_COUNT];
static bool a_schema;  // schema 2 or newer: legacy raw-button migration has already happened
static _Atomic uint64_t a_custom;  // at least one device differs from its factory action default (or live mask swapped)
static _Atomic uint64_t a_mask_diff;  // k_masks[id] != k_masks_190[id] at the last act_refresh()
static uint64_t a_explicit_key, a_explicit_pad;  // ini/action edits take precedence over legacy raw-button lines

static const uint32_t *k_masks;  // the game's table: one pad mask (sead::Controller bit) per action
static _Atomic uint64_t a_keys, a_pads;  // held now, by the keyboard side / the controller side (main thread)
static _Atomic uint64_t a_hold, a_trig;  // frozen at the last poll: what the game's queries see
static _Atomic bool a_live;

// What the game calls them. NULL until a run with the logging build (SAVAGEWIND_ACTLOG) has seen what an action does;
// only the ones whose use is certain are named.
static const char *const k_act_names[ACT_COUNT] = {
    [0] = "Interact", [2] = "Jump", [3] = "Attack", [5] = "Shield bash", [9] = "Lower shield", [10] = "Crouch step", [11] = "Dodge",
    [12] = "Sprint", [13] = "Guard / lock on", [14] = "Cancel", [15] = "Scope", [17] = "Crouch", [18] = "Bow",
    [19] = "Paraglider", [25] = "Dismount", [26] = "Speed up horse", [27] = "Pet horse", [28] = "Throw weapon", [31] = "Take photo",
    [33] = "Aim rune", [34] = "Activate rune", [35] = "Call horse", [36] = "Water spin attack", [37] = "Throw distance", [38] = "Cycle accelerate", [39] = "Cycle brake", [40] = "Cycle jump",
};
// No code in the game reaches these by a number (they may still be asked by a computed one): kept off the page.
// 32 is asked while wall climbing but nothing happens with the answer, so a row for it would only mislead.
#define ACT_HIDDEN_MASK ((1ull << 1) | (1ull << 4) | (1ull << 6) | (1ull << 7) | (1ull << 8) | (1ull << 16) | \
                         (1ull << 20) | (1ull << 21) | (1ull << 22) | (1ull << 23) | (1ull << 24) | (1ull << 29) | \
                         (1ull << 30) | (1ull << 32))

const char *act_name(int id) { return id >= 0 && id < ACT_COUNT ? k_act_names[id] : NULL; }
bool act_hidden(int id) { return id >= 0 && id < ACT_COUNT && (ACT_HIDDEN_MASK >> id & 1); }

// The game's mask bits are sead's pad indices (open-ead: PadIdx), not the Switch's nn::hid bits.
static int mask_button(uint32_t m) {
    switch (m) {
    case 0x1: return BIND_A;
    case 0x2: return BIND_B;
    case 0x4: return BIND_ZL;
    case 0x8: return BIND_X;
    case 0x10: return BIND_Y;
    case 0x20: return BIND_ZR;
    case 0x40: return BIND_RSTICK;
    case 0x80: return BIND_LSTICK;
    case 0x2000: return BIND_L;
    case 0x4000: return BIND_R;
    case 0x10000: return BIND_UP;
    case 0x20000: return BIND_DOWN;
    default: return -1;
    }
}

// The game's table for 1.9.0 (all 41 masks): factory defaults and legacy migration always use this fixed table.
// act_switch_button also uses it before the game has supplied its live table.
static const uint32_t k_masks_190[ACT_COUNT] = {
    0x1, 0x2, 0x8, 0x10, 0x8, 0x1, 0x8, 0x2, 0x8, 0x2, 0x8, 0x8, 0x2, 0x4, 0x2, 0x40, 0x1, 0x80, 0x20, 0x8, 0x2,
    0x10000, 0x20000, 0x10000, 0x20000, 0x2, 0x1, 0x2000, 0x4000, 0x4, 0x2, 0x4000, 0x4000, 0x2000, 0x1, 0x20000,
    0x10, 0x20000, 0x1, 0x2, 0x2000};

int act_switch_button(int id) {
    if (id < 0 || id >= ACT_COUNT) return -1;
    return mask_button((k_masks ? k_masks : k_masks_190)[id]);
}

static int act_default(int dev, int id) {
    int b = mask_button(k_masks_190[id]);
    if (b < 0) return ACT_NONE;
    return dev == BIND_DEV_PAD ? k_def_pad[b] : k_def_key[b];
}

bool act_is_default(int id) {
    return id < 0 || id >= ACT_COUNT ||
        (g_act_key[id] == act_default(BIND_DEV_KEY, id) && g_act_pad[id] == act_default(BIND_DEV_PAD, id) &&
         g_act_key2[id] == ACT_NONE && g_act_pad2[id] == ACT_NONE);
}

// The raw Switch input b (not a face button) is bound to something other than its defaults. In play the game reads
// those slots for actions at their factory defaults, so Call horse moved with Magnesis: pull closer and Aim rune with
// Menu: previous tab. The face buttons need none of this: in play act_gameplay_buttons builds them from the actions.
static bool raw_moved(int b) {
    if (b < 0 || b >= BIND_PAD_COUNT || b == BIND_A || b == BIND_B || b == BIND_X || b == BIND_Y) return false;
    return g_bind_key[b] != k_def_key[b] || g_bind_key2[b] != 0 || g_bind_pad[b] != k_def_pad[b] ||
           g_bind_pad2[b] != -1;
}

static void act_refresh(void) {
    uint64_t custom = 0, diff = 0;
    for (int id = 0; id < ACT_COUNT; id++) {
        bool swapped = k_masks && k_masks[id] != k_masks_190[id];
        if (swapped) diff |= 1ull << id;
        // (an action on the page keeps its own bindings when its Switch input moves; a hidden one, with no row of its
        // own, goes on following that input)
        if (!act_is_default(id) || swapped || (!act_hidden(id) && raw_moved(mask_button(k_masks_190[id]))))
            custom |= 1ull << id;
    }
    // Action 29 is the hidden Z-targeting/lock-on check sharing BIND_ZL with action 13 ("Guard / lock on", #1016).
    if (custom >> 13 & 1) custom |= 1ull << 29;
    a_mask_diff = diff;
    a_custom = custom;
}

void act_set(int dev, int id, int host) {
    if (id < 0 || id >= ACT_COUNT) return;
    if (host < 0 && host != ACT_NONE) host = act_default(dev, id);
    if (dev == BIND_DEV_PAD) {
        g_act_pad[id] = (int16_t)host;
        a_explicit_pad |= 1ull << id;
    } else {
        g_act_key[id] = (int16_t)host;
        a_explicit_key |= 1ull << id;
    }
    act_refresh();
}

void act_set2(int dev, int id, int host) {
    if (id < 0 || id >= ACT_COUNT) return;
    if (host < 0) host = ACT_NONE;
    if (dev == BIND_DEV_PAD) g_act_pad2[id] = (int16_t)host;
    else g_act_key2[id] = (int16_t)host;
    act_refresh();
}

void act_reset(void) {
    for (int i = 0; i < ACT_COUNT; i++) {
        g_act_key[i] = (int16_t)act_default(BIND_DEV_KEY, i);
        g_act_pad[i] = (int16_t)act_default(BIND_DEV_PAD, i);
        g_act_key2[i] = ACT_NONE;
        g_act_pad2[i] = ACT_NONE;
    }
    a_explicit_key = a_explicit_pad = 0;
    act_refresh();
}

// A reset puts every action back at its factory default. The hidden lock-on check (29) has no row of its own and goes
// on following Guard (#1016): a reset must leave it following, not make the default a binding of its own, or rebinding
// Guard afterwards would stop moving it and Guard's old input would go on targeting (#1057).
static void act_reset_following_guard(int dev) {
    if (dev == BIND_DEV_PAD) a_explicit_pad &= ~(1ull << 29);
    else a_explicit_key &= ~(1ull << 29);
}

// Initialized before settings or either Controls page can read the public arrays (also in standalone tests).
__attribute__((constructor)) static void act_init(void) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) g_bind_pad2[i] = -1, g_bind_key2[i] = 0;
    act_reset();
}

void act_masks(const uint32_t *table) {
    k_masks = table;
    act_refresh();
}

// Applied while reading legacy ini lines, independent of line order and independently for each device.
static void act_migrate(int dev, int slot) {
    if (a_schema) return;
    uint64_t explicit = dev == BIND_DEV_PAD ? a_explicit_pad : a_explicit_key;
    int16_t *actions = dev == BIND_DEV_PAD ? g_act_pad : g_act_key;
    int host = dev == BIND_DEV_PAD ? g_bind_pad[slot] : g_bind_key[slot];
    for (int id = 0; id < ACT_COUNT; id++)
        if (!(explicit >> id & 1) && mask_button(k_masks_190[id]) == slot)
            actions[id] = host < 0 || (dev == BIND_DEV_KEY && !host) ? ACT_NONE : (int16_t)host;
    act_refresh();
}

int act_prompt_host(int dev, int sw) {
    const int16_t *actions = dev == BIND_DEV_PAD ? g_act_pad : g_act_key;
    int host = -1;
    for (int id = 0; id < ACT_COUNT; id++) {
        if (act_hidden(id) || act_switch_button(id) != sw) continue;
        int h = actions[id];
        if (h == ACT_NONE || (dev != BIND_DEV_PAD && h <= 0)) continue;  // (unbound: says nothing about the others)
        if (host >= 0 && h != host) return -1;
        host = h;
    }
    return host;
}

uint64_t act_scan_keys(uint64_t key_buttons, int (*held)(int scancode)) {
    (void)key_buttons;  // kept for callers; actions read host inputs, never the live Switch layout
    uint64_t out = 0, custom = a_custom;
    for (int id = 0; id < ACT_COUNT; id++) {
        if (!(custom >> id & 1)) continue;
        int k = g_act_key[id], k2 = g_act_key2[id];
        out |= (uint64_t)((k > 0 && held(k)) || (k2 > 0 && held(k2))) << id;
    }
    if (!(a_explicit_key >> 29 & 1) && (custom >> 13 & 1))
        out = (out & ~(1ull << 29)) | ((out >> 13 & 1ull) << 29);
    return out;
}

uint64_t act_scan_pad(uint64_t pad_buttons, uint32_t host_buttons) {
    (void)pad_buttons;
    uint64_t out = 0, custom = a_custom;
    for (int id = 0; id < ACT_COUNT; id++) {
        if (!(custom >> id & 1)) continue;
        int k = g_act_pad[id], k2 = g_act_pad2[id];
        if (k_masks && k_masks[id] != k_masks_190[id] && k == act_default(BIND_DEV_PAD, id)) {
            int b = mask_button(k_masks[id]);
            if (b >= 0) k = k_def_pad[b];
        }
        bool on = (k >= 0 && k < PADB_HOST_COUNT && (host_buttons >> k & 1)) ||
                  (k2 >= 0 && k2 < PADB_HOST_COUNT && (host_buttons >> k2 & 1));
        out |= (uint64_t)on << id;
    }
    if (!(a_explicit_pad >> 29 & 1) && (custom >> 13 & 1))
        out = (out & ~(1ull << 29)) | ((out >> 13 & 1ull) << 29);
    return out;
}

static bool act_held_host(int id, int (*held)(int scancode), uint32_t host) {
    if (id < 0 || id >= ACT_COUNT) return false;
    int k = g_act_key[id], k2 = g_act_key2[id], p = g_act_pad[id], p2 = g_act_pad2[id];
    return (k > 0 && held && held(k)) || (k2 > 0 && held && held(k2)) ||
           (p >= 0 && p < PADB_HOST_COUNT && (host >> p & 1)) ||
           (p2 >= 0 && p2 < PADB_HOST_COUNT && (host >> p2 & 1));
}

// During gameplay, decouple in-game action buttons (Y/X/A/B) from raw menu rebinds
// so that rebinding a menu slot (e.g. "Menu: sort / sensor") never hijacks actions in play.
// Only the raw keyboard/pad mask is rewritten; script/console/cursor/auto-continue bits stay untouched.
uint64_t act_gameplay_buttons(uint64_t raw, int (*held)(int scancode), uint32_t host) {
    raw &= ~((1ull << BIND_Y) | (1ull << BIND_X) | (1ull << BIND_A) | (1ull << BIND_B));
    if (act_held_host(3, held, host) || act_held_host(36, held, host)) raw |= 1ull << BIND_Y;
    if (act_held_host(2, held, host)) raw |= 1ull << BIND_X;
    if (act_held_host(0, held, host)) raw |= 1ull << BIND_A;
    if (act_held_host(12, held, host) || act_held_host(14, held, host)) raw |= 1ull << BIND_B;
    // ZL and ZR too, once Guard / lock on (13) or Bow (18) answers from its own bindings: the game also reads ZL and
    // ZR as buttons, not only through those actions (the shield held up, the lock-on kept), so a moved Menu: previous
    // tab 2 / next tab 2 went on guarding in play and Guard's own key did half of it (#1103). Factory defaults leave
    // the raw input as it is: the two are the same key then.
    uint64_t custom = a_custom;
    if (custom >> 13 & 1) {
        raw &= ~(1ull << BIND_ZL);
        if (act_held_host(13, held, host)) raw |= 1ull << BIND_ZL;
    }
    if (custom >> 18 & 1) {
        raw &= ~(1ull << BIND_ZR);
        if (act_held_host(18, held, host)) raw |= 1ull << BIND_ZR;
    }
    return raw;
}

// Each side publishes its own half (the keyboard and the controller change at different times)
void act_publish(uint64_t keys, uint64_t pads) {
    if (keys != ~0ull) a_keys = keys;
    if (pads != ~0ull) a_pads = pads;
}

// ---- logging build: SAVAGEWIND_ACTLOG=1 prints which actions the game asks about, from where, and which of them its
// own pad object answers yes to (the unswapped mask test), once per change, so a scripted run that presses one Switch
// button at a time shows what each action is.
static int act_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_ACTLOG") != NULL;
    return on;
}
static _Atomic uint64_t l_seen[2], l_true[2], l_last[2];
static _Atomic uint64_t l_pads[4];
static _Atomic unsigned l_polls, l_queries[2];
// for the on-screen list (written on the game thread, read by the menu layer)
static _Atomic unsigned l_cnt[2][ACT_COUNT];
static _Atomic uint32_t l_last_ms[ACT_COUNT];
static _Atomic uint64_t l_holding;

bool act_log_enabled(void) { return act_log_on(); }

int act_log_rows(ActLogRow *out, int max) {
    ActLogRow all[ACT_COUNT];
    int n = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        unsigned p = l_cnt[1][i], h = l_cnt[0][i];
        if (!p && !h) continue;
        ActLogRow r = {i, p, h, l_last_ms[i], (l_holding >> i & 1) != 0};
        int j = n++;
        while (j > 0 && all[j - 1].last_ms < r.last_ms) { all[j] = all[j - 1]; j--; }  // (newest first)
        all[j] = r;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = all[i];
    return n;
}

static void act_log_ids(char *out, size_t n, uint64_t set) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < ACT_COUNT && o + 6 < n; i++)
        if (set >> i & 1) o += (size_t)snprintf(out + o, n - o, "%s%d", o ? "," : "", i);
    if (!o) snprintf(out, n, "-");
}

static void act_log_query(int kind, uint64_t pad, unsigned id, uint64_t lr, bool custom) {
    l_queries[kind]++;
    if (id >= ACT_COUNT || !pad) return;
    if (!(l_seen[kind] >> id & 1)) {
        l_seen[kind] |= 1ull << id;
        LOG("[act] %s of action %u: first asked from %llx on pad object %llx (mask %#x)\n", kind ? "pressed" : "held", id,
            (unsigned long long)lr, (unsigned long long)pad, k_masks ? k_masks[id] : 0);
    }
    for (int i = 0; i < 4; i++) {  // distinct pad objects
        uint64_t p = l_pads[i];
        if (p == pad) break;
        if (!p) { l_pads[i] = pad; LOG("[act] pad object %llx asked\n", (unsigned long long)pad); break; }
    }
    if (custom) return;  // an action with a binding of its own is logged by what the game is told (rt_act_query)
    uint32_t word = *(const uint32_t *)(uintptr_t)(pad + (kind ? 0x10 : 0x11c));
    if (k_masks && (word & k_masks[id])) l_true[kind] |= 1ull << id;
}

static void act_log_poll(void) {
    unsigned n = ++l_polls;
    for (int kind = 0; kind < 2; kind++) {
        uint64_t t = l_true[kind];
        l_true[kind] = 0;
        if (t == l_last[kind]) continue;
        uint64_t fresh = t & ~l_last[kind];
        l_last[kind] = t;
        if (!kind) l_holding = t;
        uint32_t ms = SDL_GetTicks();
        for (int i = 0; i < ACT_COUNT; i++)
            if (fresh >> i & 1) { l_cnt[kind][i]++; l_last_ms[i] = ms; }
        char ids[200];
        act_log_ids(ids, sizeof ids, t);
        LOG("[act] poll %u: game says %s: %s (t=%.1f s)\n", n, kind ? "pressed" : "held", ids, ms / 1000.0);
    }
    if (n % 600 == 0) {
        LOG("[act] %u polls: %u held queries, %u pressed queries\n", n, (unsigned)l_queries[0], (unsigned)l_queries[1]);
        l_queries[0] = l_queries[1] = 0;
    }
}

// ---- the HUD's A prompt (Talk, Pick up, Open, Climb down...) comes first
// An action moved onto the same button as Interact (Jump on A, say) would also fire whenever that button answers the
// prompt: picking something up while swimming dashed, talking jumped. While the prompt is on screen, those actions are
// "not pressed": the button does what the prompt says, as on the Switch where they are different buttons. The actions
// the game itself puts on Switch A (Interact, Activate rune, ...) keep answering. The prompt: DoCommand_00's N_A_00,
// shown while the A prompt is up (its N_InOut_00 fades the whole HUD group out).
static uint64_t pane_child(uint64_t pane, const char *name, unsigned depth, unsigned *budget) {
    if (pane < GUEST_LO || pane >= GUEST_HI - 0x100 || depth > 16 || !*budget) return 0;
    --*budget;
    if (!strncmp((const char *)(uintptr_t)(pane + 0xb8), name, 24)) return pane;
    uint64_t head = pane + 0x20, link = LD64(head + 8);  // (children: links 8 bytes into the child)
    for (int k = 0; link >= GUEST_LO && link < GUEST_HI && link != head && k < 64 && *budget; k++, link = LD64(link + 8)) {
        uint64_t found = pane_child(link - 8, name, depth + 1, budget);
        if (found) return found;
    }
    return 0;
}
static bool named(uint64_t obj, unsigned off, const char *name) {
    uint64_t p = obj >= GUEST_LO && obj < GUEST_HI - 0x100 ? LD64(obj + off) : 0;
    return p >= GUEST_LO && p < GUEST_HI - 64 && !strcmp((const char *)(uintptr_t)p, name);
}
static uint64_t a_label;  // the A prompt's label (T_GuideA_01: its text at +0xe0, UTF-16)
static uint64_t a_group;  // the A slot (N_A_00)
// The A prompt's opacity (0-255; 0 while it isn't shown, as in a fade from 0 up and back down)
static int interact_prompt_alpha(void) {
    static uint64_t screen, group, hud;  // DoCommand_00, its N_A_00 and N_InOut_00 (found again when the HUD is rebuilt)
    if (!g_site.screen_mgr) return 0;
    if (!named(screen, 0xd0, "DoCommand_00") || !group || !hud ||
        strncmp((const char *)(uintptr_t)(group + 0xb8), "N_A_00", 8) ||
        strncmp((const char *)(uintptr_t)(hud + 0xb8), "N_InOut_00", 12)) {
        screen = group = hud = 0;
        uint64_t mgr = LD64(g_site.screen_mgr), arr = mgr ? LD64(mgr + 0x30) : 0;
        unsigned count = mgr ? LD32(mgr + 0x28) : 0;
        for (unsigned i = 0; arr && i < count && i < 256 && !screen; i++)
            if (named(LD64(arr + 8 * i), 0xd0, "DoCommand_00")) screen = LD64(arr + 8 * i);
        uint64_t layout = screen ? LD64(screen + 0x30) : 0, root = layout >= GUEST_LO ? LD64(layout + 0x18) : 0;
        unsigned budget = 64;
        hud = pane_child(root, "N_InOut_00", 0, &budget);
        budget = 64;
        group = pane_child(hud, "N_A_00", 0, &budget);
        budget = 64;
        a_label = pane_child(group, "T_GuideA_01", 0, &budget);
        if (!group) { screen = group = hud = a_label = 0; return 0; }
    }
    a_group = group;
    return (LD8(group + 0x58) & 1) && (LD8(hud + 0x58) & 1) ? LD8(hud + 0x5a) : 0;  // (visible; global alpha)
}
static bool interact_prompt_up(void) { return interact_prompt_alpha() >= 0x80; }

uint64_t act_a_slot_pane(const char *name) {
    interact_prompt_alpha();  // (finds the slot again after the HUD is rebuilt)
    unsigned budget = 64;
    return a_group ? pane_child(a_group, name, 0, &budget) : 0;
}

uint32_t act_a_prompt_label(void) {  // (while the prompt is on screen at all: fading in or out too)
    if (!interact_prompt_alpha() || !a_label) return 0;
    uint64_t text = LD64(a_label + 0xe0);
    if (text < GUEST_LO || text >= GUEST_HI - 128) return 1;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++) {
        uint16_t ch = LD16(text + 2 * i);
        if (!ch) break;
        h = (h ^ ch) * 16777619u;
    }
    return h | 1;
}

// When the prompt was last seen (each controller poll): answering it takes it away within the same frame, before the
// game asks about Jump, so the press that picked something up still counts as the prompt's for a moment
#define PROMPT_GRACE_MS 250
static _Atomic uint64_t a_prompt_ms;

// id would answer through Interact's button although the game puts it on another Switch button
static bool shares_interact(int id) {
    if (id <= 0 || id >= ACT_COUNT || mask_button(k_masks_190[id]) == mask_button(k_masks_190[0])) return false;
    int p = g_act_pad[id], k = g_act_key[id];
    return (p >= 0 && p == g_act_pad[0]) || (k > 0 && k == g_act_key[0]);
}
static bool any_shares_interact(void) {
    for (int id = 1; id < ACT_COUNT; id++)
        if (a_custom >> id & 1 && shares_interact(id)) return true;
    return false;
}

// A conversation's window (MessageDialog, its closing included), not lines spoken over play, whose camera still turns:
// the press that picks an answer is the prompt's too, and the game asks about Jump again as the window closes
static bool conversation_up(void) {
    extern bool input_cam_live(void);  // (runtime/hle/input.c)
    return framepace_dialogue_ui() && !input_cam_live();
}

// ---- the controller poll and the game's queries
#define EDGE_KEEP_MS 100  // (several of the game's updates at any frame rate it runs at)
static _Atomic uint64_t a_prev;
static _Atomic bool a_served;  // a pressed query was answered since the last poll

void act_poll(bool live) {
    static bool was_live;
    uint64_t custom = a_custom;
    uint64_t cur = live ? (a_keys | a_pads) & custom : 0, prev = a_prev;
    if (live && !was_live) prev = cur;  // held through a menu closing: not a new press
    uint64_t edge = cur & ~prev;
    // A poll nobody read (no pressed query since the last one) keeps its edge for the next, so a press is never lost
    // however often the game polls compared with how often it updates its pad - but only for EDGE_KEEP_MS: while a
    // message is up ("You got ..." after a new item) the game asks about no player action, and the press that closed
    // it, kept until the player was asked again, jumped as soon as the message was gone
    static uint64_t edge_ms;
    uint64_t now = SDL_GetTicks(), kept = a_served ? 0 : a_trig;
    if (kept && now - edge_ms > EDGE_KEEP_MS) kept = 0;
    if (edge) edge_ms = now;
    a_trig = (kept | edge) & custom;
    a_served = false;
    a_hold = cur;
    a_prev = cur;
    a_live = live;
    was_live = live;
    if (live && any_shares_interact() && (interact_prompt_up() || conversation_up())) a_prompt_ms = now;
    if (act_log_on()) act_log_poll();
}

// The last time the game asked the Master Cycle Zero's accelerate action (38): it does every frame while Link rides it
static _Atomic uint64_t cycle_asked_ns;
bool act_cycle_live(void) {
    uint64_t t = cycle_asked_ns;
    return t && SDL_GetTicksNS() - t < 250000000ull;
}

static _Atomic uint32_t a_asked_ms[ACT_COUNT];  // when the game last asked about each action
bool act_asked_within(int id, unsigned ms) {
    return id >= 0 && id < ACT_COUNT && a_asked_ms[id] && (uint32_t)SDL_GetTicks() - a_asked_ms[id] < ms;
}

int rt_act_query(int kind, uint64_t pad, uint64_t id, uint64_t lr, uint64_t masks) {
    if (id == 38) cycle_asked_ns = SDL_GetTicksNS();
    if (id < ACT_COUNT) a_asked_ms[id] = (uint32_t)SDL_GetTicks() | 1;
    if (!k_masks && masks) { k_masks = (const uint32_t *)(uintptr_t)masks; act_refresh(); }
    if (id < ACT_COUNT && k_masks && (k_masks[id] != k_masks_190[id]) != ((a_mask_diff >> id & 1) != 0))
        act_refresh();
    bool custom = id < ACT_COUNT && (a_custom >> id & 1);
    if (act_log_on()) act_log_query(kind, pad, (unsigned)id, lr, custom && a_live);
    if (id >= ACT_COUNT || !a_live || !custom) return -1;
    int r;
    if (kind) {
        a_served = true;
        r = (int)(a_trig >> id & 1);
    } else {
        r = (int)(a_hold >> id & 1);
    }
    if (r && shares_interact((int)id) && SDL_GetTicks() - a_prompt_ms < PROMPT_GRACE_MS)
        r = 0;  // (the prompt's button answers the prompt)
    if (r && act_log_on()) l_true[kind] |= 1ull << id;
    return r;
}

// ---- savagewind.ini: act_key_<id> / act_pad_<id> = <key | button | none>, written only when not the default
static bool act_settings_load(const char *key, const char *val) {
    if (!strcmp(key, "act_schema")) {
        a_schema = atoi(val) >= 2;
        act_refresh();
        return true;
    }
    bool pad2 = !strncmp(key, "act_pad2_", 9), kb2 = !strncmp(key, "act_key2_", 9);
    bool pad = !strncmp(key, "act_pad_", 8), kb = !strncmp(key, "act_key_", 8);
    if (!pad && !kb && !pad2 && !kb2) return false;
    const char *num = key + (pad2 || kb2 ? 9 : 8);
    char *end;
    long id = strtol(num, &end, 10);
    if (end == num || *end || id < 0 || id >= ACT_COUNT) return true;
    int host = ACT_FOLLOW;
    if (!strcmp(val, "none")) host = ACT_NONE;
    else if (pad || pad2) {
        int h = bind_pad_from_name(val);
        if (h >= 0) host = h;
    } else {
        int sc = (int)SDL_GetScancodeFromName(val);
        for (int m = 0; m < 5; m++)
            if (!strcmp(val, k_mouse[m])) sc = BIND_MOUSE + 1 + m;
        if (sc != SDL_SCANCODE_UNKNOWN) host = sc;
    }
    if (pad2 || kb2) act_set2(pad2 ? BIND_DEV_PAD : BIND_DEV_KEY, (int)id, host);
    else if (host != ACT_FOLLOW) act_set(pad ? BIND_DEV_PAD : BIND_DEV_KEY, (int)id, host);
    return true;
}

static void act_settings_save(FILE *f) {
    // This must precede every raw bind_* line: missing action lines now mean factory defaults, not migration.
    fprintf(f, "act_schema = 2\n");
    for (int i = 0; i < ACT_COUNT; i++) {
        int k = g_act_key[i], p = g_act_pad[i];
        int k2 = g_act_key2[i], p2 = g_act_pad2[i];
        if (k != act_default(BIND_DEV_KEY, i)) fprintf(f, "act_key_%d = %s\n", i, k == ACT_NONE ? "none" : bind_key_label(k));
        if (p != act_default(BIND_DEV_PAD, i) && (p == ACT_NONE || (p >= 0 && p < PADB_HOST_COUNT)))
            fprintf(f, "act_pad_%d = %s\n", i, p == ACT_NONE ? "none" : k_pad_ini[p]);
        if (k2 > 0) fprintf(f, "act_key2_%d = %s\n", i, bind_key_label(k2));
        if (p2 >= 0 && p2 < PADB_HOST_COUNT) fprintf(f, "act_pad2_%d = %s\n", i, k_pad_ini[p2]);
    }
}
