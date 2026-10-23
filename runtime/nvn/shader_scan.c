// Every NVN shader in the game's RomFS folder. BOTW compiles none at run time: they all ship compiled for the
// console's GPU, in Yaz0-compressed SARC packages (Pack/Bootup_Graphics.pack and the rest): BNSH files inside the
// materials' shader archives (Shader/uking_*.sbfsha), effects and events, and the agl engine's own .sharcb packages
// (magic BAHS) inside System/*.ssarc. Formats as MacNativeGames' prepare_shader_seed.py reads them (MIT,
// github.com/ly05010419/MacNativeGames). The game binds the same bytes (runtime/nvn/objects.c set_shaders), so a
// shader found here is one the renderer can translate and compile before the first draw that needs it.
#include <SDL3/SDL.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../rt.h"
#include "maxwell/shader.h"
#include "shader_scan.h"

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint64_t shader_code_id(const uint8_t *code, uint32_t max_size, int stage) {
    size_t n = maxwell_binary_size(code, max_size, stage);
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage;
    const uint8_t *b = code;
    for (; n >= 8; n -= 8, b += 8) { h = (h ^ rd64(b)) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return (h ^ (h >> 32)) | 1;
}

static _Thread_local int found;  // (worker thread) shaders the current file held

// ---- the result, shared by the worker threads (lock held): shaders by id (open addressing), programs as a set
typedef struct {
    pthread_mutex_t lock;
    ScanShader *shaders;
    int n, cap;
    int *slots;  // index + 1 into shaders, 0 = free
    int nslots;
    uint64_t *pairs;  // (vs << 32 | fs) + 1, 0 = free
    int npairs, pairs_cap;
} Table;

static int table_add(Table *t, int stage, const uint8_t *code, uint32_t size, uint32_t const_offset, const uint8_t *control,
                     uint32_t control_size) {
    uint32_t max = const_offset && const_offset <= size ? const_offset : size;
    uint64_t id = shader_code_id(code, max, stage);
    pthread_mutex_lock(&t->lock);
    if ((t->n + 1) * 2 > t->nslots) {
        int ns = t->nslots ? t->nslots * 2 : 1 << 14;
        int *s = calloc((size_t)ns, sizeof *s);
        for (int i = 0; i < t->n; i++) {
            uint32_t j = (uint32_t)t->shaders[i].id & (uint32_t)(ns - 1);
            while (s[j]) j = (j + 1) & (uint32_t)(ns - 1);
            s[j] = i + 1;
        }
        free(t->slots);
        t->slots = s;
        t->nslots = ns;
    }
    uint32_t j = (uint32_t)id & (uint32_t)(t->nslots - 1);
    for (; t->slots[j]; j = (j + 1) & (uint32_t)(t->nslots - 1)) {
        ScanShader *s = &t->shaders[t->slots[j] - 1];
        if (s->id == id && s->stage == stage) { pthread_mutex_unlock(&t->lock); return t->slots[j] - 1; }
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4096;
        t->shaders = realloc(t->shaders, sizeof *t->shaders * (size_t)t->cap);
    }
    uint8_t *c = malloc(size), *ctl = malloc(control_size);
    memcpy(c, code, size);
    memcpy(ctl, control, control_size);
    int index = t->n++;
    t->shaders[index] = (ScanShader){stage, c, size, const_offset, ctl, control_size, id};
    t->slots[j] = index + 1;
    pthread_mutex_unlock(&t->lock);
    return index;
}

static void table_pair(Table *t, int vs, int fs) {
    uint64_t key = ((uint64_t)vs << 32 | (uint32_t)fs) + 1;
    pthread_mutex_lock(&t->lock);
    if ((t->npairs + 1) * 2 > t->pairs_cap) {
        int cap = t->pairs_cap ? t->pairs_cap * 2 : 1 << 14;
        uint64_t *p = calloc((size_t)cap, sizeof *p);
        for (int i = 0; i < t->pairs_cap; i++) {
            if (!t->pairs[i]) continue;
            uint32_t j = (uint32_t)(t->pairs[i] * 0x9E3779B97F4A7C15ull >> 40) & (uint32_t)(cap - 1);
            while (p[j]) j = (j + 1) & (uint32_t)(cap - 1);
            p[j] = t->pairs[i];
        }
        free(t->pairs);
        t->pairs = p;
        t->pairs_cap = cap;
    }
    uint32_t j = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (uint32_t)(t->pairs_cap - 1);
    for (; t->pairs[j]; j = (j + 1) & (uint32_t)(t->pairs_cap - 1))
        if (t->pairs[j] == key) { pthread_mutex_unlock(&t->lock); return; }
    t->pairs[j] = key;
    t->npairs++;
    pthread_mutex_unlock(&t->lock);
}

// A shader by its control section and code, as NVN gets them: the stage word at +0x714 (0 vertex, 1 fragment,
// 5 compute), the constants' offset at +0x700 and the data's size at +0x704. Graphics code starts with NVN's header
// (0x12345678) and an SPH of the same stage, as set_shaders reads it. -1: not a shader this renderer draws with.
static int add_shader(Table *t, const uint8_t *code, uint64_t code_size, const uint8_t *control, uint64_t control_size) {
    if (control_size < 0x758) return -1;
    uint32_t kind = rd32(control + 0x714), const_offset = rd32(control + 0x700), size = rd32(control + 0x704);
    if (!size || size > code_size || const_offset > size) return -1;
    int stage = kind == 0 ? STAGE_VERTEX : kind == 1 ? STAGE_FRAGMENT : kind == 5 ? STAGE_COMPUTE : -1;
    if (stage < 0) return -1;
    bool graphics = size >= 0x80 && rd32(code) == 0x12345678;
    if (stage == STAGE_COMPUTE ? graphics : !graphics) return -1;
    if (graphics && ((rd32(code + 0x30) >> 10) & 0xF) != (stage == STAGE_VERTEX ? 1u : 5u)) return -1;
    found++;
    return table_add(t, stage, code, size, const_offset, control, (uint32_t)(control_size < 0x880 ? control_size : 0x880));
}

// BNSH: offsets relative to its own start. The variations (one per program) name their vertex, fragment and compute
// stages; a stage holds the offsets and sizes of its control section and code.
static void bnsh(Table *t, const uint8_t *d, size_t n) {
    if (n < 136 || d[12] != 0xFF || d[13] != 0xFE || memcmp(d + 96, "grsc", 4)) return;
    uint32_t count = rd32(d + 124);
    uint64_t var = rd64(d + 128);
    static const int stage_at[3] = {8, 40, 48};  // vertex, fragment, compute
    for (uint32_t i = 0; i < count && var + (uint64_t)i * 64 + 24 <= n; i++) {
        uint64_t prog = rd64(d + var + (uint64_t)i * 64 + 16);
        if (prog + 56 > n) continue;
        int idx[3] = {-1, -1, -1};
        for (int s = 0; s < 3; s++) {
            uint64_t off = rd64(d + prog + stage_at[s]);
            if (!off || off + 32 > n) continue;
            uint64_t control_off = rd64(d + off + 8), code_off = rd64(d + off + 16);
            uint32_t code_size = rd32(d + off + 24), control_size = rd32(d + off + 28);
            if (control_off + control_size > n || code_off + code_size > n) continue;
            idx[s] = add_shader(t, d + code_off, code_size, d + control_off, control_size);
        }
        if (idx[0] >= 0 && idx[1] >= 0) table_pair(t, idx[0], idx[1]);
    }
}

// agl's .sharcb (BAHS): between the records' start (+20) and the code pool (+28, its size at +24), each record is
// <u64 code offset in the pool, u32 ?, u32 control size, i32 control offset from the record>; records may share a
// control section. Found by their control size (0x880 / 0x88c, BOTW's 0x7d1 / 0x848).
static void sharcb(Table *t, const uint8_t *d, size_t n) {
    if (n < 32) return;
    uint32_t start = rd32(d + 20), pool_size = rd32(d + 24), pool = rd32(d + 28);
    if (start < 32 || start > pool || (uint64_t)pool + pool_size > n) return;
    static const uint32_t sizes[] = {0x880, 0x88c, 0x7d1, 0x848};
    for (uint32_t at = (start + 8 + 3) & ~3u; at + 4 <= pool; at += 4) {
        uint32_t w = rd32(d + at);
        if (w != sizes[0] && w != sizes[1] && w != sizes[2] && w != sizes[3]) continue;
        uint32_t record = at - 8;
        uint64_t code_off = rd64(d + record);
        int64_t control_at = (int64_t)record + (int32_t)rd32(d + record + 12);
        if (control_at < start || control_at + 0x758 > pool || code_off >= pool_size) continue;
        uint64_t control_size = pool - (uint64_t)control_at < 0x880 ? pool - (uint64_t)control_at : 0x880;
        add_shader(t, d + pool + code_off, pool_size - code_off, d + control_at, control_size);
    }
}

static uint8_t *yaz0(const uint8_t *in, size_t n, size_t *out_n) {
    if (n < 16) return NULL;
    uint32_t size = be32(in + 4);
    if (size > 1u << 30) return NULL;
    uint8_t *out = malloc(size ? size : 1);
    size_t i = 16, o = 0;
    while (o < size) {
        if (i >= n) goto bad;
        uint8_t code = in[i++];
        for (int b = 0; b < 8 && o < size; b++, code <<= 1) {
            if (code & 0x80) {
                if (i >= n) goto bad;
                out[o++] = in[i++];
                continue;
            }
            if (i + 1 >= n) goto bad;
            size_t dist = ((size_t)(in[i] & 0xF) << 8 | in[i + 1]) + 1, len = in[i] >> 4;
            i += 2;
            if (!len) {
                if (i >= n) goto bad;
                len = (size_t)in[i++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > o) goto bad;
            if (len > size - o) len = size - o;
            for (size_t k = 0; k < len; k++, o++) out[o] = out[o - dist];
        }
    }
    *out_n = size;
    return out;
bad:
    free(out);
    return NULL;
}

static const uint8_t *find8(const uint8_t *p, const uint8_t *end, const char *pat) {  // (memmem: not on Windows)
    for (; end - p >= 8; p++) {
        p = memchr(p, pat[0], (size_t)(end - p - 7));
        if (!p) return NULL;
        if (!memcmp(p, pat, 8)) return p;
    }
    return NULL;
}

static void visit(Table *t, const uint8_t *d, size_t n, int depth);

static void sarc(Table *t, const uint8_t *d, size_t n, int depth) {
    if (n < 0x20) return;
    bool le = d[6] == 0xFF && d[7] == 0xFE;
    uint16_t hl = le ? rd16(d + 4) : be16(d + 4);
    uint32_t data_off = le ? rd32(d + 0xC) : be32(d + 0xC);
    if ((size_t)hl + 12 > n || memcmp(d + hl, "SFAT", 4)) return;
    uint16_t count = le ? rd16(d + hl + 6) : be16(d + hl + 6);
    const uint8_t *nodes = d + hl + 12;
    if ((size_t)hl + 12 + (size_t)count * 16 > n) return;
    for (int i = 0; i < count; i++) {
        uint32_t s = le ? rd32(nodes + i * 16 + 8) : be32(nodes + i * 16 + 8);
        uint32_t e = le ? rd32(nodes + i * 16 + 12) : be32(nodes + i * 16 + 12);
        if (s <= e && (uint64_t)data_off + e <= n) visit(t, d + data_off + s, e - s, depth + 1);
    }
}

static void visit(Table *t, const uint8_t *d, size_t n, int depth) {
    if (n >= 16 && !memcmp(d, "Yaz0", 4)) {
        size_t m;
        uint8_t *u = yaz0(d, n, &m);
        if (u) visit(t, u, m, depth);
        free(u);
        return;
    }
    if (n >= 0x20 && !memcmp(d, "SARC", 4)) {
        if (depth < 6) sarc(t, d, n, depth);
        return;
    }
    if (n >= 32 && !memcmp(d, "BAHS", 4)) sharcb(t, d, n);
    for (const uint8_t *p = d, *end = d + n; (p = find8(p, end, "BNSH\0\0\0\0"));) {
        size_t size = (size_t)(end - p) >= 32 ? rd32(p + 28) : 0;
        if (size > (size_t)(end - p)) size = (size_t)(end - p);
        bnsh(t, p, size);
        p += size > 8 ? size : 8;
    }
}

// ---- the files, spread over worker threads
typedef struct {
    Table table;
    char **paths;
    int npaths, cap;
    _Atomic int next, with_shaders;
} Scan;

static void list_files(Scan *s, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(path, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { list_files(s, path); continue; }
        if (s->npaths == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 1024;
            s->paths = realloc(s->paths, sizeof *s->paths * (size_t)s->cap);
        }
        s->paths[s->npaths++] = strdup(path);
    }
    closedir(d);
}

static void *scan_worker(void *arg) {
    Scan *s = arg;
    for (int i; (i = atomic_fetch_add(&s->next, 1)) < s->npaths;) {
        FILE *f = fopen(s->paths[i], "rb");
        uint8_t *buf = NULL;
        long n = 0;
        if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n)) &&
            fread(buf, 1, (size_t)n, f) == (size_t)n) {
            found = 0;
            visit(&s->table, buf, (size_t)n, 0);
            if (found) atomic_fetch_add(&s->with_shaders, 1);
        }
        free(buf);
        if (f) fclose(f);
    }
    return NULL;
}

