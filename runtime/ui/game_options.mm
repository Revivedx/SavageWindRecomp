// Savage Wind's settings as pages of the game's own Options screen (pause menu > System > Options): the game draws
// them, with its font, sounds and cursor. The first page has one button per section (Gameplay: the game's own
// options; Video, Audio, Controls: the game's camera options and the mouse; Keyboard, Controller, Language;
// Accessibility: assists) and a Quit button; A on a section shows its page, B goes back. The screen opens from the
// pause menu (System > Options) and from the title screen menu (Options); this replaces the overlay menu in game
// (menu.mm). Quit (here, and in place of the title menu's amiibo settings) asks with the game's own dialog: desktop or
// the Savage Wind start screen.
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

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "i18n.h"
#include "binding_row_layout.h"

extern "C" {
#include "../input_bind.h"
#include "../language.h"
#include "../game_sites.h"
#include "../nvn/output_scale.h"
#include "../settings.h"
#include "../color_grade.h"
#include "../pc_cursor.h"
#include "../window_size.h"
void guest_call(Cpu *, uint64_t);
Cpu *kernel_current_cpu(void);
void rt_request_reboot(int kind);           // runtime/main.c (1: start the game again)
void menu_request_window_apply(void);       // menu.mm (fullscreen / window size, applied by the window loop)
void menu_size_list_open(void);             // menu.mm (Window size's list, over the screen)
bool menu_size_list_up(void);
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
enum { MSG_TITLE = 0, MSG_DESC = 1, MSG_BUTTON = 2, MSG_KEY = 3, MSG_PAD = 4, MSG_VALUE = 16 };
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

enum Page { P_MAIN, P_GAMEPLAY, P_VIDEO, P_COLOUR, P_AUDIO, P_CONTROLS, P_BINDS, P_LANGUAGE, P_ACCESS, P_COUNT };
// A_FIXED: a greyed row for an input that can never be rebound (Item.arg: 0 Esc, 1 F2); A_BIND: Item.arg is a binding slot (an input, or BIND_ACTION + an action); the next key, mouse button or controller button binds it
enum Action { A_NONE, A_PAGE, A_BIND, A_BINDS_HEADER, A_RESET_BINDS, A_RESTART, A_QUIT, A_SIZE_LIST, A_FIXED };

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
    int action, arg;                             // KIND_BUTTON (arg: also a choice's own number)
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
uint64_t g_quit_dialog;            // the quit question's dialog while it is up
uint64_t g_quit_owner;             // ... and the screen it came from (title menu or Options; its input is off meanwhile)
bool g_quit_texts;                 // ... and its texts are still to be given (its two-button layout comes next)
uint64_t g_cap_row;                // a binding row whose capture is running
int cap_slot_was = -1;             // ... and its slot (for the redraw once it has ended)
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
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_FSR4, UPSCALE_DLSS};
#endif
#ifdef __APPLE__
const int UPS_DEFAULT = 1;  // (UPSCALE_DEFAULT: MetalFX)
#else
const int UPS_DEFAULT = 0;  // (UPSCALE_DEFAULT: Off, the window's resolution)
#endif
std::vector<int> g_shown;   // per item: the value shown instead of get(), one that can't be picked yet (-1: none)
const char16_t GREY = 3;    // (the game's message colour for greyed values)
const char *const k_modes[] = {"Ultra Quality", "Quality", "Balanced", "Performance", "Ultra Performance"};
#ifdef __APPLE__
const char *const k_aa[] = {"Off", "SMAA"};
#else
const char *const k_aa[] = {"Off", "SMAA", "TAA"};  // (settings.h AA_*)
#endif
const char *const k_aniso[] = {"Off", "2x", "4x", "8x", "16x"};  // (settings anisotropy 0..4)
const char *const k_drawdist[] = {"Original", "Medium", "High"};  // (settings.h DRAWDIST_*)
constexpr int k_haze[] = HAZE_CHOICES;  // percent (Video > Haze)
const char *const k_shadow[] = {"Console (896)", "High (2048)", "Ultra (4096)", "Extreme (8192)"};  // (SHADOW_*)
const char *const k_text[] = {"Off", "2x", "4x"};  // (sharper_text / 2)
const char *const k_icons[] = {"Automatic", "Keyboard", "Xbox", "PlayStation", "Nintendo"};  // (ICONS_*)
constexpr int k_fps[] = FPS_CHOICES;  // frame rate (0: Off, no limit)
const char *const k_vsync[VSYNC_COUNT] = {"On", "Fast", "Adaptive", "Off"};  // (settings.h VSYNC_*)
const char *const k_hdr[HDR_COUNT] = {"Automatic", "On", "Off"};               // (settings.h HDR_*)
const char *const k_fsmode[FSMODE_COUNT] = {"Automatic", "Borderless", "Exclusive"};  // (settings.h FSMODE_*)
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
constexpr int k_damage[] = DAMAGE_CHOICES;  // Accessibility's damage multipliers, %
constexpr int DAMAGE_DEFAULT = 3;
static_assert(k_damage[DAMAGE_DEFAULT] == 100);
constexpr int k_fov[] = FOV_CHOICES;  // field of view, % of the game's own
constexpr int FOV_DEFAULT_INDEX = 3;
static_assert(k_fov[FOV_DEFAULT_INDEX] == 100);
constexpr int k_hud_size[] = HUD_SIZE_CHOICES;  // the HUD's size, % of the game's own
constexpr int HUD_SIZE_DEFAULT_INDEX = 10;
static_assert(k_hud_size[HUD_SIZE_DEFAULT_INDEX] == 100);
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
    if (u == UPSCALE_FSR3) return upscaler_available(u) ? tr("AMD FSR 3") : soon("FSR 3");
    if (u == UPSCALE_FSR4)
        return upscaler_available(u) ? tr("AMD FSR 4") : soon("FSR 4", fsr4_offered() ? "(coming soon)" : "(AMD Radeon only)");
    return soon("DLSS", renderer_gpu_rtx() ? "(coming soon)" : "(NVIDIA RTX only)");
}

const char *fps_label(const Item &, int x) {
    static char buf[32];
    if (!k_fps[x]) return tr("Off");
    snprintf(buf, sizeof buf, tr("%d FPS"), k_fps[x]);
    return buf;
}

// What a slot is bound to on the keyboard / mouse and on the controller. An action without a binding of its own shows
// the button its Switch input has (what it follows).
const char *slot_key_text(int slot) {
    if (bind_slot_is_action(slot)) {
        int h = g_act_key[slot - BIND_ACTION];
        if (h == ACT_FOLLOW) {
            int b = act_switch_button(slot - BIND_ACTION);
            h = b < 0 ? ACT_NONE : g_bind_key[b];
        }
        return h == ACT_NONE ? tr("Unbound") : tr(bind_key_label(h));
    }
    return tr(bind_key_label(g_bind_key[slot]));
}
const char *slot_pad_text(int slot) {
    if (bind_slot_is_action(slot)) {
        int h = g_act_pad[slot - BIND_ACTION];
        if (h == ACT_FOLLOW) {
            int b = act_switch_button(slot - BIND_ACTION);
            h = b < 0 ? ACT_NONE : g_bind_pad[b];
        }
        return h == ACT_NONE ? tr("Unbound") : tr(bind_pad_label(h, bind_label_family()));
    }
    return tr(bind_pad_label(g_bind_pad[slot], bind_label_family()));
}
bool slot_has_pad(int slot) { return bind_slot_is_action(slot) || bind_pad_slot(slot); }

