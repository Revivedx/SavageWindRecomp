#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/hud_scale.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4)

// x and y on screen of a point of a pane, through its view rows and the layer's projection
static void project(const float *v, float m00, float m11, float x, float y, float *nx, float *ny) {
    float vx = v[0] * x + v[1] * y + v[3], vy = v[4] * x + v[5] * y + v[7], vz = v[8] * x + v[9] * y + v[11];
    *nx = m00 * vx / -vz, *ny = m11 * vy / -vz;
}

int main(void) {
    // The zones: the middle third of the screen stays on the centre, the others go to their edge
    assert(hud_anchor(-0.9f, 1) == -1 && hud_anchor(0.9f, 1) == 1 && hud_anchor(0.1f, 1) == 0);
    assert(hud_anchor(-0.3f, 1) == 0 && hud_anchor(-0.34f, 1) == -1);
    assert(hud_anchor(0.5f, 0.75f) == 0.75f);  // (a window wider than 16:9: the HUD's edge is inside the window's)

    // A pane in the top left corner keeps the corner: 50% of the way there, half the size
    float m00 = 1.545f, m11 = 2.747f, v[12] = {1, 0, 0, -600, 0, 1, 0, 300, 0, 0, 1, -1000};
    float x0, y0, x1, y1, a0, b0, a1, b1;
    project(v, m00, m11, 0, 0, &x0, &y0);
    project(v, m00, m11, 100, -50, &x1, &y1);
    hud_scale_view(v, 0.5f, m00, m11, -1, 1);
    project(v, m00, m11, 0, 0, &a0, &b0);
    project(v, m00, m11, 100, -50, &a1, &b1);
    assert(NEAR(a0, -1 + (x0 + 1) * 0.5f) && NEAR(b0, 1 + (y0 - 1) * 0.5f));
    assert(NEAR(a1 - a0, (x1 - x0) * 0.5f) && NEAR(b1 - b0, (y1 - y0) * 0.5f));

    // The same with the view in the matrix (text, the clock): a point, row by row
    float m[16] = {0.002f, 0, 0, 0.8f, 0, -0.003f, 0, -0.8f, 0, 0, 0, 0, 0, 0, -1, 1};
    hud_scale_matrix(m, 0.5f, 1, -1);
    assert(NEAR(m[3] / m[15], 1 + (0.8f - 1) * 0.5f) && NEAR(m[7] / m[15], -1 + (-0.8f + 1) * 0.5f));
    assert(NEAR(m[0], 0.001f) && NEAR(m[5], -0.0015f));

    // The minimap's scissor in pixels goes with its corner: 1920 wide, the right edge stays
    double p = 1700, n = 200;
    hud_scale_span(&p, &n, 960, 960, 0.5f);
    assert(NEAR(n, 100) && NEAR(p, 1810));
    // and in the middle it stays around the centre
    p = 860, n = 200;
    hud_scale_span(&p, &n, 960, 960, 0.5f);
    assert(NEAR(n, 100) && NEAR(p, 910));

    // Bigger than the game's: from the same corner, 1.5 times the size
    p = 1700, n = 200;
    hud_scale_span(&p, &n, 960, 960, 1.5f);
    assert(NEAR(n, 300) && NEAR(p, 1590));
    float big[12] = {1, 0, 0, -600, 0, 1, 0, 300, 0, 0, 1, -1000};
    project(big, m00, m11, 0, 0, &x0, &y0);
    hud_scale_view(big, 1.5f, m00, m11, -1, 1);
    project(big, m00, m11, 0, 0, &a0, &b0);
    assert(NEAR(a0, -1 + (x0 + 1) * 1.5f) && NEAR(b0, 1 + (y0 - 1) * 1.5f));

    // A quick menu's bar keeps its size; the hearts, the minimap and the clock don't
    assert(hud_quick_keeps(0, 0.27f, 1, 1) && hud_quick_keeps(-0.5f, 0.3f, 1, 1) && hud_quick_keeps(0.64f, 0.63f, 1, 1));
    assert(!hud_quick_keeps(-0.85f, 0.85f, 1, 1) && !hud_quick_keeps(0.8f, -0.75f, 1, 1) && !hud_quick_keeps(0.5f, -0.8f, 1, 1));

    // HUD at edges on taller windows (shape < 1)
    assert(NEAR(hud_edge_dy(0.8f, 0.8f, 0.8f), 0.2f));
    assert(NEAR(hud_edge_dy(-0.8f, 0.8f, 0.8f), -0.2f));
    assert(NEAR(hud_edge_dy(0, 0.8f, 0.8f), 0));
    assert(NEAR(hud_edge_dy(0.8f, 0.8f, 1.2f), 0));  // (wide window: no vertical edge shift)

    // Top and bottom pane view matrices shifted to window edges
    float v_top[12] = {1, 0, 0, -600, 0, 1, 0, 300, 0, 0, 1, -1000};
    project(v_top, m00, m11, 0, 0, &x0, &y0);
    hud_edge_view(v_top, m11, 0.2f);
    project(v_top, m00, m11, 0, 0, &a0, &b0);
    assert(NEAR(a0, x0) && NEAR(b0, y0 + 0.2f));

    float v_bot[12] = {1, 0, 0, 600, 0, 1, 0, -300, 0, 0, 1, -1000};
    project(v_bot, m00, m11, 0, 0, &x0, &y0);
    hud_edge_view(v_bot, m11, -0.2f);
    project(v_bot, m00, m11, 0, 0, &a0, &b0);
    assert(NEAR(a0, x0) && NEAR(b0, y0 - 0.2f));

    // Matrix with view in it (clock, text)
    float m_top[16] = {0.002f, 0, 0, 0.8f, 0, -0.003f, 0, 0.6f, 0, 0, 0, 0, 0, 0, -1, 1};
    hud_edge_matrix(m_top, 0.2f);
    assert(NEAR(m_top[7] / m_top[15], 0.8f));

    // Scissor vertical offset on 1080h display with shape 0.8 (shift is 540 * 0.2 = 108)
    assert(NEAR(hud_edge_scissor_dy(900, 1080, 0.8), 108.0));
    assert(NEAR(hud_edge_scissor_dy(180, 1080, 0.8), -108.0));
    assert(NEAR(hud_edge_scissor_dy(540, 1080, 0.8), 0.0));

    // HUD at edges: off in a menu, the title and a load; in an event only its top part moves (#599)
    assert(hud_edge_mode(true, 0.9f, false, false, false, false) == 1);
    assert(hud_edge_mode(true, 0.9f, false, false, false, true) == 2);
    assert(!hud_edge_mode(false, 0.9f, false, false, false, false));
    assert(!hud_edge_mode(true, 1, false, false, false, false));
    assert(!hud_edge_mode(true, 0.9f, true, false, false, false));
    assert(!hud_edge_mode(true, 0.9f, false, true, false, false));
    assert(!hud_edge_mode(true, 0.9f, false, false, true, false));
    assert(hud_edge_moves(1, -0.8f, 0.9f) && hud_edge_moves(1, 0.3f, 0.9f) && !hud_edge_moves(0, 0.8f, 0.9f));
    // in an event: the area name (0.53 of the band) and the hearts go to the edge; a chest's item box (its top piece at
    // 0.36) and a talk box (low down) stay in the band
    assert(hud_edge_moves(2, 0.53f * 0.9f, 0.9f) && hud_edge_moves(2, 0.85f * 0.75f, 0.75f));
    assert(!hud_edge_moves(2, 0.36f * 0.9f, 0.9f) && !hud_edge_moves(2, -0.65f * 0.9f, 0.9f));

    // The quick menus' bar is kept in place with HUD at edges alone too, at HUD size 100 (#696)
    assert(hud_quick_tracked(100, true) && hud_quick_tracked(80, false) && !hud_quick_tracked(100, false));
    // and the quick-select frames at 16:10 (shape 0.9): their top pieces (y about 0.33) are inside the kept bar
    assert(hud_quick_keeps(0, 0.33f, 1, 0.9f) && hud_quick_keeps(0.4f, 0.4f, 1, 0.9f));

    // HUD at edges keeps only the bar (pieces up to ~0.41 of the band), not the area name over it (~0.53), at any shape
    assert(hud_quick_bar(0, 0.37f * 0.9f, 1, 0.9f) && hud_quick_bar(-0.4f, 0.40f * 0.75f, 1, 0.75f));
    assert(!hud_quick_bar(0, 0.53f * 0.9f, 1, 0.9f) && !hud_quick_bar(0, 0.53f * 0.75f, 1, 0.75f));
    assert(hud_quick_keeps(0, 0.53f * 0.9f, 1, 0.9f));  // (HUD size still keeps the name at the game's size)
    // The same for scissors, in pixels: at 1024x768 (shape 0.75) the bar's are kept, the minimap's mask is not
    assert(hud_quick_bar_px(512, 280, 1024, 768, 0.75) && hud_quick_bar_px(330, 300, 1024, 768, 0.75));
    assert(!hud_quick_bar_px(919, 622, 1024, 768, 0.75) && !hud_quick_bar_px(100, 60, 1024, 768, 0.75));
    assert(!hud_quick_bar_px(512, 230, 1024, 768, 0.75) && !hud_quick_bar_px(512, 384, 0, 768, 0.75));

    puts("PASS: hud_scale");
}
