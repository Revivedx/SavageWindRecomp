// Savage Wind's settings as pages of the game's own Options screen (pause menu > System > Options): the game draws
// them, with its font, sounds and cursor. The first page has one button per section (Gameplay: the game's own
// options; Video, Audio, Controls: the game's camera options and the mouse; Keyboard, Controller, Language) and a
// Quit button; A on a section shows its page, B goes back. The screen opens from the pause menu (System > Options) and
// from the title screen menu (Options); this replaces the overlay menu in game (menu.mm). Quit (here, and in place of
// the title menu's amiibo settings) asks with the game's own dialog: desktop or the Savage Wind start screen.
//
// The game side (tools/patches.py): the screen builds one row per entry of a static option table each time it opens
// (entries through 0x7001cccda0 / count 0x7001cccd98, row order through 0x7001cccdb0 / count 0x7001cccda8). The
// first time it opens, the table is pointed at a copy with this file's entries after the game's 14; each opening then
// gets the row order of the page asked for. Rows of these entries have their value, texts and A handled here. A page
// change closes the screen; the title menu's update (rt_opt_title_tick) or the pause System page's Options state
// (rt_opt_pause_tick) opens it again.
//
// Threads: every hook runs on the game's thread, inside the screen's own functions.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "i18n.h"

extern "C" {
#include "../input_bind.h"
#include "../language.h"
#include "../nvn/output_scale.h"
#include "../settings.h"
#include "../window_size.h"
void rt_request_reboot(int kind);           // runtime/main.c (1: start the game again)
void menu_request_window_apply(void);       // menu.mm (fullscreen / window size, applied by the window loop)
bool renderer_gpu_rtx(void);                // vulkan/device.c, metal.m
}

