// Savage Wind's settings as pages of the game's own Options screen (pause menu > System > Options): the game draws
// them, with its font, sounds and cursor. The first page has one button per section (Gameplay: the game's own
// options; Video, Audio, Controls: the game's camera options and the mouse; Keyboard, Controller, Language) and a
// Quit button; A on a section shows its page, B goes back. The screen opens from the pause menu (System > Options) and
// from the title screen menu (Options); this replaces the overlay menu in game (menu.mm). Quit (here, and in place of
// the title menu's amiibo settings) asks with the game's own dialog: desktop or the Savage Wind start screen.
//
// The game side (tools/patches.py): the screen builds one row per entry of a static option table each time it opens
// (g_site.opt_table: its count, entries, row count and row order; 1.8.2 0x7001cccd98..0x7001cccdb0). The
// first time it opens, the table is pointed at a copy with this file's entries after the game's 14; each opening then
// gets the row order of the page asked for. Rows of these entries have their value, texts and A handled here. A page
// change closes the screen; the title menu's update (rt_opt_title_tick) or the pause System page's Options state
// (rt_opt_pause_tick) opens it again.
//
// Threads: every hook runs on the game's thread, inside the screen's own functions.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "i18n.h"

extern "C" {
#include "../achievements.h"
#include "../input_bind.h"
#include "../language.h"
#include "../game_sites.h"
#include "../nvn/output_scale.h"
#include "../settings.h"
#include "../color_grade.h"
#include "../window_size.h"
void rt_request_reboot(int kind);           // runtime/main.c (1: start the game again)
void menu_request_window_apply(void);       // menu.mm (fullscreen / window size, applied by the window loop)
void menu_size_list_open(void);             // menu.mm (Window size's list, over the screen)
bool renderer_gpu_rtx(void);                // vulkan/device.c, metal.m
}

