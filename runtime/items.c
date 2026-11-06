// The game's items, for the give command and the overlay's Cheats tab: see runtime/items.h.
//
// The list: every actor of Actor/ActorInfo.product.sbyml (BYML) tagged CanGetPouch, by profile (weapons, bows,
// shields, arrows, armour, materials), leaving out the ones tagged Important (the Sheikah Slate, the Master Sword,
// quest items, Korok seeds), cooked dishes (CookResult: they need their effects) and rupees. Each has the name the
// game gives it, <actor>_Name in Message/Msg_<lang>.product.ssarc's ActorType/*.msbt inside Pack/Bootup_<lang>.pack;
// one without a name is left out, and of several actors with one name (armour's upgrade levels, dyes) the first by
// actor name stays.
//
// Adding: the pause menu data's item get (1.9.0 0x7000d921bc; x0 the pause menu data, x1 the item's name as a
// sead::SafeString, w2 its value, x3 its weapon modifier or NULL) is what picking an item up calls: a stack's value
// is how many, a weapon's its durability x 100 (a new one: generalLife x 100). The game thread calls it from
// rt_item_tick (tools/patches.py _item_tick), at the entry of Link's damage handling, which runs every frame in play,
// as picking an item up does.
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "cpu.h"
#include "gamelib.h"
#include "hle.h"
#include "items.h"
#include "language.h"
#include "nvn/tegra.h"
#include "rt.h"
#include "services/archive.h"

uint8_t *fs_game_file(const char *path, size_t *out_n);  // runtime/services/fs.c

// ---- BYML (little-endian "YB", version 2 and up): dictionaries, arrays, strings and numbers
typedef struct {
    const uint8_t *d;
    size_t n;
    uint32_t keys, strs;  // their string tables
} Byml;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static char *copy(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (o) memcpy(o, s, n), o[n] = 0;
    return o;
}
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static const char *byml_str(const Byml *b, uint32_t table, uint32_t i) {
    if (!table || table + 8 > b->n || i >= (rd32(b->d + table) >> 8)) return NULL;
    uint32_t o = table + rd32(b->d + table + 4 + 4 * i);
    return o < b->n ? (const char *)b->d + o : NULL;
}
static uint32_t byml_count(const Byml *b, uint32_t node) { return node + 4 <= b->n ? rd32(b->d + node) >> 8 : 0; }
// Entry `key` of the dictionary at `node`: its type and value; false if it has none
static bool byml_get(const Byml *b, uint32_t node, const char *key, uint8_t *type, uint32_t *value) {
    if (node + 4 > b->n || b->d[node] != 0xc1) return false;
    uint32_t n = byml_count(b, node);
    for (uint32_t i = 0; i < n && node + 12 + 8 * i <= b->n; i++) {
        const uint8_t *e = b->d + node + 4 + 8 * i;
        const char *k = byml_str(b, b->keys, rd32(e) & 0xffffff);
        if (k && !strcmp(k, key)) { *type = e[3], *value = rd32(e + 4); return true; }
    }
    return false;
}
static const char *byml_get_str(const Byml *b, uint32_t node, const char *key) {
    uint8_t t; uint32_t v;
    return byml_get(b, node, key, &t, &v) && t == 0xa0 ? byml_str(b, b->strs, v) : NULL;
}
static int byml_get_int(const Byml *b, uint32_t node, const char *key, int def) {
    uint8_t t; uint32_t v;
    return byml_get(b, node, key, &t, &v) && (t == 0xd1 || t == 0xd3) ? (int)v : def;
}

// ---- MSBT: the label's text, UTF-16 to UTF-8, control tags left out
static void utf8_put(char **o, char *end, uint32_t c) {
    char *p = *o;
    if (c < 0x80) { if (p + 1 < end) *p++ = (char)c; }
    else if (c < 0x800) { if (p + 2 < end) *p++ = (char)(0xc0 | c >> 6), *p++ = (char)(0x80 | (c & 63)); }
    else if (p + 3 < end) *p++ = (char)(0xe0 | c >> 12), *p++ = (char)(0x80 | ((c >> 6) & 63)), *p++ = (char)(0x80 | (c & 63));
    *o = p;
}

