// BYML and MSBT readers: see formats.h.
#include "formats.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

// ---- BYML
bool byml_open(Byml *b, const uint8_t *d, size_t n) {
    if (!d || n < 16 || memcmp(d, "YB", 2)) return false;
    *b = (Byml){d, n, rd32(d + 4), rd32(d + 8), rd32(d + 12)};
    return true;
}

static const char *table_str(const Byml *b, uint32_t table, uint32_t i) {
    if (!table || table + 8 > b->n || i >= (rd32(b->d + table) >> 8)) return NULL;
    uint32_t o = table + rd32(b->d + table + 4 + 4 * i);
    return o < b->n ? (const char *)b->d + o : NULL;
}
const char *byml_str(const Byml *b, uint32_t i) { return table_str(b, b->strs, i); }

uint32_t byml_count(const Byml *b, uint32_t node) { return node + 4 <= b->n ? rd32(b->d + node) >> 8 : 0; }

bool byml_at(const Byml *b, uint32_t node, uint32_t i, uint8_t *type, uint32_t *value, const char **key) {
    if (node + 4 > b->n || i >= byml_count(b, node)) return false;
    if (b->d[node] == BYML_DICT) {  // (entries of 8 bytes: the key's index (24 bits), the type, the value)
        size_t e = node + 4 + 8 * (size_t)i;
        if (e + 8 > b->n) return false;
        *type = b->d[e + 3], *value = rd32(b->d + e + 4);
        if (key) *key = table_str(b, b->keys, rd32(b->d + e) & 0xffffff);
        return true;
    }
    if (b->d[node] != BYML_ARRAY) return false;  // (the types, a byte each, to 4 bytes, then the values)
    size_t vals = node + 4 + ((byml_count(b, node) + 3) & ~3u);
    if (vals + 4 * (size_t)i + 4 > b->n) return false;
    *type = b->d[node + 4 + i], *value = rd32(b->d + vals + 4 * i);
    if (key) *key = NULL;
    return true;
}

bool byml_get(const Byml *b, uint32_t node, const char *key, uint8_t *type, uint32_t *value) {
    if (node + 4 > b->n || b->d[node] != BYML_DICT) return false;
    uint32_t n = byml_count(b, node);
    for (uint32_t i = 0; i < n; i++) {
        const char *k;
        if (byml_at(b, node, i, type, value, &k) && k && !strcmp(k, key)) return true;
    }
    return false;
}

const char *byml_get_str(const Byml *b, uint32_t node, const char *key) {
    uint8_t t; uint32_t v;
    return byml_get(b, node, key, &t, &v) && t == BYML_STR ? byml_str(b, v) : NULL;
}

int byml_get_int(const Byml *b, uint32_t node, const char *key, int def) {
    uint8_t t; uint32_t v;
    return byml_get(b, node, key, &t, &v) && (t == BYML_INT || t == BYML_UINT) ? (int)v : def;
}

bool byml_get_float(const Byml *b, uint32_t node, const char *key, float *out) {
    uint8_t t; uint32_t v;
    if (!byml_get(b, node, key, &t, &v) || t != BYML_FLOAT) return false;
    memcpy(out, &v, 4);
    return true;
}

bool byml_at_float(const Byml *b, uint32_t node, uint32_t i, float *out) {
    uint8_t t; uint32_t v;
    if (!byml_at(b, node, i, &t, &v, NULL) || t != BYML_FLOAT) return false;
    memcpy(out, &v, 4);
    return true;
}

// ---- MSBT: the label's text, UTF-16 to UTF-8, control tags left out
static void utf8_put(char **o, char *end, uint32_t c) {
    char *p = *o;
    if (c < 0x80) { if (p + 1 < end) *p++ = (char)c; }
    else if (c < 0x800) { if (p + 2 < end) *p++ = (char)(0xc0 | c >> 6), *p++ = (char)(0x80 | (c & 63)); }
    else if (p + 3 < end) *p++ = (char)(0xe0 | c >> 12), *p++ = (char)(0x80 | ((c >> 6) & 63)), *p++ = (char)(0x80 | (c & 63));
    *o = p;
}

void msbt_each(const uint8_t *d, size_t n, void (*fn)(void *ctx, const char *label, size_t label_len, const char *text),
               void *ctx) {
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8) || d[8] != 0xff) return;  // (little-endian only, as the Switch's are)
    uint16_t nsec = rd16(d + 0xe);
    size_t off = 0x20, lbl = 0, txt = 0, txt_size = 0;
    for (int s = 0; s < nsec && off + 0x10 <= n; s++) {
        uint32_t size = rd32(d + off + 4);
        if (!memcmp(d + off, "LBL1", 4)) lbl = off + 0x10;
        if (!memcmp(d + off, "TXT2", 4)) txt = off + 0x10, txt_size = size;
        off = (off + 0x10 + size + 15) & ~(size_t)15;
    }
    if (!lbl || !txt || txt + 4 > n) return;
    uint32_t ntxt = rd32(d + txt), nb = rd32(d + lbl);
    for (uint32_t b = 0; b < nb && lbl + 12 + 8 * b <= n; b++) {
        uint32_t cnt = rd32(d + lbl + 4 + 8 * b);
        size_t p = lbl + rd32(d + lbl + 8 + 8 * b);
        for (uint32_t k = 0; k < cnt && p < n; k++) {
            uint8_t len = d[p];
            if (p + 1 + len + 4 > n) return;
            uint32_t idx = rd32(d + p + 1 + len);
            if (idx < ntxt) {
                size_t t0 = txt + rd32(d + txt + 4 + 4 * idx);
                size_t t1 = idx + 1 < ntxt ? txt + rd32(d + txt + 8 + 4 * idx) : txt + txt_size;
                char buf[256], *o = buf;
                for (size_t q = t0; q + 1 < t1 && q + 1 < n;) {
                    uint16_t c = rd16(d + q);
                    q += 2;
                    if (!c) break;
                    if (c == 0x0e) { if (q + 6 <= n) q += 6 + rd16(d + q + 4); continue; }  // (a tag: group, type, size, data)
                    if (c >= 0xd800 && c < 0xdc00 && q + 1 < n) {
                        uint16_t lo = rd16(d + q);
                        q += 2;
                        utf8_put(&o, buf + sizeof buf, 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00));
                        continue;
                    }
                    utf8_put(&o, buf + sizeof buf, c);
                }
                *o = 0;
                fn(ctx, (const char *)d + p + 1, len, buf);
            }
            p += 1 + len + 4;
        }
    }
}
