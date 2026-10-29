// Online play: the other players as actors in the world (runtime/online.h). Once per frame, on the game's frame loop
// (tools/patches.py _vfr_ratio, ksys::VFR::updateInterval on the main thread, before the actors' update):
//   - the local Link's pose is read for the others (the AS his Link plays, where his gear is, his heading);
//   - each remote player gets a Link of his own (OnlinePlayer, tools/online_mod.py: created the way the game creates
//     the Remote Bomb rune's bomb) where he is, playing the AS his Link plays; wearing his armour, the game's own
//     pieces worn on that Link's skeleton as the player's are; carrying his gear (weapon, shield, bow, sheath,
//     quiver: model-only actors, OnlineGear_*) on the bones it is on in his game.
//
// 1.9.0 (no symbols; read from the code):
//   0x7001535900  create an actor now, by name: x0 the creator (**0x7001d66550), x1 the actor's name, x2 a heap
//                 (*(**0x7001d66558 + 0x28)), x3 0, w4, w5 0 -> the actor or 0 (its resources aren't loaded). The
//                 Remote Bomb rune (0x7000b3bb40) calls it with "RemoteBomb" / "RemoteBombCube" and then
//   0x700152261c  set an actor's matrix (x0 actor, x1 Matrix34f *, x2 scale or 0): the 3x4 at actor +0x398
//   0x7001514bac  delete an actor (x0 actor, w1 0), as 0x7001535900 does with one that failed its check
//
// Off unless online_mod is the RomFS overlay (or SAVAGEWIND_ONLINE_ACTOR=<name> picks an actor: docs/ONLINE.md, what
// was tried). Without it the other players are only the overlay's figures (runtime/ui/online_ui.mm).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game_sites.h"
#include "online.h"
#include "rt.h"

#define FN_CREATE 0x7001535900ull
#define FN_SET_MTX 0x700152261cull
#define FN_DELETE 0x7001514bacull
#define FN_AS_IN_SLOT 0x70014a0040ull  // x0 an AS manager, w1 slot, w2 bank -> the AS playing there (its name at +8;
                                       // the manager: slots at +0xc0, 0x50 each (+0x20 its banks), +0xb8 how many)
#define FN_PLAY_AS 0x700151e42cull  // an AI action's "play this AS" (x0 the action: its actor at +8; x1 the AS name, w2
                                    // not again if already playing, w3 the slot, w4 the bank, s0 the blend, -1 its
                                    // own); the AS manager is the actor's +0x560 (ForkASPlay's enter, 0x7000759e74)
#define FN_HIDE_GROUPS 0x700126ba4cull  // x1 a gsys model, x2 another (0: all), w3 visible: shows or hides x1's materials
                                       // in a group of x2's (the bfres' HideMaterialGroup: a material's groups, and
                                       // a model's, those it covers); the player's armour code (0x7001175324) calls it
                                       // for each piece's model (actor +0x4e0) with each other piece's
#define P_HIDE_GROUPS_X0 0x7001d66c98ull  // (its x0, unused: **this)
#define FN_FIND_BONE 0x7000f77260ull  // x0 a gsys model, x1 a SafeString -> the unit (low 16 bits, -1: none) and the
                                      // bone (high 16); unit->vtable[0x70](unit, Matrix34f *out, bone): its matrix in
                                      // the world (0x7000741454 reads "Weapon_L" so)
#define P_SAFE_STRING_VT 0x7001d66260ull  // (sead::SafeString's vtable, +0x10)
#define FN_REQUEST 0x7001535b10ull  // request an actor (its resources loaded first, on the game's own threads): x0 the
                                    // creator, x1 the name, x2 a heap, x3 a BaseProcHandle (unit, flag), x4 its params
                                    // or 0, x5 0, w6 1 -> true when asked; the player's armour code (0x70011750c8) then
#define FN_HANDLE_TAKE 0x7001516b74ull     // x0 the handle: its actor once its unit's state is 2 (ready), and wakes it
#define FN_HANDLE_RELEASE 0x70015168a8ull  // (a unit that failed, 3 or 5)
#define FN_ARMOR_BIND 0x700117363cull  // x0 an armour actor, x1 its wearer: the armour on his model's skeleton (+0x840)
#define FN_ARMOR_INIT 0x7001173550ull  // x0 the armour, once bound
#define P_CREATOR 0x7001d66550ull
#define P_HEAP_OWNER 0x7001d66558ull

bool framepace_title_ui(void);
bool framepace_paused(void);

#define ARMOR 3  // head, upper body, lower body
#define VALID(p) ((p) >= 0x7000000000ull && (p) < 0x7400000000ull)

// a piece made by FN_REQUEST: asked for, then made once the game has loaded its resources
// An actor watched as the game's BaseProcLink does (0x7101517e60 sets one, 0x7101517e0c checks it): the actor's
// unit (+0x68) and id (+0x60); the unit holds the id (+0x40) and the actor (+0x48) while the actor lives. The game
// deletes actors of ours on its own (a real armour piece not the player's, when his equipment changes; the world's
// actors at a load): one that went is made again
typedef struct { uint64_t unit; uint32_t id; } Watch;

static Watch watch(uint64_t actor) {
    Watch w = {*(uint64_t *)(uintptr_t)(actor + 0x68), *(uint32_t *)(uintptr_t)(actor + 0x60)};
    return w;
}

static bool alive(uint64_t actor, Watch w) {
    return actor && VALID(w.unit) && *(uint32_t *)(uintptr_t)(w.unit + 0x40) == w.id &&
           *(uint64_t *)(uintptr_t)(w.unit + 0x48) == actor && *(uint8_t *)(uintptr_t)(actor + 0x64) != 3;
}

// A request's BaseProcHandle (the unit, a flag): the unit keeps the handle's address (+8) until the actor is taken
// or the handle released, so each request has one of its own, never moved, not used again for a long while (a ring)
typedef struct { uint64_t unit, flag; } Handle;
#define HANDLES 4096
static Handle g_handles[HANDLES];
static int g_next_handle;
static Handle *new_handle(void) {
    Handle *h = &g_handles[g_next_handle++ % HANDLES];
    memset(h, 0, sizeof *h);
    return h;
}