typedef struct { char *label, *text; } Msg;
static Msg *g_msgs;
static int g_nmsgs, g_capmsgs;

static void msbt_read(const uint8_t *d, size_t n) {
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
            if (len > 5 && !memcmp(d + p + 1 + len - 5, "_Name", 5) && idx < ntxt) {
                size_t t0 = txt + rd32(d + txt + 4 + 4 * idx);
                size_t t1 = idx + 1 < ntxt ? txt + rd32(d + txt + 8 + 4 * idx) : txt + txt_size;
                char buf[160], *o = buf;
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
                while (o > buf && (o[-1] == ' ' || (o[-1] == 'x' && o - 1 > buf && o[-2] == ' '))) o--;  // ("... x ": a count's place)
                *o = 0;
                if (buf[0]) {
                    if (g_nmsgs == g_capmsgs) g_capmsgs = g_capmsgs ? g_capmsgs * 2 : 1024, g_msgs = realloc(g_msgs, sizeof *g_msgs * g_capmsgs);
                    g_msgs[g_nmsgs].label = copy((const char *)d + p + 1, len - 5);
                    g_msgs[g_nmsgs++].text = copy(buf, strlen(buf));
                }
            }
            p += 1 + len + 4;
        }
    }
}
static int msg_cmp(const void *a, const void *b) { return strcmp(((const Msg *)a)->label, ((const Msg *)b)->label); }
static const char *msg_name(const char *actor) {
    Msg k = {(char *)actor, NULL};
    Msg *m = bsearch(&k, g_msgs, g_nmsgs, sizeof *g_msgs, msg_cmp);
    return m ? m->text : NULL;
}

static void names_read(void) {
    const char *lang = language_text_folder();
    char path[64], inner[64];
    snprintf(path, sizeof path, "Pack/Bootup_%s.pack", lang);
    snprintf(inner, sizeof inner, "Message/Msg_%s.product.ssarc", lang);
    size_t pn = 0, mn = 0;
    uint8_t *pack = fs_game_file(path, &pn), *msg = NULL;
    uint32_t doff, s, e;
    if (pack && sarc_find(pack, pn, inner, &doff, &s, &e) >= 0) msg = yaz0_unpack(pack + doff + s, e - s, &mn);
    int nodes = msg ? sarc_count(msg, mn) : -1;
    for (int i = 0; i < nodes; i++) {
        const char *name; const uint8_t *data; size_t size;
        if (sarc_node(msg, mn, i, &name, &data, &size) && name && !strncmp(name, "ActorType/", 10)) msbt_read(data, size);
    }
    if (g_nmsgs) qsort(g_msgs, g_nmsgs, sizeof *g_msgs, msg_cmp);
    LOG("[items] %d names from %s\n", g_nmsgs, path);
    free(msg), free(pack);
}

// ---- the list
static GameItem *g_items;
static int g_nitems = -1;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint32_t tag(const char *s) {  // (the tags are CRC-32s of their names)
    uint32_t c = 0xffffffff;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}
static bool has_tag(const Byml *b, uint32_t actor, uint32_t t) {
    uint8_t ty; uint32_t tags;
    if (!byml_get(b, actor, "tags", &ty, &tags) || ty != 0xc1) return false;
    uint32_t n = byml_count(b, tags);
    for (uint32_t i = 0; i < n && tags + 12 + 8 * i <= b->n; i++)
        if (rd32(b->d + tags + 8 + 8 * i) == t) return true;
    return false;
}
static int item_cmp(const void *a, const void *b) {
    const GameItem *x = a, *y = b;
    if (x->cat != y->cat) return x->cat - y->cat;
    int c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->actor, y->actor);
}

