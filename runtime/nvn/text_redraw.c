// Gameplay > Sharper text: the game's font sheets drawn again from outlines made out of their own glyphs.
//
// The enlarged sheet (text_upscale.c) is the game's bitmap made bigger, and its letters stay about as soft as the bitmap
// is. Here every glyph of the sheet gets an outline made from the game's own texels, on the player's machine when the
// sheet is uploaded (textures.c upload_text_sheet), and is drawn again from it into its cell of the enlarged sheet, in
// place: the game's texture coordinates and widths stay. Nothing but the sheet goes in: no other font, nothing shipped.
//
// Per glyph, in its cell with PAD empty texels around it:
//   1. Traced: its coverage (NormalS_00: its fill), interpolated ZOOM times with cubic B-splines and divided by the local
//      peak of the coverage across its strokes (a stroke thinner than a texel never reaches full coverage, and a fixed
//      threshold would thin it or lose it); the outline where that crosses 0.5 (marching squares), lightly cleaned up
//      (TRACE_*: corners, smoothing between them, straight runs) and resampled every FIT_SPACING texel.
//   2. Fitted to the game's texels: the outline's points moved until, filled with its exact coverage at the sheet's own
//      size, it gives the glyph's texels (least squares), with a bending penalty that is quadratic for small bends and
//      linear for large ones (FIT_LAM, FIT_KINK: smooth curves, corners kept) and even spacing (FIT_MU). The tracing is
//      only as good as the interpolation (Caption_00 is off by a tenth of its ink); the fit follows the game's shapes.
//   3. Cleaned up by general rules, no glyph's own data: corners where it turns more than TIDY_CORNER_DEG; a run between
//      corners within TIDY_STRAIGHT of a line becomes one (snapped to horizontal or vertical within TIDY_SNAP_DEG), the
//      others are smoothed (by font, smooth_of); each corner rebuilt where its sides meet. The fit reproduces the
//      bitmap's hinting and its BC4 noise too, which show at 4x as lumpy outlines.
//   4. Its ink restored: every edge moved out (or in) evenly until the glyph has as much ink as the game's. The clean-up
//      thins small marks: a comma's tail.
//   5. Filled with its exact coverage at the enlarged size; whatever reaches past the cell is left out (the game's quads
//      reach a little past their cells, and its neighbours' would show it).
// The settings of 2 and 3 were tuned in a prototype against the game's sheets.
//
// 1 to 4 take a few ms a glyph and the sheets hold thousands, so they are done once: the outlines are kept in
// <cache>/text/<hash>.glyphs, the hash taken over the sheet's texels and the settings above (a game update that changes
// a sheet, or new settings, makes new ones). Until a sheet has them, it shows as enlarged: they are made on a thread of
// their own (a sheet at a time, on every core at low priority: some seconds a sheet), and once they are the sheet goes
// up again (textures.c, text_redraw_generation) and they are filled in (5). Later launches only read and fill them.
// A cell without an outline is cleared when it holds next to nothing (BC4 noise) and otherwise left as enlarged.
//
// NormalS_00 is an outlined font: its texels are 128 * ring + 127 * fill (0, 128 and 255, with ramps between), and the
// game's shader shows the fill alone in the menus. The fill is fitted and drawn; the ring reaches OUTLINE_RING beyond
// it, from each pixel's distance to the fill.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "../hash.h"
#include "../rt.h"
#include "text_redraw.h"

#define PAD 3               // empty texels around a cell, for the interpolation and the smoothing
#define ZOOM 8              // contour grid points per texel
#define PEAK_FLOOR 0.35f    // (coverage under this is no stroke: BC4 noise around a glyph)
#define PEAK_EMPTY 0.05f
#define MIN_INK 0.5f        // (less glyph coverage in a cell: nothing to trace; less in all, a ring too: empty, cleared)
#define MAX_WORKERS 32

// 1: the traced outline's clean-up, before the fit
#define TRACE_STEP 0.04f          // outline points, texels apart
#define TRACE_CORNER_DEG 55.0f
#define TRACE_CORNER_REACH 0.5f
#define TRACE_SMOOTH 0.35f        // Gaussian sigma, texels
#define TRACE_PIN 0.25f           // the smoothing fades in over this much from a corner, which stays where it is
#define TRACE_STRAIGHT 0.10f      // a run between corners this close to its chord is one
#define TRACE_STRAIGHT_MIN 0.6f
#define TANGENT_NEAR 0.35f        // a corner's sides: from this far from it ...
#define TANGENT_FAR 1.0f          // ... to this far
#define CORNER_MOVE 0.6f          // (further than this, the tangents meet somewhere else: a corner of a narrow stroke)
// 2: the fit
#define FIT_SPACING 0.25f   // its points, texels apart
#define FIT_ITERS 400
#define FIT_STEP 0.05f      // the first step: the point that moves most, this far (texels)
#define FIT_SAMPLES 6       // residual samples along each edge
#define FIT_LAM 0.2f
#define FIT_KINK 0.02f
#define FIT_MU 0.2f
// 3: the clean-up
#define TIDY_STEP 0.05f
#define TIDY_REACH 0.45f          // a corner's turn: over this far either side
#define TIDY_CORNER_DEG 30.0f
#define TIDY_STRAIGHT 0.08f       // (per 3 texels of length, past 3)
#define TIDY_STRAIGHT_MIN 0.3f
#define TIDY_SNAP_DEG 4.0f
#define TIDY_PIN 0.3f
#define TIDY_SIDE 0.6f            // a curve's end tangent: over this much of it
#define TIDY_CORNER_MOVE 0.5f
// 4: the ink
#define INK_REACH 0.3f      // texels a side, at most ...
#define INK_REACH_MAX 1.0f  // ... unless that is not enough (small marks)
#define INK_STEPS 14
#define SIMPLIFY 0.01f      // the outlines kept: points this close to the line between their neighbours are dropped

// NormalS_00's ring beyond its fill (sheet pixels; measured on Font_US's sheet: within 2% of the game's)
#define OUTLINE_RING 2.01f

// The clean-up's smoothing, by font (texels): the outlined font's strokes are thinner than they look (its fill), the
// captions' are its size
static float smooth_of(int font) { return font == TEXT_FONT_NORMAL ? 0.5f : 1.0f; }

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---------------------------------------------------------------- distances (Felzenszwalb & Huttenlocher)
static void edt_1d(const float *f, float *d, int n, int *v, float *z) {
    int k = 0;
    v[0] = 0, z[0] = -1e30f, z[1] = 1e30f;
    for (int q = 1; q < n; q++) {
        float s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (q - v[k]));
        while (s <= z[k]) {
            k--;
            s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (q - v[k]));
        }
        k++, v[k] = q, z[k] = s, z[k + 1] = 1e30f;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = (float)(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

// Squared distance from every texel to the nearest one with set[] true (w x h). tmp: 3 * max(w, h) + 1 floats and as
// many ints.
static void edt(const bool *set, int w, int h, float *out, float *tmp, int *v) {
    int n = w > h ? w : h;
    float *f = tmp, *d = tmp + n, *z = tmp + 2 * n;
    for (int i = 0; i < w * h; i++) out[i] = set[i] ? 0 : 1e20f;
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) f[y] = out[y * w + x];
        edt_1d(f, d, h, v, z);
        for (int y = 0; y < h; y++) out[y * w + x] = d[y];
    }
    for (int y = 0; y < h; y++) {
        edt_1d(out + y * w, d, w, v, z);
        memcpy(out + y * w, d, sizeof(float) * w);
    }
}

// ---------------------------------------------------------------- filling
// Polygons filled with their exact coverage. Each edge adds, row by row, the signed area it bounds to its right into
// acc ((w + 2) a row), and a running sum along the row gives each pixel's coverage, |winding| counted (the holes'
// outlines run the other way round from the ones around them).
static inline float ramp_area(float u) { return u <= 0 ? 0 : u < 1 ? 0.5f * u * u : u - 0.5f; }

static void fill_edge(float *acc, int w, int h, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1;
    if (y0 > y1) {
        float tx = x0, ty = y0;
        x0 = x1, y0 = y1, x1 = tx, y1 = ty, dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    int ya = (int)floorf(y0), yb = (int)ceilf(y1);
    if (ya < 0) ya = 0;
    if (yb > h) yb = h;
    for (int y = ya; y < yb; y++) {
        float t0 = (float)y > y0 ? (float)y : y0, t1 = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        if (t1 <= t0) continue;
        float xa = x0 + (t0 - y0) * dxdy, xb = x0 + (t1 - y0) * dxdy, d = (t1 - t0) * dir;
        if (xa > xb) { float t = xa; xa = xb, xb = t; }
        xa = xa < 0 ? 0 : xa > w ? (float)w : xa, xb = xb < 0 ? 0 : xb > w ? (float)w : xb;
        float *row = acc + (size_t)y * (w + 2), prev = 0, span = xb - xa;
        int i0 = (int)xa, i1 = (int)xb + 1;
        for (int i = i0; i <= i1; i++) {  // (the share of this piece left of pixel i's right edge, cumulative)
            float c = (float)(i + 1);
            float F = span < 1e-3f ? clamp01(c - 0.5f * (xa + xb)) : (ramp_area(c - xa) - ramp_area(c - xb)) / span;
            row[i] += d * F - prev, prev = d * F;
        }
    }
}

// ---------------------------------------------------------------- outlines
// A glyph's closed outlines: count[k] points (x, y pairs, texels of the padded cell) each, one after the other
typedef struct {
    float *pts;
    int *count, n, npts, cap_pts, cap_n;
} Shape;

static void *grow_to(void *p, int *cap, int need, size_t el) {
    if (need <= *cap) return p;
    while (*cap < need) *cap = *cap ? *cap * 2 : 1024;
    return realloc(p, el * (size_t)*cap);
}

static void shape_add(Shape *sh, const float *p, int n) {
    sh->pts = grow_to(sh->pts, &sh->cap_pts, sh->npts + n, 2 * sizeof(float));
    sh->count = grow_to(sh->count, &sh->cap_n, sh->n + 1, sizeof(int));
    memcpy(sh->pts + 2 * sh->npts, p, sizeof(float) * 2 * n);
    sh->npts += n, sh->count[sh->n++] = n;
}

static void shape_free(Shape *sh) { free(sh->pts), free(sh->count); }

// p: n points of a closed outline, resampled evenly about every `spacing` (at least 8 points) into *dst (its capacity
// *cap points, grown). Returns how many (0: it has next to no length, a fitted outline drawn together into a point);
// *step: how far apart they are.
static int resample(const float *p, int n, float spacing, float **dst, int *cap, float *step) {
    float len = 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        len += hypotf(p[2 * j] - p[2 * i], p[2 * j + 1] - p[2 * i + 1]);
    }
    if (len < 1e-3f) return 0;
    int m = (int)(len / spacing);
    if (m < 8) m = 8;
    *step = len / m;
    *dst = grow_to(*dst, cap, m + 1, 2 * sizeof(float));
    float *r = *dst, at = 0;
    for (int i = 0, j = 0; i < m; i++) {
        float want = i * *step, d;
        for (;;) {
            int k = j + 1 < n ? j + 1 : 0;
            d = hypotf(p[2 * k] - p[2 * j], p[2 * k + 1] - p[2 * j + 1]);
            if (j < n - 1 && at + d <= want) at += d, j++;
            else break;
        }
        int k = j + 1 < n ? j + 1 : 0;
        float u = d > 0 ? (want - at) / d : 0;
        r[2 * i] = p[2 * j] + (p[2 * k] - p[2 * j]) * u, r[2 * i + 1] = p[2 * j + 1] + (p[2 * k + 1] - p[2 * j + 1]) * u;
    }
    return m;
}

