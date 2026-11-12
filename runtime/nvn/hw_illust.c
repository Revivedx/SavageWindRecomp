// Console illustrations for the device in use.
//
// The pause menu's Controls page (Layout/Common.sblarc, PaAllControllerTipsNN_00) explains the buttons over two
// drawings of the Switch: Nt_NNillust_00, the console from the front (576x288), and Nt_NNillust_01, from the top
// (576x100), R8G8 textures (grey, alpha) the layout tints. They are recognised on upload by size and contents, like the
// button glyphs (runtime/nvn/glyphs.c), and redrawn here for the device in use (the Steam Deck, an Xbox controller, a
// DualSense: glyphs_console): flat shapes in the original's greys (body 193 with a 3-texel outline of 135, bezel 103,
// screen 5, controls 13 / 29 / 55), so the page keeps its look; the page's lines are moved to that device's buttons
// (hw_illust_layout). The tips (PaMessageTipsDrcImageNN_00) show close-ups of the Switch (Nt_GamePadIllustSnn_00..03):
// they become the same parts of the device's drawing, enlarged. Labels are strokes, not a font. Nothing of Nintendo's, Valve's, Microsoft's or Sony's is shipped:
// the original is only measured, never copied, and the devices are drawn from their buttons' positions.
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "hw_illust.h"

typedef struct { uint16_t w, h; uint64_t hash; } Original;
static const Original k_originals[HW_ILLUST_COUNT] = {
    {576, 288, 0x8b42a6ff21f5dea4ull},  // Nt_NNillust_00 (FNV-1a of the texels, 1.6.0)
    {576, 100, 0x6e2b71b7ed623592ull},  // Nt_NNillust_01
    {228, 228, 0x158bd8dd5cc131c3ull},  // Nt_GamePadIllustSnn_00 (the tips' close-ups: ZL / L)
    {228, 228, 0x5ba630897a05fb12ull},  // Nt_GamePadIllustSnn_01 (ZR / R)
    {228, 228, 0x1c122818005987b1ull},  // Nt_GamePadIllustSnn_02 (the face buttons, the right stick)
    {228, 228, 0x1ae381a3464910a6ull},  // Nt_GamePadIllustSnn_03 (the left stick, the d-pad)
};

int hw_illust_which(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row) {
    uint64_t hash = 0;
    for (int i = 0; i < HW_ILLUST_COUNT; i++) {
        if (k_originals[i].w != w || k_originals[i].h != h) continue;
        if (!hash) {  // (once: the close-ups share a size)
            hash = 0xcbf29ce484222325ull;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w * 2; x++) hash = (hash ^ lin[(uint64_t)y * row + x]) * 0x100000001b3ull;
        }
        if (hash == k_originals[i].hash) return i;
    }
    return -1;
}

// ---- shapes: signed distances (texels, y down), drawn with a one-texel soft edge
typedef enum { SH_CIRCLE, SH_BOX, SH_SEG, SH_ELLIPSE } ShapeType;
typedef struct { ShapeType t; float p[4], r; } Shape;

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float sd(const Shape *s, float x, float y) {
    switch (s->t) {
    case SH_CIRCLE: return hypotf(x - s->p[0], y - s->p[1]) - s->r;
    case SH_ELLIPSE:  // centre, radii (near the edge, about the distance)
        return (hypotf((x - s->p[0]) / s->p[2], (y - s->p[1]) / s->p[3]) - 1) * minf(s->p[2], s->p[3]);
    case SH_BOX: {  // centre, half sizes, corner radius
        float qx = fabsf(x - s->p[0]) - s->p[2] + s->r, qy = fabsf(y - s->p[1]) - s->p[3] + s->r;
        return hypotf(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - s->r;
    }
    default: {  // a segment, r thick on each side
        float dx = s->p[2] - s->p[0], dy = s->p[3] - s->p[1];
        float h = clampf(((x - s->p[0]) * dx + (y - s->p[1]) * dy) / maxf(dx * dx + dy * dy, 1e-6f), 0, 1);
        return hypotf(x - s->p[0] - dx * h, y - s->p[1] - dy * h) - s->r;
    }
    }
}
static Shape circle(float x, float y, float r) { return (Shape){SH_CIRCLE, {x, y}, r}; }
static Shape box(float x, float y, float hw, float hh, float r) { return (Shape){SH_BOX, {x, y, hw, hh}, r}; }
static Shape seg(float ax, float ay, float bx, float by, float r) { return (Shape){SH_SEG, {ax, ay, bx, by}, r}; }
static Shape ellipse(float x, float y, float rx, float ry) { return (Shape){SH_ELLIPSE, {x, y, rx, ry}, 0}; }

typedef struct { uint8_t *lin; uint32_t w, h, row; } Paint;

// Paints shape s in grey l over what is there (alpha "over"); ring > 0: only a band that thick on its edge
static void paint_ring(Paint *p, Shape s, float ring, uint8_t l) {
    float x0, y0, x1, y1, m = s.r + ring + 2;
    if (s.t == SH_BOX) x0 = s.p[0] - s.p[2] - ring - 2, x1 = s.p[0] + s.p[2] + ring + 2, y0 = s.p[1] - s.p[3] - ring - 2,
                       y1 = s.p[1] + s.p[3] + ring + 2;
    else if (s.t == SH_CIRCLE) x0 = s.p[0] - m, x1 = s.p[0] + m, y0 = s.p[1] - m, y1 = s.p[1] + m;
    else x0 = minf(s.p[0], s.p[2]) - m, x1 = maxf(s.p[0], s.p[2]) + m, y0 = minf(s.p[1], s.p[3]) - m, y1 = maxf(s.p[1], s.p[3]) + m;
    int X0 = (int)maxf(floorf(x0), 0), X1 = (int)minf(ceilf(x1), (float)p->w), Y0 = (int)maxf(floorf(y0), 0),
        Y1 = (int)minf(ceilf(y1), (float)p->h);
    for (int y = Y0; y < Y1; y++)
        for (int x = X0; x < X1; x++) {
            float d = sd(&s, x + 0.5f, y + 0.5f);
            if (ring > 0) d = fabsf(d + ring / 2) - ring / 2;
            float c = clampf(0.5f - d, 0, 1);
            if (c <= 0) continue;
            uint8_t *t = p->lin + (uint64_t)y * p->row + x * 2;
            float a0 = t[1] / 255.0f, a = c + a0 * (1 - c);
            t[0] = (uint8_t)lroundf((l * c + t[0] * a0 * (1 - c)) / maxf(a, 1e-6f));
            t[1] = (uint8_t)lroundf(a * 255);
        }
}
static void paint(Paint *p, Shape s, uint8_t l) { paint_ring(p, s, 0, l); }

// ---- stroke labels (capitals and digits, h tall, centred on cx, cy)
static void label(Paint *p, const char *s, float cx, float cy, float h, uint8_t l) {
    float w = h * 0.62f, gap = h * 0.28f, t = maxf(h * 0.09f, 0.8f);
    float x = cx - (strlen(s) * w + (strlen(s) - 1) * gap) / 2, top = cy - h / 2, bot = cy + h / 2, mid = cy;
    for (; *s; s++, x += w + gap) {
        float r = x + w, c = x + w / 2;
        switch (*s) {
        case 'A':
            paint(p, seg(x, bot, c, top, t), l), paint(p, seg(c, top, r, bot, t), l);
            paint(p, seg(x + w * 0.22f, mid + h * 0.12f, r - w * 0.22f, mid + h * 0.12f, t), l);
            break;
        case 'B':
            paint(p, seg(x, top, x, bot, t), l);
            paint(p, seg(x, top, r - w * 0.3f, top, t), l), paint(p, seg(x, mid, r - w * 0.25f, mid, t), l);
            paint(p, seg(x, bot, r - w * 0.25f, bot, t), l);
            paint(p, seg(r - w * 0.3f, top, r - w * 0.05f, top + h * 0.12f, t), l);
            paint(p, seg(r - w * 0.05f, top + h * 0.12f, r - w * 0.05f, mid - h * 0.12f, t), l);
            paint(p, seg(r - w * 0.05f, mid - h * 0.12f, r - w * 0.3f, mid, t), l);
            paint(p, seg(r - w * 0.25f, mid, r, mid + h * 0.13f, t), l);
            paint(p, seg(r, mid + h * 0.13f, r, bot - h * 0.13f, t), l);
            paint(p, seg(r, bot - h * 0.13f, r - w * 0.25f, bot, t), l);
            break;
        case 'X': paint(p, seg(x, top, r, bot, t), l), paint(p, seg(r, top, x, bot, t), l); break;
        case 'Y':
            paint(p, seg(x, top, c, mid, t), l), paint(p, seg(r, top, c, mid, t), l), paint(p, seg(c, mid, c, bot, t), l);
            break;
        case 'L': paint(p, seg(x, top, x, bot, t), l), paint(p, seg(x, bot, r, bot, t), l); break;
        case 'T': paint(p, seg(x, top, r, top, t), l), paint(p, seg(c, top, c, bot, t), l); break;
        case 'R':
            paint(p, seg(x, top, x, bot, t), l), paint(p, seg(x, top, r - w * 0.3f, top, t), l);
            paint(p, seg(r - w * 0.3f, top, r, top + h * 0.15f, t), l), paint(p, seg(r, top + h * 0.15f, r, mid - h * 0.12f, t), l);
            paint(p, seg(r, mid - h * 0.12f, r - w * 0.3f, mid, t), l), paint(p, seg(x, mid, r - w * 0.3f, mid, t), l);
            paint(p, seg(r - w * 0.35f, mid, r, bot, t), l);
            break;
        case '1': paint(p, seg(c, top, c, bot, t), l), paint(p, seg(c, top, x + w * 0.1f, top + h * 0.22f, t), l); break;
        case '2':
            paint(p, seg(x, top + h * 0.15f, x + w * 0.3f, top, t), l), paint(p, seg(x + w * 0.3f, top, r - w * 0.3f, top, t), l);
            paint(p, seg(r - w * 0.3f, top, r, top + h * 0.15f, t), l), paint(p, seg(r, top + h * 0.15f, r, top + h * 0.35f, t), l);
            paint(p, seg(r, top + h * 0.35f, x, bot, t), l), paint(p, seg(x, bot, r, bot, t), l);
            break;
        default: break;
        }
    }
}

// The palette of the originals
enum { OUTLINE = 135, BODY = 193, BEZEL = 103, SCREEN = 5, DARK = 13, BUTTON = 29, CAP = 55, PAD = 172, MARK = 150, GRIP = 158 };

// The body: its pieces as one shape, their joins rounded `k` texels (a controller's grips flow into it), less `cut`
// (the arch between a controller's grips; NULL: none), its edges rounded too; the outline colour, then the body 3 in,
// in GRIP where it is inside one of the `ntone` shapes `tone` (a controller's darker grips, up to the outline)
static void body_tone(Paint *p, const Shape *parts, int n, float k, const Shape *cut, const Shape *tone, int ntone) {
    for (uint32_t y = 0; y < p->h; y++)
        for (uint32_t x = 0; x < p->w; x++) {
            float d = 1e9f, px = x + 0.5f, py = y + 0.5f;
            for (int i = 0; i < n; i++) {
                float e = sd(&parts[i], px, py);
                if (k > 0) {  // (polynomial smooth minimum)
                    float hh = clampf(0.5f + 0.5f * (e - d) / k, 0, 1);
                    d = e * (1 - hh) + d * hh - k * hh * (1 - hh);
                } else d = minf(d, e);
            }
            if (cut) {  // (smooth maximum with the cut's outside)
                float e = -sd(cut, px, py), kc = 10, hh = clampf(0.5f - 0.5f * (e - d) / kc, 0, 1);
                d = e * (1 - hh) + d * hh + kc * hh * (1 - hh);
            }
            float c = clampf(0.5f - d, 0, 1);
            if (c <= 0) continue;
            float in = clampf(0.5f - (d + 3), 0, 1), fill = BODY;
            for (int i = 0; i < ntone; i++) {
                float g = clampf(0.5f - sd(&tone[i], px, py), 0, 1);
                fill = fill * (1 - g) + GRIP * g;
            }
            uint8_t *t = p->lin + (uint64_t)y * p->row + x * 2;
            float a0 = t[1] / 255.0f, a = c + a0 * (1 - c), l = OUTLINE * (1 - in) + fill * in;
            t[0] = (uint8_t)lroundf((l * c + t[0] * a0 * (1 - c)) / maxf(a, 1e-6f));
            t[1] = (uint8_t)lroundf(a * 255);
        }
}
static void body_k(Paint *p, const Shape *parts, int n, float k) { body_tone(p, parts, n, k, NULL, NULL, 0); }
static void body(Paint *p, const Shape *parts, int n) { body_k(p, parts, n, 0); }

static void stick(Paint *p, float x, float y) {
    paint(p, circle(x, y, 17), DARK);
    paint(p, circle(x, y, 14), CAP);
    paint_ring(p, circle(x, y, 10), 1.4f, BUTTON);
}

static void face_button_r(Paint *p, float x, float y, const char *s, float r) {
    paint(p, circle(x, y, r), CAP);
    paint(p, circle(x, y, r * 0.8f), BUTTON);
    label(p, s, x, y, r * 0.8f, MARK);
}
static void face_button(Paint *p, float x, float y, const char *s) { face_button_r(p, x, y, s, 7.5f); }

static void dpad(Paint *p, float x, float y) {
    paint(p, box(x, y, 17, 6.5f, 2.5f), DARK), paint(p, box(x, y, 6.5f, 17, 2.5f), DARK);
    paint(p, box(x, y, 15.5f, 5, 2), CAP), paint(p, box(x, y, 5, 15.5f, 2), CAP);
    for (int i = 0; i < 4; i++) {  // the arrows' notches
        float dx = i == 2 ? -10.5f : i == 3 ? 10.5f : 0, dy = i == 0 ? -10.5f : i == 1 ? 10.5f : 0;
        paint(p, circle(x + dx, y + dy, 1.8f), BUTTON);
    }
}

static void trackpad(Paint *p, float x, float y) {
    paint(p, box(x, y, 26, 28, 7), OUTLINE);
    paint(p, box(x, y, 24, 26, 6), PAD);
}

static void pill(Paint *p, float x, float y, float hw, float hh) {
    paint(p, box(x, y, hw, hh, hh), BUTTON);
}

static void grille(Paint *p, float x, float y) {  // the speaker's holes
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 6 - (r & 1); c++) paint(p, circle(x - 10 + c * 4 + (r & 1) * 2, y - 4 + r * 4, 1.1f), BEZEL);
}

