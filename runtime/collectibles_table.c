// The Korok seeds, shrines and towers of Cheats > Collectible hints on the map, from the player's own game files: see
// collectibles_table.h.
//
// - Korok seeds: the hidden Koroks (Npc_HiddenKorokGround, Npc_HiddenKorokFly) of the main field's map units; each
//   sets its own bool flag MainField_<actor>_<HashId> when found.
// - Shrines: the shrine markers (Icon Dungeon, MessageID DungeonNNN) of Map/MainField/Static.smubin;
//   Location_DungeonNNN (s32, not 0 once found) and Clear_DungeonNNN (bool).
// - Towers: the tower markers (Icon Tower, MessageID TowerNN); MapTower_NN (bool) is set when the tower is activated
//   (the marker's own Location_TowerNN only says it was seen). Each stands in its own area of MapTower.sbeco: a
//   region, and the regions are numbered in the order of their areas.
// - Every flag is checked in GameData/gamedata.ssarc: there, of its kind (bool_data, s32_data), with the crc32 of its
//   name as its hash (HashValue), which is how runtime/collectibles.c finds it in the game's memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collectibles_table.h"
#include "rt.h"
#include "services/archive.h"
#include "services/formats.h"

uint8_t *fs_game_file(const char *path, size_t *out_n);  // runtime/services/fs.c

#define WORLD_X 10000  // the main field: x -5000..5000, z -4000..4000
#define WORLD_Z 8000

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint32_t crc32_str(const char *s) {
    uint32_t c = 0xffffffff;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}

// A Yaz0 file of an archive (SARC), unpacked and malloc'd; NULL: not there
static uint8_t *pack_file(const uint8_t *pack, size_t pn, const char *name, size_t *out_n) {
    uint32_t doff, s, e;
    if (!pack || sarc_find(pack, pn, name, &doff, &s, &e) < 0 || e < s || doff + (size_t)e > pn) return NULL;
    return yaz0_unpack(pack + doff + s, e - s, out_n);
}

static uint8_t *game_file_unpacked(const char *path, size_t *out_n) {
    size_t zn = 0;
    uint8_t *z = fs_game_file(path, &zn), *d = z ? yaz0_unpack(z, zn, out_n) : NULL;
    free(z);
    return d;
}

// ---- the tower areas
static int area_at(const CollectTable *t, float x, float z) {
    if (t->eco_n < 0x10 + 4 * (size_t)t->rows || !t->rows || !t->row_h) return -1;
    int row = (int)(((double)z + WORLD_Z / 2) / t->row_h);
    row = row < 0 ? 0 : row >= (int)t->rows ? (int)t->rows - 1 : row;
    int left = (int)((double)x + WORLD_X / 2);
    left = left < 0 ? 0 : left >= WORLD_X ? WORLD_X - 1 : left;
    size_t o = 0x10 + 4 * (size_t)t->rows + 2 * (size_t)rd32(t->eco + 0x10 + 4 * row);
    for (; o + 4 <= t->eco_n; o += 4) {
        uint16_t a = rd16(t->eco + o), n = rd16(t->eco + o + 2);
        if (left < n) return a;
        left -= n;
    }
    return -1;
}

int collect_table_region(const CollectTable *t, float x, float z) {
    if (!(x >= -WORLD_X / 2 && x < WORLD_X / 2 && z >= -WORLD_Z / 2 && z < WORLD_Z / 2)) return -1;
    int a = area_at(t, x, z);
    for (int r = 0; a >= 0 && r < COLLECT_REGIONS; r++)
        if (t->area[r] == a) return r;
    return -1;
}

// ---- the flags wanted, checked against gamedata
enum { KIND_BOOL, KIND_S32 };
typedef struct { uint32_t hash; uint8_t kind; bool ok; char name[64]; } Want;
typedef struct { Want *w; int n, cap; bool short_of_memory; } Wants;

