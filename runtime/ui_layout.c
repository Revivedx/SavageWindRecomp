#include <stdbool.h>
#include <math.h>
#include <string.h>
#include "ui_layout.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "framepace.h"
#include "nvn/nvn.h"
#include "nvn/hud_scale.h"

static bool ui_range(uint64_t p, uint64_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}

// MiniMap's native animation chooses the map presentation before pause state catches up.
// Frame zero is the full map; its end frame is the minimap, also while opening the inventory.
int ui_layout_map_mode(uint64_t screen) {
    if (strcmp(g_game_version, "1.9.0") || !ui_range(screen, 0x3648)) return -1;
    uint64_t animation = *(uint64_t *)(uintptr_t)(screen + 0x3640);
    if (!ui_range(animation, 0x24)) return -1;
    uint64_t resource = *(uint64_t *)(uintptr_t)(animation + 0x18);
    if (!ui_range(resource, 0xa)) return -1;
    unsigned end = *(uint16_t *)(uintptr_t)(resource + 8);
    float frame = *(float *)(uintptr_t)(animation + 0x20);
    if (!end || !isfinite(frame)) return -1;
    return frame == 0 ? 1 : frame == end ? 0 : -1;
}

static _Thread_local unsigned pane_draw_depth, layout_kind;  // (the scope of the layout being drawn, and its screen:)
static _Thread_local uint64_t layout_screen;
static _Thread_local bool layout_is_minimap;
RT_ON_RESET(ui_layout_reset) {
    pane_draw_depth = 0; layout_kind = UI_LAYOUT_NONE; layout_screen = 0; layout_is_minimap = false;
}

static bool any_hud_hidden(void) {
    return g_settings.hud_hide_hearts || g_settings.hud_hide_stamina || g_settings.hud_hide_abilities ||
           g_settings.hud_hide_minimap || g_settings.hud_hide_sensors || g_settings.hud_hide_enemy_bars ||
           g_settings.hud_hide_prompts;
}

static bool ui_pane_is(const char *name, const char *exact) {
    return !strncmp(name, exact, 24) && strlen(exact) < 24;
}

static bool hud_pane_hidden(const char *name) {
    if (g_settings.hud_hide_hearts && ui_pane_is(name, "Pa_HeartGauge_00")) return true;
    if (g_settings.hud_hide_stamina &&
        (ui_pane_is(name, "Pa_Ganbari_00") || ui_pane_is(name, "Pa_GanbariEx_00"))) return true;
    if (g_settings.hud_hide_abilities &&
        !memcmp(name, "Pa_SinJu_0", 10) && name[10] >= '0' && name[10] <= '3' && !name[11]) return true;
    if (g_settings.hud_hide_sensors &&
        (ui_pane_is(name, "Pa_Sensor_00") || ui_pane_is(name, "Pa_SoundGauge_00") ||
         ui_pane_is(name, "Pa_TempMeter_00") || ui_pane_is(name, "Pa_Weather_00") ||
         ui_pane_is(name, "Pa_Time_00"))) return true;
    if (g_settings.hud_hide_enemy_bars &&
        (ui_pane_is(name, "N_EnemyGauge_00") || ui_pane_is(name, "Pa_EnemyGauge_00"))) return true;
    return false;
}

static bool hud_screen_hidden(void) {
    if (layout_kind == UI_LAYOUT_HUD && layout_is_minimap && g_settings.hud_hide_minimap) return true;
    if (layout_kind == UI_LAYOUT_CONTEXT && g_settings.hud_hide_prompts) return true;
    return false;
}

// The champions' abilities in MainScreen: a row each (Pa_SinJu_00 to _03: the icon, its count and their glows), in a
// column under the hearts from the top-left zone into the middle one (layout y 170 to 20, the zones' border at 120)
static bool ui_champion_row(const char *name) {
    return !memcmp(name, "Pa_SinJu_0", 10) && name[10] >= '0' && name[10] <= '3' && !name[11];
}

// The quick selectors' screen (slot 0x2d) keeps the game's geometry for its bar and its backdrop, but its D-pad guide
// (N_MainAll_00: the rune, shield, weapon and bow in use and the arrow count) sits under the hearts all the time, part
// of the HUD. Kept as it was, on a taller window the champions' abilities went up to the window's top under it (#903).
static bool ui_dpad_guide(const char *name) {
    if (memcmp(name, "N_MainAll_00", sizeof "N_MainAll_00")) return false;
    uint64_t manager = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!ui_range(manager, 0x38)) return false;
    uint32_t count = *(uint32_t *)(uintptr_t)(manager + 0x28);
    uint64_t screens = *(uint64_t *)(uintptr_t)(manager + 0x30);
    return count > 0x2d && count <= 256 && ui_range(screens, count * 8) && layout_screen &&
           layout_screen == *(uint64_t *)(uintptr_t)(screens + 8 * 0x2d);
}