namespace {

// ---- guest memory (main is mapped at its runtime address: plain pointers)
uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
void wr8(uint64_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
void wr64(uint64_t a, uint64_t v) { *(volatile uint64_t *)(uintptr_t)a = v; }

const uint64_t OPT_COUNT = 0x7001cccd98, OPT_ENTRIES = 0x7001cccda0, OPT_ROWS = 0x7001cccda8, OPT_ORDER = 0x7001cccdb0;
const uint64_t SCREEN_MGR = 0x7001cd5c38;  // ScreenMgr *: +0x28 screen count, +0x30 screen pointers by id
const uint32_t SCREEN_OPTIONS = 0x34;
const uint32_t MENU_OPTIONS_ACTIVE = 0x6a;  // title menu +0x363c while its Options button's screen is up
const uint32_t SCREEN_DIALOG = 0x39, TITLE_AMIIBO_BUTTON = 0x6c;
const uint64_t DIALOG_ANSWER = 0x7001c4b308;  // the dialog's answer (FUN_7100d46068 takes it)
const uint32_t DIALOG_NO_ANSWER = 0xd;
const int GAME_ENTRIES = 14;
// the game's entries (game data flag names): 1 CameraUpDownReverse, 2 CameraRLReverse, 7 StickSensitivity,
// 9 AmiiboItemOnOff, 11 / 12 VoiceLanguage (a button or a level, one per platform), 13 DisplaySpecialMode (Toy-Con
// VR Goggles)
const uint32_t GAME_CAMERA_Y = 1, GAME_CAMERA_X = 2, GAME_CAMERA_SENSITIVITY = 7, GAME_AMIIBO = 9,
               GAME_VOICE_BUTTON = 11, GAME_VOICE_LEVEL = 12, GAME_VR = 13;
const uint32_t MSG_BASE = 9000;  // this file's message ids: MSG_BASE + item * 256 + part (the game's are 0000-0100)
enum { MSG_TITLE = 0, MSG_DESC = 1, MSG_BUTTON = 2, MSG_VALUE = 16 };
enum { MSG_TITLE_QUIT = 1, MSG_QUIT_QUESTION, MSG_QUIT_DESKTOP, MSG_QUIT_START };  // (storage ids below MSG_BASE)

// Row object (0x98 bytes, FUN_7100d1d828): +0x00 screen, +0x50 entry, +0x54 on/off, +0x58 value, +0x5c max,
// +0x60 min, +0x64 button state, +0x68 flag type
struct Entry {           // the game's option entry (0x30)
    uint64_t name_vtbl;  // sead::SafeString: the game data flag name
    uint64_t name;
    uint32_t kind;       // KIND_*
    uint32_t desc, title, first_value, last_value, button_off, button_on, pad;
};
static_assert(sizeof(Entry) == 0x30);
enum { KIND_TOGGLE = 0, KIND_CHOICE = 1, KIND_BUTTON = 2 };

enum Page { P_MAIN, P_GAMEPLAY, P_VIDEO, P_AUDIO, P_CONTROLS, P_KEYS, P_STICKS, P_PAD, P_LANGUAGE, P_COUNT };
enum Action { A_NONE, A_PAGE, A_BIND_KEY, A_BIND_PAD, A_RESET_KEYS, A_RESET_PAD, A_RESTART, A_QUIT };

struct Item {
    int page;
    int kind;
    const char *title, *desc;
    const char *button;                          // KIND_BUTTON: its text (NULL: label(item, -1))
    int (*count)(const Item &);                  // KIND_CHOICE: number of values
    const char *(*label)(const Item &, int v);   // KIND_CHOICE: value v's text
    int (*get)(const Item &);
    void (*set)(const Item &, int v);
    int def;                                     // the default value ("reset to defaults")
    int action, arg;                             // KIND_BUTTON
    const char *(*desc_fn)(const Item &);        // a description made when shown (NULL: desc)
    void (*reset)(const Item &);                 // "reset to defaults" when it isn't set(def)
    bool (*value_off)(const Item &, int v);      // KIND_CHOICE: value v can't be picked yet (shown greyed)
};

std::vector<Item> g_items;
std::vector<Entry> g_entries;
std::vector<uint32_t> g_order[P_COUNT];
std::vector<std::string> g_names;  // the entries' flag names (never a real game data flag)
uint64_t g_installed;              // address of g_entries while the game uses it
int g_cur_page = P_MAIN, g_next_page = P_MAIN;
bool g_reopen;                     // a page change closed the screen: open it again once it has gone
bool g_switching;                  // ... and its next opening shows g_next_page (any other opening: the first page)
uint64_t g_quit_dialog;            // the quit question's dialog while it is up
uint64_t g_quit_owner;             // ... and the screen it came from (title menu or Options; its input is off meanwhile)
bool g_quit_texts;                 // ... and its texts are still to be given (its two-button layout comes next)
uint64_t g_cap_row;                // a binding row whose capture is running
std::vector<uint64_t> g_dirty;     // rows to redraw, one per frame
std::chrono::steady_clock::time_point g_hold_until;
std::unordered_map<uint32_t, std::u16string> g_text;

// ---- values
const char *const k_perf[] = {"Off", "FPS", "Full"};
// Upscaler: Off (the game's image scaled plainly), this OS's spatial upscaler, and the temporal ones still to come. A
// value that can't be picked yet (Item.value_off) is shown greyed and never stored: g_shown keeps the row on it until
// the page opens again. So are the upscaler's modes until the game renders below the window's size (output_scale.c).
#ifdef __APPLE__
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_METALFX};
#else
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS};
#endif
#ifdef __APPLE__
const int UPS_DEFAULT = 1;  // (UPSCALE_DEFAULT: MetalFX)
#else
const int UPS_DEFAULT = 0;  // (UPSCALE_DEFAULT: Off, the window's resolution)
#endif
std::vector<int> g_shown;   // per item: the value shown instead of get(), one that can't be picked yet (-1: none)
const char16_t GREY = 3;    // (the game's message colour for greyed values)
// (the modes in upscale_mode_order: from the sharpest)
const char *const k_modes[UPQ_COUNT] = {"Ultra Quality", "Quality", "Balanced", "Performance", "Native AA", "Ultra Performance"};
const char *const k_aa[] = {"Off", "SMAA", "TAA"};
constexpr int k_fps[] = FPS_CHOICES;  // frame rate (0: Off, no limit)
constexpr int FPS_DEFAULT_INDEX = 2;
static_assert(k_fps[FPS_DEFAULT_INDEX] == FPS_DEFAULT);
constexpr int k_sens[] = {10, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000};  // mouse sensitivity, %
constexpr int SENS_DEFAULT = 9;  // (300%: MOUSE_SENS_DEFAULT)
static_assert(k_sens[SENS_DEFAULT] == 300);
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

int nearest(const int *v, int n, float x) {
    int best = 0;
    for (int i = 1; i < n; i++)
        if (fabsf(v[i] - x) < fabsf(v[best] - x)) best = i;
    return best;
}

std::string pct(int v) { return std::to_string(v) + "%"; }

Item toggle(int page, const char *title, const char *desc, int (*get)(const Item &), void (*set)(const Item &, int),
            int def) {
    Item it = {};
    it.page = page, it.kind = KIND_TOGGLE, it.title = title, it.desc = desc, it.get = get, it.set = set, it.def = def;
    return it;
}

Item choice(int page, const char *title, const char *desc, int (*count)(const Item &),
            const char *(*label)(const Item &, int), int (*get)(const Item &), void (*set)(const Item &, int), int def) {
    Item it = toggle(page, title, desc, get, set, def);
    it.kind = KIND_CHOICE, it.count = count, it.label = label;
    return it;
}

Item button(int page, const char *title, const char *desc, const char *text, int action, int arg) {
    Item it = {};
    it.page = page, it.kind = KIND_BUTTON, it.title = title, it.desc = desc, it.button = text, it.action = action;
    it.arg = arg;
    return it;
}

// Window size: the sizes offered now (runtime/window_size.c; fullscreen can offer more)
int winsize_items(WinSize *list) { return winsize_list(list, g_settings.fullscreen); }
int winsize_index(WinSize s) {
    WinSize list[WINSIZE_MAX];
    int n = winsize_items(list);
    for (int i = 0; i < n; i++)
        if (list[i].w == s.w && list[i].h == s.h) return i;
    return 0;
}

