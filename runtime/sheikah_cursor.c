// The mouse pointer of Savage Wind's windows (the F1 menu, Savage Wind Configuration, the start screen) in place of the
// system's arrow: a small slate-dark arrow with a cyan edge, a soft cyan glow and a line along its left side, drawn
// here (no game art). About 20 pixels tall at 100% display scale, larger with the display's scale.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdbool.h>

#include "sheikah_cursor.h"

// The arrow, in 100%-scale pixels from its tip (the hot spot)
static const float k_arrow[][2] = {{0, 0}, {0, 16.5f}, {4.4f, 12.6f}, {7.4f, 19.4f}, {10.1f, 18.2f},
                                   {7.2f, 11.6f}, {12.6f, 11.6f}};
#define NPTS ((int)(sizeof k_arrow / sizeof *k_arrow))

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float x = ax + t * dx - px, y = ay + t * dy - py;
    return sqrtf(x * x + y * y);
}

// Signed distance to the arrow's outline: negative inside
static float arrow_dist(float x, float y) {
    float d = 1e9f;
    bool in = false;
    for (int i = 0, j = NPTS - 1; i < NPTS; j = i++) {
        float ax = k_arrow[j][0], ay = k_arrow[j][1], bx = k_arrow[i][0], by = k_arrow[i][1];
        float e = seg_dist(x, y, ax, ay, bx, by);
        if (e < d) d = e;
        if ((by > y) != (ay > y) && x < (ax - bx) * (y - by) / (ay - by) + bx) in = !in;
    }
    return in ? -d : d;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Premultiplied "over" of a colour with coverage a
static void over(float *r, float *g, float *b, float *a, float cr, float cg, float cb, float ca) {
    *r = cr * ca + *r * (1 - ca), *g = cg * ca + *g * (1 - ca), *b = cb * ca + *b * (1 - ca), *a = ca + *a * (1 - ca);
}

SDL_Surface *sheikah_cursor_surface(float scale, int *hot_out) {
    if (!(scale >= 1)) scale = 1;
    if (scale > 3) scale = 3;
    const float pad = 3;  // (room for the glow)
    int w = (int)ceilf((13 + 2 * pad) * scale), h = (int)ceilf((20 + 2 * pad) * scale);
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    const float edge = 0.75f, glow = 2.6f;  // (half the edge's width; how far the glow reaches, 100%-scale pixels)
    const float cyan[3] = {0.37f, 0.91f, 1.0f}, slate[3] = {0.06f, 0.11f, 0.14f};
    for (int py = 0; py < h; py++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + py * s->pitch);
        for (int px = 0; px < w; px++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {  // (4x4 samples a pixel)
                    float x = (px + (sx + .5f) / 4) / scale - pad, y = (py + (sy + .5f) / 4) / scale - pad;
                    float d = arrow_dist(x, y), cr = 0, cg = 0, cb = 0, ca = 0;
                    if (d > edge) {  // the glow outside
                        float k = 1 - clamp01((d - edge) / glow);
                        over(&cr, &cg, &cb, &ca, cyan[0], cyan[1], cyan[2], 0.45f * k * k);
                    } else if (d > -edge) {  // the edge
                        over(&cr, &cg, &cb, &ca, cyan[0], cyan[1], cyan[2], 1);
                    } else {  // the body, lighter toward the tip, and a line along the left side
                        float t = clamp01(y / 19);
                        over(&cr, &cg, &cb, &ca, slate[0] + 0.05f * (1 - t), slate[1] + 0.07f * (1 - t),
                             slate[2] + 0.09f * (1 - t), 0.93f);
                        float l = seg_dist(x, y, 2.3f, 5.2f, 2.3f, 12.4f);
                        if (l < 0.55f) over(&cr, &cg, &cb, &ca, cyan[0], cyan[1], cyan[2], 0.85f * (1 - l / 0.55f));
                    }
                    r += cr, g += cg, b += cb, a += ca;
                }
            r /= 16, g /= 16, b /= 16, a /= 16;
            Uint8 A = (Uint8)lroundf(a * 255);  // (premultiplied above: back to straight colour)
            Uint8 R = a > 0 ? (Uint8)lroundf(clamp01(r / a) * 255) : 0, G = a > 0 ? (Uint8)lroundf(clamp01(g / a) * 255) : 0;
            Uint8 B = a > 0 ? (Uint8)lroundf(clamp01(b / a) * 255) : 0;
            row[px] = (Uint32)A << 24 | (Uint32)R << 16 | (Uint32)G << 8 | B;
        }
    }
    *hot_out = (int)lroundf(pad * scale);
    return s;
}

SDL_Cursor *sheikah_cursor_create(float scale) {
    int hot;
    SDL_Surface *s = sheikah_cursor_surface(scale, &hot);
    if (!s) return NULL;
    SDL_Cursor *c = SDL_CreateColorCursor(s, hot, hot);
    SDL_DestroySurface(s);
    return c;
}

static SDL_Cursor *g_cursor;
static float g_cursor_scale;

SDL_Cursor *sheikah_cursor(SDL_Window *w) {
    float scale = w ? SDL_GetWindowDisplayScale(w) : 1;
    if (!(scale > 0)) scale = 1;
    if (!g_cursor || fabsf(scale - g_cursor_scale) > 0.01f) {  // (the window went to a display of another scale)
        SDL_Cursor *c = sheikah_cursor_create(scale);
        if (!c) return g_cursor;
        if (g_cursor && SDL_GetCursor() == g_cursor) SDL_SetCursor(c);
        if (g_cursor) SDL_DestroyCursor(g_cursor);
        g_cursor = c, g_cursor_scale = scale;
    }
    return g_cursor;
}

void sheikah_cursor_use(SDL_Window *w) {
    SDL_Cursor *c = sheikah_cursor(w);
    if (c && SDL_GetCursor() != c) SDL_SetCursor(c);
}