// A HUD part whose panes all take one anchor (hud_group_kind, #913)
static bool ui_pane_group(Cpu *c, void (*native)(Cpu *), int ax, int ay) {
    uint64_t wrapper = c->x[2];
    if (!ui_range(wrapper, 0xb8)) return false;
    uint64_t commands = *(uint64_t *)(uintptr_t)(wrapper + 0xb0);
    if (!nvn_ui_scope(commands, hud_group_kind(UI_LAYOUT_HUD, ax, ay))) return false;
    pane_draw_depth++;
    native(c);  // (as below: the entry gate re-enters once, then draws the part's subtree normally)
    pane_draw_depth--;
    nvn_ui_scope(commands, UI_LAYOUT_NONE);
    return true;
}

bool rt_ui_layout_pane_draw(Cpu *c, void (*native)(Cpu *)) {
#ifdef SW_METAL
    return false;
#else
    uint64_t pane = c->x[0];
    bool scope_needed = g_settings.hud_edges || g_settings.hud_size != 100 || g_settings.dynamic_hud;
    if (pane_draw_depth || !native || (!scope_needed && !any_hud_hidden()) ||
        strcmp(g_game_version, "1.9.0") || !g_site.screen_mgr || !ui_range(pane, 0xf8)) return false;
    const char *name = (const char *)(uintptr_t)(pane + 0xb8);
    if (layout_kind == UI_LAYOUT_HUD && hud_pane_hidden(name)) return true;
    if (!scope_needed) return false;
    if ((layout_kind == UI_LAYOUT_HUD && ui_champion_row(name)) || (layout_kind == UI_LAYOUT_DIALOGUE && ui_dpad_guide(name)))
        return ui_pane_group(c, native, -1, 1);  // (both under the hearts, in their corner)
    if (memcmp((const void *)(uintptr_t)(pane + 0xb8), "Pa_QuestName_00", sizeof "Pa_QuestName_00")) return false;
    uint64_t manager = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!ui_range(manager, 0x38)) return false;
    uint32_t count = *(uint32_t *)(uintptr_t)(manager + 0x28);
    uint64_t screens = *(uint64_t *)(uintptr_t)(manager + 0x30);
    if (count <= 0x25 || count > 256 || !ui_range(screens, count * 8)) return false;
    uint64_t hud = *(uint64_t *)(uintptr_t)(screens + 8 * 0x25);
    if (!ui_range(hud, 0x3cc8)) return false;
    uint64_t quest = *(uint64_t *)(uintptr_t)(hud + 0x3cc0);
    if (!quest || *(uint64_t *)(uintptr_t)(pane + 0xf0) != quest) return false;
    // MainScreen embeds this notification part: its glyph origins and window centers fall
    // into different HUD anchor zones. Keep the complete subtree in its native layout.
    uint64_t wrapper = c->x[2];
    if (!ui_range(wrapper, 0xb8)) return false;
    uint64_t commands = *(uint64_t *)(uintptr_t)(wrapper + 0xb0);
    if (!nvn_ui_scope(commands, UI_LAYOUT_DIALOGUE)) return false;
    pane_draw_depth++;
    native(c);  // the entry gate re-enters once, then draws the original subtree normally
    pane_draw_depth--;
    nvn_ui_scope(commands, UI_LAYOUT_NONE);
    return true;
#endif
}

uint32_t rt_cjk_font_scale(void) {
    const char *env = getenv("SAVAGEWIND_CJK_SCALE");
    if (env && *env) {
        int v = atoi(env);
        if (v >= 1 && v <= 4) return (uint32_t)v;
    }
    return (g_settings.sharper_text >= 2) ? 2 : 1;
}

static bool screen_is_named(uint64_t screen, const char *want) {
    if (!ui_range(screen, 0xd8)) return false;
    uint64_t name = *(uint64_t *)(uintptr_t)(screen + 0xd0);
    return ui_range(name, 32) && !strcmp((const char *)(uintptr_t)name, want);
}