// The game's main module keeps a few BNSH files in its read-only data (37 shaders in 1.8.2: the title screen's
// among them): its segments, as the loader decompresses them (runtime/loader.c).
static void scan_module(Scan *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *raw = NULL;
    long n = 0;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0x100 && !fseek(f, 0, SEEK_SET) && (raw = malloc((size_t)n)) &&
        fread(raw, 1, (size_t)n, f) == (size_t)n && !memcmp(raw, "NSO0", 4)) {
        uint32_t flags = rd32(raw + 0xC);
        for (int i = 1; i < 3; i++) {  // rodata, data
            uint32_t file_off = rd32(raw + 0x10 + i * 0x10), size = rd32(raw + 0x18 + i * 0x10), csize = rd32(raw + 0x60 + i * 4);
            if ((uint64_t)file_off + csize > (uint64_t)n || size > 1u << 30) continue;
            uint8_t *seg = malloc(size ? size : 1);
            size_t got = flags & (1u << i) ? lz4_decompress(raw + file_off, csize, seg, size) : (memcpy(seg, raw + file_off, size), size);
            found = 0;
            if (got == size) visit(&s->table, seg, size, 0);
            if (found) atomic_fetch_add(&s->with_shaders, 1);
            free(seg);
        }
    }
    free(raw);
    fclose(f);
}

