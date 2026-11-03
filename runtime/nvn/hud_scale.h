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
