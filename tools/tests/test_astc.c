// Differential test of runtime/nvn/astc.c against ARM astcenc (tools/tests/test_astc_ref.cpp shim).
// Driven by tools/tests/test_astc.sh. Modes:
//   img [seed]                       synthetic images, encoded by astcenc at all block sizes / qualities
//   rand <blocks per size> [seed]    random 128-bit blocks (illegal, void extent, HDR, ...)
//   mkimg <out.tga> <w> <h> <seed>   write a synthetic test image (for the astcenc CLI)
//   mkrand <out.astc> <bw> <bh> <w> <h> <seed>   write a .astc file of random blocks
//   cmp <in.astc> <ref.tga> <srgb>   decode a .astc file, compare with an astcenc -dl/-ds output
//   bench                            decode throughput
#include "../../runtime/nvn/astc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ref_encode(const uint8_t *rgba, unsigned w, unsigned h, unsigned bw, unsigned bh, int srgb, float quality,
               uint8_t *blocks);
int ref_decode(const uint8_t *blocks, unsigned w, unsigned h, unsigned bw, unsigned bh, int srgb, uint8_t *rgba);
int ref_block_info(const uint8_t block[16], unsigned bw, unsigned bh, int srgb, int *partitions, int *dual,
                   int cems[4], int *hdr);
void ref_block_dump(const uint8_t block[16], unsigned bw, unsigned bh, int srgb);

static const int k_sizes[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                   {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// ---- rng ----

static uint64_t g_rng;
static uint64_t rnd64(void) {  // splitmix64
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t n) { return (uint32_t)(rnd64() % n); }
static double rndf(void) { return (double)(rnd64() >> 11) / 9007199254740992.0; }

// ---- synthetic images ----

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

// value noise, cells of size `cell`
static double vnoise(const double *grid, int gw, double x, double y, double cell) {
    double fx = x / cell, fy = y / cell;
    int ix = (int)fx, iy = (int)fy;
    double tx = fx - ix, ty = fy - iy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    double a = grid[iy * gw + ix], b = grid[iy * gw + ix + 1];
    double c = grid[(iy + 1) * gw + ix], d = grid[(iy + 1) * gw + ix + 1];
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

static void make_image(uint8_t *px, int w, int h, uint64_t seed) {
    uint64_t save = g_rng;
    g_rng = seed * 7919 + 1;
    double cell = 4 + rnd(28);
    int gw = (int)(w / cell) + 3, gh = (int)(h / cell) + 3;
    double *grid = malloc(sizeof(double) * gw * gh * 4);
    for (int i = 0; i < gw * gh * 4; i++) grid[i] = rndf();
    double base[2][4];
    for (int i = 0; i < 2; i++) for (int c = 0; c < 4; c++) base[i][c] = rndf();
    int alpha_mode = rnd(5);   // 0 opaque, 1 gradient, 2 noise, 3 binary mask, 4 random per texel
    int gray = rnd(4) == 0;
    double noise_amt = rndf() * 0.6;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double t = (double)(x + y) / (w + h);
            double v[4];
            for (int c = 0; c < 4; c++) {
                double n = vnoise(grid + c * gw * gh, gw, x, y, cell);
                v[c] = lerp(base[0][c], base[1][c], t) * (1 - noise_amt) + n * noise_amt;
            }
            if (gray) v[1] = v[2] = v[0];
            switch (alpha_mode) {
            case 0: v[3] = 1; break;
            case 1: v[3] = (double)y / h; break;
            case 2: break;
            case 3: v[3] = v[3] > 0.5 ? 1 : 0; break;
            case 4: v[3] = rndf(); break;
            }
            for (int c = 0; c < 4; c++) {
                double q = v[c] < 0 ? 0 : v[c] > 1 ? 1 : v[c];
                px[(y * w + x) * 4 + c] = (uint8_t)(q * 255 + 0.5);
            }
        }
    }
    // flat-coloured shapes with hard edges (and some exactly flat areas -> void-extent blocks)
    int shapes = rnd(12);
    for (int s = 0; s < shapes; s++) {
        uint8_t col[4] = {(uint8_t)rnd(256), (uint8_t)rnd(256), (uint8_t)rnd(256),
                          (uint8_t)(alpha_mode == 0 ? 255 : rnd(256))};
        if (gray) col[1] = col[2] = col[0];
        int cx = rnd(w), cy = rnd(h), rx = 2 + rnd(w / 2 + 1), ry = 2 + rnd(h / 2 + 1), circle = rnd(2);
        for (int y = cy - ry; y < cy + ry; y++) {
            for (int x = cx - rx; x < cx + rx; x++) {
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                if (circle) {
                    double dx = (double)(x - cx) / rx, dy = (double)(y - cy) / ry;
                    if (dx * dx + dy * dy > 1) continue;
                }
                memcpy(px + (y * w + x) * 4, col, 4);
            }
        }
    }
    // a patch of raw noise (worst case for the encoder: many partitions / dual plane)
    if (rnd(2)) {
        int x0 = rnd(w), y0 = rnd(h), pw = 1 + rnd(w / 3 + 1), ph = 1 + rnd(h / 3 + 1);
        for (int y = y0; y < y0 + ph && y < h; y++)
            for (int x = x0; x < x0 + pw && x < w; x++)
                for (int c = 0; c < 4; c++)
                    if (c < 3 || alpha_mode != 0) px[(y * w + x) * 4 + c] = (uint8_t)rnd(256);
    }
    free(grid);
    g_rng = save;
}

// ---- files ----

static int write_tga(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[18] = {0};
    hdr[2] = 2;
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8); hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;  // 8 alpha bits, top-left origin
    fwrite(hdr, 1, 18, f);
    for (int i = 0; i < w * h; i++) {
        uint8_t bgra[4] = {px[4 * i + 2], px[4 * i + 1], px[4 * i + 0], px[4 * i + 3]};
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len);
    if (fread(d, 1, *len, f) != *len) { free(d); d = NULL; }
    fclose(f);
    return d;
}