uint64_t ui_layout_begin(Cpu *c) {
#ifdef SW_METAL
    return 0;
#else
    bool scope_needed = g_settings.hud_edges || g_settings.hud_size != 100 || g_settings.dynamic_hud || g_settings.wide_view;
    if ((!scope_needed && !any_hud_hidden()) || !g_site.screen_mgr || strcmp(g_game_version, "1.9.0")) return 0;
    uint64_t manager = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!ui_range(manager, 0x38)) return 0;
    uint32_t count = *(uint32_t *)(uintptr_t)(manager + 0x28);
    uint64_t screens = *(uint64_t *)(uintptr_t)(manager + 0x30), screen = c->x[0];
    if (count <= 0x4c || count > 256 || !ui_range(screens, count * 8)) return 0;
    const uint64_t *slots = (const uint64_t *)(uintptr_t)screens;
    unsigned kind = screen == slots[0x25] || screen == slots[0x2] || screen == slots[0x1c] ? UI_LAYOUT_HUD : screen == slots[0xc] ? UI_LAYOUT_CONTEXT :
                    screen == slots[0x6] || screen == slots[0xb] || screen == slots[0x11] || screen == slots[0x27] ||
                    screen == slots[0x2d] || screen == slots[0x4a] || screen == slots[0x4c] ? UI_LAYOUT_DIALOGUE : UI_LAYOUT_NONE;
    // AppMap renders the minimap in play and the full map in pause. Keep their policies separate.
    bool is_minimap = false;
    if (screen == slots[0x2a]) {
        int map = ui_layout_map_mode(screen);
        kind = (map < 0 ? framepace_menu_ui() : map) ? UI_LAYOUT_MENU : UI_LAYOUT_HUD;
        is_minimap = kind == UI_LAYOUT_HUD;
    }
    if (screen == slots[0x4a]) kind = UI_LAYOUT_TALK;
    if (!kind || !ui_range(screen, 0x103)) return 0;
    const uint8_t *state = (const uint8_t *)(uintptr_t)screen;
    // The native draw returns here before the render context is required to be initialized.
    if (!state[0x100] || state[0x102] || !state[0xfe] || state[0xff] == 2) return 0;
    if (!scope_needed) {  // (Hide HUD elements alone: the layout's kind for its panes, no scope of its own)
        layout_kind = kind; layout_screen = screen; layout_is_minimap = is_minimap;
        return 0;
    }
    uint64_t context = c->x[1];
    if (!kind || !ui_range(context, 0x28)) return 0;
    uint64_t commands = *(uint64_t *)(uintptr_t)(context + 0x20);
    if (!ui_range(commands, 0xc0)) return 0;
    // Layout/pane draw receives nn::gfx's wrapper at +8; its NVN object is the pointer at wrapper+0xb0.
    commands = *(uint64_t *)(uintptr_t)(commands + 0xb8);
    if (!ui_range(commands, 8)) return 0;
    unsigned scope = kind;
    if (kind == UI_LAYOUT_HUD && g_settings.wide_view && screen_is_named(screen, "MainScreen3D_00"))
        scope |= HUD_WORLD_3D;
    if (!nvn_ui_scope(commands, scope)) return 0;
    layout_kind = kind; layout_screen = screen; layout_is_minimap = is_minimap;
    return commands;
#endif
}

void ui_layout_end(Cpu *c, uint64_t commands) {
    (void)c;
    if (commands) nvn_ui_scope(commands, UI_LAYOUT_NONE);
}

void rt_ui_layout_draw(Cpu *c, void (*native)(Cpu *)) {
    unsigned outer = layout_kind;
    uint64_t outer_screen = layout_screen;
    bool outer_minimap = layout_is_minimap;
    uint64_t commands = ui_layout_begin(c);
    if (!hud_screen_hidden()) native(c);
    ui_layout_end(c, commands);
    layout_kind = outer; layout_screen = outer_screen; layout_is_minimap = outer_minimap;
}

// The layout draw thunk is also the quick replacement's (runtime/quick_replace.c rt_quick_replace_ui_draw, the same
// site: tools/patches.py _ui_draw_chain): it goes first, and each native draw it makes (the screen's own, the item
// card's) is bracketed here, by the layout of the screen that draw is for. With one patch per address, the later of
// the two had silently replaced the other since 2026-11-12, and every layout here had never run.
static _Thread_local void (*chain_native)(Cpu *);
static void chain_layout_draw(Cpu *c) { rt_ui_layout_draw(c, chain_native); }
void rt_ui_draw_chain(Cpu *c, void (*native)(Cpu *)) {
    extern void rt_quick_replace_ui_draw(Cpu *, void (*)(Cpu *));
    void (*outer)(Cpu *) = chain_native;
    chain_native = native;
    rt_quick_replace_ui_draw(c, chain_layout_draw);
    chain_native = outer;
}
