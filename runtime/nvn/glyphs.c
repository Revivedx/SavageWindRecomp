#include "../pc_cursor.h"
// Button icons for the device in use.
//
// BOTW draws its button prompts (HUD hints, menus, dialogs) as characters of its own bitmap font External_00.bffnt
// (Font/Font_*.sbfarc): 51 glyphs at U+E040..E091 in 50x50 cells of one BC4 sheet (A B X Y at E040..E043, L R ZL ZR at
// E044..E047, the d-pad E049 and E060..E065, Home E04A, Plus / Minus E04B / E04C, the sticks' directions and presses
// E050..E05B, the sticks E05D..E05F, then the console, arrows and marks). The prompts' text tags choose those characters at run time, so the sheet is
// what changes, not the texts. nn::font copies the sheet into a texture memory pool of its own; the texture is
// recognised by its size and contents, and on upload its texels are replaced by glyphs drawn here for the device in use,
// in the same cells and inside each original glyph's box (the game's widths stay): keycaps and mouse buttons from the
// keyboard bindings, Xbox, PlayStation or Steam Deck buttons through the controller bindings, or the game's own glyphs (moved
// between cells when the controller bindings differ from the defaults). Nothing of Nintendo's is shipped: shapes are
// drawn, labels set in Fira Sans (OFL, third_party/fonts).
//
// The texture goes up again whenever what to show changes (glyphs_generation: device, bindings, setting), so prompts
// already on screen change on the next frame.
//
// With Gameplay > Sharper text the sheet is drawn 2 or 4 times larger (glyphs_draw_sheet, into a larger image with mip
// levels: textures.c): the game stretches its 50-texel cells to about 70 pixels at 1440p for dialogue, more in menus.
//
// SAVAGEWIND_GLYPH_DUMP=1 saves each sheet as uploaded, the original beside it (build/glyph_sheet_<n>.pgm, bottom-up).
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "glyphs.h"
#include "hw_illust.h"
#include "../hle.h"
#include "../framepace.h"
#include "../game_sites.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../rt.h"
#include "../settings.h"
#include "text_upscale.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"

#define FMT_BC4 0x49
#define FMT_R8 0x01
#define CELL 50

// The game's sheet: 256x1024 BC4, 5 x 20 cells of 50x50 (51 apart). nn::font copies it into a texture memory pool of
// its own, so it is recognised by its contents (the same in every region and language).
typedef struct { uint32_t cols, rows, sheet_w, sheet_h; } SheetFont;
static const SheetFont k_sheet = {5, 20, 256, 1024};
#define SHEET_BYTES 0x20000ull
#define SHEET_HASH 0x726127a3b63fbd95ull  // FNV-1a of those bytes (External_00.bffnt from 0x2000, 1.8.2)

// Glyph index of U+E040..E091 (External_00.bffnt's character map), 0xffff: none
static uint16_t index_of(int code) {
    if (code >= 0xE040 && code <= 0xE04C) return (uint16_t)(code - 0xE040);
    if (code >= 0xE050 && code <= 0xE05B) return (uint16_t)(13 + code - 0xE050);
    if (code >= 0xE05D && code <= 0xE067) return (uint16_t)(25 + code - 0xE05D);
    if (code >= 0xE083 && code <= 0xE091) return (uint16_t)(36 + code - 0xE083);
    return 0xffff;
}

static uint64_t fnv1a(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- BC4
static void bc4_decode(const uint8_t *lin, uint32_t wb, uint32_t hb, uint8_t *img) {
    uint32_t w = wb * 4;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *b = lin + ((uint64_t)by * wb + bx) * 8;
            uint8_t r0 = b[0], r1 = b[1], pal[8] = {r0, r1};
            if (r0 > r1) for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * r0 + i * r1) / 7);
            else { for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * r0 + i * r1) / 5); pal[6] = 0, pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) img[(uint64_t)(by * 4 + i / 4) * w + bx * 4 + i % 4] = pal[(bits >> (3 * i)) & 7];
        }
}

static void bc4_encode(const uint8_t *img, uint32_t wb, uint32_t hb, uint8_t *lin) {
    uint32_t w = wb * 4;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t v[16], lo = 255, hi = 0;
            for (int i = 0; i < 16; i++) {
                v[i] = img[(uint64_t)(by * 4 + i / 4) * w + bx * 4 + i % 4];
                if (v[i] < lo) lo = v[i];
                if (v[i] > hi) hi = v[i];
            }
            uint8_t *b = lin + ((uint64_t)by * wb + bx) * 8;
            uint64_t bits = 0;
            if (hi > lo) {  // 8-value mode: r0 = hi > r1 = lo; t = 0 (hi) .. 7 (lo) -> index 0, 2..7, 1
                for (int i = 0; i < 16; i++) {
                    int t = ((hi - v[i]) * 7 + (hi - lo) / 2) / (hi - lo);
                    bits |= (uint64_t)(t == 0 ? 0 : t == 7 ? 1 : t + 1) << (3 * i);
                }
            }
            b[0] = hi, b[1] = lo;
            for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
        }
}

// ---- the cell canvas: coverage 0..1, y down. Shapes are placed in the cell's own units (50 x 50, the game's texels);
// the canvas has k pixels per unit: 1 for the game's sheet, more when Sharper text draws it larger (glyphs_draw_sheet)
typedef struct { int k, n; float *a; } Canvas;  // n = CELL * k pixels a side
typedef struct { float cx, cy, s, w; } Box;  // the original glyph's centre, height and width

static bool canvas_init(Canvas *c, int k) {
    c->k = k, c->n = CELL * k;
    c->a = calloc((size_t)c->n * c->n, sizeof *c->a);
    return c->a != NULL;
}
static void canvas_free(Canvas *c) { free(c->a), c->a = NULL; }
static float unit(const Canvas *c, int p) { return (p + 0.5f) / (float)c->k; }  // (a pixel's centre, in units)

typedef enum { SH_CIRCLE, SH_BOX, SH_SEG, SH_TRI } ShapeType;
typedef struct { ShapeType t; float p[6], r, ring; } Shape;  // ring > 0: only a band that thick on its edge

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float sd_seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, h = clampf(((px - ax) * dx + (py - ay) * dy) / maxf(dx * dx + dy * dy, 1e-6f), 0, 1);
    return hypotf(px - ax - dx * h, py - ay - dy * h);
}

static float sd(const Shape *s, float x, float y) {
    float d = 0;
    switch (s->t) {
    case SH_CIRCLE: d = hypotf(x - s->p[0], y - s->p[1]) - s->r; break;
    case SH_BOX: {  // centre, half sizes, corner radius r
        float qx = fabsf(x - s->p[0]) - s->p[2] + s->r, qy = fabsf(y - s->p[1]) - s->p[3] + s->r;
        d = hypotf(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - s->r;
        break;
    }
    case SH_SEG: d = sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]) - s->r; break;
    case SH_TRI: {  // three corners (any order)
        float e = minf(minf(sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]), sd_seg(x, y, s->p[2], s->p[3], s->p[4], s->p[5])),
                       sd_seg(x, y, s->p[4], s->p[5], s->p[0], s->p[1]));
        float c1 = (s->p[2] - s->p[0]) * (y - s->p[1]) - (s->p[3] - s->p[1]) * (x - s->p[0]);
        float c2 = (s->p[4] - s->p[2]) * (y - s->p[3]) - (s->p[5] - s->p[3]) * (x - s->p[2]);
        float c3 = (s->p[0] - s->p[4]) * (y - s->p[5]) - (s->p[1] - s->p[5]) * (x - s->p[4]);
        bool in = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
        d = (in ? -e : e) - s->r;
        break;
    }
    }
    return s->ring > 0 ? fabsf(d) - s->ring : d;
}

// The coverage of a pixel whose centre is d units outside a shape: a ramp one pixel wide across the edge
static float cover(const Canvas *c, float d) { return clampf(0.5f - d * c->k, 0, 1); }