// TGA types 2 / 10 (RLE), 24 / 32 bpp.
static uint8_t *read_tga(const char *path, int *w, int *h) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return NULL;
    int type = d[2], bpp = d[16], desc = d[17];
    *w = d[12] | d[13] << 8; *h = d[14] | d[15] << 8;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32)) { free(d); return NULL; }
    int bytes = bpp / 8, n = *w * *h;
    uint8_t *px = malloc((size_t)n * 4), *tmp = malloc((size_t)n * 4);
    const uint8_t *p = d + 18 + d[0];
    for (int i = 0; i < n;) {
        int count = 1, raw = 1;
        if (type == 10) { count = (*p & 0x7F) + 1; raw = !(*p & 0x80); p++; }
        for (int k = 0; k < count && i < n; k++, i++) {
            tmp[4 * i + 0] = p[2]; tmp[4 * i + 1] = p[1]; tmp[4 * i + 2] = p[0];
            tmp[4 * i + 3] = bytes == 4 ? p[3] : 255;
            if (raw) p += bytes;
        }
        if (!raw) p += bytes;
    }
    for (int y = 0; y < *h; y++) {
        int sy = (desc & 0x20) ? y : *h - 1 - y;
        memcpy(px + (size_t)y * *w * 4, tmp + (size_t)sy * *w * 4, (size_t)*w * 4);
    }
    free(tmp);
    free(d);
    return px;
}

static int write_astc(const char *path, const uint8_t *blocks, int bw, int bh, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[16] = {0x13, 0xAB, 0xA1, 0x5C, (uint8_t)bw, (uint8_t)bh, 1,
                       (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16),
                       (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), 1, 0, 0};
    fwrite(hdr, 1, 16, f);
    fwrite(blocks, 16, (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh), f);
    fclose(f);
    return 0;
}

// ---- comparison ----

static long g_reported;

// Compares two decodes; returns the number of mismatching texels.
static long compare(const uint8_t *a, const uint8_t *b, int w, int h, int bw, int bh, const uint8_t *blocks,
                    const char *what) {
    long bad = 0;
    int bx = (w + bw - 1) / bw;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = a + ((size_t)y * w + x) * 4, *q = b + ((size_t)y * w + x) * 4;
            if (!memcmp(p, q, 4)) continue;
            if (bad++ == 0 && g_reported++ < 20) {
                const uint8_t *blk = blocks ? blocks + 16 * ((size_t)(y / bh) * bx + x / bw) : NULL;
                printf("  MISMATCH %s %dx%d texel (%d,%d): ours %02x%02x%02x%02x ref %02x%02x%02x%02x", what, bw, bh,
                       x, y, p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
                if (blk) {
                    printf(" block ");
                    for (int i = 0; i < 16; i++) printf("%02x", blk[i]);
                }
                printf("\n");
            }
        }
    }
    return bad;
}

