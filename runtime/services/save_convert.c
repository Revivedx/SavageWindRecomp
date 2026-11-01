// Wii U / Cemu save import: see save_convert.h. The conversion follows tools/botw_save_convert.py step by step (same
// output, byte for byte, for Wii U -> Switch); the comments say where a choice comes from rather than what a line does.
#include "save_convert.h"

#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "../platform.h"
#include "archive.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#endif

#define PATH_CAP 1100
#define HEADER 0xC          // flag save files: revision, 0xFFFFFFFF, 1; then {hash, value} entries; then 0xFFFFFFFF
#define PROFILE_DEPTH 10    // how far below the picked folder the profile may be (find_profile_root)
#define PROFILE_VISITS 100000  // folders looked at at most, and for at most PROFILE_SECONDS: a drive or a big projects
#define PROFILE_SECONDS 4      // folder picked by mistake must not run for minutes

// ------------------------------------------------------------------ small helpers
static void fail(ScReport *r, ScCode c, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(ScReport *r, ScCode c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    r->code = c;
    vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);
}

static bool path_join(char *out, size_t cap, const char *a, const char *b) {
    return (size_t)snprintf(out, cap, "%s/%s", a, b) < cap;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The names in a folder (not . and ..), sorted; NULL if it can't be read. Free with names_free.
static char **names_list(const char *dir, int *n) {
    DIR *d = opendir(dir);
    *n = 0;
    if (!d) return NULL;
    char **v = NULL;
    int cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 16;
            char **nv = realloc(v, sizeof *v * cap);
            if (!nv) goto fail;
            v = nv;
        }
        v[cnt] = strdup(e->d_name);
        if (!v[cnt]) goto fail;
        cnt++;
    }
    if (!v) v = malloc(sizeof *v);
    if (!v) goto fail;
    qsort(v, cnt, sizeof *v, cmp_str);
    *n = cnt;
    closedir(d);
    return v;
fail:
    for (int i = 0; i < cnt; i++) free(v[i]);
    free(v);
    closedir(d);
    errno = ENOMEM;
    return NULL;
}

static void names_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

// Reads a whole file (max: a size no file here comes close to). NULL with errno-style text in why.
static uint8_t *read_file(const char *path, size_t *n, size_t max, char *why, size_t whycap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, whycap, "%s: %s", path, strerror(errno));
        return NULL;
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 1 << 16;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); snprintf(why, whycap, "%s: out of memory", path); return NULL; }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
        if (len > max) { free(buf); fclose(f); snprintf(why, whycap, "%s: too big", path); return NULL; }
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); snprintf(why, whycap, "%s: read error", path); return NULL; }
    *n = len;
    return buf ? buf : malloc(1);
}

static uint32_t rd32(const uint8_t *p, bool le) {
    return le ? (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24;
}

static void wr32(uint8_t *p, uint32_t v, bool le) {
    for (int i = 0; i < 4; i++) p[le ? i : 3 - i] = (uint8_t)(v >> (8 * i));
}

static uint32_t crc32_name(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static bool buf_reserve(Buf *b, size_t more) {
    if (b->n + more <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + more) cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) return false;
    b->p = np, b->cap = cap;
    return true;
}

static bool buf_put(Buf *b, const void *d, size_t n) {
    if (!buf_reserve(b, n)) return false;
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return true;
}

static bool cancel_requested(const ScCancel *cancel) {
    return cancel && cancel->requested && cancel->requested(cancel->data);
}

static bool cancelled(const ScCancel *cancel, ScReport *r) {
    if (!cancel_requested(cancel)) return false;
    fail(r, SC_CANCELLED, "");
    return true;
}

// ------------------------------------------------------------------ files and folders this run made
// Everything a conversion or a backup creates is noted here, so that undoing it removes those and nothing else.
typedef struct { char *path; bool dir; } Made;
typedef struct { Made *v; int n, cap; } MadeList;

static bool made_add(MadeList *m, const char *path, bool dir) {
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 64;
        Made *v = realloc(m->v, sizeof *m->v * cap);
        if (!v) return false;
        m->v = v, m->cap = cap;
    }
    char *copy = strdup(path);
    if (!copy) return false;
    m->v[m->n++] = (Made){copy, dir};
    return true;
}

static void made_clear(MadeList *m) {
    for (int i = 0; i < m->n; i++) free(m->v[i].path);
    free(m->v);
    *m = (MadeList){0};
}

// Newest first: the files, then the folders they were in (rmdir only takes empty ones, so a folder that has since
// got something of someone else's stays)
static void made_undo(MadeList *m) {
    for (int i = m->n - 1; i >= 0; i--) {
        if (m->v[i].dir) rmdir(m->v[i].path);
        else remove(m->v[i].path);
    }
    made_clear(m);
}

// mkdir -p; each folder it makes is noted when made is given
static bool make_dirs(const char *path, MadeList *made, ScReport *r) {
    char p[PATH_CAP];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) { fail(r, SC_IO, "%s: path too long", path); return false; }
    char *start = p + 1;
#ifdef _WIN32
    // "C:/..." and "C:\...": the drive is not a folder to make (mkdir("C:") fails), so start after its separator
    if (isalpha((unsigned char)p[0]) && p[1] == ':') start = p[2] == '/' || p[2] == '\\' ? p + 3 : p + 2;
#endif
    for (char *q = start; ; q++) {
#ifdef _WIN32
        if (*q != '/' && *q != '\\' && *q) continue;
#else
        if (*q != '/' && *q) continue;
#endif
        char c = *q;
        *q = 0;
        if (!is_dir(p)) {
            if (mkdir(p, 0755) != 0) {
                if (!is_dir(p)) { fail(r, SC_IO, "%s: %s", p, strerror(errno)); return false; }
            } else if (made && !made_add(made, p, true)) {
                rmdir(p);
                fail(r, SC_IO, "out of memory");
                return false;
            }
        }
        *q = c;
        if (!c) break;
    }
    return true;
}

static bool parent_dirs(const char *file, MadeList *made, ScReport *r) {
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s", file);
    char *s = strrchr(p, '/');
    if (!s) return true;
    *s = 0;
    return make_dirs(p, made, r);
}

static bool write_file(const char *path, const void *d, size_t n, MadeList *made, ScReport *r) {
    if (!parent_dirs(path, made, r)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) { fail(r, SC_IO, "%s: %s", path, strerror(errno)); return false; }
    if (!made_add(made, path, false)) {
        fclose(f);
        remove(path);
        fail(r, SC_IO, "out of memory");
        return false;
    }
    bool ok = fwrite(d, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) fail(r, SC_IO, "%s: could not be written (disk full?)", path);
    else r->n_written++;
    return ok;
}

// A written copy keeps the date of the file it comes from: the Saves tab shows when a slot was saved, and that is the
// file time of caption.sav (an import would otherwise date every slot with the moment of the import)
static void keep_time(const char *from, const char *to) {
    struct stat st;
    if (stat(from, &st) != 0) return;
    struct utimbuf t = {st.st_atime, st.st_mtime};
    utime(to, &t);
}

static bool copy_file(const char *from, const char *to, MadeList *made, ScReport *r) {
    FILE *in = fopen(from, "rb");
    if (!in) { fail(r, SC_IO, "%s: %s", from, strerror(errno)); return false; }
    if (!parent_dirs(to, made, r)) { fclose(in); return false; }
    FILE *out = fopen(to, "wb");
    if (!out) { fail(r, SC_IO, "%s: %s", to, strerror(errno)); fclose(in); return false; }
    if (!made_add(made, to, false)) {
        fclose(in), fclose(out);
        remove(to);
        fail(r, SC_IO, "out of memory");
        return false;
    }
    static __thread uint8_t chunk[1 << 16];
    size_t got;
    bool ok = true;
    while (ok && (got = fread(chunk, 1, sizeof chunk, in)) > 0) ok = fwrite(chunk, 1, got, out) == got;
    bool read_bad = ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    if (read_bad) { fail(r, SC_IO, "%s: read error", from); return false; }
    if (!ok) { fail(r, SC_IO, "%s: could not be written (disk full?)", to); return false; }
    keep_time(from, to);
    r->n_written++;
    return true;
}