// The pixels a shape can touch (its bounds and a unit more): fill and cut leave the others as they are, so at 4x
// a glyph costs about what the whole cell did at 1x
static void bounds(const Canvas *c, const Shape *s, int *x0, int *y0, int *x1, int *y1) {
    float lx, ly, hx, hy, m = s->r + (s->ring > 0 ? s->ring : 0) + 1;
    switch (s->t) {
    case SH_CIRCLE: lx = hx = s->p[0], ly = hy = s->p[1]; break;
    case SH_BOX: lx = s->p[0] - s->p[2], hx = s->p[0] + s->p[2], ly = s->p[1] - s->p[3], hy = s->p[1] + s->p[3], m -= s->r; break;
    case SH_SEG: lx = minf(s->p[0], s->p[2]), hx = maxf(s->p[0], s->p[2]), ly = minf(s->p[1], s->p[3]), hy = maxf(s->p[1], s->p[3]); break;
    default:
        lx = minf(minf(s->p[0], s->p[2]), s->p[4]), hx = maxf(maxf(s->p[0], s->p[2]), s->p[4]);
        ly = minf(minf(s->p[1], s->p[3]), s->p[5]), hy = maxf(maxf(s->p[1], s->p[3]), s->p[5]);
        break;
    }
    *x0 = (int)clampf(floorf((lx - m) * c->k), 0, (float)c->n), *x1 = (int)clampf(ceilf((hx + m) * c->k), 0, (float)c->n);
    *y0 = (int)clampf(floorf((ly - m) * c->k), 0, (float)c->n), *y1 = (int)clampf(ceilf((hy + m) * c->k), 0, (float)c->n);
}
static void fill(Canvas *c, Shape s) {
    int x0, y0, x1, y1;
    bounds(c, &s, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) c->a[y * c->n + x] = maxf(c->a[y * c->n + x], cover(c, sd(&s, unit(c, x), unit(c, y))));
}
static void cut(Canvas *c, Shape s) {
    int x0, y0, x1, y1;
    bounds(c, &s, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) c->a[y * c->n + x] *= 1 - cover(c, sd(&s, unit(c, x), unit(c, y)));
}
static Shape circle(float x, float y, float r) { return (Shape){SH_CIRCLE, {x, y}, r, 0}; }
static Shape box(float x, float y, float hw, float hh, float r) { return (Shape){SH_BOX, {x, y, hw, hh}, r, 0}; }
static Shape seg(float ax, float ay, float bx, float by, float r) { return (Shape){SH_SEG, {ax, ay, bx, by}, r, 0}; }
static Shape tri(float ax, float ay, float bx, float by, float cx, float cy) { return (Shape){SH_TRI, {ax, ay, bx, by, cx, cy}, 0, 0}; }
static Shape ring(Shape s, float t) { s.ring = t; return s; }

// ---- text (Fira Sans SemiBold)
static stbtt_fontinfo g_font;
static int g_font_state;  // 0 not tried, 1 loaded, -1 missing

static bool font_ready(void) {
    if (g_font_state) return g_font_state > 0;
    g_font_state = -1;
    char path[1100];
    if (!rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", path, sizeof path)) {
        LOG("[glyphs] Fira Sans not found: button labels left out\n");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n || !stbtt_InitFont(&g_font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
        fclose(f);
        free(d);
        return false;
    }
    fclose(f);
    g_font_state = 1;  // (d stays: stbtt reads it)
    return true;
}

// The text's capitals cap_h tall (less if it is wider than max_w), centred on (cx, cy): coverage into t (sizes in
// units, set at t's own resolution: its letters are drawn from the outlines, not enlarged)
static void text(Canvas *t, const char *s, float cx, float cy, float cap_h, float max_w) {
    memset(t->a, 0, (size_t)t->n * t->n * sizeof *t->a);
    if (!font_ready() || !*s) return;
    cx *= t->k, cy *= t->k, cap_h *= t->k, max_w *= t->k;  // (pixels from here on)
    int cap_y0, cap_y1, x0, x1;  // the height of "H" gives the scale for capitals
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &cap_y0, &x1, &cap_y1);
    float scale = cap_h / (float)(cap_y1 > 0 ? cap_y1 : 700);
    float width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale;
        if (p[1]) width += stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale;
    }
    if (width > max_w) scale *= max_w / width, width = max_w;
    float pen = cx - width / 2, base = cy + cap_y1 * scale / 2;
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p, bx0, by0, bx1, by1, adv, lsb;
        float sx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, sx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        unsigned char *bm = bw > 0 && bh > 0 && bw <= 4 * t->n && bh <= 4 * t->n ? malloc((size_t)bw * bh) : NULL;
        if (bm) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, sx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = (int)lroundf(base) + by0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < t->n && Y >= 0 && Y < t->n)
                        t->a[Y * t->n + X] = maxf(t->a[Y * t->n + X], bm[y * bw + x] / 255.0f);
                }
            free(bm);
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
}
static void erase(Canvas *c, const Canvas *t) { for (int i = 0; i < c->n * c->n; i++) c->a[i] *= 1 - t->a[i]; }

// ---- icons
enum { DIR_U = 1, DIR_D = 2, DIR_L = 4, DIR_R = 8 };

static void cut_text(Canvas *c, const char *s, float cx, float cy, float cap, float max_w) {
    Canvas t;
    if (!canvas_init(&t, c->k)) return;
    text(&t, s, cx, cy, cap, max_w);
    erase(c, &t);
    canvas_free(&t);
}

// A filled round button with its letter cut out (Xbox face buttons, stick presses)
static void round_label(Canvas *c, Box b, const char *s) {
    float r = b.s / 2;
    fill(c, circle(b.cx, b.cy, r));
    cut_text(c, s, b.cx, b.cy, b.s * (strlen(s) > 1 ? 0.36f : 0.5f), b.s * 0.72f);
}

// A filled rounded tab with its label cut out (shoulders, triggers, other buttons)
static void tab_label(Canvas *c, Box b, const char *s) {
    float hw = minf(b.w / 2, b.s * 0.56f), hh = b.s * 0.36f;
    fill(c, box(b.cx, b.cy, hw, hh, b.s * 0.14f));
    cut_text(c, s, b.cx, b.cy, b.s * 0.38f, hw * 2 - b.s * 0.18f);
}

// PlayStation face buttons: cross, circle, square, triangle cut out of a filled circle
static void ps_face(Canvas *c, Box b, int host) {
    float r = b.s / 2, t = b.s * 0.06f, k = b.s * 0.2f, x = b.cx, y = b.cy;
    fill(c, circle(x, y, r));
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: cut(c, seg(x - k, y - k, x + k, y + k, t)); cut(c, seg(x - k, y + k, x + k, y - k, t)); break;
    case SDL_GAMEPAD_BUTTON_EAST: cut(c, ring(circle(x, y, k * 1.05f), t)); break;
    case SDL_GAMEPAD_BUTTON_WEST: cut(c, ring(box(x, y, k * 0.95f, k * 0.95f, 0), t)); break;
    default: {
        Shape tr = tri(x, y - k * 1.15f, x - k * 1.1f, y + k * 0.8f, x + k * 1.1f, y + k * 0.8f);
        cut(c, ring(tr, t));
        break;
    }
    }
}

// Xbox Menu / View, PlayStation Options / Create
static void menu_button(Canvas *c, Box b, bool start, bool ps) {
    float x = b.cx, y = b.cy, s = b.s, t = s * 0.045f;
    if (ps) fill(c, box(x, y, minf(b.w / 2, s * 0.5f), s * 0.3f, s * 0.3f));
    else fill(c, circle(x, y, s / 2));
    if (start) {  // three lines
        float w = ps ? s * 0.2f : s * 0.19f;
        for (int i = -1; i <= 1; i++) cut(c, seg(x - w, y + i * s * (ps ? 0.11f : 0.14f), x + w, y + i * s * (ps ? 0.11f : 0.14f), t));
    } else if (ps) {  // a small square
        cut(c, ring(box(x, y, s * 0.1f, s * 0.1f, s * 0.02f), t));
    } else {  // two overlapping windows
        cut(c, ring(box(x - s * 0.05f, y - s * 0.05f, s * 0.12f, s * 0.1f, 0), t));
        cut(c, ring(box(x + s * 0.06f, y + s * 0.06f, s * 0.12f, s * 0.1f, 0), t));
    }
}

