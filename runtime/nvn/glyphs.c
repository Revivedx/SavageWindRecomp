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
// keyboard bindings, Xbox or PlayStation buttons through the controller bindings, or the game's own glyphs (moved
// between cells when the controller bindings differ from the defaults). Nothing of Nintendo's is shipped: shapes are
// drawn, labels set in Fira Sans (OFL, third_party/fonts).
//
// The texture goes up again whenever what to show changes (glyphs_generation: device, bindings, setting), so prompts
// already on screen change on the next frame.
//
// SAVAGEWIND_GLYPH_DUMP=1 saves each sheet as uploaded, the original beside it (build/glyph_sheet_<n>.pgm, bottom-up).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "glyphs.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../rt.h"
#include "../settings.h"

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

// ---- the cell canvas: coverage 0..1, cell pixels, y down
typedef struct { float a[CELL * CELL]; } Canvas;
typedef struct { float cx, cy, s, w; } Box;  // the original glyph's centre, height and width

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

static float cover(float d) { return clampf(0.5f - d, 0, 1); }
static void fill(Canvas *c, Shape s) {
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < CELL; x++) c->a[y * CELL + x] = maxf(c->a[y * CELL + x], cover(sd(&s, x + 0.5f, y + 0.5f)));
}
static void cut(Canvas *c, Shape s) {
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < CELL; x++) c->a[y * CELL + x] *= 1 - cover(sd(&s, x + 0.5f, y + 0.5f));
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

// The text's capitals cap_h tall (less if it is wider than max_w), centred on (cx, cy): coverage into t
static void text(Canvas *t, const char *s, float cx, float cy, float cap_h, float max_w) {
    memset(t, 0, sizeof *t);
    if (!font_ready() || !*s) return;
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
    static unsigned char bm[CELL * 4 * CELL * 4];
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p, bx0, by0, bx1, by1, adv, lsb;
        float sx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, sx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw * bh <= (int)sizeof bm) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, sx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = (int)lroundf(base) + by0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < CELL && Y >= 0 && Y < CELL)
                        t->a[Y * CELL + X] = maxf(t->a[Y * CELL + X], bm[y * bw + x] / 255.0f);
                }
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
}
static void ink(Canvas *c, const Canvas *t) { for (int i = 0; i < CELL * CELL; i++) c->a[i] = maxf(c->a[i], t->a[i]); }
static void erase(Canvas *c, const Canvas *t) { for (int i = 0; i < CELL * CELL; i++) c->a[i] *= 1 - t->a[i]; }

// ---- icons
enum { DIR_U = 1, DIR_D = 2, DIR_L = 4, DIR_R = 8 };

