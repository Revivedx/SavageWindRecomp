// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "../color_grade.h"
#include "../window_size.h"
extern "C" {
#include "../language.h"
}
#include "menu.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../perfmon.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_window_apply{false};  // fullscreen or the window size changed
// (also from the game's Options screen, game_options.mm)
extern "C" void menu_request_window_apply(void) { g_want_window_apply.store(true); }
// The window size stepped through (left / right): applied once it stays put a moment, not at every size on the way
static std::atomic<uint64_t> g_size_apply_at{0};  // SDL_GetTicks() then (0: none)
extern "C" void menu_request_window_size_apply(void) { g_size_apply_at.store(SDL_GetTicks() + 600); }
extern "C" bool game_options_input_hold(void);  // game_options.mm: a binding capture from the game's Options screen

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("SAVAGEWIND_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("SAVAGEWIND_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
// Window size's list over the game's Options screen (game_options.mm A_SIZE_LIST): up, and until when the press that
// closed it is kept from the game
static std::atomic<bool> g_sizes{false};
static std::atomic<uint64_t> g_sizes_hold{0};  // SDL_GetTicks()
extern "C" void game_options_size_picked(void);  // game_options.mm (the row shows the new size)
extern "C" void menu_size_list_open(void) {
    input_reset();
    g_sizes = true;
}
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || g_sizes.load(); }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up, or a binding capture begun
// from the game's Options screen.
extern "C" bool menu_input_captured(void) {
    return capturing() || game_options_input_hold() || SDL_GetTicks() < g_sizes_hold.load();
}

// In game the settings are the game's own Options screen (game_options.mm): F1, Esc and the controller's home button no
// longer open this menu (SAVAGEWIND_OVERLAY=1 brings it back). Esc frees the mouse cursor instead, a click in the
// window holds it again (runtime/hle/input.c).
static bool overlay_enabled(void) {
    static const bool on = getenv("SAVAGEWIND_OVERLAY") && *getenv("SAVAGEWIND_OVERLAY") != '0';
    return on;
}
static std::atomic<bool> g_cursor_free{false};
extern "C" bool menu_cursor_free(void) { return g_cursor_free.load(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    // a binding capture (Controls tab, or the game's Options screen) takes the press
    if (bind_capture_event(e)) return true;
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && g_cursor_free.load() && !capturing()) {
        g_cursor_free = false;  // (the click only takes the cursor back)
        return true;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!overlay_enabled()) {
                if (e->key.scancode == SDL_SCANCODE_ESCAPE && !typing) g_cursor_free = !g_cursor_free.load();
                return true;
            }
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && e->key.scancode == SDL_SCANCODE_F10 && !typing) {  // performance overlay: off, FPS, full
            g_settings.perf_overlay = (g_settings.perf_overlay + 1) % 3;
            settings_save();
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_window_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (g_sizes.load()) {  // Window size's list (in game): the keys bound to A, B and up / down work it too
            int sc = e->key.scancode;
            if (sc == g_bind_key[BIND_A]) k = ImGuiKey_Enter;
            else if (sc == g_bind_key[BIND_B]) k = ImGuiKey_Escape;
            else if (sc == g_bind_key[BIND_LS_UP] || sc == g_bind_key[BIND_UP]) k = ImGuiKey_UpArrow;
            else if (sc == g_bind_key[BIND_LS_DOWN] || sc == g_bind_key[BIND_DOWN]) k = ImGuiKey_DownArrow;
        }
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && overlay_enabled() &&
            ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (g_sizes.load()) {  // Window size's list (in game): the buttons bound to A and B, wherever they are
            int b = e->gbutton.button;
            if (b == g_bind_pad[BIND_A]) k = ImGuiKey_GamepadFaceDown;
            else if (b == g_bind_pad[BIND_B]) k = ImGuiKey_GamepadFaceRight;
        }
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    // (the window was made at its size and fullscreen mode: ui_window_init)
    uint64_t at = g_size_apply_at.load();
    if (at && SDL_GetTicks() >= at && g_size_apply_at.compare_exchange_strong(at, 0)) g_want_window_apply.store(true);
    if (g_want_window_apply.exchange(false)) winsize_apply(w);
    else winsize_refresh(w);  // (moved to another display, or its desktop size changed: other sizes are offered)
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Fira Sans); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