// An effect type ("ResistCold", "ResistColdAndResistAncient") as up to two ITEMFX_*; one the list lacks is left out
// (WakeWind, the Island Lobster Shirt's second: its description in the game tells of it)
static void effects(const char *type, unsigned char out[2]) {
    static const char *const ids[ITEMFX_COUNT] = {
        NULL, "AttackUp", "DefenseUp", "ClimbSpeed", "BeamPowerUp", "Quietness", "ResistAncient", "ResistBurn", "ResistCold",
        "ResistElectric", "ResistFreeze", "ResistHot", "ResistLightning", "SandMove", "SnowMove", "SwimSpeed",
        "ExGutsMaxUp", "Fireproof", "GutsRecover", "LifeMaxUp", "MovingSpeed",
    };
    int n = 0;
    for (const char *p = type; p && *p && n < 2;) {
        const char *and = strstr(p, "And");
        size_t len = and ? (size_t)(and - p) : strlen(p);
        for (int k = 1; k < ITEMFX_COUNT; k++)
            if (strlen(ids[k]) == len && !strncmp(p, ids[k], len)) out[n++] = (unsigned char)k;
        p = and ? and + 3 : NULL;
    }
}

static void list_read(void) {
    g_nitems = 0;
    size_t zn = 0, n = 0;
    uint8_t *z = fs_game_file("Actor/ActorInfo.product.sbyml", &zn), *d = z ? yaz0_unpack(z, zn, &n) : NULL;
    free(z);
    if (!d || n < 16 || memcmp(d, "YB", 2)) { LOG("[items] no ActorInfo\n"); free(d); return; }
    Byml b = {d, n, rd32(d + 4), rd32(d + 8)};
    uint8_t t; uint32_t actors;
    if (!byml_get(&b, rd32(d + 12), "Actors", &t, &actors) || t != 0xc0) { free(d); return; }
    names_read();
    static const struct { const char *profile; int cat; } profiles[] = {
        {"WeaponSmallSword", ITEMCAT_WEAPON}, {"WeaponLargeSword", ITEMCAT_WEAPON}, {"WeaponSpear", ITEMCAT_WEAPON},
        {"WeaponBow", ITEMCAT_BOW}, {"WeaponShield", ITEMCAT_SHIELD}, {"Bullet", ITEMCAT_ARROW},
        {"ArmorHead", ITEMCAT_ARMOR}, {"ArmorUpper", ITEMCAT_ARMOR}, {"ArmorLower", ITEMCAT_ARMOR}, {"Item", ITEMCAT_MATERIAL},
    };
    uint32_t can_get = tag("CanGetPouch"), important = tag("Important"), cooked = tag("CookResult"), rupee = tag("Rupee"),
             arrow = tag("Arrow");
    uint32_t na = byml_count(&b, actors);
    const uint8_t *types = d + actors + 4;
    uint32_t vals = actors + 4 + ((na + 3) & ~3u);
    g_items = calloc(na ? na : 1, sizeof *g_items);
    for (uint32_t i = 0; i < na && vals + 4 * i + 4 <= n; i++) {
        if (types[i] != 0xc1) continue;
        uint32_t a = rd32(d + vals + 4 * i);
        const char *actor = byml_get_str(&b, a, "name"), *profile = byml_get_str(&b, a, "profile");
        if (!actor || !profile || !has_tag(&b, a, can_get) || has_tag(&b, a, important) || has_tag(&b, a, cooked) ||
            has_tag(&b, a, rupee))
            continue;
        int cat = -1;
        for (size_t p = 0; p < sizeof profiles / sizeof *profiles; p++)
            if (!strcmp(profile, profiles[p].profile)) cat = profiles[p].cat;
        if (cat < 0 || (cat == ITEMCAT_ARROW && !has_tag(&b, a, arrow))) continue;
        if (cat == ITEMCAT_MATERIAL && !strncmp(actor, "Obj_", 4) && strstr(actor, "Arrow")) cat = ITEMCAT_ARROW;  // (bundles: Obj_AncientArrow_C_01)
        const char *name = msg_name(actor);
        if (!name) continue;
        GameItem *it = &g_items[g_nitems++];
        it->actor = copy(actor, strlen(actor)), it->name = name, it->cat = cat;
        it->stack = cat == ITEMCAT_ARROW || cat == ITEMCAT_MATERIAL;
        it->value = cat <= ITEMCAT_SHIELD ? byml_get_int(&b, a, "generalLife", 1) * 100 : 1;
        it->attack = byml_get_int(&b, a, "attackPower", 0);
        it->durability = cat <= ITEMCAT_SHIELD ? byml_get_int(&b, a, "generalLife", 0) : 0;
        it->guard = byml_get_int(&b, a, "weaponCommonGuardPower", 0);
        it->defense = byml_get_int(&b, a, "armorDefenceAddLevel", 0);
        it->hearts = byml_get_int(&b, a, "cureItemHitPointRecover", 0);
        it->price = byml_get_int(&b, a, "itemSellingPrice", 0);
        effects(byml_get_str(&b, a, cat == ITEMCAT_ARMOR ? "armorEffectEffectType" : "cureItemEffectType"), it->effect);
    }
    qsort(g_items, g_nitems, sizeof *g_items, item_cmp);
    int w = 0;  // (one per name and category)
    for (int i = 0; i < g_nitems; i++)
        if (!w || g_items[w - 1].cat != g_items[i].cat || strcmp(g_items[w - 1].name, g_items[i].name)) g_items[w++] = g_items[i];
    g_nitems = w;
    LOG("[items] %d items can be added\n", g_nitems);
    free(d);
}