// A folder's contents into `to` (made if it isn't there), files and folders only, a few levels deep
static bool copy_tree(const char *from, const char *to, int depth, MadeList *made, ScReport *r) {
    if (depth > 16) { fail(r, SC_IO, "%s: folders nested too deep", from); return false; }
    if (!make_dirs(to, made, r)) return false;
    int n;
    char **names = names_list(from, &n);
    if (!names) { fail(r, SC_IO, "%s: %s", from, strerror(errno)); return false; }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char a[PATH_CAP], b[PATH_CAP];
        if (!path_join(a, sizeof a, from, names[i]) || !path_join(b, sizeof b, to, names[i])) {
            fail(r, SC_IO, "%s/%s: path too long", from, names[i]);
            ok = false;
        } else if (is_dir(a)) ok = copy_tree(a, b, depth + 1, made, r);
        else if (is_file(a)) ok = copy_file(a, b, made, r);
    }
    names_free(names, n);
    return ok;
}

// ------------------------------------------------------------------ BYML (read only, lazy)
// v2 / v3, the flag tables only use hash, array, string, bool, int, uint and float nodes. A BNode is a value as it
// sits in its parent: the type byte and the 32 bits next to it (a container's offset, a string's index, a number).
typedef struct { const uint8_t *d; size_t n; bool le; uint32_t root, names, strs; } Byml;
typedef struct { const Byml *b; uint8_t type; uint32_t val; } BNode;
typedef struct { enum { S_NULL, S_STR, S_BOOL, S_INT, S_FLOAT } k; int64_t i; double f; const char *s; } Scalar;

static bool by_u(const Byml *b, uint64_t o, int w, uint32_t *v) {
    if (o + w > b->n) return false;
    uint32_t x = 0;
    for (int i = 0; i < w; i++) x = x << 8 | b->d[o + (b->le ? w - 1 - i : i)];
    *v = x;
    return true;
}

static bool by_open(Byml *b, const uint8_t *d, size_t n) {
    memset(b, 0, sizeof *b);
    b->d = d, b->n = n;
    if (n < 16) return false;
    if (d[0] == 'Y' && d[1] == 'B') b->le = true;
    else if (d[0] == 'B' && d[1] == 'Y') b->le = false;
    else return false;
    return by_u(b, 4, 4, &b->names) && by_u(b, 8, 4, &b->strs) && by_u(b, 12, 4, &b->root);
}

// String idx of a string table (the node at off: type byte, 3 byte count, then the offsets from its start)
static const char *by_table_str(const Byml *b, uint32_t off, uint32_t idx) {
    uint32_t cnt, so;
    if (!off || !by_u(b, (uint64_t)off + 1, 3, &cnt) || idx >= cnt || !by_u(b, (uint64_t)off + 4 + 4 * idx, 4, &so)) return NULL;
    uint64_t at = (uint64_t)off + so;
    if (at >= b->n || !memchr(b->d + at, 0, b->n - at)) return NULL;
    return (const char *)b->d + at;
}

static bool by_root(const Byml *b, BNode *out) {
    if (b->root >= b->n || (b->d[b->root] != 0xC0 && b->d[b->root] != 0xC1)) return false;
    *out = (BNode){b, b->d[b->root], b->root};
    return true;
}

static bool by_count(BNode nd, uint32_t *cnt) {
    return (nd.type == 0xC0 || nd.type == 0xC1) && by_u(nd.b, (uint64_t)nd.val + 1, 3, cnt);
}

static bool by_at(BNode arr, uint32_t i, BNode *out) {
    uint32_t cnt, v;
    if (arr.type != 0xC0 || !by_count(arr, &cnt) || i >= cnt) return false;
    uint64_t types = (uint64_t)arr.val + 4, base = types + ((cnt + 3) & ~3u);
    if (types + i >= arr.b->n || !by_u(arr.b, base + 4 * (uint64_t)i, 4, &v)) return false;
    *out = (BNode){arr.b, arr.b->d[types + i], v};
    return true;
}

static bool by_entry(BNode h, uint32_t i, const char **key, BNode *out) {
    uint32_t cnt, name, v;
    if (h.type != 0xC1 || !by_count(h, &cnt) || i >= cnt) return false;
    uint64_t a = (uint64_t)h.val + 4 + 8 * (uint64_t)i;
    if (!by_u(h.b, a, 3, &name) || a + 3 >= h.b->n || !by_u(h.b, a + 4, 4, &v)) return false;
    const char *k = by_table_str(h.b, h.b->names, name);
    if (!k) return false;
    if (key) *key = k;
    *out = (BNode){h.b, h.b->d[a + 3], v};
    return true;
}

// The value of `key` in a hash (the last one, as a dictionary would have it)
static bool by_get(BNode h, const char *key, BNode *out) {
    uint32_t cnt;
    const char *k;
    BNode v;
    bool found = false;
    if (h.type != 0xC1 || !by_count(h, &cnt)) return false;
    for (uint32_t i = 0; i < cnt; i++)
        if (by_entry(h, i, &k, &v) && !strcmp(k, key)) *out = v, found = true;
    return found;
}

static bool by_scalar(BNode nd, Scalar *s) {
    const Byml *b = nd.b;
    uint32_t lo, hi;
    memset(s, 0, sizeof *s);
    switch (nd.type) {
    case 0xA0: s->k = S_STR; return (s->s = by_table_str(b, b->strs, nd.val)) != NULL;
    case 0xD0: s->k = S_BOOL, s->i = nd.val != 0, s->f = (double)s->i; return true;
    case 0xD1: s->k = S_INT, s->i = (int32_t)nd.val, s->f = (double)s->i; return true;
    case 0xD2: {
        float f;
        memcpy(&f, &nd.val, 4);
        s->k = S_FLOAT, s->f = f;
        return true;
    }
    case 0xD3: s->k = S_INT, s->i = nd.val, s->f = (double)s->i; return true;
    case 0xD4: case 0xD5: case 0xD6: {  // 8 bytes at an offset
        if (!by_u(b, nd.val, 4, b->le ? &lo : &hi) || !by_u(b, (uint64_t)nd.val + 4, 4, b->le ? &hi : &lo)) return false;
        uint64_t u = (uint64_t)hi << 32 | lo;
        if (nd.type == 0xD6) {
            double f;
            memcpy(&f, &u, 8);
            s->k = S_FLOAT, s->f = f;
        } else {
            s->k = S_INT, s->i = (int64_t)u, s->f = nd.type == 0xD5 ? (double)u : (double)(int64_t)u;
        }
        return true;
    }
    case 0xFF: s->k = S_NULL; return true;
    }
    return false;
}

// A scalar as the low 32 bits of an integer (Python: int(x) & 0xFFFFFFFF)
static bool scalar_u32(const Scalar *s, uint32_t *v) {
    if (s->k == S_BOOL || s->k == S_INT) *v = (uint32_t)s->i;
    else if (s->k == S_FLOAT && s->f > -9.0e18 && s->f < 9.0e18) *v = (uint32_t)(int64_t)s->f;
    else return false;
    return true;
}

// ------------------------------------------------------------------ the flag table of one game revision
// bytes per element of each flag type, in 32-bit words
static const struct { const char *base; uint32_t words; } BASES[] = {
    {"bool", 1}, {"s32", 1}, {"f32", 1}, {"vector2f", 2}, {"vector3f", 3}, {"vector4f", 4},
    {"string", 8}, {"string64", 16}, {"string256", 64}};
enum { B_BOOL, B_S32, B_F32, B_VEC2, B_VEC3, B_VEC4, B_STR, B_STR64, B_STR256, NBASES };

typedef struct {
    uint32_t hash;
    uint8_t base;          // BASES index
    uint32_t words;        // 32-bit words in the save file (element size x the array's length)
    BNode init;            // InitValue
    uint32_t seq;          // order of appearance (a later flag of the same hash replaces an earlier one)
} Flag;

typedef struct { char name[64]; uint32_t *hashes; size_t n; } FmtFile;  // a flag file: its hashes, sorted