namespace {

// ---- guest memory (main is mapped at its runtime address: plain pointers)
uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
void wr8(uint64_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
void wr64(uint64_t a, uint64_t v) { *(volatile uint64_t *)(uintptr_t)a = v; }

// (the game version's addresses: runtime/game_sites.h)
#define OPT_COUNT (g_site.opt_table)
#define OPT_ENTRIES (g_site.opt_table + 8)
#define OPT_ROWS (g_site.opt_table + 0x10)
#define OPT_ORDER (g_site.opt_table + 0x18)
#define SCREEN_MGR (g_site.screen_mgr)  // ScreenMgr *: +0x28 screen count, +0x30 screen pointers by id
const uint32_t SCREEN_OPTIONS = 0x34;
const uint32_t MENU_OPTIONS_ACTIVE = 0x6a;  // title menu [title_options_state] while its Options button's screen is up
const uint32_t SCREEN_DIALOG = 0x39, TITLE_AMIIBO_BUTTON = 0x6c;
#define DIALOG_ANSWER (g_site.dialog_answer)  // the dialog's answer (1.8.2 FUN_7100d46068 takes it)
const uint32_t DIALOG_NO_ANSWER = 0xd;
const int GAME_ENTRIES = 14;
// the game's entries (game data flag names): 1 CameraUpDownReverse, 2 CameraRLReverse, 4 GyroOnOff (motion aiming),
// 7 StickSensitivity, 9 AmiiboItemOnOff, 11 / 12 VoiceLanguage (a button or a level, one per platform),
// 13 DisplaySpecialMode (Toy-Con VR Goggles)
const uint32_t GAME_CAMERA_Y = 1, GAME_CAMERA_X = 2, GAME_GYRO = 4, GAME_CAMERA_SENSITIVITY = 7, GAME_AMIIBO = 9,
               GAME_VOICE_BUTTON = 11, GAME_VOICE_LEVEL = 12, GAME_VR = 13;
const uint32_t MSG_BASE = 9000;  // this file's message ids: MSG_BASE + item * 256 + part (the game's are 0000-0100)
enum { MSG_TITLE = 0, MSG_DESC = 1, MSG_BUTTON = 2, MSG_VALUE = 16 };
enum { MSG_TITLE_QUIT = 1, MSG_QUIT_QUESTION, MSG_QUIT_DESKTOP, MSG_QUIT_START, MSG_PAUSE_ACH, MSG_ACH_TITLE };  // (storage ids below MSG_BASE)

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

enum Page { P_MAIN, P_GAMEPLAY, P_VIDEO, P_COLOUR, P_AUDIO, P_CONTROLS, P_KEYS, P_STICKS, P_PAD, P_LANGUAGE, P_ACHIEVEMENTS,
            P_COUNT };
enum Action { A_NONE, A_PAGE, A_BIND_KEY, A_BIND_PAD, A_RESET_KEYS, A_RESET_PAD, A_RESTART, A_QUIT, A_SIZE_LIST };

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
    bool wrap;                                   // KIND_CHOICE: a step past either end goes to the other one
};

std::vector<Item> g_items;
std::vector<Entry> g_entries;
std::vector<uint32_t> g_order[P_COUNT];
std::vector<std::string> g_names;  // the entries' flag names (never a real game data flag)
uint64_t g_installed;              // address of g_entries while the game uses it
int g_cur_page = P_MAIN, g_next_page = P_MAIN;
bool g_reopen;                     // a page change closed the screen: open it again once it has gone
bool g_switching;                  // ... and its next opening shows g_next_page (any other opening: the first page)
bool g_ach_direct;                 // the screen was opened on Achievements by the pause menu's button: B closes it
uint64_t g_quit_dialog;            // the quit question's dialog while it is up
uint64_t g_quit_owner;             // ... and the screen it came from (title menu or Options; its input is off meanwhile)
bool g_quit_texts;                 // ... and its texts are still to be given (its two-button layout comes next)
uint64_t g_cap_row;                // a binding row whose capture is running
uint64_t g_size_row;               // the Window size row whose list was opened
std::atomic<bool> g_size_redraw;   // ... and a size was picked there (render thread): redraw it
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
const char16_t RED = 0, GREEN = 1;  // (the game's message colours: an achievement locked / unlocked)
const char *const k_modes[] = {"Ultra Quality", "Quality", "Balanced", "Performance"};
const char *const k_aa[] = {"Off", "SMAA"};
const char *const k_aniso[] = {"Off", "2x", "4x", "8x", "16x"};  // (settings anisotropy 0..4)
const char *const k_shadow[] = {"Console (896)", "High (2048)", "Ultra (4096)", "Extreme (8192)"};  // (SHADOW_*)
const char *const k_icons[] = {"Automatic", "Keyboard", "Xbox", "PlayStation", "Nintendo"};  // (ICONS_*)
constexpr int k_fps[] = FPS_CHOICES;  // frame rate (0: Off, no limit)
const char *const k_vsync[VSYNC_COUNT] = {"On", "Fast", "Adaptive", "Off"};  // (settings.h VSYNC_*)
const char *const k_hdr[HDR_COUNT] = {"Automatic", "On", "Off"};               // (settings.h HDR_*)
constexpr int FPS_DEFAULT_INDEX = 2;
static_assert(k_fps[FPS_DEFAULT_INDEX] == FPS_DEFAULT);
constexpr int k_sens[] = {10, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000};  // mouse sensitivity, %
constexpr int SENS_DEFAULT = 9;  // (300%: MOUSE_SENS_DEFAULT)
static_assert(k_sens[SENS_DEFAULT] == 300);
constexpr int k_sens_y[] = MOUSE_SENS_Y_CHOICES;  // vertical mouse sensitivity, % of the horizontal
constexpr int SENS_Y_DEFAULT = 5;
static_assert(k_sens_y[SENS_Y_DEFAULT] == 100);
constexpr int k_motion_sens[] = {25, 50, 75, 100, 125, 150, 200, 250, 300, 400};  // motion sensitivity, %
constexpr int MOTION_SENS_DEFAULT = 3;
static_assert(k_motion_sens[MOTION_SENS_DEFAULT] == 100);
constexpr int k_fov[] = FOV_CHOICES;  // field of view, % of the game's own
constexpr int FOV_DEFAULT_INDEX = 3;
static_assert(k_fov[FOV_DEFAULT_INDEX] == 100);
constexpr int k_deadzone[] = {0, 3, 5, 8, 10, 12, 15, 20, 25, 30, 40};  // stick dead zone, % of full tilt
constexpr int DEADZONE_DEFAULT = 4;
static_assert(k_deadzone[DEADZONE_DEFAULT] == (int)(STICK_DEADZONE_DEFAULT * 100 + 0.5f));
const char *const k_invert[] = {"Off", "Up and down", "Left and right", "Both"};  // (MOTION_INVERT_* bits)
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
int g_nluts = 1;  // Colour > LUT's values (color_lut_list: None, the shipped ones, the player's), listed per opening

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

// KIND_CHOICE: the values the game's row has. A wrapping choice has two more, 0 and count + 1, one past each end:
// stepping onto one stores the value at the other end (value v is the game's v + 1)
int game_count(const Item &it) { return it.kind != KIND_CHOICE ? 2 : it.count(it) + (it.wrap ? 2 : 0); }
int game_value(const Item &it, int g) {
    int n = it.count(it);
    return it.wrap ? (g - 1 + n) % n : g;
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
    if (u == UPSCALE_FSR3) return soon("FSR 3");
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
#ifndef __APPLE__
    v.push_back(button(P_MAIN, "Colour", "Presets, exposure, contrast, saturation and LUTs.", "Open", A_PAGE, P_COLOUR));
#endif
    v.push_back(button(P_MAIN, "Audio", "Volume and sound effects.", "Open", A_PAGE, P_AUDIO));
    v.push_back(button(P_MAIN, "Controls", "Camera, motion, d-pad, mouse and button icons.", "Open", A_PAGE, P_CONTROLS));
    v.push_back(button(P_MAIN, "Keyboard (buttons)", "Which key does each button.", "Open", A_PAGE, P_KEYS));
    v.push_back(button(P_MAIN, "Keyboard (sticks)", "Keys for the sticks and for walking.", "Open", A_PAGE, P_STICKS));
    v.push_back(button(P_MAIN, "Controller", "Which controller button does each button.", "Open", A_PAGE, P_PAD));
    v.push_back(button(P_MAIN, "Language", "Language of the texts and of the voices.", "Open", A_PAGE, P_LANGUAGE));
    {   // (runtime/achievements.h: the page below lists them; its description here is the count)
        Item it = button(P_MAIN, "Achievements", "", "Open", A_PAGE, P_ACHIEVEMENTS);
        it.desc_fn = [](const Item &) -> const char * {
            static char buf[256];
            snprintf(buf, sizeof buf, tr("%d of %d achievements unlocked. They're checked each time the game saves."),
                     ach_unlocked_total(), g_ach_count);
            return buf;
        };
        v.push_back(it);
    }
    v.push_back(button(P_MAIN, "Quit game", "To the desktop, or back to the Savage Wind start screen.", "Quit",
                       A_QUIT, 0));

    // Video (fullscreen and the window size are applied by the window loop: menu.mm)
    v.push_back(toggle(P_VIDEO, "Fullscreen", "Fills the screen. Also F11 or Alt+Enter.",
                       [](const Item &) { return (int)g_settings.fullscreen; },
                       [](const Item &, int x) { g_settings.fullscreen = x; menu_request_window_apply(); }, 0));
    // Window size: a button showing the size; A drops the sizes down as a list over it (menu.mm size_list_draw)
    Item ws = button(P_VIDEO, "Window size", "", nullptr, A_SIZE_LIST, 0);
    ws.label = [](const Item &, int) {
        static char buf[64];
        WinSize list[WINSIZE_MAX];
        int n = winsize_items(list), x = winsize_index(winsize_effective(g_settings.fullscreen));
        winsize_name(list[x < n ? x : 0], tr("native"), buf, sizeof buf);
        return (const char *)buf;
    };
    ws.desc_fn = [](const Item &) {
        static char buf[256];
        WinSize d = winsize_desktop();
        snprintf(buf, sizeof buf, tr("Window or fullscreen size. A: choose it from a list. Your display: %d x %d."), d.w,
                 d.h);
        return (const char *)buf;
    };
    ws.reset = [](const Item &) {  // (unset: 1280 x 720 in a window, the desktop in fullscreen)
        g_settings.window_w = g_settings.window_h = 0;
        menu_request_window_apply();
    };
    v.push_back(ws);
    v.push_back(toggle(P_VIDEO, "Fill wide screens", "Shows more to the sides on screens wider than 16:9, instead of bars.",
                       [](const Item &) { return (int)g_settings.wide_view; },
                       [](const Item &, int x) { g_settings.wide_view = x; }, 1));
    v.push_back(choice(P_VIDEO, "Field of view", "How much the camera shows in play. Cutscenes keep their own.",
                       [](const Item &) { return N(k_fov); },
                       [](const Item &, int x) {
                           static std::string str;
                           str = pct(k_fov[x]);
                           return str.c_str();
                       },
                       [](const Item &) { return nearest(k_fov, N(k_fov), (float)g_settings.fov); },
                       [](const Item &, int x) { g_settings.fov = k_fov[x]; }, FOV_DEFAULT_INDEX));
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
    // VSync (settings.h VSYNC_*; the present mode: runtime/nvn/vulkan/device.c), next to the frame rate it caps. (One
    // line for all its values: the game reads a row's description when the cursor comes to it, not when it changes.)
    v.push_back(choice(P_VIDEO, "VSync", "On suits G-SYNC and FreeSync. Fast: no tearing and no waiting.",
                       [](const Item &) { return (int)VSYNC_COUNT; },
                       [](const Item &, int x) { return tr(k_vsync[x]); },
                       [](const Item &) { return g_settings.vsync; },
                       [](const Item &, int x) { g_settings.vsync = x; }, VSYNC_DEFAULT));
    // HDR (settings.h HDR_*: runtime/nvn/vulkan/device.c hdr_wanted)
    v.push_back(choice(P_VIDEO, "HDR", "Automatic: in HDR while Windows HDR is on, as bright as in SDR. Off: always SDR.",
                       [](const Item &) { return (int)HDR_COUNT; },
                       [](const Item &, int x) { return tr(k_hdr[x]); },
                       [](const Item &) { return g_settings.hdr; },
                       [](const Item &, int x) { g_settings.hdr = x; }, HDR_AUTO));
    v.push_back(choice(P_VIDEO, "Performance overlay", "Frame rate and system load in a corner. Also F10.",
                       [](const Item &) { return N(k_perf); }, [](const Item &, int x) { return tr(k_perf[x]); },
                       [](const Item &) { return g_settings.perf_overlay; },
                       [](const Item &, int x) { g_settings.perf_overlay = x; }, PERF_OFF));
    v.push_back(choice(P_VIDEO, "Upscaler", "Off: full resolution. AMD FSR 1: renders smaller, then upscales.",
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
                       [](const Item &, int x) { return upscale_modes_available() ? tr(k_modes[x]) : soon(k_modes[x]); },
                       [](const Item &) { return g_settings.upscale_quality; },
                       [](const Item &, int x) { g_settings.upscale_quality = x; }, UPQ_QUALITY);
    mode.value_off = [](const Item &, int) { return !upscale_modes_available(); };
    mode.desc_fn = [](const Item &) {
        // (one line: the game's description pane is one English line wide)
        return upscale_modes_available() ? tr("Ultra Quality is the sharpest, Performance the fastest.")
                                         : tr("Renders smaller, then upscales. Coming in a later version.");
    };
    v.push_back(mode);
    v.push_back(choice(P_VIDEO, "Anti-aliasing", "Smooths jagged edges. Best at a lower resolution.",
                       [](const Item &) { return N(k_aa); }, [](const Item &, int x) { return tr(k_aa[x]); },
                       [](const Item &) { return g_settings.antialias; },
                       [](const Item &, int x) { g_settings.antialias = x; }, AA_OFF));
#ifndef __APPLE__
    // Anisotropic filtering and sharpening (Vulkan: vulkan/textures.c sampler_aniso, vulkan/exec.c scene_post)
    v.push_back(choice(P_VIDEO, "Anisotropic filtering", "Sharper ground and paths at a distance. Off: as on the console.",
                       [](const Item &) { return N(k_aniso); },
                       [](const Item &, int x) { return x ? k_aniso[x] : tr("Off"); },
                       [](const Item &) { return g_settings.anisotropy; },
                       [](const Item &, int x) { g_settings.anisotropy = x; }, 0));
    v.push_back(choice(P_VIDEO, "Sharpening (AMD CAS)", "Sharpens the 3D image, not the HUD. Most useful with FSR 1.",
                       [](const Item &) { return 11; },
                       [](const Item &, int x) {
                           static std::string s;
                           s = x ? pct(x * 10) : std::string(tr("Off"));
                           return s.c_str();
                       },
                       [](const Item &) { return g_settings.sharpening; },
                       [](const Item &, int x) { g_settings.sharpening = x; }, 0));
    // AMD FSR 3 frame generation (vulkan/framegen.cpp)
    v.push_back(toggle(P_VIDEO, "Frame generation (AMD FSR 3)", "Twice the frames on screen. Adds a little delay.",
                       [](const Item &) { return (int)g_settings.frame_gen; },
                       [](const Item &, int x) { g_settings.frame_gen = x; }, 0));
#endif
    v.push_back(toggle(P_VIDEO, "Asynchronous shaders", "On: no stutter, but some effects appear late.",
                       [](const Item &) { return (int)g_settings.async_shaders; },
                       [](const Item &, int x) { g_settings.async_shaders = x; }, 0));
    // shadow resolution (its own block)
    v.push_back(choice(P_VIDEO, "Shadow resolution", "Sharper sun shadows. Higher costs more GPU time.",
                       [](const Item &) { return N(k_shadow); }, [](const Item &, int x) { return tr(k_shadow[x]); },
                       [](const Item &) { return g_settings.shadow_res; },
                       [](const Item &, int x) { g_settings.shadow_res = x; }, SHADOW_CONSOLE));

#ifndef __APPLE__
    // Colour (runtime/color_grade.h; vulkan/exec.c scene_post): the 3D image only, the HUD keeps its colours. The preset
    // sets the sliders below it; "Custom" (sliders matching no preset) is shown, never picked.
    Item preset = choice(P_COLOUR, "Colour preset", "Ready-made looks. Natural: the game's own colours.",
                         [](const Item &) { return g_color_preset_count + 1; },
                         [](const Item &, int x) { return tr(x < g_color_preset_count ? g_color_presets[x].name : "Custom"); },
                         [](const Item &) {
                             int p = color_preset_current();
                             return p < 0 ? g_color_preset_count : p;
                         },
                         [](const Item &, int x) { color_preset_apply(x); }, 0);
    preset.value_off = [](const Item &, int x) { return x >= g_color_preset_count; };
    preset.reset = [](const Item &) {
        color_preset_apply(0);
        g_settings.color_lut[0] = 0;
        g_settings.color_lut_strength = 100;
    };
    v.push_back(preset);
    v.push_back(choice(P_COLOUR, "Exposure", "Brighter or darker 3D image, in stops (EV).",
                       [](const Item &) { return 41; },
                       [](const Item &, int x) {
                           static char buf[32];
                           snprintf(buf, sizeof buf, "%+.1f EV", (x - 20) / 10.0);
                           return (const char *)buf;
                       },
                       [](const Item &) { return g_settings.color_exposure + 20; },
                       [](const Item &, int x) { g_settings.color_exposure = x - 20; }, 20));
    // contrast -50..50, the others -100..100, in steps of 5
    auto signed_pct = [](const Item &it, int x) {
        static char buf[32];
        int v = (x - (it.count(it) - 1) / 2) * 5;
        snprintf(buf, sizeof buf, v ? "%+d" : "%d", v);
        return (const char *)buf;
    };
    v.push_back(choice(P_COLOUR, "Contrast", "More or less difference between dark and bright areas.",
                       [](const Item &) { return 21; }, signed_pct,
                       [](const Item &) { return g_settings.color_contrast / 5 + 10; },
                       [](const Item &, int x) { g_settings.color_contrast = (x - 10) * 5; }, 10));
    v.push_back(choice(P_COLOUR, "Saturation", "How strong the colours are. -100: black and white.",
                       [](const Item &) { return 41; }, signed_pct,
                       [](const Item &) { return g_settings.color_saturation / 5 + 20; },
                       [](const Item &, int x) { g_settings.color_saturation = (x - 20) * 5; }, 20));
    v.push_back(choice(P_COLOUR, "Temperature", "Cooler (blue) or warmer (orange) light.",
                       [](const Item &) { return 41; }, signed_pct,
                       [](const Item &) { return g_settings.color_temperature / 5 + 20; },
                       [](const Item &, int x) { g_settings.color_temperature = (x - 20) * 5; }, 20));
    v.push_back(choice(P_COLOUR, "Tint", "Toward green, or toward magenta.",
                       [](const Item &) { return 41; }, signed_pct,
                       [](const Item &) { return g_settings.color_tint / 5 + 20; },
                       [](const Item &, int x) { g_settings.color_tint = (x - 20) * 5; }, 20));
    Item lut = choice(P_COLOUR, "LUT", "A colour look. Yours: .cube or .png files in the luts folder.",
                      [](const Item &) { return g_nluts; },
                      [](const Item &, int x) { return x ? color_lut_name(x) : tr("None"); },
                      [](const Item &) { return color_lut_index(g_settings.color_lut); },
                      [](const Item &, int x) {
                          snprintf(g_settings.color_lut, sizeof g_settings.color_lut, "%s", color_lut_id(x));
                      }, 0);
    v.push_back(lut);
    v.push_back(choice(P_COLOUR, "LUT intensity", "How much of the LUT's look is used.",
                       [](const Item &) { return 11; },
                       [](const Item &, int x) {
                           static std::string str;
                           str = pct(x * 10);
                           return str.c_str();
                       },
                       [](const Item &) { return (g_settings.color_lut_strength + 5) / 10; },
                       [](const Item &, int x) { g_settings.color_lut_strength = x * 10; }, 10));
#endif

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
    for (int s = 0; s < 2; s++)  // (runtime/hle/input.c dead_zone)
        v.push_back(choice(P_CONTROLS, s ? "Right stick dead zone" : "Left stick dead zone",
                           "How far the stick must move before it counts. Raise it if the stick drifts.",
                           [](const Item &) { return N(k_deadzone); },
                           [](const Item &, int x) {
                               static std::string str;
                               str = pct(k_deadzone[x]);
                               return str.c_str();
                           },
                           s ? [](const Item &) { return nearest(k_deadzone, N(k_deadzone), g_settings.stick_deadzone[1] * 100); }
                             : [](const Item &) { return nearest(k_deadzone, N(k_deadzone), g_settings.stick_deadzone[0] * 100); },
                           s ? [](const Item &, int x) { g_settings.stick_deadzone[1] = k_deadzone[x] / 100.0f; }
                             : [](const Item &, int x) { g_settings.stick_deadzone[0] = k_deadzone[x] / 100.0f; },
                           DEADZONE_DEFAULT));
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
    v.push_back(choice(P_CONTROLS, "Mouse vertical sensitivity",
                       "Up and down, as a percentage of the mouse sensitivity.",
                       [](const Item &) { return N(k_sens_y); },
                       [](const Item &, int x) {
                           static std::string s;
                           s = pct(k_sens_y[x]);
                           return s.c_str();
                       },
                       [](const Item &) { return nearest(k_sens_y, N(k_sens_y), g_settings.mouse_sens_y * 100); },
                       [](const Item &, int x) { g_settings.mouse_sens_y = k_sens_y[x] / 100.0f; }, SENS_Y_DEFAULT));
    v.push_back(toggle(P_CONTROLS, "Invert mouse Y", "Only the mouse. The camera Y-axis option inverts both.",
                       [](const Item &) { return (int)g_settings.mouse_invert_y; },
                       [](const Item &, int x) { g_settings.mouse_invert_y = x; }, 0));
    // motion controls (runtime/hle/motion.c; the game's own Motion Controls option turns motion aiming on and off)
    v.push_back(choice(P_CONTROLS, "Motion sensitivity", "How far turning the controller turns the aim.",
                       [](const Item &) { return N(k_motion_sens); },
                       [](const Item &, int x) {
                           static std::string s;
                           s = pct(k_motion_sens[x]);
                           return s.c_str();
                       },
                       [](const Item &) { return nearest(k_motion_sens, N(k_motion_sens), g_settings.motion_sens * 100); },
                       [](const Item &, int x) { g_settings.motion_sens = k_motion_sens[x] / 100.0f; },
                       MOTION_SENS_DEFAULT));
    v.push_back(choice(P_CONTROLS, "Invert motion", "Turning the controller moves the other way: aiming and tilting.",
                       [](const Item &) { return N(k_invert); },
                       [](const Item &, int x) { return tr(k_invert[x]); },
                       [](const Item &) { return g_settings.motion_invert & 3; },
                       [](const Item &, int x) { g_settings.motion_invert = x; }, 0));
    {   // the tilt key: held, the mouse tilts the controller (for motion puzzles without a gyroscope)
        Item it = button(P_CONTROLS, bind_input_name(BIND_TILT),
                         "Hold it and move the mouse to tilt the controller. Press A, then the new key.", nullptr,
                         A_BIND_KEY, BIND_TILT);
        it.label = bind_label;
        v.push_back(it);
    }
    v.push_back(choice(P_CONTROLS, "Button icons",
                       "The buttons the game's prompts show. Automatic: the device you used last.",
                       [](const Item &) { return N(k_icons); },
                       [](const Item &, int x) { return tr(k_icons[x]); },
                       [](const Item &) { return g_settings.button_icons; },
                       [](const Item &, int x) { g_settings.button_icons = x; }, ICONS_AUTO));

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
                       [](const Item &) { return language_text_count(); },  // (Thai: 1.9.0 on)
                       [](const Item &, int x) { return tr(g_text_languages[x].name); },
                       [](const Item &) { return language_text_index(); },
                       [](const Item &, int x) { g_settings.text_language = x; }, 0));
    v.push_back(choice(P_LANGUAGE, "Voices", "The characters' voices. Applies after a restart.",
                       [](const Item &) { return g_nvoice_languages; },
                       [](const Item &, int x) { return tr(g_voice_languages[x].name); },
                       [](const Item &) { return g_settings.voice_language; },
                       [](const Item &, int x) { g_settings.voice_language = x; }, 0));
    v.push_back(button(P_LANGUAGE, "Restart now",
                       "Starts the game again with these languages. Unsaved progress is lost.", "Restart", A_RESTART, 0));

