// Control remapping: tables runtime/hle/input.c reads on every input event (so a change applies at once), the
// capture used by the "click a binding, then press" UI, and their savagewind.ini lines. See runtime/input_bind.h.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"

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
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN}
#define DEF_KEY { \
    SDL_SCANCODE_E, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, BIND_MOUSE + SDL_BUTTON_LEFT, SDL_SCANCODE_LCTRL, \
    BIND_MOUSE + SDL_BUTTON_MIDDLE, SDL_SCANCODE_Q, SDL_SCANCODE_R, BIND_MOUSE + SDL_BUTTON_RIGHT, SDL_SCANCODE_F, \
    SDL_SCANCODE_TAB, SDL_SCANCODE_M, \
    SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_1, SDL_SCANCODE_4, \
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_I, SDL_SCANCODE_K, \
    SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_C, SDL_SCANCODE_T}
static const int16_t k_def_pad[BIND_PAD_COUNT] = DEF_PAD, k_def_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_pad[BIND_PAD_COUNT] = DEF_PAD, g_bind_key[BIND_KEY_COUNT] = DEF_KEY;

static const char *const k_ini[BIND_KEY_COUNT] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right", "walk", "tilt",
};
static const char *const k_names[BIND_KEY_COUNT] = {
    "A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down", "Left stick Up", "Left stick Down", "Left stick Left",
    "Left stick Right", "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right", "Walk (hold)",
    "Tilt with the mouse (hold)",
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

const char *bind_input_name(int slot) { return slot >= 0 && slot < BIND_KEY_COUNT ? k_names[slot] : "?"; }

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
    if (host < 0 || host >= PADB_HOST_COUNT) return "(none)";
    return fam == 1 ? xb[host] : fam == 2 ? ps[host] : nin[host];
}

static const char *const k_mouse[5] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse Back", "Mouse Forward"};

const char *bind_key_label(int sc) {
    if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) return k_mouse[sc - BIND_MOUSE - 1];
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "(unnamed key)";
}

int bind_label_family(void) {
    extern int input_pad_family(void);  // runtime/hle/input.c (the game's open pad, or SAVAGEWIND_PAD_TYPE)
    int f = input_pad_family();
    if (f) return f;
    int n = 0, t = 0;  // start screen: input.c hasn't opened a pad yet
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 1 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 2 : 0;
}

static void result(int kind, int dev, int slot, int other);

void bind_reset(int dev) {
    if (dev == BIND_DEV_PAD) memcpy(g_bind_pad, k_def_pad, sizeof g_bind_pad);
    else memcpy(g_bind_key, k_def_key, sizeof g_bind_key);
    LOG("[input] %s bindings reset to the defaults\n", dev == BIND_DEV_PAD ? "controller" : "keyboard");
    result(5, dev, -1, -1);
}

bool bind_is_default(int dev) {
    return dev == BIND_DEV_PAD ? !memcmp(g_bind_pad, k_def_pad, sizeof g_bind_pad) : !memcmp(g_bind_key, k_def_key, sizeof g_bind_key);
}

bool bind_is_default_slot(int dev, int slot) {
    return dev == BIND_DEV_PAD ? g_bind_pad[slot] == k_def_pad[slot] : g_bind_key[slot] == k_def_key[slot];
}


// ------------------------------------------------------------------ capture
static _Atomic int cap_dev = -1, cap_slot = -1;
static _Atomic uint64_t cap_deadline;
static _Atomic int res_kind, res_dev, res_slot, res_other, res_serial;

static void result(int kind, int dev, int slot, int other) {
    res_kind = kind, res_dev = dev, res_slot = slot, res_other = other;
    res_serial++;
}

void bind_capture_begin(int dev, int slot) {
    if (slot < 0 || slot >= (dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)) return;
    cap_deadline = SDL_GetTicks() + BIND_CAPTURE_MS;
    cap_slot = slot;
    cap_dev = dev;
}

void bind_capture_cancel(void) {
    if (cap_dev < 0) return;
    result(3, cap_dev, cap_slot, -1);
    cap_dev = -1;
}