struct ScFormat {
    uint32_t revision;
    Flag *flags;           // sorted by hash, one per hash
    size_t nflags;
    FmtFile *files;
    int nfiles;
    Byml *docs;            // gamedata's BYML files (the flags point into them) ...
    int ndocs;
    uint8_t *gamedata;     // ... which live in this archive
};

uint32_t sc_format_revision(const ScFormat *f) { return f->revision; }

void sc_format_free(ScFormat *f) {
    if (!f) return;
    for (int i = 0; i < f->nfiles; i++) free(f->files[i].hashes);
    free(f->files), free(f->flags), free(f->docs), free(f->gamedata);
    free(f);
}

// Python: kind.replace('_array_data', '').replace('_data', '')
static int kind_base(const char *kind, bool *array) {
    char t[64];
    size_t n = strlen(kind);
    *array = n >= 11 && !strcmp(kind + n - 11, "_array_data");
    if (n >= sizeof t) return -1;
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        if (!strncmp(kind + i, "_array_data", 11)) i += 11;
        else if (!strncmp(kind + i, "_data", 5)) i += 5;
        else t[w++] = kind[i++];
    }
    t[w] = 0;
    for (int b = 0; b < NBASES; b++)
        if (!strcmp(t, BASES[b].base)) return b;
    return -1;
}

static int cmp_flag(const void *a, const void *b) {
    const Flag *x = a, *y = b;
    return x->hash != y->hash ? (x->hash < y->hash ? -1 : 1) : x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static const Flag *flag_find(const ScFormat *f, uint32_t h) {
    size_t lo = 0, hi = f->nflags;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f->flags[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    return lo < f->nflags && f->flags[lo].hash == h ? &f->flags[lo] : NULL;
}

// gamedata's BYML files: {"bool_data": [{DataName, HashValue, InitValue, ...}, ...], "s32_array_data": [...], ...}
static bool load_flags(ScFormat *f, const uint8_t *arc, size_t arcn, char *why, size_t whycap) {
    int nodes = sarc_count(arc, arcn);
    size_t cap = 0;
    uint32_t seq = 0;
    if (nodes < 0) { snprintf(why, whycap, "gamedata.ssarc is not an archive"); return false; }
    f->docs = calloc(nodes ? nodes : 1, sizeof *f->docs);
    if (!f->docs) { snprintf(why, whycap, "out of memory"); return false; }
    for (int i = 0; i < nodes; i++) {
        const char *name;
        const uint8_t *blob;
        size_t bn;
        Byml *doc = &f->docs[f->ndocs];
        BNode root, list;
        if (!sarc_node(arc, arcn, i, &name, &blob, &bn) || !by_open(doc, blob, bn) || !by_root(doc, &root)) {
            snprintf(why, whycap, "gamedata.ssarc: file %d is not a BYML file", i);
            return false;
        }
        f->ndocs++;
        uint32_t nkinds = 0;
        if (!by_count(root, &nkinds)) { snprintf(why, whycap, "gamedata: bad root"); return false; }
        for (uint32_t k = 0; k < nkinds; k++) {
            const char *kind;
            bool array;
            uint32_t cnt;
            if (!by_entry(root, k, &kind, &list)) { snprintf(why, whycap, "gamedata: bad entry"); return false; }
            int base = kind_base(kind, &array);
            if (base < 0 || !by_count(list, &cnt) || list.type != 0xC0) continue;
            for (uint32_t e = 0; e < cnt; e++) {
                BNode ent, hv, iv, el, vals;
                Scalar hs;
                uint32_t h, words = BASES[base].words, count = 1;
                if (!by_at(list, e, &ent) || !by_get(ent, "HashValue", &hv) || !by_scalar(hv, &hs) || !scalar_u32(&hs, &h) ||
                    !by_get(ent, "InitValue", &iv)) {
                    snprintf(why, whycap, "gamedata: a %s entry has no hash or default", kind);
                    return false;
                }
                if (array) {
                    if (!by_at(iv, 0, &el) || !by_get(el, "Values", &vals) || !by_count(vals, &count)) {
                        snprintf(why, whycap, "gamedata: an array flag of %s has no length", kind);
                        return false;
                    }
                    words *= count;
                }
                if (f->nflags == cap) {
                    cap = cap ? cap * 2 : 1 << 14;
                    Flag *v = realloc(f->flags, sizeof *f->flags * cap);
                    if (!v) { snprintf(why, whycap, "out of memory"); return false; }
                    f->flags = v;
                }
                f->flags[f->nflags++] = (Flag){h, (uint8_t)base, words, iv, seq++};
            }
        }
    }
    qsort(f->flags, f->nflags, sizeof *f->flags, cmp_flag);
    size_t w = 0;  // one per hash: the last one read
    for (size_t i = 0; i < f->nflags; i++)
        if (i + 1 == f->nflags || f->flags[i + 1].hash != f->flags[i].hash) f->flags[w++] = f->flags[i];
    f->nflags = w;
    return true;
}

// savedataformat's .bgsvdata files: {"save_info": [{"revision": N}], "file_list": [{"file_name": "game_data.sav"}, [{"HashValue": N}, ...]]}
static bool load_files(ScFormat *f, const uint8_t *arc, size_t arcn, char *why, size_t whycap) {
    int nodes = sarc_count(arc, arcn);
    if (nodes < 0) { snprintf(why, whycap, "savedataformat.ssarc is not an archive"); return false; }
    for (int i = 0; i < nodes; i++) {
        const char *name, *fname;
        const uint8_t *blob;
        size_t bn;
        Byml doc;
        BNode root, info, info0, rev, flist, desc, fn, hashes, hv, hh;
        Scalar s;
        uint32_t cnt, rv;
        if (!sarc_node(arc, arcn, i, &name, &blob, &bn)) { snprintf(why, whycap, "savedataformat: bad archive"); return false; }
        size_t nl = name ? strlen(name) : 0;
        if (nl < 9 || strcmp(name + nl - 9, ".bgsvdata")) continue;
        if (!by_open(&doc, blob, bn) || !by_root(&doc, &root) || !by_get(root, "save_info", &info) ||
            !by_at(info, 0, &info0) || !by_get(info0, "revision", &rev) || !by_scalar(rev, &s) || !scalar_u32(&s, &rv) ||
            !by_get(root, "file_list", &flist) || !by_at(flist, 0, &desc) || !by_get(desc, "file_name", &fn) ||
            !by_scalar(fn, &s) || s.k != S_STR || !by_at(flist, 1, &hashes) || !by_count(hashes, &cnt)) {
            snprintf(why, whycap, "savedataformat: %s is not what was expected", name);
            return false;
        }
        fname = s.s;
        f->revision = rv;
        FmtFile *ff = NULL;
        for (int k = 0; k < f->nfiles; k++)
            if (!strcmp(f->files[k].name, fname)) ff = &f->files[k];
        if (!ff) {
            if (strlen(fname) >= sizeof ff->name) { snprintf(why, whycap, "savedataformat: a file name is too long"); return false; }
            FmtFile *v = realloc(f->files, sizeof *f->files * (f->nfiles + 1));
            if (!v) { snprintf(why, whycap, "out of memory"); return false; }
            f->files = v;
            ff = &f->files[f->nfiles++];
            *ff = (FmtFile){0};
            strcpy(ff->name, fname);
        }
        uint32_t *v = realloc(ff->hashes, sizeof *ff->hashes * (ff->n + cnt + 1));
        if (!v) { snprintf(why, whycap, "out of memory"); return false; }
        ff->hashes = v;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t h;
            if (!by_at(hashes, k, &hv) || !by_get(hv, "HashValue", &hh) || !by_scalar(hh, &s) || !scalar_u32(&s, &h)) {
                snprintf(why, whycap, "savedataformat: %s has a flag without a hash", name);
                return false;
            }
            ff->hashes[ff->n++] = h;
        }
    }
    for (int k = 0; k < f->nfiles; k++) {
        FmtFile *ff = &f->files[k];
        qsort(ff->hashes, ff->n, sizeof *ff->hashes, cmp_u32);
        size_t missing = 0;
        for (size_t i = 0; i < ff->n; i++) missing += flag_find(f, ff->hashes[i]) == NULL;
        if (missing) {
            snprintf(why, whycap, "%s: %zu flags have no type in gamedata", ff->name, missing);
            return false;
        }
    }
    return true;
}

ScCode sc_format_load(const char *bootup_path, ScFormat **out, ScReport *r) {
    char why[512];
    size_t n = 0;
    *out = NULL;
    if (!is_file(bootup_path)) { fail(r, SC_NO_BOOTUP, "%s", bootup_path); return r->code; }
    uint8_t *pack = read_file(bootup_path, &n, (size_t)1 << 31, why, sizeof why);
    if (!pack) { fail(r, SC_NO_BOOTUP, "%s", why); return r->code; }
    ScFormat *f = calloc(1, sizeof *f);
    static const char *const want[] = {"GameData/gamedata.ssarc", "GameData/savedataformat.ssarc"};
    uint8_t *arcs[2] = {NULL, NULL};
    size_t arcn[2] = {0, 0};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        uint32_t doff, s, e;
        if (sarc_find(pack, n, want[i], &doff, &s, &e) < 0) {
            snprintf(why, sizeof why, "%s is not in the pack", want[i]);
            ok = false;
        } else if (e - s >= 4 && memcmp(pack + doff + s, "Yaz0", 4)) {  // (not compressed: used as it is)
            arcn[i] = e - s;
            arcs[i] = memcpy(malloc(arcn[i]), pack + doff + s, arcn[i]);
        } else if (!(arcs[i] = yaz0_unpack(pack + doff + s, e - s, &arcn[i]))) {
            snprintf(why, sizeof why, "%s can't be unpacked", want[i]);
            ok = false;
        }
    }
    free(pack);
    f->gamedata = arcs[0];
    ok = ok && load_flags(f, arcs[0], arcn[0], why, sizeof why) && load_files(f, arcs[1], arcn[1], why, sizeof why);
    free(arcs[1]);
    if (ok && f->nfiles == 0) { snprintf(why, sizeof why, "no save file formats"); ok = false; }
    if (!ok) {
        fail(r, SC_BAD_BOOTUP, "%s", why);
        sc_format_free(f);
        return r->code;
    }
    *out = f;
    return SC_OK;
}