// ---------------------------------------------------------------- one glyph's work
// Sized for its sheet's cells: W x H texels with the padding, GW x GH contour grid points
typedef struct {
    int W, H, GW, GH;
    int *bx, *by, *px0, *px1, *py0, *py1;      // per grid column / row: its B-spline taps, its peak's two texels
    float *wx, *wy, *fx, *fy;                  // ... and their weights
    float *cov, *peak, *coef, *line, *hz, *pkh, *f;  // coverage; its 3x3 peak; B-spline coefficients ((W+4) x (H+4))
    uint8_t *in;                               // f > 0 at each grid point
    int *next, *touched, ntouched;             // marching squares: grid edge -> the next one along the outline
    float *loop;                               // one outline as traced
    int cap_loop;
    // one outline being cleaned up (cap points each, and one more): resampled, its turning, its points as cleaned,
    // a run between corners, that run smoothed (x, then y) and the line being smoothed with its ends
    float *rs, *ang, *out, *seg, *sx, *sy, *spad;
    int *corner, cap, cap_rs;
    // runs between corners (cap_runs): line or curve, first point, points, and a line's centre and direction
    int *run_line, *run_a, *run_n, cap_runs;
    float *run_c;
    Shape traced, tidy, done;                  // 1 and 2 (fitted in place), 3 and 4, the outlines kept
    // the fit: the residual (W x H), its row sums, two sets of points and their gradients, the bends' clipped
    // second differences; per outline its outward side and its edges' first mean length; normals per point
    float *res, *racc, *x0, *x1, *g0, *g1, *hg, *side, *len0, *nrm;
    int cap_fit, cap_side;
    float *acc;                                // the fill's area sums, (W * s + 2) x (H * s)
    uint8_t *canvas;                           // the glyph at the enlarged size, with the padding
    // NormalS_00's ring: each pixel's squared distance to the fill, and the distance transform's room
    bool *inside;
    float *dout, *etmp;
    int *ev;
} Tracer;

static inline void bspline_weights(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t, u = 1 - t;
    w[0] = u * u * u / 6, w[1] = (4 - 6 * t2 + 3 * t3) / 6, w[2] = (1 + 3 * t + 3 * t2 - 3 * t3) / 6, w[3] = t3 / 6;
}

// Where grid point g samples the texels (n of them, along one axis): its B-spline's first tap (coefficient index, they
// start 2 before the texels) and weights, and the two texels (clamped) and weight of its peak
static void grid_axis(int n, int g_count, int *b, float *w, int *p0, int *p1, float *pf) {
    for (int g = 0; g < g_count; g++) {
        float u = (g + 0.5f) / ZOOM - 0.5f;  // (grid points are samples, ZOOM to a texel)
        int j = (int)floorf(u);
        b[g] = j + 1;
        bspline_weights(u - j, w + 4 * g);
        p0[g] = j < 0 ? 0 : j, p1[g] = j + 1 >= n ? n - 1 : j + 1, pf[g] = u - j;
    }
}

// s: 0, room for making outlines (1 to 4); else for filling them at that scale (5)
static void tracer_init(Tracer *t, int cw, int ch, int s) {
    memset(t, 0, sizeof *t);
    t->W = cw + 2 * PAD, t->H = ch + 2 * PAD, t->GW = t->W * ZOOM, t->GH = t->H * ZOOM;
    if (s) {
        int CW = t->W * s, CH = t->H * s, cn = CW * CH, en = CW > CH ? CW : CH;
        t->acc = malloc(sizeof(float) * (CW + 2) * CH), t->canvas = malloc((size_t)cn);
        t->inside = malloc((size_t)cn), t->dout = malloc(sizeof(float) * cn);
        t->etmp = malloc(sizeof(float) * (3 * en + 1)), t->ev = malloc(sizeof(int) * en);
        return;
    }
    int n = t->W * t->H, g = t->GW * t->GH, L = (t->W > t->H ? t->W : t->H) + 4 + 24;
    t->bx = malloc(sizeof(int) * t->GW), t->px0 = malloc(sizeof(int) * t->GW), t->px1 = malloc(sizeof(int) * t->GW);
    t->wx = malloc(sizeof(float) * 4 * t->GW), t->fx = malloc(sizeof(float) * t->GW);
    t->by = malloc(sizeof(int) * t->GH), t->py0 = malloc(sizeof(int) * t->GH), t->py1 = malloc(sizeof(int) * t->GH);
    t->wy = malloc(sizeof(float) * 4 * t->GH), t->fy = malloc(sizeof(float) * t->GH);
    grid_axis(t->W, t->GW, t->bx, t->wx, t->px0, t->px1, t->fx);
    grid_axis(t->H, t->GH, t->by, t->wy, t->py0, t->py1, t->fy);
    t->cov = malloc(sizeof(float) * n), t->peak = malloc(sizeof(float) * n);
    t->coef = malloc(sizeof(float) * (t->W + 4) * (t->H + 4)), t->line = malloc(sizeof(float) * L);
    t->hz = malloc(sizeof(float) * (t->H + 4) * t->GW), t->pkh = malloc(sizeof(float) * t->H * t->GW);
    t->f = malloc(sizeof(float) * g), t->in = malloc((size_t)g);
    t->next = malloc(sizeof(int) * 2 * g), t->touched = malloc(sizeof(int) * 2 * g);
    for (int i = 0; i < 2 * g; i++) t->next[i] = -1;
    t->res = malloc(sizeof(float) * n), t->racc = malloc(sizeof(float) * (t->W + 2) * t->H);
}

static void tracer_free(Tracer *t) {
    free(t->bx), free(t->px0), free(t->px1), free(t->wx), free(t->fx);
    free(t->by), free(t->py0), free(t->py1), free(t->wy), free(t->fy);
    free(t->cov), free(t->peak), free(t->coef), free(t->line), free(t->hz), free(t->pkh), free(t->f), free(t->in);
    free(t->next), free(t->touched), free(t->loop);
    free(t->rs), free(t->ang), free(t->out), free(t->seg), free(t->corner), free(t->sx), free(t->sy), free(t->spad);
    free(t->run_line), free(t->run_a), free(t->run_n), free(t->run_c);
    shape_free(&t->traced), shape_free(&t->tidy), shape_free(&t->done);
    free(t->res), free(t->racc), free(t->x0), free(t->x1), free(t->g0), free(t->g1), free(t->hg), free(t->side);
    free(t->len0), free(t->nrm);
    free(t->acc), free(t->canvas);
    free(t->inside), free(t->dout), free(t->etmp), free(t->ev);
}

// Room for an outline of m points being cleaned up (a run between corners can be m + 1 long)
static void room_for(Tracer *t, int m) {
    if (m + 1 <= t->cap) return;
    int c = t->cap;
    while (c < m + 1) c = c ? c * 2 : 1024;
    t->cap = c;
    t->ang = realloc(t->ang, sizeof(float) * c), t->out = realloc(t->out, 2 * sizeof(float) * c);
    t->seg = realloc(t->seg, 2 * sizeof(float) * c), t->corner = realloc(t->corner, sizeof(int) * c);
    t->sx = realloc(t->sx, sizeof(float) * c), t->sy = realloc(t->sy, sizeof(float) * c);
    t->spad = realloc(t->spad, sizeof(float) * (c + 2 * 128));
}

// Cubic B-spline coefficients of one line of n values, in place: the interpolation through them passes through the
// values. The line has 12 zeros on either side, so that its ends see what an endless line would.
static void bspline_line(float *c, int n) {
    const float z = -0.267949192f;  // sqrt(3) - 2
    for (int i = 0; i < n; i++) c[i] *= 6;
    for (int i = 1; i < n; i++) c[i] += z * c[i - 1];
    c[n - 1] = z / (z * z - 1) * (c[n - 1] + z * c[n - 2]);
    for (int i = n - 2; i >= 0; i--) c[i] = z * (c[i + 1] - c[i]);
}

