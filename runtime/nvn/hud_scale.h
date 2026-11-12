// Gameplay > HUD size: the HUD's panes scale from the part of the screen they are in (exec.c hud_patch, scissor_rect), so
// the hearts stay in their corner and the minimap in its. The screen is three by three zones: a pane in an outer zone
// goes to that edge, one in the middle (the stamina wheel) to the centre. All in the view, the panes' own matrices: on
// screen a point p goes to anchor + (p - anchor) x s.
#pragma once
#include <stdbool.h>

// The anchor of a pane centred at c, on a screen from -edge to edge
static inline float hud_anchor(float c, float edge) { return c < -edge / 3 ? -edge : c > edge / 3 ? edge : 0; }

// While a quick menu is up its bar keeps the game's size: a pane centred at x, y in the middle of the screen and not low
// down, where the rest of the HUD (the hearts, the minimap, the clock) never is
static inline bool hud_quick_keeps(float x, float y, float edge_x, float edge_y) {
    return x > -0.72f * edge_x && x < 0.72f * edge_x && y > -0.4f * edge_y && y < 0.85f * edge_y;
}

// A pane's view rows (3 x 4, row 2 its depth: projected x = m00 x / -z, y = m11 y / -z)
static inline void hud_scale_view(float *v, float s, float m00, float m11, float ax, float ay) {
    for (int k = 0; k < 4; k++) {
        v[k] = s * v[k] - (1 - s) * ax / m00 * v[8 + k];
        v[4 + k] = s * v[4 + k] - (1 - s) * ay / m11 * v[8 + k];
    }
}

// A matrix with the view already in it (4 x 4 rows, row 3 its w: text, the clock)
static inline void hud_scale_matrix(float *m, float s, float ax, float ay) {
    for (int k = 0; k < 4; k++) {
        m[k] = s * m[k] + (1 - s) * ax * m[12 + k];
        m[4 + k] = s * m[4 + k] + (1 - s) * ay * m[12 + k];
    }
}

// A scissor's position and length in pixels, on one axis of a screen centred at c
static inline void hud_scale_span(double *pos, double *len, double c, double edge, float s) {
    double a = c + hud_anchor((float)(*pos + *len / 2 - c), (float)edge);
    *pos = a + (*pos - a) * s;
    *len *= s;
}

// HUD at edges: where the middle of the HUD ends and its top part begins, as a share of the 16:9 band's height above
// the middle. Under it: the quick menus' bar (its pieces reach about 0.41) and a chest's item box (0.38). Over it: the
// area name (about 0.53) and the hearts.
#define HUD_EDGE_TOP 0.46f

// HUD at edges keeps only the quick menus' bar itself in place while one is up (the item frames, their brackets and
// page dots), not all that hud_quick_keeps keeps for HUD size. The area name over it stays at the window's edge like
// the rest of the HUD, instead of dropping onto the bar while a quick menu is up.
static inline bool hud_quick_bar(float x, float y, float edge_x, float edge_y) {
    return hud_quick_keeps(x, y, edge_x, edge_y) && y < HUD_EDGE_TOP * edge_y;
}

// The same for a scissor at cx, cy in pixels (y down) on a gw x gh target, on a taller window (edge_x 1). It tested only
// y before, so on 4:3 the minimap's mask stayed while the minimap went to the edge, cutting it in half.
static inline bool hud_quick_bar_px(double cx, double cy, double gw, double gh, double shape) {
    return gw > 0 && gh > 0 && hud_quick_bar((float)(cx / gw * 2 - 1), (float)(1 - cy / gh * 2), 1, (float)shape);
}

// HUD at edges this frame: 0 off, 1 the HUD in play, 2 an event. Off in menus, the title and a load, like HUD size. In
// an event (a talk, a chest's "You got" box) only the top part moves (hud_edge_moves): a box's top edge went to the
// window's edge while the box and its text stayed in the 16:9 band (#599), and the area name, still fading out when an
// event starts, stays at the edge instead of dropping into the band.
static inline int hud_edge_mode(bool setting, float shape, bool menu, bool title, bool loading, bool event) {
    if (!setting || shape >= 1 || menu || title || loading) return 0;
    return event ? 2 : 1;
}

// Whether a pane centred at y goes to the edge in that mode
static inline bool hud_edge_moves(int mode, float y, float edge_y) {
    return mode == 1 || (mode == 2 && y > HUD_EDGE_TOP * edge_y);
}

// Whether a quick menu's bar is tracked (hud_quick): with HUD size or HUD at edges. Only HUD size did, so with HUD at
// edges alone the top pieces of the quick-select frames went to the window's top edge without the rest (#696).
static inline bool hud_quick_tracked(int hud_size, bool hud_edges) { return hud_size != 100 || hud_edges; }

// On windows taller than 16:9 (shape < 1), outer vertical anchors move from the 16:9 edge (shape) to the window edge (1)
static inline float hud_edge_dy(float ay, float edge_y, float shape) {
    if (shape >= 1 || edge_y <= 0) return 0;
    return (ay / edge_y) * (1.0f - shape);
}

// Offset a pane's view matrix vertically by dy in projected coordinates
static inline void hud_edge_view(float *v, float m11, float dy) {
    if (dy == 0) return;
    for (int k = 0; k < 4; k++)
        v[4 + k] -= dy / m11 * v[8 + k];
}

// Offset a matrix with the view already in it (text, the clock) vertically by dy
static inline void hud_edge_matrix(float *m, float dy) {
    if (dy == 0) return;
    for (int k = 0; k < 4; k++)
        m[4 + k] += dy * m[12 + k];
}

// Offset a scissor rectangle vertically on a screen taller than 16:9
static inline double hud_edge_scissor_dy(double y_center, double gh, double shape) {
    if (shape >= 1 || gh <= 0) return 0;
    float my = (float)(y_center / gh * 2.0 - 1.0);
    double dir = my > 1.0f / 3.0f ? 1.0 : my < -1.0f / 3.0f ? -1.0 : 0.0;
    return dir * (gh / 2.0) * (1.0 - shape);
}