typedef struct {
    char name[48];  // the actor it is ("" none)
    uint64_t actor;
    Watch w;
    Handle *handle;  // (its request's, while pending)
    bool pending;
    int fails, wait;  // (wait: frames before asking again, after a failure)
} Piece;

// A remote player in the world: his Link, his armour and his gear
static struct {
    uint64_t link;  // OnlinePlayer; 0: none
    Watch link_w;
    int link_fails;
    Piece armor[ARMOR], gear[ONLINE_GEAR], arrow;  // (arrow: on his bow's string while he draws it)
    char name[ONLINE_NAME_MAX];
    int anim;                                     // the one-clip AS played (no pose: ONLINE_A_*; -1 none yet)
    char as[ONLINE_AS_CHANNELS][ONLINE_AS_MAX];   // the player's AS played on each channel
    int covered;                                  // the pieces whose cover is applied (a bit each; -1: none yet)
    int frames;                                   // frames since his Link was made
    bool skeleton;                                // his skeleton has come: his Link is posed by it, plays no AS
} g_act[ONLINE_MAX];

// tools/online_mod.py ANIMS: the AS names of OnlinePlayer's one-clip animations, in ONLINE_A_* order
static const char *const k_anims[ONLINE_A_COUNT] = {
    "OnlineWait", "OnlineWalk", "OnlineRun", "OnlineDash", "OnlineJump", "OnlineFall", "OnlineGlide",
    "OnlineSwimWait", "OnlineSwim", "OnlineClimbWait", "OnlineClimb", "OnlineSquatWait", "OnlineSquat"};
// OnlinePose's channels: (slot, bank)
static const int k_channel[ONLINE_AS_CHANNELS][2] = {{0, 0}, {1, 0}, {1, 1}, {2, 0}};
// the bones gear is on (OnlinePose.gear_bone - 1): hands, the back's mounts (Pod_*), ...
static const char *const k_bones[] = {"Weapon_R", "Weapon_L", "Pod_A", "Pod_B", "Pod_C", "Spine_2", "Waist", "Skl_Root"};
#define BONES (int)(sizeof k_bones / sizeof *k_bones)

static float g_last_link[3];
volatile bool g_online_actor[ONLINE_MAX];  // (online.h online_has_actor: the overlay draws no figure for these)

bool online_has_actor(int slot) { return slot >= 0 && slot < ONLINE_MAX && g_online_actor[slot]; }
static int g_settle;  // frames to wait after a load before creating anything

// SAVAGEWIND_ONLINE_ACTOR, else OnlinePlayer when the overlay is online play's (tools/online_mod.py)
static const char *actor_name(void) {
    const char *n = getenv("SAVAGEWIND_ONLINE_ACTOR");
    if (n && *n) return n;
    static int mod = -1;
    if (mod < 0) {
        const char *ov = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char p[1100];
        snprintf(p, sizeof p, "%s/online_mod.txt", ov ? ov : "");
        FILE *f = ov && *ov ? fopen(p, "rb") : NULL;
        mod = f != NULL;
        if (f) fclose(f);
    }
    return mod ? "OnlinePlayer" : NULL;
}

static bool is_online_player(void) { return actor_name() && !strcmp(actor_name(), "OnlinePlayer"); }

static bool online_actors_on(void) {
    static int on = -1;
    if (on < 0) on = actor_name() != NULL;
    return on;
}

// a call into the game with the frame loop's registers kept as they were
static uint64_t call_f(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                       uint64_t a6, float s0) {
    static Cpu saved;  // (the main thread only)
    memcpy(&saved, c, sizeof saved);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5, c->x[6] = a6;
    union { float f; uint32_t u; } v = {s0};
    c->v[0] = v.u;
    c->sp = (c->sp - 0x400) & ~0xFull;  // (room below the frame loop's own stack frame)
    guest_call(c, fn);
    uint64_t r = c->x[0];
    memcpy(c, &saved, sizeof saved);
    return r;
}

static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return call_f(c, fn, a0, a1, a2, a3, a4, a5, 0, 0);
}

// The AS an actor plays in a slot and bank ("" for none)
static const char *playing(Cpu *c, uint64_t actor, int slot, int bank) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)(actor + 0x560);
    uint64_t as = mgr ? call(c, FN_AS_IN_SLOT, mgr, slot, bank, 0, 0, 0) : 0;
    const char *now = as ? *(const char **)(uintptr_t)(as + 8) : NULL;
    return now ? now : "";
}

// play the AS `name` on actor in a slot and bank (blend: frames, -1 the AS's own; again: even if it plays it already,
// from its start); true when it plays
static bool play_as(Cpu *c, uint64_t actor, const char *name, int slot, int bank, float blend, bool again) {
    uint64_t fake_action[2] = {0, actor};
    call_f(c, FN_PLAY_AS, (uint64_t)(uintptr_t)fake_action, (uint64_t)(uintptr_t)name, again ? 0 : 1, slot, bank, 0, 0,
           blend);
    const char *now = playing(c, actor, slot, bank);
    if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG")) LOG("[online] AS %s on %llx %d.%d: now %s\n", name, (unsigned long long)actor, slot, bank, now);
    return !strcmp(now, name);
}

#define POOL 24  // (each player's pool of pieces: below)
static Piece g_pool[ONLINE_MAX][POOL];

static void forget_all(void) {
    memset(g_act, 0, sizeof g_act);
    memset(g_pool, 0, sizeof g_pool);
    for (int i = 0; i < ONLINE_MAX; i++) g_act[i].anim = -1, g_act[i].covered = -1;
}