static void gauss_kernel(float sigma, float *k, int *r) {
    *r = (int)(4 * sigma + 0.5f);
    float sum = 0;
    for (int i = -*r; i <= *r; i++) sum += k[i + *r] = expf(-0.5f * i * i / (sigma * sigma));
    for (int i = 0; i <= 2 * *r; i++) k[i] /= sum;
}

static inline int wrap(int i, int m) { return ((i % m) + m) % m; }

// One coordinate of n points (src: x, y pairs) smoothed by the Gaussian k (2 * kr + 1 taps) into dst, beyond the ends
// the ends repeated, or (closed) the outline going round. pad: n + 2 * kr. (Taps outside, points inside: it vectorises
// without reordering any sum.)
static void smooth_axis(const float *src, int n, bool closed, const float *k, int kr, float *pad, float *dst) {
    for (int j = 0; j < n + 2 * kr; j++) {
        int q = j - kr;
        q = closed ? wrap(q, n) : q < 0 ? 0 : q >= n ? n - 1 : q;
        pad[j] = src[2 * q];
    }
    memset(dst, 0, sizeof(float) * n);
    for (int i = 0; i <= 2 * kr; i++) {
        const float w = k[i], *p = pad + i;
        for (int j = 0; j < n; j++) dst[j] += w * p[j];
    }
}

// Turning at each of m points (closed), degrees, over k points either side, into t->ang; the corners (where it turns
// most within k, and more than deg) into t->corner. Returns how many.
static int find_corners(Tracer *t, const float *r, int m, int k, float deg) {
    int nc = 0;
    for (int i = 0; i < m; i++) {
        int a = wrap(i - k, m), b = wrap(i + k, m);
        float v1x = r[2 * i] - r[2 * a], v1y = r[2 * i + 1] - r[2 * a + 1], v2x = r[2 * b] - r[2 * i], v2y = r[2 * b + 1] - r[2 * i + 1];
        t->ang[i] = fabsf(atan2f(v1x * v2y - v1y * v2x, v1x * v2x + v1y * v2y)) * 57.2957795f;
    }
    for (int i = 0; i < m; i++) {
        if (t->ang[i] <= deg) continue;
        bool top = true;
        for (int j = -k; j <= k && top; j++) top = t->ang[wrap(i + j, m)] <= t->ang[i];
        if (top) t->corner[nc++] = i;
    }
    return nc;
}

// A smoothing kernel for sigma texels on points step apart (kern: 256 taps)
static void step_kernel(float sigma, float step, float *kern, int *kr) {
    float sig = sigma / step;
    if (sig > 31) sig = 31;  // (kern's and spad's size)
    gauss_kernel(sig, kern, kr);
}

// 1: one traced outline (t->loop: n points, closed, texels) cleaned up, resampled every FIT_SPACING, into t->traced
static void clean(Tracer *t, int n) {
    float step;
    int m = resample(t->loop, n, TRACE_STEP, &t->rs, &t->cap_rs, &step);
    if (!m) return;
    room_for(t, m);
    const float *r = t->rs;
    // corners: where it turns most, and more than TRACE_CORNER_DEG, within TRACE_CORNER_REACH either side
    int k = (int)(TRACE_CORNER_REACH / step);
    if (k < 2) k = 2;
    int nc = find_corners(t, r, m, k, TRACE_CORNER_DEG);
    float kern[256];
    int kr;
    step_kernel(TRACE_SMOOTH, step, kern, &kr);
    float *out = t->out;
    if (!nc) {  // (no corner: smoothed all round)
        smooth_axis(r, m, true, kern, kr, t->spad, t->sx);
        smooth_axis(r + 1, m, true, kern, kr, t->spad, t->sy);
        for (int i = 0; i < m; i++) out[2 * i] = t->sx[i], out[2 * i + 1] = t->sy[i];
    } else {
        memcpy(out, r, sizeof(float) * 2 * m);
        for (int c = 0; c < nc; c++) {  // the runs between corners, each on its own, ends pinned
            int a = t->corner[c], b = c + 1 < nc ? t->corner[c + 1] : t->corner[0] + m, sl = b - a + 1;
            float *seg = t->seg;
            for (int j = 0; j < sl; j++) {
                int q = a + j < m ? a + j : a + j - m;
                seg[2 * j] = r[2 * q], seg[2 * j + 1] = r[2 * q + 1];
            }
            if (sl > 4) {
                smooth_axis(seg, sl, false, kern, kr, t->spad, t->sx);
                smooth_axis(seg + 1, sl, false, kern, kr, t->spad, t->sy);
                for (int j = 0; j < sl; j++) {
                    int e = j < sl - 1 - j ? j : sl - 1 - j;
                    float bl = clamp01(e * step / TRACE_PIN);
                    seg[2 * j] += (t->sx[j] - seg[2 * j]) * bl, seg[2 * j + 1] += (t->sy[j] - seg[2 * j + 1]) * bl;
                }
            }
            float dx = seg[2 * (sl - 1)] - seg[0], dy = seg[2 * (sl - 1) + 1] - seg[1], dl = hypotf(dx, dy);
            if (dl > TRACE_STRAIGHT_MIN) {
                float nx = -dy / dl, ny = dx / dl, dev = 0;
                for (int j = 0; j < sl; j++) {
                    float e = fabsf((seg[2 * j] - seg[0]) * nx + (seg[2 * j + 1] - seg[1]) * ny);
                    if (e > dev) dev = e;
                }
                if (dev < TRACE_STRAIGHT)
                    for (int j = 1; j < sl - 1; j++) {
                        float u = (float)j / (sl - 1);
                        seg[2 * j] = seg[0] + u * dx, seg[2 * j + 1] = seg[1] + u * dy;
                    }
            }
            for (int j = 1; j < sl - 1; j++) {
                int q = a + j < m ? a + j : a + j - m;
                out[2 * q] = seg[2 * j], out[2 * q + 1] = seg[2 * j + 1];
            }
        }
        // each corner where its sides' tangents meet
        int i0 = (int)(TANGENT_NEAR / step), i1 = (int)(TANGENT_FAR / step);
        for (int c = 0; c < nc && i1 - i0 >= 3; c++) {
            int ci = t->corner[c];
            const float *a0 = r + 2 * wrap(ci - i0, m), *a1 = r + 2 * wrap(ci - i1 + 1, m);
            const float *b0 = r + 2 * wrap(ci + i0, m), *b1 = r + 2 * wrap(ci + i1 - 1, m);
            float dax = a0[0] - a1[0], day = a0[1] - a1[1], dbx = b0[0] - b1[0], dby = b0[1] - b1[1];
            float det = -dax * dby + dbx * day;
            if (fabsf(det) < 1e-6f) continue;
            float rx = b1[0] - a1[0], ry = b1[1] - a1[1], u = (-rx * dby + dbx * ry) / det;
            float X = a1[0] + u * dax, Y = a1[1] + u * day;
            if (hypotf(X - r[2 * ci], Y - r[2 * ci + 1]) < CORNER_MOVE) out[2 * ci] = X, out[2 * ci + 1] = Y;
        }
    }
    // (for the fit: fewer points, each with room to move)
    float sp;
    int fm = resample(out, m, FIT_SPACING, &t->loop, &t->cap_loop, &sp);
    if (fm) shape_add(&t->traced, t->loop, fm);
}