static uint32_t want(Wants *ws, const char *name, int kind) {
    if (ws->n == ws->cap) {
        Want *w = realloc(ws->w, sizeof *ws->w * (ws->cap ? ws->cap * 2 : 1024));
        if (!w) { ws->short_of_memory = true; return 0; }
        ws->w = w, ws->cap = ws->cap ? ws->cap * 2 : 1024;
    }
    Want *w = &ws->w[ws->n++];
    snprintf(w->name, sizeof w->name, "%s", name);
    w->hash = crc32_str(name), w->kind = (uint8_t)kind, w->ok = false;
    return w->hash;
}

static int want_cmp(const void *a, const void *b) {
    uint32_t x = ((const Want *)a)->hash, y = ((const Want *)b)->hash;
    return x < y ? -1 : x > y;
}

// Each wanted flag found in gamedata, of its kind and with its name's crc32 as its hash; false (logged) if one isn't
static bool wants_check(Wants *ws, const uint8_t *boot, size_t bn) {
    if (ws->short_of_memory) { LOG("[collect] out of memory\n"); return false; }
    size_t gn = 0;
    uint8_t *g = pack_file(boot, bn, "GameData/gamedata.ssarc", &gn);
    int files = g ? sarc_count(g, gn) : -1;
    if (files <= 0) { LOG("[collect] no GameData/gamedata.ssarc in Pack/Bootup.pack\n"); free(g); return false; }
    qsort(ws->w, ws->n, sizeof *ws->w, want_cmp);
    for (int i = 0; i < files; i++) {
        const char *name; const uint8_t *data; size_t size;
        Byml b;
        if (!sarc_node(g, gn, i, &name, &data, &size) || !byml_open(&b, data, size)) continue;
        for (uint32_t k = 0; k < byml_count(&b, b.root); k++) {  // (bool_data: [...], s32_data: [...], ...)
            uint8_t ty; uint32_t list; const char *kind_name;
            if (!byml_at(&b, b.root, k, &ty, &list, &kind_name) || ty != BYML_ARRAY || !kind_name) continue;
            int kind = !strcmp(kind_name, "bool_data") ? KIND_BOOL : !strcmp(kind_name, "s32_data") ? KIND_S32 : -1;
            if (kind < 0) continue;
            for (uint32_t e = 0; e < byml_count(&b, list); e++) {
                uint32_t entry;
                if (!byml_at(&b, list, e, &ty, &entry, NULL) || ty != BYML_DICT) continue;
                uint8_t ht; uint32_t hash;
                const char *data_name = byml_get_str(&b, entry, "DataName");
                if (!data_name || !byml_get(&b, entry, "HashValue", &ht, &hash)) continue;
                Want key = {.hash = hash};
                Want *w = bsearch(&key, ws->w, ws->n, sizeof *ws->w, want_cmp);
                for (; w && w > ws->w && w[-1].hash == hash; w--) {}
                for (; w && w < ws->w + ws->n && w->hash == hash; w++)
                    if (w->kind == kind && !strcmp(w->name, data_name)) w->ok = true;
            }
        }
    }
    free(g);
    int bad = 0;
    for (int i = 0; i < ws->n; i++)
        if (!ws->w[i].ok && !bad++)
            LOG("[collect] %s: no %s flag with the crc32 of its name in gamedata\n", ws->w[i].name,
                ws->w[i].kind == KIND_BOOL ? "bool" : "s32");
    if (bad > 1) LOG("[collect] (%d flags like it)\n", bad);
    return !bad;
}

// ---- the table
static int korok_cmp(const void *a, const void *b) {
    const CollectKorok *x = a, *y = b;
    if (x->region != y->region) return x->region - y->region;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->found < y->found ? -1 : x->found > y->found;
}
static int shrine_cmp(const void *a, const void *b) {
    const CollectShrine *x = a, *y = b;
    if (x->region != y->region) return x->region - y->region;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->found < y->found ? -1 : x->found > y->found;
}

// The region of a Korok, shrine or tower at x, z; -1 (logged): none
static int region_of(const CollectTable *t, const char *what, float x, float z) {
    int r = collect_table_region(t, x, z);
    if (r < 0) LOG("[collect] %s at %.0f %.0f: in no tower region\n", what, x, z);
    return r;
}