// A value that can't be picked yet: its name and why ("(coming soon)")
const char *soon(const char *name, const char *why = "(coming soon)") {
    static std::string s[8];
    static int i;
    std::string &o = s[i++ & 7];
    o = std::string(tr(name)) + " " + tr(why);
    return o.c_str();
}

const char *ups_label(const Item &, int x) {
    int u = k_ups_value[x];
    if (u == UPSCALE_OFF) return tr("Off");
    if (u == UPSCALE_METALFX) return tr("MetalFX");
    if (u == UPSCALE_FSR1) return tr("AMD FSR 1");
    if (u == UPSCALE_FSR3) return upscaler_available(UPSCALE_FSR3) ? tr("AMD FSR 3.1") : soon("AMD FSR 3.1", "(not available)");
    return soon("DLSS", renderer_gpu_rtx() ? "(coming soon)" : "(NVIDIA RTX only)");
}

const char *fps_label(const Item &, int x) {
    static char buf[32];
    if (!k_fps[x]) return tr("Off");
    snprintf(buf, sizeof buf, tr("%d FPS"), k_fps[x]);
    return buf;
}

const char *bind_label(const Item &it, int) {
    int dev, slot, ms;
    bool key = it.action == A_BIND_KEY;
    if (bind_capture_active(&dev, &slot, &ms) && dev == (key ? BIND_DEV_KEY : BIND_DEV_PAD) && slot == it.arg)
        return tr(key ? "Press a key..." : "Press a button...");
    return tr(key ? bind_key_label(g_bind_key[it.arg]) : bind_pad_label(g_bind_pad[it.arg], bind_label_family()));
}