int items_count(void) { pthread_once(&g_once, list_read); return g_nitems; }
const GameItem *items_at(int i) { return i >= 0 && i < items_count() ? &g_items[i] : NULL; }

// ---- the item's picture in the pouch: UI/StockItem/<actor>.sbitemico, a Yaz0 BFRES whose BNTX holds one RGBA8
// texture (BRTI: format 0x0bXX at +0x1c, width and height at +0x24, GOBs per block (log2) at +0x34), block linear,
// its data 16 bytes into the BRTD section
static const uint8_t *find4(const uint8_t *p, const uint8_t *end, const char *magic) {
    for (; p + 4 <= end; p++)
        if (!memcmp(p, magic, 4)) return p;
    return NULL;
}
unsigned char *items_icon(int i, int *w, int *h) {
    const GameItem *it = items_at(i);
    if (!it) return NULL;
    char path[128];
    snprintf(path, sizeof path, "UI/StockItem/%s.sbitemico", it->actor);
    size_t zn = 0, n = 0;
    uint8_t *z = fs_game_file(path, &zn), *d = z ? yaz0_unpack(z, zn, &n) : NULL, *out = NULL;
    free(z);
    const uint8_t *end = d ? d + n : NULL, *k = d ? find4(d, end, "BNTX") : NULL;
    const uint8_t *r = k ? find4(k, end, "BRTI") : NULL, *t = k ? find4(k, end, "BRTD") : NULL;
    if (r && t && r + 0x38 <= end) {
        uint32_t fmt = rd32(r + 0x1c), iw = rd32(r + 0x24), ih = rd32(r + 0x28), bh = rd32(r + 0x34);
        uint64_t gob_rows = 8ull << (bh < 6 ? bh : 0), rows = (ih + gob_rows - 1) / gob_rows * gob_rows;
        uint64_t size = ((uint64_t)iw * 4 + 63) / 64 * 64 * rows;
        if ((fmt >> 8) == 0x0b && iw && ih && iw <= 512 && ih <= 512 && bh < 6 && (uint64_t)(end - t) >= 16 + size &&
            (out = malloc((size_t)iw * ih * 4))) {
            tegra_deswizzle(t + 16, out, iw, ih, 1, 4, bh, 0);
            *w = (int)iw, *h = (int)ih;
        }
    }
    free(d);
    return out;
}

