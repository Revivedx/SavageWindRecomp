// The game's items, for the give command and the overlay's Cheats tab: see runtime/items.h.
//
// The list: every actor of Actor/ActorInfo.product.sbyml (BYML) tagged CanGetPouch, by profile (weapons, bows,
// shields, arrows, armour, materials), leaving out the ones tagged Important (the Sheikah Slate, the Master Sword,
// quest items, Korok seeds), cooked dishes (CookResult: they need their effects) and rupees. Each has the name the
// game gives it, <actor>_Name in Message/Msg_<lang>.product.ssarc's ActorType/*.msbt inside Pack/Bootup_<lang>.pack;
// one without a name is left out, and of several actors with one name (armour's upgrade levels, dyes) the first by
// actor name stays. The name's pinyin comes with it (runtime/pinyin.h): the menu's search is typed on a controller's
// on-screen keyboard, which has no IME and only ASCII keys, so 鲈鱼 is found by "luyu".
//
// Adding: the pause menu data's item get (1.9.0 0x7000d921bc; x0 the pause menu data, x1 the item's name as a
// sead::SafeString, w2 its value, x3 its weapon modifier or NULL) is what picking an item up calls: a stack's value
// is how many, a weapon's its durability x 100 (a new one: generalLife x 100). The game thread calls it from
// rt_item_tick (tools/patches.py _item_tick), at the entry of Link's damage handling, which runs every frame in play,
// as picking an item up does.
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "cpu.h"
#include "game_sites.h"
#include "gamelib.h"
#include "hle.h"
#include "items.h"
#include "kernel.h"
#include "language.h"
#include "nvn/tegra.h"
#include "pinyin.h"
#include "platform.h"
#include "rt.h"
#include "services/archive.h"
#include "services/formats.h"
#include "settings.h"
#include "ui/i18n.h"

uint8_t *fs_game_file(const char *path, size_t *out_n);  // runtime/services/fs.c

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static char *copy(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (o) memcpy(o, s, n), o[n] = 0;
    return o;
}

// ---- the names: each <actor>_Name message's text
typedef struct { char *label, *text; } Msg;
static Msg *g_msgs;
static int g_nmsgs, g_capmsgs;

static void name_add(void *ctx, const char *label, size_t len, const char *text) {
    (void)ctx;
    if (len <= 5 || memcmp(label + len - 5, "_Name", 5)) return;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || (text[n - 1] == 'x' && n > 1 && text[n - 2] == ' '))) n--;  // ("... x ": a count's place)
    if (!n) return;
    if (g_nmsgs == g_capmsgs) g_capmsgs = g_capmsgs ? g_capmsgs * 2 : 1024, g_msgs = realloc(g_msgs, sizeof *g_msgs * g_capmsgs);
    g_msgs[g_nmsgs].label = copy(label, len - 5);
    g_msgs[g_nmsgs++].text = copy(text, n);
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
        if (sarc_node(msg, mn, i, &name, &data, &size) && name && !strncmp(name, "ActorType/", 10))
            msbt_each(data, size, name_add, NULL);
    }
    if (g_nmsgs) qsort(g_msgs, g_nmsgs, sizeof *g_msgs, msg_cmp);
    LOG("[items] %d names from %s\n", g_nmsgs, path);
    free(msg), free(pack);
}

// ---- the list
static GameItem *g_items;
static int g_nitems = -1;
static GameItem *g_all;  // every item, by actor (g_items has one per name: the Divine Helms' upgrades share theirs)
static int g_nall;
static const char **g_stack_actors;
static int g_nstack;
// The list is read once the game's files are there: an ask from before the RomFS is mounted finds no ActorInfo and
// asks again later, rather than leaving the list (Cheats > Add items, give) empty for the whole run
static pthread_mutex_t g_list_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_list_done;

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static uint32_t tag(const char *s) {  // (the tags are CRC-32s of their names)
    uint32_t c = 0xffffffff;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}