// A d-pad: the arms in mask filled, the rest outlined
static void dpad(Canvas *c, Box b, int mask) {
    float x = b.cx, y = b.cy, L = b.s * 0.48f, w = b.s * 0.17f, t = b.s * 0.045f, r = b.s * 0.05f;
    Shape h = box(x, y, L, w, r), v = box(x, y, w, L, r);
    if (mask == (DIR_U | DIR_D | DIR_L | DIR_R)) {
        fill(c, h);
        fill(c, v);
        cut(c, circle(x, y, w * 0.55f));
        return;
    }
    Shape all = ring(box(x, y, L, L, 0), t);
    int x0, y0, x1, y1;
    bounds(c, &all, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++)  // the cross's outline (both bars as one shape)
        for (int px = x0; px < x1; px++) {
            float X = unit(c, px), Y = unit(c, py), d = fabsf(minf(sd(&h, X, Y), sd(&v, X, Y))) - t;
            c->a[py * c->n + px] = maxf(c->a[py * c->n + px], cover(c, d));
        }
    if (mask & DIR_U) fill(c, box(x, y - L / 2 - w / 2, w, L / 2 - w / 2 + r, r));
    if (mask & DIR_D) fill(c, box(x, y + L / 2 + w / 2, w, L / 2 - w / 2 + r, r));
    if (mask & DIR_L) fill(c, box(x - L / 2 - w / 2, y, L / 2 - w / 2 + r, w, r));
    if (mask & DIR_R) fill(c, box(x + L / 2 + w / 2, y, L / 2 - w / 2 + r, w, r));
}

static Shape arrow(float x, float y, float k, int dir) {  // a triangle pointing dir, k: its half size
    switch (dir) {
    case DIR_U: return tri(x, y - k, x - k, y + k * 0.7f, x + k, y + k * 0.7f);
    case DIR_D: return tri(x, y + k, x - k, y - k * 0.7f, x + k, y - k * 0.7f);
    case DIR_L: return tri(x - k, y, x + k * 0.7f, y - k, x + k * 0.7f, y + k);
    default: return tri(x + k, y, x - k * 0.7f, y - k, x - k * 0.7f, y + k);
    }
}

// A keycap with its label (or arrows: dirs) cut out; wide labels get a wider key
static void keycap(Canvas *c, Box b, const char *label, int dirs) {
    float hh = b.s * 0.42f, hw = (label && strlen(label) > 1) ? minf(b.w / 2, b.s * 0.6f) : hh;
    if (!label && dirs && (dirs & (dirs - 1))) hw = minf(b.w / 2, b.s * 0.6f);
    fill(c, box(b.cx, b.cy, hw, hh, b.s * 0.12f));
    if (label) {
        cut_text(c, label, b.cx, b.cy, b.s * (strlen(label) > 1 ? 0.34f : 0.46f), hw * 2 - b.s * 0.16f);
        return;
    }
    int n = 0, d[4];
    for (int i = 0; i < 4; i++) if (dirs & (1 << i)) d[n++] = 1 << i;
    float k = b.s * (n == 1 ? 0.17f : 0.12f);
    if (n == 4) {  // the arrow cluster
        cut(c, arrow(b.cx, b.cy - k * 1.6f, k * 0.8f, DIR_U));
        cut(c, arrow(b.cx, b.cy + k * 1.6f, k * 0.8f, DIR_D));
        cut(c, arrow(b.cx - k * 1.9f, b.cy, k * 0.8f, DIR_L));
        cut(c, arrow(b.cx + k * 1.9f, b.cy, k * 0.8f, DIR_R));
    } else {
        for (int i = 0; i < n; i++) cut(c, arrow(b.cx + (n > 1 ? (i ? 1 : -1) * k * 1.5f : 0), b.cy, k, d[i]));
    }
}

// A mouse: button 1 left, 2 middle, 3 right, 4 / 5 the side buttons; 0 none. dirs: arrows around it (movement)
static void mouse(Canvas *c, Box b, int button, int dirs) {
    float sc = dirs ? 0.7f : 1, x = b.cx, y = b.cy, hw = b.s * 0.29f * sc, hh = b.s * 0.45f * sc, t = b.s * 0.05f;
    float split = y - hh * 0.15f;
    Shape body = box(x, y, hw, hh, hw);
    fill(c, ring(body, t));
    fill(c, seg(x - hw, split, x + hw, split, t * 0.8f));
    fill(c, seg(x, y - hh, x, split, t * 0.8f));
    int x0, y0, x1, y1;
    bounds(c, &body, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float X = unit(c, px), Y = unit(c, py), d = sd(&body, X, Y);
            if (button == 1) d = maxf(d, maxf(X - x, Y - split));
            else if (button == 3) d = maxf(d, maxf(x - X, Y - split));
            else if (button == 4 || button == 5) d = maxf(d, maxf(X - (x - hw * 0.3f), fabsf(Y - (y + hh * (button == 4 ? 0.2f : 0.55f))) - hh * 0.14f));
            else continue;
            c->a[py * c->n + px] = maxf(c->a[py * c->n + px], cover(c, d));
        }
    if (button == 2) fill(c, seg(x, y - hh * 0.62f, x, y - hh * 0.32f, hw * 0.32f));
    float k = b.s * 0.09f, g = b.s * 0.08f;
    if (dirs & DIR_U) fill(c, arrow(x, y - hh - g, k, DIR_U));
    if (dirs & DIR_D) fill(c, arrow(x, y + hh + g, k, DIR_D));
    if (dirs & DIR_L) fill(c, arrow(x - hw - g * 1.4f, y, k, DIR_L));
    if (dirs & DIR_R) fill(c, arrow(x + hw + g * 1.4f, y, k, DIR_R));
}

// ---- what each glyph shows
// G_LETTER: a dim letter of the HUD's button diamond (glyphs_letter_code), the controller's button in its place
enum { G_KEEP, G_BUTTON, G_DPAD, G_LSTICK, G_RSTICK, G_LETTER };
typedef struct { uint8_t kind, arg; } Meaning;

// The diamond's letters A B X Y: cells no prompt uses (the two empty squares, the power button, the second up-left
// arrow), each drawn inside the box of the letter it stands for
static const int k_letter_code[4] = {0xE090, 0xE091, 0xE048, 0xE08E};
int glyphs_letter_code(int btn) { return btn >= 0 && btn < 4 ? k_letter_code[btn] : 0xE040; }
int glyphs_letter_index(int btn) { return index_of(glyphs_letter_code(btn)); }