// ---- modes ----

static int mode_img(uint64_t seed) {
    static const float quals[4] = {0.0f, 10.0f, 60.0f, 98.0f};  // fastest, fast, medium, thorough
    static const int dims[3][2] = {{96, 80}, {67, 45}, {130, 33}};  // incl. sizes that cut blocks
    long total_bad = 0, images = 0, texels = 0;
    for (int s = 0; s < 14; s++) {
        int bw = k_sizes[s][0], bh = k_sizes[s][1];
        long bad_size = 0;
        for (int srgb = 0; srgb < 2; srgb++) {
            for (int qi = 0; qi < 4; qi++) {
                for (int di = 0; di < 3; di++) {
                    int w = dims[di][0], h = dims[di][1];
                    size_t nb = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
                    uint8_t *src = malloc((size_t)w * h * 4), *blocks = malloc(nb * 16);
                    uint8_t *ours = malloc((size_t)w * h * 4), *ref = malloc((size_t)w * h * 4);
                    make_image(src, w, h, seed * 1000 + s * 100 + srgb * 50 + qi * 10 + di);
                    if (ref_encode(src, w, h, bw, bh, srgb, quals[qi], blocks) ||
                        ref_decode(blocks, w, h, bw, bh, srgb, ref)) {
                        printf("astcenc failed\n");
                        return 2;
                    }
                    astc_decode_ldr(blocks, w, h, bw, bh, srgb, ours);
                    char what[64];
                    snprintf(what, sizeof what, "img q%g %s %dx%d", quals[qi], srgb ? "srgb" : "lin", w, h);
                    bad_size += compare(ours, ref, w, h, bw, bh, blocks, what);
                    images++;
                    texels += (long)w * h;
                    free(src); free(blocks); free(ours); free(ref);
                }
            }
        }
        printf("img %2dx%-2d: %ld mismatching texels\n", bw, bh, bad_size);
        total_bad += bad_size;
    }
    printf("img: %ld images, %ld texels, %ld mismatches\n", images, texels, total_bad);
    return total_bad ? 1 : 0;
}

typedef struct { long err, cnst, part[5], dual, hdr, cem[16]; } Stats;

static void gen_block(uint8_t *b, int bw, int bh, int srgb, Stats *st) {
    uint32_t kind = rnd(100);
    for (int i = 0; i < 2; i++) {
        uint64_t v = rnd64();
        memcpy(b + 8 * i, &v, 8);
    }
    if (kind < 15) {  // void extent
        uint32_t m = 0x1FC | (rnd(5) == 0) << 9 | (rnd(10) ? 3u : rnd(4)) << 10;
        uint64_t lo = (m & 0xFFF);
        uint32_t c[4];
        for (int i = 0; i < 4; i++) c[i] = rnd(0x2000);
        uint32_t r = rnd(10);
        if (r < 5) {  // ordered coordinates
            if (c[0] > c[1]) { uint32_t t = c[0]; c[0] = c[1]; c[1] = t; }
            if (c[2] > c[3]) { uint32_t t = c[2]; c[2] = c[3]; c[3] = t; }
        } else if (r < 7) {
            c[0] = c[1] = c[2] = c[3] = 0x1FFF;
        } else if (r < 8) {
            c[1] = c[0];
        }
        lo |= (uint64_t)c[0] << 12 | (uint64_t)c[1] << 25 | (uint64_t)c[2] << 38 | (uint64_t)c[3] << 51;
        memcpy(b, &lo, 8);
    } else if (kind < 60) {  // random, resampled towards blocks the reference accepts
        for (int tries = 0; tries < 16; tries++) {
            if (rnd(2)) b[1] &= ~0x18;  // single partition is far more often legal
            int p, d, cems[4], hdr;
            if (ref_block_info(b, bw, bh, srgb, &p, &d, cems, &hdr) == 2) break;
            for (int i = 0; i < 2; i++) {
                uint64_t v = rnd64();
                memcpy(b + 8 * i, &v, 8);
            }
        }
    }  // else: fully random
    int p = 0, d = 0, cems[4], hdr = 0;
    int r = ref_block_info(b, bw, bh, srgb, &p, &d, cems, &hdr);
    if (r == 0) st->err++;
    else if (r == 1) st->cnst++;
    else {
        st->part[p]++;
        st->dual += d;
        int any_hdr = 0;  // blocks with an HDR endpoint mode (magenta partition in LDR)
        for (int i = 0; i < p; i++) {
            st->cem[cems[i]]++;
            any_hdr |= cems[i] == 2 || cems[i] == 3 || cems[i] == 7 || cems[i] == 11 || cems[i] >= 14;
        }
        st->hdr += any_hdr;
    }
}