void build_items(void) {
    std::vector<Item> &v = g_items;
    // first page: the sections (the game's own options are on Gameplay and Controls)
    v.push_back(button(P_MAIN, "Gameplay", "Mini-map, HUD, jump button and more.", "Open", A_PAGE, P_GAMEPLAY));
    v.push_back(button(P_MAIN, "Video", "Screen, resolution and image quality.", "Open", A_PAGE, P_VIDEO));
    v.push_back(button(P_MAIN, "Audio", "Volume and sound effects.", "Open", A_PAGE, P_AUDIO));
    v.push_back(button(P_MAIN, "Controls", "Camera, d-pad and mouse.", "Open", A_PAGE, P_CONTROLS));
    v.push_back(button(P_MAIN, "Keyboard (buttons)", "Which key does each button.", "Open", A_PAGE, P_KEYS));
    v.push_back(button(P_MAIN, "Keyboard (sticks)", "Keys for the sticks and for walking.", "Open", A_PAGE, P_STICKS));
    v.push_back(button(P_MAIN, "Controller", "Which controller button does each button.", "Open", A_PAGE, P_PAD));
    v.push_back(button(P_MAIN, "Language", "Language of the texts and of the voices.", "Open", A_PAGE, P_LANGUAGE));
    v.push_back(button(P_MAIN, "Quit game", "To the desktop, or back to the Savage Wind start screen.", "Quit",
                       A_QUIT, 0));

    // Video (fullscreen and the window size are applied by the window loop: menu.mm)
    v.push_back(toggle(P_VIDEO, "Fullscreen", "Fills the screen. Also F11 or Alt+Enter.",
                       [](const Item &) { return (int)g_settings.fullscreen; },
                       [](const Item &, int x) { g_settings.fullscreen = x; menu_request_window_apply(); }, 0));
    Item ws = choice(P_VIDEO, "Window size", "",
                     [](const Item &) {
                         WinSize list[WINSIZE_MAX];
                         return winsize_items(list);
                     },
                     [](const Item &, int x) {
                         static char buf[48];
                         WinSize list[WINSIZE_MAX];
                         int n = winsize_items(list);
                         winsize_name(list[x < n ? x : 0], buf, sizeof buf);
                         return (const char *)buf;
                     },
                     [](const Item &) { return winsize_index(winsize_effective(g_settings.fullscreen)); },
                     [](const Item &, int x) {
                         WinSize list[WINSIZE_MAX];
                         int n = winsize_items(list);
                         if (x >= n) return;
                         g_settings.window_w = list[x].w, g_settings.window_h = list[x].h;
                         menu_request_window_apply();
                     }, 0);
    ws.desc_fn = [](const Item &) {
        static char buf[256];
        WinSize d = winsize_desktop();
        snprintf(buf, sizeof buf, tr("Window or fullscreen size. Your display: %d x %d."), d.w, d.h);
        return (const char *)buf;
    };
    ws.reset = [](const Item &) {  // (unset: 1280 x 720 in a window, the desktop in fullscreen)
        g_settings.window_w = g_settings.window_h = 0;
        menu_request_window_apply();
    };
    v.push_back(ws);
    Item fps = choice(P_VIDEO, "Frame rate", "",
                      [](const Item &) { return N(k_fps); }, fps_label,
                      [](const Item &) {
                          for (int i = 0; i < N(k_fps); i++)
                              if (k_fps[i] == g_settings.fps_limit) return i;
                          return FPS_DEFAULT_INDEX;
                      },
                      [](const Item &, int x) { g_settings.fps_limit = k_fps[x]; }, FPS_DEFAULT_INDEX);
    fps.desc_fn = [](const Item &) {  // (one line)
        static char buf[256];
        int hz = (int)lroundf(winsize_refresh_rate());
        if (hz > 0) snprintf(buf, sizeof buf, tr("Game speed stays the same at every rate. Your display: %d Hz."), hz);
        else snprintf(buf, sizeof buf, "%s", tr("Game speed stays the same at every rate."));
        return (const char *)buf;
    };
    v.push_back(fps);
    v.push_back(choice(P_VIDEO, "Performance overlay", "Frame rate and system load in a corner. Also F10.",
                       [](const Item &) { return N(k_perf); }, [](const Item &, int x) { return tr(k_perf[x]); },
                       [](const Item &) { return g_settings.perf_overlay; },
                       [](const Item &, int x) { g_settings.perf_overlay = x; }, PERF_OFF));
    v.push_back(choice(P_VIDEO, "Upscaler", "Off: full resolution. FSR: renders smaller, then upscales (F12).",
                       [](const Item &) { return N(k_ups_value); }, ups_label,
                       [](const Item &) {
                           for (int i = 0; i < N(k_ups_value); i++)
                               if (k_ups_value[i] == g_settings.upscaler) return i;
                           return 0;
                       },
                       [](const Item &, int x) { g_settings.upscaler = k_ups_value[x]; }, UPS_DEFAULT));
    v.back().value_off = [](const Item &, int x) { return !upscaler_available(k_ups_value[x]); };
    Item mode = choice(P_VIDEO, "Upscaler mode", "",
                       [](const Item &) { return N(k_modes); },
                       [](const Item &, int x) {
                           int q = upscale_mode_order[x];
                           return !upscale_modes_available() ? soon(k_modes[q])
                                : !upscale_mode_offered(g_settings.upscaler, q) ? soon(k_modes[q], "(FSR 3.1 only)") : tr(k_modes[q]);
                       },
                       [](const Item &) {
                           for (int i = 0; i < UPQ_COUNT; i++)
                               if (upscale_mode_order[i] == g_settings.upscale_quality) return i;
                           return 2;
                       },
                       [](const Item &, int x) { g_settings.upscale_quality = upscale_mode_order[x]; }, 2);
    mode.value_off = [](const Item &, int x) {
        return !upscale_modes_available() || !upscale_mode_offered(g_settings.upscaler, upscale_mode_order[x]);
    };
    mode.desc_fn = [](const Item &) {
        // (one line: the game's description pane is one English line wide)
        return upscale_modes_available() ? tr("Native AA is the sharpest, Ultra Performance the fastest.")
                                         : tr("Renders smaller, then upscales. Coming in a later version.");
    };
    v.push_back(mode);
    v.push_back(choice(P_VIDEO, "Anti-aliasing", "Smooths jagged edges. TAA: temporal (FSR 3.1 has its own).",
                       [](const Item &) { return N(k_aa); }, [](const Item &, int x) { return tr(k_aa[x]); },
                       [](const Item &) { return g_settings.antialias; },
                       [](const Item &, int x) { g_settings.antialias = x; }, AA_OFF));
    v.push_back(toggle(P_VIDEO, "Asynchronous shaders", "On: no stutter, but some effects appear late.",
                       [](const Item &) { return (int)g_settings.async_shaders; },
                       [](const Item &, int x) { g_settings.async_shaders = x; }, 0));

    // Audio
    v.push_back(choice(P_AUDIO, "Volume", "The game's volume. 100% is its own level.",
                       [](const Item &) { return 21; },
                       [](const Item &, int x) {
                           static std::string s;
                           s = pct(x * 10);
                           return s.c_str();
                       },
                       [](const Item &) { return (int)lroundf(g_settings.volume * 10); },
                       [](const Item &, int x) { g_settings.volume = x / 10.0f; }, 10));
    v.push_back(toggle(P_AUDIO, "Room reverb", "Echo in caves, shrines and big rooms.",
                       [](const Item &) { return (int)g_settings.reverb; },
                       [](const Item &, int x) { g_settings.reverb = x; }, 1));
    v.push_back(toggle(P_AUDIO, "Movie audio", "Sound in the game's movies.",
                       [](const Item &) { return (int)g_settings.movie_audio; },
                       [](const Item &, int x) { g_settings.movie_audio = x; }, 1));

    // Controls
    v.push_back(toggle(P_CONTROLS, "D-pad as left stick", "The d-pad also moves like the left stick.",
                       [](const Item &) { return (int)g_settings.dpad_move; },
                       [](const Item &, int x) { g_settings.dpad_move = x; }, 0));
    v.push_back(toggle(P_CONTROLS, "Mouse camera", "The mouse turns the camera. Esc frees the cursor.",
                       [](const Item &) { return (int)g_settings.mouse_camera; },
                       [](const Item &, int x) { g_settings.mouse_camera = x; }, 1));
    v.push_back(choice(P_CONTROLS, "Mouse sensitivity", "How fast the mouse turns the camera.",
                       [](const Item &) { return N(k_sens); },
                       [](const Item &, int x) {
                           static std::string s;
                           s = pct(k_sens[x]);
                           return s.c_str();
                       },
                       [](const Item &) { return nearest(k_sens, N(k_sens), g_settings.mouse_sens * 100); },
                       [](const Item &, int x) { g_settings.mouse_sens = k_sens[x] / 100.0f; }, SENS_DEFAULT));
    v.push_back(toggle(P_CONTROLS, "Invert mouse Y", "Only the mouse. The camera Y-axis option inverts both.",
                       [](const Item &) { return (int)g_settings.mouse_invert_y; },
                       [](const Item &, int x) { g_settings.mouse_invert_y = x; }, 0));

    // Keyboard and controller: one button per Switch input, showing what drives it; A waits for the new one
    auto bind = [&](int page, int action, int slot) {
        Item it = button(page, bind_input_name(slot),
                         action == A_BIND_KEY ? "Press A, then the new key. Esc cancels."
                                              : "Press A, then the new button.",
                         nullptr, action, slot);
        it.label = bind_label;
        v.push_back(it);
    };
    for (int s = 0; s < BIND_PAD_COUNT; s++) bind(P_KEYS, A_BIND_KEY, s);
    bind(P_KEYS, A_BIND_KEY, BIND_WALK);
    v.push_back(button(P_KEYS, "Reset keyboard to defaults", "Every key back to its default.", "Reset", A_RESET_KEYS, 0));
    for (int s = BIND_LS_UP; s <= BIND_RS_RIGHT; s++) bind(P_STICKS, A_BIND_KEY, s);
    v.push_back(button(P_STICKS, "Reset keyboard to defaults", "Every key back to its default.", "Reset", A_RESET_KEYS, 0));
    for (int s = 0; s < BIND_PAD_COUNT; s++) bind(P_PAD, A_BIND_PAD, s);
    v.push_back(button(P_PAD, "Reset controller to defaults", "Every button back to its default.", "Reset", A_RESET_PAD, 0));

    // Language
    v.push_back(choice(P_LANGUAGE, "Text", "Texts and menus. Applies after a restart.",
                       [](const Item &) { return g_ntext_languages; },
                       [](const Item &, int x) { return tr(g_text_languages[x].name); },
                       [](const Item &) { return g_settings.text_language; },
                       [](const Item &, int x) { g_settings.text_language = x; }, 0));
    v.push_back(choice(P_LANGUAGE, "Voices", "The characters' voices. Applies after a restart.",
                       [](const Item &) { return g_nvoice_languages; },
                       [](const Item &, int x) { return tr(g_voice_languages[x].name); },
                       [](const Item &) { return g_settings.voice_language; },
                       [](const Item &, int x) { g_settings.voice_language = x; }, 0));
    v.push_back(button(P_LANGUAGE, "Restart now",
                       "Starts the game again with these languages. Unsaved progress is lost.", "Restart", A_RESTART, 0));
}