static Meaning meaning(int code) {
    for (int i = 0; i < 4; i++)
        if (code == k_letter_code[i]) return (Meaning){G_LETTER, (uint8_t)(BIND_A + i)};
    switch (code) {
    case 0xE040: case 0xE041: case 0xE042: case 0xE043: return (Meaning){G_BUTTON, (uint8_t)(BIND_A + code - 0xE040)};
    case 0xE044: return (Meaning){G_BUTTON, BIND_L};
    case 0xE045: return (Meaning){G_BUTTON, BIND_R};
    case 0xE046: return (Meaning){G_BUTTON, BIND_ZL};
    case 0xE047: return (Meaning){G_BUTTON, BIND_ZR};
    case 0xE04B: return (Meaning){G_BUTTON, BIND_PLUS};
    case 0xE04C: return (Meaning){G_BUTTON, BIND_MINUS};
    case 0xE056: return (Meaning){G_BUTTON, BIND_LSTICK};
    case 0xE057: return (Meaning){G_BUTTON, BIND_RSTICK};
    case 0xE049: return (Meaning){G_DPAD, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE060: return (Meaning){G_DPAD, DIR_U};
    case 0xE061: return (Meaning){G_DPAD, DIR_D};
    case 0xE062: return (Meaning){G_DPAD, DIR_L};
    case 0xE063: return (Meaning){G_DPAD, DIR_R};
    case 0xE064: return (Meaning){G_DPAD, DIR_U | DIR_D};
    case 0xE065: return (Meaning){G_DPAD, DIR_L | DIR_R};
    case 0xE05E: return (Meaning){G_LSTICK, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE052: return (Meaning){G_LSTICK, DIR_U};
    case 0xE050: return (Meaning){G_LSTICK, DIR_D};
    case 0xE054: return (Meaning){G_LSTICK, DIR_L | DIR_R};
    case 0xE058: return (Meaning){G_LSTICK, DIR_R};
    case 0xE05A: return (Meaning){G_LSTICK, DIR_L};
    case 0xE05F: return (Meaning){G_RSTICK, DIR_U | DIR_D | DIR_L | DIR_R};
    case 0xE05D: return (Meaning){G_RSTICK, DIR_U | DIR_D};
    case 0xE053: return (Meaning){G_RSTICK, DIR_U};
    case 0xE051: return (Meaning){G_RSTICK, DIR_D};
    case 0xE055: return (Meaning){G_RSTICK, DIR_L | DIR_R};
    case 0xE059: return (Meaning){G_RSTICK, DIR_R};
    case 0xE05B: return (Meaning){G_RSTICK, DIR_L};
    default: return (Meaning){G_KEEP, 0};  // (the power button, Home, the console, arrows and marks)
    }
}

static const int k_dpad_slot[4] = {BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT};  // (DIR_U, DIR_D, DIR_L, DIR_R order)
static const int k_dpad_host[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                   SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
static int dir_index(int dir) { return dir == DIR_U ? 0 : dir == DIR_D ? 1 : dir == DIR_L ? 2 : 3; }

// The game's own glyph for a controller button by position (Nintendo names), 0: none
static int nintendo_code(int host) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_EAST: return 0xE040;
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0xE041;
    case SDL_GAMEPAD_BUTTON_NORTH: return 0xE042;
    case SDL_GAMEPAD_BUTTON_WEST: return 0xE043;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 0xE044;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 0xE045;
    case PADB_LTRIGGER: return 0xE046;
    case PADB_RTRIGGER: return 0xE047;
    case SDL_GAMEPAD_BUTTON_START: return 0xE04B;
    case SDL_GAMEPAD_BUTTON_BACK: return 0xE04C;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 0xE056;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 0xE057;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 0xE060;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 0xE061;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 0xE062;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 0xE063;
    case SDL_GAMEPAD_BUTTON_GUIDE: return 0xE04A;
    default: return 0;
    }
}

// A controller button as an Xbox / PlayStation icon
static void pad_button(Canvas *c, Box b, int host, int fam) {
    bool ps = fam == 2, num = ps || fam == 3;  // (the Steam Deck: Xbox's letters, numbered shoulders and sticks)
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
        if (ps) ps_face(c, b, host);
        else round_label(c, b, host == SDL_GAMEPAD_BUTTON_SOUTH ? "A" : host == SDL_GAMEPAD_BUTTON_EAST ? "B" :
                               host == SDL_GAMEPAD_BUTTON_WEST ? "X" : "Y");
        return;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: tab_label(c, b, num ? "L1" : "LB"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: tab_label(c, b, num ? "R1" : "RB"); return;
    case PADB_LTRIGGER: tab_label(c, b, num ? "L2" : "LT"); return;
    case PADB_RTRIGGER: tab_label(c, b, num ? "R2" : "RT"); return;
    case SDL_GAMEPAD_BUTTON_START: menu_button(c, b, true, ps); return;
    case SDL_GAMEPAD_BUTTON_BACK: menu_button(c, b, false, ps); return;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: round_label(c, b, num ? "L3" : "LS"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: round_label(c, b, num ? "R3" : "RS"); return;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: dpad(c, b, DIR_U); return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dpad(c, b, DIR_D); return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dpad(c, b, DIR_L); return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dpad(c, b, DIR_R); return;
    default: tab_label(c, b, bind_pad_label(host, fam)); return;
    }
}

// A key's short label ("Ctrl", "Space"), NULL for the arrow keys (*dir set)
static const char *key_short(int sc, int *dir) {
    static char buf[16];
    *dir = 0;
    switch (sc) {
    case SDL_SCANCODE_UP: *dir = DIR_U; return NULL;
    case SDL_SCANCODE_DOWN: *dir = DIR_D; return NULL;
    case SDL_SCANCODE_LEFT: *dir = DIR_L; return NULL;
    case SDL_SCANCODE_RIGHT: *dir = DIR_R; return NULL;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Ctrl";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return "Win";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_BACKSPACE: return "Bksp";
    case SDL_SCANCODE_CAPSLOCK: return "Caps";
    case SDL_SCANCODE_DELETE: return "Del";
    case SDL_SCANCODE_INSERT: return "Ins";
    case SDL_SCANCODE_PAGEUP: return "PgUp";
    case SDL_SCANCODE_PAGEDOWN: return "PgDn";
    case SDL_SCANCODE_SPACE: return "Space";
    default: break;
    }
    const char *n = sc > 0 && sc < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    if (!strncmp(n, "Keypad ", 7)) { snprintf(buf, sizeof buf, "Num%s", n + 7); return buf; }
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "?");
    return buf;
}

// A keyboard binding (key or mouse button) as an icon
static void key_icon(Canvas *c, Box b, int sc) {
    if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) { mouse(c, b, sc - BIND_MOUSE, 0); return; }
    int dir;
    const char *l = key_short(sc, &dir);
    keycap(c, b, l, dir);
}

// Several keys (a stick's directions) on one keycap: arrows, or single-character labels side by side
static void keys_icon(Canvas *c, Box b, const int *slots, int dirs) {
    char label[16] = "";
    int arrows = 0, n = 0;
    bool letters = true;
    for (int i = 0; i < 4; i++) {
        if (!(dirs & (1 << i))) continue;
        int sc = g_bind_key[slots[i]], dir;
        if (sc > BIND_MOUSE && sc <= BIND_MOUSE_LAST) { letters = false; continue; }
        const char *l = key_short(sc, &dir);
        if (!l) arrows |= dir;
        else if (strlen(l) == 1 && n < 6) label[n++] = l[0];
        else letters = false;
    }
    if (arrows == dirs) { keycap(c, b, NULL, dirs); return; }
    if (letters && n && !arrows) {
        if (n == 4) {  // up left down right: "WASD"
            char wasd[5] = {label[0], label[2], label[1], label[3], 0};
            keycap(c, b, wasd, 0);
            return;
        }
        keycap(c, b, label, 0);
        return;
    }
    keycap(c, b, NULL, dirs);  // (a mix: show the directions)
}

static int resolve_kind(void) {
    int k = g_settings.button_icons;
    if (k > ICONS_AUTO && k < ICONS_COUNT) return k;
    if (input_last_device() == DEVICE_KEYBOARD) return ICONS_KEYBOARD;
    int f = input_pad_family();
    return f == 1 ? ICONS_XBOX : f == 2 ? ICONS_PLAYSTATION : f == 3 ? ICONS_STEAMDECK : ICONS_NINTENDO;
}

static int face_vertex(int host) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_EAST: return 0;
    case SDL_GAMEPAD_BUTTON_SOUTH: return 1;
    case SDL_GAMEPAD_BUTTON_NORTH: return 2;
    case SDL_GAMEPAD_BUTTON_WEST: return 3;
    default: return -1;
    }
}