// 1: the cell's glyph traced (t->cov holds its coverage, padded) into t->traced. false: no outline.
static bool trace_glyph(Tracer *t) {
    const int W = t->W, H = t->H, CW = W + 4;
    t->traced.n = t->traced.npts = 0;
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (t->cov[y * W + x] > 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return false;
    // what the coverage is divided by: the stroke's coverage across it, the highest within a texel. Across it: a texel's
    // own and its higher neighbour's, along the axis where that is less (along the stroke, the neighbour is the stroke
    // itself). A stroke a texel wide that falls between two texels is half in each, and their highest alone (0.5) made it
    // twice as wide as one that falls on one (Caption_00's "二", its kanji's strokes running together).
    float *across = t->hz;  // (free until the grid is made)
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float *c = t->cov + y * W + x;
            float l = x > 0 ? c[-1] : 0, r = x + 1 < W ? c[1] : 0, u = y > 0 ? c[-W] : 0, d = y + 1 < H ? c[W] : 0;
            float ph = *c + (l > r ? l : r), pv = *c + (u > d ? u : d), p = ph < pv ? ph : pv;
            across[y * W + x] = p > 1 ? 1 : p;
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float m = 0;
            for (int j = y - 1; j <= y + 1; j++)
                for (int i = x - 1; i <= x + 1; i++)
                    if (i >= 0 && i < W && j >= 0 && j < H && across[j * W + i] > m) m = across[j * W + i];
            t->peak[y * W + x] = m;
        }
    // B-spline coefficients, rows then columns (the coefficients reach 2 past the padding: the interpolation reads them)
    float *ln = t->line;
    memset(t->coef, 0, sizeof(float) * CW * (H + 4));
    for (int y = 0; y < H; y++) {
        memset(ln, 0, sizeof(float) * (W + 24));
        memcpy(ln + 12, t->cov + y * W, sizeof(float) * W);
        bspline_line(ln, W + 24);
        memcpy(t->coef + (y + 2) * CW, ln + 10, sizeof(float) * CW);
    }
    for (int x = 0; x < CW; x++) {
        memset(ln, 0, sizeof(float) * (H + 24));
        for (int y = 0; y < H; y++) ln[12 + y] = t->coef[(y + 2) * CW + x];
        bspline_line(ln, H + 24);
        for (int y = 0; y < H + 4; y++) t->coef[y * CW + x] = ln[10 + y];
    }
    // the grid, only around the ink (3 texels: the peak's reach and the interpolation's; the rest is outside), as
    // rows: every coefficient row and peak row interpolated across, then down
    int ex0 = x0 - 3 < 0 ? 0 : x0 - 3, ex1 = x1 + 3 >= W ? W - 1 : x1 + 3, ey0 = y0 - 3 < 0 ? 0 : y0 - 3,
        ey1 = y1 + 3 >= H ? H - 1 : y1 + 3;
    int gx0 = ex0 * ZOOM, gy0 = ey0 * ZOOM, RW = (ex1 + 1) * ZOOM - gx0, RH = (ey1 + 1) * ZOOM - gy0;
    for (int r = 0; r < H + 4; r++) {
        const float *c = t->coef + r * CW;
        float *o = t->hz + r * RW;
        for (int g = 0; g < RW; g++) {
            const float *w = t->wx + 4 * (gx0 + g), *cc = c + t->bx[gx0 + g];
            o[g] = w[0] * cc[0] + w[1] * cc[1] + w[2] * cc[2] + w[3] * cc[3];
        }
    }
    for (int r = 0; r < H; r++) {
        const float *pr = t->peak + r * W;
        float *o = t->pkh + r * RW;
        for (int g = 0; g < RW; g++) {
            float u = t->fx[gx0 + g];
            o[g] = pr[t->px0[gx0 + g]] * (1 - u) + pr[t->px1[gx0 + g]] * u;
        }
    }
    for (int gy = 0; gy < RH; gy++) {
        const float *wy = t->wy + 4 * (gy0 + gy), v = t->fy[gy0 + gy];
        const float *h0 = t->hz + t->by[gy0 + gy] * RW, *h1 = h0 + RW, *h2 = h1 + RW, *h3 = h2 + RW;
        const float *p0 = t->pkh + t->py0[gy0 + gy] * RW, *p1 = t->pkh + t->py1[gy0 + gy] * RW;
        float *f = t->f + gy * RW;
        uint8_t *in = t->in + gy * RW;
        for (int g = 0; g < RW; g++) {
            float up = wy[0] * h0[g] + wy[1] * h1[g] + wy[2] * h2[g] + wy[3] * h3[g];
            float pk = p0[g] * (1 - v) + p1[g] * v;
            float val = pk < PEAK_EMPTY ? -0.5f : up / (pk > PEAK_FLOOR ? pk : PEAK_FLOOR) - 0.5f;
            f[g] = val, in[g] = val > 0;
        }
    }
    // marching squares: grid edges where f crosses 0, linked with the inside on one side. Edge ids: (point * 2) for
    // the edge right of a grid point, + 1 for the one below it.
    static const signed char table[16][4] = {
        {-1}, {3, 2, -1}, {2, 1, -1}, {3, 1, -1}, {1, 0, -1}, {3, 0, 1, 2}, {2, 0, -1}, {3, 0, -1},
        {0, 3, -1}, {0, 2, -1}, {0, 1, 2, 3}, {0, 1, -1}, {1, 3, -1}, {1, 2, -1}, {2, 3, -1}, {-1}};
    const float *f = t->f;
    t->ntouched = 0;
    for (int y = 0; y < RH - 1; y++) {
        const uint8_t *a = t->in + y * RW, *b = a + RW;
        for (int x = 0; x < RW - 1; x++) {
            int k = a[x] << 3 | a[x + 1] << 2 | b[x + 1] << 1 | b[x];
            if (k == 0 || k == 15) continue;
            int id[4] = {(y * RW + x) * 2, (y * RW + x + 1) * 2 + 1, ((y + 1) * RW + x) * 2, (y * RW + x) * 2 + 1};
            for (int s = 0; s < 4 && table[k][s] >= 0; s += 2) {
                int e = id[(int)table[k][s]];
                t->next[e] = id[(int)table[k][s + 1]], t->touched[t->ntouched++] = e;
            }
        }
    }
    for (int i = 0; i < t->ntouched; i++) {
        int s = t->touched[i], e = s, n = 0;
        if (t->next[s] < 0) continue;  // (already on an outline)
        do {
            int v = e >> 1, gx = v % RW, gy = v / RW;
            float a = f[v], b = (e & 1) ? f[v + RW] : f[v + 1], u = a / (a - b);
            t->loop = grow_to(t->loop, &t->cap_loop, n + 1, 2 * sizeof(float));
            t->loop[2 * n] = (gx0 + gx + ((e & 1) ? 0 : u) + 0.5f) / ZOOM;  // (grid points are texel samples)
            t->loop[2 * n + 1] = (gy0 + gy + ((e & 1) ? u : 0) + 0.5f) / ZOOM;
            n++;
            int nx = t->next[e];
            t->next[e] = -1, e = nx;
        } while (e >= 0 && e != s);
        if (n >= 6) clean(t, n);
    }
    for (int i = 0; i < t->ntouched; i++) t->next[t->touched[i]] = -1;
    return t->traced.n > 0;
}

// ---------------------------------------------------------------- 2: the fit
// The outlines sh (their points at pts) filled at the sheet's size (the padded cell, W x H texels) into cov; returns
// its sum, the ink
static float raster_cov(Tracer *t, const Shape *sh, const float *pts, float *cov) {
    const int W = t->W, H = t->H;
    memset(t->racc, 0, sizeof(float) * (W + 2) * H);
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++])
        for (int i = 0, n = sh->count[k]; i < n; i++) {
            const float *a = pts + 2 * (o + i), *b = pts + 2 * (o + (i + 1 < n ? i + 1 : 0));
            fill_edge(t->racc, W, H, a[0], a[1], b[0], b[1]);
        }
    float ink = 0;
    for (int y = 0; y < H; y++) {
        const float *a = t->racc + (size_t)y * (W + 2);
        float sum = 0;
        for (int x = 0; x < W; x++) {
            sum += a[x];
            float c = fabsf(sum);
            c = c > 1 ? 1 : c;
            if (cov) cov[y * W + x] = c;
            ink += c;
        }
    }
    return ink;
}

// Each point's normal (unit; its neighbours' chord turned a quarter): nrm, 2 per point
static void normals(const Shape *sh, const float *pts, float *nrm) {
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++])
        for (int i = 0, n = sh->count[k]; i < n; i++) {
            const float *a = pts + 2 * (o + (i ? i - 1 : n - 1)), *b = pts + 2 * (o + (i + 1 < n ? i + 1 : 0));
            float qx = b[0] - a[0], qy = b[1] - a[1], l = hypotf(qx, qy) + 1e-12f;
            nrm[2 * (o + i)] = qy / l, nrm[2 * (o + i) + 1] = -qx / l;
        }
}

static void room_for_fit(Tracer *t, const Shape *sh) {
    int c = t->cap_fit;
    if (sh->npts > c) {
        while (c < sh->npts) c = c ? c * 2 : 1024;
        t->cap_fit = c;
        t->x0 = realloc(t->x0, 2 * sizeof(float) * c), t->x1 = realloc(t->x1, 2 * sizeof(float) * c);
        t->g0 = realloc(t->g0, 2 * sizeof(float) * c), t->g1 = realloc(t->g1, 2 * sizeof(float) * c);
        t->hg = realloc(t->hg, 2 * sizeof(float) * c), t->nrm = realloc(t->nrm, 2 * sizeof(float) * c);
    }
    if (sh->n > t->cap_side) {
        t->cap_side = sh->n * 2;
        t->side = realloc(t->side, sizeof(float) * t->cap_side), t->len0 = realloc(t->len0, sizeof(float) * t->cap_side);
    }
}

// Per outline, which way along its normals is out of the ink: moved that way, the glyph has more (with all the other
// outlines drawn: a hole's outward side is into the hole). Normals into t->nrm, sides into t->side. Uses t->x1.
static void outward_sides(Tracer *t, const Shape *sh) {
    normals(sh, sh->pts, t->nrm);
    float base = raster_cov(t, sh, sh->pts, NULL);
    memcpy(t->x1, sh->pts, sizeof(float) * 2 * sh->npts);
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++]) {
        int n = sh->count[k];
        for (int i = 2 * o; i < 2 * (o + n); i++) t->x1[i] = sh->pts[i] + 0.05f * t->nrm[i];
        t->side[k] = raster_cov(t, sh, t->x1, NULL) > base ? 1.0f : -1.0f;
        memcpy(t->x1 + 2 * o, sh->pts + 2 * o, sizeof(float) * 2 * n);
    }
}

static inline float res_at(const float *r, int W, int H, float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    ix = ix < 0 ? 0 : ix >= W ? W - 1 : ix, iy = iy < 0 ? 0 : iy >= H ? H - 1 : iy;
    return r[iy * W + ix];
}