// The fonts, with the menu language's glyphs that Latin fonts lack merged in (runtime/ui/i18n.mm); made again when the
// menu language changes (fonts_follow_language).
static int g_fonts_lang = -1;
static void load_fonts(void) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_font = g_font_big = g_menu_font = g_menu_font_big = NULL;
    g_fonts_lang = ui_lang_generation();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    auto add = [&](const char *path, float size, ImFontConfig c) {
        ImFont *f = io.Fonts->AddFontFromFileTTF(path, size, &c);
        if (f) ui_merge_fallback_font(io.Fonts, size, &cfg);
        return f;
    };
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = add(p, 34, cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = add(p, 56, b); }
    }
    // the menu window itself: Fira Sans, as on the start screen (shipped, OFL) - Medium for the rows, SemiBold for the
    // titles - at its Settings window's sizes; the FPS counter, overlays and on-screen keyboard keep the fonts above
    char med[1024], semi[1024];
    const char *test = getenv("SAVAGEWIND_MENU_FONT");  // (test) another weight / face
    if (test) strlcpy(med, test, sizeof med), strlcpy(semi, test, sizeof semi);
    if (test || (rt_resource("FiraSans-Medium.ttf", "third_party/fonts/FiraSans/FiraSans-Medium.ttf", med, sizeof med) &&
                 rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", semi, sizeof semi))) {
        g_menu_font = add(med, 29, cfg);
        g_menu_font_big = add(semi, 53.5f, cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
}

// Before a frame: another menu language may need other glyphs - the fonts and their texture are made again
extern "C" void vk_imgui_fonts_changed(void);  // (runtime/nvn/vulkan/imgui_glue.cpp)
static void fonts_follow_language(void) {
    if (g_fonts_lang == ui_lang_generation()) return;
    load_fonts();
#ifdef __APPLE__
    ImGui_ImplMetal_DestroyFontsTexture();  // (NewFrame makes it again)
#else
    vk_imgui_fonts_changed();
#endif
}

#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    load_fonts();
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header in the big font.
static void header(const char *t) {
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr(t));
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(t));
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", tr(names[*value]))) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tr(names[i]), i == *value)) { *value = i; changed = true; }
            ImGui::PopID();
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scene_scale(float *sx, float *sy);
extern "C" bool renderer_gpu_rtx(void);  // (vulkan/device.c, metal.m)
extern "C" {
#include "../nvn/output_scale.h"
}

#ifndef __APPLE__
// Colour (runtime/color_grade.h; vulkan/exec.c scene_post), the Video tab's last section: a preset sets the sliders,
// which then show "Custom" when they match none
static void tab_colour(void) {
    opt_section("Colour");
    {
        static std::string names_buf[16];
        const char *names[16];
        bool avail[16];
        int n = g_color_preset_count;
        for (int i = 0; i <= n && i < 16; i++) {
            names_buf[i] = tr(i < n ? g_color_presets[i].name : "Custom");
            names[i] = names_buf[i].c_str();
            avail[i] = i < n;
        }
        static int shown = -1;
        int v = color_preset_current();
        if (v < 0) v = n;  // (custom: shown, never picked)
        if (opt_choice_avail("Colour preset", &v, &shown, names, avail, n + 1, 0,
                             "Ready-made looks for the 3D image, made from the sliders below: Natural is the game's "
                             "own colours, Warm and Cool shift the light, Vivid and Cinematic add contrast, Muted "
                             "calms the colours. The HUD and menus keep their colours.") && v < n)
            color_preset_apply(v);
    }
    float ev = g_settings.color_exposure / 10.0f;
    if (opt_slider("Exposure", &ev, -2, 2, 0.1f, 0, "%+.1f EV",
                   "Makes the 3D image brighter or darker, in stops: +1 EV is twice as bright."))
        g_settings.color_exposure = (int)lroundf(ev * 10);
    opt_int("Contrast", &g_settings.color_contrast, -50, 50, 5, 0, "%+d",
            "More (or less) difference between dark and bright areas. Shadows and highlights keep their detail.");
    opt_int("Saturation", &g_settings.color_saturation, -100, 100, 5, 0, "%+d",
            "How strong the colours are. -100: black and white.");
    opt_int("Temperature", &g_settings.color_temperature, -100, 100, 5, 0, "%+d",
            "The colour of the light: below 0 cooler (bluer), above 0 warmer (more orange).");
    opt_int("Tint", &g_settings.color_tint, -100, 100, 5, 0, "%+d",
            "Below 0 toward green, above 0 toward magenta: evens out a colour cast.");
    {   // LUT: None, the shipped ones, the player's own (listed again every couple of seconds while shown)
        static int n;
        static uint64_t at;
        if (!n || SDL_GetTicks() - at > 2000) n = color_lut_list(), at = SDL_GetTicks();
        static std::string none;
        none = tr("None");
        const char *names[COLOR_LUT_MAX];
        for (int i = 0; i < n; i++) names[i] = i ? color_lut_name(i) : none.c_str();
        int cur = color_lut_index(g_settings.color_lut);
        char desc[1500];
        snprintf(desc, sizeof desc,
                 tr("A look-up table (LUT) that recolours the 3D image. Savage Wind's own are listed first. Your own LUTs "
                    "go in this folder: .cube files (up to 64 x 64 x 64) and ReShade-style .png strips (1024 x 32, "
                    "4096 x 64). They are listed by file name.\n\n%s"), color_lut_folder());
        bool picked;
        if (opt_list("LUT", &cur, names, n, 0, desc, &picked))
            snprintf(g_settings.color_lut, sizeof g_settings.color_lut, "%s", color_lut_id(cur));
    }
    opt_indent(1);
    if (!g_settings.color_lut[0]) opt_disable_next();
    opt_int("LUT intensity", &g_settings.color_lut_strength, 0, 100, 5, 100, "%d%%",
            "How much of the LUT's look is used: 100% is all of it, 0% none.");
    opt_indent(-1);
}
#endif

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_window_apply.store(true);
    }
    {   // Window size (runtime/window_size.c): the sizes the display can show, its own first, then grouped by shape
        WinSize list[WINSIZE_MAX];
        static char names_buf[WINSIZE_MAX][64];
        const char *names[WINSIZE_MAX];
        int n = winsize_list(list, g_settings.fullscreen), cur = 0, def = 0;
        WinSize e = winsize_effective(g_settings.fullscreen), d = winsize_default(g_settings.fullscreen);
        WinSize desk = winsize_desktop();
        for (int i = 0; i < n; i++) {
            winsize_name(list[i], tr("native"), names_buf[i], sizeof names_buf[i]);
            names[i] = names_buf[i];
            if (list[i].w == e.w && list[i].h == e.h) cur = i;
            if (list[i].w == d.w && list[i].h == d.h) def = i;
        }
        char desc[600];
        snprintf(desc, sizeof desc, tr("The size of the game's window, or of the screen in fullscreen. Only the sizes your "
                                       "display can show: its own first, then grouped by shape. A shape wider than 16:9 "
                                       "is filled with Fill wide screens on; any other keeps 16:9 with bars.\n\nYour display: %d x %d."), desk.w, desk.h);
        bool picked;
        if (opt_list("Window size", &cur, names, n, def, desc, &picked)) {  // (past either end: the other one)
            g_settings.window_w = list[cur].w, g_settings.window_h = list[cur].h;
            if (picked) g_want_window_apply.store(true);
            else menu_request_window_size_apply();  // (stepping: the size where it stops)
        }
    }
    {   // Fill wide screens (runtime/nvn/output_scale.c rt_persp)
        bool wv = g_settings.wide_view;
        if (opt_toggle("Fill wide screens", &wv, true,
                       "On a window or screen wider than 16:9 (21:9, 32:9) the game shows more to the sides instead "
                       "of black bars. The HUD stays 16:9 in the middle; the pause menu, the map and the start "
                       "screen are shown 16:9."))
            g_settings.wide_view = wv;
    }
    {   // Field of view (runtime/nvn/output_scale.c rt_persp)
        static const int fov[] = FOV_CHOICES;
        enum { NV = sizeof fov / sizeof fov[0] };
        static char names_buf[NV][16];
        const char *names[NV];
        int cur = 0, def = 0;
        for (int i = 0; i < NV; i++) {
            snprintf(names_buf[i], sizeof names_buf[i], "%d%%", fov[i]);
            names[i] = names_buf[i];
            if (fov[i] == g_settings.fov) cur = i;
            if (fov[i] == 100) def = i;
        }
        if (opt_choice("Field of view", &cur, names, NV, def,
                       "How much of the world the camera shows in play: 100% is the game's own. Cutscenes and "
                       "conversations keep theirs.", false))
            g_settings.fov = fov[cur];
    }
    {   // Frame rate (runtime/hle/framepace.c)
        static const int fps[] = FPS_CHOICES;
        enum { NF = sizeof fps / sizeof fps[0] };
        static char names_buf[NF][32];
        const char *names[NF];
        int cur = 0, def = 0;
        for (int i = 0; i < NF; i++) {
            if (fps[i]) snprintf(names_buf[i], sizeof names_buf[i], tr("%d FPS"), fps[i]);
            names[i] = fps[i] ? names_buf[i] : "Off";
            if (fps[i] == g_settings.fps_limit) cur = i;
            if (fps[i] == FPS_DEFAULT) def = i;
        }
        std::string desc = tr("How many pictures a second the game draws. The game itself runs at the same speed at "
                              "every rate (the console runs it at 30). Higher rates need a faster computer. Menus, "
                              "cutscenes and conversations use it too, and videos play at their own speed. Off: as "
                              "many as the computer can draw.");
        int hz = (int)lroundf(winsize_refresh_rate());
        if (hz > 0) {
            char line[200];
            snprintf(line, sizeof line, tr("Your display refreshes %d times a second: a rate above that shows no more "
                                           "pictures, and one that divides it evenly is the smoothest."), hz);
            desc += std::string("\n\n") + line;
        }
        if (opt_choice("Frame rate", &cur, names, NF, def, desc.c_str(), false)) g_settings.fps_limit = fps[cur];
    }
    {   // VSync (settings.h VSYNC_*: runtime/nvn/vulkan/device.c, runtime/hle/framepace.c)
        static const char *const names[VSYNC_COUNT] = {"On", "Fast", "Adaptive", "Off"};
        std::string desc = tr("On: no tearing; the game waits for the display. With G-SYNC or FreeSync the display "
                              "waits for the game instead: each frame shows as soon as it's ready, and the frame rate "
                              "stays a little under the display's refresh rate. Fast: no tearing and no waiting: each "
                              "refresh shows the newest frame. Adaptive: as On, but a frame that comes late shows at "
                              "once and may tear. Off: each frame shows at once: the least delay, but it tears.");
        float hz = winsize_refresh_rate();
        if (hz >= 30) {
            char line[200];
            snprintf(line, sizeof line,
                     tr("With On and Adaptive, the frame rate on this display goes up to about %d FPS."),
                     (int)lroundf(hz - hz * hz / 3600));
            desc += std::string("\n\n") + line;
        }
        opt_choice("VSync", &g_settings.vsync, names, VSYNC_COUNT, VSYNC_DEFAULT, desc.c_str(), false);
    }
    {   // HDR (settings.h HDR_*: runtime/nvn/vulkan/device.c hdr_wanted)
        static const char *const names[HDR_COUNT] = {"Automatic", "On", "Off"};
        opt_choice("HDR", &g_settings.hdr, names, HDR_COUNT, HDR_AUTO,
                   "Automatic: while Windows HDR is on for this display, the picture is sent in HDR, so it looks as bright "
                   "and colourful as in SDR (in SDR, Windows shows it dim and dull then). On: always in HDR, where the "
                   "display takes it. Off: always SDR. White follows Windows' SDR content brightness (Settings > System > "
                   "Display > HDR).", false);
    }
    static const char *const perf_names[] = {"Off", "FPS", "Full"};
    opt_choice("Performance overlay", &g_settings.perf_overlay, perf_names, 3, PERF_OFF,
               "FPS: the frame rate and frame time. Full: also CPU, memory and GPU load and temperatures, and a "
               "frame time graph. F10 switches between them while you play.");

    opt_section("Rendering");
    {   // Upscaler: Off, this OS's spatial upscaler (its modes on the row below), DLSS where NGX runs, and FSR 3 still
        // to come. Values that can't be picked are shown greyed; stepping onto them keeps the one in use.
        // The modes wait for render-target scaling (upscale_modes_available): until then the upscaler brings the
        // game's own 1920 x 1080 image to a bigger window.
        std::string fsr3 = std::string(tr("FSR 3")) + " " + tr("(coming soon)");
        std::string dlss = upscaler_available(UPSCALE_DLSS) ? std::string(tr("DLSS")) :
                           std::string(tr("DLSS")) + " " + tr("(NVIDIA RTX only)");
#ifdef __APPLE__
        const char *ups[] = {"Off", "MetalFX"};
        static const int ups_value[] = {UPSCALE_OFF, UPSCALE_METALFX};
        const char *ud = "How the game's image (1920 x 1080) is scaled up to a bigger window. Off: plain (bilinear) "
                         "scaling. MetalFX: Apple's upscaler, sharper.\n\nRendering below the window's size (the modes "
                         "below) is coming in a later version.";
#else
        const char *ups[] = {"Off", "AMD FSR 1", fsr3.c_str(), dlss.c_str()};
        static const int ups_value[] = {UPSCALE_OFF, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS};
        const char *ud = "Off: the 3D image renders at the window's resolution. AMD FSR 1: it renders smaller (the mode "
                         "below), then FSR 1's edge-aware upscaling and sharpening bring it to the window. The HUD and "
                         "menus always render at the window's resolution.\n\nAMD FSR 3 and NVIDIA DLSS (NVIDIA RTX "
                         "graphics cards) are coming in a later version.";
#endif
        enum { NU = sizeof ups_value / sizeof ups_value[0] };
        bool avail[NU];
        int ui = 0;
        for (int i = 0; i < NU; i++) {
            avail[i] = upscaler_available(ups_value[i]);
            if (ups_value[i] == g_settings.upscaler) ui = i;
        }
        static int shown = -1;
        int udef = 0;  // (the reset value: UPSCALE_DEFAULT)
        for (int i = 0; i < NU; i++) if (ups_value[i] == UPSCALE_DEFAULT) udef = i;
        if (opt_choice_avail("Upscaler", &ui, &shown, ups, avail, NU, udef, ud)) g_settings.upscaler = ups_value[ui];
        // its mode (AMD's scale factors), one step in; greyed when the upscaler is off
        static const char *const modes[] = {"Ultra Quality", "Quality", "Balanced", "Performance"};
        bool modes_ok = upscale_modes_available();
        std::string mode_names[UPQ_COUNT];
        const char *mn[UPQ_COUNT];
        bool mavail[UPQ_COUNT];
        for (int i = 0; i < UPQ_COUNT; i++) {
            mode_names[i] = modes_ok ? tr(modes[i]) : std::string(tr(modes[i])) + " " + tr("(coming soon)");
            mn[i] = mode_names[i].c_str();
            mavail[i] = modes_ok;
        }
        float sx, sy;  // the 3D scene's size (BOTW docked: 1600 x 900 times its scale)
        renderer_get_scene_scale(&sx, &sy);
        char rendering[100];
        snprintf(rendering, sizeof rendering, tr("Rendering at %.0f x %.0f."), 1600.0 * sx, 900.0 * sy);
        std::string md = std::string(tr("How much smaller the game renders before the upscaler brings it to the window, "
                                        "on each side: Ultra Quality 1.3x, Quality 1.5x, Balanced 1.7x, Performance "
                                        "2x. Smaller is faster and softer.")) +
                         "\n\n" + (modes_ok ? rendering : tr("Coming in a later version."));
        opt_indent(1);
        if (g_settings.upscaler == UPSCALE_OFF) opt_disable_next();
        static int mshown = -1;
        opt_choice_avail("Upscaler mode", &g_settings.upscale_quality, &mshown, mn, mavail, UPQ_COUNT, UPQ_QUALITY,
                         md.c_str());
        opt_indent(-1);
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful with an upscaler.");
#ifndef __APPLE__
    {   // Anisotropic filtering and sharpening (Vulkan: vulkan/textures.c sampler_aniso, vulkan/exec.c scene_post)
        static const char *const aniso[] = {"Off", "2x", "4x", "8x", "16x"};
        opt_choice("Anisotropic filtering", &g_settings.anisotropy, aniso, 5, 0,
                   "Keeps the textures of ground, paths and walls sharp where you look along them into the distance. "
                   "Off: the game's own filtering, as on the console. 16x is the sharpest and costs very little on a PC "
                   "graphics card.", false);
        static const char *const sharp[] = {"Off", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};
        opt_choice("Sharpening (AMD CAS)", &g_settings.sharpening, sharp, 11, 0,
                   "AMD FidelityFX CAS sharpens the 3D image after the upscaler (or at the window's resolution with the "
                   "upscaler off), before the HUD is drawn over it: edges get crisper without halos. Most useful with "
                   "AMD FSR 1.", false);
        // AMD FSR 3 frame generation (vulkan/framegen.cpp; the frame rate counts the presented frames: framepace.c)
        opt_toggle("Frame generation (AMD FSR 3)", &g_settings.frame_gen, false,
                   "AMD FSR 3 makes a frame between each two the game draws: about twice the frames on screen. The "
                   "frame rate above counts them all, so the game itself draws half of it (60 with 120) and the "
                   "controls answer as at that rate, a little later. In play only: the menus and the map show the "
                   "game's own frames. Fast moves can leave faint smears around Link and what moves on its own.");
    }
#endif
    opt_toggle("Asynchronous shaders", &g_settings.async_shaders, false,
               "Off: the game waits for each new shader to compile, a short stutter the first time an effect "
               "appears, and everything is drawn. On: no waiting, but what needs a shader still compiling is skipped "
               "meanwhile: effects pop in late, and pictures the game draws only once (the save file's screenshot) "
               "can come out as garbage.");
    // shadow resolution (its own block)
    static const char *shadow[] = {"Console (896)", "High (2048)", "Ultra (4096)", "Extreme (8192)"};
    opt_choice("Shadow resolution", &g_settings.shadow_res, shadow, SHADOW_COUNT, SHADOW_CONSOLE,
               "The size of the maps the sun's shadows are drawn from. Console: the game's own, 896 x 896. High (2048), "
               "Ultra (4096) and Extreme (8192) give sharper shadow edges (Link, trees, buildings) for more GPU time "
               "and video memory (Ultra: about 100 MB, Extreme: about 400 MB).");
#ifndef __APPLE__
    tab_colour();
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
}

extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_GAME = 1, RT_REBOOT_TITLE = 2)

// Text and voice languages (runtime/language.c). The game reads both when it starts: in game they apply after
// Quit > Start screen and Play.
static void tab_language(bool in_game) {
    opt_begin("##language", ImGui::GetFontSize() / 34);
    opt_section("Language");
    static const char *tnames[32], *vnames[32];
    for (int i = 0; i < g_ntext_languages && i < 32; i++) tnames[i] = g_text_languages[i].name;
    for (int i = 0; i < g_nvoice_languages && i < 32; i++) vnames[i] = g_voice_languages[i].name;
    const char *later = in_game ? tr("\n\nApplies the next time the game starts: Restart now below, or Quit > Start "
                                     "screen, then Play.") : "";
    char d1[600], d2[800];
    snprintf(d1, sizeof d1, tr("The language of the game's texts and of these menus. Auto: your system's language "
                               "(English if the game doesn't have it).%s"), later);
    snprintf(d2, sizeof d2, tr("The language the characters speak. Same as text: the voices the game picks for the text "
                               "language (Japanese for Korean and Chinese, English for the other languages it has no "
                               "voices for). Any other: those voices with any text language.%s"), later);
    int text = language_text_index();  // (of the languages the game files have: Thai from 1.9.0 on)
    if (opt_choice("Text", &text, tnames, language_text_count(), 0, d1, false)) g_settings.text_language = text;
    opt_choice("Voices", &g_settings.voice_language, vnames, g_nvoice_languages, 0, d2, false);
    if (in_game && language_pending() &&
        opt_button("Restart now", "Starts the game again with these languages. Progress since your last save is lost.")) {
        settings_save();
        rt_request_reboot(1);  // (RT_REBOOT_GAME: boot again at once)
    }
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? tr("\n\nProgress since your last save is lost.") : "";
    char d1[400], d2[400];
    snprintf(d1, sizeof d1, tr("Leaves the game and goes back to the Savage Wind start screen.%s"), lost);
    snprintf(d2, sizeof d2, tr("Closes Savage Wind.%s"), lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

static void tab_about(void) {
    header("Movement");
    toggle("D-pad as left stick", &g_settings.dpad_move);
    hint("The d-pad (and the arrow keys) also move like the left stick while the stick is at rest.");
    ImGui::Spacing();
    header("Mouse");
    toggle("Mouse camera", &g_settings.mouse_camera);
    hint("The mouse turns the camera. While you play, the cursor stays in the window; Esc or F1 frees it.");
    float sens = roundf(g_settings.mouse_sens * 100);
    if (opt_slider("Mouse sensitivity", &sens, 10, 1000, 5, MOUSE_SENS_DEFAULT * 100, "%g%%", "How fast the mouse turns the camera."))
        g_settings.mouse_sens = sens / 100.0f;
    float sens_y = roundf(g_settings.mouse_sens_y * 100);
    if (opt_slider("Mouse vertical sensitivity", &sens_y, 50, 200, 5, 100, "%g%%",
                   "Up and down, as a percentage of the mouse sensitivity."))
        g_settings.mouse_sens_y = sens_y / 100.0f;
    toggle("Invert mouse Y", &g_settings.mouse_invert_y);
    ImGui::Spacing();
    header("Controls");
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted(tr("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start"));
    ImGui::TextUnformatted(tr("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)"));
    ImGui::TextUnformatted(tr("Fullscreen:  F11 / Alt+Enter"));
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

// Performance overlay (F10), in the style of MangoHud: the game's frame rate and frame times, and the machine's load
// and temperatures (runtime/perfmon.c). Numbers the platform doesn't give read "-".
static void perf_cell(const char *fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) ImGui::TextDisabled("-");
    else ImGui::Text(fmt, v);
}
static void perf_overlay_draw(ImGuiIO &io, float scale) {
    PerfStats s;
    perfmon_snapshot(&s);
    // top right: BOTW keeps its hearts and stamina top left, its map and gauges bottom right
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c(0.55f, 0.83f, 0.31f, 1), cpu_c(0.18f, 0.59f, 0.80f, 1), ram_c(0.80f, 0.45f, 0.70f, 1),
                 fps_c(0.95f, 0.95f, 0.95f, 1);
    if (g_settings.perf_overlay == PERF_FPS) {
        // (frame generation: the presented frames, generated ones included)
        ImGui::TextColored(fps_c, "%.0f FPS  %.1f ms", s.fps + s.fps_generated, s.frame_ms_avg);
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextColumn(); ImGui::TextColored(gpu_c, "GPU");
        perf_cell("%.0f%%", s.gpu_pct); perf_cell("%.0f C", s.gpu_temp_c); perf_cell("%.0f W", s.gpu_power_w);
        ImGui::TableNextColumn(); ImGui::TextColored(gpu_c, "VRAM");
        perf_cell("%.2f GB", s.vram_used_mb < 0 ? -1 : s.vram_used_mb / 1024); perf_cell("/ %.1f GB", s.vram_total_mb < 0 ? -1 : s.vram_total_mb / 1024);
        perf_cell("%.0f MHz", s.gpu_clock_mhz);
        ImGui::TableNextColumn(); ImGui::TextColored(cpu_c, "CPU");
        perf_cell("%.0f%%", s.cpu_total_pct); perf_cell("%.0f C", s.cpu_temp_c); perf_cell(tr("game %.0f%%"), s.cpu_game_pct);
        ImGui::TableNextColumn(); ImGui::TextColored(ram_c, "RAM");
        perf_cell("%.1f GB", s.ram_used_mb / 1024); perf_cell("/ %.1f GB", s.ram_total_mb / 1024); perf_cell(tr("game %.1f GB"), s.ram_game_mb / 1024);
        ImGui::TableNextColumn(); ImGui::TextColored(fps_c, "FPS");
        perf_cell("%.0f", s.fps + s.fps_generated);
        perf_cell("%.1f ms", s.frame_ms_avg); perf_cell(tr("max %.1f ms"), s.frame_ms_max);
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, NULL, 0, top, ImVec2(330 * scale, 48 * scale));
    ImGui::TextDisabled("%s", s.gpu_name[0] ? s.gpu_name : tr("GPU: no NVML / sysfs data"));
    ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Enter your name"));
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled(tr("Enter  confirm      Esc  cancel      (%d / %d)"), (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Window size's list, dropped down under the row's button on the game's Options screen (Video), drawn like the game's
// own pop-up lists: the menu is 16:9 in the middle of the window (output_scale.c), the button where its layout has it
static void size_list_draw(ImGuiIO &io, float scale) {
    static bool was_open;
    static int frames;
    bool appearing = !was_open;
    was_open = true;
    frames = appearing ? 0 : frames + 1;
    float gw = io.DisplaySize.x, gh = gw * 9 / 16;
    if (gh > io.DisplaySize.y) gh = io.DisplaySize.y, gw = gh * 16 / 9;
    float ox = (io.DisplaySize.x - gw) * 0.5f, oy = (io.DisplaySize.y - gh) * 0.5f;
    float x0 = ox + gw * 0.496f, x1 = ox + gw * 0.708f, y0 = oy + gh * 0.318f, bottom = oy + gh * 0.87f;
    float row_h = gh * 0.046f;
    WinSize list[WINSIZE_MAX];
    static char names[WINSIZE_MAX][64];
    int n = winsize_list(list, g_settings.fullscreen), cur = 0;
    WinSize e = winsize_effective(g_settings.fullscreen);
    for (int i = 0; i < n; i++) {
        winsize_name(list[i], tr("native"), names[i], sizeof names[i]);
        if (list[i].w == e.w && list[i].h == e.h) cur = i;
    }
    ImGui::SetNextWindowPos(ImVec2(x0, y0), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(x1 - x0, 0), ImVec2(x1 - x0, bottom - y0));
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * scale, 4 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.04f, 0.04f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.62f, 0.62f, 0.58f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.95f, 0.93f, 0.85f, 1));
    ImGui::Begin("##sizes", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font);
    int picked = -1;
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##size", i == cur, 0, ImVec2(w, row_h))) picked = i;
        if (appearing && i == cur) {
            ImGui::SetItemDefaultFocus();
            ImGui::SetScrollHereY(0.5f);
            ImGui::SetNavCursorVisible(g_last_pad.load());
        }
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImVec2 ts = ImGui::CalcTextSize(names[i]);
        ImU32 col = i == cur ? IM_COL32(236, 194, 84, 255) : hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 213, 205, 255);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (row_h - ts.y) * 0.5f), col, names[i]);
        ImGui::PopID();
    }
    // (test) SAVAGEWIND_SIZE_PICK=<n>: picks size n three seconds after the list is up
    if (const char *t = getenv("SAVAGEWIND_SIZE_PICK"); t && frames == 180) picked = atoi(t) < n ? atoi(t) : cur;
    bool cancel = !appearing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                 ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                 (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered()));
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(4);
    if (picked >= 0 && picked != cur) {
        g_settings.window_w = list[picked].w, g_settings.window_h = list[picked].h;
        settings_save();
        g_want_window_apply.store(true);
        game_options_size_picked();
    }
    if (picked >= 0 || cancel) {
        g_sizes_hold = SDL_GetTicks() + 300;  // (the A / B that closed it, still down, stays out of the game)
        g_sizes = false;
        was_open = false;
    }
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) SAVAGEWIND_KBD_AUTO=<text>: answer at once
    if (getenv("SAVAGEWIND_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("SAVAGEWIND_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (ids 2-7 were Link's Awakening tabs; the ids stay: test hooks)
    static const char *names[] = {"Video", "Audio", "Language", "", "", "", "", "", "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 2, 9};  // Controls, Video, Audio, Language, Quit
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[160];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s ###%s" : "  %s  ###%s", tr(t.name), t.name);  // (### : ID stays English)
            ImGui::BeginDisabled(!t.on);
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.19f, 0.27f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.27f, 0.22f, 1));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2: tab_language(in_game); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("SAVAGEWIND_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool kbd = g_kbd.load() == 1;
    bool sizes = g_sizes.load();
    if (!open && !kbd && !sizes && g_settings.perf_overlay == PERF_OFF) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
    fonts_follow_language();
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.perf_overlay != PERF_OFF) perf_overlay_draw(io, scale);

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (sizes) size_list_draw(io, scale);
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
}