static int by_stage_id(const void *a, const void *b) {
    const ScanShader *x = a, *y = b;
    if (x->stage != y->stage) return x->stage - y->stage;
    return x->id < y->id ? -1 : x->id > y->id;
}

bool shader_scan(const char *romfs_dir, const char *main_nso, ScanResult *out, char *why, size_t why_size) {
    memset(out, 0, sizeof *out);
    Scan *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->table.lock, NULL);
    static const char *folders[] = {"Pack", "Effect", "Event", "System"};
    for (int i = 0; i < 4; i++) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%s", romfs_dir, folders[i]);
        list_files(s, dir);
    }
    int nthreads = SDL_GetNumLogicalCPUCores();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    pthread_t th[16];
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, scan_worker, s);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    if (main_nso) scan_module(s, main_nso);
    Table *t = &s->table;
    // in a fixed order (stage, id) whatever order the threads found them in; the programs follow
    int *order = malloc(sizeof *order * (size_t)(t->n ? t->n : 1));
    ScanShader *sorted = malloc(sizeof *sorted * (size_t)(t->n ? t->n : 1));
    memcpy(sorted, t->shaders, sizeof *sorted * (size_t)t->n);
    qsort(sorted, (size_t)t->n, sizeof *sorted, by_stage_id);
    for (int i = 0; i < t->n; i++) {
        ScanShader key = sorted[i];
        int j = 0;
        uint32_t k = (uint32_t)key.id & (uint32_t)(t->nslots - 1);
        for (; t->slots[k]; k = (k + 1) & (uint32_t)(t->nslots - 1))
            if (t->shaders[t->slots[k] - 1].id == key.id && t->shaders[t->slots[k] - 1].stage == key.stage) { j = t->slots[k] - 1; break; }
        order[j] = i;
    }
    out->shaders = sorted;
    out->nshaders = t->n;
    out->programs = malloc(sizeof *out->programs * (size_t)(t->npairs ? t->npairs : 1));
    for (int i = 0; i < t->pairs_cap; i++) {
        if (!t->pairs[i]) continue;
        uint64_t key = t->pairs[i] - 1;
        out->programs[out->nprograms++] = (ScanProgram){order[key >> 32], order[(uint32_t)key]};
    }
    out->files = s->with_shaders;
    free(order);
    free(t->shaders);
    free(t->slots);
    free(t->pairs);
    for (int i = 0; i < s->npaths; i++) free(s->paths[i]);
    free(s->paths);
    pthread_mutex_destroy(&t->lock);
    free(s);
    if (!out->nshaders) {
        snprintf(why, why_size, "no shaders in %s", romfs_dir);
        return false;
    }
    return true;
}

void shader_scan_free(ScanResult *r) {
    for (int i = 0; i < r->nshaders; i++) {
        free((void *)r->shaders[i].code);
        free((void *)r->shaders[i].control);
    }
    free(r->shaders);
    free(r->programs);
    memset(r, 0, sizeof *r);
}