// The fit's error at points x (of sh's outlines), and its gradient into g. The error: the squared difference between
// their coverage and the game's texels, plus the bending and spacing penalties. An edge's share of the gradient comes
// from the residual where it runs, one-sided at texel boundaries: the sheets are hinted, stems sit on texel edges, and
// such an edge gains area in the texel outside it and loses it in the one inside: it is moved only where that lowers
// the error.
static double fit_eval(Tracer *t, const Shape *sh, const float *x, float *g) {
    const int W = t->W, H = t->H;
    float *r = t->res;
    raster_cov(t, sh, x, r);
    double E = 0, Eb = 0, Es = 0;
    for (int i = 0; i < W * H; i++) r[i] -= t->cov[i], E += (double)r[i] * r[i];
    memset(g, 0, sizeof(float) * 2 * sh->npts);
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++]) {
        const int n = sh->count[k];
        const float *p = x + 2 * o, sd = t->side[k], L0 = t->len0[k];
        float *gg = g + 2 * o, *hg = t->hg + 2 * o;
        for (int i = 0; i < n; i++) {
            int j = i + 1 < n ? i + 1 : 0;
            float ex = p[2 * j] - p[2 * i], ey = p[2 * j + 1] - p[2 * i + 1], L = sqrtf(ex * ex + ey * ey);
            if (L < 1e-6f) L = 1e-6f;
            float nx = sd * ey / L, ny = -sd * ex / L, a0 = 0, a1 = 0;  // (its outward normal)
            for (int s = 0; s < FIT_SAMPLES; s++) {
                float u = (s + 0.5f) / FIT_SAMPLES, px = p[2 * i] + u * ex, py = p[2 * i + 1] + u * ey;
                float ro = res_at(r, W, H, px + 1e-4f * nx, py + 1e-4f * ny), ri = res_at(r, W, H, px - 1e-4f * nx, py - 1e-4f * ny);
                float rr = 2 * ((ro < 0 ? ro : 0) + (ri > 0 ? ri : 0)) * (L / FIT_SAMPLES);  // (per unit moved out)
                a0 += (1 - u) * rr, a1 += u * rr;
            }
            gg[2 * i] += a0 * nx, gg[2 * i + 1] += a0 * ny, gg[2 * j] += a1 * nx, gg[2 * j + 1] += a1 * ny;
            float dl = L - L0, gx = 2 * FIT_MU * dl * ex / L, gy = 2 * FIT_MU * dl * ey / L;
            Es += dl * dl;
            gg[2 * j] += gx, gg[2 * j + 1] += gy, gg[2 * i] -= gx, gg[2 * i + 1] -= gy;
        }
        for (int i = 0; i < n; i++) {  // bending: second differences, Huber
            int a = i ? i - 1 : n - 1, b = i + 1 < n ? i + 1 : 0;
            for (int c = 0; c < 2; c++) {
                float d = p[2 * a + c] - 2 * p[2 * i + c] + p[2 * b + c], ad = fabsf(d);
                Eb += ad <= FIT_KINK ? 0.5f * d * d : FIT_KINK * (ad - 0.5f * FIT_KINK);
                hg[2 * i + c] = d < -FIT_KINK ? -FIT_KINK : d > FIT_KINK ? FIT_KINK : d;
            }
        }
        for (int i = 0; i < n; i++) {
            int a = i ? i - 1 : n - 1, b = i + 1 < n ? i + 1 : 0;
            for (int c = 0; c < 2; c++) gg[2 * i + c] += FIT_LAM * (hg[2 * b + c] - 2 * hg[2 * i + c] + hg[2 * a + c]);
        }
    }
    return E + FIT_LAM * Eb + FIT_MU * Es;
}

// 2: t->traced fitted to the game's texels (t->cov), in place. Steepest descent along the gradient, scaled so that the
// point that moves most moves `step`: a step is kept only if it lowers the error (then the next is longer), else it is
// halved. (The exact coverage is only piecewise smooth: line searches stall on it.)
static void fit_glyph(Tracer *t) {
    Shape *sh = &t->traced;
    room_for_fit(t, sh);
    outward_sides(t, sh);
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++]) {
        int n = sh->count[k];
        float sum = 0;
        for (int i = 0; i < n; i++) {
            int j = i + 1 < n ? i + 1 : 0;
            sum += hypotf(sh->pts[2 * (o + j)] - sh->pts[2 * (o + i)], sh->pts[2 * (o + j) + 1] - sh->pts[2 * (o + i) + 1]);
        }
        t->len0[k] = sum / n;
    }
    const int N = 2 * sh->npts;
    float *x = t->x0, *g = t->g0, *xn = t->x1, *gn = t->g1, step = FIT_STEP;
    memcpy(x, sh->pts, sizeof(float) * N);
    double E = fit_eval(t, sh, x, g);
    for (int it = 0; it < FIT_ITERS && isfinite(E); it++) {
        float gm = 0;
        for (int i = 0; i < N; i++) gm = fabsf(g[i]) > gm ? fabsf(g[i]) : gm;
        if (gm < 1e-12f || step < 1e-4f) break;
        float k = step / gm;
        for (int i = 0; i < N; i++) xn[i] = x[i] - k * g[i];
        double En = fit_eval(t, sh, xn, gn);
        if (En < E) {
            float *tx = x, *tg = g;
            x = xn, g = gn, xn = tx, gn = tg, E = En, step *= 1.3f;
        } else
            step *= 0.5f;
    }
    memcpy(sh->pts, x, sizeof(float) * N);
}

// ---------------------------------------------------------------- 3: the clean-up
// The principal direction of n points (x, y pairs) and their centre
static void principal(const float *p, int n, float *cx, float *cy, float *dx, float *dy) {
    double mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) mx += p[2 * i], my += p[2 * i + 1];
    mx /= n, my /= n;
    for (int i = 0; i < n; i++) {
        double u = p[2 * i] - mx, v = p[2 * i + 1] - my;
        sxx += u * u, syy += v * v, sxy += u * v;
    }
    double a = 0.5 * atan2(2 * sxy, sxx - syy);
    *cx = (float)mx, *cy = (float)my, *dx = (float)cos(a), *dy = (float)sin(a);
}

// One fitted outline (p, n points) cleaned up into t->tidy
static void tidy(Tracer *t, const float *p0, int n0, float smooth) {
    float step;
    int m = resample(p0, n0, TIDY_STEP, &t->rs, &t->cap_rs, &step);
    if (!m) return;
    room_for(t, m);
    const float *p = t->rs;
    float *out = t->out, kern[256];
    int kr, k = (int)(TIDY_REACH / step);
    if (k < 2) k = 2;
    step_kernel(smooth, step, kern, &kr);
    int nc = find_corners(t, p, m, k, TIDY_CORNER_DEG);
    if (!nc) {  // (no corner: smoothed all round)
        smooth_axis(p, m, true, kern, kr, t->spad, t->sx);
        smooth_axis(p + 1, m, true, kern, kr, t->spad, t->sy);
        for (int i = 0; i < m; i++) out[2 * i] = t->sx[i], out[2 * i + 1] = t->sy[i];
        shape_add(&t->tidy, out, m);
        return;
    }
    if (nc > t->cap_runs) {
        t->cap_runs = 2 * nc;
        t->run_line = realloc(t->run_line, sizeof(int) * t->cap_runs), t->run_a = realloc(t->run_a, sizeof(int) * t->cap_runs);
        t->run_n = realloc(t->run_n, sizeof(int) * t->cap_runs), t->run_c = realloc(t->run_c, 4 * sizeof(float) * t->cap_runs);
    }
    memcpy(out, p, sizeof(float) * 2 * m);
    for (int c = 0; c < nc; c++) {  // the runs between corners: lines, or curves smoothed with their ends pinned
        int a = t->corner[c], b = c + 1 < nc ? t->corner[c + 1] : t->corner[0] + m, sl = b - a + 1;
        float *seg = t->seg, *rc = t->run_c + 4 * c;
        for (int j = 0; j < sl; j++) {
            int q = a + j < m ? a + j : a + j - m;
            seg[2 * j] = p[2 * q], seg[2 * j + 1] = p[2 * q + 1];
        }
        t->run_a[c] = a, t->run_n[c] = sl, t->run_line[c] = 0;
        float dx = seg[2 * (sl - 1)] - seg[0], dy = seg[2 * (sl - 1) + 1] - seg[1], dl = hypotf(dx, dy);
        if (dl > TIDY_STRAIGHT_MIN) {
            float nx = -dy / dl, ny = dx / dl, dev = 0, tol = TIDY_STRAIGHT * (dl / 3 > 1 ? dl / 3 : 1);
            for (int j = 0; j < sl; j++) {
                float e = fabsf((seg[2 * j] - seg[0]) * nx + (seg[2 * j + 1] - seg[1]) * ny);
                if (e > dev) dev = e;
            }
            t->run_line[c] = dev < tol;
        }
        if (t->run_line[c]) {  // the line through the run (least squares), horizontal or vertical when nearly so
            principal(seg, sl, &rc[0], &rc[1], &rc[2], &rc[3]);
            float deg = atan2f(rc[3], rc[2]) * 57.2957795f;
            deg = fmodf(deg + 360.0f, 180.0f);
            if (deg < TIDY_SNAP_DEG || deg > 180 - TIDY_SNAP_DEG) rc[2] = 1, rc[3] = 0;
            else if (fabsf(deg - 90) < TIDY_SNAP_DEG) rc[2] = 0, rc[3] = 1;
            for (int j = 0; j < sl; j++) {  // (the corners at its ends too)
                int q = a + j < m ? a + j : a + j - m;
                float u = (p[2 * q] - rc[0]) * rc[2] + (p[2 * q + 1] - rc[1]) * rc[3];
                out[2 * q] = rc[0] + u * rc[2], out[2 * q + 1] = rc[1] + u * rc[3];
            }
            continue;
        }
        if (sl > 5) {
            smooth_axis(seg, sl, false, kern, kr, t->spad, t->sx);
            smooth_axis(seg + 1, sl, false, kern, kr, t->spad, t->sy);
            for (int j = 0; j < sl; j++) {
                int e = j < sl - 1 - j ? j : sl - 1 - j;
                float bl = clamp01(e * step / TIDY_PIN);
                seg[2 * j] += (t->sx[j] - seg[2 * j]) * bl, seg[2 * j + 1] += (t->sy[j] - seg[2 * j + 1]) * bl;
            }
        }
        for (int j = 1; j < sl - 1; j++) {
            int q = a + j < m ? a + j : a + j - m;
            out[2 * q] = seg[2 * j], out[2 * q + 1] = seg[2 * j + 1];
        }
    }
    // each corner where its two sides meet: a line itself, a curve's tangent at its end (over TIDY_SIDE)
    int K = (int)(TIDY_SIDE / step);
    for (int c = 0; c < nc; c++) {
        int r1 = c ? c - 1 : nc - 1, r2 = c, ci = t->corner[c];
        float s[2][4];
        bool ok = true;
        for (int e = 0; e < 2 && ok; e++) {
            int r = e ? r2 : r1;
            if (t->run_line[r]) {
                memcpy(s[e], t->run_c + 4 * r, sizeof s[e]);
                continue;
            }
            int sl = t->run_n[r], from = e ? 1 : (sl - K > 0 ? sl - K : 0), to = e ? (K < sl ? K : sl) - 1 : sl - 2, nn = to - from + 1;
            if (nn < 3) { ok = false; break; }
            float *q = t->seg;
            for (int j = 0; j < nn; j++) {
                int at = wrap(t->run_a[r] + from + j, m);
                q[2 * j] = out[2 * at], q[2 * j + 1] = out[2 * at + 1];
            }
            principal(q, nn, &s[e][0], &s[e][1], &s[e][2], &s[e][3]);
        }
        if (!ok) continue;
        float det = -s[0][2] * s[1][3] + s[1][2] * s[0][3];
        if (fabsf(det) < 1e-3f) continue;
        float rx = s[1][0] - s[0][0], ry = s[1][1] - s[0][1], u = (-rx * s[1][3] + s[1][2] * ry) / det;
        float X = s[0][0] + u * s[0][2], Y = s[0][1] + u * s[0][3];
        if (hypotf(X - p[2 * ci], Y - p[2 * ci + 1]) < TIDY_CORNER_MOVE) out[2 * ci] = X, out[2 * ci + 1] = Y;
    }
    shape_add(&t->tidy, out, m);
}