static const FmtFile *fmt_file(const ScFormat *f, const char *name) {
    for (int i = 0; i < f->nfiles; i++)
        if (!strcmp(f->files[i].name, name)) return &f->files[i];
    return NULL;
}

// ------------------------------------------------------------------ flag default values
// The default of a flag as little-endian words (strings as plain bytes), `words` long: an element after the other,
// each as wide as the type; a bool is 1 only for a literal 1 (the numbers some bool flags carry are respawn data).
typedef struct { uint8_t *out; size_t bytes, k, slot; int base; bool err; } Enc;

static bool enc_scalar(const Scalar *s, Enc *e) {
    uint8_t el[4] = {0};
    if (e->base == B_STR || e->base == B_STR64 || e->base == B_STR256) {
        if (s->k != S_STR) return false;
        size_t at = e->k++ * e->slot, len = strlen(s->s);
        if (len > e->slot - 1) len = e->slot - 1;
        for (size_t i = 0; i < len && at + i < e->bytes; i++) e->out[at + i] = (uint8_t)s->s[i];
        return true;
    }
    uint32_t v = 0;
    if (e->base == B_BOOL) {
        v = (s->k == S_BOOL || s->k == S_INT || s->k == S_FLOAT) && s->f == 1.0;
    } else if (e->base == B_F32 || e->base == B_VEC2 || e->base == B_VEC3 || e->base == B_VEC4) {
        if (s->k == S_STR || s->k == S_NULL) return false;
        float f = (float)s->f;
        memcpy(&v, &f, 4);
    } else if (!scalar_u32(s, &v)) {
        return false;
    }
    wr32(el, v, true);
    size_t at = e->k++ * 4;
    for (int i = 0; i < 4 && at + i < e->bytes; i++) e->out[at + i] = el[i];
    return true;
}

static bool enc_walk(BNode nd, int depth, Enc *e) {
    uint32_t cnt;
    BNode sub;
    Scalar s;
    if (depth > 16) return false;
    if (nd.type == 0xC0) {
        if (!by_count(nd, &cnt)) return false;
        for (uint32_t i = 0; i < cnt; i++)
            if (!by_at(nd, i, &sub) || !enc_walk(sub, depth + 1, e)) return false;
        return true;
    }
    if (nd.type == 0xC1) return by_get(nd, "Values", &sub) && enc_walk(sub, depth + 1, e);
    return by_scalar(nd, &s) && enc_scalar(&s, e);
}

static bool encode_init(const Flag *fl, uint8_t *out) {
    Enc e = {out, (size_t)fl->words * 4, 0, (size_t)BASES[fl->base].words * 4, fl->base, false};
    memset(out, 0, e.bytes);
    return enc_walk(fl->init, 0, &e);
}

// ------------------------------------------------------------------ flag files
static bool is_string_flag(const Flag *f) { return f->base >= B_STR; }

// 'little' or 'big' by the revision in the header (a small number); false: not a BotW flag save file
static bool detect_endian(const uint8_t *d, size_t n, bool *le, char *why, size_t whycap) {
    if (n < HEADER + 4 || rd32(d + n - 4, true) != 0xFFFFFFFFu) {
        snprintf(why, whycap, "not a BotW flag save file (bad size or footer)");
        return false;
    }
    uint32_t l = rd32(d, true), b = rd32(d, false);
    if (l < 0x10000 && b >= 0x10000) *le = true;
    else if (b < 0x10000 && l >= 0x10000) *le = false;
    else {
        snprintf(why, whycap, "cannot tell the byte order (header 0x%08X)", l);
        return false;
    }
    return true;
}