static bool has_tag(const Byml *b, uint32_t actor, uint32_t t) {
    uint8_t ty; uint32_t tags, v;
    if (!byml_get(b, actor, "tags", &ty, &tags) || ty != BYML_DICT) return false;
    uint32_t n = byml_count(b, tags);
    for (uint32_t i = 0; i < n; i++)
        if (byml_at(b, tags, i, &ty, &v, NULL) && v == t) return true;
    return false;
}
static int actor_cmp(const void *a, const void *b) {
    return strcmp(((const GameItem *)a)->actor, ((const GameItem *)b)->actor);
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

// false: no ActorInfo yet (the game's files not mounted): try again later
static bool list_read(void) {
    g_nitems = 0;
    size_t zn = 0, n = 0;
    uint8_t *z = fs_game_file("Actor/ActorInfo.product.sbyml", &zn), *d = z ? yaz0_unpack(z, zn, &n) : NULL;
    free(z);
    Byml b;
    if (!byml_open(&b, d, n)) {
        static bool told;
        if (!told) told = true, LOG("[items] no ActorInfo yet: the list is read once the game's files are mounted\n");
        free(d);
        return false;
    }
    uint8_t t; uint32_t actors;
    if (!byml_get(&b, b.root, "Actors", &t, &actors) || t != BYML_ARRAY) { free(d); return true; }
    names_read();
    static const struct { const char *profile; int cat; } profiles[] = {
        {"WeaponSmallSword", ITEMCAT_WEAPON}, {"WeaponLargeSword", ITEMCAT_WEAPON}, {"WeaponSpear", ITEMCAT_WEAPON},
        {"WeaponBow", ITEMCAT_BOW}, {"WeaponShield", ITEMCAT_SHIELD}, {"Bullet", ITEMCAT_ARROW},
        {"ArmorHead", ITEMCAT_ARMOR}, {"ArmorUpper", ITEMCAT_ARMOR}, {"ArmorLower", ITEMCAT_ARMOR}, {"Item", ITEMCAT_MATERIAL},
        // critters and fairies (Animal_Insect_*: bugs, lizards, frogs, fairies) are CapturedActors; CanGetPouch below
        // leaves out what the pouch can't hold
        {"CapturedActor", ITEMCAT_MATERIAL},
    };
    uint32_t can_get = tag("CanGetPouch"), important = tag("Important"), cooked = tag("CookResult"), rupee = tag("Rupee"),
             arrow = tag("Arrow");
    uint32_t na = byml_count(&b, actors);
    g_items = calloc(na ? na : 1, sizeof *g_items);
    for (uint32_t i = 0; i < na; i++) {
        uint32_t a;
        if (!byml_at(&b, actors, i, &t, &a, NULL)) break;
        if (t != BYML_DICT) continue;
        const char *actor = byml_get_str(&b, a, "name"), *profile = byml_get_str(&b, a, "profile");
        if (!actor || !profile || !has_tag(&b, a, can_get) || has_tag(&b, a, important) || has_tag(&b, a, cooked) ||
            has_tag(&b, a, rupee))
            continue;
        // The One-Hit Obliterator belongs to one quest: outside it, Link can't put it away and it drains his hearts (#652)
        if (!strcmp(actor, "Weapon_Sword_502")) continue;
        int cat = -1;
        for (size_t p = 0; p < sizeof profiles / sizeof *profiles; p++)
            if (!strcmp(profile, profiles[p].profile)) cat = profiles[p].cat;
        if (cat < 0 || (cat == ITEMCAT_ARROW && !has_tag(&b, a, arrow))) continue;
        if (cat == ITEMCAT_MATERIAL && !strncmp(actor, "Obj_", 4) && strstr(actor, "Arrow")) cat = ITEMCAT_ARROW;  // (bundles: Obj_AncientArrow_C_01)
        const char *name = msg_name(actor);
        if (!name) continue;
        GameItem *it = &g_items[g_nitems++];
        it->actor = copy(actor, strlen(actor)), it->name = name, it->cat = cat;
        char py[256];  // (the MSBT reader stops a name at 160 bytes: the readings always fit)
        if (pinyin_text(name, py, sizeof py)) it->pinyin = copy(py, strlen(py));
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
    g_stack_actors = calloc((size_t)(g_nitems ? g_nitems : 1), sizeof *g_stack_actors);
    if (g_stack_actors) {
        for (int i = 0; i < g_nitems; i++)
            if (g_items[i].stack) g_stack_actors[g_nstack++] = g_items[i].actor;
        qsort(g_stack_actors, (size_t)g_nstack, sizeof *g_stack_actors, str_cmp);
    }
    if ((g_all = malloc(sizeof *g_all * (size_t)(g_nitems ? g_nitems : 1)))) {
        memcpy(g_all, g_items, sizeof *g_all * (size_t)g_nitems);
        g_nall = g_nitems;
        qsort(g_all, (size_t)g_nall, sizeof *g_all, actor_cmp);
    }
    qsort(g_items, g_nitems, sizeof *g_items, item_cmp);
    int w = 0;  // (one per name and category)
    for (int i = 0; i < g_nitems; i++)
        if (!w || g_items[w - 1].cat != g_items[i].cat || strcmp(g_items[w - 1].name, g_items[i].name)) g_items[w++] = g_items[i];
    g_nitems = w;
    for (int i = 0; i < g_nitems; i++) ui_register_glyphs(g_items[i].name);
    LOG("[items] %d items can be added\n", g_nitems);
    free(d);
    return true;
}

int items_count(void) {
    if (!atomic_load(&g_list_done)) {
        static uint64_t tried_ns;
        pthread_mutex_lock(&g_list_mu);
        uint64_t now = plat_now_ns();
        if (!atomic_load(&g_list_done) && (!tried_ns || now - tried_ns > 1000000000ull)) {  // (at most once a second)
            tried_ns = now;
            if (list_read()) atomic_store(&g_list_done, true);
        }
        pthread_mutex_unlock(&g_list_mu);
        if (!atomic_load(&g_list_done)) return 0;
    }
    return g_nitems;
}
const GameItem *items_at(int i) { return i >= 0 && i < items_count() ? &g_items[i] : NULL; }

const GameItem *items_by_actor(const char *actor) {
    if (items_count() <= 0 || !g_all || !actor) return NULL;
    GameItem key = {.actor = actor};
    return bsearch(&key, g_all, (size_t)g_nall, sizeof *g_all, actor_cmp);
}

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
typedef struct { char actor[64]; int item, value, times, asked; unsigned seq; bool stack; int chest; } Request;
static Request g_req[32];
static int g_nreq;
static atomic_int g_pending;
static unsigned g_seq;
static ItemResult g_result;
static ItemResult g_results[64];  // the latest results, by their request's number (seq % 64: items_result_of)
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;

RT_ON_RESET(items_reset) {  // (what was asked for in the last boot isn't for this one)
    pthread_mutex_lock(&g_req_lock);
    for (int r = 0; r < g_nreq; r++) g_results[g_req[r].seq % 64] = (ItemResult){g_req[r].seq, -1, g_req[r].asked, 0};
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

bool items_result_of(unsigned seq, ItemResult *out) {
    pthread_mutex_lock(&g_req_lock);
    bool done = seq && g_results[seq % 64].seq == seq;
    if (done) *out = g_results[seq % 64];
    pthread_mutex_unlock(&g_req_lock);
    return done;
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

bool items_request(int i, int count) { return items_request_seq(i, count) != 0; }

// chest: 0 into the pouch; else in a chest in front of Link, the chest-th of those asked for at once (side by side)
static unsigned request(const GameItem *it, int i, int count, int chest) {
    if (!it || count < 1) return 0;
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
        r->chest = chest;
        atomic_store(&g_pending, g_nreq);
    }
    unsigned seq = ok ? g_seq : 0;
    pthread_mutex_unlock(&g_req_lock);
    return seq;
}

unsigned items_request_seq(int i, int count) { return request(items_at(i), i, count, 0); }

unsigned items_request_actor(const char *actor, int count) { return request(items_by_actor(actor), 0, count, 0); }

bool items_chests_available(void) {
    return g_site.actor_create && g_site.actor_creator && g_site.actor_create_heap && g_site.inst_params_vt &&
           g_site.inst_params_init && g_site.inst_params_add && g_site.inst_params_add_string && g_site.player_info;
}

unsigned items_request_chest(const char *actor, int place) {
    return items_chests_available() ? request(items_by_actor(actor), 0, 1, place + 1) : 0;
}

static _Thread_local bool g_in_give;

static bool guest_ptr_ok(uint64_t addr, size_t size) {
    if (!addr || addr + size < addr) return false;
    uint64_t top = HEAP_BASE + kernel_heap_size();
    if (addr >= HEAP_BASE && addr + size <= top) return true;
    if (addr >= STACK_BASE && addr + size <= STACK_BASE + STACK_SIZE) return true;
    if (addr >= GUEST_LO && addr + size <= GUEST_HI) return true;
    return false;
}

static bool items_is_stackable(const char *actor) {
    if (!actor || !*actor) return false;
    items_count();
    if (!g_stack_actors || g_nstack <= 0) return false;
    return bsearch(&actor, g_stack_actors, (size_t)g_nstack, sizeof *g_stack_actors, str_cmp) != NULL;
}

uint32_t rt_item_get_scale(uint64_t safe_str, uint32_t value) {
    int mult = g_settings.item_multiplier;
    if ((mult != 2 && mult != 4 && mult != 8) || g_in_give || value == 0 || value >= 1000)
        return value;
    if (!guest_ptr_ok(safe_str, 16)) return value;
    uint64_t cstr = *(const uint64_t *)(uintptr_t)(safe_str + 8);
    if (!guest_ptr_ok(cstr, 1)) return value;
    const char *actor = (const char *)(uintptr_t)cstr;
    if (!items_is_stackable(actor)) return value;
    uint64_t scaled = (uint64_t)value * (uint32_t)mult;
    return scaled > 999 ? 999 : (uint32_t)scaled;
}

// A treasure chest with req's item in it, in front of Link: the amiibo drop's own (iron with gold), or with the
// downloadable content set up, its EX chest (purple and gold). Made by the actor creation the amiibo drop makes its
// chest with (tools/game_sites.py actor_create), its instance parameters
// @P (where), @R (turned, in radians: its front to Link) and DropActor (what is in it). The game thread, from
// rt_item_tick (its CPU state is put back after). Opening it is the game's own: with no room in the pouch the game
// offers to drop something or leaves the item by the chest. false: no Link, or the creator didn't take the request.
#define REWARD_CHEST "TBox_Field_Iron"
#define REWARD_CHEST_DLC "TBox_Field_Iron_NoReaction_Aoc"
static bool spawn_chest(Cpu *c, const Request *req, uint64_t sstr_vt) {
    uint64_t pinfo = *(volatile uint64_t *)(uintptr_t)g_site.player_info;
    if (!pinfo) return false;
    float link[3];
    memcpy(link, (const void *)(uintptr_t)(pinfo + 0x88), sizeof link);
    if (link[0] == 0 && link[1] == 0 && link[2] == 0) return false;
    // ahead of Link along the camera (where the player looks), the chests of one reward side by side
    extern bool control_camera_angles(double *yaw, double *pitch);
    double yaw_deg = 0, pitch = 0;
    control_camera_angles(&yaw_deg, &pitch);
    double yaw = yaw_deg * 3.14159265358979 / 180, fx = sin(yaw), fz = cos(yaw);
    // (the chests made close together: the first ahead, the next two to its sides, then a row further on)
    static uint64_t last_ns;
    static int made;
    uint64_t now_ns = plat_now_ns();
    if (now_ns - last_ns > 10000000000ull) made = 0;
    last_ns = now_ns;
    int k = made++;
    double side = k % 3 == 1 ? 1.8 : k % 3 == 2 ? -1.8 : 0, ahead = 3.0 + 2.2 * (k / 3);
    float pos[3] = {(float)(link[0] + fx * ahead + fz * side), link[1] + 0.2f, (float)(link[2] + fz * ahead - fx * side)};
    float rot[3] = {0, (float)(yaw + 3.14159265358979), 0};

    uint64_t buf = (c->sp - 0x800) & ~15ull;
    uint8_t *m = (uint8_t *)(uintptr_t)buf;
    memset(m, 0, 0x700);
    uint64_t pack = buf, data = buf + 0x400;  // (the parameters' object: 0x10 of its own, then what the setup lays out)
    // sead::SafeStrings (vtable, text) and their texts, after the parameters
    struct { uint64_t ss; const char *text; } strs[] = {{0, "@P"}, {0, "@R"}, {0, "DropActor"}, {0, req->actor}};
    uint64_t at = data;
    for (size_t k = 0; k < sizeof strs / sizeof *strs; k++) {
        size_t n = strlen(strs[k].text) + 1;
        uint64_t text = at + 16;
        memcpy((void *)(uintptr_t)text, strs[k].text, n);
        *(uint64_t *)(uintptr_t)at = sstr_vt, *(uint64_t *)(uintptr_t)(at + 8) = text;
        strs[k].ss = at;
        at = (text + n + 15) & ~15ull;
    }
    uint64_t vpos = at, vrot = at + 16, name = at + 32;
    memcpy((void *)(uintptr_t)vpos, pos, sizeof pos);
    memcpy((void *)(uintptr_t)vrot, rot, sizeof rot);
    const char *chest = getenv("SAVAGEWIND_REWARD_CHEST");  // (testing: another chest actor)
    if (!chest || !*chest || strlen(chest) > 90) chest = fs_aoc_present(1) ? REWARD_CHEST_DLC : REWARD_CHEST;
    memcpy((void *)(uintptr_t)name, chest, strlen(chest) + 1);

    *(uint64_t *)(uintptr_t)pack = g_site.inst_params_vt + 0x10;  // (its vtable: the class's address, 0x10 in)
    c->sp = buf;
    c->x[0] = pack + 0x10;
    guest_call(c, g_site.inst_params_init);
    c->sp = buf, c->x[0] = pack + 0x10, c->x[1] = vpos, c->x[2] = strs[0].ss, c->x[3] = 12, c->x[4] = 4;
    guest_call(c, g_site.inst_params_add);
    c->sp = buf, c->x[0] = pack + 0x10, c->x[1] = vrot, c->x[2] = strs[1].ss, c->x[3] = 12, c->x[4] = 4;
    guest_call(c, g_site.inst_params_add);
    c->sp = buf, c->x[0] = pack + 0x10, c->x[1] = strs[3].ss, c->x[2] = strs[2].ss;
    guest_call(c, g_site.inst_params_add_string);
    uint64_t creator = *(volatile uint64_t *)(uintptr_t)g_site.actor_creator;
    uint64_t holder = *(volatile uint64_t *)(uintptr_t)g_site.actor_create_heap;
    if (!creator || !holder) return false;
    c->sp = buf;
    c->x[0] = creator, c->x[1] = name, c->x[2] = *(volatile uint64_t *)(uintptr_t)(holder + 0x28);
    c->x[3] = 0, c->x[4] = pack, c->x[5] = 0, c->x[6] = 1;
    guest_call(c, g_site.actor_create);
    bool ok = c->x[0] & 1;
    LOG("[items] %s in a chest at %.1f %.1f %.1f: %s\n", req->actor, pos[0], pos[1], pos[2], ok ? "made" : "refused");
    return ok;
}

// The game thread, every frame in play (tools/patches.py _item_tick: at a function's entry, so only its arguments are
// live; the whole guest CPU state is put back afterwards). get: the item get; mgr: where the pause menu data's address
// is; sstr_vt: sead::SafeString's vtable. The name and its SafeString go on the guest stack below sp.
void rt_item_tick(Cpu *c, uint64_t get, uint64_t mgr, uint64_t sstr_vt) {
    { extern void rt_hearts_tick(Cpu *c); rt_hearts_tick(c); }
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
        if (req[r].chest) {  // (a chest in front of Link instead of the pouch)
            bool made = spawn_chest(c, &req[r], sstr_vt);
            pthread_mutex_lock(&g_req_lock);
            g_result = g_results[req[r].seq % 64] = (ItemResult){req[r].seq, req[r].item, req[r].asked, made ? 1 : 0};
            pthread_mutex_unlock(&g_req_lock);
            continue;
        }
        memcpy((void *)(uintptr_t)(buf + 0x20), req[r].actor, sizeof req[r].actor);
        *(uint64_t *)(uintptr_t)buf = sstr_vt;
        *(uint64_t *)(uintptr_t)(buf + 8) = buf + 0x20;
        bool stack = req[r].stack;
        int added = stack ? req[r].asked : 0;  // (a stack: all of it at once)
        for (int k = 0; k < req[r].times; k++) {
            int before = pouch_entries(pmdm);
            c->sp = buf;
            c->x[0] = pmdm, c->x[1] = buf, c->x[2] = (uint32_t)req[r].value, c->x[3] = 0;
            g_in_give = true;
            guest_call(c, get);
            g_in_give = false;
            // (a stack may only grow an entry it has: there no new entry says nothing)
            if (!stack && before >= 0 && pouch_entries(pmdm) == before) break;
            if (!stack) added++;
        }
        LOG("[items] %s: value %d, %d of %d\n", req[r].actor, req[r].value, added, req[r].asked);
        pthread_mutex_lock(&g_req_lock);
        g_result = g_results[req[r].seq % 64] = (ItemResult){req[r].seq, req[r].item, req[r].asked, added};
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

bool items_matches(int i, const char *text) {
    const GameItem *it = items_at(i);
    return it && (has_text(it->name, text) || has_text(it->actor, text) || (it->pinyin && pinyin_match(it->pinyin, text)));
}

static void cmd_give(int argc, char **argv, ControlReply *r) {
    if (argc < 2 || argc > 3) { control_fail(r, "Usage: give <actor, part of a name or its pinyin> [count]"); return; }
    int count = argc == 3 ? atoi(argv[2]) : 1, i = 0, n = items_count();
    while (i < n && strcmp(g_items[i].actor, argv[1])) i++;
    if (i == n) {  // not an actor: the items whose name, actor or pinyin has this in it. One is given, several are listed
        int found = 0, first = n;
        for (int k = 0; k < n; k++) {
            if (!items_matches(k, argv[1])) continue;
            if (!found++) first = k;
        }
        if (!found) { control_fail(r, "No item has this in its name, actor or pinyin"); return; }
        if (found > 1) {
            // (the fields for scripts, the lines for the console, which prints them instead)
            char list[2048] = "";
            size_t at = 0;
            int shown = 0;
            for (int k = first; k < n && shown < 30; k++) {
                if (!items_matches(k, argv[1])) continue;
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
        {"give", "<actor, part of a name or its pinyin> [count]", "Put an item in the pouch, in play (several matches: lists them)", cmd_give},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