// The hidden Koroks of a map unit file (none when it isn't there) added to out: their flags wanted; how many out has
// then. -1 (logged): one doesn't read as expected, or more than max
static int unit_koroks(CollectTable *t, Wants *ws, const char *path, CollectKorok *out, int n, int max) {
    size_t dn = 0;
    uint8_t *d = game_file_unpacked(path, &dn);
    Byml b;
    uint8_t ty; uint32_t objs;
    if (!byml_open(&b, d, dn) || !byml_get(&b, b.root, "Objs", &ty, &objs) || ty != BYML_ARRAY) { free(d); return n; }
    for (uint32_t i = 0; i < byml_count(&b, objs); i++) {
        uint32_t o, id, pos;
        uint8_t it, pt;
        float x, z;
        if (!byml_at(&b, objs, i, &ty, &o, NULL) || ty != BYML_DICT) continue;
        const char *actor = byml_get_str(&b, o, "UnitConfigName");
        if (!actor || (strcmp(actor, "Npc_HiddenKorokGround") && strcmp(actor, "Npc_HiddenKorokFly"))) continue;
        if (!byml_get(&b, o, "HashId", &it, &id) || (it != BYML_UINT && it != BYML_INT) ||
            !byml_get(&b, o, "Translate", &pt, &pos) || pt != BYML_ARRAY || !byml_at_float(&b, pos, 0, &x) ||
            !byml_at_float(&b, pos, 2, &z)) {
            LOG("[collect] %s: a hidden Korok without its HashId or place\n", path);
            free(d);
            return -1;
        }
        char name[64];
        snprintf(name, sizeof name, "MainField_%s_%u", actor, id);
        int r = region_of(t, name, x, z);
        if (n == max) LOG("[collect] more than %d hidden Koroks in the map units\n", max);
        if (r < 0 || n == max) { free(d); return -1; }
        out[n++] = (CollectKorok){(uint8_t)r, want(ws, name, KIND_BOOL), x, z};
    }
    free(d);
    return n;
}

// The towers' markers (pass 0: their areas and labels) or the shrines' (pass 1: their flags wanted)
static bool markers(CollectTable *t, Wants *ws, const Byml *b, uint32_t list, int pass, char tower_msg[][16], int *count) {
    for (uint32_t i = 0; i < byml_count(b, list); i++) {
        uint8_t ty; uint32_t m, pos;
        float x, z;
        if (!byml_at(b, list, i, &ty, &m, NULL) || ty != BYML_DICT) continue;
        const char *icon = byml_get_str(b, m, "Icon"), *msg = byml_get_str(b, m, "MessageID");
        if (!icon || !msg || strcmp(icon, pass ? "Dungeon" : "Tower")) continue;
        if (!byml_get(b, m, "Translate", &ty, &pos) || ty != BYML_DICT || !byml_get_float(b, pos, "X", &x) ||
            !byml_get_float(b, pos, "Z", &z)) {
            LOG("[collect] %s: a marker without its place\n", msg);
            return false;
        }
        char name[64];
        if (!pass) {  // (TowerNN, alone in its area)
            int a = area_at(t, x, z), k = 0;
            while (k < *count && t->area[k] != a) k++;
            if (a < 0 || k < *count || *count == COLLECT_REGIONS || strncmp(msg, "Tower", 5) || strlen(msg) >= 16 ||
                !msg[5] || strspn(msg + 5, "0123456789") != strlen(msg + 5)) {
                LOG("[collect] tower marker %s: towers named TowerNN, each in an area of its own, expected\n", msg);
                return false;
            }
            t->area[*count] = (uint16_t)a;
            snprintf(tower_msg[(*count)++], 16, "%s", msg);
            continue;
        }
        int r = region_of(t, msg, x, z);
        if (r < 0) return false;
        if (*count == COLLECT_SHRINES) { LOG("[collect] more than %d shrine markers\n", COLLECT_SHRINES); return false; }
        CollectShrine *sh = &t->shrine[(*count)++];
        sh->region = (uint8_t)r, sh->x = x, sh->z = z;
        snprintf(name, sizeof name, "Location_%s", msg);
        sh->found = want(ws, name, KIND_S32);
        snprintf(name, sizeof name, "Clear_%s", msg);
        sh->cleared = want(ws, name, KIND_BOOL);
    }
    return true;
}