// The game's table, extended (first opening, and again after a reboot reloads the game's own)
bool install(void) {
    uint64_t ent = rd64(OPT_ENTRIES);
    if (g_installed && ent == g_installed) return true;
    uint32_t count = rd32(OPT_COUNT), rows = rd32(OPT_ROWS);
    uint64_t order = rd64(OPT_ORDER);
    if (count != GAME_ENTRIES || !ent || !order || rows == 0 || rows > 32) {
        fprintf(stderr, "[options] unexpected option table (%u entries, %u rows): Savage Wind's pages left out\n",
                count, rows);
        g_installed = 0;
        return false;
    }
    if (g_items.empty()) build_items();
    g_entries.assign((const Entry *)(uintptr_t)ent, (const Entry *)(uintptr_t)ent + count);
    g_names.clear();
    g_names.reserve(g_items.size());
    for (size_t i = 0; i < g_items.size(); i++) g_names.push_back("sw_option_" + std::to_string(i));
    for (size_t i = 0; i < g_items.size(); i++) {
        const Item &it = g_items[i];
        uint32_t m = MSG_BASE + (uint32_t)i * 256;
        Entry e = {};
        e.name_vtbl = g_entries[0].name_vtbl;
        e.name = (uint64_t)(uintptr_t)g_names[i].c_str();
        e.kind = (uint32_t)it.kind;
        e.title = m + MSG_TITLE, e.desc = m + MSG_DESC;
        e.first_value = m + MSG_VALUE;
        e.last_value = m + MSG_VALUE + (it.kind == KIND_CHOICE ? it.count(it) - 1 : 1);
        e.button_off = e.button_on = m + MSG_BUTTON;
        g_entries.push_back(e);
    }
    for (auto &o : g_order) o.clear();
    // the game's rows: camera ones on Controls; amiibo, the voice language (Language has the voices) and the VR
    // goggles mode (no use on a PC screen) left out; the rest on Gameplay
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t e = ((const uint32_t *)(uintptr_t)order)[r];
        if (e == GAME_AMIIBO || e == GAME_VOICE_BUTTON || e == GAME_VOICE_LEVEL || e == GAME_VR) continue;
        bool camera = e == GAME_CAMERA_Y || e == GAME_CAMERA_X || e == GAME_CAMERA_SENSITIVITY;
        g_order[camera ? P_CONTROLS : P_GAMEPLAY].push_back(e);
    }
    for (size_t i = 0; i < g_items.size(); i++) g_order[g_items[i].page].push_back(GAME_ENTRIES + (uint32_t)i);
    g_installed = (uint64_t)(uintptr_t)g_entries.data();
    wr64(OPT_ENTRIES, g_installed);
    wr32(OPT_COUNT, (uint32_t)g_entries.size());
    fprintf(stderr, "[options] Savage Wind's pages added to the Options screen (%zu entries)\n", g_items.size());
    return true;
}