static int mode_rand(long per_size, uint64_t seed) {
    long total_bad = 0;
    Stats all = {0};
    for (int s = 0; s < 14; s++) {
        int bw = k_sizes[s][0], bh = k_sizes[s][1];
        const int gx = 64, gy = 64;  // blocks per batch
        int w = gx * bw, h = gy * bh;
        uint8_t *blocks = malloc(16 * gx * gy), *ours = malloc((size_t)w * h * 4), *ref = malloc((size_t)w * h * 4);
        long bad = 0;
        Stats st = {0};
        g_rng = seed * 1000003 + s;
        for (long done = 0; done < per_size; done += gx * gy) {
            int srgb = (int)((done / (gx * gy)) & 1);
            for (int i = 0; i < gx * gy; i++) gen_block(blocks + 16 * i, bw, bh, srgb, &st);
            if (ref_decode(blocks, w, h, bw, bh, srgb, ref)) { printf("astcenc failed\n"); return 2; }
            astc_decode_ldr(blocks, w, h, bw, bh, srgb, ours);
            bad += compare(ours, ref, w, h, bw, bh, blocks, srgb ? "rand srgb" : "rand lin");
        }
        long n = st.err + st.cnst + st.part[1] + st.part[2] + st.part[3] + st.part[4];
        printf("rand %2dx%-2d: %ld blocks (error %ld, void-extent %ld, 1/2/3/4 partitions %ld/%ld/%ld/%ld, "
               "dual %ld, hdr %ld): %ld mismatching texels\n",
               bw, bh, n, st.err, st.cnst, st.part[1], st.part[2], st.part[3], st.part[4], st.dual, st.hdr, bad);
        all.err += st.err; all.cnst += st.cnst; all.dual += st.dual; all.hdr += st.hdr;
        for (int i = 0; i < 5; i++) all.part[i] += st.part[i];
        for (int i = 0; i < 16; i++) all.cem[i] += st.cem[i];
        total_bad += bad;
        free(blocks); free(ours); free(ref);
    }
    printf("rand CEM histogram:");
    for (int i = 0; i < 16; i++) printf(" %d:%ld", i, all.cem[i]);
    printf("\nrand: %ld mismatching texels total\n", total_bad);
    return total_bad ? 1 : 0;
}

