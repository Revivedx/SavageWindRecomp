// Settings > Camera. The game's cameras are AI actions of the actor GameRomCamera, with their values in its program
// (Actor/AIProgram/GameRomCamera.baiprog). The styles are the Classic Camera mod's (camera_styles.inc): its values put in
// that program as the game reads the actor (runtime/services/fs.c camera_patch), the same as BCML merging the mod in.
// They need a restart.
#include "camera_tune.h"

#include <string.h>

const char *const g_camera_style_names[CAMERA_STYLES] = {
    "Default", "Classic", "Classic, bow over the shoulder", "Close", "Close, bow over the shoulder", "Far",
    "Far, bow over the shoulder", "Over the shoulder, close", "Over the shoulder",
};

// ---- the program. AAMP 2: a header, then lists (name crc, lists, how many, objects, how many), objects (name crc,
// parameters, how many) and parameters (name crc, data and type), each position counted in words from the entry itself.
static uint32_t name_crc(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

typedef struct { uint8_t *d; size_t n, cap; } Aamp;  // (n: its size now, cap: what it can grow to)

static uint32_t u32_at(const Aamp *a, size_t at) { uint32_t v; memcpy(&v, a->d + at, 4); return v; }
static uint16_t u16_at(const Aamp *a, size_t at) { uint16_t v; memcpy(&v, a->d + at, 2); return v; }
static bool holds(const Aamp *a, size_t at, size_t size) { return at <= a->n && size <= a->n - at; }

// A list's child list / object / an object's parameter by index: where it is, 0 when it would be outside the file
static size_t list_child(const Aamp *a, size_t list, int i) {
    size_t at = list + (size_t)u16_at(a, list + 4) * 4 + (size_t)i * 12;
    return holds(a, at, 12) ? at : 0;
}
static size_t list_object(const Aamp *a, size_t list, int i) {
    size_t at = list + (size_t)u16_at(a, list + 8) * 4 + (size_t)i * 8;
    return holds(a, at, 8) ? at : 0;
}
static size_t object_param(const Aamp *a, size_t obj, int i) {
    size_t at = obj + (size_t)u16_at(a, obj + 4) * 4 + (size_t)i * 8;
    return holds(a, at, 8) ? at : 0;
}

// Where the parameter's data is, 0 outside the file
static size_t param_data(const Aamp *a, size_t param, size_t size) {
    size_t at = param + (size_t)(u32_at(a, param + 4) & 0xFFFFFF) * 4;
    return holds(a, at, size) ? at : 0;
}

static size_t find_list(const Aamp *a, size_t list, uint32_t crc) {
    for (int i = 0, n = u16_at(a, list + 6); i < n; i++) {
        size_t l = list_child(a, list, i);
        if (l && u32_at(a, l) == crc) return l;
    }
    return 0;
}

static size_t find_object(const Aamp *a, size_t list, uint32_t crc) {
    for (int i = 0, n = u16_at(a, list + 10); i < n; i++) {
        size_t o = list_object(a, list, i);
        if (o && u32_at(a, o) == crc) return o;
    }
    return 0;
}

static size_t find_param(const Aamp *a, size_t obj, uint32_t crc) {
    for (int i = 0, n = u16_at(a, obj + 6); i < n; i++) {
        size_t p = object_param(a, obj, i);
        if (p && u32_at(a, p) == crc) return p;
    }
    return 0;
}

// How many bytes a parameter's value takes, 0 for a kind this doesn't know
static size_t param_size(const Aamp *a, size_t param) {
    size_t at = param_data(a, param, 0), cap;
    if (!at) return 0;
    switch (u32_at(a, param + 4) >> 24) {
    case 0: case 1: case 2: case 17: return 4;
    case 3: return 8;
    case 4: return 12;
    case 5: case 6: case 16: return 16;
    case 7: cap = 32; break;
    case 8: cap = 64; break;
    case 15: cap = 256; break;
    case 20: cap = a->n - at; break;
    default: return 0;
    }
    size_t len = 0;  // (a string: its characters and the 0 after them, in words)
    while (len < cap && at + len < a->n && a->d[at + len]) len++;
    return (len + 4) & ~(size_t)3;
}

// ---- the styles: the values each sets, by list, child list, object and parameter name crc. The program keeps each
// value once, and every parameter that has it points there, so a value set goes after the file, the parameter pointed
// at it. Where a camera becomes another kind (its Def's ClassName) it gets the new kind's parameters too, so objects
// can grow.
enum { CAM_BOOL = 0, CAM_F32 = 1, CAM_INT = 2, CAM_VEC3 = 4, CAM_STR32 = 7, CAM_U32 = 17 };  // (the AAMP types)
typedef struct { uint32_t list, child, obj, param; uint8_t type; float v[3]; const char *s; } CamStyleParam;
#include "camera_styles.inc"
#define STYLE(t) {t, sizeof t / sizeof t[0]}
static const struct { const CamStyleParam *p; int n; } style_over[CAMERA_STYLES] = {
    {0, 0}, {0, 0}, STYLE(style_classic_ots_bow), STYLE(style_close), STYLE(style_close_ots_bow), STYLE(style_far),
    STYLE(style_far_ots_bow), STYLE(style_ots_close), STYLE(style_ots),
};

static size_t style_object(const Aamp *a, size_t root, const CamStyleParam *e) {
    size_t l = find_list(a, root, e->list), c = l ? find_list(a, l, e->child) : 0;
    return c ? find_object(a, c, e->obj) : 0;
}

// The object's parameters again at the end of the file with room for more after them, each with its own copy of its
// value (a parameter's value has to come after it). False when there's no room.
static bool object_grow(Aamp *a, size_t obj, int more) {
    int n = u16_at(a, obj + 6);
    size_t table = a->n, at = table + (size_t)(n + more) * 8, need = 0;
    if ((table - obj) / 4 > 0xFFFF) return false;
    for (int i = 0; i < n; i++) {
        size_t p = object_param(a, obj, i), size = p ? param_size(a, p) : 0;
        if (!size) return false;
        need += size;
    }
    if (at + need > a->cap) return false;
    for (int i = 0; i < n; i++) {
        size_t p = object_param(a, obj, i), size = param_size(a, p), from = param_data(a, p, size), q = table + (size_t)i * 8;
        memcpy(a->d + q, a->d + p, 8);
        memcpy(a->d + at, a->d + from, size);
        uint32_t info = (u32_at(a, p + 4) & 0xFF000000u) | (uint32_t)((at - q) / 4);
        memcpy(a->d + q + 4, &info, 4);
        at += size;
    }
    memset(a->d + table + (size_t)n * 8, 0, (size_t)more * 8);
    uint16_t words = (uint16_t)((table - obj) / 4);
    memcpy(a->d + obj + 4, &words, 2);
    a->n = at;
    return true;
}

// The parameter (one already there, or the object's next free one) holding the value, after the file
static bool put_value(Aamp *a, size_t param, const CamStyleParam *e) {
    uint8_t v[32] = {0};
    size_t size = 4;
    if (e->type == CAM_F32) memcpy(v, &e->v[0], 4);
    else if (e->type == CAM_VEC3) memcpy(v, e->v, 12), size = 12;
    else if (e->type == CAM_STR32) {
        size_t len = strlen(e->s) < 31 ? strlen(e->s) : 31;
        memcpy(v, e->s, len), size = (len + 4) & ~(size_t)3;
    } else {
        int32_t i = (int32_t)e->v[0];
        memcpy(v, &i, 4);
    }
    size_t at = a->n;
    if (at + size > a->cap || (at - param) / 4 > 0xFFFFFF) return false;
    memcpy(a->d + at, v, size);
    a->n = at + size;
    uint32_t info = (uint32_t)e->type << 24 | (uint32_t)((at - param) / 4);
    memcpy(a->d + param, &e->param, 4);
    memcpy(a->d + param + 4, &info, 4);
    return true;
}

// How many of its values it set
static int apply_style(Aamp *a, size_t root, const CamStyleParam *t, int n) {
    // first the objects that need room for new parameters, each grown once
    enum { MAX_GROWN = 64 };
    size_t grown[MAX_GROWN];
    int more[MAX_GROWN], ngrown = 0, done = 0;
    for (int i = 0; i < n; i++) {
        size_t obj = style_object(a, root, &t[i]);
        if (!obj || find_param(a, obj, t[i].param)) continue;
        int k = 0;
        while (k < ngrown && grown[k] != obj) k++;
        if (k == MAX_GROWN) return 0;
        if (k == ngrown) grown[ngrown] = obj, more[ngrown++] = 0;
        more[k]++;
    }
    for (int k = 0; k < ngrown; k++)
        if (!object_grow(a, grown[k], more[k])) return 0;
    for (int i = 0; i < n; i++) {
        size_t obj = style_object(a, root, &t[i]), p = obj ? find_param(a, obj, t[i].param) : 0;
        if (!obj) continue;
        if (!p) {  // (the object's next free one)
            uint16_t count = u16_at(a, obj + 6);
            p = obj + (size_t)u16_at(a, obj + 4) * 4 + (size_t)count * 8;
            count++;
            memcpy(a->d + obj + 6, &count, 2);
        }
        done += put_value(a, p, &t[i]);
    }
    return done;
}

int camera_style_program(uint8_t *file, size_t *size, size_t cap, int style) {
    Aamp a = {file, *size, cap};
    if (style <= 0 || style >= CAMERA_STYLES || *size < 0x34 || memcmp(file, "AAMP", 4) || u32_at(&a, 4) != 2) return 0;
    size_t root = 0x30 + (size_t)u32_at(&a, 0x14);
    if (!holds(&a, root, 12)) return 0;
    // the mod's base, then the option over it, as BCML merges them
    int done = apply_style(&a, root, style_classic, sizeof style_classic / sizeof style_classic[0]);
    done += apply_style(&a, root, style_over[style].p, style_over[style].n);
    uint32_t total = (uint32_t)a.n;
    memcpy(file + 0xC, &total, 4);
    *size = a.n;
    return done;
}

// ---- what a program holds, for changing the style while the game runs (runtime/camera_live.c)
static size_t program_actions(const Aamp *a) {
    if (a->n < 0x34 || memcmp(a->d, "AAMP", 4) || u32_at(a, 4) != 2) return 0;
    size_t root = 0x30 + (size_t)u32_at(a, 0x14);
    return holds(a, root, 12) ? find_list(a, root, name_crc("Action")) : 0;
}

int camera_program_values(const uint8_t *file, size_t n, CameraValue *out, int max) {
    Aamp a = {(uint8_t *)file, n, n};
    size_t actions = program_actions(&a);
    int count = 0;
    for (int k = 0, m = actions ? u16_at(&a, actions + 6) : 0; k < m; k++) {
        size_t cam = list_child(&a, actions, k), s = cam ? find_object(&a, cam, name_crc("SInst")) : 0;
        for (int i = 0, np = s ? u16_at(&a, s + 6) : 0; i < np && count < max; i++) {
            size_t p = object_param(&a, s, i), type = p ? u32_at(&a, p + 4) >> 24 : 99;
            size_t size = type == 4 ? 12 : type == 0 || type == 1 || type == 2 || type == 17 ? 4 : 0, at;
            if (!size || !(at = param_data(&a, p, size))) continue;  // (strings: never set by a style)
            CameraValue *v = &out[count++];
            v->action = (uint16_t)k, v->type = (uint8_t)type, v->size = (uint8_t)size, v->crc = u32_at(&a, p);
            memcpy(v->v, a.d + at, size);
        }
    }
    return count;
}

int camera_program_kinds(const uint8_t *file, size_t n, char (*out)[32], int max) {
    Aamp a = {(uint8_t *)file, n, n};
    size_t actions = program_actions(&a);
    uint32_t def = name_crc("Def"), cls = name_crc("ClassName");
    int m = actions ? u16_at(&a, actions + 6) : 0;
    for (int k = 0; k < m && k < max; k++) {
        size_t c = list_child(&a, actions, k), d = c ? find_object(&a, c, def) : 0, p = d ? find_param(&a, d, cls) : 0;
        size_t size = p ? param_size(&a, p) : 0;
        memset(out[k], 0, 32);
        if (size) memcpy(out[k], a.d + param_data(&a, p, size), size < 31 ? size : 31);
    }
    return m < max ? m : max;
}

bool camera_program_same_kinds(const uint8_t *x, size_t xn, const uint8_t *y, size_t yn) {
    static char a[512][32], b[512][32];
    int na = camera_program_kinds(x, xn, a, 512), nb = camera_program_kinds(y, yn, b, 512);
    return na && na == nb && !memcmp(a, b, sizeof a[0] * (size_t)na);
}

// ---- room for it. SARC: a header, the nodes (name hash, attributes, start, end), then the files' data
bool camera_sarc_grow(uint8_t *d, size_t n, size_t cap, uint32_t data_off, uint32_t start, uint32_t end, uint32_t grow) {
    if (n + grow > cap || n < 0x20) return false;
    size_t after = (size_t)data_off + end;
    uint32_t nodes = d[0x1A] | (uint32_t)d[0x1B] << 8;
    if (after > n || 0x20 + (size_t)nodes * 16 > data_off) return false;
    memmove(d + after + grow, d + after, n - after);
    memset(d + after, 0, grow);
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t s, e;
        memcpy(&s, d + 0x20 + i * 16 + 8, 4);
        memcpy(&e, d + 0x20 + i * 16 + 12, 4);
        if (s >= end) s += grow;
        if (e > start && e >= end) e += grow;
        memcpy(d + 0x20 + i * 16 + 8, &s, 4);
        memcpy(d + 0x20 + i * 16 + 12, &e, 4);
    }
    uint32_t total;
    memcpy(&total, d + 8, 4);
    total += grow;
    memcpy(d + 8, &total, 4);
    return true;
}