static void cut_text(Canvas *c, const char *s, float cx, float cy, float cap, float max_w) {
    Canvas t;
    text(&t, s, cx, cy, cap, max_w);
    erase(c, &t);
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
    for (int py = 0; py < CELL; py++)  // the cross's outline (both bars as one shape)
        for (int px = 0; px < CELL; px++) {
            float d = fabsf(minf(sd(&h, px + 0.5f, py + 0.5f), sd(&v, px + 0.5f, py + 0.5f))) - t;
            c->a[py * CELL + px] = maxf(c->a[py * CELL + px], cover(d));
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
    for (int py = 0; py < CELL; py++)
        for (int px = 0; px < CELL; px++) {
            float X = px + 0.5f, Y = py + 0.5f, d = sd(&body, X, Y);
            if (button == 1) d = maxf(d, maxf(X - x, Y - split));
            else if (button == 3) d = maxf(d, maxf(x - X, Y - split));
            else if (button == 4 || button == 5) d = maxf(d, maxf(X - (x - hw * 0.3f), fabsf(Y - (y + hh * (button == 4 ? 0.2f : 0.55f))) - hh * 0.14f));
            else continue;
            c->a[py * CELL + px] = maxf(c->a[py * CELL + px], cover(d));
        }
    if (button == 2) fill(c, seg(x, y - hh * 0.62f, x, y - hh * 0.32f, hw * 0.32f));
    float k = b.s * 0.09f, g = b.s * 0.08f;
    if (dirs & DIR_U) fill(c, arrow(x, y - hh - g, k, DIR_U));
    if (dirs & DIR_D) fill(c, arrow(x, y + hh + g, k, DIR_D));
    if (dirs & DIR_L) fill(c, arrow(x - hw - g * 1.4f, y, k, DIR_L));
    if (dirs & DIR_R) fill(c, arrow(x + hw + g * 1.4f, y, k, DIR_R));
}

// ---- what each glyph shows
enum { G_KEEP, G_BUTTON, G_DPAD, G_LSTICK, G_RSTICK };
typedef struct { uint8_t kind, arg; } Meaning;

static Meaning meaning(int code) {
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
static void pad_button(Canvas *c, Box b, int host, bool ps) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
        if (ps) ps_face(c, b, host);
        else round_label(c, b, host == SDL_GAMEPAD_BUTTON_SOUTH ? "A" : host == SDL_GAMEPAD_BUTTON_EAST ? "B" :
                               host == SDL_GAMEPAD_BUTTON_WEST ? "X" : "Y");
        return;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: tab_label(c, b, ps ? "L1" : "LB"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: tab_label(c, b, ps ? "R1" : "RB"); return;
    case PADB_LTRIGGER: tab_label(c, b, ps ? "L2" : "LT"); return;
    case PADB_RTRIGGER: tab_label(c, b, ps ? "R2" : "RT"); return;
    case SDL_GAMEPAD_BUTTON_START: menu_button(c, b, true, ps); return;
    case SDL_GAMEPAD_BUTTON_BACK: menu_button(c, b, false, ps); return;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: round_label(c, b, ps ? "L3" : "LS"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: round_label(c, b, ps ? "R3" : "RS"); return;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: dpad(c, b, DIR_U); return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dpad(c, b, DIR_D); return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dpad(c, b, DIR_L); return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dpad(c, b, DIR_R); return;
    default: tab_label(c, b, bind_pad_label(host, ps ? 2 : 1)); return;
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
    return f == 1 ? ICONS_XBOX : f == 2 ? ICONS_PLAYSTATION : ICONS_NINTENDO;
}

uint32_t glyphs_generation(void) {
    int k = resolve_kind();
    uint32_t h = 2166136261u ^ (uint32_t)k;
    if (k == ICONS_KEYBOARD) {
        h = (h ^ pc_cursor_preview_active()) * 16777619u;
        h = (h ^ pc_cursor_sensor_active()) * 16777619u;
        for (int i = 0; i < BIND_KEY_COUNT; i++) h = (h ^ (uint16_t)g_bind_key[i]) * 16777619u;
        h = (h ^ g_settings.mouse_camera) * 16777619u;
    } else {
        for (int i = 0; i < BIND_PAD_COUNT; i++) h = (h ^ (uint16_t)g_bind_pad[i]) * 16777619u;
    }
    return h | 1;
}

static const char *const k_kind_names[ICONS_COUNT] = {"auto", "keyboard", "Xbox", "PlayStation", "Nintendo"};

// The icon for one glyph: false = keep the game's (or *copy: the game's glyph for that code instead)
static bool draw_glyph(Canvas *c, Box b, int code, int kind, int *copy) {
    Meaning m = meaning(code);
    *copy = 0;
    if (m.kind == G_KEEP) return false;
    bool ps = kind == ICONS_PLAYSTATION;
    if (kind == ICONS_NINTENDO) {  // the game's glyphs, following the controller bindings
        int host = -1;
        if (m.kind == G_BUTTON) host = g_bind_pad[m.arg];
        else if (m.kind == G_DPAD && !(m.arg & (m.arg - 1))) host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
        int want = host >= 0 ? nintendo_code(host) : 0;
        if (want && want != code) *copy = want;
        return false;
    }
    if (kind == ICONS_KEYBOARD) {
        switch (m.kind) {
        case G_BUTTON:
            if(m.arg==BIND_RSTICK&&pc_cursor_preview_active()) {mouse(c,b,SDL_BUTTON_LEFT,0);return true;}
            key_icon(c, b, m.arg==BIND_Y&&pc_cursor_sensor_active()?g_bind_key[BIND_LEFT]:g_bind_key[m.arg]); return true;
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
    // Xbox / PlayStation: buttons through the bindings; the sticks keep the game's (L / R)
    switch (m.kind) {
    case G_BUTTON: pad_button(c, b, g_bind_pad[m.arg], ps); return true;
    case G_DPAD:
        if (!(m.arg & (m.arg - 1))) {
            int host = g_bind_pad[k_dpad_slot[dir_index(m.arg)]];
            if (host != k_dpad_host[dir_index(m.arg)]) { pad_button(c, b, host, ps); return true; }
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
static int sheet_y(const SheetFont *f, int y0, int y) { return (int)f->sheet_h - 1 - (y0 + y); }  // (stored bottom-up)

bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb) {
    const SheetFont *f = &k_sheet;
    if (format != FMT_BC4 || w != f->sheet_w || h != f->sheet_h || w != wb * 4 || h != hb * 4) return false;
    if (addr < pool_lo || addr + SHEET_BYTES > pool_hi) return false;
    uint64_t hash = fnv1a((const uint8_t *)(uintptr_t)addr, SHEET_BYTES);
    if (hash != SHEET_HASH) {
        static int nlog;
        if (nlog++ < 4) LOG("[glyphs] 256x1024 BC4 texture %llx is not the button glyph sheet (hash %016llx)\n",
                            (unsigned long long)addr, (unsigned long long)hash);
        return false;
    }
    int kind = resolve_kind();
    uint8_t *orig = malloc((uint64_t)w * h), *img = malloc((uint64_t)w * h);
    bc4_decode(lin, wb, hb, orig);
    memcpy(img, orig, (uint64_t)w * h);
    int changed = 0;
    for (int code = 0xE040; code < 0xE0A0; code++) {
        uint32_t gi = index_of(code);
        if (gi == 0xffff) continue;
        int x0, y0;
        cell_origin(f, gi, &x0, &y0);
        if (x0 + CELL > (int)w || y0 + CELL > (int)h) continue;
        // the original glyph's box (its lit pixels)
        int bx0 = CELL, by0 = CELL, bx1 = -1, by1 = -1;
        for (int y = 0; y < CELL; y++)
            for (int x = 0; x < CELL; x++)
                if (orig[(uint64_t)sheet_y(f, y0, y) * w + x0 + x] > 96) {
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
        if (bx1 < 0) bx0 = 4, by0 = 6, bx1 = 45, by1 = 45;
        Box b = {(bx0 + bx1 + 1) / 2.0f, (by0 + by1 + 1) / 2.0f, (float)(by1 - by0 + 1), (float)(bx1 - bx0 + 1)};
        static Canvas cv;
        memset(&cv, 0, sizeof cv);
        int copy = 0;
        bool drawn = draw_glyph(&cv, b, code, kind, &copy);
        if (copy && index_of(copy) != 0xffff) {  // the game's glyph from another cell
            int sx0, sy0;
            cell_origin(f, index_of(copy), &sx0, &sy0);
            for (int y = 0; y < CELL; y++)
                memcpy(img + (uint64_t)sheet_y(f, y0, y) * w + x0, orig + (uint64_t)sheet_y(f, sy0, y) * w + sx0, CELL);
            changed++;
        } else if (drawn) {
            for (int y = 0; y < CELL; y++)
                for (int x = 0; x < CELL; x++)
                    img[(uint64_t)sheet_y(f, y0, y) * w + x0 + x] = (uint8_t)lroundf(cv.a[y * CELL + x] * 255);
            changed++;
        }
    }
    if (changed) bc4_encode(img, wb, hb, lin);
    static int nlog;
    if (nlog++ < 16)
        LOG("[glyphs] button glyph sheet %llx: %s icons, %d glyphs redrawn\n", (unsigned long long)addr,
            k_kind_names[kind], changed);
    if (getenv("SAVAGEWIND_GLYPH_DUMP")) {
        static int n;
        char path[128];
        snprintf(path, sizeof path, "build/glyph_sheet_%d.pgm", ++n);
        FILE *fp = n <= 16 ? fopen(path, "wb") : NULL;
        if (fp) {  // (as stored: bottom-up) and the original before it
            fprintf(fp, "P5\n%u %u\n255\n", w * 2, h);
            for (uint32_t y = 0; y < h; y++) {
                fwrite(orig + (uint64_t)y * w, 1, w, fp);
                fwrite(img + (uint64_t)y * w, 1, w, fp);
            }
            fclose(fp);
        }
    }
    free(orig);
    free(img);
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

bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row) {
    if (format != FMT_R8 || w > 2 * CELL || h > 2 * CELL || row < w) return false;
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
    if (!li) return false;
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
    if (bx1 < 0) return true;
    Box b = {ox + (bx0 + bx1 + 1) * k / 2, oy + (by0 + by1 + 1) * k / 2, (by1 - by0 + 1) * k, (bx1 - bx0 + 1) * k};
    static Canvas cv;
    memset(&cv, 0, sizeof cv);
    int kind = resolve_kind(), copy = 0;
    bool drawn = draw_glyph(&cv, b, li->code, kind, &copy);  // (Nintendo: the game's own, even with other bindings)
    if (drawn)
        for (int y = 0; y < (int)h; y++)
            for (int x = 0; x < (int)w; x++) {  // (bilinear from the canvas)
                float fx = clampf(ox + (x + 0.5f) * k - 0.5f, 0, CELL - 1), fy = clampf(oy + (y + 0.5f) * k - 0.5f, 0, CELL - 1);
                int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < CELL ? x0 + 1 : x0, y1 = y0 + 1 < CELL ? y0 + 1 : y0;
                float tx = fx - x0, ty = fy - y0;
                float a = (cv.a[y0 * CELL + x0] * (1 - tx) + cv.a[y0 * CELL + x1] * tx) * (1 - ty) +
                          (cv.a[y1 * CELL + x0] * (1 - tx) + cv.a[y1 * CELL + x1] * tx) * ty;
                lin[(uint64_t)y * row + x] = (uint8_t)lroundf(a * 255);
            }
    static int nlog;
    if (nlog++ < 16) LOG("[glyphs] layout icon %04X (%ux%u): %s icons%s\n", li->code, w, h, k_kind_names[kind], drawn ? "" : ", kept");
    return true;
}
