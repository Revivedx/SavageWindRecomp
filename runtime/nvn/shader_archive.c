// The game's NVN shader binaries from its RomFS image (see shader_archive.h).
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../lz4.h"
#include "../romfs_image.h"
#include "../rt.h"
#include "maxwell/shader.h"
#include "shader_archive.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint64_t shader_code_hash(const uint8_t *code, size_t max_size, int stage, uint32_t *bin_size) {
    size_t n = maxwell_binary_size(code, max_size, stage);
    if (bin_size) *bin_size = (uint32_t)n;
    uint64_t h = 0xcbf29ce484222325ull ^ n ^ ((uint64_t)stage << 56);
    const uint8_t *p = code;
    for (; n >= 8; n -= 8, p += 8) { uint64_t w; memcpy(&w, p, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, p++) h = (h ^ *p) * 0x100000001B3ull;
    return h ? h : 1;
}

// ------------------------------------------------------------------ the set
typedef struct { uint64_t *keys; int32_t *vals; size_t cap, n; } Index;  // shader hash -> index in shaders

static int index_get(Index *x, uint64_t h) {
    if (!x->cap) return -1;
    for (size_t i = h & (x->cap - 1); x->keys[i]; i = (i + 1) & (x->cap - 1))
        if (x->keys[i] == h) return x->vals[i];
    return -1;
}

static void index_put(Index *x, uint64_t h, int32_t v) {
    if ((x->n + 1) * 2 > x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 4096;
        uint64_t *k = calloc(cap, sizeof *k);
        int32_t *vv = calloc(cap, sizeof *vv);
        for (size_t i = 0; i < x->cap; i++) {
            if (!x->keys[i]) continue;
            size_t j = x->keys[i] & (cap - 1);
            while (k[j]) j = (j + 1) & (cap - 1);
            k[j] = x->keys[i];
            vv[j] = x->vals[i];
        }
        free(x->keys);
        free(x->vals);
        x->keys = k;
        x->vals = vv;
        x->cap = cap;
    }
    size_t j = h & (x->cap - 1);
    while (x->keys[j]) j = (j + 1) & (x->cap - 1);
    x->keys[j] = h;
    x->vals[j] = v;
    x->n++;
}

// A stage binary (code, control section) into the set; its index (an earlier one with the same code), -1 if unusable.
static int add_shader(SaSet *set, int stage, const uint8_t *code, size_t size, const uint8_t *ctl, size_t ctl_size,
                      int source) {
    if (!size || size > (1u << 20) || ctl_size < 0x760) return -1;
    if ((stage == STAGE_COMPUTE) == (rd32(code) == 0x12345678)) return -1;  // (graphics stages have the NVN header)
    uint32_t co = rd32(ctl + 0x700);  // compiler constants after the code (bound as c1)
    uint32_t max_size = co && co <= size ? co : (uint32_t)size;
    uint64_t h = shader_code_hash(code, max_size, stage, NULL);
    Index *x = set->index;
    int i = index_get(x, h);
    if (i >= 0) return i;
    if (set->nshaders == set->cap_shaders) {
        set->cap_shaders = set->cap_shaders ? set->cap_shaders * 2 : 1024;
        set->shaders = realloc(set->shaders, sizeof *set->shaders * (size_t)set->cap_shaders);
    }
    SaShader *s = &set->shaders[set->nshaders];
    memset(s, 0, sizeof *s);
    s->stage = (uint8_t)stage;
    s->size = (uint32_t)size;
    s->max_size = co ? co : 0x10000;
    if (stage == STAGE_COMPUTE) {
        for (int k = 0; k < 3; k++) s->local[k] = rd32(ctl + 0x748 + 4 * k) ? rd32(ctl + 0x748 + 4 * k) : 1;
        s->shared = rd32(ctl + 0x754);
    }
    s->hash = h;
    s->code = malloc(size);
    memcpy(s->code, code, size);
    s->source = (uint16_t)source;
    index_put(x, h, set->nshaders);
    return set->nshaders++;
}

static void add_program(SaSet *set, int vs, int fs, int cs, int source) {
    if (set->nprograms == set->cap_programs) {
        set->cap_programs = set->cap_programs ? set->cap_programs * 2 : 1024;
        set->programs = realloc(set->programs, sizeof *set->programs * (size_t)set->cap_programs);
    }
    set->programs[set->nprograms++] = (SaProgram){vs, fs, cs, (uint16_t)source};
}

