// Yaz0 and SARC readers: see archive.h.
#include "archive.h"

#include <stdlib.h>
#include <string.h>

uint8_t *yaz0_unpack(const uint8_t *d, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(d, "Yaz0", 4)) return NULL;
    size_t size = (size_t)d[4] << 24 | (size_t)d[5] << 16 | (size_t)d[6] << 8 | d[7], s = 16, w = 0;
    if (size / 100 > n) return NULL;  // (no stream expands that far: a damaged size)
    uint8_t *o = malloc(size ? size : 1);
    if (!o) return NULL;
    while (w < size && s < n) {
        uint8_t code = d[s++];
        for (int b = 0; b < 8 && w < size && s < n; b++) {
            if (code & (0x80 >> b)) { o[w++] = d[s++]; continue; }
            if (s + 1 >= n) break;
            size_t dist = (size_t)((d[s] & 0xF) << 8 | d[s + 1]) + 1, cnt = d[s] >> 4;
            s += 2;
            if (!cnt) cnt = s < n ? (size_t)d[s++] + 0x12 : 0;
            else cnt += 2;
            if (dist > w) { free(o); return NULL; }  // (damaged)
            for (size_t i = 0; i < cnt && w < size; i++, w++) o[w] = o[w - dist];
        }
    }
    if (w < size) { free(o); return NULL; }
    *out_n = size;
    return o;
}

// SARC (little-endian): the node of `name` (its index; start and end in the data, which begins at *data_off), or -1
int sarc_find(const uint8_t *d, size_t n, const char *name, uint32_t *data_off, uint32_t *start, uint32_t *end) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE || memcmp(d + 0x14, "SFAT", 4)) return -1;
    uint32_t doff, cnt = d[0x1A] | d[0x1B] << 8;
    memcpy(&doff, d + 0xC, 4);
    size_t names = 0x20 + (size_t)cnt * 16 + 8;
    if (names > n || doff > n) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t attr, s, e;
        memcpy(&attr, d + 0x20 + i * 16 + 4, 4);
        memcpy(&s, d + 0x20 + i * 16 + 8, 4);
        memcpy(&e, d + 0x20 + i * 16 + 12, 4);
        size_t at = names + (size_t)(attr & 0xFFFFFF) * 4;
        if (!(attr >> 24) || at >= n || strncmp((const char *)d + at, name, n - at)) continue;
        if (s > e || doff + (uint64_t)e > n) return -1;
        *data_off = doff, *start = s, *end = e;
        return (int)i;
    }
    return -1;
}

int sarc_count(const uint8_t *d, size_t n) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE || memcmp(d + 0x14, "SFAT", 4)) return -1;
    int cnt = d[0x1A] | d[0x1B] << 8;
    return 0x20 + (size_t)cnt * 16 + 8 > n ? -1 : cnt;
}

bool sarc_node(const uint8_t *d, size_t n, int i, const char **name, const uint8_t **data, size_t *size) {
    int cnt = sarc_count(d, n);
    uint32_t doff, attr, s, e;
    if (cnt < 0 || i < 0 || i >= cnt) return false;
    memcpy(&doff, d + 0xC, 4);
    memcpy(&attr, d + 0x20 + i * 16 + 4, 4);
    memcpy(&s, d + 0x20 + i * 16 + 8, 4);
    memcpy(&e, d + 0x20 + i * 16 + 12, 4);
    if (s > e || doff > n || doff + (uint64_t)e > n) return false;
    *name = NULL;
    if (attr >> 24) {
        size_t at = 0x20 + (size_t)cnt * 16 + 8 + (size_t)(attr & 0xFFFFFF) * 4;
        if (at >= n || !memchr(d + at, 0, n - at)) return false;
        *name = (const char *)d + at;
        if (**name == '/') (*name)++;
    }
    *data = d + doff + s;
    *size = e - s;
    return true;
}