// ---------------------------------------------------------------- 4: the ink, and the outlines kept
// sh's points moved g along their normals, outwards (t->nrm, t->side), into out
static void offset(const Tracer *t, const Shape *sh, float g, float *out) {
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++])
        for (int i = 2 * o; i < 2 * (o + sh->count[k]); i++) out[i] = sh->pts[i] + g * t->side[k] * t->nrm[i];
}

// Douglas-Peucker on p[a..b] (b may be n: the first point again): keep[] set on the points kept between them
static void simplify(const float *p, int n, int a, int b, uint8_t *keep, int *stack) {
    int top = 0;
    stack[top++] = a, stack[top++] = b;
    while (top) {
        int e = stack[--top], s = stack[--top];
        if (e - s < 2) continue;
        const float *A = p + 2 * s, *B = p + 2 * (e % n);
        float dx = B[0] - A[0], dy = B[1] - A[1], L = hypotf(dx, dy) + 1e-12f, best = -1;
        int bi = -1;
        for (int i = s + 1; i < e; i++) {
            float d = fabsf((p[2 * i] - A[0]) * dy - (p[2 * i + 1] - A[1]) * dx) / L;
            if (d > best) best = d, bi = i;
        }
        if (best > SIMPLIFY) {
            keep[bi] = 1;
            stack[top++] = s, stack[top++] = bi, stack[top++] = bi, stack[top++] = e;
        }
    }
}

// The ink of sh at points pts inside the cell: what the game's quads show (a dot on the cell's first column, made
// round, reaches past it)
static float cell_ink(Tracer *t, const Shape *sh, const float *pts) {
    raster_cov(t, sh, pts, t->res);
    float ink = 0;
    for (int y = PAD; y < t->H - PAD; y++)
        for (int x = PAD; x < t->W - PAD; x++) ink += t->res[y * t->W + x];
    return ink;
}

// n points of an outline where a cell's can be (texels of the padded cell, w x h, and as far again around it). Anything
// else is not an outline of ours (a cache file gone bad): far enough out, the fill's sums overflow and it reads outside
// its rows.
static bool pts_sane(const float *p, int n, float w, float h) {
    for (int i = 0; i < n; i++)
        if (!(p[2 * i] > -w && p[2 * i] < 2 * w && p[2 * i + 1] > -h && p[2 * i + 1] < 2 * h)) return false;  // (NaN too)
    return true;
}

// t->tidy, its ink made the game glyph's (`ink`), simplified, into t->done
static void finish(Tracer *t, float ink) {
    Shape *sh = &t->tidy;
    room_for_fit(t, sh);
    outward_sides(t, sh);
    float lo = -INK_REACH, hi = INK_REACH, ink_lo, ink_hi;
    offset(t, sh, lo, t->x0);
    ink_lo = cell_ink(t, sh, t->x0);
    offset(t, sh, hi, t->x0);
    ink_hi = cell_ink(t, sh, t->x0);
    if (ink_hi <= ink) {  // (further out, on that side only: a dot moved in further turns inside out)
        lo = hi, ink_lo = ink_hi, hi = INK_REACH_MAX;
        offset(t, sh, hi, t->x0);
        ink_hi = cell_ink(t, sh, t->x0);
    } else if (ink_lo >= ink) {
        hi = lo, ink_hi = ink_lo, lo = -INK_REACH_MAX;
        offset(t, sh, lo, t->x0);
        ink_lo = cell_ink(t, sh, t->x0);
    }
    if (ink_lo < ink && ink < ink_hi) {
        for (int i = 0; i < INK_STEPS; i++) {
            float mid = 0.5f * (lo + hi);
            offset(t, sh, mid, t->x0);
            if (cell_ink(t, sh, t->x0) < ink) lo = mid;
            else hi = mid;
        }
        float g = 0.5f * (lo + hi);
        if (fabsf(g) > 0.005f) {
            offset(t, sh, g, t->x0);
            memcpy(sh->pts, t->x0, sizeof(float) * 2 * sh->npts);
        }
    }
    t->done.n = t->done.npts = 0;
    uint8_t *keep = (uint8_t *)t->x1;  // (x1 and g1: free now, room for 2 points of floats each)
    int *stack = (int *)t->g1;
    for (int k = 0, o = 0; k < sh->n; o += sh->count[k++]) {
        const float *p = sh->pts + 2 * o;
        int n = sh->count[k], far = 0;
        float best = -1, area = 0;
        for (int i = 0; i < n; i++) {
            float d = hypotf(p[2 * i] - p[0], p[2 * i + 1] - p[1]);
            if (d > best) best = d, far = i;
        }
        if (n < 3 || far == 0 || !pts_sane(p, n, (float)t->W, (float)t->H)) continue;
        memset(keep, 0, (size_t)n);
        keep[0] = keep[far] = 1;
        simplify(p, n, 0, far, keep, stack);
        simplify(p, n, far, n, keep, stack);
        int m = 0;
        float *q = t->x0;
        for (int i = 0; i < n; i++)
            if (keep[i]) q[2 * m] = p[2 * i], q[2 * m + 1] = p[2 * i + 1], m++;
        for (int i = 0; i < m; i++) {
            int j = i + 1 < m ? i + 1 : 0;
            area += q[2 * i] * q[2 * j + 1] - q[2 * j] * q[2 * i + 1];
        }
        if (m >= 3 && fabsf(area) > 0.002f) shape_add(&t->done, q, m);  // (|area| / 2 over a thousandth of a texel)
    }
}

// ---------------------------------------------------------------- the game's sheet
// The game's glyph coverage (its fill, for the outlined font) at sheet texel (x, y), y from the top
static inline float game_cov(const uint8_t *orig, uint32_t pw, uint32_t ph, int x, int y, bool outline) {
    uint8_t v = orig[(size_t)(ph - 1 - y) * pw + x];
    return outline ? clamp01((v - 128) / 127.0f) : v / 255.0f;
}

// A cell's top-left texel (from the sheet's top). The one-texel gutter comes before each cell, on its left and above it:
// the game's glyphs never touch the stride's first row or column, and do reach its last.
static void cell_origin(const TextSheetInfo *info, uint32_t cell, int *x, int *y) {
    *x = (int)(cell % info->cols) * (info->cell_w + 1) + 1;
    *y = (int)(cell / info->cols) * (info->cell_h + 1) + 1;
}

// One cell's outlines (texels of the cell padded by PAD), as made or as read from the cache
typedef struct {
    int n, npts;
    int *count;
    float *pts;
} CellOutlines;

// One sheet's work, shared by its workers: each takes the next cell until none are left
typedef struct {
    const TextSheetInfo *info;
    const uint8_t *orig;
    uint8_t *big;
    uint32_t pw, ph, s;
    bool outline;
    float smooth;  // the clean-up's, smooth_of
    CellOutlines *cells;  // per cell
    atomic_int next_cell;
    atomic_int drawn, kept;  // (kept: glyphs without outlines, left as enlarged)
} SheetJob;

// The cell's game glyph into t->cov (padded); returns its ink. false: it is not on the sheet.
static bool load_cell(const SheetJob *job, Tracer *t, uint32_t c, float *ink) {
    const TextSheetInfo *info = job->info;
    int cx, cy, cw = info->cell_w, ch = info->cell_h;
    cell_origin(info, c, &cx, &cy);
    if (cx + cw > (int)job->pw || cy + ch > (int)job->ph) return false;
    memset(t->cov, 0, sizeof(float) * t->W * t->H);
    float sum = 0;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            sum += t->cov[(y + PAD) * t->W + x + PAD] = game_cov(job->orig, job->pw, job->ph, cx + x, cy + y, job->outline);
    *ink = sum;
    return true;
}

// 1 to 4 for each cell, into job->cells (on threads of their own: the game goes on meanwhile, ahead of them)
static void *make_cells(void *arg) {
    SheetJob *job = arg;
    const uint32_t per = (uint32_t)job->info->cols * job->info->rows;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    Tracer t;
    tracer_init(&t, job->info->cell_w, job->info->cell_h, 0);
    for (;;) {
        uint32_t c = (uint32_t)atomic_fetch_add(&job->next_cell, 1);
        if (c >= per) break;
        float ink;
        if (!load_cell(job, &t, c, &ink) || ink < MIN_INK || !trace_glyph(&t)) continue;
        fit_glyph(&t);
        t.tidy.n = t.tidy.npts = 0;
        for (int k = 0, o = 0; k < t.traced.n; o += t.traced.count[k++])
            if (t.traced.count[k] >= 3) tidy(&t, t.traced.pts + 2 * o, t.traced.count[k], job->smooth);
        if (!t.tidy.n) continue;
        finish(&t, ink);
        if (!t.done.n) continue;
        CellOutlines *co = &job->cells[c];
        co->n = t.done.n, co->npts = t.done.npts;
        co->count = malloc(sizeof(int) * co->n), co->pts = malloc(sizeof(float) * 2 * co->npts);
        memcpy(co->count, t.done.count, sizeof(int) * co->n), memcpy(co->pts, t.done.pts, sizeof(float) * 2 * co->npts);
    }
    tracer_free(&t);
    return NULL;
}