void glyphs_hud_positions(int pos[4]) {
    static const int k_presets[BUTTON_LAYOUT_COUNT][4] = {
        {0, 1, 2, 3},  // BUTTON_LAYOUT_AUTO (fallback: Nintendo)
        {0, 1, 2, 3},  // BUTTON_LAYOUT_NINTENDO: East A, South B, North X, West Y
        {1, 0, 2, 3},  // BUTTON_LAYOUT_SWAP_AB:  South A, East B, North X, West Y
        {1, 0, 3, 2},  // BUTTON_LAYOUT_XBOX:     South A, East B, West X, North Y
        {0, 1, 3, 2},  // BUTTON_LAYOUT_SWAP_XY:  East A, South B, West X, North Y
        {1, 3, 0, 2},  // BUTTON_LAYOUT_GAMECUBE: South A, West B, East X, North Y
    };
    int layout = g_settings.button_layout;
    if (layout > BUTTON_LAYOUT_AUTO && layout < BUTTON_LAYOUT_COUNT) {
        memcpy(pos, k_presets[layout], 4 * sizeof(int));
        return;
    }
    int p[4], mask = 0;
    for (int i = 0; i < 4; i++) {
        p[i] = face_vertex(g_bind_pad[BIND_A + i]);
        if (p[i] >= 0) mask |= 1 << p[i];
    }
    if (mask == 0xF && (p[0] != 0 || p[1] != 1 || p[2] != 2 || p[3] != 3)) {
        memcpy(pos, p, sizeof p);
        return;
    }
    static const int k_acts[4] = {0, 14, 2, 3};  // Interact (A), Cancel (B), Jump (X), Attack (Y)
    mask = 0;
    for (int i = 0; i < 4; i++) {
        int h = g_act_pad[k_acts[i]];
        if (h == ACT_FOLLOW) h = g_bind_pad[BIND_A + i];
        p[i] = face_vertex(h);
        if (p[i] >= 0) mask |= 1 << p[i];
    }
    if (mask == 0xF) {
        memcpy(pos, p, sizeof p);
        return;
    }
    memcpy(pos, k_presets[0], 4 * sizeof(int));
}

static int g_hud_built[4];
static _Atomic bool g_hud_built_valid;
RT_ON_RESET(glyphs_hud_built_reset) { g_hud_built_valid = false; }  // (the next boot patches the layouts again)

void glyphs_hud_positions_built(const int pos[4]) {
    memcpy(g_hud_built, pos, sizeof g_hud_built);
    g_hud_built_valid = true;
}

bool glyphs_hud_positions_stale(void) {
    if (!g_hud_built_valid) return false;
    int pos[4];
    glyphs_hud_positions(pos);
    return memcmp(pos, g_hud_built, sizeof pos) != 0;
}

// ---- the face buttons' characters follow the actions in play
// The game draws its prompts with the Switch buttons' characters (A B X Y). With per-action bindings an action can sit
// on another button than its Switch button's: in play each character stands for the button every action of its Switch
// button moved to (input_bind.h act_prompt_host: Jump and the other Switch X actions all on A, the X character shows
// A), while a rune is out the A character stands for Activate rune's button, and on the Master Cycle Zero A and B
// stand for Cycle accelerate's and Cycle brake's. The menus keep the Switch layer
// (g_bind_pad / g_bind_key): their prompts are the menus' own buttons. The sheet goes up again when that changes
// (generation), so a prompt on screen changes on the next frame.
#define ACT_ACTIVATE_RUNE 34   // (bindings.c k_act_names)
#define ACT_CYCLE_ACCELERATE 38
#define ACT_CYCLE_BRAKE 39
#define RUNE_ASKED_MS 150      // asked that recently: a rune is out
static int g_face[2][4];       // latched by generation(): per device (pad, keys) the host for A, B, X, Y

static bool in_play(void) { return g_site.pause_state && LD32(g_site.pause_state) == 0; }  // (not the pause menu, map)

// A rune is out: the game asks about Activate rune. Its prompt fades in before the first time it asks and out after the
// last, and while a rune is out it doesn't ask every frame: so the A prompt's label seen while it asked (any language)
// stands for the rune whenever it is on screen, fading in or out included, and is remembered for the next time a rune
// comes out (else the fade-in showed "A Activate" in the slot's own corner first). A time limit on top of that made the
// character and the slot flicker between the two while Magnesis was out.
// With no prompt on screen the last answer stays: the game redraws the button's texture and lays the slot out only
// while the prompt shows, so going back as it vanished left the next rune prompt's first frame in the old state ("Y
// Activate" in the slot's own corner). Another prompt (Take, Talk) is what puts it back.
static int g_letter_swap = -1;  // the dim letter (B X Y: 1-3) in the slot's own corner while the slot is moved, or -1
static bool rune_out(void) {
    static uint32_t rune_label;
    static bool latched;
    uint32_t label = act_a_prompt_label();
    if (act_asked_within(ACT_ACTIVATE_RUNE, RUNE_ASKED_MS)) {
        if (label > 1) rune_label = label;
        return latched = true;
    }
    if (label > 1) latched = label == rune_label;
    return latched;
}

// While the A character stands for Activate rune, the A slot of the HUD's button diamond moves to that button's corner
// (and the dim letter there to the slot's own), with the values runtime/services/fs.c hud_letters_flyt gives a slot at
// boot. Not to the left corner, whose labels the game lays out the other way round.
static const float k_act_xy[4][2] = {{26.0f, 0.0f}, {0.0f, -26.0f}, {0.0f, 26.0f}, {-26.0f, 0.0f}};  // (fs.c)
static const float k_dim_xy[4][2] = {{26.2f, 0.8f}, {1.0f, -26.0f}, {0.8f, 26.0f}, {-25.5f, -0.2f}};
static void setf(uint64_t p, unsigned off, float v) { if (p) memcpy((void *)(uintptr_t)(p + off), &v, 4); }
static float getf(uint64_t p, unsigned off) {
    float v = 0;
    if (p) memcpy(&v, (const void *)(uintptr_t)(p + off), 4);
    return v;
}
static int face_vertex(int host);
// The panes' new places show from the game's next layout pass, a frame after the glyph sheet's new character: the
// character waits for them (RUNE_SETTLE: 1.5 frames), or for one frame "Y Activate" showed in the slot's own corner.
// Where the slot is gets read back each time (the game can put it back by itself). Returns true once the slot has been
// where host's button is that long; *moved: the dim letter (1-3) that went to the slot's own corner, or -1.
static bool rune_slot(int host, int *moved) {  // host < 0: back to the slot's own corner
    static int applied = -1;
    static uint64_t applied_ns;
    int pos[4];
    glyphs_hud_positions(pos);
    int own = pos[0], to = host >= 0 ? face_vertex(host) : -1;
    if (to < 0 || to == 3 || own == 3) to = own;
    *moved = -1;
    for (int i = 1; i < 4 && to != own; i++)
        if (pos[i] == to) *moved = i;
    static uint64_t btn;  // (looked up again only when the HUD was rebuilt)
    if (!btn || strncmp((const char *)(uintptr_t)(btn + 0xb8), "N_BtnA_00", 10)) btn = act_a_slot_pane("N_BtnA_00");
    uint64_t now = SDL_GetTicksNS(), period = framepace_period_ns();
    uint64_t settle = (period ? period : 33333333ull) * 3 / 2;  // RUNE_SETTLE
    if (!btn) return true;
    bool there = getf(btn, 0x30) == k_act_xy[to][0] && getf(btn, 0x34) == k_act_xy[to][1];
    if (there && to == applied) return to == own || now - applied_ns >= settle;
    if (there) { applied = to, applied_ns = now; return to == own; }
    uint64_t base = act_a_slot_pane("P_Base_00"), capture = act_a_slot_pane("N_CaptureA_00");
    setf(btn, 0x30, k_act_xy[to][0]), setf(btn, 0x34, k_act_xy[to][1]);           // (translate)
    setf(base, 0x44, (to == 2 || to == 3) ? 180.0f : 0.0f), setf(base, 0x48, to == 2 ? -1.0f : 1.0f);  // (rotate z, scale x)
    setf(capture, 0x30, k_act_xy[to][0] + 60), setf(capture, 0x34, k_act_xy[to][1]);
    static const char *const letters[4] = {0, "T_B_00", "T_X_00", "T_Y_00"};  // (B X Y: pos[1..3])
    for (int i = 1; i < 4; i++) {
        uint64_t t = act_a_slot_pane(letters[i]);
        int at = pos[i] == to ? own : pos[i];  // (the letter in the slot's new corner takes its old one)
        setf(t, 0x30, k_dim_xy[at][0]), setf(t, 0x34, k_dim_xy[at][1]);
    }
    applied = to, applied_ns = now;
    return to == own;
}