// The front: as the original, 576x288, the body from x 18 to 558, the screen nearly its height. Each side: the d-pad /
// face buttons outside at the top, the stick inside, the trackpad under it, STEAM / ... and the speaker under that;
// View / Menu above the stick. (The trackpads sit a little outward: the Controls page's left and right stick lines
// come up the gap between them and the screen.)
static void deck_drawn(Paint *p) {
    const Shape parts[] = {box(288, 144, 270, 108, 46)};
    body(p, parts, 1);
    paint(p, box(288, 144, 160, 101, 10), BEZEL);
    paint(p, box(288, 144, 145, 90, 3), SCREEN);
    dpad(p, 47, 73);
    stick(p, 106, 84);
    trackpad(p, 86, 146);
    pill(p, 80, 50, 6, 4);                                   // View
    paint(p, box(86, 190, 13, 5.5f, 2.5f), BUTTON);          // STEAM
    grille(p, 86, 212);
    face_button(p, 529, 60, "Y");
    face_button(p, 516, 73, "X");
    face_button(p, 542, 73, "B");
    face_button(p, 529, 86, "A");
    stick(p, 470, 84);
    trackpad(p, 490, 146);
    pill(p, 496, 50, 6, 4);                                  // Menu
    paint(p, box(490, 190, 13, 5.5f, 2.5f), BUTTON);         // ...
    for (int i = -1; i <= 1; i++) paint(p, circle(490 + i * 4.5f, 190, 1.2f), MARK);
    grille(p, 490, 212);
}

// The top: the shoulders at the ends (bumper in front, trigger behind), the power and volume buttons, the vents
static void deck_top_drawn(Paint *p) {
    paint(p, box(76, 24, 44, 14, 8), DARK), paint(p, box(76, 24, 42, 12, 7), CAP);      // L2
    paint(p, box(500, 24, 44, 14, 8), DARK), paint(p, box(500, 24, 42, 12, 7), CAP);    // R2
    const Shape parts[] = {box(288, 58, 270, 26, 20)};
    body(p, parts, 1);
    paint(p, box(76, 40, 48, 8, 6), DARK), paint(p, box(76, 40, 46, 6, 5), BUTTON);     // L1
    paint(p, box(500, 40, 48, 8, 6), DARK), paint(p, box(500, 40, 46, 6, 5), BUTTON);   // R1
    label(p, "L2", 76, 22, 9, BODY), label(p, "R2", 500, 22, 9, BODY);
    label(p, "L1", 76, 58, 8, OUTLINE), label(p, "R1", 500, 58, 8, OUTLINE);
    pill(p, 196, 36, 9, 3.5f);                                                            // power
    pill(p, 236, 36, 16, 3.5f);                                                           // volume
    for (int i = 0; i < 6; i++) paint(p, box(300 + i * 9, 50, 2, 9, 1), BEZEL);       // vents
    paint(p, box(380, 40, 7, 3.5f, 3), BUTTON);                                           // USB-C
    paint_ring(p, circle(404, 40, 3.5f), 1.4f, BUTTON);                                   // headphones
    paint(p, box(288, 86, 140, 8, 3), BEZEL);                                             // the screen's edge, below
}

// ---- controllers: a body with two grips, as the original's console is drawn (same greys and outline), about as tall
// as the texture so the Controls page shows it large. Positions measured off front photos of each one (x, y in texels).

// PlayStation's face symbols, cut in grey on a dark button
static void ps_symbol(Paint *p, float x, float y, int which, uint8_t l) {
    float r = 5.2f, t = 0.9f;
    switch (which) {
    case 0:  // triangle
        paint(p, seg(x, y - r, x - r * 0.9f, y + r * 0.6f, t), l), paint(p, seg(x - r * 0.9f, y + r * 0.6f, x + r * 0.9f, y + r * 0.6f, t), l);
        paint(p, seg(x + r * 0.9f, y + r * 0.6f, x, y - r, t), l);
        break;
    case 1: paint_ring(p, box(x, y, r * 0.75f, r * 0.75f, 0.5f), t * 2, l); break;  // square
    case 2: paint_ring(p, circle(x, y, r * 0.85f), t * 2, l); break;                 // circle
    default:  // cross
        paint(p, seg(x - r * 0.75f, y - r * 0.75f, x + r * 0.75f, y + r * 0.75f, t), l);
        paint(p, seg(x + r * 0.75f, y - r * 0.75f, x - r * 0.75f, y + r * 0.75f, t), l);
        break;
    }
}

static void pad_stick(Paint *p, float x, float y, float r) {
    paint(p, circle(x, y, r), DARK);
    paint(p, circle(x, y, r * 0.8f), CAP);
    paint_ring(p, circle(x, y, r * 0.55f), 1.4f, BUTTON);
}

// Xbox Series: the left stick above the d-pad on the left, the face buttons above the right stick; the Xbox button
// at the top centre, View and Menu below it, Share under them; the bumpers over the top edge
static void xbox_drawn(Paint *p) {
    // (an Xbox One / Series controller is 153 x 102 mm from the front: the photo's positions, its width to that)
    paint(p, box(176, 12, 40, 10, 9), DARK), paint(p, box(400, 12, 40, 10, 9), DARK);  // the bumpers, behind
    const Shape parts[] = {
        box(288, 130, 182, 122, 78),
        circle(140, 125, 48), circle(436, 125, 48),                // the shoulders' widest
        seg(150, 150, 172, 238, 42), seg(426, 150, 404, 238, 42),  // the grips
    };
    const Shape arch = ellipse(288, 283, 82, 88);                  // between the grips
    const Shape grips[] = {circle(118, 300, 128), circle(458, 300, 128)};  // (their darker outer part)
    body_tone(p, parts, 5, 34, &arch, grips, 2);
    paint(p, circle(180, 86, 27), OUTLINE), paint(p, circle(180, 86, 25.5f), BODY);      // the sticks' and d-pad's rims
    paint(p, circle(340, 142, 27), OUTLINE), paint(p, circle(340, 142, 25.5f), BODY);
    paint(p, circle(234, 148, 25), OUTLINE), paint(p, circle(234, 148, 23.5f), BODY);
    pad_stick(p, 180, 86, 23);
    paint(p, circle(234, 148, 22), DARK), paint(p, circle(234, 148, 20.5f), BUTTON);     // the d-pad's base
    paint(p, box(234, 148, 16, 5.5f, 2), CAP), paint(p, box(234, 148, 5.5f, 16, 2), CAP);
    pad_stick(p, 340, 142, 23);
    face_button_r(p, 402, 59, "Y", 10.5f);
    face_button_r(p, 375, 86, "X", 10.5f);
    face_button_r(p, 429, 86, "B", 10.5f);
    face_button_r(p, 402, 113, "A", 10.5f);
    paint(p, circle(288, 41, 13), CAP), paint(p, circle(288, 41, 11), BUTTON);          // the guide button (plain: no logo)
    paint(p, circle(257, 81, 6.5f), BUTTON), paint(p, circle(319, 81, 6.5f), BUTTON);   // View, Menu
    paint_ring(p, box(255.5f, 79.5f, 2, 1.6f, 0.4f), 0.8f, MARK), paint_ring(p, box(258.5f, 82.5f, 2, 1.6f, 0.4f), 0.8f, MARK);
    for (int i = -1; i <= 1; i++) paint(p, seg(316, 81 + i * 2.2f, 322, 81 + i * 2.2f, 0.55f), MARK);
    paint(p, box(288, 102, 8.5f, 4.2f, 4), BUTTON);                                     // Share (the Series')
}