// 5: each cell with outlines filled into the enlarged sheet; one without, cleared when the game's cell holds next to
// nothing (BC4 noise), else left as enlarged (a glyph that gave none)
static void *draw_cells(void *arg) {
    SheetJob *job = arg;
    const TextSheetInfo *info = job->info;
    const int cw = info->cell_w, ch = info->cell_h, S = (int)job->s;
    const uint32_t per = (uint32_t)info->cols * info->rows, dw = job->pw * job->s, dh = job->ph * job->s;
    Tracer t;
    tracer_init(&t, cw, ch, S);
    const int CW = t.W * S, CH = t.H * S;
    int drawn = 0, kept = 0;
    for (;;) {
        uint32_t c = (uint32_t)atomic_fetch_add(&job->next_cell, 1);
        if (c >= per) break;
        const CellOutlines *co = &job->cells[c];
        int cx, cy;
        cell_origin(info, c, &cx, &cy);
        if (cx + cw > (int)job->pw || cy + ch > (int)job->ph) continue;
        int X0 = cx * S, Y0 = cy * S;  // (big's rows are bottom-up, as the sheet's)
        if (!co->n) {
            float ink = 0;  // (all of it: the outlined font's ring too)
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) ink += job->orig[(size_t)(job->ph - 1 - (cy + y)) * job->pw + cx + x] / 255.0f;
            if (ink >= MIN_INK) kept++;
            else
                for (int yy = 0; yy < ch * S; yy++) memset(job->big + (size_t)(dh - 1 - (Y0 + yy)) * dw + X0, 0, (size_t)cw * S);
            continue;
        }
        memset(t.acc, 0, sizeof(float) * (CW + 2) * CH);
        for (int k = 0, o = 0; k < co->n; o += co->count[k++])
            for (int i = 0, m = co->count[k]; i < m; i++) {
                const float *a = co->pts + 2 * (o + i), *b = co->pts + 2 * (o + (i + 1 < m ? i + 1 : 0));
                fill_edge(t.acc, CW, CH, a[0] * S, a[1] * S, b[0] * S, b[1] * S);
            }
        for (int y = 0; y < CH; y++) {
            const float *a = t.acc + (size_t)y * (CW + 2);
            uint8_t *o = t.canvas + (size_t)y * CW;
            float sum = 0;
            for (int x = 0; x < CW; x++) {
                sum += a[x];
                float v = fabsf(sum);
                o[x] = (uint8_t)(v >= 1 ? 255 : v * 255 + 0.5f);
            }
        }
        // NormalS_00's ring: OUTLINE_RING beyond the fill, from each pixel's distance to it (pixels of the enlarged sheet)
        float rp = OUTLINE_RING * S;
        int bx0 = CW, bx1 = -1, by0 = CH, by1 = -1, rw = 0;
        if (job->outline) {  // (only around the ink, as far as the ring reaches)
            int reach = (int)ceilf(rp) + 2;
            for (int y = 0; y < CH; y++)
                for (int x = 0; x < CW; x++)
                    if (t.canvas[y * CW + x]) {
                        if (x < bx0) bx0 = x;
                        if (x > bx1) bx1 = x;
                        if (y < by0) by0 = y;
                        if (y > by1) by1 = y;
                    }
            bx0 = bx0 - reach < 0 ? 0 : bx0 - reach, bx1 = bx1 + reach >= CW ? CW - 1 : bx1 + reach;
            by0 = by0 - reach < 0 ? 0 : by0 - reach, by1 = by1 + reach >= CH ? CH - 1 : by1 + reach;
            rw = bx1 - bx0 + 1;
            for (int y = by0; y <= by1; y++)
                for (int x = bx0; x <= bx1; x++) t.inside[(y - by0) * rw + x - bx0] = t.canvas[y * CW + x] >= 128;
            edt(t.inside, rw, by1 - by0 + 1, t.dout, t.etmp, t.ev);
        }
        // into the cell
        for (int yy = 0; yy < ch * S; yy++) {
            int y = yy + PAD * S;
            uint8_t *row = job->big + (size_t)(dh - 1 - (Y0 + yy)) * dw + X0;
            for (int xx = 0; xx < cw * S; xx++) {
                int x = xx + PAD * S;
                uint8_t v = t.canvas[y * CW + x];
                if (job->outline) {
                    float ring = 0;
                    if (v) ring = 1;  // (in the fill or on its edge: the ring is full there)
                    else if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1)
                        ring = clamp01(1 + rp - sqrtf(t.dout[(y - by0) * rw + x - bx0]));
                    row[xx] = (uint8_t)(128 * ring + 127 * (v / 255.0f) + 0.5f);
                } else
                    row[xx] = v;
            }
        }
        drawn++;
    }
    atomic_fetch_add(&job->drawn, drawn);
    atomic_fetch_add(&job->kept, kept);
    tracer_free(&t);
    return NULL;
}

// fn on n threads (this one among them)
static int run_workers(void *(*fn)(void *), SheetJob *job, int n) {
    pthread_t th[MAX_WORKERS];
    int started = 0;
    atomic_store(&job->next_cell, 0);
    for (int i = 1; i < n; i++)
        if (!pthread_create(&th[started], NULL, fn, job)) started++;
    fn(job);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    return started + 1;
}

// Every core, as far as run_workers goes
static int worker_count(void) {
    int n = SDL_GetNumLogicalCPUCores();
    return n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n;
}

static void cells_free(CellOutlines *cells, uint32_t per) {
    if (!cells) return;
    for (uint32_t c = 0; c < per; c++) free(cells[c].count), free(cells[c].pts);
    free(cells);
}

// ---------------------------------------------------------------- the cache
// <cache>/text/<key>.glyphs: a header, then per cell with outlines: its index, its outlines' count, their point
// counts, their points (floats, x y). The key: the sheet's texels, its cells and every setting the outlines depend on
// (a change to the way they are made: bump GLYPHS_VERSION).
#define GLYPHS_MAGIC "SWTG"
#define GLYPHS_VERSION 1
typedef struct { char magic[4]; uint32_t version, font, cell_w, cell_h, cols, rows, ncells; uint64_t key; } GlyphsHeader;

static uint64_t sheet_key(const SheetJob *job) {
    const TextSheetInfo *info = job->info;
    const float settings[] = {PAD, ZOOM, PEAK_FLOOR, PEAK_EMPTY, MIN_INK, TRACE_STEP, TRACE_CORNER_DEG, TRACE_CORNER_REACH,
                              TRACE_SMOOTH, TRACE_PIN, TRACE_STRAIGHT, TRACE_STRAIGHT_MIN, TANGENT_NEAR, TANGENT_FAR,
                              CORNER_MOVE, FIT_SPACING, FIT_ITERS, FIT_STEP, FIT_SAMPLES, FIT_LAM, FIT_KINK, FIT_MU,
                              TIDY_STEP, TIDY_REACH, TIDY_CORNER_DEG, TIDY_STRAIGHT, TIDY_STRAIGHT_MIN, TIDY_SNAP_DEG,
                              TIDY_PIN, TIDY_SIDE, TIDY_CORNER_MOVE, INK_REACH, INK_REACH_MAX, INK_STEPS, SIMPLIFY,
                              job->smooth, job->outline, GLYPHS_VERSION};
    const uint32_t cells[] = {(uint32_t)info->font, info->cell_w, info->cell_h, info->cols, info->rows, job->pw, job->ph};
    uint64_t h = hash64(0xcbf29ce484222325ull, settings, sizeof settings);
    h = hash64(h, cells, sizeof cells);
    return hash64(h, job->orig, (size_t)job->pw * job->ph);
}

static void glyphs_path(char *out, size_t n, const char *dir, uint64_t key) {
    snprintf(out, n, "%s/text/%016llx.glyphs", dir, (unsigned long long)key);
}

static GlyphsHeader glyphs_header(const SheetJob *job, uint64_t key, uint32_t ncells) {
    const TextSheetInfo *info = job->info;
    GlyphsHeader h = {{0}, GLYPHS_VERSION, (uint32_t)info->font, info->cell_w, info->cell_h, info->cols, info->rows, ncells, key};
    memcpy(h.magic, GLYPHS_MAGIC, 4);
    return h;
}