// Controls > Prompts follow action bindings (g_settings.prompts_follow); off: the Switch layer everywhere and the slot
// in its own corner (put back if the setting goes off with the slot moved)
static void resolve_face(void) {
    bool on = g_settings.prompts_follow, play = on && in_play(), rune = play && rune_out();
    bool cycle = play && !rune && act_cycle_live();
    int rune_host = rune ? g_act_pad[ACT_ACTIVATE_RUNE] : -1, moved = -1;
    if ((play || !on) && !rune_slot(rune_host, &moved)) rune = false;  // (the characters follow once the slot is there)
    g_letter_swap = rune ? moved : -1;
    for (int dev = 0; dev < 2; dev++)
        for (int i = 0; i < 4; i++) {
            int layer = dev ? g_bind_key[BIND_A + i] : g_bind_pad[BIND_A + i];
            int h = play ? act_prompt_host(dev ? BIND_DEV_KEY : BIND_DEV_PAD, BIND_A + i) : -1;
            if (rune && i == 0) {
                int r = dev ? g_act_key[ACT_ACTIVATE_RUNE] : g_act_pad[ACT_ACTIVATE_RUNE];
                if (dev ? r > 0 : r >= 0) h = r;
            }
            if (cycle && i < 2) {  // (on the Master Cycle Zero its prompts are Accelerate and Brake: #967)
                int id = i == 0 ? ACT_CYCLE_ACCELERATE : ACT_CYCLE_BRAKE;
                int r = dev ? g_act_key[id] : g_act_pad[id];
                if (dev ? r > 0 : r >= 0) h = r;
            }
            g_face[dev][i] = h >= 0 && (dev ? h > 0 : 1) ? h : layer;
        }
}
// The Switch button whose controller button a dim letter shows: its own, or A's for the letter that took the slot's
// own corner while the slot is at the rune's (that corner is A's place on the controller)
static int letter_slot(int b) { return g_letter_swap >= 0 && b == BIND_A + g_letter_swap ? BIND_A : b; }
static int face_pad(int b) { return b >= BIND_A && b <= BIND_Y ? g_face[0][b - BIND_A] : g_bind_pad[b]; }
static int face_key(int b) { return b >= BIND_A && b <= BIND_Y ? g_face[1][b - BIND_A] : g_bind_key[b]; }

// cursor: with the state of the menus' mouse pointer, which changes some keyboard icons while a menu is open.
static uint32_t generation(bool cursor) {
    int k = resolve_kind();
    uint32_t h = 2166136261u ^ (uint32_t)k;
    resolve_face();
    for (int d = 0; d < 2; d++)
        for (int i = 0; i < 4; i++) h = (h ^ (uint16_t)g_face[d][i]) * 16777619u;
    h = (h ^ (uint32_t)(g_letter_swap + 1)) * 16777619u;
    if (k == ICONS_KEYBOARD) {
        if (cursor) {
            h = (h ^ pc_cursor_preview_active()) * 16777619u;
            h = (h ^ pc_cursor_sensor_active()) * 16777619u;
        }
        for (int i = 0; i < BIND_KEY_COUNT; i++) h = (h ^ (uint16_t)g_bind_key[i]) * 16777619u;
        h = (h ^ g_settings.mouse_camera) * 16777619u;
    } else {
        for (int i = 0; i < BIND_PAD_COUNT; i++) h = (h ^ (uint16_t)g_bind_pad[i]) * 16777619u;
    }
    return h | 1;
}

uint32_t glyphs_generation(void) { return generation(true); }
uint32_t glyphs_font_generation(void) { return generation(false); }

static int glyphs_console_wanted(void) {
    switch (g_settings.button_icons) {
    case ICONS_STEAMDECK: return HW_DEVICE_DECK;
    case ICONS_XBOX: return HW_DEVICE_XBOX;
    case ICONS_PLAYSTATION: return HW_DEVICE_PS5;
    case ICONS_AUTO: {  // the controller in use (an Xbox or PlayStation one also on a Deck), else the Deck's own
        int f = bind_label_family();  // (the controller connected, also before the game opens one)
        if (f == 1 || f == 2) return f == 1 ? HW_DEVICE_XBOX : HW_DEVICE_PS5;
        return f == 3 || settings_steam_deck() ? HW_DEVICE_DECK : -1;
    }
    default: return -1;  // (keyboard, Nintendo: the game's Switch)
    }
}

static volatile int g_console = -2;  // (-2: not decided this boot)
RT_ON_RESET(glyphs_console_reset) { g_console = -2; }  // (a restart decides again)

int glyphs_console(void) {
    if (g_console == -2) {
        g_console = glyphs_console_wanted();
        static const char *const names[HW_DEVICE_COUNT] = {"the Steam Deck", "an Xbox controller", "a DualSense"};
        LOG("[glyphs] Controls page: %s\n", g_console >= 0 ? names[g_console] : "the game's Switch");
    }
    return g_console;
}

static const char *const k_kind_names[ICONS_COUNT] = {"auto", "keyboard", "Xbox", "PlayStation", "Nintendo", "Steam Deck"};

// The icon for one glyph: false = keep the game's (or *copy: the game's glyph for that code instead)
static bool draw_glyph(Canvas *c, Box b, int code, int kind, int *copy) {
    Meaning m = meaning(code);
    *copy = 0;
    if (m.kind == G_KEEP) return false;
    int fam = kind == ICONS_PLAYSTATION ? 2 : kind == ICONS_STEAMDECK ? 3 : 1;
    if (kind == ICONS_NINTENDO) {  // the game's glyphs, following the controller bindings
        int host = -1;
        if (m.kind == G_BUTTON) host = face_pad(m.arg);
        else if (m.kind == G_LETTER) host = g_bind_pad[letter_slot(m.arg)];
        else if (m.kind == G_DPAD && !(m.arg & (m.arg - 1))) host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
        int want = host >= 0 ? nintendo_code(host) : 0;
        if (want && want != code) *copy = want;
        return false;
    }
    if (kind == ICONS_KEYBOARD) {
        switch (m.kind) {
        case G_BUTTON:
            if(m.arg==BIND_RSTICK&&pc_cursor_preview_active()) {mouse(c,b,SDL_BUTTON_LEFT,0);return true;}
            key_icon(c, b, m.arg==BIND_Y&&pc_cursor_sensor_active()?g_bind_key[BIND_LEFT]:face_key(m.arg)); return true;
        case G_LETTER: key_icon(c, b, g_bind_key[letter_slot(m.arg)]); return true;
        case G_DPAD:
            if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[k_dpad_slot[dir_index(m.arg)]]);
            else keys_icon(c, b, k_dpad_slot, m.arg);
            return true;
        case G_LSTICK: {
            static const int ls[4] = {BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT};
            if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[ls[dir_index(m.arg)]]);
            else keys_icon(c, b, ls, m.arg);
            return true;
        }
        default: {  // the right stick: the mouse when it turns the camera, else its keys
            if(pc_cursor_preview_active()){mouse(c,b,SDL_BUTTON_LEFT,0);return true;}
            static const int rs[4] = {BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT};
            if (g_settings.mouse_camera) mouse(c, b, 0, m.arg);
            else if (!(m.arg & (m.arg - 1))) key_icon(c, b, g_bind_key[rs[dir_index(m.arg)]]);
            else keys_icon(c, b, rs, m.arg);
            return true;
        }
        }
    }
    // Xbox / PlayStation / Steam Deck: buttons through the bindings; the sticks keep the game's (L / R)
    switch (m.kind) {
    case G_BUTTON: pad_button(c, b, face_pad(m.arg), fam); return true;
    case G_LETTER: pad_button(c, b, g_bind_pad[letter_slot(m.arg)], fam); return true;
    case G_DPAD:
        if (!(m.arg & (m.arg - 1))) {
            int host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
            if (host != k_dpad_host[dir_index(m.arg)]) { pad_button(c, b, host, fam); return true; }
        }
        dpad(c, b, m.arg);
        return true;
    default: return false;
    }
}

// Where glyph index i's cell is in the uploaded sheet: its top-left pixel and whether rows run bottom-up
static void cell_origin(const SheetFont *f, uint32_t i, int *x0, int *y0) {
    *x0 = (int)(i % f->cols) * (CELL + 1);
    *y0 = (int)(i / f->cols) * (CELL + 1);
}
// The row of a cell's pixel y (y down) in the sheet as stored (bottom-up), k pixels for each of the game's texels
static int sheet_y(const SheetFont *f, uint32_t k, int y0, int y) { return (int)(f->sheet_h * k) - 1 - (y0 * (int)k + y); }