// Each piece is made once for a player and kept for the session: a second actor of a piece made for him (after the
// first was deleted, or made and never taken) is never drawn, whatever was tried (made at once or asked for, with or
// without init params, worn or not when deleted). A piece he takes off goes to his pool: unworn, under the world; one
// still being made goes there as it is (0x70015168a8 doesn't stop a request under way). Put on again, it comes back
// from there. The pool goes when he leaves, or at a load (the game removes the world's actors)
static void delete_piece(Cpu *c, Piece *p) {
    if (alive(p->actor, p->w)) {
        if (!strncmp(p->name, "Armor_", 6)) call(c, FN_ARMOR_BIND, p->actor, 0, 0, 0, 0, 0);
        call(c, FN_DELETE, p->actor, 0, 0, 0, 0, 0);
    }
    if (p->pending) call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)p->handle, 0, 0, 0, 0, 0);
    memset(p, 0, sizeof *p);
}

// out of sight: unworn, under the world
// every material of an actor's model shown or hidden (an armour piece taken off: unworn, it still goes with the
// skeleton it was on). The model's units: +0x38 how many, +0x40 the array (each **unit); unit->vtable[0xb8](unit)
// its material count, unit->vtable[0x1b0](unit, material, visible) (as 0x700126ba4c goes through them)
static void model_visible(Cpu *c, uint64_t actor, bool visible) {
    uint64_t model = *(uint64_t *)(uintptr_t)(actor + 0x4e0);
    if (!VALID(model)) return;
    int n = *(int *)(uintptr_t)(model + 0x38);
    uint64_t units = *(uint64_t *)(uintptr_t)(model + 0x40);
    for (int u = 0; VALID(units) && u < n && u < 16; u++) {
        uint64_t unit = *(uint64_t *)(uintptr_t)(units + 8 * u);
        unit = VALID(unit) ? *(uint64_t *)(uintptr_t)unit : 0;
        if (!VALID(unit)) continue;
        uint64_t vt = *(uint64_t *)(uintptr_t)unit;
        int mats = (int)call(c, *(uint64_t *)(uintptr_t)(vt + 0xb8), unit, 0, 0, 0, 0, 0);
        for (int m = 0; m < mats && m < 256; m++) call(c, *(uint64_t *)(uintptr_t)(vt + 0x1b0), unit, m, visible, 0, 0, 0);
    }
}

static void hide_piece(Cpu *c, const Piece *p, const float *at) {
    if (!strncmp(p->name, "Armor_", 6)) call(c, FN_ARMOR_BIND, p->actor, 0, 0, 0, 0, 0), model_visible(c, p->actor, false);
    float m[12] = {1, 0, 0, at[0], 0, 1, 0, at[1] - 5000, 0, 0, 1, at[2]};
    call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
}

static float g_pool_at[3];  // (where the pool's pieces are hidden: under the local Link)

static void pool_piece(Cpu *c, int i, Piece *p) {
    if (!p->pending && !alive(p->actor, p->w)) { memset(p, 0, sizeof *p); return; }
    if (p->actor) hide_piece(c, p, g_pool_at);
    Piece *slot = NULL;
    for (int k = 0; k < POOL && !slot; k++)
        if (!g_pool[i][k].actor && !g_pool[i][k].pending) slot = &g_pool[i][k];
    if (!slot) slot = &g_pool[i][0], delete_piece(c, slot);  // (full: one goes; it won't be drawn again)
    *slot = *p;
    memset(p, 0, sizeof *p);
}

// a piece of `want` from his pool into p: true when it is there (made; armour to be worn again)
static bool from_pool(Cpu *c, int i, Piece *p, const char *want) {
    for (int k = 0; k < POOL; k++) {
        Piece *q = &g_pool[i][k];
        if ((!q->actor && !q->pending) || strcmp(q->name, want)) continue;
        if (q->actor && !alive(q->actor, q->w)) { memset(q, 0, sizeof *q); continue; }
        *p = *q;
        memset(q, 0, sizeof *q);
        return p->actor != 0;
    }
    return false;
}

// the pool's pieces still being made, as they come: taken, out of sight
static void pool_update(Cpu *c) {
    for (int i = 0; i < ONLINE_MAX; i++)
        for (int k = 0; k < POOL; k++) {
            Piece *q = &g_pool[i][k];
            int *unit = q->pending ? (int *)(uintptr_t)q->handle->unit : NULL;
            if (!unit) continue;
            if (*unit == 2) {
                q->pending = false;
                q->actor = call(c, FN_HANDLE_TAKE, (uint64_t)(uintptr_t)q->handle, 0, 0, 0, 0, 0);
                if (q->actor) q->w = watch(q->actor), hide_piece(c, q, g_pool_at);
                else memset(q, 0, sizeof *q);
            } else if (*unit == 3 || *unit == 5) {
                call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)q->handle, 0, 0, 0, 0, 0);
                memset(q, 0, sizeof *q);
            }
        }
}

// his Link goes (out of reach, his Link removed by the game...): his pieces to his pool; left: they go too
static void drop(Cpu *c, int i, bool left) {
    for (int k = 0; k < ARMOR; k++) pool_piece(c, i, &g_act[i].armor[k]);
    for (int k = 0; k < ONLINE_GEAR; k++) pool_piece(c, i, &g_act[i].gear[k]);
    pool_piece(c, i, &g_act[i].arrow);
    if (left)
        for (int k = 0; k < POOL; k++) delete_piece(c, &g_pool[i][k]);
    if (alive(g_act[i].link, g_act[i].link_w)) call(c, FN_DELETE, g_act[i].link, 0, 0, 0, 0, 0);
    LOG("[online] removed %s's Link\n", g_act[i].name);
    memset(&g_act[i], 0, sizeof g_act[i]);
    g_act[i].anim = -1, g_act[i].covered = -1;
}