// ------------------------------------------------------------------ BNSH (nn::gfx shader file)
// Offsets in it are relative to its start. grsc (shader container): +0x1C variation count, +0x20 variation array
// (0x40 bytes each, +0x10 the binary program). Program: +8 + 8 * stage the code of vertex, hull, domain, geometry,
// pixel, compute. Code (NVN): +8 control, +0x10 data, +0x18 data size, +0x1C control size.
static void bnsh(SaSet *set, const uint8_t *d, size_t n, int source) {
    const uint8_t *g = NULL;
    for (size_t o = 4; o + 0x30 <= n && o < 0x200; o += 4)
        if (!memcmp(d + o, "grsc", 4)) { g = d + o; break; }
    if (!g) return;
    uint32_t nvar = rd32(g + 0x1C);
    uint64_t varr = rd64(g + 0x20);
    if (varr > n || nvar > (n - varr) / 0x40) return;
    for (uint32_t v = 0; v < nvar; v++) {
        uint64_t prog = rd64(d + varr + v * 0x40 + 0x10);
        if (!prog || prog + 0x38 > n) continue;
        int idx[6];
        for (int s = 0; s < 6; s++) {
            idx[s] = -1;
            uint64_t c = rd64(d + prog + 8 + 8 * s);
            if (!c || c + 0x20 > n) continue;
            uint64_t ctl = rd64(d + c + 8), dat = rd64(d + c + 0x10);
            uint32_t dsz = rd32(d + c + 0x18), csz = rd32(d + c + 0x1C);
            if (ctl > n || dat > n || dsz > n - dat || csz > n - ctl) continue;
            static const int stage_of[6] = {STAGE_VERTEX, STAGE_TESS_CTRL, STAGE_TESS_EVAL, STAGE_GEOMETRY, STAGE_FRAGMENT,
                                            STAGE_COMPUTE};
            if (s == 0 || s == 4 || s == 5) idx[s] = add_shader(set, stage_of[s], d + dat, dsz, d + ctl, csz, source);
        }
        if (idx[0] >= 0 && idx[4] >= 0) add_program(set, idx[0], idx[4], -1, source);
        else if (idx[5] >= 0) add_program(set, -1, -1, idx[5], source);
    }
}

static void bnsh_all(SaSet *set, const uint8_t *d, size_t n, int source) {  // (inside FSHA, VFXB)
    for (size_t o = 0; o + 0x100 <= n; o += 4)
        if (!memcmp(d + o, "BNSH", 4)) bnsh(set, d + o, n - o, source);
}

// ------------------------------------------------------------------ agl shader archive (SHARCB, "BAHS")
// +0x1C: where the code region starts. Each stage of a program has a record: +8 record size (s), +0xC stage
// (0 vertex, 1 fragment, 3 compute), +0x14 s - 0x10, +0x18 code offset in the code region, +0x20 control section
// size, +0x24 the control section, relative (signed, from +0x24, less 0xC). A program's vertex record comes right before its
// fragment record. Control +0x704: bytes of code.
static void sharcb(SaSet *set, const uint8_t *d, size_t n, int source) {
    if (n < 0x40) return;
    uint32_t region = rd32(d + 0x1C);
    if (region >= n) return;
    int last_vs = -1;
    for (size_t p = 0; p + 0x28 <= region; p += 4) {
        uint32_t rs = rd32(d + p + 8), st = rd32(d + p + 0xC);
        if (rs < 0x28 || rs > 0x10000 || rd32(d + p + 0x14) != rs - 0x10 || (st != 0 && st != 1 && st != 3))
            continue;
        int64_t ctl = (int64_t)p + 0x24 + (int32_t)rd32(d + p + 0x24) - 0xC;
        uint32_t csz = rd32(d + p + 0x20);
        if (csz < 0x760 || csz > 0x10000 || ctl < 0 || (size_t)ctl + csz > n || rd32(d + ctl) != 0x98761234) continue;
        uint64_t code = (uint64_t)region + rd32(d + p + 0x18);
        uint32_t size = rd32(d + ctl + 0x704);
        if (code > n || size > n - code) continue;
        int stage = st == 0 ? STAGE_VERTEX : st == 1 ? STAGE_FRAGMENT : STAGE_COMPUTE;
        int i = add_shader(set, stage, d + code, size, d + ctl, csz, source);
        if (stage == STAGE_VERTEX) last_vs = i;
        else if (stage == STAGE_FRAGMENT && last_vs >= 0 && i >= 0) { add_program(set, last_vs, i, -1, source); last_vs = -1; }
        else if (stage == STAGE_COMPUTE && i >= 0) add_program(set, -1, -1, i, source);
        p += rs - 4;  // (records do not overlap)
    }
}

