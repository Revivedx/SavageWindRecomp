// Gameplay > Sharper text: the game's bitmap font sheets, enlarged on the player's machine.
//
// BOTW's Latin, Cyrillic and Japanese text comes from bitmap fonts (Normal_00.bffnt and the like: BC4 sheets of up to
// 1024x1024 with cells of 18x22 to 91x110), made for 720p and 1080p. At 1440p and 4K their letters are stretched, and text
// looks soft next to everything else. There are no outlines to draw them from, so a sheet is enlarged: nothing here
// comes from anywhere but the sheet (no neural network, no other font), and nothing of Nintendo's is shipped.
//
//   1. Catmull-Rom interpolation of the coverage: a smooth surface through the original texels.
//   2. The edge stays where that surface crosses half of the local peak, the place the original's antialiasing puts
//      it. Half of the *local* peak, because a stroke thinner than a texel never reaches full coverage, and a fixed
//      threshold would thin it or lose it. The letters' weight stays (at 4x their total coverage changes by 0.1% in
//      Normal_00 and Special_00, 2.7% in the small Caption_00).
//   3. The ramp across the edge is narrowed to 0.6 of a source texel. Not further: a ramp of about one new texel looks
//      sharper in a still picture, but it rounds corners into blobs, and the letters look painted.
//
// NormalS_00 only gets step 1: its R8 texels aren't coverage (they gather at 0, 128 and 255) and the game's shader
// reads them in its own way. Edges drawn into them made its letters fat and notched ("Start", "Cancel").
//
// An enlarged sheet gets mip levels and a sampler that uses them (textures.c): text is often drawn smaller than it, and
// then its narrow edges alias into stair steps on every diagonal.
//
// The sheets are told from other textures by their contents: fs.c hashes the ones in the game's own font archives.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "text_upscale.h"

#define MAX_SCALE 4
#define EDGE_HALF_RAMP 0.3f  // half the ramp across an edge, in source texels: the coverage it spans around 0.5
#define MAX_SHEETS 64

static uint64_t sheets[MAX_SHEETS];
static int nsheets;

// (the whole sheet: the first rows of one can be empty, and the same as any other texture's)
static uint64_t sheet_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void text_sheet_add(const uint8_t *sheet, size_t bytes) {
    if (nsheets < MAX_SHEETS) sheets[nsheets++] = sheet_hash(sheet, bytes);
}

int text_sheet_count(void) { return nsheets; }

bool text_sheet_known(const uint8_t *texels, size_t bytes) {
    if (!nsheets) return false;
    uint64_t h = sheet_hash(texels, bytes);
    for (int i = 0; i < nsheets; i++)
        if (sheets[i] == h) return true;
    return false;
}

static void catmull_weights(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1;
    w[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool text_upscale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t scale, bool edges, uint8_t *dst) {
    if (scale < 2 || scale > MAX_SCALE || !w || !h) return false;
    uint32_t dw = w * scale;
    float *row = malloc(sizeof(float) * (size_t)dw * h);  // horizontally interpolated, one row per source row
    uint8_t *peak = malloc((size_t)w * h);                // the highest coverage around each source texel
    if (!row || !peak) { free(row); free(peak); return false; }

    float wt[MAX_SCALE][4];
    int shift[MAX_SCALE];
    for (uint32_t s = 0; s < scale; s++) {
        float p = (s + 0.5f) / scale - 0.5f;  // (texel centres of the new size, in source texels)
        shift[s] = (int)floorf(p);
        catmull_weights(p - floorf(p), wt[s]);
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *in = src + (size_t)y * w;
        float *out = row + (size_t)y * dw;
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t s = 0; s < scale; s++) {
                int b = (int)x + shift[s];
                const float *k = wt[s];
                out[x * scale + s] = k[0] * in[clampi(b - 1, 0, (int)w - 1)] + k[1] * in[clampi(b, 0, (int)w - 1)] +
                                     k[2] * in[clampi(b + 1, 0, (int)w - 1)] + k[3] * in[clampi(b + 2, 0, (int)w - 1)];
            }
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    uint8_t v = src[(size_t)clampi((int)y + j, 0, (int)h - 1) * w + clampi((int)x + i, 0, (int)w - 1)];
                    if (v > m) m = v;
                }
            peak[(size_t)y * w + x] = m;
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t s = 0; s < scale; s++) {
            int b = (int)y + shift[s];
            const float *k = wt[s];
            const float *r0 = row + (size_t)clampi(b - 1, 0, (int)h - 1) * dw, *r1 = row + (size_t)clampi(b, 0, (int)h - 1) * dw,
                        *r2 = row + (size_t)clampi(b + 1, 0, (int)h - 1) * dw, *r3 = row + (size_t)clampi(b + 2, 0, (int)h - 1) * dw;
            uint8_t *out = dst + (size_t)(y * scale + s) * dw;
            for (uint32_t x = 0; x < dw; x++) {
                float v = k[0] * r0[x] + k[1] * r1[x] + k[2] * r2[x] + k[3] * r3[x];
                if (!edges) {
                    out[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
                    continue;
                }
                float m = peak[(size_t)y * w + x / scale];
                if (m < 8) {  // (the empty padding between cells stays empty)
                    out[x] = 0;
                    continue;
                }
                float t = (v / m - (0.5f - EDGE_HALF_RAMP)) / (2 * EDGE_HALF_RAMP);  // 0 outside the edge, 1 inside
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                out[x] = (uint8_t)(m * (t * t * (3 - 2 * t)) + 0.5f);
            }
        }
    free(row);
    free(peak);
    return true;
}