// online_mod/gear.txt (tools/online_mod.py): each weapon, shield, bow, sheath and quiver's online actor, "actor=its
// online actor" a line
static char (*g_gear_map)[2][48];
static int g_gear_n = -1;
static const char *gear_actor(const char *piece) {
    if (g_gear_n < 0) {
        g_gear_n = 0;
        const char *ov = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char p[1100], line[128];
        snprintf(p, sizeof p, "%s/gear.txt", ov ? ov : "");
        FILE *f = ov && *ov ? fopen(p, "r") : NULL;
        int cap = 0;
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (g_gear_n == cap) g_gear_map = realloc(g_gear_map, sizeof *g_gear_map * (cap = cap ? cap * 2 : 256));
            snprintf(g_gear_map[g_gear_n][0], 48, "%s", line);
            snprintf(g_gear_map[g_gear_n][1], 48, "%s", eq + 1);
            g_gear_n++;
        }
        if (f) fclose(f);
        LOG("[online] %d pieces of gear for the other players\n", g_gear_n);
    }
    for (int i = 0; i < g_gear_n; i++)
        if (!strcmp(g_gear_map[i][0], piece)) return g_gear_map[i][1];
    return NULL;
}

static uint64_t create(Cpu *c, const char *name) {
    uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
    creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
    owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
    if (!creator || !owner) return 0;
    uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
    return call(c, FN_CREATE, creator, (uint64_t)(uintptr_t)name, heap, 0, 0, 0);
}

static bool request(Cpu *c, const char *name, Handle *handle) {
    uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
    creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
    owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
    if (!creator || !owner) return false;
    uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
    memset(handle, 0, 16);
    return call_f(c, FN_REQUEST, creator, (uint64_t)(uintptr_t)name, heap, (uint64_t)(uintptr_t)handle, 0, 0, 1, 0) & 1;
}

// p should be `want` ("" none): a piece that changed goes, and the new one is asked for; true when it was made just now
static bool piece_update(Cpu *c, int i, Piece *p, const char *want, const char *who) {
    if ((p->actor || p->pending || p->name[0]) && strcmp(p->name, want)) pool_piece(c, i, p);
    if (!p->actor && !p->pending && want[0] && from_pool(c, i, p, want)) {
        LOG("[online] %s: %s again (actor %llx)\n", who, want, (unsigned long long)p->actor);
        return true;
    }
    snprintf(p->name, sizeof p->name, "%s", want);
    if (p->actor && !alive(p->actor, p->w)) {
        LOG("[online] %s: the game removed his %s, made again\n", who, want);
        p->actor = 0, p->fails = 0;
    }
    if (!want[0] || p->actor) return false;
    if (p->pending) {
        int *unit = (int *)(uintptr_t)p->handle->unit;
        if (unit && *unit == 2) {
            p->pending = false;
            p->actor = call(c, FN_HANDLE_TAKE, (uint64_t)(uintptr_t)p->handle, 0, 0, 0, 0, 0);
            if (p->actor) p->w = watch(p->actor);
            if (p->actor) LOG("[online] %s: %s (actor %llx)\n", who, want, (unsigned long long)p->actor);
            return p->actor != 0;
        }
        if (unit && (*unit == 3 || *unit == 5)) {
            call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)p->handle, 0, 0, 0, 0, 0);
            p->pending = false, p->fails++, p->wait = 120;  // (again in two seconds, a few times)
            LOG("[online] %s: %s couldn't be made\n", who, want);
        }
        return false;
    }
    if (p->wait > 0) { p->wait--; return false; }
    if (p->fails < 5) {
        p->handle = new_handle();
        if (request(c, want, p->handle)) p->pending = true;
        else if (p->fails++, p->wait = 60, p->fails == 1) LOG("[online] can't ask for a %s\n", want);
    }
    return false;
}

// Link's body and his armour as the player's own: each part shows the materials no other part covers (Link's skin
// under a tunic, the top of trousers under a long one, his hair under a hood); all once all parts run (their models)
static void cover(Cpu *c, int i) {
    uint64_t x0 = *(uint64_t *)(uintptr_t)P_HIDE_GROUPS_X0;
    x0 = x0 ? *(uint64_t *)(uintptr_t)x0 : 0;
    uint64_t part[1 + ARMOR] = {g_act[i].link};
    for (int k = 0; k < ARMOR; k++) part[1 + k] = g_act[i].armor[k].actor;
    for (int k = 0; k < 1 + ARMOR; k++) {
        uint64_t model = part[k] ? *(uint64_t *)(uintptr_t)(part[k] + 0x4e0) : 0;
        if (!model) continue;
        call(c, FN_HIDE_GROUPS, x0, model, 0, 1, 0, 0);
        for (int j = 0; j < 1 + ARMOR; j++) {
            uint64_t other = j != k && part[j] ? *(uint64_t *)(uintptr_t)(part[j] + 0x4e0) : 0;
            if (other) call(c, FN_HIDE_GROUPS, x0, model, other, 0, 0, 0);
        }
    }
}

static bool running(uint64_t actor) { return actor && *(uint8_t *)(uintptr_t)(actor + 0x64) == 1; }  // (calc)

// An actor's skeleton: the gsys model unit that has Link's bones (the one Skl_Root is in) and how many bones it has
// (its skeleton, unit +0x118: +0x18 the local matrices, 0x60 each; +0x50 the bone count)
#define FN_BONE_LOCAL_GET 0x7000f960f8ull  // x0 unit, x1 Matrix34f *out, x2 Vec3 *scale out, w3 bone
#define FN_BONE_LOCAL_SET 0x7000f95fa8ull  // x0 unit, x1 Matrix34f *, x2 Vec3 *scale, w3 bone
static uint64_t skel_unit(Cpu *c, uint64_t actor, int *bones) {
    uint64_t model = *(uint64_t *)(uintptr_t)(actor + 0x4e0);
    if (!VALID(model)) return 0;
    uint64_t name[6] = {*(uint64_t *)(uintptr_t)P_SAFE_STRING_VT + 0x10, (uint64_t)(uintptr_t)"Skl_Root"};
    uint32_t r = (uint32_t)call(c, FN_FIND_BONE, model, (uint64_t)(uintptr_t)name, 0, 0, 0, 0);
    int16_t unit_i = (int16_t)(r & 0xffff);
    if (unit_i < 0) return 0;
    uint64_t units = *(uint64_t *)(uintptr_t)(model + 0x40);
    uint64_t unit = VALID(units) ? *(uint64_t *)(uintptr_t)(units + 8 * unit_i) : 0;
    unit = VALID(unit) ? *(uint64_t *)(uintptr_t)unit : 0;
    uint64_t sk = VALID(unit) ? *(uint64_t *)(uintptr_t)(unit + 0x118) : 0;
    if (!VALID(sk)) return 0;
    *bones = *(uint16_t *)(uintptr_t)(sk + 0x50);
    return unit;
}