// Rebuilds one flag save file for the revision and byte order of fmt
static ScCode convert_flags(const uint8_t *d, size_t n, const ScFormat *fmt, const FmtFile *ff, bool to_le, Buf *out,
                            ScFlagFile *st, char *why, size_t whycap) {
    bool src_le;
    if (!detect_endian(d, n, &src_le, why, whycap)) return SC_BAD_SAVE;
    size_t body = n - HEADER - 4;
    if (body % 8) { snprintf(why, whycap, "size is not a whole number of entries"); return SC_BAD_SAVE; }
    size_t m = body / 8;
    const uint8_t *ent = d + HEADER;
    for (size_t i = 1; i < m; i++)
        if (rd32(ent + 8 * i, src_le) < rd32(ent + 8 * (i - 1), src_le)) {
            snprintf(why, whycap, "entries are not sorted by hash");
            return SC_BAD_SAVE;
        }
    uint8_t hdr[HEADER];
    wr32(hdr, fmt->revision, to_le), wr32(hdr + 4, 0xFFFFFFFFu, to_le), wr32(hdr + 8, 1, to_le);
    out->n = 0;
    if (!buf_put(out, hdr, HEADER)) { snprintf(why, whycap, "out of memory"); return SC_IO; }
    *st = (ScFlagFile){0};
    st->src_rev = rd32(d, src_le), st->dst_rev = fmt->revision;
    for (size_t i = 0; i < m;) {  // flags the game's revision doesn't have
        uint32_t h = rd32(ent + 8 * i, src_le);
        size_t j = i;
        while (j < m && rd32(ent + 8 * j, src_le) == h) j++;
        const uint32_t *lo = ff->hashes;
        size_t a = 0, z = ff->n;
        while (a < z) {
            size_t mid = (a + z) / 2;
            if (lo[mid] < h) a = mid + 1;
            else z = mid;
        }
        if (!(a < ff->n && lo[a] == h)) st->dropped++;
        i = j;
    }
    uint8_t *raw = NULL;
    size_t rawcap = 0;
    ScCode rc = SC_OK;
    for (size_t fi = 0; fi < ff->n && rc == SC_OK; fi++) {
        uint32_t h = ff->hashes[fi];
        const Flag *fl = flag_find(fmt, h);
        size_t a = 0, z = m;  // the run of this flag in the save
        while (a < z) {
            size_t mid = (a + z) / 2;
            if (rd32(ent + 8 * mid, src_le) < h) a = mid + 1;
            else z = mid;
        }
        size_t first = a, have = 0;
        while (first + have < m && rd32(ent + 8 * (first + have), src_le) == h) have++;
        bool str = is_string_flag(fl);
        if (!buf_reserve(out, (size_t)fl->words * 8)) { snprintf(why, whycap, "out of memory"); rc = SC_IO; break; }
        if (!raw || rawcap < (size_t)fl->words * 4) {
            rawcap = (size_t)fl->words * 4;
            uint8_t *v = realloc(raw, rawcap ? rawcap : 1);
            if (!v) { snprintf(why, whycap, "out of memory"); rc = SC_IO; break; }
            raw = v;
        }
        if (!have || have != fl->words) {
            if (!encode_init(fl, raw)) {
                snprintf(why, whycap, "the default of flag 0x%08X can't be read from gamedata", h);
                rc = SC_BAD_BOOTUP;
                break;
            }
            if (have) st->resized++;
        }
        if (!have) st->defaulted++;
        else st->kept++;
        for (size_t w = 0; w < fl->words; w++) {
            uint8_t word[4], rec[8];
            if (w < have) {  // the save's own word, swapped when the byte order changes (text is plain bytes)
                memcpy(word, ent + 8 * (first + w) + 4, 4);
                if (!str && src_le != to_le) {
                    uint8_t t = word[0]; word[0] = word[3]; word[3] = t;
                    t = word[1]; word[1] = word[2]; word[2] = t;
                }
            } else {  // the default: little-endian in raw
                memcpy(word, raw + 4 * w, 4);
                if (!str && !to_le) {
                    uint8_t t = word[0]; word[0] = word[3]; word[3] = t;
                    t = word[1]; word[1] = word[2]; word[2] = t;
                }
            }
            wr32(rec, h, to_le);
            memcpy(rec + 4, word, 4);
            memcpy(out->p + out->n, rec, 8);
            out->n += 8;
        }
    }
    free(raw);
    if (rc != SC_OK) return rc;
    uint8_t foot[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    if (buf_put(out, foot, 4)) return SC_OK;
    snprintf(why, whycap, "out of memory");
    return SC_IO;
}

// One Hero's Path block: a fixed 64 KiB record, swapped in 32-bit words except its 8 byte header (four plain bytes,
// then two 16-bit fields). The swap is its own inverse. In place.
static void convert_tracker(uint8_t *b, size_t n) {
    uint8_t t = b[4]; b[4] = b[5]; b[5] = t;
    t = b[6]; b[6] = b[7]; b[7] = t;
    for (size_t i = 8; i + 4 <= n; i += 4) {
        t = b[i]; b[i] = b[i + 3]; b[i + 3] = t;
        t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t;
    }
}

// ------------------------------------------------------------------ the profile folder
static bool looks_like_profile(const char *p) {
    int n;
    char **v = names_list(p, &n);
    bool yes = false;
    for (int i = 0; v && i < n && !yes; i++) {
        char f[PATH_CAP], g[PATH_CAP];
        if (!all_digits(v[i]) || !path_join(f, sizeof f, p, v[i]) || !path_join(g, sizeof g, f, "game_data.sav")) continue;
        yes = is_file(g);
    }
    if (v) names_free(v, n);
    return yes;
}

// Whether the first slot of a profile is in the Wii U's byte order (the revision in the header is a small number)
static bool profile_is_big_endian(const char *p) {
    int n;
    char **v = names_list(p, &n);
    bool be = false;
    for (int i = 0; v && i < n; i++) {
        char f[PATH_CAP], g[PATH_CAP];
        uint8_t head[4];
        if (!all_digits(v[i]) || !path_join(f, sizeof f, p, v[i]) || !path_join(g, sizeof g, f, "game_data.sav") || !is_file(g))
            continue;
        FILE *fp = fopen(g, "rb");
        if (fp && fread(head, 1, 4, fp) == 4) be = rd32(head, false) < 0x10000 && rd32(head, true) >= 0x10000;
        if (fp) fclose(fp);
        break;
    }
    if (v) names_free(v, n);
    return be;
}

// What the search found: the first Wii U profile, else the first profile of any kind (so that a Switch save can be
// told apart from no save, and a folder holding both Savage Wind's own saves and a Cemu profile finds the latter)
typedef struct { int visits; time_t t0; const ScCancel *cancel; bool have_be, have_any; char be[PATH_CAP], any[PATH_CAP]; } Search;

static bool search_over(Search *s) {  // (out of folders or of time: stop looking)
    if (cancel_requested(s->cancel) || s->visits > PROFILE_VISITS) return true;
    if (time(NULL) - s->t0 > PROFILE_SECONDS) s->visits = PROFILE_VISITS + 1;
    return s->visits > PROFILE_VISITS;
}

static void find_profile(const char *dir, int depth, Search *s) {
    if (s->have_be || search_over(s) || ++s->visits > PROFILE_VISITS) return;
    if ((s->visits & 63) == 0 && search_over(s)) return;
    if (looks_like_profile(dir)) {
        if (profile_is_big_endian(dir)) s->have_be = snprintf(s->be, sizeof s->be, "%s", dir) < (int)sizeof s->be;
        else if (!s->have_any) s->have_any = snprintf(s->any, sizeof s->any, "%s", dir) < (int)sizeof s->any;
        return;
    }
    if (depth >= PROFILE_DEPTH) return;
    int n;
    char **v = names_list(dir, &n);
    for (int i = 0; v && i < n && !s->have_be && !search_over(s); i++) {
        char sub[PATH_CAP];
        if (!strcmp(v[i], ".git") || !strcmp(v[i], "node_modules")) continue;  // (never a save; often huge)
        if ((i & 63) == 63 && search_over(s)) break;
        if (path_join(sub, sizeof sub, dir, v[i]) && is_dir(sub)) find_profile(sub, depth + 1, s);
    }
    if (v) names_free(v, n);
}

// The start time is passed separately so the tests can exercise an expired search without waiting.
static ScCode find_profile_root(const char *dir, char *root, size_t cap, time_t started, const ScCancel *cancel) {
    Search s = {0};
    s.t0 = started, s.cancel = cancel;
    find_profile(dir, 0, &s);
    if (cancel_requested(cancel)) return SC_CANCELLED;
    if (!s.have_be && search_over(&s)) return SC_SEARCH_LIMIT;
    const char *found = s.have_be ? s.be : s.have_any ? s.any : NULL;
    if (found) snprintf(root, cap, "%s", found);
    return found ? SC_OK : SC_NO_SAVE;
}

ScCode sc_find_profile(const char *dir, char *root, size_t cap) {
    return find_profile_root(dir, root, cap, time(NULL), NULL);
}

// ------------------------------------------------------------------ converting a profile
struct ScStage {
    char dir[PATH_CAP];
    MadeList made;
};

static bool report_add_file(ScReport *r, const char *file, const ScFlagFile *st) {
    ScFlagFile *v = realloc(r->files, sizeof *r->files * (r->nfiles + 1));
    if (!v) { fail(r, SC_IO, "out of memory"); return false; }
    r->files = v;
    r->files[r->nfiles] = *st;
    snprintf(r->files[r->nfiles].file, sizeof r->files[r->nfiles].file, "%s", file);
    r->nfiles++;
    r->kept += st->kept, r->defaulted += st->defaulted, r->dropped += st->dropped, r->resized += st->resized;
    return true;
}

void sc_report_free(ScReport *r) {
    for (int i = 0; i < r->nslots; i++) free(r->slots[i]);
    free(r->slots), free(r->files);
    memset(r, 0, sizeof *r);
}

void sc_stage_keep(ScStage *st) {
    if (!st) return;
    made_clear(&st->made);
    free(st);
}

void sc_stage_discard(ScStage *st) {
    if (!st) return;
    made_undo(&st->made);
    free(st);
}

// A flag file of the profile -> converted file under dst (rel is the name shown in the report)
static bool convert_one(const char *from, const char *to, const char *rel, const ScFormat *fmt, const FmtFile *ff,
                        bool to_le, bool *src_le, const ScCancel *cancel, MadeList *made, ScReport *r) {
    if (cancelled(cancel, r)) return false;
    char why[512];
    size_t n;
    uint8_t *data = read_file(from, &n, (size_t)1 << 28, why, sizeof why);
    if (!data) { fail(r, SC_IO, "%s", why); return false; }
    Buf out = {0};
    ScFlagFile st;
    if (src_le && !detect_endian(data, n, src_le, why, sizeof why)) {  // (the first slot's byte order decides the Hero's Path)
        fail(r, SC_BAD_SAVE, "%s: %s", from, why);
        free(data);
        return false;
    }
    ScCode c = convert_flags(data, n, fmt, ff, to_le, &out, &st, why, sizeof why);
    free(data);
    if (c != SC_OK) {
        if (c == SC_BAD_SAVE) fail(r, c, "%s: %s", from, why);
        else fail(r, c, "%s", why);
        free(out.p);
        return false;
    }
    bool ok = write_file(to, out.p, out.n, made, r);
    free(out.p);
    if (ok) keep_time(from, to), ok = report_add_file(r, rel, &st);
    return ok;
}

static bool has_jpg_suffix(const char *s) {
    size_t n = strlen(s);
    return n >= 4 && (s[n - 4] == '.') && (s[n - 3] | 32) == 'j' && (s[n - 2] | 32) == 'p' && (s[n - 1] | 32) == 'g';
}

// tracker/ and the picture folders: every file (pictures: the .jpg ones) copied; swap: the Hero's Path's byte order
static bool copy_folder(const char *from, const char *to, bool pictures_only, bool swap, int *count, const ScCancel *cancel, MadeList *made,
                        ScReport *r) {
    int n;
    char **v = names_list(from, &n);
    if (!v) { fail(r, SC_IO, "%s: %s", from, strerror(errno)); return false; }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        if (cancelled(cancel, r)) { ok = false; break; }
        char a[PATH_CAP], b[PATH_CAP];
        if (pictures_only && !has_jpg_suffix(v[i])) continue;
        if (!path_join(a, sizeof a, from, v[i]) || !path_join(b, sizeof b, to, v[i])) {
            fail(r, SC_IO, "%s/%s: path too long", from, v[i]);
            ok = false;
        } else if (!is_file(a)) {
            continue;
        } else if (swap) {
            char why[512];
            size_t sz;
            uint8_t *blk = read_file(a, &sz, (size_t)1 << 26, why, sizeof why);
            if (!blk) { fail(r, SC_IO, "%s", why); ok = false; break; }
            if (sz % 4 == 0 && sz >= 8) convert_tracker(blk, sz);  // (an odd-sized file is copied as it is)
            ok = write_file(b, blk, sz, made, r);
            if (ok) keep_time(a, b);
            free(blk);
            if (ok) (*count)++;
        } else {
            ok = copy_file(a, b, made, r);
            if (ok) (*count)++;
        }
    }
    names_free(v, n);
    return ok;
}