const Item *item_of_index(uint32_t idx) {
    if (!g_installed || rd64(OPT_ENTRIES) != g_installed) return nullptr;
    if (idx < GAME_ENTRIES || idx >= GAME_ENTRIES + g_items.size()) return nullptr;
    return &g_items[idx - GAME_ENTRIES];
}
const Item *item_of_row(uint64_t row) { return row ? item_of_index(rd32(row + 0x50)) : nullptr; }

int clamp_value(const Item &it, int x) {
    int n = it.kind == KIND_CHOICE ? it.count(it) : 2;
    return x < 0 ? 0 : x >= n ? n - 1 : x;
}

void mark_all_rows(uint64_t screen) {
    if (!screen) return;
    int n = (int)rd32(screen + 0x3698);
    uint64_t rows = rd64(screen + 0x36a0);
    for (int i = 0; i < n && i < 64 && rows; i++)
        if (uint64_t r = rd64(rows + 8 * i)) g_dirty.push_back(r);
}

uint64_t screen_by_id(uint32_t id) {
    uint64_t mgr = rd64(SCREEN_MGR);
    if (!mgr) return 0;
    uint32_t n = rd32(mgr + 0x28);
    uint64_t screens = rd64(mgr + 0x30);
    return screens && n > id ? rd64(screens + 8 * id) : 0;
}
uint64_t options_screen(void) { return screen_by_id(SCREEN_OPTIONS); }

std::u16string utf16(const char *s) {
    std::u16string o;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        unsigned c = *p++;
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (n) c &= 0x3F >> n;
        for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) c = c << 6 | (*p++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            o.push_back((char16_t)(0xD800 + (c >> 10)));
            o.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            o.push_back((char16_t)c);
        }
    }
    return o;
}

// A message for the game ({char16_t *, u32 length with the terminator} at out); id: its storage. grey: in the
// game's grey (its message colour tag: 0x0e, group 0, tag 3, 2 bytes of colour; 0xffff back to the text's own)
void put_text(uint32_t id, const char *s, uint64_t out, bool grey = false) {
    std::u16string &t = g_text[id];
    std::u16string n = utf16(s);
    static const char16_t tag[] = {0x0e, 0, 3, 2};  // (then the colour)
    if (grey) n = std::u16string(tag, 4) + GREY + n + std::u16string(tag, 4) + (char16_t)0xffff;
    if (t != n) t = n;
    wr64(out, (uint64_t)(uintptr_t)t.c_str());
    wr32(out + 8, (uint32_t)t.size() + 1);
}

// The quit question's answer (title menu and Options updates)
void quit_poll(void) {
    if (!g_quit_dialog) return;
    uint32_t a = rd32(DIALOG_ANSWER);
    if (a == 0 || a == 1) {
        wr32(DIALOG_ANSWER, DIALOG_NO_ANSWER);
        g_quit_dialog = 0;
        fprintf(stderr, "[options] quit: %s\n", a == 1 ? "start screen" : "desktop");
        settings_save();
        if (a == 1) {
            rt_request_reboot(2);  // (RT_REBOOT_TITLE: the start screen)
        } else {
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
        }
    } else if (screen_by_id(SCREEN_DIALOG) != g_quit_dialog) {  // (closed with B: the asking screen gets its input back)
        g_quit_dialog = 0;
        if (g_quit_owner && rd64(g_quit_owner + 0x38)) {
            uint64_t in = rd64(g_quit_owner + 0x38) + 0x38;
            wr32(in, rd32(in) | 2u);
        }
        if (g_quit_owner == options_screen()) mark_all_rows(g_quit_owner);  // (the pressed button looks pressed)
        g_quit_owner = 0;
    }
}

}  // namespace

// The game gets no input while a binding capture runs, and briefly after it (the pressed button is still down)
extern "C" bool game_options_input_hold(void) {
    int d, s, ms;
    return bind_capture_active(&d, &s, &ms) || std::chrono::steady_clock::now() < g_hold_until;
}