// The player's equipment, as the game keeps it: actor links (a BaseProcLink: the unit, the id; Watch) to the actors
// of what he wears and carries. His armour's manager (player +0x2500; the armour code's, 0x70011750c8: its +0x128 is
// the player) links its pieces at +0x10, 0x10 each: head, upper, lower; the player links his weapon at +0xb98, his
// shield at +0xbb0, his bow at +0xbc8, 0x18 each; a weapon links its sheath, a bow its quiver, at +0x958 (their
// PodName, QuiverName)
static uint64_t linked(uint64_t at) {
    if (!VALID(at)) return 0;
    uint64_t u = *(uint64_t *)(uintptr_t)at;
    uint32_t id = *(uint32_t *)(uintptr_t)(at + 8);
    if (!VALID(u) || *(uint32_t *)(uintptr_t)(u + 0x40) != id) return 0;
    uint64_t a = *(uint64_t *)(uintptr_t)(u + 0x48);
    return VALID(a) && *(uint8_t *)(uintptr_t)(a + 0x64) != 3 ? a : 0;
}

static void equipment(uint64_t player, uint64_t armor[ARMOR], uint64_t gear[ONLINE_GEAR]) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)(player + 0x2500);
    bool mine = VALID(mgr) && *(uint64_t *)(uintptr_t)(mgr + 0x128) == player;
    for (int k = 0; k < ARMOR; k++) armor[k] = mine ? linked(mgr + 0x10 + 0x10 * k) : 0;
    for (int k = 0; k < 3; k++) gear[k] = linked(player + 0xb98 + 0x18 * k);
    gear[3] = gear[0] ? linked(gear[0] + 0x958) : 0;
    gear[4] = gear[2] ? linked(gear[2] + 0x958) : 0;
}

// an actor's name, when it is one ("" else)
static void actor_name_of(uint64_t a, char out[48]) {
    memset(out, 0, 48);
    uint64_t n = a ? *(uint64_t *)(uintptr_t)(a + 0x10) : 0;
    if (!VALID(n)) return;
    const char *s = (const char *)(uintptr_t)n;
    int l = 0;
    while (l < 47 && s[l] > ' ' && s[l] < 127) l++;
    if (!s[l]) memcpy(out, s, l), out[l] = 0;
}

// a model's first unit and its bone count (a weapon's: one unit; a bow's bones bend it and pull its string)
static uint64_t first_unit(uint64_t actor, int *bones) {
    uint64_t model = VALID(actor) ? *(uint64_t *)(uintptr_t)(actor + 0x4e0) : 0;
    uint64_t units = VALID(model) ? *(uint64_t *)(uintptr_t)(model + 0x40) : 0;
    uint64_t unit = VALID(units) ? *(uint64_t *)(uintptr_t)units : 0;
    unit = VALID(unit) ? *(uint64_t *)(uintptr_t)unit : 0;
    uint64_t sk = VALID(unit) ? *(uint64_t *)(uintptr_t)(unit + 0x118) : 0;
    if (!VALID(sk)) return 0;
    *bones = *(uint16_t *)(uintptr_t)(sk + 0x50);
    return unit;
}

// a bone's matrix in the world (Matrix34f, row-major), from an actor's model
static bool bone_mtx(Cpu *c, uint64_t actor, const char *bone, float out[12]) {
    uint64_t model = *(uint64_t *)(uintptr_t)(actor + 0x4e0);
    if (!VALID(model)) return false;
    uint64_t name[6] = {*(uint64_t *)(uintptr_t)P_SAFE_STRING_VT + 0x10, (uint64_t)(uintptr_t)bone};
    uint32_t r = (uint32_t)call(c, FN_FIND_BONE, model, (uint64_t)(uintptr_t)name, 0, 0, 0, 0);
    int16_t unit_i = (int16_t)(r & 0xffff);
    if (unit_i < 0) return false;
    uint64_t units = *(uint64_t *)(uintptr_t)(model + 0x40);
    uint64_t unit = VALID(units) ? *(uint64_t *)(uintptr_t)(units + 8 * unit_i) : 0;
    unit = VALID(unit) ? *(uint64_t *)(uintptr_t)unit : 0;
    if (!VALID(unit)) return false;
    float m[12];
    call(c, *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)unit + 0x70), unit, (uint64_t)(uintptr_t)m, r >> 16, 0, 0, 0);
    for (int k = 0; k < 12; k++)
        if (!isfinite(m[k])) return false;
    memcpy(out, m, sizeof m);
    return true;
}

// a * b (3x4: the 3x3 rotation and the translation)
static void mtx_mul(const float a[12], const float b[12], float out[12]) {
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++)
            out[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k];
        out[r * 4 + 3] = a[r * 4] * b[3] + a[r * 4 + 1] * b[7] + a[r * 4 + 2] * b[11] + a[r * 4 + 3];
    }
}

// inverse of a rigid 3x4 (rotation, translation)
static void mtx_inv(const float a[12], float out[12]) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) out[r * 4 + k] = a[k * 4 + r];
    for (int r = 0; r < 3; r++)
        out[r * 4 + 3] = -(out[r * 4] * a[3] + out[r * 4 + 1] * a[7] + out[r * 4 + 2] * a[11]);
}