ScCode sc_convert(const char *src, const char *stage_dir, const ScFormat *fmt, unsigned flags, ScStage **stage,
                  ScReport *r) {
    return sc_convert_cancel(src, stage_dir, fmt, flags, NULL, stage, r);
}

ScCode sc_convert_cancel(const char *src, const char *stage_dir, const ScFormat *fmt, unsigned flags,
                         const ScCancel *cancel, ScStage **stage, ScReport *r) {
    *stage = NULL;
    r->code = SC_OK;
    if (cancelled(cancel, r)) return r->code;
    const bool to_le = true;  // the Switch (Savage Wind); the Python tool's --to wiiu is not offered here
    const FmtFile *gd = fmt_file(fmt, "game_data.sav"), *cap = fmt_file(fmt, "caption.sav"), *opt = fmt_file(fmt, "option.sav");
    if (!gd) { fail(r, SC_BAD_BOOTUP, "the game has no game_data.sav format"); return r->code; }
    ScCode found = find_profile_root(src, r->root, sizeof r->root, time(NULL), cancel);
    if (found != SC_OK) {
        fail(r, found, "%s", src);
        return r->code;
    }
    if (exists(stage_dir)) { fail(r, SC_IO, "%s: exists already", stage_dir); return r->code; }
    ScStage *st = calloc(1, sizeof *st);
    snprintf(st->dir, sizeof st->dir, "%s", stage_dir);
    char dst[PATH_CAP];
    if (!make_dirs(stage_dir, &st->made, r) || !path_join(dst, sizeof dst, stage_dir, "save_0") ||
        !make_dirs(dst, &st->made, r))
        goto fail;
    int ns;
    char **slots = names_list(r->root, &ns);
    bool src_known = false, src_le = false;
    if (!slots) { fail(r, SC_IO, "%s: %s", r->root, strerror(errno)); goto fail; }
    for (int i = 0; i < ns; i++) {
        char sd[PATH_CAP], gp[PATH_CAP], to[PATH_CAP], rel[64], from[PATH_CAP];
        if (!all_digits(slots[i]) || !path_join(sd, sizeof sd, r->root, slots[i]) || !path_join(gp, sizeof gp, sd, "game_data.sav") ||
            !is_file(gp))
            continue;
        snprintf(to, sizeof to, "%s/%s/game_data.sav", dst, slots[i]);
        snprintf(rel, sizeof rel, "%s/game_data.sav", slots[i]);
        bool first = !src_known;
        if (!convert_one(gp, to, rel, fmt, gd, to_le, first ? &src_le : NULL, cancel, &st->made, r)) { names_free(slots, ns); goto fail; }
        if (first) {
            src_known = true;
            if (src_le && !(flags & SC_ALLOW_SWITCH)) {
                fail(r, SC_SWITCH_SAVE, "%s", r->root);
                names_free(slots, ns);
                goto fail;
            }
        }
        char **v = realloc(r->slots, sizeof *r->slots * (r->nslots + 1));
        if (!v) { fail(r, SC_IO, "out of memory"); names_free(slots, ns); goto fail; }
        r->slots = v;
        char *name = strdup(slots[i]);
        if (!name) { fail(r, SC_IO, "out of memory"); names_free(slots, ns); goto fail; }
        r->slots[r->nslots++] = name;
        path_join(from, sizeof from, sd, "caption.sav");
        if (is_file(from) && cap) {
            snprintf(to, sizeof to, "%s/%s/caption.sav", dst, slots[i]);
            snprintf(rel, sizeof rel, "%s/caption.sav", slots[i]);
            if (!convert_one(from, to, rel, fmt, cap, to_le, NULL, cancel, &st->made, r)) { names_free(slots, ns); goto fail; }
        }
        path_join(from, sizeof from, sd, "caption.jpg");
        if (is_file(from)) {
            snprintf(to, sizeof to, "%s/%s/caption.jpg", dst, slots[i]);
            if (cancelled(cancel, r) || !copy_file(from, to, &st->made, r)) { names_free(slots, ns); goto fail; }
        }
    }
    names_free(slots, ns);
    char from[PATH_CAP], to[PATH_CAP];
    path_join(from, sizeof from, r->root, "option.sav");
    if (is_file(from) && opt) {
        path_join(to, sizeof to, dst, "option.sav");
        if (!convert_one(from, to, "option.sav", fmt, opt, to_le, NULL, cancel, &st->made, r)) goto fail;
    }
    // (the Hero's Path swaps only when the byte order changes: a Switch save converted for the Switch stays as it is)
    path_join(from, sizeof from, r->root, "tracker");
    path_join(to, sizeof to, dst, "tracker");
    if (is_dir(from) && !copy_folder(from, to, false, src_known && src_le != to_le, &r->n_tracker, cancel, &st->made, r)) goto fail;
    path_join(from, sizeof from, r->root, "album");
    path_join(to, sizeof to, dst, "album");
    if (is_dir(from) && !copy_folder(from, to, true, false, &r->n_album, cancel, &st->made, r)) goto fail;
    path_join(from, sizeof from, r->root, "pict_book");
    path_join(to, sizeof to, dst, "pict_book");
    if (is_dir(from) && !copy_folder(from, to, true, false, &r->n_pict, cancel, &st->made, r)) goto fail;
    if (cancelled(cancel, r)) goto fail;
    if (!r->nslots) { fail(r, SC_NO_SAVE, "%s", src); goto fail; }
    *stage = st;
    return SC_OK;
fail:
    sc_stage_discard(st);
    return r->code;
}