bool bind_capture_active(int *dev, int *slot, int *ms_left) {
    int d = cap_dev;
    if (d >= 0 && SDL_GetTicks() >= cap_deadline) { result(4, d, cap_slot, -1); cap_dev = d = -1; }
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

// Bind `host` to `slot`; an input already on `host` takes the slot's old binding (swap).
static void assign(int dev, int slot, int host) {
    int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, other = -1;
    for (int i = 0; i < n; i++)
        if (i != slot && t[i] == host) { other = i; t[i] = t[slot]; }
    t[slot] = (int16_t)host;
    LOG("[input] %s %s -> %s%s%s%s\n", dev == BIND_DEV_PAD ? "controller" : "keyboard", k_names[slot],
        dev == BIND_DEV_PAD ? k_pad_ini[host] : bind_key_label(host), other >= 0 ? " (swapped with " : "",
        other >= 0 ? k_names[other] : "", other >= 0 ? ")" : "");
    if (other >= 0) LOG("[input]   %s -> %s\n", k_names[other], dev == BIND_DEV_PAD ? k_pad_ini[t[other]] : bind_key_label(t[other]));
    result(other >= 0 ? 2 : 1, dev, slot, other);
}

bool bind_capture_event(const SDL_Event *e) {
    int dev = cap_dev;
    if (dev < 0) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) return true;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { bind_capture_cancel(); return true; }
        // F1 (menu), F9 (frame dump), F11 (fullscreen) stay the overlay's; a controller capture ignores keys
        if (dev != BIND_DEV_KEY || e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_F9 ||
            e->key.scancode == SDL_SCANCODE_F11 || e->key.scancode >= SDL_SCANCODE_COUNT) return true;
        cap_dev = -1;
        assign(dev, cap_slot, e->key.scancode);
        return true;
    // Keep host held-state current even while capturing. Swallowing the release of the confirm key / button left
    // it down in input.c, so ending a capture pressed confirm again and reopened the binding row (#175).
    case SDL_EVENT_KEY_UP:
        return false;
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:  // a keyboard binding can be a mouse button (a controller capture ignores it)
        if (dev != BIND_DEV_KEY || e->button.button < SDL_BUTTON_LEFT || e->button.button > SDL_BUTTON_X2) return true;
        cap_dev = -1;
        assign(dev, cap_slot, BIND_MOUSE + e->button.button);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Home / Guide opens the menu; a keyboard capture ignores the controller (it times out, or Esc)
        if (dev != BIND_DEV_PAD || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) return true;
        cap_dev = -1;
        assign(dev, cap_slot, e->gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return false;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            e->gaxis.value <= 16000) return false;  // a trigger may also have been bound to confirm
        if (dev == BIND_DEV_PAD && e->gaxis.value > 16000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            cap_dev = -1;
            assign(dev, cap_slot, e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER);
        }
        return true;  // (sticks: no menu movement while waiting)
    default:
        return false;
    }
}

// ------------------------------------------------------------------ savagewind.ini
bool bind_settings_load(const char *key, const char *val) {
    bool pad = !strncmp(key, "bind_pad_", 9), kb = !strncmp(key, "bind_key_", 9);
    if (!pad && !kb) return false;
    for (int i = 0; i < (pad ? BIND_PAD_COUNT : BIND_KEY_COUNT); i++) {
        if (strcmp(key + 9, k_ini[i])) continue;
        if (pad) {
            for (int h = 0; h < PADB_HOST_COUNT; h++)
                if (!strcmp(val, k_pad_ini[h])) g_bind_pad[i] = (int16_t)h;
        } else {
            int sc = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : SDL_SCANCODE_UNKNOWN;
            for (int m = 0; m < 5; m++)
                if (!strcmp(val, k_mouse[m])) sc = BIND_MOUSE + 1 + m;
            if (sc != SDL_SCANCODE_UNKNOWN || !strcmp(val, "none")) g_bind_key[i] = (int16_t)sc;
        }
    }
    return true;
}

void bind_settings_save(FILE *f) {
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] != k_def_pad[i] && g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT)
            fprintf(f, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[g_bind_pad[i]]);
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (g_bind_key[i] != k_def_key[i])
            fprintf(f, "bind_key_%s = %s\n", k_ini[i], g_bind_key[i] ? bind_key_label(g_bind_key[i]) : "none");
}