// The local Link's pose for the others: his AS on each channel, his heading, and each piece of his gear on the bone
// nearest to it (the hands while drawn, the back's mounts while put away), as it sits on that bone
static void sample_pose(Cpu *c, uint64_t pinfo) {
    OnlinePose pose;
    memset(&pose, 0, sizeof pose);
    uint64_t player = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
    if (!VALID(player)) return;
    uint64_t mgr = *(uint64_t *)(uintptr_t)(player + 0x560);
    if (!VALID(mgr)) return;
    int slots = *(int *)(uintptr_t)(mgr + 0xb8);
    for (int ch = 0; ch < ONLINE_AS_CHANNELS; ch++) {
        int sl = k_channel[ch][0], bank = k_channel[ch][1];
        if (sl >= slots || bank >= *(int *)(uintptr_t)(*(uint64_t *)(uintptr_t)(mgr + 0xc0) + sl * 0x50 + 0x20)) continue;
        snprintf(pose.as[ch], ONLINE_AS_MAX, "%s", playing(c, player, sl, bank));
    }
    const float *pm = (const float *)(uintptr_t)(player + 0x398);
    pose.yaw = atan2f(pm[2], pm[10]);
    uint64_t armor[ARMOR], gear[ONLINE_GEAR];
    equipment(player, armor, gear);
    {   // what he wears and carries, for the others (the default pieces are Link's own body: "")
        char an[ARMOR][48] = {{0}}, gn[ONLINE_GEAR][48] = {{0}};
        for (int k = 0; k < ARMOR; k++) {
            actor_name_of(armor[k], an[k]);
            if (!strncmp(an[k], "Armor_Default", 13)) an[k][0] = 0;
        }
        for (int k = 0; k < ONLINE_GEAR; k++) actor_name_of(gear[k], gn[k]);
        online_set_local_equip((const char (*)[48])an, (const char (*)[48])gn);
    }
    float bm[BONES][12];
    bool have[BONES];
    for (int b = 0; b < BONES; b++) have[b] = bone_mtx(c, player, k_bones[b], bm[b]);
    for (int k = 0; k < ONLINE_GEAR; k++) {
        uint64_t a = gear[k];
        if (!a) continue;
        const float *w = (const float *)(uintptr_t)(a + 0x398);
        static uint8_t last_bone[ONLINE_GEAR];
        float best = 0.6f * 0.6f, kept = -1;  // (farther from every bone: thrown, dropped; not shown)
        float rel_of[BONES][12];
        for (int b = 0; b < BONES; b++) {
            if (!have[b]) continue;
            float inv[12], *rel = rel_of[b];
            mtx_inv(bm[b], inv);
            mtx_mul(inv, w, rel);
            float d = rel[3] * rel[3] + rel[7] * rel[7] + rel[11] * rel[11];
            if (b + 1 == last_bone[k]) kept = d;
            if (d < best) best = d, pose.gear_bone[k] = (uint8_t)(b + 1);
        }
        // the bone it was on, while it stays near it and no other is much nearer (two bones as near: no flicker)
        if (kept >= 0 && kept < 0.6f * 0.6f && kept < best * 4) pose.gear_bone[k] = last_bone[k];
        if (pose.gear_bone[k]) memcpy(pose.gear_rel[k], rel_of[pose.gear_bone[k] - 1], sizeof pose.gear_rel[k]);
        last_bone[k] = pose.gear_bone[k];
    }
    pose.valid = pose.as[0][0] != 0;
    online_set_local_pose(&pose);
    if (getenv("SAVAGEWIND_ONLINE_POSE_LOG")) {
        static OnlinePose last;
        if (memcmp(last.as, pose.as, sizeof pose.as) || memcmp(last.gear_bone, pose.gear_bone, sizeof pose.gear_bone))
            LOG("[online] pose %s|%s|%s|%s gear %d %d %d %d %d\n", pose.as[0], pose.as[1], pose.as[2], pose.as[3],
                pose.gear_bone[0], pose.gear_bone[1], pose.gear_bone[2], pose.gear_bone[3], pose.gear_bone[4]);
        last = pose;
    }
}

static void animate(Cpu *c, int i, const OnlinePlayer *p);

// a rotation matrix (row-major 3x4's 3x3) as a quaternion (x y z w), and back
static void mtx_to_quat(const float m[12], float q[4]) {
    float tr = m[0] + m[5] + m[10];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (m[9] - m[6]) / s, q[1] = (m[2] - m[8]) / s, q[2] = (m[4] - m[1]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[9] - m[6]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[4]) / s, q[2] = (m[2] + m[8]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[2] - m[8]) / s, q[0] = (m[1] + m[4]) / s, q[1] = 0.25f * s, q[2] = (m[6] + m[9]) / s;
    } else {
        float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[4] - m[1]) / s, q[0] = (m[2] + m[8]) / s, q[1] = (m[6] + m[9]) / s, q[2] = 0.25f * s;
    }
}

static void quat_to_mtx(const float q[4], float m[12]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[4] = 2 * (x * y + z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z - x * w);
    m[8] = 2 * (x * z - y * w), m[9] = 2 * (y * z + x * w), m[10] = 1 - 2 * (x * x + y * y);
}

// The local Link's skeleton for the others: each bone's local rotation and translation, as his animation left them
static void sample_skeleton(Cpu *c, uint64_t pinfo) {
    uint64_t player = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
    int n = 0;
    uint64_t unit = VALID(player) ? skel_unit(c, player, &n) : 0;
    if (!unit || n <= 0) return;
    if (n > ONLINE_BONES) n = ONLINE_BONES;
    // and his bow's, after his
    uint64_t bow = linked(player + 0xb98 + 0x18 * 2);
    int nbow = 0;
    uint64_t bow_unit = VALID(bow) ? first_unit(bow, &nbow) : 0;
    if (!bow_unit || nbow < 0) nbow = 0;
    if (nbow > ONLINE_BOW_BONES) nbow = ONLINE_BOW_BONES;
    if (n > ONLINE_BONES - nbow) n = ONLINE_BONES - nbow;
    static float q[ONLINE_BONES][4], t[ONLINE_BONES][3];
    for (int b = 0; b < n + nbow; b++) {
        float m[12], sc[3];
        call(c, FN_BONE_LOCAL_GET, b < n ? unit : bow_unit, (uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)sc, b < n ? b : b - n,
             0, 0);
        mtx_to_quat(m, q[b]);
        t[b][0] = m[3], t[b][1] = m[7], t[b][2] = m[11];
    }
    online_set_local_skeleton(n, nbow, (const float (*)[4])q, (const float (*)[3])t);
}