bool items_available(void) {
    const GameLibTables *t = gamelib_tables();
    if (!t) return false;
    for (uint32_t k = 0; k < *t->nsites; k++)
        if (!strcmp(t->sites[k].name, "item_tick")) return true;
    return false;
}

// ---- requests, carried out on the game thread
typedef struct { char actor[64]; int item, value, times, asked; unsigned seq; bool stack; } Request;
static Request g_req[32];
static int g_nreq;
static atomic_int g_pending;
static unsigned g_seq;
static ItemResult g_result;
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;

RT_ON_RESET(items_reset) {  // (what was asked for in the last boot isn't for this one)
    pthread_mutex_lock(&g_req_lock);
    g_nreq = 0;
    atomic_store(&g_pending, 0);
    g_result = (ItemResult){g_seq, -1, 0, 0};
    pthread_mutex_unlock(&g_req_lock);
}

unsigned items_requested(void) {
    pthread_mutex_lock(&g_req_lock);
    unsigned n = g_seq;
    pthread_mutex_unlock(&g_req_lock);
    return n;
}

ItemResult items_last_result(void) {
    pthread_mutex_lock(&g_req_lock);
    ItemResult r = g_result;
    pthread_mutex_unlock(&g_req_lock);
    return r;
}

// How many entries the pouch has: the pause menu data's list of them at +0x78, its free entries at +0x90, 420 in all
// (seen comparing the object around item gets, 1.9.0). The item get adds nothing when the player has no free slot of
// that kind, and says nothing of it. -1: not those fields, this game version has them elsewhere.
static int pouch_entries(uint64_t pmdm) {
    uint32_t used = *(volatile uint32_t *)(uintptr_t)(pmdm + 0x78), free_ = *(volatile uint32_t *)(uintptr_t)(pmdm + 0x90);
    return used + free_ == 420 ? (int)used : -1;
}

bool items_request(int i, int count) {
    const GameItem *it = items_at(i);
    if (!it || count < 1) return false;
    pthread_mutex_lock(&g_req_lock);
    bool ok = g_nreq < (int)(sizeof g_req / sizeof *g_req);
    if (ok) {
        Request *r = &g_req[g_nreq++];
        snprintf(r->actor, sizeof r->actor, "%s", it->actor);
        r->item = i, r->seq = ++g_seq;
        r->value = it->stack ? (count > 999 ? 999 : count) : it->value;
        r->times = it->stack ? 1 : (count > ITEMS_MAX_EACH ? ITEMS_MAX_EACH : count);
        r->asked = it->stack ? r->value : r->times;
        r->stack = it->stack;
        atomic_store(&g_pending, g_nreq);
    }
    pthread_mutex_unlock(&g_req_lock);
    return ok;
}