static int mode_cmp(const char *astc_path, const char *tga_path, int srgb) {
    size_t len;
    uint8_t *d = read_file(astc_path, &len);
    int w, h;
    uint8_t *ref = read_tga(tga_path, &w, &h);
    if (!d || !ref || len < 16 || (d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24) != 0x5CA1AB13u) {
        printf("cmp: cannot read %s / %s\n", astc_path, tga_path);
        return 2;
    }
    int bw = d[4], bh = d[5];
    int aw = d[7] | d[8] << 8 | d[9] << 16, ah = d[10] | d[11] << 8 | d[12] << 16;
    if (aw != w || ah != h) { printf("cmp: size mismatch\n"); return 2; }
    uint8_t *ours = malloc((size_t)w * h * 4);
    astc_decode_ldr(d + 16, w, h, bw, bh, srgb, ours);
    // astcenc's CLI writes error texels (NaN) as 0 after its [0,1] clamp: count those separately
    long bad = 0, err_texels = 0;
    for (long i = 0; i < (long)w * h; i++) {
        const uint8_t *p = ours + 4 * i, *q = ref + 4 * i;
        if (!memcmp(p, q, 4)) continue;
        if (p[0] == 0xFF && p[1] == 0 && p[2] == 0xFF && p[3] == 0xFF && !(q[0] | q[1] | q[2] | q[3])) {
            err_texels++;
            continue;
        }
        if (bad++ < 5)
            printf("  MISMATCH cmp %s texel %ld: ours %02x%02x%02x%02x ref %02x%02x%02x%02x\n", astc_path, i, p[0],
                   p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
    }
    printf("cmp %s (%dx%d, %dx%d, %s): %ld mismatching texels%s\n", astc_path, w, h, bw, bh, srgb ? "srgb" : "lin", bad,
           err_texels ? " (+ error texels: ours magenta, CLI 0000)" : "");
    if (err_texels) printf("    error texels: %ld\n", err_texels);
    return bad ? 1 : 0;
}

static int mode_bench(void) {
    for (int s = 0; s < 14; s++) {
        int bw = k_sizes[s][0], bh = k_sizes[s][1];
        int w = 1024, h = 1024;
        size_t nb = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
        uint8_t *src = malloc((size_t)w * h * 4), *blocks = malloc(nb * 16), *out = malloc((size_t)w * h * 4);
        make_image(src, w, h, 42 + s);
        ref_encode(src, w, h, bw, bh, 0, 10.0f, blocks);
        clock_t t0 = clock();
        int reps = 0;
        do { astc_decode_ldr(blocks, w, h, bw, bh, false, out); reps++; } while (clock() - t0 < CLOCKS_PER_SEC / 2);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("bench %2dx%-2d: %.1f Mtexel/s (%.1f MB/s RGBA8 out)\n", bw, bh, reps * (double)w * h / secs / 1e6,
               reps * (double)w * h * 4 / secs / 1e6);
        free(src); free(blocks); free(out);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see header\n"); return 2; }
    const char *m = argv[1];
    if (!strcmp(m, "img")) return mode_img(argc > 2 ? strtoull(argv[2], 0, 0) : 1);
    if (!strcmp(m, "rand") && argc > 2) return mode_rand(atol(argv[2]), argc > 3 ? strtoull(argv[3], 0, 0) : 1);
    if (!strcmp(m, "bench")) return mode_bench();
    if (!strcmp(m, "mkimg") && argc > 5) {
        int w = atoi(argv[3]), h = atoi(argv[4]);
        uint8_t *px = malloc((size_t)w * h * 4);
        make_image(px, w, h, strtoull(argv[5], 0, 0));
        return write_tga(argv[2], px, w, h);
    }
    if (!strcmp(m, "mkrand") && argc > 7) {
        int bw = atoi(argv[3]), bh = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]);
        g_rng = strtoull(argv[7], 0, 0);
        size_t nb = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
        uint8_t *blocks = malloc(nb * 16);
        Stats st = {0};
        for (size_t i = 0; i < nb; i++) gen_block(blocks + 16 * i, bw, bh, 0, &st);
        return write_astc(argv[2], blocks, bw, bh, w, h);
    }
    if (!strcmp(m, "cmp") && argc > 4) return mode_cmp(argv[2], argv[3], atoi(argv[4]));
    if (!strcmp(m, "blk") && argc > 5) {  // blk <32 hex digits> <bw> <bh> <srgb>: debug one block
        uint8_t b[16], ours[144 * 4], ref[144 * 4];
        for (int i = 0; i < 16; i++) sscanf(argv[2] + 2 * i, "%2hhx", &b[i]);
        int bw = atoi(argv[3]), bh = atoi(argv[4]), srgb = atoi(argv[5]);
        ref_block_dump(b, bw, bh, srgb);
        astc_decode_block(b, bw, bh, srgb, ours);
        ref_decode(b, bw, bh, bw, bh, srgb, ref);
        for (int i = 0; i < bw * bh; i++)
            printf("  t%d ours %02x%02x%02x%02x ref %02x%02x%02x%02x%s\n", i, ours[4 * i], ours[4 * i + 1],
                   ours[4 * i + 2], ours[4 * i + 3], ref[4 * i], ref[4 * i + 1], ref[4 * i + 2], ref[4 * i + 3],
                   memcmp(ours + 4 * i, ref + 4 * i, 4) ? "  <--" : "");
        return 0;
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