static void deck_front(Paint *p) {
    deck_drawn(p);
}
static void deck_top(Paint *p) {
    deck_top_drawn(p);
}

static void xbox_front(Paint *p) {
    xbox_drawn(p);
}

// DualSense (PS5): the d-pad's four buttons and the face buttons high on the outside, the sticks low and inboard; the
// touchpad across the top, Create and Options beside it, the PS and mute buttons between the sticks
static void ps5_drawn(Paint *p) {
    const Shape parts[] = {
        box(288, 93, 170, 84, 60),
        seg(180, 150, 172, 240, 40), seg(396, 150, 404, 240, 40),  // the grips
    };
    body_k(p, parts, 3, 28);
    paint(p, box(289, 51, 70, 33, 9), OUTLINE), paint(p, box(289, 51, 68, 31, 8), PAD);  // the touchpad
    paint(p, seg(213, 30, 213, 48, 1.2f), BEZEL), paint(p, seg(365, 30, 365, 48, 1.2f), BEZEL);  // its light bars
    for (int i = 0; i < 4; i++) {  // the d-pad's buttons, each an arrow's key
        float dx = i == 2 ? -18 : i == 3 ? 18 : 0, dy = i == 0 ? -18 : i == 1 ? 18 : 0;
        bool across = i >= 2;
        paint(p, box(180 + dx, 66 + dy, across ? 9 : 7.5f, across ? 7.5f : 9, 2.5f), BUTTON);
    }
    pad_stick(p, 238, 112, 22);
    pad_stick(p, 340, 112, 22);
    static const float face[4][2] = {{399, 44}, {375, 68}, {423, 68}, {399, 92}};  // triangle, square, circle, cross
    for (int i = 0; i < 4; i++) {
        paint(p, circle(face[i][0], face[i][1], 11), CAP), paint(p, circle(face[i][0], face[i][1], 9.5f), BUTTON);
        ps_symbol(p, face[i][0], face[i][1], i, MARK);
    }
    pill(p, 208, 22, 3, 7), pill(p, 370, 22, 3, 7);                                        // Create, Options
    for (int r = 0; r < 2; r++)                                                             // the speaker
        for (int c = 0; c < 5; c++) paint(p, circle(281 + c * 4, 94 + r * 4, 1.1f), BEZEL);
    paint(p, circle(289, 117, 6), BUTTON);                                                  // PS
    pill(p, 289, 137, 9, 2.5f);                                                             // mute
}

static void ps5_front(Paint *p) {
    ps5_drawn(p);
}

// A controller from the top: its back edge between the shoulders, the bumper in front of the trigger on each side
static void pad_top(Paint *p, float lx, float rx, const char *b, const char *t, const char *rb, const char *rt) {
    paint(p, box(lx, 30, 40, 16, 10), DARK), paint(p, box(lx, 30, 38, 14, 9), CAP);
    paint(p, box(rx, 30, 40, 16, 10), DARK), paint(p, box(rx, 30, 38, 14, 9), CAP);
    const Shape parts[] = {box(288, 66, (rx - lx) / 2 + 52, 24, 22)};
    body(p, parts, 1);
    paint(p, box(lx, 48, 44, 8, 6), DARK), paint(p, box(lx, 48, 42, 6, 5), BUTTON);
    paint(p, box(rx, 48, 44, 8, 6), DARK), paint(p, box(rx, 48, 42, 6, 5), BUTTON);
    label(p, t, lx, 27, 9, BODY), label(p, rt, rx, 27, 9, BODY);
    label(p, b, lx, 66, 8, OUTLINE), label(p, rb, rx, 66, 8, OUTLINE);
    paint(p, box(288, 58, 9, 3.5f, 3), BUTTON);  // the USB port
}
static void xbox_top(Paint *p) {
    pad_top(p, 170, 406, "LB", "LT", "RB", "RT");
}
static void ps5_top(Paint *p) {
    pad_top(p, 207, 369, "L1", "L2", "R1", "R2");
}