// The game thread, every frame in play (tools/patches.py _item_tick: at a function's entry, so only its arguments are
// live; the whole guest CPU state is put back afterwards). get: the item get; mgr: where the pause menu data's address
// is; sstr_vt: sead::SafeString's vtable. The name and its SafeString go on the guest stack below sp.
void rt_item_tick(Cpu *c, uint64_t get, uint64_t mgr, uint64_t sstr_vt) {
    if (!atomic_load_explicit(&g_pending, memory_order_relaxed)) return;
    Request req[32];
    pthread_mutex_lock(&g_req_lock);
    int n = g_nreq;
    memcpy(req, g_req, sizeof *req * n);
    g_nreq = 0;
    atomic_store(&g_pending, 0);
    pthread_mutex_unlock(&g_req_lock);
    uint64_t pmdm = *(volatile uint64_t *)(uintptr_t)mgr;
    if (!pmdm) return;
    Cpu *keep = malloc(sizeof *keep);
    if (!keep) return;
    memcpy(keep, c, sizeof *c);
    uint64_t buf = (c->sp - 0x100) & ~15ull;
    for (int r = 0; r < n; r++) {
        memcpy((void *)(uintptr_t)(buf + 0x20), req[r].actor, sizeof req[r].actor);
        *(uint64_t *)(uintptr_t)buf = sstr_vt;
        *(uint64_t *)(uintptr_t)(buf + 8) = buf + 0x20;
        bool stack = req[r].stack;
        int added = stack ? req[r].asked : 0;  // (a stack: all of it at once)
        for (int k = 0; k < req[r].times; k++) {
            int before = pouch_entries(pmdm);
            c->sp = buf;
            c->x[0] = pmdm, c->x[1] = buf, c->x[2] = (uint32_t)req[r].value, c->x[3] = 0;
            guest_call(c, get);
            // (a stack may only grow an entry it has: there no new entry says nothing)
            if (!stack && before >= 0 && pouch_entries(pmdm) == before) break;
            if (!stack) added++;
        }
        LOG("[items] %s: value %d, %d of %d\n", req[r].actor, req[r].value, added, req[r].asked);
        pthread_mutex_lock(&g_req_lock);
        g_result = (ItemResult){req[r].seq, req[r].item, req[r].asked, added};
        pthread_mutex_unlock(&g_req_lock);
    }
    memcpy(c, keep, sizeof *c);
    free(keep);
}

// ---- give <actor> [count]
static bool has_text(const char *s, const char *t) {  // (ignoring case)
    size_t n = strlen(t);
    for (; *s; s++) {
        size_t k = 0;
        while (k < n && tolower((unsigned char)s[k]) == tolower((unsigned char)t[k])) k++;
        if (k == n) return true;
    }
    return false;
}

static void cmd_give(int argc, char **argv, ControlReply *r) {
    if (argc < 2 || argc > 3) { control_fail(r, "Usage: give <actor or part of a name> [count]"); return; }
    int count = argc == 3 ? atoi(argv[2]) : 1, i = 0, n = items_count();
    while (i < n && strcmp(g_items[i].actor, argv[1])) i++;
    if (i == n) {  // not an actor: the items whose name or actor has this in it. One is given, several are listed
        int found = 0, first = n;
        for (int k = 0; k < n; k++) {
            if (!has_text(g_items[k].name, argv[1]) && !has_text(g_items[k].actor, argv[1])) continue;
            if (!found++) first = k;
        }
        if (!found) { control_fail(r, "No item has this in its name or actor"); return; }
        if (found > 1) {
            // (the fields for scripts, the lines for the console, which prints them instead)
            char list[2048] = "";
            size_t at = 0;
            int shown = 0;
            for (int k = first; k < n && shown < 30; k++) {
                if (!has_text(g_items[k].name, argv[1]) && !has_text(g_items[k].actor, argv[1])) continue;
                control_text(r, "%s  %s", g_items[k].actor, g_items[k].name);
                if (at < sizeof list) at += (size_t)snprintf(list + at, sizeof list - at, "%s%s", shown ? " " : "", g_items[k].actor);
                shown++;
            }
            control_text(r, "%d items match: give one by its actor", found);
            control_field_int(r, "matches", found);
            control_field_str(r, "actors", list);
            return;
        }
        i = first;
    }
    if (!items_available()) { control_fail(r, "The game code has no item get"); return; }
    if (!items_request(i, count)) { control_fail(r, "Too many requests waiting"); return; }
    control_field_str(r, "name", g_items[i].name);
    control_field_int(r, "count", count);
    control_field_str(r, "applied", "next frame in play");
}

void control_items_init(void) {
    static const ControlCmd cmds[] = {
        {"give", "<actor or part of a name> [count]", "Put an item in the pouch, in play (several matches: lists them)", cmd_give},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
