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

    puts("PASS: hud_scale");
}
