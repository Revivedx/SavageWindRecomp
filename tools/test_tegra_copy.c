/* Build with cc -O2 -Iruntime/nvn tools/test_tegra_copy.c runtime/nvn/tegra.c -o test_tegra_copy */
#include "tegra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t offset(unsigned x, unsigned y, unsigned z, unsigned row_gobs,
                     unsigned block_rows, unsigned bh, unsigned bd) {
    size_t block = ((size_t)(z / bd) * block_rows + y / (8 * bh)) * row_gobs + x / 64;
    size_t gob = (block * bd + z % bd) * bh + (y / 8) % bh;
    return gob * 512 + (x % 64 / 32) * 256 + (y % 8 / 2) * 64 +
           (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

static int check(unsigned w, unsigned h, unsigned d, unsigned bpp, unsigned bh_log, unsigned bd_log) {
    unsigned bh = 1u << bh_log, bd = 1u << bd_log, row_bytes = w * bpp;
    unsigned row_gobs = (row_bytes + 63) / 64, block_rows = (h + 8 * bh - 1) / (8 * bh);
    size_t tiled_size = (size_t)((d + bd - 1) / bd) * block_rows * row_gobs * bh * bd * 512;
    size_t n = (size_t)row_bytes * h * d;
    unsigned char *src = malloc(n), *expected = malloc(tiled_size + 2);
    unsigned char *tiled = malloc(tiled_size + 2), *back = malloc(n + 2);
    if (!src || !expected || !tiled || !back) abort();
    memset(expected, 0xdf, tiled_size + 2);
    memset(tiled, 0xdf, tiled_size + 2);
    memset(back, 0xa5, n + 2);
    uint32_t state = 12345;
    for (size_t i = 0; i < n; i++) {
        state = 1664525 * state + 1013904223;
        src[i] = (uint8_t)(state >> 24);
    }
    for (unsigned z = 0; z < d; z++)
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < row_bytes; x++)
                expected[1 + offset(x, y, z, row_gobs, block_rows, bh, bd)] = src[((size_t)z*h+y)*row_bytes+x];
    tegra_swizzle(src, tiled + 1, w, h, d, bpp, bh_log, bd_log);
    tegra_deswizzle(expected + 1, back + 1, w, h, d, bpp, bh_log, bd_log);
    int ok = !memcmp(tiled, expected, tiled_size + 2) && !memcmp(src, back + 1, n) &&
             back[0] == 0xa5 && back[n + 1] == 0xa5;
    free(back); free(tiled); free(expected); free(src);
    return ok;
}

int main(void) {
    const unsigned widths[] = {1, 15, 16, 17, 63, 64, 65, 137}, bpp[] = {1, 2, 3, 4, 8, 16};
    for (unsigned w = 0; w < sizeof widths / sizeof *widths; w++)
        for (unsigned b = 0; b < sizeof bpp / sizeof *bpp; b++)
            for (unsigned bh = 0; bh < 4; bh++)
                for (unsigned bd = 0; bd < 3; bd++)
                    if (!check(widths[w], 19, 5, bpp[b], bh, bd)) {
                        fprintf(stderr, "Mismatch: width %u, block bytes %u, height log %u, depth log %u\n",
                                widths[w], bpp[b], bh, bd);
                        return 1;
                    }
    puts("Tegra surface copies match the byte reference");
    return 0;
}