    // Achievements (runtime/achievements.h): one row per achievement, its state where a button's text goes (Unlocked, or
    // how far along it is, or Locked); the description line says what it takes, and when it was unlocked. A does nothing.
    for (int i = 0; i < g_ach_count; i++) {
        Item it = button(P_ACHIEVEMENTS, g_ach_defs[i].name, g_ach_defs[i].desc, nullptr, A_NONE, i);
        it.label = [](const Item &row, int) -> const char * {
            static char buf[48];
            AchInfo in;
            ach_info(row.arg, &in);
            if (in.unlocked) return tr("Unlocked");
            if (in.target <= 1) return tr("Locked");
            snprintf(buf, sizeof buf, "%u / %u  (%u%%)", in.cur, in.target, in.cur * 100 / in.target);
            return buf;
        };
        it.desc_fn = [](const Item &row) -> const char * {
            static char buf[320];
            AchInfo in;
            ach_info(row.arg, &in);
            if (!in.def) return "";
            if (!in.unlocked || in.at <= 0) return tr(in.def->desc);
            char when[32] = "";
            time_t t = in.at;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            strftime(when, sizeof when, "%Y-%m-%d", &tmv);
            snprintf(buf, sizeof buf, tr("%s  Unlocked: %s"), tr(in.def->desc), when);
            return buf;
        };
        v.push_back(it);
    }
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
        e.last_value = m + MSG_VALUE + game_count(it) - 1;
        e.button_off = e.button_on = m + MSG_BUTTON;
        g_entries.push_back(e);
    }
    for (auto &o : g_order) o.clear();
    // the game's rows: camera and motion aiming ones on Controls; amiibo, the voice language (Language has the voices)
    // and the VR goggles mode (no use on a PC screen) left out; the rest on Gameplay
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t e = ((const uint32_t *)(uintptr_t)order)[r];
        if (e == GAME_AMIIBO || e == GAME_VOICE_BUTTON || e == GAME_VOICE_LEVEL || e == GAME_VR) continue;
        bool camera = e == GAME_CAMERA_Y || e == GAME_CAMERA_X || e == GAME_GYRO || e == GAME_CAMERA_SENSITIVITY;
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
    int n = (int)rd32(screen + g_site.opt_screen_rows);
    uint64_t rows = rd64(screen + g_site.opt_screen_rows + 8);
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
            // (Ё / ё become Е / е, as in the game's own Russian text: its Caption_00 font, the option names', has
            // no glyph for them and left a gap)
            o.push_back((char16_t)(c == 0x401 ? 0x415 : c == 0x451 ? 0x435 : c));
        }
    }
    return o;
}