static bool build(CollectTable *t, Wants *ws, const uint8_t *boot, size_t bn) {
    // The tower areas, and the markers: the towers first (their areas number the regions), then the shrines
    t->eco = pack_file(boot, bn, "Ecosystem/MapTower.sbeco", &t->eco_n);
    if (!t->eco || t->eco_n < 0x10 || rd32(t->eco) != 0x00112233) {
        LOG("[collect] no tower areas (Ecosystem/MapTower.sbeco) in Pack/Bootup.pack\n");
        return false;
    }
    t->rows = rd32(t->eco + 4), t->row_h = rd32(t->eco + 8);
    size_t sn = 0;
    uint8_t *s = pack_file(boot, bn, "Map/MainField/Static.smubin", &sn);
    Byml b;
    uint8_t ty; uint32_t list;
    if (!byml_open(&b, s, sn) || !byml_get(&b, b.root, "LocationMarker", &ty, &list) || ty != BYML_ARRAY) {
        LOG("[collect] no map markers (Map/MainField/Static.smubin) in Pack/Bootup.pack\n");
        free(s);
        return false;
    }
    char tower_msg[COLLECT_REGIONS][16];
    int towers = 0, shrines = 0;
    bool ok = markers(t, ws, &b, list, 0, tower_msg, &towers);
    if (ok && towers != COLLECT_REGIONS)
        LOG("[collect] %d tower markers, %d expected\n", towers, COLLECT_REGIONS), ok = false;
    if (ok)  // (the regions in the order of their areas, the towers' labels with them)
        for (int i = 1; i < COLLECT_REGIONS; i++)
            for (int k = i; k > 0 && t->area[k - 1] > t->area[k]; k--) {
                uint16_t a = t->area[k];
                char m[16];
                t->area[k] = t->area[k - 1], t->area[k - 1] = a;
                memcpy(m, tower_msg[k], 16), memcpy(tower_msg[k], tower_msg[k - 1], 16), memcpy(tower_msg[k - 1], m, 16);
            }
    ok = ok && markers(t, ws, &b, list, 1, NULL, &shrines);
    free(s);
    if (ok && shrines != COLLECT_SHRINES)
        LOG("[collect] %d shrine markers, %d expected\n", shrines, COLLECT_SHRINES), ok = false;
    if (!ok) return false;
    for (int r = 0; r < COLLECT_REGIONS; r++) {
        char name[64];
        memcpy(t->tower_msg[r], tower_msg[r], 16);
        snprintf(name, sizeof name, "MapTower_%s", tower_msg[r] + 5);
        t->tower[r] = want(ws, name, KIND_BOOL);
    }
    t->koroks_found = want(ws, "HiddenKorok_Number", KIND_S32);

    // The hidden Koroks of each map unit (A-1 .. J-8), each once
    enum { MAX = COLLECT_KOROKS + 64 };
    CollectKorok *k = malloc(sizeof *k * MAX);
    int n = 0;
    for (char col = 'A'; k && col <= 'J' && n >= 0; col++)
        for (int row = 1; row <= 8 && n >= 0; row++)
            for (int kind = 0; kind < 2 && n >= 0; kind++) {
                char path[64];
                snprintf(path, sizeof path, "Map/MainField/%c-%d/%c-%d_%s.smubin", col, row, col, row,
                         kind ? "Dynamic" : "Static");
                n = unit_koroks(t, ws, path, k, n, MAX);
            }
    if (n > 0) {
        qsort(k, n, sizeof *k, korok_cmp);
        int w = 0;
        for (int i = 0; i < n; i++) {
            bool seen = false;
            for (int j = 0; j < w && !seen; j++) seen = k[j].found == k[i].found;
            if (!seen) k[w++] = k[i];
        }
        n = w;
    }
    if (n != COLLECT_KOROKS) {
        if (n >= 0) LOG("[collect] %d hidden Koroks in the map units, %d expected\n", n, COLLECT_KOROKS);
        free(k);
        return false;
    }
    memcpy(t->korok, k, sizeof t->korok);
    free(k);
    qsort(t->shrine, COLLECT_SHRINES, sizeof *t->shrine, shrine_cmp);
    return wants_check(ws, boot, bn);
}