// ------------------------------------------------------------------ the lock on the save folder
// The game holds <save>.lock (next to the save folder, so the import's rename of the folder never moves it) for as long as
// it runs; the Saves tab refuses to import or back up while somebody else holds it. The operating system drops the lock
// when the process ends, however it ends, so a crash leaves nothing stale behind (the file itself stays, empty).
struct ScLock {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
};

static bool lock_path(const char *save_dir, char *out, size_t cap) {
    size_t n = strlen(save_dir);
    while (n > 1 && (save_dir[n - 1] == '/' || save_dir[n - 1] == '\\')) n--;
    return n + 6 < cap && snprintf(out, cap, "%.*s.lock", (int)n, save_dir) > 0;
}

ScLockResult sc_lock_take(const char *save_dir, ScLock **out) {
    *out = NULL;
    char path[PATH_CAP];
    if (!lock_path(save_dir, path, sizeof path)) return SC_LOCK_UNAVAILABLE;
    ScLock *l = calloc(1, sizeof *l);
    if (!l) return SC_LOCK_UNAVAILABLE;
#ifdef _WIN32
    l->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (l->h == INVALID_HANDLE_VALUE) { free(l); return SC_LOCK_UNAVAILABLE; }
    OVERLAPPED ov = {0};
    if (!LockFileEx(l->h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        bool busy = GetLastError() == ERROR_LOCK_VIOLATION;
        CloseHandle(l->h);
        free(l);
        return busy ? SC_LOCK_BUSY : SC_LOCK_UNAVAILABLE;
    }
#else
    l->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (l->fd < 0) { free(l); return SC_LOCK_UNAVAILABLE; }
    if (flock(l->fd, LOCK_EX | LOCK_NB) != 0) {
        bool busy = errno == EWOULDBLOCK;
        close(l->fd);
        free(l);
        return busy ? SC_LOCK_BUSY : SC_LOCK_UNAVAILABLE;
    }
#endif
    *out = l;
    return SC_LOCK_OK;
}

void sc_lock_drop(ScLock *l) {
    if (!l) return;
#ifdef _WIN32
    CloseHandle(l->h);  // (closing the handle releases the lock)
#else
    close(l->fd);
#endif
    free(l);
}

// Import and backup take the lock for their whole run: the game can't be started into a folder that is being swapped
// without noticing (it logs that the lock is held), and two Saves tabs can't swap at the same time. A lock file that
// can't be made (a read-only data root) protects nothing, and does not stop the player either.
static bool save_in_use(const char *save_dir, ScLock **held, ScReport *r) {
    *held = NULL;
    if (sc_lock_take(save_dir, held) != SC_LOCK_BUSY) return false;
    fail(r, SC_IN_USE, "%s", save_dir);
    return true;
}

// ------------------------------------------------------------------ backups and the swap
static void stamp(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y%m%d-%H%M%S", &tmv);
}

static void unique_path(char *out, size_t cap, const char *prefix) {
    char st[32];
    stamp(st, sizeof st);
    snprintf(out, cap, "%s%s", prefix, st);
    for (int k = 2; exists(out) && k < 1000; k++) snprintf(out, cap, "%s%s-%d", prefix, st, k);
}

static void strip_slash(char *dst, size_t cap, const char *dir) {
    snprintf(dst, cap, "%s", dir);
    size_t n = strlen(dst);
    while (n > 1 && (dst[n - 1] == '/' || dst[n - 1] == '\\')) dst[--n] = 0;
}

void sc_backup_path(const char *save_dir, char *out, size_t cap) {
    char base[PATH_CAP], prefix[PATH_CAP + 32];
    strip_slash(base, sizeof base, save_dir);
    snprintf(prefix, sizeof prefix, "%s-backups/save-", base);
    unique_path(out, cap, prefix);
}

void sc_stage_path(const char *save_dir, char *out, size_t cap) {
    char base[PATH_CAP], prefix[PATH_CAP + 32];
    strip_slash(base, sizeof base, save_dir);
    snprintf(prefix, sizeof prefix, "%s-import-", base);
    unique_path(out, cap, prefix);
}

static ScCode backup_locked(const char *save_dir, const char *backup_dir, ScReport *r) {
    MadeList made = {0};
    r->code = SC_OK;
    if (!is_dir(save_dir)) { fail(r, SC_NO_SAVE, "%s", save_dir); return r->code; }
    if (exists(backup_dir)) { fail(r, SC_IO, "%s: exists already", backup_dir); return r->code; }
    if (!copy_tree(save_dir, backup_dir, 0, &made, r)) {
        made_undo(&made);
        return r->code;
    }
    made_clear(&made);
    return SC_OK;
}

ScCode sc_backup(const char *save_dir, const char *backup_dir, ScReport *r) {
    r->code = SC_OK;
    ScLock *lock;
    if (save_in_use(save_dir, &lock, r)) return r->code;  // (a game that is saving may have a file half written)
    ScCode c = backup_locked(save_dir, backup_dir, r);
    sc_lock_drop(lock);
    return c;
}

static ScCode install_locked(ScStage *st, const char *save_dir, const char *backup_dir, ScReport *r) {
    r->code = SC_OK;
    bool had = exists(save_dir);
    if (had && !is_dir(save_dir)) { fail(r, SC_IO, "%s: not a folder", save_dir); return r->code; }
#ifndef _WIN32
    struct stat ls;  // (renaming a link moves the link, not the saves it points to; Windows junctions: the caller checks)
    if (had && lstat(save_dir, &ls) == 0 && S_ISLNK(ls.st_mode)) { fail(r, SC_LINKED, "%s", save_dir); return r->code; }
#endif
    MadeList parents = {0};
    if (had) {
        if (exists(backup_dir)) { fail(r, SC_IO, "%s: exists already", backup_dir); return r->code; }
        if (!parent_dirs(backup_dir, &parents, r)) { made_undo(&parents); return r->code; }
        if (rename(save_dir, backup_dir) != 0) {
            fail(r, SC_IO, "%s: %s", save_dir, strerror(errno));  // (nothing has moved; on Windows: a program has it open)
            made_undo(&parents);
            return r->code;
        }
    }
#ifdef SC_TEST
    extern void sc_test_before_install(void);
    sc_test_before_install();  // (tests pause between the two renames; production has no pause)
#endif
    if (rename(st->dir, save_dir) != 0) {
        int err = errno;
        if (had && rename(backup_dir, save_dir) != 0) {
            fprintf(stderr, "[saves] %s could not be put in place (%s) and %s could not be put back\n", st->dir, strerror(err), backup_dir);
            fail(r, SC_MOVED, "%s", backup_dir);
        } else
            fail(r, SC_IO, "%s: %s", st->dir, strerror(err));
        made_undo(&parents);
        return r->code;
    }
    made_clear(&parents);
    made_clear(&st->made);  // (it is the save folder now: nothing to undo)
    snprintf(st->dir, sizeof st->dir, "%s", save_dir);
    if (had) {  // what the old folder held besides save_0 (the SD card folder, ...) goes on being there
        int n;
        char **v = names_list(backup_dir, &n);
        for (int i = 0; v && i < n; i++) {
            char a[PATH_CAP], b[PATH_CAP];
            MadeList junk = {0};
            ScReport tmp = {0};
            if (!strcmp(v[i], "save_0") || !path_join(a, sizeof a, backup_dir, v[i]) || !path_join(b, sizeof b, save_dir, v[i]))
                continue;
            bool ok = is_dir(a) ? copy_tree(a, b, 0, &junk, &tmp) : is_file(a) ? copy_file(a, b, &junk, &tmp) : true;
            if (!ok) r->warnings++;
            made_clear(&junk);
        }
        if (v) names_free(v, n);
    }
    free(st);
    return SC_OK;
}

ScCode sc_stage_install(ScStage *st, const char *save_dir, const char *backup_dir, ScReport *r) {
    r->code = SC_OK;
    ScLock *lock;
    if (save_in_use(save_dir, &lock, r)) return r->code;  // (the caller still owns the stage)
    ScCode c = install_locked(st, save_dir, backup_dir, r);
    sc_lock_drop(lock);
    return c;
}

// Only the dated names this importer makes. A similarly named folder can belong to something else.
static bool dated_name(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n)) return false;
    const char *p = name + n;
    if (strlen(p) < 15) return false;
    for (int i = 0; i < 15; i++)
        if (i == 8 ? p[i] != '-' : !isdigit((unsigned char)p[i])) return false;
    return !p[15] || (p[15] == '-' && all_digits(p + 16));
}

