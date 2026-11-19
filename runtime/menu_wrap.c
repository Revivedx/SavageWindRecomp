// Controls > Menus wrap around: pressing past the last button of a menu goes to the first, and back.
//
// The game's menus (the title menu, the System tab, the Adventure Log, Options, shops...) move their focus with one
// routine: the focused button's neighbour link for that direction, else the nearest focusable button roughly that
// way (tools/patches.py _menu_wrap: the end of that search). At the end of a list it finds none and the focus stays.
// Then this returns the button farthest the other way, in the same column (up / down) or row (left / right): nothing
// lies the pressed way, so every candidate is the other way. The candidates are the search's own: the focusable
// buttons (their isFocusable, vtable +0x28, for the screen's layer) of the control's active layers. Off: the game's
// result, untouched. Screens that switch pages at a list's edge (the inventory's categories) do it elsewhere and keep
// doing it (user play test 2026-11-11: the inventory still changes category at the grid's edge).
#include <stdbool.h>
#include <string.h>

#include "game_sites.h"
#include "hle.h"
#include "settings.h"

static bool in_guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI - 0x100; }
static bool ptr(uint64_t p) { return in_guest(p) && !(p & 7); }

static uint64_t call(Cpu *c, uint64_t fn, uint64_t x0, uint64_t x1) {
    Cpu saved = *c;
    c->x[0] = x0, c->x[1] = x1;
    guest_call(c, fn);
    uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// A button's centre on screen: its pane's global matrix translation (what the search compares)
static bool centre(uint64_t button, float *x, float *y) {
    uint64_t owner = LD64(button + 0x18), pane = ptr(owner) ? LD64(owner + 0x60) : 0;
    if (!ptr(pane)) return false;
    memcpy(x, (const void *)(uintptr_t)(pane + 0x7c), 4);
    memcpy(y, (const void *)(uintptr_t)(pane + 0x8c), 4);
    return true;
}

// found: the search's result; control: the control it searched for (its focused button at +0x48, its screen's
// layout at +0x20 -> +0x80); dir: 0 up, 1 down, 2 left, 3 right
uint64_t rt_menu_wrap(Cpu *c, uint64_t found, uint64_t control, uint32_t dir) {
    if (found || !g_settings.menu_wrap || dir > 3 || !g_site.ui_control_layer) return found;
    uint64_t cur = LD64(control + 0x48), owner = LD64(control + 0x20), screen = ptr(owner) ? LD64(owner + 0x80) : 0;
    uint64_t mgr = ptr(screen) ? LD64(screen + 0x28) : 0;
    float cx, cy;
    if (!ptr(cur) || !ptr(mgr) || !centre(cur, &cx, &cy)) return found;
    uint32_t layer = (uint32_t)call(c, g_site.ui_control_layer, screen, 0);
    const int8_t *active = (const int8_t *)(uintptr_t)LD64(mgr + 0x40);  // (per layer: negative = off)
    int count = (int)LD32(mgr + 0x28), end = (int)LD32(mgr + 0x38), i = 0;
    if (!in_guest((uint64_t)(uintptr_t)active) || count <= 0 || count > 4096) return found;  // (1.9.0: 99 layers)
    while (i < count && active[i] < 0) i++;
    while (end < count && active[end] < 0) end++;
    uint64_t best = 0;
    float best_score = -1e30f;
    for (; i != end && i < count; i++) {
        if (active[i] < 0) continue;
        uint64_t list = LD64(LD64(mgr + 0x30) + 8ull * i);
        if (!ptr(list)) continue;
        int64_t ofs = (int32_t)LD32(list + 0x9c);  // (the buttons' list node inside each button)
        uint64_t head = list + 0x88, node = LD64(list + 0x90);
        for (int n = 0; node != head && ptr(node) && n < 512; n++, node = LD64(node + 8)) {
            uint64_t b = node - ofs;
            if (b == cur || !ptr(LD64(b)) || !(call(c, LD64(LD64(b) + 0x28), b, layer) & 1)) continue;
            float x, y;
            if (!centre(b, &x, &y)) continue;
            float dx = x - cx, dy = y - cy, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            float along = dir < 2 ? ay : ax, across = dir < 2 ? ax : ay;
            if (along <= across) continue;  // (the same column / row: mostly along the pressed axis)
            float score = along - 3 * across;
            if (score > best_score) best_score = score, best = b;
        }
    }
    return best;
}