// A message for the game ({char16_t *, u32 length with the terminator} at out); id: its storage. colour: one of the
// game's message colours (its colour tag: 0x0e, group 0, tag 3, 2 bytes of colour; 0xffff back to the text's own)
void put_text_coloured(uint32_t id, const char *s, uint64_t out, char16_t colour) {
    std::u16string &t = g_text[id];
    std::u16string n = utf16(s);
    static const char16_t tag[] = {0x0e, 0, 3, 2};  // (then the colour)
    if (colour != 0xffff) n = std::u16string(tag, 4) + colour + n + std::u16string(tag, 4) + (char16_t)0xffff;
    if (t != n) t = n;
    wr64(out, (uint64_t)(uintptr_t)t.c_str());
    wr32(out + 8, (uint32_t)t.size() + 1);
}

// ... in the text's own colour, or the game's grey
void put_text(uint32_t id, const char *s, uint64_t out, bool grey = false) {
    put_text_coloured(id, s, out, grey ? GREY : 0xffff);
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

// The game gets no A while the Achievements page is up (runtime/hle/input.c): A on a row would move the cursor onto
// its button, and the achievements' rows only show what they say
extern "C" bool game_options_hold_a(void) {
    return g_installed && g_cur_page == P_ACHIEVEMENTS && options_screen();
}

// The game gets no input while a binding capture runs, and briefly after it (the pressed button is still down)
extern "C" bool game_options_input_hold(void) {
    int d, s, ms;
    return bind_capture_active(&d, &s, &ms) || std::chrono::steady_clock::now() < g_hold_until;
}

// The Achievements page's rows as on Steam: the unlocked ones first (the latest on top), then the rest by how far
// along they are; ties keep the table's order (easiest first)
void ach_order(std::vector<uint32_t> &o) {
    auto key = [](uint32_t e, AchInfo *in, double *frac) {
        ach_info(g_items[e - GAME_ENTRIES].arg, in);
        *frac = in->target ? (double)in->cur / in->target : 0;
    };
    std::sort(o.begin(), o.end());  // (the table's order first: entries are in it)
    std::stable_sort(o.begin(), o.end(), [&](uint32_t a, uint32_t b) {
        AchInfo ia, ib;
        double fa, fb;
        key(a, &ia, &fa), key(b, &ib, &fb);
        if (ia.unlocked != ib.unlocked) return ia.unlocked;
        if (ia.unlocked) return ia.at > ib.at;
        return fa > fb;
    });
}

// createRows: the row list of the page to show
extern "C" void rt_opt_rows(void) {
    if (!install()) return;
    int page = g_switching ? g_next_page : P_MAIN;
    g_switching = false;
    if (page == P_ACHIEVEMENTS) ach_order(g_order[page]);
    std::vector<uint32_t> &o = g_order[page];
    g_shown.assign(g_items.size(), -1);
    if (page == P_COLOUR) g_nluts = color_lut_list();  // (files the player added since the last opening)
    for (size_t i = 0; i < g_items.size(); i++) {  // (how many window sizes there are depends on the display)
        Entry &e = g_entries[GAME_ENTRIES + i];
        if (g_items[i].kind == KIND_CHOICE) e.last_value = e.first_value + game_count(g_items[i]) - 1;
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
        // +0x64 not 0: the row's redraw (opt_row_redraw) turns its button off, as the game's own unavailable ones -
        // the achievements' rows, which only show what they say: A on them does nothing (no sound, no press)
        wr8(row + 0x64, it->action == A_NONE);
        return 1;
    }
    size_t idx = (size_t)(it - g_items.data());
    int x = clamp_value(*it, idx < g_shown.size() && g_shown[idx] >= 0 ? g_shown[idx] : it->get(*it));
    wr8(row + 0x54, x != 0);
    wr32(row + 0x58, (uint32_t)(x + (it->kind == KIND_CHOICE && it->wrap)));
    wr32(row + 0x5c, (uint32_t)(game_count(*it) - 1));
    wr32(row + 0x60, 0);
    return 1;
}

extern "C" int rt_opt_store(uint64_t row) {
    const Item *it = item_of_row(row);
    if (!it) return 0;
    size_t idx = (size_t)(it - g_items.data());
    bool past_end = false;
    if (it->kind == KIND_TOGGLE) {
        it->set(*it, rd32(row + 0x54) & 0xff ? 1 : 0);
    } else if (it->kind == KIND_CHOICE) {
        int g = (int)rd32(row + 0x58), gn = game_count(*it);
        g = g < 0 ? 0 : g >= gn ? gn - 1 : g;
        past_end = it->wrap && (g == 0 || g == gn - 1);
        int x = clamp_value(*it, game_value(*it, g));
        if (past_end) wr32(row + 0x58, (uint32_t)(x + 1));  // (the row's next step goes on from there)
        bool off = it->value_off && it->value_off(*it, x);  // (shown, not stored)
        if (idx < g_shown.size()) g_shown[idx] = off ? x : -1;
        if (!off) it->set(*it, x);
        if (getenv("SAVAGEWIND_LOG_OPTIONS"))  // (tests: when each value is stored)
            fprintf(stderr, "[options] %s = %d at %.3f s\n", it->title, x, SDL_GetTicksNS() / 1e9);
    }
    if (it->kind != KIND_BUTTON) settings_save();
    // (the sizes offered follow Fullscreen, texts the upscaler; Colour: the preset sets the sliders, they the preset)
    if (it->page == P_VIDEO || it->page == P_COLOUR) mark_all_rows(rd64(row));
    if (past_end) g_dirty.push_back(row);                // (first: the row shows the value at the other end)
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
    else if (part == MSG_BUTTON && it->page == P_ACHIEVEMENTS) {  // (an achievement's state: red until unlocked, then green)
        AchInfo in;
        ach_info(it->arg, &in);
        put_text_coloured(id, it->label(*it, -1), out, in.unlocked ? GREEN : RED);
        return 1;
    }
    else if (part == MSG_BUTTON) s = it->button ? tr(it->button) : it->label ? it->label(*it, -1) : "";
    else if (part >= MSG_VALUE && it->kind == KIND_CHOICE && (int)(part - MSG_VALUE) < game_count(*it)) {
        int v = game_value(*it, (int)(part - MSG_VALUE));
        s = it->label(*it, v);
        grey = it->value_off && it->value_off(*it, v);
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
    case A_SIZE_LIST:
        g_size_row = row;
        menu_size_list_open();
        return 2;
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

// Window size's list (menu.mm, render thread): a size was picked, the row shows it
extern "C" void game_options_size_picked(void) { g_size_redraw = true; }

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
    if (g_size_redraw.exchange(false) && g_size_row) g_dirty.push_back(g_size_row);
    if (g_dirty.empty()) return 0;
    uint64_t r = g_dirty.back();
    g_dirty.pop_back();
    // (the redraw shows the row's own value: read it again, for values set from another row - Colour's preset)
    if (const Item *it = item_of_row(r); it && it->kind != KIND_BUTTON) rt_opt_read(r);
    if (getenv("SAVAGEWIND_LOG_OPTIONS")) {
        const Item *it = item_of_row(r);
        fprintf(stderr, "[options] redraw %s value %u (%zu left)\n", it ? it->title : "?", rd32(r + 0x58), g_dirty.size());
    }
    return r;
}

// B: from one of these pages back to the first one
extern "C" void rt_opt_back(void) {
    bind_capture_cancel();
    g_cap_row = 0;
    bool direct = g_ach_direct && g_cur_page == P_ACHIEVEMENTS;
    g_ach_direct = false;
    if (g_installed && g_cur_page != P_MAIN && !direct) {
        g_next_page = P_MAIN;
        g_reopen = g_switching = true;
    } else {  // (it closes: the next opening is on the first page, unless a button asks for another)
        g_reopen = g_switching = false;
        g_cur_page = P_MAIN;
    }
}

// The title menu's Downloadable Content button (id 0x6d) is gone until Savage Wind has DLC: its eShop button opened the
// shop applet, which isn't there, and the game waited for it forever (issue #19). Two parts, both through the game's
// own means, so the menu behaves as it does when the game itself leaves buttons out (no save: no Continue):
//  - which buttons the cursor can reach: the menu enables them at its start from static lists {u32 count, ids} (one
//    per menu variant, in main's .bss, filled by a static constructor). The ones that hold 0x6d hold it last, so
//    their count goes down by one, once the module's static constructors have run (rt_title_menu_prepare, called from
//    runtime/main.c; the lists are found by their contents: title menu ids 0x69..0x6f, ascending).
//  - what is drawn: the menu shows its buttons (the panes Pa_SaveFile_00..05) through the frame it sets on its
//    button-count animation (Title_00_BtnNum), whose visibility curves show Quit and Downloadable Content
//    (Pa_SaveFile_03 / _04) together in every frame. That animation's data, in the loaded layout archive, gets
//    Pa_SaveFile_04's visibility keys set to hidden (title_hide_dlc, on every update: the archive is loaded again with
//    the title screen; the animation's handle is at the Options state field - 0x24 in 1.8.2 and 1.9.0, its pai1 block
//    at handle + 0x18).
// It was the menu's last button, so there is no gap.
// With downloadable content set up the button opens the game's own list of the packs (no eShop): once the game mounts
// an add-on content (rt_title_menu_dlc, from runtime/services/fs.c) the lists get it back and the pane is left alone.
const uint32_t TITLE_DLC_BUTTON = 0x6d;
static uint64_t g_dlc_lists[8];
static int g_ndlc_lists;
static bool g_dlc_button;

extern "C" void rt_title_menu_prepare(void) {
    int n = 0;
    g_ndlc_lists = 0;
    g_dlc_button = false;
    for (uint64_t p = g_site.main_data; p + 0x10 <= g_site.main_end; p += 8) {
        uint32_t count = rd32(p);
        uint64_t ids = rd64(p + 8);
        if (count < 2 || count > 7 || ids < 0x7000000000ull || ids + 4 * count > g_site.main_end || (ids & 3))  // (in main)
            continue;
        bool menu = true;
        for (uint32_t i = 0; i < count && menu; i++) {
            uint32_t id = rd32(ids + 4 * i);
            menu = id >= 0x69 && id <= 0x6f && (i == 0 || id > rd32(ids + 4 * i - 4));
        }
        if (!menu || rd32(ids + 4 * (count - 1)) != TITLE_DLC_BUTTON) continue;
        wr32(p, count - 1);
        if (g_ndlc_lists < 8) g_dlc_lists[g_ndlc_lists++] = p;
        n++;
    }
    fprintf(stderr, "[options] title menu: Downloadable Content left out of %d button lists\n", n);
}

extern "C" void rt_title_menu_dlc(void) {
    if (g_dlc_button) return;
    g_dlc_button = true;
    for (int i = 0; i < g_ndlc_lists; i++) wr32(g_dlc_lists[i], rd32(g_dlc_lists[i]) + 1);
    fprintf(stderr, "[options] title menu: Downloadable Content back in %d button lists (add-on content mounted)\n",
            g_ndlc_lists);
}

static void title_hide_dlc(uint64_t menu) {
    if (g_dlc_button) return;
    uint64_t anim = rd64(menu + g_site.title_options_state - 0x24);
    uint64_t pai = anim ? rd64(anim + 0x18) : 0;
    if (!pai || memcmp((const void *)(uintptr_t)pai, "pai1", 4)) return;
    uint32_t ncontent = *(volatile uint16_t *)(uintptr_t)(pai + 0xe), table = rd32(pai + 0x10);
    for (uint32_t i = 0; i < ncontent && i < 64; i++) {
        uint64_t c = pai + rd32(pai + table + 4 * i);
        if (strncmp((const char *)(uintptr_t)c, "Pa_SaveFile_04", 28)) continue;
        uint32_t ninfo = *(volatile uint8_t *)(uintptr_t)(c + 28);
        for (uint32_t j = 0; j < ninfo; j++) {
            uint64_t info = c + rd32(c + 32 + 4 * j);
            if (memcmp((const void *)(uintptr_t)info, "FLVI", 4)) continue;
            uint32_t ntarget = *(volatile uint8_t *)(uintptr_t)(info + 4);
            for (uint32_t k = 0; k < ntarget; k++) {
                uint64_t t = info + rd32(info + 8 + 4 * k);
                uint32_t nkeys = *(volatile uint16_t *)(uintptr_t)(t + 4);
                uint64_t keys = t + rd32(t + 8);
                for (uint32_t q = 0; q < nkeys && q < 64; q++)  // step keys: {float frame, u16 value, u16}
                    *(volatile uint16_t *)(uintptr_t)(keys + 8 * q + 4) = 0;
            }
        }
        return;
    }
}

// ---- The title menu's Achievements button: runtime/services/fs.c adds an eighth button part to the menu's layout
// (Pa_SaveFile_06, reading Achievements), and the game makes its button like the seven others' - with no id, as the
// menu knows of seven (its id stays 0: an id the menu doesn't know of would take its cursor). It goes above Options:
// each update moves its button before Options' in the menu's list (the cursor's order), puts it where Options is, and
// Options and the buttons after it 40 lower (from their own places in the layout, kept the first time: which buttons
// show depends on the saves, the downloadable content and the menu's variant). Pressed, it does what Options (0x6b)
// does - the Options screen - with the Achievements page as the one the screen opens on; B from there goes straight
// back to the menu.
// The menu's buttons are a list (head at title_options_state + 0x22c; a button's node at +8: prev, next) of 0x68-byte
// objects: +0x18 their pane's name, +0x44 the id. A pane (nn::ui2d::Pane) is found by its name, 0xb8 into it: from
// there, -0xa0 its parent's pane, -0x84 its y in its parent, -0x60 its flags (bit 0: shown), -0x2c its y on the
// screen.
const uint32_t TITLE_OPTIONS_BUTTON = 0x6b, TITLE_ACH_BUTTON = 0;
#define TITLE_ACH_PART "Pa_SaveFile_06"  // (runtime/services/fs.c)
const float TITLE_BUTTON_STEP = 40;
uint64_t g_title_ach;      // its Achievements button
uint64_t g_title_pressed;  // ... while its id reads as Options for a press (given back at the next update)
uint64_t g_title_head;     // the menu the places below are of
struct { uint64_t name; float y; } g_title_place[16];  // the buttons' own places (y in their parents)
int g_title_places;

float rdf(uint64_t a) { float f; uint32_t v = rd32(a); memcpy(&f, &v, 4); return f; }
void wrf(uint64_t a, float f) { uint32_t v; memcpy(&v, &f, 4); wr32(a, v); }

bool pane_shown(uint64_t name) {  // (shown, and every pane it is in)
    for (int depth = 0; name && depth < 16; depth++) {
        if (!(rd32(name - 0x60) & 1)) return false;
        uint64_t parent = rd64(name - 0xa0);
        name = parent ? parent + 0xb8 : 0;
    }
    return true;
}

float own_place(uint64_t name) {  // (the y the layout and its animations give the pane)
    for (int i = 0; i < g_title_places; i++)
        if (g_title_place[i].name == name) return g_title_place[i].y;
    float y = rdf(name - 0x84);
    if (g_title_places < 16) g_title_place[g_title_places++] = {name, y};
    return y;
}

bool is_ach(uint64_t b) {
    uint64_t name = rd64(b + 0x18);
    return name && !memcmp((const void *)(uintptr_t)name, TITLE_ACH_PART, sizeof TITLE_ACH_PART);
}

void title_ach_button(uint64_t menu) {
    uint64_t head = menu + g_site.title_options_state + 0x22c, ach = 0, options = 0;
    if (head != g_title_head) g_title_head = head, g_title_places = 0;
    int n = 0;
    for (uint64_t node = rd64(head + 8); node && node != head && n < 16; node = rd64(node + 8), n++) {
        uint64_t b = node - 8;
        if (is_ach(b)) ach = b;
        else if (rd32(b + 0x44) == TITLE_OPTIONS_BUTTON && !options) options = b;
    }
    g_title_ach = ach;
    if (!ach || !options) return;
    if (rd64(ach + 0x10) != options + 8) {  // (before Options in the list: out of it, in again)
        uint64_t prev = rd64(ach + 8), next = rd64(ach + 0x10);
        wr64(prev + 8, next), wr64(next, prev);
        prev = rd64(options + 8);
        wr64(ach + 8, prev), wr64(ach + 0x10, options + 8);
        wr64(prev + 8, ach + 8), wr64(options + 8, ach + 8);
    }
    // Options and what follows: 40 lower than their own places; the button: where Options' would be
    bool after = false;
    n = 0;
    for (uint64_t node = rd64(head + 8); node && node != head && n < 16; node = rd64(node + 8), n++) {
        uint64_t b = node - 8, name = rd64(b + 0x18);
        if (b == ach) { after = true; continue; }
        if (!name) continue;
        wrf(name - 0x84, own_place(name) - (after ? TITLE_BUTTON_STEP : 0));
    }
    uint64_t name = rd64(ach + 0x18), parent = rd64(name - 0xa0), oname = rd64(options + 0x18);
    if (!parent || !oname || !pane_shown(oname)) return;
    float at = rdf(oname - 0x2c) - (rdf(oname - 0x84) - own_place(oname));  // (Options' place on the screen, unmoved)
    wrf(name - 0x84, at - rdf(parent + 0xb8 - 0x2c));  // (its place is in its parent's)
    wr32(name - 0x60, rd32(name - 0x60) | 1);
}

// The title menu's update (menu = the screen): 1 = open the Options screen again now (it has gone, a page is due).
// The quit question's answer is read here too.
extern "C" int rt_opt_title_tick(uint64_t menu) {
    if (g_title_pressed) wr32(g_title_pressed + 0x44, TITLE_ACH_BUTTON), g_title_pressed = 0;
    title_ach_button(menu);
    quit_poll();
    title_hide_dlc(menu);
    if (rd32(menu + g_site.title_options_state) != MENU_OPTIONS_ACTIVE) {  // (the Options button isn't active: next opening, first page)
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

// ---- The pause System page's Achievements button: the seventh button the page's setup adds when the Nintendo Switch 2
// Edition upgrade is offered (0x53, Pa_Btn_06, in the gap the layout leaves under Load; tools/patches.py
// _pause_upgrade has it added). Pressed, it does what Options (0x4f) does - the Options screen, which the System page
// waits on in its Options state - with the Achievements page as the one the screen opens on; B from there goes
// straight back to the System page.
const uint32_t PAUSE_OPTIONS_BUTTON = 0x4f, PAUSE_ACH_BUTTON = 0x53;

// The System page's setup: whether to add that button
extern "C" int rt_pause_ach_button(void) { return 1; }

// ... and its text: at str ({char16_t *, u32 length}) the setup has "Nintendo Switch 2 Edition", a string of its code
extern "C" void rt_pause_ach_text(uint64_t str) {
    if (!rt_pause_ach_button()) return;
    std::u16string &t = g_text[MSG_PAUSE_ACH];
    t = utf16(tr("Achievements"));
    wr64(str, (uint64_t)(uintptr_t)t.c_str());
    wr32(str + 8, (uint32_t)t.size());  // (as the game counts it: the characters, not the terminator)
}

// The System page's button handler (id = the button pressed): the id it goes on with
extern "C" uint32_t rt_pause_button(uint32_t id) {
    if (id != PAUSE_ACH_BUTTON) return id;
    g_next_page = P_ACHIEVEMENTS;
    g_switching = g_ach_direct = true;
    return PAUSE_OPTIONS_BUTTON;
}

// A message file's search by label (label: a C string; out: {char16_t *, u32 length}): the Options screen's title (LayoutMsg/OptionWindow_00 "T_OptionTitle_00") while it shows the Achievements page (the
// screen opens again on a page change, so the title is looked up once per page). SAVAGEWIND_LOG_PAUSE_MSG=1 logs the
// System page's labels as they're looked up.
extern "C" int rt_pause_msg(uint64_t label, uint64_t out) {
    const char *l = (const char *)(uintptr_t)label;
    if (!l) return 0;
    if (!strcmp(l, "T_OptionTitle_00")) {  // (looked up as the screen opens: the page it opens on is the one due)
        int page = g_switching ? g_next_page : g_cur_page;
        if (page != P_ACHIEVEMENTS) return 0;
        char title[96];
        snprintf(title, sizeof title, "%s  %d / %d", tr("Achievements"), ach_unlocked_total(), g_ach_count);
        put_text(MSG_ACH_TITLE, title, out);
        return 1;
    }
    if (strncmp(l, "Pa_Save_00-", 11)) return 0;
    static int log = -1;
    if (log < 0) log = getenv("SAVAGEWIND_LOG_PAUSE_MSG") && strcmp(getenv("SAVAGEWIND_LOG_PAUSE_MSG"), "0");
    if (log) fprintf(stderr, "[options] pause message %s\n", l);
    return 0;
}

extern "C" uint64_t rt_opt_screen(void) { return options_screen(); }

// (runtime/services/fs.c, which is C: the title menu's Achievements button text)
extern "C" const char *rt_tr(const char *en) { return tr(en); }

// ---- Quit: the game's two-button dialog (screen 0x39, type 7) asks: desktop (upper button) or the start screen
// (lower). Its buttons write their answer to dialog_answer (1.8.2 0x7001c4b308; upper 0x5b: 0, lower 0x5a: 1;
// FUN_7100d46598) and leave the dialog up; B closes it without an answer (rt_quit_dialog_back).

// The title menu's button handler (id: the button pressed's, read before the handler reads it). The amiibo settings
// button (Savage Wind has no amiibo support) is Quit: 1. The Achievements button reads as Options for this press, with
// its page due.
extern "C" int rt_title_event(uint32_t id) {
    if (id == TITLE_ACH_BUTTON && g_title_ach && rd32(g_title_ach + 0x44) == TITLE_ACH_BUTTON) {  // (the only one)
        g_next_page = P_ACHIEVEMENTS;
        g_switching = g_ach_direct = true;
        wr32(g_title_ach + 0x44, TITLE_OPTIONS_BUTTON);
        g_title_pressed = g_title_ach;
    }
    return id == TITLE_AMIIBO_BUTTON;
}

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