// ------------------------------------------------------------------ containers
static uint8_t *yaz0(const uint8_t *d, size_t n, size_t *out_n) {
    uint32_t size = be32(d + 4);
    uint8_t *o = malloc(size ? size : 1);
    if (!o) return NULL;
    size_t s = 16, w = 0;
    while (w < size && s < n) {
        uint8_t code = d[s++];
        for (int b = 0; b < 8 && w < size; b++) {
            if (code & (0x80 >> b)) {
                if (s >= n) goto done;
                o[w++] = d[s++];
            } else {
                if (s + 1 >= n) goto done;
                uint8_t b1 = d[s], b2 = d[s + 1];
                s += 2;
                size_t dist = (size_t)(((b1 & 0xF) << 8) | b2) + 1, cnt = b1 >> 4;
                if (!cnt) {
                    if (s >= n) goto done;
                    cnt = (size_t)d[s++] + 0x12;
                } else {
                    cnt += 2;
                }
                if (dist > w) goto done;  // (damaged)
                for (size_t i = 0; i < cnt && w < size; i++, w++) o[w] = o[w - dist];
            }
        }
    }
done:
    *out_n = w;
    return o;
}

static bool wanted(const char *name) {
    static const char *const ext[] = {".pack", ".sbfsha", ".bfsha", ".sesetlist", ".ssarc", ".sarc", ".sblarc", ".bnsh",
                                      ".sharcb", ".sbeventpack"};
    const char *e = strrchr(name, '.');
    if (!e) return false;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcmp(e, ext[i])) return true;
    return false;
}

static int add_source(SaSet *set, const char *path) {
    set->sources = realloc(set->sources, sizeof *set->sources * (size_t)(set->nsources + 1));
    set->sources[set->nsources] = strdup(path);
    return set->nsources++;
}

static void scan_data(SaSet *set, const char *path, const uint8_t *d, size_t n, int depth);

static void sarc(SaSet *set, const char *path, const uint8_t *d, size_t n, int depth) {
    if (n < 0x14) return;
    bool be = d[6] == 0xFE && d[7] == 0xFF;
    #define R16(p) (be ? (uint16_t)((p)[0] << 8 | (p)[1]) : (uint16_t)((p)[0] | (p)[1] << 8))
    #define R32(p) (be ? be32(p) : rd32(p))
    uint32_t hs = R16(d + 4), data = R32(d + 0xC);
    if (hs + 0xC > n || memcmp(d + hs, "SFAT", 4)) return;
    uint32_t nodes = R16(d + hs + 6);
    size_t nt = hs + 0xC, sfnt = nt + (size_t)nodes * 16;
    if (sfnt + 8 > n || memcmp(d + sfnt, "SFNT", 4)) return;
    size_t names = sfnt + 8;
    for (uint32_t i = 0; i < nodes; i++) {
        const uint8_t *e = d + nt + i * 16;
        uint32_t attr = R32(e + 4), a = R32(e + 8), b = R32(e + 12);
        if (!(attr >> 24) || (size_t)data + b > n || a > b) continue;
        size_t no = names + (attr & 0xFFFFFF) * 4;
        if (no >= n) continue;
        const char *name = (const char *)d + no;
        size_t nl = strnlen(name, n - no);
        if (nl == n - no || !wanted(name)) continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%.*s", path, (int)nl, name);
        scan_data(set, sub, d + data + a, b - a, depth + 1);
    }
    #undef R16
    #undef R32
}

static void scan_data(SaSet *set, const char *path, const uint8_t *d, size_t n, int depth) {
    if (n < 16 || depth > 4) return;
    if (!memcmp(d, "Yaz0", 4)) {
        size_t m = 0;
        uint8_t *u = yaz0(d, n, &m);
        if (u) scan_data(set, path, u, m, depth + 1);
        free(u);
        return;
    }
    if (!memcmp(d, "SARC", 4)) { sarc(set, path, d, n, depth); return; }
    if (!memcmp(d, "BAHS", 4)) sharcb(set, d, n, add_source(set, path));
    else if (!memcmp(d, "BNSH", 4)) bnsh(set, d, n, add_source(set, path));
    else if (!memcmp(d, "FSHA", 4) || !memcmp(d, "VFXB", 4)) bnsh_all(set, d, n, add_source(set, path));
}

// ------------------------------------------------------------------ the RomFS image
// header: size, dir hash (off, size), dir meta (off, size), file hash (off, size), file meta (off, size), data off.
// File meta: parent dir, sibling, data offset, size, hash, name length, name. Dir meta: parent, sibling, first dir,
// first file, hash, name length, name.
static void dir_path(const uint8_t *dm, size_t dsz, uint32_t dir, char *out, size_t cap) {
    char tmp[1024] = "";
    for (int guard = 0; dir && dir + 0x18 <= dsz && guard < 64; guard++) {
        uint32_t nl = rd32(dm + dir + 0x14);
        if (dir + 0x18 + nl > dsz) break;
        char seg[300];
        snprintf(seg, sizeof seg, "%.*s/%s", (int)nl, (const char *)dm + dir + 0x18, tmp);
        snprintf(tmp, sizeof tmp, "%s", seg);
        dir = rd32(dm + dir);
    }
    snprintf(out, cap, "%s", tmp);
}