// The sheet as already redrawn, one per generation and size. Opening or closing a menu with the keyboard changes the
// generation (the pointer's state above) and each redraw took 50-60 ms of a frame (#591). The sheet's contents are
// checked first and the icons depend only on the generation, so a sheet of the same generation is the same.
#define SHEET_CACHE 8
static struct { uint32_t gen, k; uint64_t bytes; uint8_t *data; } g_sheet_cache[SHEET_CACHE];
static int g_sheet_next;
static pthread_mutex_t g_sheet_lock = PTHREAD_MUTEX_INITIALIZER;

static bool sheet_cached(uint32_t gen, uint32_t k, uint8_t *out, uint64_t bytes) {
    bool found = false;
    pthread_mutex_lock(&g_sheet_lock);
    for (int i = 0; i < SHEET_CACHE && !found; i++)
        if (g_sheet_cache[i].data && g_sheet_cache[i].gen == gen && g_sheet_cache[i].k == k && g_sheet_cache[i].bytes == bytes)
            memcpy(out, g_sheet_cache[i].data, bytes), found = true;
    pthread_mutex_unlock(&g_sheet_lock);
    return found;
}

static void sheet_cache_add(uint32_t gen, uint32_t k, const uint8_t *data, uint64_t bytes) {
    pthread_mutex_lock(&g_sheet_lock);
    typeof(g_sheet_cache[0]) *e = &g_sheet_cache[g_sheet_next];
    if (e->bytes != bytes) free(e->data), e->data = malloc(bytes), e->bytes = bytes;
    if (e->data) {
        memcpy(e->data, data, bytes);
        e->gen = gen, e->k = k;
        g_sheet_next = (g_sheet_next + 1) % SHEET_CACHE;
    } else {
        e->bytes = 0;
    }
    pthread_mutex_unlock(&g_sheet_lock);
}

// The texture at addr is the game's glyph sheet (its size, format and contents)
static bool is_sheet(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h) {
    const SheetFont *f = &k_sheet;
    if (format != FMT_BC4 || w != f->sheet_w || h != f->sheet_h) return false;
    if (addr < pool_lo || addr + SHEET_BYTES > pool_hi) return false;
    uint64_t hash = fnv1a((const uint8_t *)(uintptr_t)addr, SHEET_BYTES);
    if (hash != SHEET_HASH) {
        static int nlog;
        if (nlog++ < 4) LOG("[glyphs] 256x1024 BC4 texture %llx is not the button glyph sheet (hash %016llx)\n",
                            (unsigned long long)addr, (unsigned long long)hash);
        return false;
    }
    return true;
}

// The icons for the device in use, into img: the sheet as stored, k pixels for each of the game's texels. orig: the
// game's sheet; base: the same at img's size (orig itself at 1x), for the glyphs kept or moved. Returns how many changed.
static int redraw_sheet(const uint8_t *orig, const uint8_t *base, uint32_t k, int kind, uint8_t *img) {
    const SheetFont *f = &k_sheet;
    uint32_t w = f->sheet_w, h = f->sheet_h, W = w * k, n = CELL * k;
    memcpy(img, base, (uint64_t)W * h * k);
    Canvas cv;
    if (!canvas_init(&cv, (int)k)) return 0;
    int changed = 0;
    for (int code = 0xE040; code < 0xE0A0; code++) {
        uint32_t gi = index_of(code);
        if (gi == 0xffff) continue;
        int x0, y0, lx0, ly0;
        cell_origin(f, gi, &x0, &y0);
        if (x0 + CELL > (int)w || y0 + CELL > (int)h) continue;
        Meaning mc = meaning(code);  // (a diamond letter: the box of the letter it stands for)
        cell_origin(f, mc.kind == G_LETTER ? index_of(0xE040 + mc.arg - BIND_A) : gi, &lx0, &ly0);
        // the original glyph's box (its lit pixels)
        int bx0 = CELL, by0 = CELL, bx1 = -1, by1 = -1;
        for (int y = 0; y < CELL; y++)
            for (int x = 0; x < CELL; x++)
                if (orig[(uint64_t)sheet_y(f, 1, ly0, y) * w + lx0 + x] > 96) {  // (a diamond letter: its letter's box)
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
        if (bx1 < 0) bx0 = 4, by0 = 6, bx1 = 45, by1 = 45;
        Box b = {(bx0 + bx1 + 1) / 2.0f, (by0 + by1 + 1) / 2.0f, (float)(by1 - by0 + 1), (float)(bx1 - bx0 + 1)};
        memset(cv.a, 0, (size_t)n * n * sizeof *cv.a);
        int copy = 0;
        bool drawn = draw_glyph(&cv, b, code, kind, &copy);
        if (copy && index_of(copy) != 0xffff) {  // the game's glyph from another cell
            int sx0, sy0;
            cell_origin(f, index_of(copy), &sx0, &sy0);
            for (uint32_t y = 0; y < n; y++)
                memcpy(img + (uint64_t)sheet_y(f, k, y0, (int)y) * W + x0 * k, base + (uint64_t)sheet_y(f, k, sy0, (int)y) * W + sx0 * k, n);
            changed++;
        } else if (drawn) {
            for (uint32_t y = 0; y < n; y++)
                for (uint32_t x = 0; x < n; x++)
                    img[(uint64_t)sheet_y(f, k, y0, (int)y) * W + x0 * k + x] = (uint8_t)lroundf(cv.a[y * n + x] * 255);
            changed++;
        }
    }
    canvas_free(&cv);
    return changed;
}

// SAVAGEWIND_GLYPH_DUMP: the sheet as uploaded (as stored: bottom-up), the original before it when one is given
static void dump_sheet(const uint8_t *orig, const uint8_t *img, uint32_t w, uint32_t h) {
    static int n;
    char path[128];
    snprintf(path, sizeof path, "build/glyph_sheet_%d.pgm", ++n);
    FILE *fp = n <= 16 ? fopen(path, "wb") : NULL;
    if (!fp) return;
    fprintf(fp, "P5\n%u %u\n255\n", orig ? w * 2 : w, h);
    for (uint32_t y = 0; y < h; y++) {
        if (orig) fwrite(orig + (uint64_t)y * w, 1, w, fp);
        fwrite(img + (uint64_t)y * w, 1, w, fp);
    }
    fclose(fp);
}

bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb) {
    if (w != wb * 4 || h != hb * 4 || !is_sheet(addr, pool_lo, pool_hi, format, w, h)) return false;
    uint32_t gen = glyphs_generation();
    bool dump = getenv("SAVAGEWIND_GLYPH_DUMP") != NULL;
    if (!dump && sheet_cached(gen, 1, lin, SHEET_BYTES)) return true;
    int kind = resolve_kind();
    uint8_t *orig = malloc((uint64_t)w * h), *img = malloc((uint64_t)w * h);
    bc4_decode(lin, wb, hb, orig);
    int changed = redraw_sheet(orig, orig, 1, kind, img);
    if (changed) bc4_encode(img, wb, hb, lin);
    sheet_cache_add(gen, 1, lin, SHEET_BYTES);
    static int nlog;
    if (nlog++ < 16)
        LOG("[glyphs] button glyph sheet %llx: %s icons, %d glyphs redrawn\n", (unsigned long long)addr,
            k_kind_names[kind], changed);
    if (dump) dump_sheet(orig, img, w, h);
    free(orig);
    free(img);
    return true;
}

bool glyphs_sheet_known(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h) {
    return is_sheet(addr, pool_lo, pool_hi, format, w, h);
}

// Gameplay > Sharper text. The icons are shapes and Fira Sans, so they are drawn again at the larger size rather
// than enlarged; the game's own glyphs (the ones kept, and all of them with the Nintendo icons) are enlarged as the
// game's text is (text_upscale.c), once a boot: the sheet's contents never change.
static uint8_t *g_big;
static uint32_t g_big_k;