// createRows: the row list of the page to show
extern "C" void rt_opt_rows(void) {
    if (!install()) return;
    int page = g_switching ? g_next_page : P_MAIN;
    g_switching = false;
    std::vector<uint32_t> &o = g_order[page];
    g_shown.assign(g_items.size(), -1);
    for (size_t i = 0; i < g_items.size(); i++) {  // (how many window sizes there are depends on the display)
        Entry &e = g_entries[GAME_ENTRIES + i];
        if (g_items[i].kind == KIND_CHOICE) e.last_value = e.first_value + g_items[i].count(g_items[i]) - 1;
    }
    wr64(OPT_ORDER, (uint64_t)(uintptr_t)o.data());
    wr32(OPT_ROWS, (uint32_t)o.size());
    g_cur_page = page;
    g_reopen = false;
    g_dirty.clear();
}

// Row init: the flag type of an entry (0 bool, 1 s32); -1: the game's own entry
extern "C" int rt_opt_flag_type(uint64_t entry) {
    if (!g_installed || entry < g_installed) return -1;
    const Item *it = item_of_index((uint32_t)((entry - g_installed) / sizeof(Entry)));
    return it ? (it->kind == KIND_CHOICE ? 1 : 0) : -1;
}

extern "C" int rt_opt_read(uint64_t row) {
    const Item *it = item_of_row(row);
    if (!it) return 0;
    if (it->kind == KIND_BUTTON) {
        wr8(row + 0x64, 0);
        return 1;
    }
    size_t idx = (size_t)(it - g_items.data());
    int x = clamp_value(*it, idx < g_shown.size() && g_shown[idx] >= 0 ? g_shown[idx] : it->get(*it));
    wr8(row + 0x54, x != 0);
    wr32(row + 0x58, (uint32_t)x);
    wr32(row + 0x5c, (uint32_t)(it->kind == KIND_CHOICE ? it->count(*it) - 1 : 1));
    wr32(row + 0x60, 0);
    return 1;
}

extern "C" int rt_opt_store(uint64_t row) {
    const Item *it = item_of_row(row);
    if (!it) return 0;
    size_t idx = (size_t)(it - g_items.data());
    if (it->kind == KIND_TOGGLE) {
        it->set(*it, rd32(row + 0x54) & 0xff ? 1 : 0);
    } else if (it->kind == KIND_CHOICE) {
        int x = clamp_value(*it, (int)rd32(row + 0x58));
        bool off = it->value_off && it->value_off(*it, x);  // (shown, not stored)
        if (idx < g_shown.size()) g_shown[idx] = off ? x : -1;
        if (!off) it->set(*it, x);
        if (getenv("SAVAGEWIND_LOG_OPTIONS"))  // (tests: when each value is stored)
            fprintf(stderr, "[options] %s = %d at %.3f s\n", it->title, x, SDL_GetTicksNS() / 1e9);
    }
    if (it->kind != KIND_BUTTON) settings_save();
    if (it->page == P_VIDEO) mark_all_rows(rd64(row));  // (the sizes offered follow Fullscreen, texts the upscaler)
    return 1;
}

extern "C" int rt_opt_reset(uint64_t row) {
    const Item *it = item_of_row(row);
    if (!it) return 0;
    size_t idx = (size_t)(it - g_items.data());
    if (idx < g_shown.size()) g_shown[idx] = -1;
    if (it->reset) it->reset(*it);
    else if (it->kind != KIND_BUTTON) it->set(*it, it->def);
    else if (it->action == A_BIND_KEY || it->action == A_RESET_KEYS) bind_reset(BIND_DEV_KEY);
    else if (it->action == A_BIND_PAD || it->action == A_RESET_PAD) bind_reset(BIND_DEV_PAD);
    settings_save();
    return 1;
}

// Message w21 into out ({char16_t *, u32 length with the terminator}); 0: the game's own message
extern "C" int rt_opt_msg(uint32_t id, uint64_t out) {
    if (id < MSG_BASE || !g_installed) return 0;
    uint32_t i = (id - MSG_BASE) / 256, part = (id - MSG_BASE) % 256;
    const Item *it = item_of_index(GAME_ENTRIES + i);
    if (!it) return 0;
    const char *s = "";
    bool grey = false;
    if (part == MSG_TITLE) s = tr(it->title);
    else if (part == MSG_DESC) s = it->desc_fn ? it->desc_fn(*it) : tr(it->desc);
    else if (part == MSG_BUTTON) s = it->button ? tr(it->button) : it->label ? it->label(*it, -1) : "";
    else if (part >= MSG_VALUE && it->kind == KIND_CHOICE && (int)(part - MSG_VALUE) < it->count(*it)) {
        s = it->label(*it, (int)(part - MSG_VALUE));
        grey = it->value_off && it->value_off(*it, (int)(part - MSG_VALUE));
    }
    put_text(id, s, out, grey);
    return 1;
}