// ------------------------------------------------------------------ the game's executable
// exefs/main, next to the romfs/ folder: its data segment has nn::gfx shader files of its own (BOTW 1.8.2: two, with
// shaders the game draws that no archive of the RomFS has)
static void scan_exefs(SaSet *set, const char *romfs) {
    char path[1100];
    snprintf(path, sizeof path, "%s", romfs);
    char *end = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > end) end = bs;
    snprintf(end ? end + 1 : path, sizeof path - (size_t)(end ? end + 1 - path : 0), "exefs/main");
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return;
    off_t n = lseek(fd, 0, SEEK_END);
    uint8_t *raw = n > 0x100 && n < (256 << 20) ? malloc((size_t)n) : NULL;
    if (raw && pread(fd, raw, (size_t)n, 0) == (ssize_t)n && !memcmp(raw, "NSO0", 4)) {
        uint32_t flags = rd32(raw + 0xC);
        int source = add_source(set, "exefs/main");
        for (int i = 1; i < 3; i++) {  // (read-only data, data)
            uint32_t off = rd32(raw + 0x10 + i * 0x10), size = rd32(raw + 0x18 + i * 0x10), csize = rd32(raw + 0x60 + i * 4);
            bool lz4 = flags & (1 << i);
            if (off > n || csize > n - off || size > (128u << 20) || (!lz4 && csize < size)) continue;
            uint8_t *seg = lz4 ? malloc(size ? size : 1) : NULL;
            if (!lz4 || (seg && lz4_decompress(raw + off, csize, seg, size) == size)) {
                set->files++;
                set->bytes += size;
                bnsh_all(set, lz4 ? seg : raw + off, size, source);
            }
            free(seg);
        }
    }
    free(raw);
    close(fd);
}

bool shader_archive_scan(const char *romfs, SaSet *set, void (*progress)(float, void *), void *user, char *why, size_t wn) {
    memset(set, 0, sizeof *set);
    set->index = calloc(1, sizeof(Index));
    char open_why[256] = "";
    RomfsImage *img = romfs_image_open(romfs, open_why, sizeof open_why);
    if (!img) { snprintf(why, wn, "cannot read %s: %s", romfs, open_why); return false; }
    uint64_t h[10];
    if (romfs_image_pread(img, h, sizeof h, 0) != (ssize_t)sizeof h || h[4] > (64u << 20) || h[8] > (64u << 20)) {
        romfs_image_close(img);
        snprintf(why, wn, "%s is not a RomFS image", romfs);
        return false;
    }
    uint8_t *dm = malloc(h[4]), *fm = malloc(h[8]);
    if (romfs_image_pread(img, dm, h[4], h[3]) != (ssize_t)h[4] ||
        romfs_image_pread(img, fm, h[8], h[7]) != (ssize_t)h[8]) {
        free(dm); free(fm); romfs_image_close(img);
        snprintf(why, wn, "%s: cannot read its tables", romfs);
        return false;
    }
    uint64_t total = 0, done = 0;
    for (int pass = 0; pass < 2; pass++) {  // (sizes first, for the progress)
        for (uint64_t o = 0; o + 0x20 <= h[8];) {
            uint32_t parent = rd32(fm + o), nl = rd32(fm + o + 0x1C);
            uint64_t data = rd64(fm + o + 8), size = rd64(fm + o + 0x10);
            if (o + 0x20 + nl > h[8]) break;
            char name[300], path[1400];
            snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)fm + o + 0x20);
            o += 0x20 + ((nl + 3) & ~3u);
            if (!wanted(name) || size > (1ull << 31)) continue;
            if (!pass) { total += size; continue; }
            char dir[1024];
            dir_path(dm, h[4], parent, dir, sizeof dir);
            snprintf(path, sizeof path, "%s%s", dir, name);
            uint8_t *buf = malloc(size ? size : 1);
            if (buf && romfs_image_pread(img, buf, size, h[9] + data) == (ssize_t)size) {
                set->files++;
                set->bytes += size;
                scan_data(set, path, buf, size, 0);
            }
            free(buf);
            done += size;
            if (progress && total) progress((float)((double)done / (double)total), user);
        }
    }
    free(dm);
    free(fm);
    romfs_image_close(img);
    scan_exefs(set, romfs);
    return true;
}

int shader_archive_find(const SaSet *set, uint64_t hash) { return set->index ? index_get(set->index, hash) : -1; }

void shader_archive_free(SaSet *set) {
    for (int i = 0; i < set->nshaders; i++) free(set->shaders[i].code);
    for (int i = 0; i < set->nsources; i++) free(set->sources[i]);
    free(set->shaders);
    free(set->programs);
    free(set->sources);
    Index *x = set->index;
    if (x) { free(x->keys); free(x->vals); free(x); }
    memset(set, 0, sizeof *set);
}