static bool path_is_link(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

// Never descend through a link, including a junction inside an abandoned stage.
static bool remove_stale(const char *dir, int depth, ScReport *r) {
    if (depth > 32 || path_is_link(dir)) { fail(r, SC_IO, "%s: linked or too deeply nested", dir); return false; }
    int n;
    char **v = names_list(dir, &n);
    if (!v) { fail(r, SC_IO, "%s: %s", dir, strerror(errno)); return false; }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char p[PATH_CAP];
        if (!path_join(p, sizeof p, dir, v[i])) { fail(r, SC_IO, "%s: path too long", dir); ok = false; }
        else if (path_is_link(p)) { fail(r, SC_IO, "%s: linked", p); ok = false; }
        else if (is_dir(p)) ok = remove_stale(p, depth + 1, r);
        else if (remove(p) != 0) { fail(r, SC_IO, "%s: %s", p, strerror(errno)); ok = false; }
    }
    names_free(v, n);
    if (ok && rmdir(dir) != 0) { fail(r, SC_IO, "%s: %s", dir, strerror(errno)); ok = false; }
    return ok;
}

// Compare the date first, then the collision suffix as a number (10 is newer than 9).
static bool newer_dated(const char *a, const char *b) {
    int c = strncmp(a, b, 15);
    return c > 0 || (!c && strtoul(a + 15 + (a[15] == '-'), NULL, 10) > strtoul(b + 15 + (b[15] == '-'), NULL, 10));
}

static bool same_path(const char *a, const char *b) {
    while (a[0] == '.' && a[1] == '/') a += 2;
    while (b[0] == '.' && b[1] == '/') b += 2;
#ifdef _WIN32
    return !_stricmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

ScCode sc_cleanup_stages(const char *save_dir, const char *keep_dir, ScRecovery *recovery, ScReport *r) {
    memset(recovery, 0, sizeof *recovery);
    r->code = SC_OK;
    char base[PATH_CAP], parent[PATH_CAP], prefix[PATH_CAP], keep[PATH_CAP];
    strip_slash(base, sizeof base, save_dir);
#ifdef _WIN32
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
#endif
    snprintf(parent, sizeof parent, "%s", base);
    char *slash = strrchr(parent, '/');
    const char *name = strrchr(base, '/');
    name = name ? name + 1 : base;
    if (snprintf(prefix, sizeof prefix, "%s-import-", name) >= (int)sizeof prefix) {
        fail(r, SC_IO, "%s: path too long", base);
        return r->code;
    }
    if (slash) { if (slash == parent || (slash == parent + 2 && parent[1] == ':')) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    strip_slash(keep, sizeof keep, keep_dir ? keep_dir : "");
#ifdef _WIN32
    for (char *p = keep; *p; p++) if (*p == '\\') *p = '/';
#endif
    int n;
    char **v = names_list(parent, &n);
    if (!v) {
        if (errno != ENOENT) fail(r, SC_IO, "%s: %s", parent, strerror(errno));
        return r->code;
    }
    bool missing = !exists(base);
    const char *newest = NULL;
    for (int i = 0; i < n; i++) {
        char p[PATH_CAP];
        if (!dated_name(v[i], prefix)) continue;
        if (!path_join(p, sizeof p, parent, v[i])) { fail(r, SC_IO, "%s: path too long", parent); break; }
        if (!is_dir(p) || path_is_link(p)) continue;
        if (missing) {
            const char *date = v[i] + strlen(prefix);
            if (!newest || newer_dated(date, newest)) {
                newest = date;
                snprintf(recovery->imported, sizeof recovery->imported, "%s", p);
            }
        } else if (!same_path(p, keep)) {
            if (!remove_stale(p, 0, r)) break;
        }
    }
    if (recovery->imported[0]) {
        char backups[PATH_CAP];
        if (snprintf(backups, sizeof backups, "%s-backups", base) >= (int)sizeof backups) {
            fail(r, SC_IO, "%s: path too long", base);
        } else {
            int nb;
            char **b = names_list(backups, &nb);
            const char *latest = NULL;
            for (int i = 0; b && i < nb; i++) {
                char p[PATH_CAP];
                if (!dated_name(b[i], "save-") || !path_join(p, sizeof p, backups, b[i]) || !is_dir(p) || path_is_link(p)) continue;
                if (!latest || newer_dated(b[i] + 5, latest)) {
                    latest = b[i] + 5;
                    snprintf(recovery->backup, sizeof recovery->backup, "%s", p);
                }
            }
            if (b) names_free(b, nb);
        }
    }
    names_free(v, n);
    return r->code;
}

// ------------------------------------------------------------------ slots
int sc_list_slots(const char *save_0_dir, ScSlot **out) {
    int n, cnt = 0;
    char **v = names_list(save_0_dir, &n);
    ScSlot *slots = calloc(n ? n : 1, sizeof *slots);
    for (int i = 0; v && i < n; i++) {
        char sd[PATH_CAP], gd[PATH_CAP], cp[PATH_CAP], jp[PATH_CAP];
        struct stat sb;
        if (!all_digits(v[i]) || strlen(v[i]) >= sizeof slots->name || !path_join(sd, sizeof sd, save_0_dir, v[i]) ||
            !path_join(gd, sizeof gd, sd, "game_data.sav") || !path_join(cp, sizeof cp, sd, "caption.sav") ||
            !path_join(jp, sizeof jp, sd, "caption.jpg") || !is_dir(sd))
            continue;
        bool have_cap = is_file(cp);
        if (!have_cap && !is_file(gd)) continue;
        ScSlot *s = &slots[cnt++];
        snprintf(s->name, sizeof s->name, "%s", v[i]);
        s->number = atoi(v[i]);
        s->saved = stat(have_cap ? cp : gd, &sb) == 0 ? sb.st_mtime : 0;
        s->has_picture = is_file(jp);
        s->play_seconds = -1;
        // the play time in seconds: the flag a real save keeps in game_data.sav, else a PlayTime flag in caption.sav
        const char *play_files[] = {gd, cp};
        const char *play_flags[] = {"PlayReport_PlayTime", "PlayTime"};
        for (int k = 0; k < 2 && s->play_seconds < 0; k++) {
            if (!is_file(play_files[k])) continue;
            char why[64];
            size_t len;
            uint8_t *d = read_file(play_files[k], &len, 1 << 26, why, sizeof why);
            bool le;
            if (d && detect_endian(d, len, &le, why, sizeof why)) {
                uint32_t want = crc32_name(play_flags[k]);
                for (size_t o = HEADER; o + 8 <= len - 4; o += 8)
                    if (rd32(d + o, le) == want) {
                        int32_t sec = (int32_t)rd32(d + o + 4, le);
                        if (sec >= 0 && sec < 100000 * 3600) s->play_seconds = sec;
                        break;
                    }
            }
            free(d);
        }
    }
    if (v) names_free(v, n);
    for (int i = 1; i < cnt; i++)  // by number (the names sort as text)
        for (int j = i; j > 0 && slots[j].number < slots[j - 1].number; j--) {
            ScSlot t = slots[j];
            slots[j] = slots[j - 1], slots[j - 1] = t;
        }
    *out = slots;
    return cnt;
}