const char *bind_label(const Item &it, int) {
    static char buf[160];
    int dev, slot, ms;
    if (bind_capture_active(&dev, &slot, &ms) && slot == it.arg)
        return tr(slot_has_pad(slot) ? "Press a key or button..." : "Press a key...");
    if (slot_has_pad(it.arg)) snprintf(buf, sizeof buf, "%s  |  %s", slot_key_text(it.arg), slot_pad_text(it.arg));
    else snprintf(buf, sizeof buf, "%s", slot_key_text(it.arg));
    return buf;
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
    v.push_back(button(P_MAIN, "Key bindings", "Which key, mouse button or controller button does what.", "Open", A_PAGE, P_BINDS));
    v.push_back(button(P_MAIN, "Language", "Language of the texts and of the voices.", "Open", A_PAGE, P_LANGUAGE));
    v.push_back(button(P_MAIN, "Accessibility", "Assists that make the game easier.", "Open", A_PAGE, P_ACCESS));
    v.push_back(button(P_MAIN, "Quit game", "To the desktop, or back to the Savage Wind start screen.", "Quit",
                       A_QUIT, 0));

    // Video (fullscreen and the window size are applied by the window loop: menu.mm)
    v.push_back(toggle(P_VIDEO, "Fullscreen", "Fills the screen. Also F11 or Alt+Enter.",
                       [](const Item &) { return (int)g_settings.fullscreen; },
                       [](const Item &, int x) { g_settings.fullscreen = x; menu_request_window_apply(); }, 0));
    // Fullscreen mode (settings.h FSMODE_*; runtime/window_size.c winsize_apply)
    v.push_back(choice(P_VIDEO, "Fullscreen mode", "Borderless never switches the display. Exclusive always does.",
                       [](const Item &) { return (int)FSMODE_COUNT; },
                       [](const Item &, int x) { return tr(k_fsmode[x]); },
                       [](const Item &) { return g_settings.fullscreen_mode; },
                       [](const Item &, int x) {
                           g_settings.fullscreen_mode = x;
                           if (g_settings.fullscreen) menu_request_window_apply();
                       }, FSMODE_AUTO));
    // Monitor: Automatic, then the displays connected (runtime/window_size.c; listed again at each opening)
    Item disp = choice(P_VIDEO, "Monitor", "The monitor the game is shown on. Automatic: where the window is.",
                       [](const Item &) {
                           WinDisplay ds[WINDISPLAY_MAX];
                           return windisplay_list(ds) + 1;
                       },
                       [](const Item &, int x) {
                           static char buf[4][96];
                           static int k;
                           char *b = buf[k++ & 3];
                           WinDisplay ds[WINDISPLAY_MAX];
                           int n = windisplay_list(ds);
                           if (x < 1 || x > n) return tr("Automatic");
                           snprintf(b, sizeof buf[0], "%d: %s", x, ds[x - 1].name);  // (short: the row is narrow)
                           return (const char *)b;
                       },
                       [](const Item &) { return windisplay_target(); },
                       [](const Item &, int x) {
                           windisplay_set(x);
                           menu_request_window_apply();
                       }, 0);
    disp.wrap = true;
    v.push_back(disp);
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
    v.push_back(toggle(P_VIDEO, "Fill wide screens", "Shows more of the world on screens that aren't 16:9 (wider or taller), instead of bars.",
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
    v.push_back(choice(P_GAMEPLAY, "HUD size", "How big the hearts, stamina, minimap and the rest of the HUD are. Each part stays in its corner.",
                       [](const Item &) { return N(k_hud_size); },
                       [](const Item &, int x) {
                           static std::string str;
                           str = pct(k_hud_size[x]);
                           return str.c_str();
                       },
                       [](const Item &) { return nearest(k_hud_size, N(k_hud_size), (float)g_settings.hud_size); },
                       [](const Item &, int x) { g_settings.hud_size = k_hud_size[x]; }, HUD_SIZE_DEFAULT_INDEX));
    // Draw distance (runtime/draw_distance.c): applied while playing, from the next placed actor the game looks at
    v.push_back(choice(P_VIDEO, "Draw distance", "How far out grass, trees, rocks, buildings and enemies appear. Higher costs more CPU and GPU time.",
                       [](const Item &) { return (int)DRAWDIST_COUNT; },
                       [](const Item &, int x) { return tr(k_drawdist[x]); },
                       [](const Item &) { return g_settings.draw_distance; },
                       [](const Item &, int x) { g_settings.draw_distance = x; }, DRAWDIST_ORIGINAL));
    // the air's haze (services/fs.c haze_patch): the game reads it at the start
    v.push_back(choice(P_VIDEO, "Haze", "How hazy the air is. Lower: clearer distances. From the next start.",
                       [](const Item &) { return N(k_haze); },
                       [](const Item &, int x) {
                           static std::string s;
                           s = pct(k_haze[x]);
                           return s.c_str();
                       },
                       [](const Item &) { return nearest(k_haze, N(k_haze), (float)g_settings.haze); },
                       [](const Item &, int x) { g_settings.haze = k_haze[x]; }, N(k_haze) - 1));
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
    v.back().desc_fn = [](const Item &) {  // (one line)
        return g_settings.upscaler == UPSCALE_FSR4 ? tr("AMD FSR 4: renders smaller, then rebuilds detail with machine learning.")
             : g_settings.upscaler == UPSCALE_FSR3 ? tr("AMD FSR 3: renders smaller, then rebuilds detail from past frames.")
                                                   : tr("Off: full resolution. AMD FSR 1: renders smaller, then upscales.");
    };
    // With Upscaler Off no mode applies: the row has the one value Off, greyed, so its arrows go nowhere (rt_opt_read
    // gives the row its range at each redraw, and a change of the Upscaler row redraws the page); the mode chosen
    // before stays stored and comes back with an upscaler.
    Item mode = choice(P_VIDEO, "Upscaler mode", "",
                       [](const Item &) { return g_settings.upscaler == UPSCALE_OFF ? 1 : N(k_modes); },
                       [](const Item &, int x) {
                           return g_settings.upscaler == UPSCALE_OFF ? tr("Off")
                                  : upscale_modes_available()        ? tr(k_modes[x])
                                                                     : soon(k_modes[x]);
                       },
                       [](const Item &) { return g_settings.upscaler == UPSCALE_OFF ? 0 : g_settings.upscale_quality; },
                       [](const Item &, int x) {
                           if (g_settings.upscaler != UPSCALE_OFF) g_settings.upscale_quality = x;
                       },
                       UPQ_QUALITY);
    mode.value_off = [](const Item &, int) { return !upscale_modes_available() || g_settings.upscaler == UPSCALE_OFF; };
    mode.reset = [](const Item &) { g_settings.upscale_quality = UPQ_QUALITY; };  // (also while Off)
    mode.desc_fn = [](const Item &) {
        // (one line: the game's description pane is one English line wide)
        return !upscale_modes_available()           ? tr("Renders smaller, then upscales. Coming in a later version.")
               : g_settings.upscaler == UPSCALE_OFF ? tr("Only with an upscaler: choose one above first.")
                                                    : tr("Ultra Quality is the sharpest, Ultra Performance the fastest.");
    };
    v.push_back(mode);
    v.push_back(choice(P_VIDEO, "Anti-aliasing", "Smooths jagged edges. Best at a lower resolution.",
                       [](const Item &) { return N(k_aa); }, [](const Item &, int x) { return tr(k_aa[x]); },
                       [](const Item &) { return g_settings.antialias; },
                       [](const Item &, int x) { g_settings.antialias = x; }, AA_OFF));
#ifndef __APPLE__
    // the game's own FXAA (vulkan/exec.c fxaa_patch)
    v.push_back(toggle(P_VIDEO, "Game's FXAA", "The game's own edge smoothing. Off: sharper, more jagged edges.",
                       [](const Item &) { return (int)g_settings.game_fxaa; },
                       [](const Item &, int x) { g_settings.game_fxaa = x; }, 1));
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
    v.push_back(choice(P_VIDEO, "Shadow resolution", "Sharper sun shadows. Extreme can cut the frame rate to a third in forests, even on a fast GPU.",
                       [](const Item &) { return N(k_shadow); }, [](const Item &, int x) { return tr(k_shadow[x]); },
                       [](const Item &) { return g_settings.shadow_res; },
                       [](const Item &, int x) { g_settings.shadow_res = x; }, SHADOW_CONSOLE));
    // the game's font sheets enlarged (runtime/nvn/text_upscale.c): read when the game starts
    v.push_back(choice(P_GAMEPLAY, "Sharper text", "Upscales the game's letters for 1440p and 4K, after a restart. Not the original art.",
                       [](const Item &) { return N(k_text); }, [](const Item &, int x) { return tr(k_text[x]); },
                       [](const Item &) { return g_settings.sharper_text / 2; },
                       [](const Item &, int x) { g_settings.sharper_text = x * 2; }, 0));

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
    v.push_back(toggle(P_CONTROLS, "Controller rumble", "Vibrate the controller during play.",
                       [](const Item &) { return (int)g_settings.rumble; },
                       [](const Item &, int x) { g_settings.rumble = x; }, 1));
    v.push_back(toggle(P_CONTROLS, "Free camera with both sticks",
                       "Clicking both sticks together turns the free camera on and off. Home on the keyboard always does.",
                       [](const Item &) { return (int)g_settings.freecam_sticks; },
                       [](const Item &, int x) { g_settings.freecam_sticks = x; }, 1));
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
    v.push_back(toggle(P_CONTROLS, "PC mouse pointer", "Point and click in menus and map. Esc pauses/goes back; F2 frees the cursor.",
                       [](const Item &) { return (int)g_settings.pc_cursor; },
                       [](const Item &, int x) { g_settings.pc_cursor = x; }, 1));
    v.push_back(toggle(P_CONTROLS, "Mouse camera", "The mouse turns the camera. Pointer off: Esc frees it; pointer on: F2 frees it.",
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
    v.push_back(toggle(P_CONTROLS, "Mouse bow aiming", "Hold right mouse to aim a bow, left click to fire. Right mouse no longer raises the shield.",
                       [](const Item &) { return (int)g_settings.pc_bow; },
                       [](const Item &, int x) { g_settings.pc_bow = x; }, 0));
    v.push_back(toggle(P_CONTROLS, "Shrine keyboard gyro",
                       "Only while controlling a shrine apparatus. Remap under Key bindings. Outside it, camera keys keep working.",
                       [](const Item &) { return (int)g_settings.pc_gyro; },
                       [](const Item &, int x) { g_settings.pc_gyro = x; }, 1));
    v.push_back(toggle(P_CONTROLS, "Shrine mouse drag",
                       "At a shrine apparatus, hold left click and drag to rotate. Release to keep its orientation and re-grip.",
                       [](const Item &) { return (int)g_settings.gyro_scene; },
                       [](const Item &, int x) { g_settings.gyro_scene = x; }, 1));
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
    v.push_back(choice(P_CONTROLS, "Button icons",
                       "The buttons the game's prompts show. Automatic: the device you used last.",
                       [](const Item &) { return N(k_icons); },
                       [](const Item &, int x) { return tr(k_icons[x]); },
                       [](const Item &) { return g_settings.button_icons; },
                       [](const Item &, int x) { g_settings.button_icons = x; }, ICONS_AUTO));

    // Key bindings: one row per action (key | controller button), then the inputs no action covers. A waits for the
    // next press: a key or mouse button binds the keyboard side, a controller button the controller side
    v.push_back(button(P_BINDS, "", "Press A, then the new key or button. Esc cancels.", "", A_BINDS_HEADER, 0));
    auto bind = [&](int slot, const char *title) {
        Item it = button(P_BINDS, title, "Press A, then the new key or button. Esc cancels.", nullptr, A_BIND, slot);
        it.label = bind_label;
        if (!slot_has_pad(slot)) it.desc = "Press A, then the new key. Esc cancels.";
        v.push_back(it);
    };
    for (int id = 0; id < ACT_COUNT; id++)
        if (!act_hidden(id) && act_name(id)) bind(BIND_ACTION + id, act_name(id));
    // the game's menus: what the Switch buttons do outside the actions, listed by function (never by button name)
    for (int sl : {BIND_PLUS, BIND_MINUS, BIND_UP, BIND_LEFT, BIND_RIGHT, BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R,
                   BIND_ZL, BIND_ZR, BIND_RSTICK})
        bind(sl, bind_input_name(sl));
    // Esc and F2 are hard-wired (the game can always be paused, the cursor freed): shown greyed, never rebound
    v.push_back(button(P_BINDS, "Pause menu / back", "Fixed: this key always works.", "", A_FIXED, 0));
    v.push_back(button(P_BINDS, "Free mouse cursor", "Fixed: this key frees the mouse cursor.", "", A_FIXED, 1));
    for (int sl = BIND_LS_UP; sl <= BIND_RS_RIGHT; sl++) bind(sl, bind_input_name(sl));
    bind(BIND_WALK, bind_input_name(BIND_WALK));
    bind(BIND_TILT, bind_input_name(BIND_TILT));
    v.back().desc = "Hold it and move the mouse to tilt the controller. Press A, then the new key.";
    static const char *const gyro_help[] = {"Tilt upward.", "Tilt downward.", "Tilt left.", "Tilt right.",
                                            "Rotate left on the third axis.", "Rotate right on the third axis.",
                                            "Reset orientation and stop pending rotation."};
    for (int sl = BIND_GYRO_UP; sl <= BIND_GYRO_CENTER; sl++) {  // (the virtual gyro at a shrine apparatus)
        bind(sl, bind_input_name(sl));
        v.back().desc = gyro_help[sl - BIND_GYRO_UP];
    }
    v.push_back(button(P_BINDS, "Reset all bindings", "Every key and button back to its default.", "Reset", A_RESET_BINDS, 0));

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

    // Accessibility (runtime/assists.c): applied at once, while playing
    v.push_back(toggle(P_ACCESS, "God mode", "Link takes no damage.",
                       [](const Item &) { return (int)g_settings.god_mode; },
                       [](const Item &, int x) { g_settings.god_mode = x; }, 0));
    v.push_back(toggle(P_ACCESS, "One-hit kill", "Enemies go down in one hit.",
                       [](const Item &) { return (int)g_settings.one_hit_kill; },
                       [](const Item &, int x) { g_settings.one_hit_kill = x; }, 0));
    for (int dealt = 0; dealt < 2; dealt++) {
        Item it = choice(P_ACCESS, dealt ? "Damage dealt" : "Damage taken",
                         dealt ? "How much damage enemies take. 100% is the game's own."
                               : "How much damage Link takes. 100% is the game's own.",
                         [](const Item &) { return N(k_damage); },
                         [](const Item &, int x) {
                             static std::string s;
                             s = pct(k_damage[x]);
                             return s.c_str();
                         },
                         nullptr, nullptr, DAMAGE_DEFAULT);
        it.arg = dealt;
        it.get = [](const Item &i) {
            return nearest(k_damage, N(k_damage), i.arg ? g_settings.damage_dealt : g_settings.damage_taken);
        };
        it.set = [](const Item &i, int x) { (i.arg ? g_settings.damage_dealt : g_settings.damage_taken) = k_damage[x]; };
        v.push_back(it);
    }
    v.push_back(toggle(P_ACCESS, "Unbreakable weapons", "Weapons, bows and shields lose no durability and never break.",
                       [](const Item &) { return (int)g_settings.no_weapon_wear; },
                       [](const Item &, int x) { g_settings.no_weapon_wear = x; }, 0));
    v.push_back(toggle(P_ACCESS, "Infinite arrows", "Shooting a bow uses up no arrows. You still need one to shoot.",
                       [](const Item &) { return (int)g_settings.infinite_arrows; },
                       [](const Item &, int x) { g_settings.infinite_arrows = x; }, 0));
    v.push_back(toggle(P_ACCESS, "Infinite stamina", "The stamina wheel never runs out.",
                       [](const Item &) { return (int)g_settings.infinite_stamina; },
                       [](const Item &, int x) { g_settings.infinite_stamina = x; }, 0));

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

// Three rows plus the column header row. No guest object is enlarged and no shared archive is changed.
// The row band is the toggle part's own plate (Pa_Btn_00, hidden natively, drawn before both buttons): full width, bright
// while the row is selected. Under the button that is not selected, that part's box (a darkener) cancels the band
// shining through the semi-transparent native box, so that button keeps its base colour in a selected row.
struct TwoRow {
    uint64_t row, tog, band, disable, dark, box[2];
    float dark_x[2];
    int dark_col;  // the column the darkener sits under now (-1: not placed yet)
};
std::vector<TwoRow> g_two_rows;
uint64_t g_band_row;
int g_band_col;
uint64_t g_two_header;
int g_two_column;
uint64_t g_two_current;
bool g_two_focusing;
// Debug log of the two-button rows (SAVAGEWIND_ROW_SPIKE_LOG=1); every stderr line costs about 6 ms on Windows
bool two_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_ROW_SPIKE_LOG") != nullptr;
    return on;
}

bool is_binds_header(const Item *it) { return it && it->action == A_BINDS_HEADER; }
bool row_is_header(uint64_t row) { return row && (row == g_two_header || is_binds_header(item_of_row(row))); }
bool two_enabled(void) {
    return g_site.opt_two_redraw && g_site.opt_two_text && g_site.opt_two_help &&
           g_site.opt_two_pane_calc && g_game_version && !strcmp(g_game_version, "1.9.0") &&
           (!getenv("SAVAGEWIND_TWO_BUTTON_ROWS") || strcmp(getenv("SAVAGEWIND_TWO_BUTTON_ROWS"), "0"));
}
float two_header_scale(void) {
    const char *env = getenv("SAVAGEWIND_HEADER_SCALE");
    if (env && *env) {
        char *end = nullptr;
        float v = strtof(env, &end);
        if (end != env && isfinite(v) && v > 0.1f && v <= 4.f) return v;
    }
    return binding_row_header_scale;
}
bool two_candidate(uint64_t row) {
    const Item *it = item_of_row(row);
    if (g_cur_page != P_BINDS || !two_enabled() || !it) return false;
    if (is_binds_header(it)) return true;
    return it->action == A_BIND || it->action == A_FIXED;  // every binding row: label, keyboard/mouse button, controller button
}
bool two_ready(uint64_t row) {
    for (const auto &r : g_two_rows) if (r.row == row) return true;
    return false;
}
uint64_t two_call(uint64_t fn, uint64_t a = 0, uint64_t b = 0, uint64_t d = 0) {
    Cpu *c = kernel_current_cpu();
    if (!c) return 0;
    Cpu saved = *c;
    c->x[0] = a; c->x[1] = b; c->x[2] = d;
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}
bool two_ptr(uint64_t p) { return p >= 0x7000000000ull && p < 0x7320000000ull; }
uint64_t two_pane(uint64_t root, const char *name, unsigned depth = 0) {
    if (!two_ptr(root) || depth > 16) return 0;
    if (!strncmp((const char *)(uintptr_t)(root + 0xb8), name, 40)) return root;
    uint64_t head = root + 0x20, link = rd64(head + 8);
    for (unsigned n = 0; n < 128 && two_ptr(link) && link != head; n++, link = rd64(link + 8))
        if (uint64_t p = two_pane(link - 8, name, depth + 1)) return p;
    return 0;
}
void two_float(uint64_t pane, unsigned field, float value) {
    *(float *)(uintptr_t)(pane + field) = value;
    // Pane::Calculate at opt_two_pane_calc consumes this bit, rebuilds +0x70,
    // then passes force=true to descendants. Never edit the cached world matrix.
    wr8(pane + 0x58, *(uint8_t *)(uintptr_t)(pane + 0x58) | 0x10);
}
struct TwoButtonPanes {
    uint64_t root, texts[2], frames[8], plate;
};
bool two_button_panes(uint64_t root, TwoButtonPanes &p) {
    p.root = root;
    const char *frames[] = {"N_Cursor_00", "B_Hit_00", "W_Base_00", "W_Base_01",
                            "W_BaseLine_02", "W_BaseLine_01", "W_Decide_00", "Window_00"};
    for (int i = 0; i < 2; i++)
        if (!(p.texts[i] = two_pane(root, i ? "T_Text_01" : "T_Text_00"))) return false;
    for (int i = 0; i < 8; i++)
        if (!(p.frames[i] = two_pane(root, frames[i]))) return false;
    return (p.plate = two_pane(root, "W_Base_02")) != 0;
}
void two_geometry(const TwoButtonPanes &p, BindingRowColumn col) {
    const auto layout = binding_row_layout();
    two_float(p.root, 0x30, col.center());
    two_float(p.root, 0x48, 1.f);
    // Resize native nine-slice frames, hit area and cursor together. Glyphs stay at native scale.
    for (uint64_t text : p.texts) {
        two_float(text, 0x48, 1.f);
        two_float(text, 0x4c, 1.f);
        two_float(text, 0x50, col.width - layout.text_inset);
    }
    for (int i = 0; i < 8; i++)
        two_float(p.frames[i], 0x50, col.width + (i == 7 ? layout.glow_extra : 0.f));
    // The part's own row plate is drawn over the first button's box when it is the second part, and is bright only while
    // its own button is the selected one: both are hidden, the toggle part's plate is the row band (two_band).
    wr8(p.plate + 0x58, *(uint8_t *)(uintptr_t)(p.plate + 0x58) & ~1u);
}
void two_header_geometry(const TwoButtonPanes &p, BindingRowColumn col, float scale) {
    two_float(p.root, 0x30, col.center());
    two_float(p.root, 0x48, 1.f);
    for (uint64_t text : p.texts) {
        two_float(text, 0x30, 0.f);
        two_float(text, 0x34, binding_row_header_y);
        two_float(text, 0x48, scale);
        two_float(text, 0x4c, scale);
        two_float(text, 0x50, col.width / scale);
    }
    for (uint64_t frame : p.frames)
        wr8(frame + 0x58, *(uint8_t *)(uintptr_t)(frame + 0x58) & ~1u);
    wr8(p.plate + 0x58, *(uint8_t *)(uintptr_t)(p.plate + 0x58) & ~1u);
}
// Window pane colours are materials: the white colour's alpha is the brightness (band, box). Seen at two places of the pane.
constexpr unsigned two_mat_alpha[2] = {0x16f, 0x207};
constexpr uint8_t two_band_selected = 0x10, two_band_faint = 0x02;  // the native values of a focused / unfocused plate
constexpr uint8_t two_dark_alpha = 0x92;   // the darkener under the other button (one value fits both columns' backdrop)
constexpr uint8_t two_hover_alpha = 0xde;  // pane alpha of the selected button's box: the band shows through a little
// The selected button's box is black natively; a little white (linear colour) in it lifts it a notch above the others.
constexpr unsigned two_mat_white[2] = {0x16c, 0x204};
constexpr uint8_t two_hover_tint = 0x06;
void two_alpha(uint64_t pane, uint8_t v) {
    for (unsigned o : two_mat_alpha) if (*(uint8_t *)(uintptr_t)(pane + o) != v) wr8(pane + o, v);
}
void two_tint(uint64_t pane, uint8_t v) {
    for (unsigned o : two_mat_white)
        for (unsigned i = 0; i < 3; i++)
            if (*(uint8_t *)(uintptr_t)(pane + o + i) != v) wr8(pane + o + i, v);
}
void two_show(uint64_t pane, bool on) {
    uint8_t f = *(uint8_t *)(uintptr_t)(pane + 0x58);
    if ((f & 1u) != (on ? 1u : 0u)) wr8(pane + 0x58, on ? f | 0x11u : (f & ~1u) | 0x10u);
}
void two_pane_alpha(uint64_t pane, uint8_t v) {
    if (*(uint8_t *)(uintptr_t)(pane + 0x59) == v) return;
    wr8(pane + 0x59, v);
    wr8(pane + 0x58, *(uint8_t *)(uintptr_t)(pane + 0x58) | 0x10u);
}
void two_band_row(TwoRow &r, bool selected, int col) {
    two_show(r.tog, true);
    two_show(r.band, true);
    two_alpha(r.band, selected ? two_band_selected : two_band_faint);
    two_show(r.disable, selected);
    if (selected) {
        int other = 1 - col;
        if (r.dark_col != other) {
            two_float(r.dark, 0x30, r.dark_x[other]);
            r.dark_col = other;
        }
        two_alpha(r.dark, two_dark_alpha);
    }
    for (int i = 0; i < 2; i++) {
        bool hover = selected && i == col;
        two_pane_alpha(r.box[i], hover ? two_hover_alpha : 0xff);
        two_tint(r.box[i], hover ? two_hover_tint : 0);
    }
}
// One row at a time is the selected one; the rest show the faint band. Also run every frame (two_band_refresh): the
// game redraws a row's parts when a binding changes.
void two_band(uint64_t selected, int col) {
    g_band_row = selected; g_band_col = col;
    for (auto &r : g_two_rows) two_band_row(r, r.row == selected, col);
}
// The native selection brackets are not clipped by the list: when the wheel scrolls the selected row out of view they
// would stay behind, floating over the title. So after a wheel scroll a selection outside the list moves to the nearest
// row inside it. (Rows are 50 apart; y is 150 for the first row at scroll 0, grows as the list scrolls down, and the list
// shows about y -305 .. 205 with the outer rows faded. A row the list no longer draws stops updating its y, so the selected
// row's position is carried along with the scroll offset from the last frame its y changed.)
void two_focus(uint64_t screen, uint64_t row, int col);
int g_wheel_frames;
struct SelTrack { uint64_t row; float raw_y, y, off; bool seen; } g_sel;
void two_wheel_scrolled(void) { g_wheel_frames = 6; }
void two_follow_wheel(uint64_t screen) {
    if (g_wheel_frames <= 0) return;
    g_wheel_frames--;
    uint64_t row = g_band_row;
    const int col = g_band_col < 0 ? 0 : g_band_col;
    if (!row || !two_ready(row)) return;
    auto hit_y = [&](uint64_t r) {
        uint64_t button = rd64(r + 0x20), hit = two_ptr(button) ? rd64(button + 0x60) : 0;
        return two_ptr(hit) ? *(float *)(uintptr_t)(hit + 0x8c) : NAN;
    };
    const float off = *(float *)(uintptr_t)(screen + 0x3628 + 0x2c), raw = hit_y(row);
    if (!isfinite(off) || !isfinite(raw)) return;
    if (g_sel.row != row || !g_sel.seen || raw != g_sel.raw_y) g_sel = {row, raw, raw, off, true};
    const float y = g_sel.y + (off - g_sel.off);
    if (y <= 205.f && y >= -305.f) return;
    uint64_t best = 0;
    float best_y = 0;
    for (const auto &r : g_two_rows) {
        if (r.row == row || row_is_header(r.row)) continue;
        const float ry = hit_y(r.row);
        if (!isfinite(ry) || ry > 150.f || ry < -250.f) continue;
        if (!best || (y > 205.f ? ry > best_y : ry < best_y)) best = r.row, best_y = ry;
    }
    if (best) two_focus(screen, best, col);
}
void two_band_refresh(uint64_t screen) {
    if (g_two_rows.empty() || rd64(g_two_rows[0].row) != screen) return;
    two_band(g_band_row, g_band_col);
    two_follow_wheel(screen);
}
uint64_t first_selectable_row(uint64_t screen) {
    if (!screen) return 0;
    uint32_t nrows = rd32(screen + g_site.opt_screen_rows);
    uint64_t rows = rd64(screen + g_site.opt_screen_rows + 8);
    if (!rows || !nrows || nrows > 64) return g_two_rows.empty() ? 0 : g_two_rows[0].row;
    uint32_t norder = g_site.opt_table ? rd32(OPT_ROWS) : 0;
    uint64_t order = g_site.opt_table ? rd64(OPT_ORDER) : 0;
    for (uint32_t oi = 0; oi < norder && oi < 64 && order; oi++) {
        uint32_t want = rd32(order + 4 * oi);
        if (is_binds_header(item_of_index(want))) continue;
        for (uint32_t ri = 0; ri < nrows; ri++) {
            uint64_t r = rd64(rows + 8 * ri);
            if (r && rd32(r + 0x50) == want) return r;
        }
    }
    for (uint32_t ri = 0; ri < nrows; ri++) {
        uint64_t r = rd64(rows + 8 * ri);
        if (r && !row_is_header(r)) return r;
    }
    return 0;
}
uint64_t row_active_control(uint64_t r) {
    if (!r) return 0;
    if (two_ready(r)) return rd64(r + (g_two_column ? 0x28 : 0x20));
    const Item *it = item_of_row(r);
    int kind = it ? it->kind : KIND_BUTTON;
    return rd64(r + (kind == KIND_TOGGLE ? 0x18 : kind == KIND_CHOICE ? 0x20 : 0x28));
}
void two_focus(uint64_t screen, uint64_t row, int col) {
    if (g_two_focusing) return;
    g_two_focusing = true;
    g_two_column = col; g_two_current = row;
    pc_cursor_options_focus(screen, row, col ? KIND_BUTTON : KIND_CHOICE);
    g_two_focusing = false;
}
void two_text(uint64_t layout, const char *pane, uint32_t id) {
    uint64_t name[] = {g_entries[0].name_vtbl, (uint64_t)(uintptr_t)pane};
    two_call(g_site.opt_two_text, layout, (uint64_t)(uintptr_t)name, id);
}
void hide_failed_header(uint64_t row) {
    uint64_t btn = rd64(row + 0x28);
    if (two_ptr(btn) && two_ptr(rd64(btn)) && two_ptr(rd64(rd64(btn) + 0x60)))
        two_call(rd64(rd64(btn) + 0x60), btn, 0);
    if (uint64_t root = rd64(row + 0x80); two_ptr(root))
        wr8(root + 0x58, *(uint8_t *)(uintptr_t)(root + 0x58) & ~1u);
    g_two_header = row;
}
// The scale that fits a row name before the keyboard button: widths are estimated per character (the game's font is
// proportional): narrow marks count less, kana / hanzi / hangul double, Thai combining marks nothing.
float label_fit(const char *text) {
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        unsigned cp = *p, n = 1;
        if (cp >= 0xf0) cp &= 0x07, n = 4;
        else if (cp >= 0xe0) cp &= 0x0f, n = 3;
        else if (cp >= 0xc0) cp &= 0x1f, n = 2;
        for (unsigned i = 1; i < n && p[i]; i++) cp = cp << 6 | (p[i] & 0x3f);
        p += n;
        if (cp == 0x0e31 || (cp >= 0x0e34 && cp <= 0x0e3a) || (cp >= 0x0e47 && cp <= 0x0e4e)) continue;
        const bool narrow = cp == ' ' || cp == '.' || cp == ',' || cp == ':' || cp == ';' || cp == '\'' || cp == '!' ||
                            cp == '|' || cp == 'i' || cp == 'l' || cp == '(' || cp == ')' || cp == '/' || cp == '-';
        w += cp >= 0x2e80 ? 2.f : narrow ? .55f : 1.f;
    }
    const float room = binding_row_layout().label_width / 9.7f;  // (9.7 layout units per average character)
    return w <= room ? 1.f : fmaxf(room / w, .62f);
}
// The label box (right edge fixed, text left-aligned); a name still too long shrinks to fit before the first button
void label_geometry(uint64_t title, const char *text) {
    const auto layout = binding_row_layout();
    const float fit = label_fit(text);
    two_float(title, 0x30, layout.label_right);
    two_float(title, 0x48, fit);
    two_float(title, 0x4c, fit);
    two_float(title, 0x50, layout.label_width / fit);  // (scaled about its centre: the box grows so its edges stay)
}
// The Reset row's button spans both columns (key and controller), like the rows above it
void wide_button(uint64_t row) {
    uint64_t button = rd64(row + 0x28);
    if (!two_ptr(button) || !two_ptr(rd64(button + 0x20))) return;
    uint64_t root = rd64(rd64(button + 0x20) + 0x18);
    TwoButtonPanes p;
    if (!two_ptr(root) || !two_button_panes(root, p)) return;
    const auto layout = binding_row_layout();
    const uint8_t plate = *(uint8_t *)(uintptr_t)(p.plate + 0x58);  // (the row's own band: stays)
    const BindingRowColumn span{layout.key.x, layout.pad.x + layout.pad.width - layout.key.x};
    // The band is a child of the button and would move with it: its offset takes the move back
    const float moved = *(float *)(uintptr_t)(root + 0x30) - span.center();
    if (fabsf(moved) > .01f) two_float(p.plate, 0x30, *(float *)(uintptr_t)(p.plate + 0x30) + moved);
    two_geometry(p, span);
    two_float(p.plate, 0x50, binding_row_plate_width);
    wr8(p.plate + 0x58, plate | 0x10);
    uint64_t row_layout = rd64(row + 0x10);
    uint64_t layout_root = two_ptr(row_layout) ? rd64(row_layout + 0x18) : 0;
    uint64_t title = two_ptr(layout_root) ? two_pane(layout_root, "T_Option_00") : 0;
    if (title) if (const Item *it = item_of_row(row)) label_geometry(title, tr(it->title));
}
bool two_configure(uint64_t row) {
    bool header = is_binds_header(item_of_row(row));
    if (header ? g_two_header == row : two_ready(row)) return true;
    if (!two_candidate(row) || !kernel_current_cpu()) {
        if (header && kernel_current_cpu()) hide_failed_header(row);
        return false;
    }
    uint64_t roots[2], buttons[2], arrows[4];
    TwoButtonPanes panes[2];
    const char *arrow_names[] = {"Pa_ArrowL_00", "Pa_ArrowR_00", "P_Arrow_00", "P_Arrow_01"};
    for (int col = 0; col < 2; col++) {
        buttons[col] = rd64(row + (col ? 0x28 : 0x20));
        if (!two_ptr(buttons[col]) || !two_ptr(rd64(buttons[col] + 0x20))) {
            if (header) hide_failed_header(row);
            return false;
        }
        roots[col] = rd64(rd64(buttons[col] + 0x20) + 0x18);
        if (!two_ptr(roots[col]) || !two_ptr(rd64(buttons[col] + 0x60)) || !two_button_panes(roots[col], panes[col])) {
            if (header) hide_failed_header(row);
            return false;
        }
    }
    for (int i = 0; i < 4; i++) {
        arrows[i] = two_pane(roots[0], arrow_names[i]);
        if (!arrows[i]) {
            if (header) hide_failed_header(row);
            return false;
        }
    }
    uint64_t row_layout = rd64(row + 0x10);
    uint64_t layout_root = two_ptr(row_layout) ? rd64(row_layout + 0x18) : 0;
    uint64_t title = two_ptr(layout_root) ? two_pane(layout_root, "T_Option_00") : 0;
    TwoRow band = {row, 0, 0, 0, 0, {panes[0].frames[2], panes[1].frames[2]}, {0, 0}, -1};
    if (!header && title) {
        band.tog = two_pane(layout_root, "Pa_Btn_00");
        band.band = two_pane(band.tog, "W_Base_02");
        band.disable = two_pane(band.tog, "N_Disable_00");
        band.dark = two_pane(band.disable, "W_Base_00");
        if (!band.band || !band.disable || !band.dark) title = 0;
    }
    if (!title) {
        if (header) hide_failed_header(row);
        return false;
    }
    // Validate all panes before the first mutation, leaving ordinary rows usable on failure.
    const auto layout = binding_row_layout();
    float hscale = header ? two_header_scale() : 1.f;
    for (int col = 0; col < 2; col++) {
        two_call(rd64(rd64(buttons[col]) + 0x60), buttons[col], header ? 0 : 1);
        wr8(roots[col] + 0x58, *(uint8_t *)(uintptr_t)(roots[col] + 0x58) | 1);
        const auto column = col ? layout.pad : layout.key;
        if (header) two_header_geometry(panes[col], column, hscale);
        else two_geometry(panes[col], column);
    }
    for (uint64_t arrow : arrows) wr8(arrow + 0x58, *(uint8_t *)(uintptr_t)(arrow + 0x58) & ~1u);
    if (header) {
        wr8(title + 0x58, *(uint8_t *)(uintptr_t)(title + 0x58) & ~1u);
        g_two_header = row;
    } else {
        if (const Item *it = item_of_row(row)) label_geometry(title, tr(it->title));
        // The toggle part: only its plate and its box stay (its other panes are hidden for good).
        float togx = *(float *)(uintptr_t)(band.tog + 0x30);
        for (uint64_t c = band.tog, n = 0; n < 2; n++) {
            uint64_t head = c + 0x20, link = rd64(head + 8);
            for (unsigned k = 0; k < 16 && two_ptr(link) && link != head; k++, link = rd64(link + 8)) {
                uint64_t child = link - 8;
                if (child == band.band || child == band.disable || child == band.dark) continue;
                wr8(child + 0x58, *(uint8_t *)(uintptr_t)(child + 0x58) & ~1u);
            }
            c = band.disable;
        }
        two_float(band.band, 0x50, binding_row_plate_width);
        band.dark_x[0] = layout.key.center() - togx;
        band.dark_x[1] = layout.pad.center() - togx;
        two_float(band.dark, 0x48, 1.f);
        two_float(band.dark, 0x4c, 1.f);
        two_float(band.dark, 0x34, 0.f);
        two_float(band.dark, 0x50, layout.key.width);
        two_float(band.dark, 0x54, *(float *)(uintptr_t)(panes[0].frames[2] + 0x54));
        for (int col = 0; col < 2; col++) wr8(panes[col].plate + 0x58, *(uint8_t *)(uintptr_t)(panes[col].plate + 0x58) & ~1u);
        g_two_rows.push_back(band);
        two_band_row(g_two_rows.back(), false, 0);
    }
    return true;
}

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

// Only the binding rows of these two slots (the one just bound and the one it swapped with): a page of ~55 rows
// redrawn one per frame took two seconds to show the new key
void mark_bind_rows(uint64_t screen, int slot, int other) {
    if (!screen) return;
    int n = (int)rd32(screen + g_site.opt_screen_rows);
    uint64_t rows = rd64(screen + g_site.opt_screen_rows + 8);
    for (int i = 0; i < n && i < 64 && rows; i++) {
        uint64_t r = rd64(rows + 8 * i);
        const Item *it = item_of_row(r);
        if (it && it->action == A_BIND && (it->arg == slot || it->arg == other)) g_dirty.push_back(r);
    }
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

// A message for the game ({char16_t *, u32 length with the terminator} at out); id: its storage. grey: in the
// game's grey (its message colour tag: 0x0e, group 0, tag 3, 2 bytes of colour; 0xffff back to the text's own).
// Right-to-left menus: the game draws text left to right, so it gets each line in visual order.
void put_text(uint32_t id, const char *s, uint64_t out, bool grey = false) {
    std::u16string &t = g_text[id];
    std::u16string n = utf16(ui_visual_order(s).c_str());
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
    if (page == P_COLOUR) g_nluts = color_lut_list();  // (files the player added since the last opening)
    for (size_t i = 0; i < g_items.size(); i++) {  // (how many window sizes there are depends on the display)
        Entry &e = g_entries[GAME_ENTRIES + i];
        if (g_items[i].kind == KIND_CHOICE) e.last_value = e.first_value + game_count(g_items[i]) - 1;
    }
    const uint32_t *order_data = o.data();
    uint32_t order_count = (uint32_t)o.size();
    if (page == P_BINDS && order_count > 0 && !two_enabled() && is_binds_header(item_of_index(order_data[0]))) {
        order_data++;
        order_count--;
    }
    wr64(OPT_ORDER, (uint64_t)(uintptr_t)order_data);
    wr32(OPT_ROWS, order_count);
    pc_cursor_options_page_changed();
    g_cur_page = page;
    g_reopen = false;
    g_dirty.clear();
    g_two_rows.clear(); g_two_header = 0; g_two_column = 0; g_two_current = 0;
}

// Called only by the strict optional hook group. Other pages use their original instructions.
extern "C" int rt_opt_two_column(uint64_t row) { return two_ready(row) ? g_two_column : -1; }
extern "C" uint64_t rt_opt_two_control(uint64_t row) {
    if (row_is_header(row)) {
        uint64_t screen = rd64(row);
        uint64_t first = first_selectable_row(screen);
        if (first && screen) {
            g_two_current = first;
            wr64(screen + 0x36c8, first);
        }
        return row_active_control(first);
    }
    return two_ready(row) ? rd64(row + (g_two_column ? 0x28 : 0x20)) : 0;
}
extern "C" int rt_opt_two_row_state(uint64_t row) {
    if (row_is_header(row)) {
        for (unsigned offset : {0x18u, 0x20u, 0x28u})
            if (uint64_t button = rd64(row + offset); two_ptr(button))
                *(uint16_t *)(uintptr_t)(button + 0x3a) &= ~4u;
        return 1;
    }
    if (!two_ready(row)) return 0;
    for (unsigned offset : {0x20u, 0x28u}) {
        uint64_t button = rd64(row + offset);
        *(uint16_t *)(uintptr_t)(button + 0x3a) |= 4; // both behave as confirmable ordinary controls
    }
    return 1;
}
extern "C" void rt_opt_wheel_scrolled(void) { two_wheel_scrolled(); }
extern "C" int rt_opt_two_redraw(uint64_t row) {
    if (g_cur_page == P_BINDS && two_enabled()) {
        const Item *it = item_of_row(row);
        if (it && it->action == A_RESET_BINDS) wide_button(row);
    }
    if (!two_configure(row)) return 0;
    uint32_t msg = MSG_BASE + (rd32(row + 0x50) - GAME_ENTRIES) * 256;
    for (int col = 0; col < 2; col++) {
        uint64_t layout = rd64(rd64(row + (col ? 0x28 : 0x20)) + 0x20);
        two_text(layout, "T_Text_00", msg + (col ? MSG_PAD : MSG_KEY));
        two_text(layout, "T_Text_01", msg + (col ? MSG_PAD : MSG_KEY));
    }
    rt_opt_two_row_state(row);
    return 1;
}
extern "C" int rt_opt_decide(uint64_t row);
extern "C" int rt_opt_pointer_kind(uint64_t row);
extern "C" int rt_opt_two_callback(uint64_t screen, uint64_t button, int which) {
    if (g_two_header && rd64(g_two_header) == screen) {
        for (unsigned off : {0x18u, 0x20u, 0x28u}) {
            if (button && button == rd64(g_two_header + off)) {
                if (which == 0) {
                    if (uint64_t first = first_selectable_row(screen)) {
                        if (two_ready(first)) two_focus(screen, first, g_two_column);
                        else if (int k = rt_opt_pointer_kind(first); k >= 0)
                            pc_cursor_options_focus(screen, first, (unsigned)k);
                    }
                }
                return 1;
            }
        }
    }
    for (const auto &r : g_two_rows) {
        uint64_t row = r.row;
        if (rd64(row) != screen) continue;
        int col = button == rd64(row + 0x20) ? 0 : button == rd64(row + 0x28) ? 1 : -1;
        if (col < 0) continue;
        if (which == 0) {
            // Up/down keeps the selected device. Mouse focus sets current row first below.
            int want = !g_two_current || g_two_current == row ? col : g_two_column;
            g_two_column = want; g_two_current = row;
            wr64(screen + 0x36c8, row);
            two_call(g_site.opt_two_help, row);
            two_band(row, want);
            if (want != col) two_focus(screen, row, want);
            if (two_log()) fprintf(stderr, "[row-spike] select row %llx column %d\n", (unsigned long long)row, want);
        } else if (which == 1) rt_opt_two_row_state(row);
        else if (which == 2) {
            uint32_t action = rd32(screen + 0x36c0);
            wr32(screen + 0x36c0, 0); // this callback is the native control's confirmed event
            rt_opt_decide(row);
            wr32(screen + 0x36c0, action);
        }
        return 1;
    }
    if (which == 0 && !g_two_rows.empty() && rd64(g_two_rows[0].row) == screen) two_band(0, 0);  // (another row: Reset)
    return 0;
}
extern "C" void rt_opt_two_pointer(uint64_t row, unsigned kind) {
    if (two_ready(row)) g_two_current = row, g_two_column = kind == KIND_BUTTON;
}
extern "C" void rt_opt_two_horizontal(uint64_t screen, unsigned action) {
    uint64_t row = rd64(screen + 0x36c8);
    if (row_is_header(row)) {
        if (uint64_t first = first_selectable_row(screen); two_ready(first))
            two_focus(screen, first, action == 2);
        wr32(screen + 0x36c0, 3);
        return;
    }
    if (!two_ready(row)) return;
    two_focus(screen, row, action == 2);
    wr32(screen + 0x36c0, 3); // movement is never confirmation or a value change
}

// Row init: the flag type of an entry (0 bool, 1 s32); -1: the game's own entry
// The installed entries are a host vector, not guest-heap pointers.
extern "C" int rt_opt_pointer_kind(uint64_t row) {
    if(!g_installed||rd64(OPT_ENTRIES)!=g_installed||!row||game_options_input_hold()||row_is_header(row))return -1;
    uint32_t index=rd32(row+0x50);
    return index<g_entries.size()?(int)g_entries[index].kind:-1;
}

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
    // (the sizes offered follow Fullscreen, texts and Upscaler mode's values the upscaler; Colour: the preset sets the
    // sliders, they the preset). A value kept on show because it couldn't be picked goes once it can (Upscaler mode
    // after Off: the mode stored, not the Off shown).
    if (it->page == P_VIDEO || it->page == P_COLOUR) {
        for (size_t i = 0; i < g_items.size() && i < g_shown.size(); i++) {
            const Item &o = g_items[i];
            if (g_shown[i] >= 0 && (!o.value_off || !o.value_off(o, g_shown[i]))) g_shown[i] = -1;
            if (o.kind == KIND_CHOICE) {
                Entry &e = g_entries[GAME_ENTRIES + i];
                e.last_value = e.first_value + game_count(o) - 1;
            }
        }
        mark_all_rows(rd64(row));
    }
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
    else if (it->action == A_BIND) bind_reset_slot(it->arg);
    else if (it->action == A_RESET_BINDS) bind_reset(BIND_DEV_KEY), bind_reset(BIND_DEV_PAD), act_reset();
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
    if (part == MSG_TITLE) {
        const char *title = it->title;
        // Probe fixture only: exercise long labels without changing the actions being captured.
        if (g_cur_page == P_BINDS && it->action == A_BIND && getenv("SAVAGEWIND_TWO_BUTTON_LABEL_TEST")) {
            if (it->arg == BIND_ACTION) title = "Guard / lock on";
            if (it->arg == BIND_ACTION + 2) title = "Use rune";  // (test fixture, not shown) i18n-skip
            if (it->arg == BIND_ACTION + 3) title = "Master Cycle jump";  // (test fixture, not shown) i18n-skip
        }
        s = tr(title);
    }
    else if (part == MSG_DESC) s = it->desc_fn ? it->desc_fn(*it) : tr(it->desc);
    else if (part == MSG_KEY || part == MSG_PAD) {
        bool key = part == MSG_KEY;
        if (is_binds_header(it)) {
            s = tr(key ? "Keyboard/Mouse" : "Controller");
        } else if (it->action == A_FIXED) {
            grey = true;
            s = key ? tr(bind_key_label(it->arg ? SDL_SCANCODE_F2 : SDL_SCANCODE_ESCAPE)) : "-";
        } else {
            int dev, slot, ms;
            bool capture = bind_capture_active(&dev, &slot, &ms) && slot == it->arg &&
                           dev == (key ? BIND_DEV_KEY : BIND_DEV_PAD);
            if (!key && !slot_has_pad(it->arg)) s = tr("Stick"), grey = true;  // (keys only: a stick direction)
            else s = capture ? tr(key ? "Press a key..." : "Press a button...") :
                              key ? slot_key_text(it->arg) : slot_pad_text(it->arg);
        }
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
    case A_FIXED:
        return 2;
    case A_BIND:
        if (two_ready(row) && rd32(rd64(row) + 0x36c0) != 0) return 2;
        if (two_ready(row) && g_two_column && !slot_has_pad(it->arg)) return 2;  // (no controller side to bind)
        bind_capture_begin(two_ready(row) ? (g_two_column ? BIND_DEV_PAD : BIND_DEV_KEY) : BIND_DEV_ANY, it->arg);
        cap_slot_was = it->arg;
        if (two_ready(row) && two_log()) fprintf(stderr, "[row-spike] capture slot %d device %s\n", it->arg,
                                   g_two_column ? "pad" : "key");
        g_cap_row = row;
        g_dirty.push_back(row);
        return 2;
    case A_RESET_BINDS:
        bind_reset(BIND_DEV_KEY);
        bind_reset(BIND_DEV_PAD);
        act_reset();
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
    if (g_cap_row && bind_capture_active(&d, &s, &ms)) cap_slot_was = s;
    if (g_cap_row && !bind_capture_active(&d, &s, &ms)) {
        g_cap_row = 0;
        g_hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        settings_save();
        int rd, rs, other, serial;
        bind_last_result(&rd, &rs, &other, &serial);
        mark_bind_rows(screen, cap_slot_was, other);
    }
    if (g_size_redraw.exchange(false) && g_size_row) g_dirty.push_back(g_size_row);
    if (!game_options_input_hold() && !menu_size_list_up()) pc_cursor_options_tick(screen);  // (the list has the pointer)
    two_band_refresh(screen);
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
    if (g_installed && g_cur_page != P_MAIN) {
        g_next_page = P_MAIN;
        g_reopen = g_switching = true;
    } else {
        g_reopen = g_switching = false;
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

// The title menu's update (menu = the screen): 1 = open the Options screen again now (it has gone, a page is due).
// The quit question's answer is read here too.
extern "C" int rt_opt_title_tick(uint64_t menu) {
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

extern "C" uint64_t rt_opt_screen(void) { return options_screen(); }

// ---- Quit: the game's two-button dialog (screen 0x39, type 7) asks: desktop (upper button) or the start screen
// (lower). Its buttons write their answer to dialog_answer (1.8.2 0x7001c4b308; upper 0x5b: 0, lower 0x5a: 1;
// FUN_7100d46598) and leave the dialog up; B closes it without an answer (rt_quit_dialog_back).

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