bool collect_table_build(CollectTable *t) {
    memset(t, 0, sizeof *t);
    size_t bn = 0;
    uint8_t *boot = fs_game_file("Pack/Bootup.pack", &bn);
    if (!boot) { LOG("[collect] no Pack/Bootup.pack in the game files\n"); return false; }
    Wants ws = {0};
    bool ok = build(t, &ws, boot, bn);
    free(boot);
    free(ws.w);
    if (!ok) free(t->eco), t->eco = NULL, t->eco_n = 0;
    return ok;
}

// ---- the regions' names
typedef struct { const CollectTable *t; char (*names)[64]; int found; } Names;

static void name_of(void *ctx, const char *label, size_t len, const char *text) {
    Names *n = ctx;
    for (int r = 0; r < COLLECT_REGIONS; r++) {
        if (strlen(n->t->tower_msg[r]) != len || memcmp(n->t->tower_msg[r], label, len)) continue;
        // A long name is broken to fit the map: a space, except after a CJK letter (from U+2E80: 3 bytes, the first
        // from 0xe2 on)
        char *o = n->names[r], *end = o + 63;
        const char *last = NULL;
        for (const char *p = text; *p && o < end; p++) {
            if (*p == '\n') {
                if (o > n->names[r] && !(last && (uint8_t)*last >= 0xe2 && !((uint8_t)*last == 0xe2 && (uint8_t)last[1] < 0xba))) *o++ = ' ';
                continue;
            }
            if (((uint8_t)*p & 0xc0) != 0x80) last = p;
            *o++ = *p;
        }
        *o = 0;
        // "Lake Tower" -> "Lake"; "Torre del lago" -> "Lago" (other languages build the name in too many ways:
        // Hebra-Turm, Turm der Ebene; there it stays the tower's)
        size_t l = strlen(n->names[r]);
        static const char *const pre[] = {"Torre de las ", "Torre de la ", "Torre del ", "Torre de "};
        if (l > 6 && !strcmp(n->names[r] + l - 6, " Tower")) n->names[r][l - 6] = 0;
        else
            for (int k = 0; k < 4; k++)
                if (!strncmp(n->names[r], pre[k], strlen(pre[k]))) {
                    memmove(n->names[r], n->names[r] + strlen(pre[k]), l - strlen(pre[k]) + 1);
                    if (n->names[r][0] >= 'a' && n->names[r][0] <= 'z') n->names[r][0] -= 'a' - 'A';
                    break;
                }
        n->found++;
    }
}

bool collect_table_names(const CollectTable *t, const char *folder, char names[COLLECT_REGIONS][64]) {
    char path[64], inner[64];
    snprintf(path, sizeof path, "Pack/Bootup_%s.pack", folder);
    snprintf(inner, sizeof inner, "Message/Msg_%s.product.ssarc", folder);
    size_t pn = 0, mn = 0, ln = 0;
    uint8_t *pack = fs_game_file(path, &pn), *msg = pack_file(pack, pn, inner, &mn), *lm = NULL;
    free(pack);
    uint32_t doff, s, e;
    if (msg && sarc_find(msg, mn, "StaticMsg/LocationMarker.msbt", &doff, &s, &e) >= 0 && e >= s && doff + (size_t)e <= mn)
        lm = msg + doff + s, ln = e - s;
    memset(names, 0, COLLECT_REGIONS * 64);
    Names n = {t, names, 0};
    if (lm) msbt_each(lm, ln, name_of, &n);
    free(msg);
    if (n.found < COLLECT_REGIONS) LOG("[collect] %d of the towers' names in %s\n", n.found, path);
    return n.found >= COLLECT_REGIONS;
}