// His Link posed: his player's skeleton, bone by bone, written into his Link's own (it plays no AS, so nothing
// writes over it before the game makes his bones' matrices in the world from it); while none has come, the AS
// his Link plays (online.h OnlinePose: an older game, the test bot)
static void pose_link(Cpu *c, int i, const OnlinePlayer *p) {
    static float q[ONLINE_BONES][4], t[ONLINE_BONES][3];
    int n = 0, nbow = 0, nb = 0, nbb = 0;
    g_act[i].frames++;
    if (online_skeleton(i, &n, &nbow, q, t)) {
        uint64_t unit = skel_unit(c, g_act[i].link, &nb);
        // his bow (its string drawn) when the one he carries is the one posed
        Piece *bp = &g_act[i].gear[2];
        uint64_t bow_unit = nbow && alive(bp->actor, bp->w) ? first_unit(bp->actor, &nbb) : 0;
        g_act[i].skeleton = true;
        for (int b = 0; b < n + nbow; b++) {
            uint64_t u = b < n ? unit : bow_unit;
            int k = b < n ? b : b - n;
            if (!u || k >= (b < n ? nb : nbb)) continue;
            float m[12], sc[3] = {1, 1, 1};
            quat_to_mtx(q[b], m);
            m[3] = t[b][0], m[7] = t[b][1], m[11] = t[b][2];
            call(c, FN_BONE_LOCAL_SET, u, (uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)sc, k, 0, 0);
        }
        return;
    }
    if (!g_act[i].skeleton && g_act[i].frames > 60) animate(c, i, p);  // (a second for his skeleton to come first)
}

// the remote Link plays what his player's Link plays: the player's own AS on each channel (a channel that went
// quiet keeps its last one: its bank is played over by the others), or without a pose the one-clip AS of anim
static void animate(Cpu *c, int i, const OnlinePlayer *p) {
    uint64_t link = g_act[i].link;
    if (!p->pose.valid) {
        if (p->anim < ONLINE_A_COUNT && p->anim != g_act[i].anim && play_as(c, link, k_anims[p->anim], 0, 0, 6.0f, true))
            g_act[i].anim = p->anim;
        return;
    }
    for (int ch = 0; ch < ONLINE_AS_CHANNELS; ch++) {
        const char *want = p->pose.as[ch];
        if (!want[0] || !strcmp(g_act[i].as[ch], want)) continue;
        play_as(c, link, want, k_channel[ch][0], k_channel[ch][1], -1.0f, true);
        snprintf(g_act[i].as[ch], ONLINE_AS_MAX, "%s", want);  // (once, played or not: an AS it lacks isn't asked again)
    }
    g_act[i].anim = -1;
}