// The cells' outlines from the cache, into job->cells (all empty). false: none there (or not valid: removed).
static bool glyphs_load(SheetJob *job, const char *dir, uint64_t key) {
    char path[600];
    glyphs_path(path, sizeof path, dir, key);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    const uint32_t per = (uint32_t)job->info->cols * job->info->rows;
    const float W = (float)(job->info->cell_w + 2 * PAD), H = (float)(job->info->cell_h + 2 * PAD);
    GlyphsHeader want = glyphs_header(job, key, 0), h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.ncells <= per;
    want.ncells = h.ncells;
    ok = ok && !memcmp(&h, &want, sizeof h);
    for (uint32_t i = 0; ok && i < h.ncells; i++) {
        uint32_t c, n;
        ok = fread(&c, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && c < per && n && n < 4096 && !job->cells[c].n;
        if (!ok) break;
        CellOutlines *co = &job->cells[c];
        co->count = malloc(sizeof(int) * n);
        co->n = (int)n;
        ok = co->count && fread(co->count, sizeof(int), n, f) == n;
        int64_t total = 0;
        for (uint32_t k = 0; ok && k < n; k++) ok = co->count[k] >= 3 && (total += co->count[k]) < (1 << 20);
        if (!ok) break;
        co->npts = (int)total;
        co->pts = malloc(sizeof(float) * 2 * total);
        ok = co->pts && fread(co->pts, sizeof(float) * 2, (size_t)total, f) == (size_t)total && pts_sane(co->pts, co->npts, W, H);
    }
    fclose(f);
    if (!ok) {
        LOG("[text] %s: not valid, made again\n", path);
        remove(path);
        for (uint32_t c = 0; c < per; c++) free(job->cells[c].count), free(job->cells[c].pts);
        memset(job->cells, 0, sizeof(CellOutlines) * per);
    }
    return ok;
}

static void glyphs_store(const SheetJob *job, const char *dir, uint64_t key) {
    char path[600], tmp[640];
    snprintf(path, sizeof path, "%s/text", dir);
    mkdir(path, 0755);
    glyphs_path(path, sizeof path, dir, key);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)job);
    const uint32_t per = (uint32_t)job->info->cols * job->info->rows;
    uint32_t ncells = 0;
    for (uint32_t c = 0; c < per; c++) ncells += job->cells[c].n > 0;
    GlyphsHeader h = glyphs_header(job, key, ncells);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(&h, sizeof h, 1, f) == 1;
    for (uint32_t c = 0; ok && c < per; c++) {
        const CellOutlines *co = &job->cells[c];
        uint32_t n = (uint32_t)co->n;
        if (!n) continue;
        ok = fwrite(&c, 4, 1, f) == 1 && fwrite(&n, 4, 1, f) == 1 && fwrite(co->count, sizeof(int), n, f) == n &&
             fwrite(co->pts, sizeof(float) * 2, (size_t)co->npts, f) == (size_t)co->npts;
    }
    if (f && fclose(f)) ok = false;
    ok = ok && !rename(tmp, path);  // (a sheet's outlines are made once a run: nothing is there yet)
    if (f && !ok) remove(tmp);
    if (!ok) LOG("[text] could not keep %s: these glyphs are made again next time\n", path);
}

// ---------------------------------------------------------------- the outlines of this run
// Per sheet (by its key): its outlines, made or read (cells; NULL: they could not be made), or being made. Kept for the
// run: a sheet that goes up again is drawn from them.
typedef struct {
    uint64_t key;
    bool made;
    CellOutlines *cells;
} Outlines;

static Outlines *g_outlines;
static int g_noutlines, g_cap_outlines;
static pthread_mutex_t g_outlines_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_making_lock = PTHREAD_MUTEX_INITIALIZER;  // (a sheet at a time, on every core)
static _Atomic uint32_t g_generation = 1;  // (moves on whenever a sheet's outlines have been made)

uint32_t text_redraw_generation(void) { return atomic_load(&g_generation); }

// The sheet's entry (g_outlines_lock held). add: made, being made, when there is none (NULL: no memory).
static Outlines *outlines_find(uint64_t key, bool add) {
    for (int i = 0; i < g_noutlines; i++)
        if (g_outlines[i].key == key) return &g_outlines[i];
    if (!add) return NULL;
    if (g_noutlines == g_cap_outlines) {
        int cap = g_cap_outlines ? 2 * g_cap_outlines : 16;
        Outlines *o = realloc(g_outlines, sizeof *o * cap);
        if (!o) return NULL;
        g_outlines = o, g_cap_outlines = cap;
    }
    Outlines *o = &g_outlines[g_noutlines++];
    *o = (Outlines){key, false, NULL};
    return o;
}

// A sheet's outlines, made or read, kept for the run. Returns the ones kept (another's, when it got there first: then
// cells are freed).
static CellOutlines *outlines_keep(uint64_t key, CellOutlines *cells, uint32_t per) {
    pthread_mutex_lock(&g_outlines_lock);
    Outlines *o = outlines_find(key, true);
    if (o && o->made) cells_free(cells, per), cells = o->cells;
    else if (o) o->made = true, o->cells = cells;
    pthread_mutex_unlock(&g_outlines_lock);
    return cells;
}

// A sheet's outlines being made on a thread of their own, from a copy of its texels
typedef struct {
    SheetJob job;
    uint64_t key;
    uint32_t layer;
    char cache_dir[512];  // ("": not kept on disk)
} MakeJob;

static void *make_sheet(void *arg) {
    MakeJob *mj = arg;
    SheetJob *job = &mj->job;
    const uint32_t per = (uint32_t)job->info->cols * job->info->rows;
    pthread_mutex_lock(&g_making_lock);
    uint64_t t0 = SDL_GetTicksNS();
    int threads = run_workers(make_cells, job, worker_count());
    uint64_t ns = SDL_GetTicksNS() - t0;
    pthread_mutex_unlock(&g_making_lock);
    int glyphs = 0;
    for (uint32_t c = 0; c < per; c++) glyphs += job->cells[c].n > 0;
    LOG("[text] %s sheet %u: %d glyphs traced and fitted in %.2f s on %d threads (kept as %016llx)\n", job->info->name,
        mj->layer, glyphs, ns / 1e9, threads, (unsigned long long)mj->key);
    if (*mj->cache_dir) glyphs_store(job, mj->cache_dir, mj->key);
    outlines_keep(mj->key, job->cells, per);
    atomic_fetch_add(&g_generation, 1);  // (the sheet goes up again: textures.c)
    free((void *)job->orig);
    free(mj);
    return NULL;
}

// The sheet's outlines (from) to be made on a thread of their own, unless they are already. false: they cannot be,
// and are not tried again this run (the sheet stays as enlarged).
static bool make_later(const SheetJob *from, uint64_t key, uint32_t layer, const char *cache_dir) {
    const uint32_t per = (uint32_t)from->info->cols * from->info->rows;
    pthread_mutex_lock(&g_outlines_lock);
    Outlines *o = outlines_find(key, false);
    if (o) {  // (another call got there first)
        bool making = !o->made;
        pthread_mutex_unlock(&g_outlines_lock);
        return making;
    }
    o = outlines_find(key, true);
    MakeJob *mj = o ? calloc(1, sizeof *mj) : NULL;
    uint8_t *copy = mj ? malloc((size_t)from->pw * from->ph) : NULL;
    CellOutlines *cells = copy ? calloc(per, sizeof *cells) : NULL;
    pthread_t th;
    bool ok = cells != NULL;
    if (ok) {
        memcpy(copy, from->orig, (size_t)from->pw * from->ph);
        SheetJob *job = &mj->job;
        job->info = from->info, job->orig = copy, job->pw = from->pw, job->ph = from->ph;
        job->outline = from->outline, job->smooth = from->smooth, job->cells = cells;
        mj->key = key, mj->layer = layer;
        snprintf(mj->cache_dir, sizeof mj->cache_dir, "%s", cache_dir ? cache_dir : "");
        ok = !pthread_create(&th, NULL, make_sheet, mj);
        if (ok) pthread_detach(th);
    }
    if (!ok) {
        free(cells), free(copy), free(mj);
        if (o) o->made = true;
        LOG("[text] %s sheet %u: its outlines could not be made, it stays as enlarged\n", from->info->name, layer);
    }
    pthread_mutex_unlock(&g_outlines_lock);
    return ok;
}

// ---------------------------------------------------------------- the sheet
int text_redraw_sheet(const TextSheetInfo *info, uint32_t layer, const uint8_t *orig, uint32_t pw, uint32_t ph, uint8_t *big,
                      uint32_t s, const char *cache_dir) {
    if (!info || info->font == TEXT_FONT_OTHER || !info->cols || !info->rows || s < 2) return 0;
    uint64_t t0 = SDL_GetTicksNS();
    const int cw = info->cell_w, ch = info->cell_h;
    const uint32_t dw = pw * s, dh = ph * s, per = (uint32_t)info->cols * info->rows;
    SheetJob job = {.info = info, .orig = orig, .big = big, .pw = pw, .ph = ph, .s = s,
                    .outline = info->font == TEXT_FONT_OUTLINE, .smooth = smooth_of(info->font)};
    uint64_t key = sheet_key(&job);
    // the outlines: those of this run (or being made), else the cache's, else made on a thread of their own
    pthread_mutex_lock(&g_outlines_lock);
    const Outlines *o = outlines_find(key, false);
    bool known = o != NULL, making = o && !o->made;
    job.cells = o ? o->cells : NULL;
    pthread_mutex_unlock(&g_outlines_lock);
    if (making) return -1;
    const char *from = "made this run";
    if (!known) {
        CellOutlines *cells = calloc(per, sizeof *cells);
        job.cells = cells;
        if (!cells || !cache_dir || !glyphs_load(&job, cache_dir, key)) {
            free(cells);
            return make_later(&job, key, layer, cache_dir) ? -1 : 0;
        }
        job.cells = outlines_keep(key, cells, per);
        from = "from the cache";
    }
    if (!job.cells) return 0;  // (they could not be made)
    // the gutters cleared: the sampler at a quad's edge reads them (the game's quads reach a little past their cells),
    // and nothing but the glyphs drawn again is to show there
    uint32_t gw = (uint32_t)info->cols * (cw + 1) + 1, gh = (uint32_t)info->rows * (ch + 1) + 1;
    if (gw > pw) gw = pw;
    if (gh > ph) gh = ph;
    for (uint32_t y = 0; y < gh; y++)
        for (uint32_t r = 0; r < s; r++) {
            uint8_t *row = big + (size_t)(dh - 1 - (y * s + r)) * dw;
            if (y % (ch + 1) == 0) memset(row, 0, (size_t)gw * s);
            else
                for (uint32_t x = 0; x < gw; x += cw + 1) memset(row + (size_t)x * s, 0, s);
        }
    run_workers(draw_cells, &job, worker_count());
    int drawn = atomic_load(&job.drawn), kept = atomic_load(&job.kept);
    char also[40] = "";
    if (kept) snprintf(also, sizeof also, ", %d left as enlarged", kept);
    LOG("[text] %s sheet %u: %d glyphs drawn again%s in %.0f ms (outlines %s)\n", info->name, layer, drawn, also,
        (SDL_GetTicksNS() - t0) / 1e6, from);
    return drawn;
}