// A on a row: 0 the game's handling, 1 close the screen (page change), 2 handled, 3 ask whether to quit
extern "C" int rt_opt_decide(uint64_t row) {
    const Item *it = item_of_row(row);
    if (!it || it->kind != KIND_BUTTON) return 0;
    int d, s, ms;
    switch (it->action) {
    case A_PAGE:
        if (bind_capture_active(&d, &s, &ms)) return 2;
        g_next_page = it->arg;
        g_reopen = g_switching = true;
        return 1;
    case A_BIND_KEY:
    case A_BIND_PAD:
        bind_capture_begin(it->action == A_BIND_KEY ? BIND_DEV_KEY : BIND_DEV_PAD, it->arg);
        g_cap_row = row;
        g_dirty.push_back(row);
        return 2;
    case A_RESET_KEYS:
    case A_RESET_PAD:
        bind_reset(it->action == A_RESET_KEYS ? BIND_DEV_KEY : BIND_DEV_PAD);
        settings_save();
        mark_all_rows(rd64(row));
        return 2;
    case A_RESTART:
        if (language_pending()) {
            settings_save();
            rt_request_reboot(1);
        }
        return 2;
    case A_QUIT:
        return 3;
    }
    return 2;
}

// Options update: a binding capture that ended redraws the page's rows (a conflict swaps two inputs)
extern "C" uint64_t rt_opt_tick(uint64_t screen) {
    if (g_quit_dialog) {  // (the quit question is up: 1 skips the screen's update, whose dialog handling takes answers)
        quit_poll();
        if (g_quit_dialog) return 1;
    }
    int d, s, ms;
    if (g_cap_row && !bind_capture_active(&d, &s, &ms)) {
        g_cap_row = 0;
        g_hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        settings_save();
        mark_all_rows(screen);
    }
    if (g_dirty.empty()) return 0;
    uint64_t r = g_dirty.back();
    g_dirty.pop_back();
    return r;
}

// B: from one of these pages back to the first one
extern "C" void rt_opt_back(void) {
    bind_capture_cancel();
    g_cap_row = 0;
    if (g_installed && g_cur_page != P_MAIN) {
        g_next_page = P_MAIN;
        g_reopen = g_switching = true;
    } else {
        g_reopen = g_switching = false;
    }
}

// The title menu's update (menu = the screen): 1 = open the Options screen again now (it has gone, a page is due).
// The quit question's answer is read here too.
extern "C" int rt_opt_title_tick(uint64_t menu) {
    quit_poll();
    if (rd32(menu + 0x363c) != MENU_OPTIONS_ACTIVE) {  // (the Options button isn't active: next opening, first page)
        g_reopen = g_switching = false;
        return 0;
    }
    if (!g_reopen || options_screen()) return 0;
    g_reopen = false;
    return 1;
}

// The pause System page's Options state: 0 as usual, 2 wait (a page change: the screen is closing), 1 open it again
extern "C" int rt_opt_pause_tick(void) {
    if (!g_reopen) return 0;
    if (options_screen()) return 2;
    g_reopen = false;
    return 1;
}

extern "C" uint64_t rt_opt_screen(void) { return options_screen(); }

// ---- Quit: the game's two-button dialog (screen 0x39, type 7) asks: desktop (upper button) or the start screen
// (lower). Its buttons write their answer to 0x7001c4b308 (upper 0x5b: 0, lower 0x5a: 1; FUN_7100d46598) and leave
// the dialog up; B closes it without an answer (rt_quit_dialog_back).

// The title menu's amiibo settings button (Savage Wind has no amiibo support) is Quit
extern "C" int rt_title_event(uint32_t id) { return id == TITLE_AMIIBO_BUTTON; }

// LayoutMsg/Title_00 lookups: "0003" (the amiibo settings button) reads Quit
extern "C" int rt_title_msg(uint64_t label, uint64_t out) {
    const char *l = label ? (const char *)(uintptr_t)rd64(label + 8) : nullptr;
    if (!l || strcmp(l, "0003")) return 0;
    put_text(MSG_TITLE_QUIT, tr("Quit"), out);
    return 1;
}

// owner: the screen asking; as the game's own handlers do after opening a screen, its input goes off ([+0x38]+0x38
// bit 1) until the question goes away
extern "C" uint64_t rt_quit_dialog_begin(uint64_t owner) {
    g_quit_dialog = screen_by_id(SCREEN_DIALOG);
    g_quit_texts = g_quit_dialog != 0;
    wr32(DIALOG_ANSWER, DIALOG_NO_ANSWER);
    if (g_quit_dialog && owner && rd64(owner + 0x38)) {
        g_quit_owner = owner;
        uint64_t in = rd64(owner + 0x38) + 0x38;
        wr32(in, rd32(in) & ~2u);
    }
    return g_quit_dialog;
}

extern "C" void rt_quit_dialog_texts(uint64_t msg, uint64_t left, uint64_t right) {
    if (!g_quit_texts) return;
    g_quit_texts = false;
    put_text(MSG_QUIT_QUESTION, tr("Quit to the desktop, or back to the Savage Wind start screen? Unsaved progress "
                                   "is lost."), msg);
    // (left: the lower button, 0x5a, the cursor's and B's; right: the upper one, 0x5b)
    put_text(MSG_QUIT_START, tr("Start screen"), left);
    put_text(MSG_QUIT_DESKTOP, tr("Desktop"), right);
}

extern "C" int rt_quit_dialog_back(void) { return g_quit_dialog != 0; }