void rt_online_frame(Cpu *c) {
    if (!online_actors_on()) return;
    int me = online_local_id();
    uint64_t pinfo = g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0;
    float link[3] = {0, 0, 0};
    if (pinfo) memcpy(link, (const void *)(uintptr_t)(pinfo + 0x88), sizeof link);
    bool world = pinfo && me >= 0 && !framepace_title_ui() && (link[0] != 0 || link[1] != 0 || link[2] != 0);
    // a load (no Link, or Link somewhere else at once): the game has removed the world's actors, ours with them
    float dx = link[0] - g_last_link[0], dy = link[1] - g_last_link[1], dz = link[2] - g_last_link[2];
    if (!world || dx * dx + dy * dy + dz * dz > 100 * 100) {
        bool any = false;
        for (int i = 0; i < ONLINE_MAX; i++) any |= g_act[i].link != 0;
        if (any) LOG("[online] a load: the players' actors are gone\n");
        forget_all();
        g_settle = 60;
    }
    memcpy(g_last_link, link, sizeof link);
    for (int i = 0; i < ONLINE_MAX; i++) g_online_actor[i] = world && running(g_act[i].link);
    if (!world) return;
    if (!framepace_paused()) sample_pose(c, pinfo), sample_skeleton(c, pinfo);
    memcpy(g_pool_at, link, sizeof g_pool_at);
    pool_update(c);
    if (g_settle > 0) { g_settle--; return; }

    OnlinePlayer pl[ONLINE_MAX];
    online_players(pl);
    for (int i = 0; i < ONLINE_MAX; i++) {
        bool want = pl[i].used && i != me && !(pl[i].flags & ONLINE_F_LOADING);
        if (want) {
            float ex = pl[i].pos[0] - link[0], ez = pl[i].pos[2] - link[2];
            want = ex * ex + ez * ez < 400 * 400;  // (far away: the world there isn't loaded)
        }
        if (!want) {
            if (g_act[i].link && !framepace_paused()) drop(c, i, !pl[i].used);
            else if (!pl[i].used)  // (left: his pool goes)
                for (int k = 0; k < POOL; k++)
                    if (g_pool[i][k].actor || g_pool[i][k].pending) delete_piece(c, &g_pool[i][k]);
            continue;
        }
        // his Link (gone, deleted by the game: made again, his pieces with it)
        if (g_act[i].link && !alive(g_act[i].link, g_act[i].link_w)) {
            LOG("[online] the game removed %s's Link, made again\n", pl[i].name);
            g_act[i].link = 0;
            drop(c, i, false);  // (his pieces still there to his pool)
            g_act[i].anim = -1, g_act[i].covered = -1;
        }
        if (!g_act[i].link) {
            if (g_act[i].link_fails > 10) continue;
            g_act[i].link = create(c, actor_name());
            if (!g_act[i].link) {
                if (g_act[i].link_fails++ == 0) LOG("[online] can't create a %s for %s (not loaded?)\n", actor_name(), pl[i].name);
                continue;
            }
            g_act[i].link_w = watch(g_act[i].link);
            memcpy(g_act[i].name, pl[i].name, sizeof g_act[i].name);
            LOG("[online] %s: %s (actor %llx)\n", pl[i].name, actor_name(), (unsigned long long)g_act[i].link);
        }
        // the matrix: turned about y by the heading (forward is +z), at the player's feet
        float s = sinf(pl[i].yaw), co = cosf(pl[i].yaw);
        float m[12] = {co, 0, s, pl[i].pos[0], 0, 1, 0, pl[i].pos[1], -s, 0, co, pl[i].pos[2]};
        call(c, FN_SET_MTX, g_act[i].link, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
        if (!is_online_player() || !running(g_act[i].link)) continue;
        pose_link(c, i, &pl[i]);

        // his armour: the game's own pieces (no helmet: Link's hair), worn on his Link's skeleton
        for (int k = 0; k < ARMOR; k++) {
            const char *w = pl[i].armor[k][0] ? pl[i].armor[k] : k == 0 ? "Armor_Default_Head" : "";
            Piece *p = &g_act[i].armor[k];
            bool had = p->actor != 0;
            if (piece_update(c, i, p, w, pl[i].name)) {
                call(c, FN_ARMOR_BIND, p->actor, g_act[i].link, 0, 0, 0, 0);
                call(c, FN_ARMOR_INIT, p->actor, 0, 0, 0, 0, 0);
                model_visible(c, p->actor, true);  // (one from his pool: hidden there)
                g_act[i].covered = -1;
            }
            if (had != (p->actor != 0)) g_act[i].covered = -1;
        }
        int pieces = 0;
        bool all_running = true;
        for (int k = 0; k < ARMOR; k++) {
            pieces |= (g_act[i].armor[k].actor != 0) << k;
            if (g_act[i].armor[k].actor) all_running &= running(g_act[i].armor[k].actor);
        }
        if (all_running && pieces != g_act[i].covered) cover(c, i), g_act[i].covered = pieces;

        // his gear, on his Link's bones as in his game (not there: under the world, out of sight)
        float bm[BONES][12];
        int have = 0;  // (bones read: a bit each)
        for (int k = 0; k < ONLINE_GEAR; k++) {
            const char *ga = pl[i].gear[k][0] ? gear_actor(pl[i].gear[k]) : NULL;
            Piece *p = &g_act[i].gear[k];
            piece_update(c, i, p, ga ? ga : "", pl[i].name);
            if (!p->actor) continue;
            int b = pl[i].pose.valid ? pl[i].pose.gear_bone[k] - 1 : -1;
            float gm[12] = {1, 0, 0, pl[i].pos[0], 0, 1, 0, pl[i].pos[1] - 5000, 0, 0, 1, pl[i].pos[2]};
            if (b >= 0 && b < BONES) {
                if (!(have >> b & 1) && bone_mtx(c, g_act[i].link, k_bones[b], bm[b])) have |= 1 << b;
                if (have >> b & 1) mtx_mul(bm[b], pl[i].pose.gear_rel[k], gm);
            }
            call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)gm, 0, 0, 0, 0);
            static int n;
            if (getenv("SAVAGEWIND_ONLINE_GEAR_LOG") && n++ % 120 < ONLINE_GEAR)
                LOG("[online] gear %d %s bone %d (%s) at %.2f %.2f %.2f, Link %.2f %.2f %.2f, rel t %.2f %.2f %.2f\n", k,
                    p->name, b, b >= 0 && (have >> b & 1) ? "ok" : "-", gm[3], gm[7], gm[11], pl[i].pos[0], pl[i].pos[1],
                    pl[i].pos[2], pl[i].pose.gear_rel[k][3], pl[i].pose.gear_rel[k][7], pl[i].pose.gear_rel[k][11]);
        }
        // an arrow in his right hand, on the string, while he nocks and draws his bow (the upper body's AS: BowReload,
        // BowCharge...; his bow in a hand); kept made while he carries a bow, out of sight the rest of the time
        Piece *ar = &g_act[i].arrow;
        const char *an = pl[i].gear[2][0] ? gear_actor("NormalArrow") : NULL;
        piece_update(c, i, ar, an ? an : "", pl[i].name);
        if (ar->actor) {
            const char *up = pl[i].pose.as[2];
            bool drawn = pl[i].pose.valid && (pl[i].pose.gear_bone[2] == 1 || pl[i].pose.gear_bone[2] == 2) &&
                         (!strncmp(up, "BowCharge", 9) || !strncmp(up, "BowReload", 9));
            float am[12] = {1, 0, 0, pl[i].pos[0], 0, 1, 0, pl[i].pos[1] - 5000, 0, 0, 1, pl[i].pos[2]};
            if (drawn && !(have & 1) && bone_mtx(c, g_act[i].link, k_bones[0], bm[0])) have |= 1;
            if (drawn && !(have & 2) && bone_mtx(c, g_act[i].link, k_bones[1], bm[1])) have |= 2;
            if (drawn && (have & 3) == 3) {  // (its nock in the right hand, its point (+z) past the bow in the left)
                float z[3] = {bm[1][3] - bm[0][3], bm[1][7] - bm[0][7], bm[1][11] - bm[0][11]}, l = 0;
                for (int k = 0; k < 3; k++) l += z[k] * z[k];
                l = l > 1e-8f ? 1 / sqrtf(l) : 0;
                for (int k = 0; k < 3; k++) z[k] *= l;
                float x[3] = {z[2], 0, -z[0]}, lx = sqrtf(x[0] * x[0] + x[2] * x[2]);  // (up x z)
                if (lx < 1e-4f) x[0] = 1, x[2] = 0, lx = 1;
                x[0] /= lx, x[2] /= lx;
                float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
                float m[12] = {x[0], y[0], z[0], bm[0][3], x[1], y[1], z[1], bm[0][7], x[2], y[2], z[2], bm[0][11]};
                if (l > 0) memcpy(am, m, sizeof am);
            }
            call(c, FN_SET_MTX, ar->actor, (uint64_t)(uintptr_t)am, 0, 0, 0, 0);
        }
    }
}