// ---- the Controls page's lines (PaAllControllerTipsNN_00.bflyt), moved to the device's buttons
// The callouts run from the labels to the buttons in pieces (straight, corner, end dot, the button's diamond), panes
// of the layout, laid out again for each device. The Xbox controller keeps the Switch's order (the left stick above the
// d-pad): its lines are stretched to its buttons, the diamonds sized to them. The Steam Deck and the DualSense have the
// d-pad high outside and the left stick low inside: the d-pad's label rises to the d-pad (it grows downward, to three
// lines) and the left stick's goes below it, its line coming up into the stick (on the Deck up the gap beside the
// trackpad). The face buttons' label rises to them (up to the Menu label); View, Menu, the right stick and, on the
// controllers, the shoulders (from the top drawing) on every page; on the horse and sand seal pages the left stick and
// d-pad too on the Xbox (the Deck-like ones keep the Switch's there for now). Each pane's translation, size, rotation
// and scale, as the layout gives them; `was` is its translation in the game's layout, checked first (another layout:
// nothing changes). Worked out against the drawings above (the targets' texels; the page draws the front drawing at
// 2/3, centred 10 below its middle).
typedef struct { const char *name; float was[2], t[2], size[2], rot, scale[2]; } PaneEdit;
static const PaneEdit k_tips_deck[] = {
    {"N_OpenY_00", {123, 27}, {123, 36}, {60, 60}, 0, {1, 1}},
    {"P_Line_39", {-62.50f, 0}, {-71.13f, 0}, {4.13f, 10}, 180, {1, 1}},
    {"Nt_TipsLineCircle_00", {-30.50f, 0.00f}, {-39.13f, 0.00f}, {64, 64}, 180, {1, 1}},
    {"P_Line_07", {-17.50f, 110}, {17.23f, 149.47f}, {10, 10}, 90, {1, 1}},
    {"P_Line_08", {-17.50f, 105}, {17.23f, 34}, {110.47f, 10}, 90, {1, 1}},
    {"P_Line_09", {-17.50f, 29}, {17.23f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_13", {-22.50f, 29}, {12.23f, 29}, {66.23f, 10}, 0, {1, 1}},
    {"N_CrossKey_00", {-122, -15}, {-122, 40.27f}, {60, 60}, 0, {1, 1}},
    {"P_Line_15", {-64, 0}, {-72.73f, 0}, {4.73f, 10}, 180, {1, 1}},
    {"Nt_TipsLineCircle_03", {-32, 0}, {-40.73f, 0}, {64, 64}, 180, {1, 1}},
    {"P_Line_25", {-34, -82}, {-37.07f, -82}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_26", {-29, -82}, {-32.07f, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_86", {-29, -103}, {-32.07f, -96.87f}, {10, 10}, -90, {1, 1}},
    {"P_Line_87", {-29, -98}, {-32.07f, -91.87f}, {4.87f, 10}, -90, {1, 1}},
    {"P_Line_79", {-358, 211}, {-361.07f, 211}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_81", {-353, 211}, {-356.07f, 211}, {10, 10}, 0, {1, 1}},
    {"P_Line_82", {-353, 175}, {-356.07f, 181.13f}, {24.87f, 10}, -90, {1, 1}},
    {"P_Line_85", {-353, 170}, {-356.07f, 176.13f}, {10, 10}, -90, {1, 1}},
    {"P_Line_33", {-30, -3}, {-13.55f, -34.21f}, {10, 10}, -90, {1, 1}},
    {"P_Line_34", {-35, 14}, {-8.55f, 14}, {49.45f, 10}, 0, {1, 1}},
    {"P_Line_28", {-30, 2}, {-13.55f, -29.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_51", {-30, 14}, {-13.55f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_43", {-21, 0}, {-8.76f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_44", {-26, 0}, {-3.76f, 0}, {38.24f, 10}, 0, {1, 1}},
    {"P_Line_101", {-13, 0}, {-0.76f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_102", {-18, 0}, {4.24f, 0}, {38.24f, 10}, 0, {1, 1}},
    {"W_Base_02", {-192, 52}, {-192, -59.73f}, {180, 65}, 0, {1, 1}},
    {"P_Line_00", {-11, 2}, {34.63f, 12.87f}, {10, 10}, 90, {1, 1}},
    {"P_Line_01", {-11, 7}, {34.63f, 7.87f}, {87.60f, 10}, 90, {1, 1}},
    {"P_Line_02", {-11, 27}, {34.63f, -84.73f}, {10, 10}, 0, {1, -1}},
    {"P_Line_03", {-52, 27}, {-52, -84.73f}, {10, 10}, 180, {1, 1}},
    {"P_Line_04", {-52, 32}, {-52, -79.73f}, {25, 10}, -90, {1, 1}},
    {"P_Line_05", {-52, 22}, {-52, -89.73f}, {25, 10}, 90, {1, 1}},
    {"P_Line_06", {-16, 27}, {29.63f, -84.73f}, {76.63f, 10}, 0, {1, 1}},
    {"P_Line_29", {-35, 17}, {-8.55f, 17}, {49.45f, 10}, 0, {1, 1}},
    {"P_Line_45", {-30, 17}, {-13.55f, 17}, {10, 10}, 0, {1, 1}},
    {"P_Line_49", {-30, 5}, {-13.55f, -26.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_50", {-30, 0}, {-13.55f, -31.21f}, {10, 10}, -90, {1, 1}},
    {"T_Icon_16", {-215, 61}, {-215, -50.73f}, {36, 36}, 0, {1, 1}},
    {"T_Lstick_00", {-232, 61}, {-232, -50.73f}, {136, 22}, 0, {1, 1}},
    {"T_Lstick_01", {-204, 38}, {-204, -73.73f}, {157, 22}, 0, {1, 1}},
    {"P_Line_56", {-52, -2}, {-52, -9.73f}, {10, 10}, 180, {1, 1}},
    {"P_Line_57", {-52, 3}, {-52, -4.73f}, {51.73f, 10}, -90, {1, 1}},
    {"P_Line_67", {-52, -7}, {-52, -14.73f}, {36.27f, 10}, 90, {1, 1}},
    {"P_Line_88", {-16, -2}, {-5.07f, -9.73f}, {41.93f, 10}, 0, {1, 1}},
    {"P_Line_89", {-11, -2}, {-0.07f, -9.73f}, {10, 10}, 0, {1, 1}},
    {"P_Line_74", {-20, -9}, {-3.55f, -40.21f}, {10, 10}, -90, {1, 1}},
    {"P_Line_75", {-20, -4}, {-3.55f, -35.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_76", {-20, 8}, {-3.55f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_77", {-25, 8}, {1.45f, 8}, {49.45f, 10}, 0, {1, 1}},
    {"P_Line_78", {-32, -26}, {-9.76f, -26}, {38.24f, 10}, 0, {1, 1}},
    {"P_Line_84", {-27, -26}, {-14.76f, -26}, {10, 10}, 180, {1, 1}},
    {"P_Line_20", {-17.50f, 110}, {17.23f, 149.47f}, {10, 10}, 90, {1, 1}},
    {"P_Line_21", {-17.50f, 105}, {17.23f, 34}, {110.47f, 10}, 90, {1, 1}},
    {"P_Line_27", {-17.50f, 29}, {17.23f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_98", {-22.50f, 29}, {12.23f, 29}, {66.23f, 10}, 0, {1, 1}},
    {"P_Line_99", {-34, -82}, {-37.07f, -82}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_100", {-29, -82}, {-32.07f, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_109", {-29, -103}, {-32.07f, -96.87f}, {10, 10}, -90, {1, 1}},
    {"P_Line_110", {-29, -98}, {-32.07f, -91.87f}, {4.87f, 10}, -90, {1, 1}},
    {"P_Line_113", {-358, 205}, {-361.07f, 205}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_115", {-353, 205}, {-356.07f, 205}, {10, 10}, 0, {1, 1}},
    {"P_Line_116", {-353, 175}, {-356.07f, 181.13f}, {18.87f, 10}, -90, {1, 1}},
    {"P_Line_117", {-353, 170}, {-356.07f, 176.13f}, {10, 10}, -90, {1, 1}},
    {"P_Line_118", {-30, -3}, {-13.55f, -34.21f}, {10, 10}, -90, {1, 1}},
    {"P_Line_119", {-35, 14}, {-8.55f, 14}, {49.45f, 10}, 0, {1, 1}},
    {"P_Line_123", {-30, 2}, {-13.55f, -29.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_124", {-30, 14}, {-13.55f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_125", {-21, 0}, {-8.76f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_126", {-26, 0}, {-3.76f, 0}, {38.24f, 10}, 0, {1, 1}},
    {"N_OpenY_02", {123, 27}, {123, 36}, {60, 60}, 0, {1, 1}},
    {"P_Line_130", {-62.50f, 0}, {-71.13f, 0}, {4.13f, 10}, 180, {1, 1}},
    {"Nt_TipsLineCircle_04", {-30.50f, 0.00f}, {-39.13f, 0.00f}, {64, 64}, 180, {1, 1}},
    {"P_Line_134", {-37, -39}, {-45.73f, -39}, {22.27f, 10}, 0, {1, 1}},
    {"P_Line_139", {-32, -34}, {-40.73f, 32.87f}, {66.87f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_05", {-32, 0}, {-40.73f, 55.27f}, {44.80f, 44.80f}, -90, {1, 1}},
    {"P_Line_138", {-32, -39}, {-40.73f, -39}, {10, 10}, -90, {1, 1}},
    {"P_Line_38", {-11, -2}, {-0.07f, -9.73f}, {10, 10}, 0, {1, 1}},
    {"P_Line_52", {-16, -2}, {-5.07f, -9.73f}, {41.93f, 10}, 0, {1, 1}},
    {"P_Line_53", {-52, 3}, {-52, -4.73f}, {33.73f, 10}, -90, {1, 1}},
    {"P_Line_54", {-52, -2}, {-52, -9.73f}, {10, 10}, 180, {1, 1}},
    {"P_Line_55", {-52, -7}, {-52, -14.73f}, {18.27f, 10}, 90, {1, 1}},
    {"P_Line_24", {-20, -9}, {-3.55f, -40.21f}, {10, 10}, -90, {1, 1}},
    {"P_Line_68", {-20, -4}, {-3.55f, -35.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_72", {-20, 8}, {-3.55f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_73", {-25, 8}, {1.45f, 8}, {49.45f, 10}, 0, {1, 1}},
    {"N_Line_13", {142, 27}, {142, 36}, {30, 40}, 0, {-1, 1}},
    {"P_Line_58", {-43.50f, 0}, {-52.13f, 0}, {4.13f, 10}, 180, {1, 1}},
    {"Nt_TipsLineCircle_02", {-11.50f, 0}, {-20.13f, 0}, {64, 64}, 180, {1, 1}},
    {"P_Line_14", {-37, -44}, {-45.73f, -44}, {22.27f, 10}, 0, {1, 1}},
    {"P_Line_140", {-32, -39}, {-40.73f, 32.87f}, {71.87f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_01", {-32, 0}, {-40.73f, 55.27f}, {44.80f, 44.80f}, -90, {1, 1}},
    {"P_Line_141", {-32, -44}, {-40.73f, -44}, {10, 10}, -90, {1, 1}},
    {"P_Line_142", {-30, -3}, {-13.55f, -34.21f}, {10, 10}, -90, {1, 1}},
    {"P_Line_143", {-35, 14}, {-8.55f, 14}, {49.45f, 10}, 0, {1, 1}},
    {"P_Line_147", {-30, 2}, {-13.55f, -29.21f}, {38.21f, 10}, -90, {1, 1}},
    {"P_Line_148", {-30, 14}, {-13.55f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_149", {-34, -86}, {-37.07f, -86}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_150", {-29, -86}, {-32.07f, -86}, {10, 10}, 0, {1, 1}},
    {"P_Line_154", {-29, -103}, {-32.07f, -96.87f}, {10, 10}, -90, {1, 1}},
    {"P_Line_155", {-29, -98}, {-32.07f, -91.87f}, {0.87f, 10}, -90, {1, 1}},
    {"P_Line_158", {-358, 187}, {-361.07f, 187}, {43.93f, 10}, 0, {1, 1}},
    {"P_Line_160", {-353, 187}, {-356.07f, 187}, {10, 10}, 0, {1, 1}},
    {"P_Line_161", {-353, 175}, {-356.07f, 181.13f}, {0.87f, 10}, -90, {1, 1}},
    {"P_Line_162", {-353, 170}, {-356.07f, 176.13f}, {10, 10}, -90, {1, 1}},
    {"P_Line_163", {-17.50f, 110}, {17.23f, 149.47f}, {10, 10}, 90, {1, 1}},
    {"P_Line_164", {-17.50f, 105}, {17.23f, 34}, {110.47f, 10}, 90, {1, 1}},
    {"P_Line_165", {-17.50f, 29}, {17.23f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_169", {-22.50f, 29}, {12.23f, 29}, {66.23f, 10}, 0, {1, 1}},
};
static const PaneEdit k_tips_xbox[] = {
    {"N_OpenY_00", {123, 27}, {123, 30.00f}, {60, 60}, 0, {1, 1}},
    {"P_Line_39", {-62.50f, 0}, {13.03f, 0}, {80.03f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_00", {-30.50f, 0.00f}, {54.33f, 0.00f}, {82.61f, 82.61f}, 180, {1, 1}},
    {"P_Line_07", {-17.50f, 110}, {108.63f, 105.67f}, {10, 10}, 90, {1, 1}},
    {"P_Line_08", {-17.50f, 105}, {108.63f, 34}, {66.67f, 10}, 90, {1, 1}},
    {"P_Line_09", {-17.50f, 29}, {108.63f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_13", {-22.50f, 29}, {103.63f, 29}, {157.63f, 10}, 0, {1, 1}},
    {"N_CrossKey_00", {-122, -15}, {-122, -11.33f}, {60, 60}, 0, {1, 1}},
    {"P_Line_15", {-64, 0}, {54.65f, 0}, {122.65f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_03", {-32, 0}, {87.33f, 0}, {65.36f, 65.36f}, 180, {1, 1}},
    {"P_Line_25", {-34, -82}, {84, -82}, {165, 10}, 0, {1, 1}},
    {"P_Line_26", {-29, -82}, {89, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_86", {-29, -103}, {89, -122.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_87", {-29, -98}, {89, -117.33f}, {30.33f, 10}, -90, {1, 1}},
    {"P_Line_79", {-358, 211}, {-239.33f, 211}, {165.67f, 10}, 0, {1, 1}},
    {"P_Line_81", {-353, 211}, {-234.33f, 211}, {10, 10}, 0, {1, 1}},
    {"P_Line_82", {-353, 175}, {-234.33f, 155.67f}, {50.33f, 10}, -90, {1, 1}},
    {"P_Line_85", {-353, 170}, {-234.33f, 150.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_33", {-30, -3}, {79.96f, -34.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_34", {-35, 14}, {84.96f, 14}, {142.96f, 10}, 0, {1, 1}},
    {"P_Line_28", {-30, 2}, {79.96f, -29.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_51", {-30, 14}, {79.96f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_43", {-21, 0}, {89.65f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_44", {-26, 0}, {94.65f, 0}, {136.65f, 10}, 0, {1, 1}},
    {"P_Line_101", {-13, 0}, {97.65f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_102", {-18, 0}, {102.65f, 0}, {136.65f, 10}, 0, {1, 1}},
    {"P_Line_00", {-11, 2}, {74.33f, 21.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_01", {-11, 7}, {74.33f, 26.33f}, {4.33f, 10}, 90, {1, 1}},
    {"P_Line_02", {-11, 27}, {74.33f, 27}, {10, 10}, 0, {1, 1}},
    {"P_Line_06", {-16, 27}, {69.33f, 27}, {116.33f, 10}, 0, {1, 1}},
    {"P_Line_29", {-35, 17}, {84.96f, 17}, {142.96f, 10}, 0, {1, 1}},
    {"P_Line_45", {-30, 17}, {79.96f, 17}, {10, 10}, 0, {1, 1}},
    {"P_Line_49", {-30, 5}, {79.96f, -26.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_50", {-30, 0}, {79.96f, -31.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_88", {-16, -2}, {51.67f, -2}, {98.67f, 10}, 0, {1, 1}},
    {"P_Line_89", {-11, -2}, {56.67f, -2}, {10, 10}, 0, {1, 1}},
    {"P_Line_74", {-20, -9}, {89.96f, -40.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_75", {-20, -4}, {89.96f, -35.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_76", {-20, 8}, {89.96f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_77", {-25, 8}, {94.96f, 8}, {142.96f, 10}, 0, {1, 1}},
    {"P_Line_78", {-32, -26}, {88.65f, -26}, {136.65f, 10}, 0, {1, 1}},
    {"P_Line_84", {-27, -26}, {83.65f, -26}, {10, 10}, 180, {1, 1}},
    {"P_Line_20", {-17.50f, 110}, {108.63f, 105.67f}, {10, 10}, 90, {1, 1}},
    {"P_Line_21", {-17.50f, 105}, {108.63f, 34}, {66.67f, 10}, 90, {1, 1}},
    {"P_Line_27", {-17.50f, 29}, {108.63f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_98", {-22.50f, 29}, {103.63f, 29}, {157.63f, 10}, 0, {1, 1}},
    {"P_Line_99", {-34, -82}, {84, -82}, {165, 10}, 0, {1, 1}},
    {"P_Line_100", {-29, -82}, {89, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_109", {-29, -103}, {89, -122.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_110", {-29, -98}, {89, -117.33f}, {30.33f, 10}, -90, {1, 1}},
    {"P_Line_113", {-358, 205}, {-239.33f, 205}, {165.67f, 10}, 0, {1, 1}},
    {"P_Line_115", {-353, 205}, {-234.33f, 205}, {10, 10}, 0, {1, 1}},
    {"P_Line_116", {-353, 175}, {-234.33f, 155.67f}, {44.33f, 10}, -90, {1, 1}},
    {"P_Line_117", {-353, 170}, {-234.33f, 150.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_118", {-30, -3}, {79.96f, -34.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_119", {-35, 14}, {84.96f, 14}, {142.96f, 10}, 0, {1, 1}},
    {"P_Line_123", {-30, 2}, {79.96f, -29.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_124", {-30, 14}, {79.96f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_125", {-21, 0}, {89.65f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_126", {-26, 0}, {94.65f, 0}, {136.65f, 10}, 0, {1, 1}},
    {"N_OpenY_02", {123, 27}, {123, 30.00f}, {60, 60}, 0, {1, 1}},
    {"P_Line_130", {-62.50f, 0}, {13.03f, 0}, {80.03f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_04", {-30.50f, 0.00f}, {54.33f, 0.00f}, {82.61f, 82.61f}, 180, {1, 1}},
    {"P_Line_134", {-37, -39}, {82.33f, -39}, {150.33f, 10}, 0, {1, 1}},
    {"P_Line_139", {-32, -34}, {87.33f, -29.01f}, {4.99f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_05", {-32, 0}, {87.33f, 3.67f}, {65.36f, 65.36f}, -90, {1, 1}},
    {"P_Line_138", {-32, -39}, {87.33f, -39}, {10, 10}, -90, {1, 1}},
    {"P_Line_38", {-11, -2}, {56.67f, -2}, {10, 10}, 0, {1, 1}},
    {"P_Line_52", {-16, -2}, {51.67f, -2}, {98.67f, 10}, 0, {1, 1}},
    {"P_Line_24", {-20, -9}, {89.96f, -40.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_68", {-20, -4}, {89.96f, -35.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_72", {-20, 8}, {89.96f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_73", {-25, 8}, {94.96f, 8}, {142.96f, 10}, 0, {1, 1}},
    {"N_Line_13", {142, 27}, {142, 30}, {30, 40}, 0, {-1, 1}},
    {"P_Line_58", {-43.50f, 0}, {32.03f, 0}, {80.03f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_02", {-11.50f, 0}, {73.33f, 0}, {82.61f, 82.61f}, 180, {1, 1}},
    {"P_Line_14", {-37, -44}, {82.33f, -44}, {150.33f, 10}, 0, {1, 1}},
    {"P_Line_140", {-32, -39}, {87.33f, -29.01f}, {9.99f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_01", {-32, 0}, {87.33f, 3.67f}, {65.36f, 65.36f}, -90, {1, 1}},
    {"P_Line_141", {-32, -44}, {87.33f, -44}, {10, 10}, -90, {1, 1}},
    {"P_Line_142", {-30, -3}, {79.96f, -34.64f}, {10, 10}, -90, {1, 1}},
    {"P_Line_143", {-35, 14}, {84.96f, 14}, {142.96f, 10}, 0, {1, 1}},
    {"P_Line_147", {-30, 2}, {79.96f, -29.64f}, {38.64f, 10}, -90, {1, 1}},
    {"P_Line_148", {-30, 14}, {79.96f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_149", {-34, -86}, {84, -86}, {165, 10}, 0, {1, 1}},
    {"P_Line_150", {-29, -86}, {89, -86}, {10, 10}, 0, {1, 1}},
    {"P_Line_154", {-29, -103}, {89, -122.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_155", {-29, -98}, {89, -117.33f}, {26.33f, 10}, -90, {1, 1}},
    {"P_Line_158", {-358, 187}, {-239.33f, 187}, {165.67f, 10}, 0, {1, 1}},
    {"P_Line_160", {-353, 187}, {-234.33f, 187}, {10, 10}, 0, {1, 1}},
    {"P_Line_161", {-353, 175}, {-234.33f, 155.67f}, {26.33f, 10}, -90, {1, 1}},
    {"P_Line_162", {-353, 170}, {-234.33f, 150.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_163", {-17.50f, 110}, {108.63f, 105.67f}, {10, 10}, 90, {1, 1}},
    {"P_Line_164", {-17.50f, 105}, {108.63f, 34}, {66.67f, 10}, 90, {1, 1}},
    {"P_Line_165", {-17.50f, 29}, {108.63f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_169", {-22.50f, 29}, {103.63f, 29}, {157.63f, 10}, 0, {1, 1}},
};
static const PaneEdit k_tips_ps5[] = {
    {"N_OpenY_00", {123, 27}, {123, 30.67f}, {60, 60}, 0, {1, 1}},
    {"P_Line_39", {-62.50f, 0}, {-10.87f, 0}, {56.13f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_00", {-30.50f, 0.00f}, {36.33f, 0.00f}, {94.41f, 94.41f}, 180, {1, 1}},
    {"P_Line_07", {-17.50f, 110}, {99.30f, 107}, {10, 10}, 90, {1, 1}},
    {"P_Line_08", {-17.50f, 105}, {99.30f, 34}, {68, 10}, 90, {1, 1}},
    {"P_Line_09", {-17.50f, 29}, {99.30f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_13", {-22.50f, 29}, {94.30f, 29}, {148.30f, 10}, 0, {1, 1}},
    {"N_CrossKey_00", {-122, -15}, {-122, 30.67f}, {60, 60}, 0, {1, 1}},
    {"P_Line_15", {-64, 0}, {-3.70f, 0}, {64.30f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_03", {-32, 0}, {35.33f, 0}, {78.07f, 78.07f}, 180, {1, 1}},
    {"P_Line_25", {-34, -82}, {39.33f, -82}, {120.33f, 10}, 0, {1, 1}},
    {"P_Line_26", {-29, -82}, {44.33f, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_86", {-29, -103}, {44.33f, -92.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_87", {-29, -98}, {44.33f, -87.33f}, {0.33f, 10}, -90, {1, 1}},
    {"P_Line_79", {-358, 211}, {-285.33f, 211}, {119.67f, 10}, 0, {1, 1}},
    {"P_Line_81", {-353, 211}, {-280.33f, 211}, {10, 10}, 0, {1, 1}},
    {"P_Line_82", {-353, 175}, {-280.33f, 185.67f}, {20.33f, 10}, -90, {1, 1}},
    {"P_Line_85", {-353, 170}, {-280.33f, 180.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_33", {-30, -3}, {77.87f, -42.20f}, {10, 10}, -90, {1, 1}},
    {"P_Line_34", {-35, 14}, {82.87f, 14}, {140.87f, 10}, 0, {1, 1}},
    {"P_Line_28", {-30, 2}, {77.87f, -37.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_51", {-30, 14}, {77.87f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_43", {-21, 0}, {86.07f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_44", {-26, 0}, {91.07f, 0}, {133.07f, 10}, 0, {1, 1}},
    {"P_Line_101", {-13, 0}, {94.44f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_102", {-18, 0}, {99.44f, 0}, {133.44f, 10}, 0, {1, 1}},
    {"W_Base_02", {-192, 52}, {-192, -69.33f}, {180, 65}, 0, {1, 1}},
    {"P_Line_00", {-11, 2}, {97.67f, -44.73f}, {10, 10}, 90, {1, 1}},
    {"P_Line_01", {-11, 7}, {97.67f, -49.73f}, {39.60f, 10}, 90, {1, 1}},
    {"P_Line_02", {-11, 27}, {97.67f, -94.33f}, {10, 10}, 0, {1, -1}},
    {"P_Line_03", {-52, 27}, {-52, -94.33f}, {10, 10}, 180, {1, 1}},
    {"P_Line_04", {-52, 32}, {-52, -89.33f}, {25, 10}, -90, {1, 1}},
    {"P_Line_05", {-52, 22}, {-52, -99.33f}, {25, 10}, 90, {1, 1}},
    {"P_Line_06", {-16, 27}, {92.67f, -94.33f}, {139.67f, 10}, 0, {1, 1}},
    {"P_Line_29", {-35, 17}, {83.24f, 17}, {141.24f, 10}, 0, {1, 1}},
    {"P_Line_45", {-30, 17}, {78.24f, 17}, {10, 10}, 0, {1, 1}},
    {"P_Line_49", {-30, 5}, {78.24f, -34.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_50", {-30, 0}, {78.24f, -39.20f}, {10, 10}, -90, {1, 1}},
    {"T_Icon_16", {-215, 61}, {-215, -60.33f}, {36, 36}, 0, {1, 1}},
    {"T_Lstick_00", {-232, 61}, {-232, -60.33f}, {136, 22}, 0, {1, 1}},
    {"T_Lstick_01", {-204, 38}, {-204, -83.33f}, {157, 22}, 0, {1, 1}},
    {"P_Line_56", {-52, -2}, {-52, -46.97f}, {10, 10}, 180, {1, 1}},
    {"P_Line_57", {-52, 3}, {-52, -41.97f}, {88.97f, 10}, -90, {1, 1}},
    {"P_Line_67", {-52, -7}, {-52, -51.97f}, {0.97f, 10}, 270, {1, 1}},
    {"P_Line_88", {-16, -2}, {72.33f, -46.97f}, {119.33f, 10}, 0, {1, 1}},
    {"P_Line_89", {-11, -2}, {77.33f, -46.97f}, {10, 10}, 0, {1, 1}},
    {"P_Line_74", {-20, -9}, {88.24f, -48.20f}, {10, 10}, -90, {1, 1}},
    {"P_Line_75", {-20, -4}, {88.24f, -43.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_76", {-20, 8}, {88.24f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_77", {-25, 8}, {93.24f, 8}, {141.24f, 10}, 0, {1, 1}},
    {"P_Line_78", {-32, -26}, {85.44f, -26}, {133.44f, 10}, 0, {1, 1}},
    {"P_Line_84", {-27, -26}, {80.44f, -26}, {10, 10}, 180, {1, 1}},
    {"P_Line_20", {-17.50f, 110}, {99.30f, 107}, {10, 10}, 90, {1, 1}},
    {"P_Line_21", {-17.50f, 105}, {99.30f, 34}, {68, 10}, 90, {1, 1}},
    {"P_Line_27", {-17.50f, 29}, {99.30f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_98", {-22.50f, 29}, {94.30f, 29}, {148.30f, 10}, 0, {1, 1}},
    {"P_Line_99", {-34, -82}, {39.33f, -82}, {120.33f, 10}, 0, {1, 1}},
    {"P_Line_100", {-29, -82}, {44.33f, -82}, {10, 10}, 0, {1, 1}},
    {"P_Line_109", {-29, -103}, {44.33f, -92.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_110", {-29, -98}, {44.33f, -87.33f}, {0.33f, 10}, -90, {1, 1}},
    {"P_Line_113", {-358, 205}, {-285.33f, 205}, {119.67f, 10}, 0, {1, 1}},
    {"P_Line_115", {-353, 205}, {-280.33f, 205}, {10, 10}, 0, {1, 1}},
    {"P_Line_116", {-353, 175}, {-280.33f, 185.67f}, {14.33f, 10}, -90, {1, 1}},
    {"P_Line_117", {-353, 170}, {-280.33f, 180.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_118", {-30, -3}, {77.87f, -42.20f}, {10, 10}, -90, {1, 1}},
    {"P_Line_119", {-35, 14}, {82.87f, 14}, {140.87f, 10}, 0, {1, 1}},
    {"P_Line_123", {-30, 2}, {77.87f, -37.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_124", {-30, 14}, {77.87f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_125", {-21, 0}, {86.07f, 0}, {10, 10}, 180, {1, 1}},
    {"P_Line_126", {-26, 0}, {91.07f, 0}, {133.07f, 10}, 0, {1, 1}},
    {"N_OpenY_02", {123, 27}, {123, 30.67f}, {60, 60}, 0, {1, 1}},
    {"P_Line_130", {-62.50f, 0}, {-10.87f, 0}, {56.13f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_04", {-30.50f, 0.00f}, {36.33f, 0.00f}, {94.41f, 94.41f}, 180, {1, 1}},
    {"P_Line_134", {-37, -39}, {30.33f, -39}, {98.33f, 10}, 0, {1, 1}},
    {"P_Line_139", {-32, -34}, {35.33f, 6.63f}, {40.63f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_05", {-32, 0}, {35.33f, 45.67f}, {78.07f, 78.07f}, -90, {1, 1}},
    {"P_Line_138", {-32, -39}, {35.33f, -39}, {10, 10}, -90, {1, 1}},
    {"P_Line_38", {-11, -2}, {77.33f, -46.97f}, {10, 10}, 0, {1, 1}},
    {"P_Line_52", {-16, -2}, {72.33f, -46.97f}, {119.33f, 10}, 0, {1, 1}},
    {"P_Line_53", {-52, 3}, {-52, -41.97f}, {70.97f, 10}, -90, {1, 1}},
    {"P_Line_54", {-52, -2}, {-52, -46.97f}, {10, 10}, 180, {1, 1}},
    {"P_Line_55", {-52, -7}, {-52, -51.97f}, {18.97f, 10}, 270, {1, 1}},
    {"P_Line_24", {-20, -9}, {88.24f, -48.20f}, {10, 10}, -90, {1, 1}},
    {"P_Line_68", {-20, -4}, {88.24f, -43.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_72", {-20, 8}, {88.24f, 8}, {10, 10}, 0, {1, 1}},
    {"P_Line_73", {-25, 8}, {93.24f, 8}, {141.24f, 10}, 0, {1, 1}},
    {"N_Line_13", {142, 27}, {142, 30.67f}, {30, 40}, 0, {-1, 1}},
    {"P_Line_58", {-43.50f, 0}, {8.13f, 0}, {56.13f, 10}, 0, {1, 1}},
    {"Nt_TipsLineCircle_02", {-11.50f, 0}, {55.33f, 0}, {94.41f, 94.41f}, 180, {1, 1}},
    {"P_Line_14", {-37, -44}, {30.33f, -44}, {98.33f, 10}, 0, {1, 1}},
    {"P_Line_140", {-32, -39}, {35.33f, 6.63f}, {45.63f, 10}, 90, {1, 1}},
    {"Nt_TipsLineCircle_01", {-32, 0}, {35.33f, 45.67f}, {78.07f, 78.07f}, -90, {1, 1}},
    {"P_Line_141", {-32, -44}, {35.33f, -44}, {10, 10}, -90, {1, 1}},
    {"P_Line_142", {-30, -3}, {77.87f, -42.20f}, {10, 10}, -90, {1, 1}},
    {"P_Line_143", {-35, 14}, {82.87f, 14}, {140.87f, 10}, 0, {1, 1}},
    {"P_Line_147", {-30, 2}, {77.87f, -37.20f}, {46.20f, 10}, -90, {1, 1}},
    {"P_Line_148", {-30, 14}, {77.87f, 14}, {10, 10}, 0, {1, 1}},
    {"P_Line_149", {-34, -86}, {39.33f, -86}, {120.33f, 10}, 0, {1, 1}},
    {"P_Line_150", {-29, -86}, {44.33f, -86}, {10, 10}, 0, {1, 1}},
    {"P_Line_154", {-29, -103}, {44.33f, -92.33f}, {10, 10}, -90, {1, 1}},
    {"P_Line_155", {-29, -98}, {44.33f, -87.33f}, {3.67f, 10}, 90, {1, 1}},
    {"P_Line_158", {-358, 187}, {-285.33f, 187}, {119.67f, 10}, 0, {1, 1}},
    {"P_Line_160", {-353, 187}, {-280.33f, 187}, {10, 10}, 0, {1, 1}},
    {"P_Line_161", {-353, 175}, {-280.33f, 185.67f}, {3.67f, 10}, 90, {1, 1}},
    {"P_Line_162", {-353, 170}, {-280.33f, 180.67f}, {10, 10}, -90, {1, 1}},
    {"P_Line_163", {-17.50f, 110}, {99.30f, 107}, {10, 10}, 90, {1, 1}},
    {"P_Line_164", {-17.50f, 105}, {99.30f, 34}, {68, 10}, 90, {1, 1}},
    {"P_Line_165", {-17.50f, 29}, {99.30f, 29}, {10, 10}, -90, {1, 1}},
    {"P_Line_169", {-22.50f, 29}, {94.30f, 29}, {148.30f, 10}, 0, {1, 1}},
};
static const struct { const PaneEdit *e; size_t n; } k_tips[HW_DEVICE_COUNT] = {
    {k_tips_deck, sizeof k_tips_deck / sizeof *k_tips_deck},
    {k_tips_xbox, sizeof k_tips_xbox / sizeof *k_tips_xbox},
    {k_tips_ps5, sizeof k_tips_ps5 / sizeof *k_tips_ps5},
};

// ---- the tips' marks (PaMessageTipsDrcImageNN_00.bflyt): the blue glow, the dot, the shoulder's window and the
// stick's arrows of each button the tip shows, moved onto the device's close-ups (k_tips_crop below). Each group's
// marks keep their places around the button, scaled by its size to the Switch's; the shoulders' windows take the
// trigger's and the bumper's size. Worked out against the close-ups (the Switch's marks over its own close-ups first).
static const PaneEdit k_marks_deck[] = {
    {"P_ZL_00", {-10, 18}, {-15.07f, -21.37f}, {113.1f, 76.9f}, 0, {0.8f, 0.8f}},
    {"P_Color_11", {-9, 17}, {-14.07f, -22.37f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_L_00", {-13, -3}, {3.25f, 20.41f}, {149.29f, 36.19f}, 0, {0.8f, 0.8f}},
    {"P_Color_13", {-13, -3}, {3.25f, 20.41f}, {96, 96}, 0, {1.21f, 1.21f}},
    {"P_ZR_00", {10, 18}, {15.07f, -21.37f}, {-113.1f, 76.9f}, 0, {0.8f, 0.8f}},
    {"P_Color_12", {9, 17}, {14.07f, -22.37f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_R_00", {13, -3}, {-3.25f, 20.41f}, {-149.29f, 36.19f}, 0, {0.8f, 0.8f}},
    {"P_Color_23", {13, -3}, {-3.25f, 20.41f}, {96, 96}, 0, {1.21f, 1.21f}},
    {"P_Color_01", {13.5f, 9.5f}, {39.35f, -11.23f}, {64, 64}, 0, {0.86f, 0.86f}},
    {"P_Point_00", {13.5f, 9.5f}, {39.35f, -11.23f}, {16, 16}, 0, {0.64f, 0.64f}},
    {"P_Color_15", {-0.5f, -4}, {24.18f, -26.79f}, {64, 64}, 0, {0.86f, 0.86f}},
    {"P_Point_01", {-0.5f, -4}, {24.18f, -26.79f}, {16, 16}, 0, {0.64f, 0.64f}},
    {"P_Color_25", {-0.5f, 23}, {24.18f, 3.94f}, {64, 64}, 0, {0.86f, 0.86f}},
    {"P_Point_02", {-0.5f, 23}, {24.18f, 3.94f}, {16, 16}, 0, {0.64f, 0.64f}},
    {"P_Color_26", {-15, 9.5f}, {9.02f, -11.23f}, {64, 64}, 0, {0.86f, 0.86f}},
    {"P_Point_03", {-15, 9.5f}, {9.02f, -11.23f}, {16, 16}, 0, {0.64f, 0.64f}},
    {"P_Color_00", {-19, 37.2f}, {-4.95f, 10.32f}, {64, 64}, 0, {0.59f, 0.59f}},
    {"Nt_ControllerNNPlus_00", {-19, 37.2f}, {-4.95f, 10.32f}, {19, 19}, 0, {0.49f, 0.49f}},
    {"P_Color_02", {18.5f, 37.2f}, {4.86f, 10.14f}, {64, 64}, 0, {0.58f, 0.58f}},
    {"Nt_ControllerNNMinus_00", {18.5f, 37.2f}, {4.86f, 10.14f}, {19, 19}, 0, {0.48f, 0.48f}},
    {"P_Point_16", {0.5f, -27.5f}, {-24.53f, 0.34f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_17", {0.5f, -54.5f}, {-24.53f, -23.18f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_18", {-13.5f, -41}, {-36.73f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_19", {14.5f, -41}, {-12.34f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_08", {0.5f, -27.5f}, {-24.53f, 0.34f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Color_03", {0.5f, -27.5f}, {-24.53f, 0.34f}, {64, 64}, 0, {0.7f, 0.7f}},
    {"P_Color_20", {0.5f, -54.5f}, {-24.53f, -23.18f}, {64, 64}, 0, {0.7f, 0.7f}},
    {"P_Point_09", {0.5f, -54.5f}, {-24.53f, -23.18f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Color_21", {-13.5f, -41}, {-36.73f, -11.42f}, {64, 64}, 0, {0.7f, 0.7f}},
    {"P_Point_10", {-13.5f, -41}, {-36.73f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Color_27", {14.5f, -41}, {-12.34f, -11.42f}, {64, 64}, 0, {0.7f, 0.7f}},
    {"P_Point_11", {14.5f, -41}, {-12.34f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_12", {0.5f, -27.5f}, {-24.53f, 0.34f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_13", {0.5f, -54.5f}, {-24.53f, -23.18f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_14", {-13.5f, -41}, {-36.73f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"P_Point_15", {14.5f, -41}, {-12.34f, -11.42f}, {16, 16}, 0, {0.52f, 0.52f}},
    {"N_Center_02", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, 90, {0.97f, 0.97f}},
    {"P_Color_07", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"N_Center_03", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, -90, {0.97f, 0.97f}},
    {"P_Color_04", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"N_Center_04", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, 180, {0.97f, 0.97f}},
    {"P_Color_06", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"N_Center_05", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, 0, {0.97f, 0.97f}},
    {"P_Color_05", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"N_Center_06", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, 90, {0.97f, 0.97f}},
    {"P_Color_24", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"N_Center_01", {0, 9.5f}, {25.25f, -19.26f}, {30, 40}, 0, {0.97f, 0.97f}},
    {"P_Color_22", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"P_Point_06", {0, 9.5f}, {25.25f, -19.26f}, {24, 24}, 0, {0.58f, 0.58f}},
    {"N_ArrowPos_01", {0, 24}, {25.25f, -5.26f}, {79, 63}, 0, {0.97f, 0.97f}},
    {"N_Center_11", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 90, {0.98f, 0.98f}},
    {"P_Color_09", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"P_Color_19", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"N_Center_10", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 90, {0.98f, 0.98f}},
    {"N_Center_09", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 0, {0.98f, 0.98f}},
    {"P_Color_17", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"N_Center_08", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 0, {0.98f, 0.98f}},
    {"P_Color_14", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"N_Center_07", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 90, {0.98f, 0.98f}},
    {"P_Color_08", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"N_Center_00", {-1, -41}, {-25.7f, -19.61f}, {30, 40}, 0, {0.98f, 0.98f}},
    {"P_Color_18", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
    {"P_Point_07", {-1, -41}, {-25.7f, -19.61f}, {24, 24}, 0, {0.59f, 0.59f}},
    {"N_ArrowPos_00", {-1, -26.5f}, {-25.7f, -5.36f}, {79, 63}, 0, {0.98f, 0.98f}},
    {"P_Color_16", {0, 9.5f}, {25.25f, -19.26f}, {84, 84}, 0, {0.77f, 0.77f}},
    {"P_Color_10", {-1, -41}, {-25.7f, -19.61f}, {84, 84}, 0, {0.79f, 0.79f}},
};
static const PaneEdit k_marks_xbox[] = {
    {"P_ZL_00", {-10, 18}, {-18.2f, -20.73f}, {67.86f, 58.81f}, 0, {0.8f, 0.8f}},
    {"P_Color_11", {-9, 17}, {-17.2f, -21.73f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_L_00", {-13, -3}, {-12.06f, 34.55f}, {97.26f, 27.14f}, 0, {0.8f, 0.8f}},
    {"P_Color_13", {-13, -3}, {-12.06f, 34.55f}, {96, 96}, 0, {0.9f, 0.9f}},
    {"P_ZR_00", {10, 18}, {18.2f, -20.73f}, {-67.86f, 58.81f}, 0, {0.8f, 0.8f}},
    {"P_Color_12", {9, 17}, {17.2f, -21.73f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_R_00", {13, -3}, {12.06f, 34.55f}, {-97.26f, 27.14f}, 0, {0.8f, 0.8f}},
    {"P_Color_23", {13, -3}, {12.06f, 34.55f}, {96, 96}, 0, {0.9f, 0.9f}},
    {"P_Color_01", {13.5f, 9.5f}, {33.11f, 3.1f}, {64, 64}, 0, {0.97f, 0.97f}},
    {"P_Point_00", {13.5f, 9.5f}, {33.11f, 3.1f}, {16, 16}, 0, {0.72f, 0.72f}},
    {"P_Color_15", {-0.5f, -4}, {15.42f, -13.86f}, {64, 64}, 0, {0.97f, 0.97f}},
    {"P_Point_01", {-0.5f, -4}, {15.42f, -13.86f}, {16, 16}, 0, {0.72f, 0.72f}},
    {"P_Color_25", {-0.5f, 23}, {15.37f, 20.67f}, {64, 64}, 0, {0.97f, 0.97f}},
    {"P_Point_02", {-0.5f, 23}, {15.37f, 20.67f}, {16, 16}, 0, {0.72f, 0.72f}},
    {"P_Color_26", {-15, 9.5f}, {-1.48f, 3.28f}, {64, 64}, 0, {0.97f, 0.97f}},
    {"P_Point_03", {-15, 9.5f}, {-1.48f, 3.28f}, {16, 16}, 0, {0.72f, 0.72f}},
    {"P_Color_00", {-19, 37.2f}, {-31.5f, 3.77f}, {64, 64}, 0, {0.52f, 0.52f}},
    {"Nt_ControllerNNPlus_00", {-19, 37.2f}, {-31.5f, 3.77f}, {19, 19}, 0, {0.43f, 0.43f}},
    {"P_Color_02", {18.5f, 37.2f}, {30.38f, 2.93f}, {64, 64}, 0, {0.49f, 0.49f}},
    {"Nt_ControllerNNMinus_00", {18.5f, 37.2f}, {30.38f, 2.93f}, {19, 19}, 0, {0.41f, 0.41f}},
    {"P_Point_16", {0.5f, -27.5f}, {15.96f, -25.05f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_17", {0.5f, -54.5f}, {15.96f, -45.97f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_18", {-13.5f, -41}, {5.11f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_19", {14.5f, -41}, {26.8f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_08", {0.5f, -27.5f}, {15.96f, -25.05f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Color_03", {0.5f, -27.5f}, {15.96f, -25.05f}, {64, 64}, 0, {0.62f, 0.62f}},
    {"P_Color_20", {0.5f, -54.5f}, {15.96f, -45.97f}, {64, 64}, 0, {0.62f, 0.62f}},
    {"P_Point_09", {0.5f, -54.5f}, {15.96f, -45.97f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Color_21", {-13.5f, -41}, {5.11f, -35.51f}, {64, 64}, 0, {0.62f, 0.62f}},
    {"P_Point_10", {-13.5f, -41}, {5.11f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Color_27", {14.5f, -41}, {26.8f, -35.51f}, {64, 64}, 0, {0.62f, 0.62f}},
    {"P_Point_11", {14.5f, -41}, {26.8f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_12", {0.5f, -27.5f}, {15.96f, -25.05f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_13", {0.5f, -54.5f}, {15.96f, -45.97f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_14", {-13.5f, -41}, {5.11f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"P_Point_15", {14.5f, -41}, {26.8f, -35.51f}, {16, 16}, 0, {0.46f, 0.46f}},
    {"N_Center_02", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, 90, {1.03f, 1.03f}},
    {"P_Color_07", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"N_Center_03", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, -90, {1.03f, 1.03f}},
    {"P_Color_04", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"N_Center_04", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, 180, {1.03f, 1.03f}},
    {"P_Color_06", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"N_Center_05", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, 0, {1.03f, 1.03f}},
    {"P_Color_05", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"N_Center_06", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, 90, {1.03f, 1.03f}},
    {"P_Color_24", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"N_Center_01", {0, 9.5f}, {-15.59f, 4.03f}, {30, 40}, 0, {1.03f, 1.03f}},
    {"P_Color_22", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"P_Point_06", {0, 9.5f}, {-15.59f, 4.03f}, {24, 24}, 0, {0.62f, 0.62f}},
    {"N_ArrowPos_01", {0, 24}, {-15.59f, 18.9f}, {79, 63}, 0, {1.03f, 1.03f}},
    {"N_Center_11", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 90, {1.08f, 1.08f}},
    {"P_Color_09", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"P_Color_19", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"N_Center_10", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 90, {1.08f, 1.08f}},
    {"N_Center_09", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 0, {1.08f, 1.08f}},
    {"P_Color_17", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"N_Center_08", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 0, {1.08f, 1.08f}},
    {"P_Color_14", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"N_Center_07", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 90, {1.08f, 1.08f}},
    {"P_Color_08", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"N_Center_00", {-1, -41}, {-17.46f, -34.69f}, {30, 40}, 0, {1.08f, 1.08f}},
    {"P_Color_18", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
    {"P_Point_07", {-1, -41}, {-17.46f, -34.69f}, {24, 24}, 0, {0.65f, 0.65f}},
    {"N_ArrowPos_00", {-1, -26.5f}, {-17.46f, -19.06f}, {79, 63}, 0, {1.08f, 1.08f}},
    {"P_Color_16", {0, 9.5f}, {-15.59f, 4.03f}, {84, 84}, 0, {0.82f, 0.82f}},
    {"P_Color_10", {-1, -41}, {-17.46f, -34.69f}, {84, 84}, 0, {0.86f, 0.86f}},
};
static const PaneEdit k_marks_ps5[] = {
    {"P_ZL_00", {-10, 18}, {-13.82f, -34.02f}, {70.12f, 72.38f}, 0, {0.8f, 0.8f}},
    {"P_Color_11", {-9, 17}, {-12.82f, -35.02f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_L_00", {-13, -3}, {-13.82f, 13.63f}, {65.6f, 40.71f}, 0, {0.8f, 0.8f}},
    {"P_Color_13", {-13, -3}, {-13.82f, 13.63f}, {96, 96}, 0, {1.36f, 1.36f}},
    {"P_ZR_00", {10, 18}, {14.82f, -34.02f}, {-70.12f, 72.38f}, 0, {0.8f, 0.8f}},
    {"P_Color_12", {9, 17}, {13.82f, -35.02f}, {96, 96}, 0, {1.44f, 1.44f}},
    {"P_R_00", {13, -3}, {14.82f, 13.63f}, {-65.6f, 40.71f}, 0, {0.8f, 0.8f}},
    {"P_Color_23", {13, -3}, {14.82f, 13.63f}, {96, 96}, 0, {1.36f, 1.36f}},
    {"P_Color_01", {13.5f, 9.5f}, {35.34f, 0.93f}, {64, 64}, 0, {1, 1}},
    {"P_Point_00", {13.5f, 9.5f}, {35.34f, 0.93f}, {16, 16}, 0, {0.75f, 0.75f}},
    {"P_Color_15", {-0.5f, -4}, {17.58f, -16.8f}, {64, 64}, 0, {1, 1}},
    {"P_Point_01", {-0.5f, -4}, {17.58f, -16.8f}, {16, 16}, 0, {0.75f, 0.75f}},
    {"P_Color_25", {-0.5f, 23}, {17.58f, 18.85f}, {64, 64}, 0, {1, 1}},
    {"P_Point_02", {-0.5f, 23}, {17.58f, 18.85f}, {16, 16}, 0, {0.75f, 0.75f}},
    {"P_Color_26", {-15, 9.5f}, {-0.01f, 0.89f}, {64, 64}, 0, {1, 1}},
    {"P_Point_03", {-15, 9.5f}, {-0.01f, 0.89f}, {16, 16}, 0, {0.75f, 0.75f}},
    {"P_Color_00", {-19, 37.2f}, {-1.65f, 27.72f}, {64, 64}, 0, {0.47f, 0.47f}},
    {"Nt_ControllerNNPlus_00", {-19, 37.2f}, {-1.65f, 27.72f}, {19, 19}, 0, {0.39f, 0.39f}},
    {"P_Color_02", {18.5f, 37.2f}, {1.24f, 28.89f}, {64, 64}, 0, {0.48f, 0.48f}},
    {"Nt_ControllerNNMinus_00", {18.5f, 37.2f}, {1.24f, 28.89f}, {19, 19}, 0, {0.4f, 0.4f}},
    {"P_Point_16", {0.5f, -27.5f}, {-18.12f, 16.53f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_17", {0.5f, -54.5f}, {-18.12f, -13.39f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_18", {-13.5f, -41}, {-33.63f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_19", {14.5f, -41}, {-2.6f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_08", {0.5f, -27.5f}, {-18.12f, 16.53f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Color_03", {0.5f, -27.5f}, {-18.12f, 16.53f}, {64, 64}, 0, {0.89f, 0.89f}},
    {"P_Color_20", {0.5f, -54.5f}, {-18.12f, -13.39f}, {64, 64}, 0, {0.89f, 0.89f}},
    {"P_Point_09", {0.5f, -54.5f}, {-18.12f, -13.39f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Color_21", {-13.5f, -41}, {-33.63f, 1.57f}, {64, 64}, 0, {0.89f, 0.89f}},
    {"P_Point_10", {-13.5f, -41}, {-33.63f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Color_27", {14.5f, -41}, {-2.6f, 1.57f}, {64, 64}, 0, {0.89f, 0.89f}},
    {"P_Point_11", {14.5f, -41}, {-2.6f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_12", {0.5f, -27.5f}, {-18.12f, 16.53f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_13", {0.5f, -54.5f}, {-18.12f, -13.39f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_14", {-13.5f, -41}, {-33.63f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"P_Point_15", {14.5f, -41}, {-2.6f, 1.57f}, {16, 16}, 0, {0.66f, 0.66f}},
    {"N_Center_02", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, 90, {1.16f, 1.16f}},
    {"P_Color_07", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"N_Center_03", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, -90, {1.16f, 1.16f}},
    {"P_Color_04", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"N_Center_04", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, 180, {1.16f, 1.16f}},
    {"P_Color_06", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"N_Center_05", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, 0, {1.16f, 1.16f}},
    {"P_Color_05", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"N_Center_06", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, 90, {1.16f, 1.16f}},
    {"P_Color_24", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"N_Center_01", {0, 9.5f}, {19.01f, -32.55f}, {30, 40}, 0, {1.16f, 1.16f}},
    {"P_Color_22", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"P_Point_06", {0, 9.5f}, {19.01f, -32.55f}, {24, 24}, 0, {0.69f, 0.69f}},
    {"N_ArrowPos_01", {0, 24}, {19.01f, -15.77f}, {79, 63}, 0, {1.16f, 1.16f}},
    {"N_Center_11", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 90, {1.13f, 1.13f}},
    {"P_Color_09", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"P_Color_19", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"N_Center_10", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 90, {1.13f, 1.13f}},
    {"N_Center_09", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 0, {1.13f, 1.13f}},
    {"P_Color_17", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"N_Center_08", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 0, {1.13f, 1.13f}},
    {"P_Color_14", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"N_Center_07", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 90, {1.13f, 1.13f}},
    {"P_Color_08", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"N_Center_00", {-1, -41}, {-19.9f, -32.84f}, {30, 40}, 0, {1.13f, 1.13f}},
    {"P_Color_18", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
    {"P_Point_07", {-1, -41}, {-19.9f, -32.84f}, {24, 24}, 0, {0.68f, 0.68f}},
    {"N_ArrowPos_00", {-1, -26.5f}, {-19.9f, -16.38f}, {79, 63}, 0, {1.13f, 1.13f}},
    {"P_Color_16", {0, 9.5f}, {19.01f, -32.55f}, {84, 84}, 0, {0.93f, 0.93f}},
    {"P_Color_10", {-1, -41}, {-19.9f, -32.84f}, {84, 84}, 0, {0.91f, 0.91f}},
};
static const struct { const PaneEdit *e; size_t n; } k_marks[HW_DEVICE_COUNT] = {
    {k_marks_deck, sizeof k_marks_deck / sizeof *k_marks_deck},
    {k_marks_xbox, sizeof k_marks_xbox / sizeof *k_marks_xbox},
    {k_marks_ps5, sizeof k_marks_ps5 / sizeof *k_marks_ps5},
};

static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static void wrf(uint8_t *p, float f) { memcpy(p, &f, 4); }

// The panes of a BFLYT (little endian), calls f with each pane section; false: not a layout
static bool each_pane(uint8_t *d, size_t n, bool (*f)(uint8_t *pane, void *ctx), void *ctx) {
    if (n < 0x14 || memcmp(d, "FLYT", 4) || d[4] != 0xFF || d[5] != 0xFE) return false;
    uint16_t hs = (uint16_t)(d[6] | d[7] << 8), ns = (uint16_t)(d[0x10] | d[0x11] << 8);
    size_t o = hs;
    for (uint16_t i = 0; i < ns && o + 8 <= n; i++) {
        uint32_t sz;
        memcpy(&sz, d + o + 4, 4);
        if (sz < 8 || o + sz > n) return false;
        if (sz >= 0x54 && (!memcmp(d + o, "pan1", 4) || !memcmp(d + o, "pic1", 4) || !memcmp(d + o, "txt1", 4) ||
                           !memcmp(d + o, "wnd1", 4) || !memcmp(d + o, "prt1", 4) || !memcmp(d + o, "bnd1", 4)))
            if (!f(d + o, ctx)) return false;
        o += sz;
    }
    return true;
}

typedef struct { const PaneEdit *e; size_t n; int found; } EditCtx;
static const PaneEdit *edit_for(const uint8_t *pane, const EditCtx *c) {
    char name[25];
    memcpy(name, pane + 0x0C, 24);
    name[24] = 0;
    for (size_t i = 0; i < c->n; i++)
        if (!strcmp(name, c->e[i].name)) return &c->e[i];
    return NULL;
}
static bool check_pane(uint8_t *pane, void *ctx) {
    EditCtx *c = ctx;
    const PaneEdit *e = edit_for(pane, c);
    if (!e) return true;
    c->found++;
    return fabsf(rdf(pane + 0x2C) - e->was[0]) < 0.01f && fabsf(rdf(pane + 0x30) - e->was[1]) < 0.01f;
}
static bool edit_pane(uint8_t *pane, void *ctx) {
    const PaneEdit *e = edit_for(pane, ctx);
    if (!e) return true;
    wrf(pane + 0x2C, e->t[0]), wrf(pane + 0x30, e->t[1]);   // translation x, y
    wrf(pane + 0x40, e->rot);                               // rotation z
    wrf(pane + 0x44, e->scale[0]), wrf(pane + 0x48, e->scale[1]);
    wrf(pane + 0x4C, e->size[0]), wrf(pane + 0x50, e->size[1]);
    return true;
}

int hw_illust_layout(int device, int layout, uint8_t *flyt, size_t n) {
    if (device < 0 || device >= HW_DEVICE_COUNT) return 0;
    EditCtx c = {layout == HW_LAYOUT_TIPS ? k_marks[device].e : k_tips[device].e,
                 layout == HW_LAYOUT_TIPS ? k_marks[device].n : k_tips[device].n, 0};
    if (!each_pane(flyt, n, check_pane, &c) || c.found != (int)c.n) return 0;
    each_pane(flyt, n, edit_pane, &c);
    return c.found;
}

// ---- the tips' close-ups (PaMessageTipsDrcImageNN_00): the same part of the device's own drawing, enlarged. Each: the
// drawing (0 front, 1 top), the square's centre and side there (texels): ZL / L, ZR / R, the face buttons and the right
// stick, the left stick and the d-pad, as the Switch's close-ups frame them (the controllers': the two buttons as far
// apart as on the Switch's, so its marks fit them).
typedef struct { int view; float cx, cy, side; } TipCrop;
static const TipCrop k_tips_crop[HW_DEVICE_COUNT][4] = {
    {{1, 78, 55, 84}, {1, 498, 55, 84}, {0, 505, 56, 165}, {0, 71, 56, 168}},        // Steam Deck (its illustration)
    {{1, 220, 56, 84}, {1, 356, 56, 84}, {0, 367, 89, 235}, {0, 210, 88, 247}},      // Xbox (its illustration)
    {{1, 215, 60, 84}, {1, 361, 60, 84}, {0, 388, 85, 259}, {0, 188, 86, 254}},      // DualSense (its illustration)
};

static void (*const k_draw[HW_DEVICE_COUNT][2])(Paint *) = {
    {deck_front, deck_top}, {xbox_front, xbox_top}, {ps5_front, ps5_top},
};

static void tip(int device, int which, Paint *p) {
    static uint8_t src[576 * 288 * 2];  // (the texture upload's thread only)
    const TipCrop *c = &k_tips_crop[device][which - HW_ILLUST_TIP_ZL];
    Paint s = {src, 576, c->view ? 100 : 288, 576 * 2};
    memset(src, 0, sizeof src);
    k_draw[device][c->view](&s);
    float k = c->side / (float)p->w;
    for (uint32_t y = 0; y < p->h; y++)
        for (uint32_t x = 0; x < p->w; x++) {  // (bilinear, the grey weighted by alpha)
            float sx = c->cx + (x + 0.5f - p->w / 2.0f) * k - 0.5f, sy = c->cy + (y + 0.5f - p->h / 2.0f) * k - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0, a = 0, l = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int X = x0 + i, Y = y0 + j;
                    if (X < 0 || Y < 0 || X >= (int)s.w || Y >= (int)s.h) continue;
                    float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    const uint8_t *q = src + (uint64_t)Y * s.row + X * 2;
                    a += q[1] * wgt, l += q[0] * q[1] * wgt;
                }
            uint8_t *d = p->lin + (uint64_t)y * p->row + x * 2;
            d[0] = a > 0 ? (uint8_t)lroundf(l / a) : 0;
            d[1] = (uint8_t)lroundf(a);
        }
}

void hw_illust_draw(int device, int which, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row) {
    if (device < 0 || device >= HW_DEVICE_COUNT || which < 0 || which >= HW_ILLUST_COUNT) return;
    Paint p = {lin, w, h, row};
    for (uint32_t y = 0; y < h; y++) memset(lin + (uint64_t)y * row, 0, w * 2);
    if (which <= HW_ILLUST_TOP) k_draw[device][which](&p);
    else tip(device, which, &p);
}
