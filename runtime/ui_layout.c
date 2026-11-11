#include <stdbool.h>
#include <string.h>
#include "ui_layout.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "framepace.h"
#include "nvn/nvn.h"

static bool ui_range(uint64_t p, uint64_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}

uint64_t ui_layout_begin(Cpu *c) {
#ifdef __APPLE__
    return 0;
#else
    if ((!g_settings.hud_edges && g_settings.hud_size == 100) || !g_site.screen_mgr || strcmp(g_game_version, "1.9.0")) return 0;
    uint64_t manager = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!ui_range(manager, 0x38)) return 0;
    uint32_t count = *(uint32_t *)(uintptr_t)(manager + 0x28);
    uint64_t screens = *(uint64_t *)(uintptr_t)(manager + 0x30), screen = c->x[0];
    if (count <= 0x4c || count > 256 || !ui_range(screens, count * 8)) return 0;
    const uint64_t *slots = (const uint64_t *)(uintptr_t)screens;
    unsigned kind = screen == slots[0x25] || screen == slots[0x2] || screen == slots[0x1c] ? UI_LAYOUT_HUD : screen == slots[0xc] ? UI_LAYOUT_CONTEXT :
                    screen == slots[0x11] || screen == slots[0x4a] || screen == slots[0x4c] ? UI_LAYOUT_DIALOGUE : UI_LAYOUT_NONE;
    // AppMap renders the minimap in play and the full map in pause. Keep their policies separate.
    if (screen == slots[0x2a]) kind = framepace_menu_ui() ? UI_LAYOUT_MENU : UI_LAYOUT_HUD;
    if (screen == slots[0x4a]) kind = UI_LAYOUT_TALK;
    if (!kind || !ui_range(screen, 0x103)) return 0;
    const uint8_t *state = (const uint8_t *)(uintptr_t)screen;
    // The native draw returns here before the render context is required to be initialized.
    if (!state[0x100] || state[0x102] || !state[0xfe] || state[0xff] == 2) return 0;
    uint64_t context = c->x[1];
    if (!kind || !ui_range(context, 0x28)) return 0;
    uint64_t commands = *(uint64_t *)(uintptr_t)(context + 0x20);
    if (!ui_range(commands, 0xc0)) return 0;
    // Layout/pane draw receives nn::gfx's wrapper at +8; its NVN object is the pointer at wrapper+0xb0.
    commands = *(uint64_t *)(uintptr_t)(commands + 0xb8);
    if (!ui_range(commands, 8)) return 0;
    return nvn_ui_scope(commands, kind) ? commands : 0;
#endif
}

void ui_layout_end(Cpu *c, uint64_t commands) {
    (void)c;
    if (commands) nvn_ui_scope(commands, UI_LAYOUT_NONE);
}

void rt_ui_layout_draw(Cpu *c, void (*native)(Cpu *)) {
    uint64_t commands = ui_layout_begin(c);
    native(c);
    ui_layout_end(c, commands);
}