bool glyphs_draw_sheet(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                       const uint8_t *lin, uint32_t wb, uint32_t hb, uint32_t k, uint8_t *out) {
    uint64_t bytes = (uint64_t)w * k * h * k;
    if (k < 2 || k > 4 || w != wb * 4 || h != hb * 4) return false;
    uint8_t *orig = malloc((uint64_t)w * h);
    if (!orig) return false;
    bc4_decode(lin, wb, hb, orig);
    if (!is_sheet(addr, pool_lo, pool_hi, format, w, h)) {  // (something else in its memory now: only enlarged)
        if (!text_upscale(orig, w, h, k, false, 0, 0, out)) memset(out, 0, bytes);
        free(orig);
        return false;
    }
    uint32_t gen = glyphs_generation();
    bool dump = getenv("SAVAGEWIND_GLYPH_DUMP") != NULL;
    if (!dump && sheet_cached(gen, k, out, bytes)) {
        free(orig);
        return true;
    }
    uint64_t t0 = SDL_GetTicksNS();
    pthread_mutex_lock(&g_sheet_lock);
    if (g_big_k != k) {
        free(g_big);
        g_big = malloc(bytes);
        g_big_k = g_big && text_upscale(orig, w, h, k, true, 0, 0, g_big) ? k : 0;
    }
    pthread_mutex_unlock(&g_sheet_lock);
    int kind = resolve_kind(), changed = 0;
    if (g_big_k == k) changed = redraw_sheet(orig, g_big, k, kind, out);
    else memset(out, 0, bytes);
    sheet_cache_add(gen, k, out, bytes);
    static int nlog;
    if (nlog++ < 16)
        LOG("[glyphs] button glyph sheet %llx drawn %ux: %s icons, %d glyphs redrawn in %.0f ms\n", (unsigned long long)addr,
            k, k_kind_names[kind], changed, (SDL_GetTicksNS() - t0) / 1e6);
    if (dump) dump_sheet(NULL, out, w * k, h * k);
    free(orig);
    return true;
}

// Some prompts are not text: the HUD draws them with textures of its own layouts (Layout/Common.sblarc): the action
// button ("Pick up", "Talk"...), the paraglider's button with its diamond of the others, the d-pad of the quick menus.
// R8 coverage masks, a button glyph each, recognised by size and contents (as uploaded, deswizzled). The dim letters
// of that diamond are text: runtime/services/fs.c (hud_letters_patch) makes them icon characters of this sheet.
typedef struct { uint16_t w, h; uint64_t hash; int code; } LayoutIcon;
static const LayoutIcon k_layout_icons[] = {
    {48, 48, 0x3cf281fb77fe6e1full, 0xE040},  // Nt_KeyTexA_00
    {48, 48, 0x5c11641d8572ef3eull, 0xE041},  // Nt_KeyTexB_00
    {48, 48, 0x80ac73dec7998dc2ull, 0xE042},  // Nt_KeyTexX_00
    {48, 48, 0x4d2a26b5991ee454ull, 0xE043},  // Nt_KeyTexY_00
    {48, 48, 0x01ba4a1c31788ef6ull, 0xE044},  // Nt_KeyTexL_00
    {48, 48, 0xc85329ba2d18b0cbull, 0xE046},  // Nt_KeyTexZL_00
    {48, 48, 0xc3719621a38a3959ull, 0xE05E},  // Nt_KeyTexLSt_01: the left stick
    {48, 48, 0x0d90f7505ab43e59ull, 0xE052},  // Nt_KeyTexLSt_00: the left stick, up
    {46, 46, 0xde6c79012595283cull, 0xE049},  // ButtonIcon46_00: the d-pad
    {64, 64, 0xc171fee97aa2e0b1ull, 0xE049},  // ButtonIconL46NN_00: the d-pad (the shortcuts at the top left)
};

static const LayoutIcon *find_layout_icon(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row) {
    if (w > 2 * CELL || h > 2 * CELL || row < w) return NULL;
    const LayoutIcon *li = NULL;
    for (size_t i = 0; i < sizeof k_layout_icons / sizeof *k_layout_icons && !li; i++)
        if (k_layout_icons[i].w == w && k_layout_icons[i].h == h) {
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) hash = (hash ^ lin[(uint64_t)y * row + x]) * 0x100000001b3ull;
            for (size_t k = i; k < sizeof k_layout_icons / sizeof *k_layout_icons; k++)
                if (k_layout_icons[k].w == w && k_layout_icons[k].h == h && k_layout_icons[k].hash == hash) li = &k_layout_icons[k];
            break;
        }
    return li;
}

bool glyphs_is_layout_icon(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row) {
    return find_layout_icon(w, h, lin, row) != NULL;
}

// The icon li for the device in use into out, s texels for each of the texture's (rows out_row apart; out may be lin
// at s = 1). False when the game's own stays (lin unchanged at s = 1).
static bool layout_draw(const LayoutIcon *li, uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row, uint32_t s,
                        uint8_t *out, uint32_t out_row) {
    // the icon's box (its lit pixels), in a cell canvas with the texture centred in it (scaled down to fit)
    float k = w > CELL || h > CELL ? (float)CELL / (float)(w > h ? w : h) : 1;
    float ox = (CELL - w * k) / 2, oy = (CELL - h * k) / 2;
    int bx0 = (int)w, by0 = (int)h, bx1 = -1, by1 = -1;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++)
            if (lin[(uint64_t)y * row + x] > 96) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
    if (bx1 < 0) return false;
    Box b = {ox + (bx0 + bx1 + 1) * k / 2, oy + (by0 + by1 + 1) * k / 2, (by1 - by0 + 1) * k, (bx1 - bx0 + 1) * k};
    Canvas cv;
    if (!canvas_init(&cv, (int)s)) return false;
    int copy = 0;
    bool drawn = draw_glyph(&cv, b, li->code, resolve_kind(), &copy);  // (Nintendo: the game's own, even with other bindings)
    if (drawn)
        for (uint32_t y = 0; y < h * s; y++)
            for (uint32_t x = 0; x < w * s; x++) {  // (bilinear from the canvas; at 48x48 the canvas's own pixels)
                float fx = clampf((ox + (x + 0.5f) / s * k) * s - 0.5f, 0, cv.n - 1), fy = clampf((oy + (y + 0.5f) / s * k) * s - 0.5f, 0, cv.n - 1);
                int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < cv.n ? x0 + 1 : x0, y1 = y0 + 1 < cv.n ? y0 + 1 : y0;
                float tx = fx - x0, ty = fy - y0;
                float a = (cv.a[y0 * cv.n + x0] * (1 - tx) + cv.a[y0 * cv.n + x1] * tx) * (1 - ty) +
                          (cv.a[y1 * cv.n + x0] * (1 - tx) + cv.a[y1 * cv.n + x1] * tx) * ty;
                out[(uint64_t)y * out_row + x] = (uint8_t)lroundf(a * 255);
            }
    canvas_free(&cv);
    return drawn;
}

bool glyphs_draw_layout(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row, uint32_t s, uint8_t *out) {
    const LayoutIcon *li = s >= 2 && s <= 4 ? find_layout_icon(w, h, lin, row) : NULL;
    if (li && layout_draw(li, w, h, lin, row, s, out, w * s)) {
        static int nlog;
        if (nlog++ < 16) LOG("[glyphs] layout icon %04X (%ux%u) drawn %ux: %s icons\n", li->code, w, h, s, k_kind_names[resolve_kind()]);
        return true;
    }
    uint8_t *plain = malloc((uint64_t)w * h);  // (the game's own, or something else there now: enlarged)
    if (plain) for (uint32_t y = 0; y < h; y++) memcpy(plain + (uint64_t)y * w, lin + (uint64_t)y * row, w);
    if (!plain || !text_upscale(plain, w, h, s, true, 0, 0, out)) memset(out, 0, (uint64_t)w * s * h * s);
    free(plain);
    return li != NULL;
}

bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row) {
    if (format != FMT_R8) return false;
    const LayoutIcon *li = find_layout_icon(w, h, lin, row);
    if (!li) return false;
    bool drawn = layout_draw(li, w, h, lin, row, 1, lin, row);
    static int nlog;
    if (nlog++ < 16) LOG("[glyphs] layout icon %04X (%ux%u): %s icons%s\n", li->code, w, h, k_kind_names[resolve_kind()], drawn ? "" : ", kept");
    return true;
}
