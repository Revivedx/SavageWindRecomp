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
#define ACTOR_HASH_ID 0x640  // (an actor's map object HashId: u32)
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

#define MAGNE_BEAM 10
// A remote player in the world: his Link, his armour and his gear
static struct {
    uint64_t link;  // OnlinePlayer; 0: none
    Watch link_w;
    int link_fails;
    Piece armor[ARMOR], gear[ONLINE_GEAR];
    Piece shot[ONLINE_SHOTS];      // what he holds, threw or shot (bombs, arrows: OnlineShots)
    uint16_t shot_id[ONLINE_SHOTS];  // (its id; 0: unused, out of sight)
    Piece nock;                    // the arrow on his bow's string while he draws it (it's no actor in his game)
    struct { uint64_t p; uint32_t id, pad; } kago[2];  // Daruk's Protection on his Link: its effects' handles
    int kago_n;                    // (the ring's charges it was made for)
    uint64_t grab;                 // what he holds with the Magnesis rune, here (0: none)
    Watch grab_w;
    char grab_name[ONLINE_GRAB_NAME];
    float grab_from[3];
    int grab_wait;                 // (frames before looking for it again: not found)
    struct { uint64_t p; uint32_t id, pad; } magne;  // the rune's glow on it (its handle)
    struct { uint64_t p; uint32_t id, pad; } beam[MAGNE_BEAM];  // the rune's beam from his hand to it (its effects)
    uint64_t stop;                 // what he stopped with the Stasis rune, here (0: none)
    Watch stop_w;
    char stop_name[ONLINE_GRAB_NAME];
    int stop_wait;
    struct { uint64_t p; uint32_t id, pad; } chains;  // its chains (BitaLocked_Start)
    struct { Piece p; float m[12]; bool used, rose; } ice[3];  // his Cryonis pillars, here (OnlineIce)
    uint16_t boom_seen;              // (the last explosion played)
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
static bool g_local_dead;  // the local Link is dead (his body's AS: Dead...): game over is coming
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

// a call whose result the game returns in memory (x8: where)
static void call_sret(Cpu *c, uint64_t fn, uint64_t out, uint64_t a0, uint64_t a1) {
    static Cpu saved;  // (the main thread only)
    memcpy(&saved, c, sizeof saved);
    for (int k = 0; k < 8; k++) c->x[k] = 0;
    c->x[0] = a0, c->x[1] = a1, c->x[8] = out;
    c->sp = (c->sp - 0x400) & ~0xFull;
    guest_call(c, fn);
    memcpy(c, &saved, sizeof saved);
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

static void booms_forget(void);

// requests still under way when all was forgotten: what they make is deleted once made (else it would stay in the
// world, a piece nobody moves)
#define ORPHANS 128
static Piece g_orphan[ORPHANS];
static void orphan(Piece *p) {
    if (!p->pending) return;
    for (int k = 0; k < ORPHANS; k++)
        if (!g_orphan[k].pending) { g_orphan[k] = *p; return; }
}

static void orphans_update(Cpu *c) {
    for (int k = 0; k < ORPHANS; k++) {
        Piece *q = &g_orphan[k];
        if (!q->pending) continue;
        int *unit = (int *)(uintptr_t)q->handle->unit;
        if (unit && *unit == 2) {
            uint64_t a = call(c, FN_HANDLE_TAKE, (uint64_t)(uintptr_t)q->handle, 0, 0, 0, 0, 0);
            if (a) call(c, FN_DELETE, a, 0, 0, 0, 0, 0);
            memset(q, 0, sizeof *q);
        } else if (unit && (*unit == 3 || *unit == 5)) {
            call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)q->handle, 0, 0, 0, 0, 0);
            memset(q, 0, sizeof *q);
        }
    }
}

static void puppets_forget(void);
static void owns_forget(void);
static void loot_forget(void);

static void forget_all(void) {
    for (int i = 0; i < ONLINE_MAX; i++) {
        for (int k = 0; k < ARMOR; k++) orphan(&g_act[i].armor[k]);
        for (int k = 0; k < ONLINE_GEAR; k++) orphan(&g_act[i].gear[k]);
        for (int k = 0; k < ONLINE_SHOTS; k++) orphan(&g_act[i].shot[k]);
        orphan(&g_act[i].nock);
        for (int k = 0; k < POOL; k++) orphan(&g_pool[i][k]);
    }
    memset(g_act, 0, sizeof g_act);
    puppets_forget();
    owns_forget();
    loot_forget();
    booms_forget();
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
    for (int k = 0; k < ONLINE_SHOTS; k++) pool_piece(c, i, &g_act[i].shot[k]);
    pool_piece(c, i, &g_act[i].nock);
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

// online_mod/armor.txt: each armour piece's online actor (OnlineArmor_<its model list>), "piece=actor" a line;
// online_mod/bones.txt: each of those actors' bone map, "actor=unit|unit..." (per unit of its model list, each bone's
// index in Link's skeleton, 255 for one Link hasn't: its cloth's, its support bones')
typedef struct { char key[48]; char *val; } MapLine;
static int map_load(const char *file, MapLine **out) {
    const char *ov = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", ov ? ov : "", file);
    FILE *f = ov && *ov ? fopen(p, "rb") : NULL;
    int n = 0, cap = 0;
    static char line[16384];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (n == cap) *out = realloc(*out, sizeof **out * (cap = cap ? cap * 2 : 256));
        snprintf((*out)[n].key, sizeof (*out)[n].key, "%s", line);
        (*out)[n].val = strdup(eq + 1);
        n++;
    }
    if (f) fclose(f);
    return n;
}

static const char *map_find(MapLine *m, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].key, key)) return m[i].val;
    return NULL;
}

static const char *armor_actor(const char *piece) {
    static MapLine *m;
    static int n = -1;
    if (n < 0) n = map_load("armor.txt", &m), LOG("[online] %d armour pieces for the other players\n", n);
    return map_find(m, n, piece);
}

// an armour actor's bone map for unit u: Link bone index per bone (255 none); its length in *len
#define ARMOR_BONES 160
static const uint8_t *armor_bones(const char *actor, int u, int *len) {
    static MapLine *m;
    static int n = -1;
    static struct { char key[48]; int units; uint8_t map[4][ARMOR_BONES]; int len[4]; } cache[16];
    static int next;
    if (n < 0) n = map_load("bones.txt", &m);
    for (int k = 0; k < 16; k++)
        if (!strcmp(cache[k].key, actor) && cache[k].units) {
            int uu = u < cache[k].units ? u : cache[k].units - 1;
            *len = cache[k].len[uu];
            return cache[k].map[uu];
        }
    const char *v = map_find(m, n, actor);
    if (!v) return NULL;
    int slot = next++ % 16;
    memset(&cache[slot], 0, sizeof cache[slot]);
    snprintf(cache[slot].key, sizeof cache[slot].key, "%s", actor);
    int unit = 0, b = 0;
    for (const char *c = v; *c && unit < 4; c++) {
        if (*c == '|') { cache[slot].len[unit++] = b, b = 0; continue; }
        if (*c == ',') continue;
        int x = atoi(c);
        while (c[1] >= '0' && c[1] <= '9') c++;
        if (b < ARMOR_BONES) cache[slot].map[unit][b++] = (uint8_t)(x > 255 ? 255 : x);
    }
    if (unit < 4) cache[slot].len[unit++] = b;
    cache[slot].units = unit;
    int uu = u < unit ? u : unit - 1;
    *len = cache[slot].len[uu];
    return cache[slot].map[uu];
}

static uint64_t create(Cpu *c, const char *name) {
    uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
    creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
    owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
    if (!creator || !owner) return 0;
    uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
    return call(c, FN_CREATE, creator, (uint64_t)(uintptr_t)name, heap, 0, 0, 0);
}

static bool request_p(Cpu *c, const char *name, Handle *handle, uint64_t params);
static bool request(Cpu *c, const char *name, Handle *handle) { return request_p(c, name, handle, 0); }

// params: an InstParamPack (the values it is made with: an enemy's EquipItem1, ...), 0 none
static bool request_p(Cpu *c, const char *name, Handle *handle, uint64_t params) {
    uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
    creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
    owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
    if (!creator || !owner) return false;
    uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
    memset(handle, 0, 16);
    return call_f(c, FN_REQUEST, creator, (uint64_t)(uintptr_t)name, heap, (uint64_t)(uintptr_t)handle, params, 0, 1, 0) & 1;
}

// An InstParamPack (as 0x7000b1711c makes one to make an enemy with its weapon): +0 its vtable (the GOT's 0x7001d664b0
// + 0x10), +0x10 its buffer: s16 how many, s16 bytes used, then 0xc0 bytes of entries, each the key (a char *), its
// type (5 a string), its value (0x7001126168 adds one)
typedef struct { uint64_t vt, pad; int16_t n, used; uint8_t data[0xc0]; } InstParams;
static void inst_params_string(InstParams *p, const char *key, const char *value) {
    if (!p->vt) p->vt = *(uint64_t *)(uintptr_t)0x7001d664b0ull + 0x10;
    int len = (int)strlen(value) + 1;
    if (p->used + 9 + len > (int)sizeof p->data) return;
    uint64_t k = (uint64_t)(uintptr_t)key;
    memcpy(p->data + p->used, &k, 8);
    p->data[p->used + 8] = 5;
    memcpy(p->data + p->used + 9, value, len);
    p->used += 9 + len, p->n++;
}

float test_warp_hour(bool *held);   // runtime/services/test_warp.c
void test_warp_set_hour(float hour);
uint64_t test_warp_gdt_mgr(void);   // runtime/services/test_warp.c
static float g_dist;  // (how far the player being done is)

// p should be `want` ("" none): a piece that changed goes, and the new one is asked for; true when it was made just now
static bool piece_update(Cpu *c, int i, Piece *p, const char *want, const char *who) {
    if ((p->actor || p->pending || p->name[0]) && strcmp(p->name, want)) pool_piece(c, i, p);
    if (!p->actor && !p->pending && want[0] && from_pool(c, i, p, want)) {
        LOG("[online] %s: %s again (actor %llx)\n", who, want, (unsigned long long)p->actor);
        return true;
    }
    snprintf(p->name, sizeof p->name, "%s", want);
    if (p->actor && !alive(p->actor, p->w)) {
        LOG("[online] %s: the game removed his %s, made again (%.0f m away)\n", who, want, g_dist);
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
    if (!strncmp(want, "Armor_Default", 13)) {  // (Link's default pieces, resident: asked for, they are never drawn;
        uint64_t a = create(c, want);           // made at once, they are)
        if (a) {
            p->actor = a, p->w = watch(a), p->fails = 0;
            LOG("[online] %s: %s (actor %llx)\n", who, want, (unsigned long long)a);
            return true;
        }
    }
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
#define FN_EFFECT_EMIT 0x7001100bd0ull  // x8 out {effect, id}, x0 an actor, x1 a name in its ELink user
#define FN_EFFECT_KILL 0x700056d010ull  // x0 such a handle: the effect ends
#define FN_XLINK_EMIT 0x700159f00cull  // x0 an actor, x1 an XLink keyword ("Disappear"), w2 2, x3 0x20 bytes of 0
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

static void actor_name_of(uint64_t a, char out[48]);


// his paraglider (PlayerStole2, or its _Vagrant look): an actor the player links (the link's
// place in the player found once, by looking through him for one; again every second while none is)
static uint64_t glider_of(uint64_t player) {
    static int off = -1, miss;
    char n[48];
    if (off >= 0) {
        uint64_t a = linked(player + off);
        actor_name_of(a, n);
        if (strncmp(n, "PlayerStole2", 12)) a = 0;
        if (a || miss++ % 30) return a;
    } else if (miss++ % 30) return 0;
    for (int o = 0x100; o < 0x4000; o += 8) {
        uint64_t u = *(uint64_t *)(uintptr_t)(player + o);
        if (!VALID(u) || (u & 7)) continue;
        uint64_t a = linked(player + o);
        actor_name_of(a, n);
        if (strncmp(n, "PlayerStole2", 12)) continue;
        if (off != o) LOG("[online] his paraglider: player +0x%x\n", o);
        off = o;
        return a;
    }
    return 0;
}

static void equipment(uint64_t player, uint64_t armor[ARMOR], uint64_t gear[ONLINE_GEAR]) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)(player + 0x2500);
    bool mine = VALID(mgr) && *(uint64_t *)(uintptr_t)(mgr + 0x128) == player;
    for (int k = 0; k < ARMOR; k++) armor[k] = mine ? linked(mgr + 0x10 + 0x10 * k) : 0;
    for (int k = 0; k < 3; k++) gear[k] = linked(player + 0xb98 + 0x18 * k);
    gear[3] = gear[0] ? linked(gear[0] + 0x958) : 0;
    gear[4] = gear[2] ? linked(gear[2] + 0x958) : 0;
    gear[5] = glider_of(player);
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

// OnlineShots' kinds: the game's actors
static const char *const k_shot_names[ONLINE_K_COUNT] = {
    "RemoteBomb", "RemoteBombCube", "RemoteBomb2", "RemoteBombCube2", "NormalArrow", "FireArrow", "IceArrow",
    "ElectricArrow", "BombArrow_A", "AncientArrow", "BrightArrow", "PlayerBeam"};
#define IS_BOMB(kind) ((kind) <= ONLINE_K_REMOTE_BOMB_CUBE2)

// Arrows in flight (tools/patches.py _arrow_move: each update of an arrow's flight action, on whatever thread the
// actors update on): their actors, noted in a ring for the frame loop to look at
#define ARROW_RING 64
static uint64_t volatile g_arrow_ring[ARROW_RING];
static volatile int g_arrow_next;

// the Remote Bomb rune's bombs, as it makes them (tools/patches.py _remote_bomb_made; the main thread): his
static uint64_t volatile g_bomb_made[4];
static volatile int g_bomb_next;

void rt_online_bomb(uint64_t bomb) {
    if (!VALID(bomb)) return;
    int k = __atomic_fetch_add(&g_bomb_next, 1, __ATOMIC_RELAXED);
    g_bomb_made[k & 3] = bomb;
}

void rt_online_arrow(uint64_t action) {
    uint64_t a = *(uint64_t *)(uintptr_t)(action + 8);
    if (!VALID(a)) return;
    int k = __atomic_fetch_add(&g_arrow_next, 1, __ATOMIC_RELAXED);
    g_arrow_ring[k & (ARROW_RING - 1)] = a;
}

// What the local Link threw or shot: his rune's bombs (rt_online_bomb), his arrows in flight (rt_online_arrow), each
// followed from then until it goes (held, thrown, flying, stuck: as it is in the world), a bomb that goes having gone
// off (the Remote Bomb's bombs go when they do)
static void sample_shots(Cpu *c, uint64_t player, OnlineShots *out) {
    static struct { uint64_t a; Watch w; uint16_t id; uint8_t kind; bool boomed; float m[12]; } tr[ONLINE_SHOTS];
    static uint16_t next_id = 1, boom_seq;
    static struct { uint16_t seq; uint8_t kind; float pos[3]; int frames; } booms[ONLINE_BOOMS];
    // the bombs his rune made
    static uint64_t bomb_seen[4];
    for (int r = 0; r < 4; r++) {
        uint64_t held = g_bomb_made[r];
        if (!held || held == bomb_seen[r]) continue;
        bomb_seen[r] = held;
        char name[48];
        actor_name_of(held, name);
        int kind = -1;
        for (int k = 0; IS_BOMB(k); k++)
            if (!strcmp(name, k_shot_names[k])) kind = k;
        bool known = false;
        for (int k = 0; k < ONLINE_SHOTS; k++) known |= tr[k].a == held;
        for (int k = 0; kind >= 0 && !known && k < ONLINE_SHOTS; k++)
            if (!tr[k].a) {
                tr[k].a = held, tr[k].w = watch(held), tr[k].kind = (uint8_t)kind, tr[k].id = next_id++;
                LOG("[online] shot %d: %s %llx\n", tr[k].id, name, (unsigned long long)held);
                if (!next_id) next_id = 1;
                break;
            }
    }
    // arrows in flight his own: those first seen near him (an enemy's are far); his beams (only his: anywhere)
    static uint64_t seen[ARROW_RING];
    const float *me = (const float *)(uintptr_t)(player + 0x398);
    for (int r = 0; r < ARROW_RING; r++) {
        uint64_t a = g_arrow_ring[r];
        if (!a || a == seen[r]) continue;
        seen[r] = a;
        bool known2 = false;
        for (int k = 0; k < ONLINE_SHOTS; k++) known2 |= tr[k].a == a;
        if (known2) continue;
        char an[48];
        actor_name_of(a, an);
        int ak = -1;
        for (int k = ONLINE_K_NORMAL_ARROW; k < ONLINE_K_COUNT; k++)
            if (!strcmp(an, k_shot_names[k])) ak = k;
        const float *am = (const float *)(uintptr_t)(a + 0x398);
        float dx = am[3] - me[3], dy = am[7] - me[7], dz = am[11] - me[11];
        if (ak < 0 || (ak != ONLINE_K_BEAM && dx * dx + dy * dy + dz * dz > 4 * 4)) continue;
        if (ak == ONLINE_K_BEAM) LOG("[online] his beam %llx\n", (unsigned long long)a);
        for (int k = 0; k < ONLINE_SHOTS; k++)
            if (!tr[k].a) {
                tr[k].a = a, tr[k].w = watch(a), tr[k].kind = (uint8_t)ak, tr[k].id = next_id++;
                if (!next_id) next_id = 1;
                break;
            }
    }
    memset(out, 0, sizeof *out);
    for (int k = 0; k < ONLINE_SHOTS; k++) {
        if (!tr[k].a) continue;
        bool gone = !alive(tr[k].a, tr[k].w);
        // a bomb goes off as it starts its AS "Explode" (it goes some frames later)
        if (!gone) memcpy(tr[k].m, (const float *)(uintptr_t)(tr[k].a + 0x398), sizeof tr[k].m);
        // (gone without it: put away, cancelled - no explosion)
        if (IS_BOMB(tr[k].kind) && !tr[k].boomed && !gone && !strcmp(playing(c, tr[k].a, 0, 0), "Explode")) {
            memmove(&booms[1], &booms[0], sizeof booms[0] * (ONLINE_BOOMS - 1));
            booms[0].seq = ++boom_seq ? boom_seq : ++boom_seq, booms[0].kind = tr[k].kind, booms[0].frames = 0;
            booms[0].pos[0] = tr[k].m[3], booms[0].pos[1] = tr[k].m[7], booms[0].pos[2] = tr[k].m[11];
            tr[k].boomed = true;
            LOG("[online] shot %d (%s) went off%s\n", tr[k].id, k_shot_names[tr[k].kind], gone ? " (gone)" : "");
        }
        if (gone) {
            LOG("[online] shot %d (%s) gone\n", tr[k].id, k_shot_names[tr[k].kind]);
            memset(&tr[k], 0, sizeof tr[k]);
            continue;
        }
        if (tr[k].boomed) continue;  // (going off: no longer shown as a bomb)
        out->shot[out->n].id = tr[k].id, out->shot[out->n].kind = tr[k].kind;
        memcpy(out->shot[out->n].m, tr[k].m, sizeof tr[k].m);
        out->n++;
    }
    for (int k = 0; k < ONLINE_BOOMS; k++)  // (each sent for two seconds: the others see it once by its number)
        if (booms[k].seq && booms[k].frames++ < 120) {
            out->boom[k].seq = booms[k].seq, out->boom[k].kind = booms[k].kind;
            memcpy(out->boom[k].pos, booms[k].pos, sizeof booms[k].pos);
        }
}

// Explosions of the others' bombs: an OnlineBoom actor asked for at that place (its resources load meanwhile), then
// sent the Remote Bomb rune's "go off" message (0x800000a: 0x7000b3b66c sends it to each of its bombs) through a
// message transceiver of the local player's (his armour manager's, player +0x2500 +0xd8: the armour code sends its
// messages so); its own AI goes off (shock wave, effects, sound) and deletes it. Sent again while it is there
#define FN_SEND_MSG 0x700154c61cull  // x0 the sender's transceiver, x1 the receiver (actor +0x5f0), x2 the message, w4 1
#define BOOMS_MAX 8
static struct { Handle *h; uint64_t actor; Watch w; float pos[3]; int frames; bool used, quiet; } g_boom[BOOMS_MAX];  // (quiet: OnlineKnock, unseen)

static void boom_start(Cpu *c, const char *name, const float pos[3]) {
    for (int k = 0; k < BOOMS_MAX; k++) {
        if (g_boom[k].used) continue;
        memset(&g_boom[k], 0, sizeof g_boom[k]);
        g_boom[k].h = new_handle();
        if (!request(c, name, g_boom[k].h)) return;
        g_boom[k].used = true;
        g_boom[k].quiet = !strcmp(name, "OnlineKnock") || !strncmp(name, "OnlineShock", 11);
        memcpy(g_boom[k].pos, pos, sizeof g_boom[k].pos);
        return;
    }
}

static void booms_forget(void) { memset(g_boom, 0, sizeof g_boom); }

static void booms_update(Cpu *c, uint64_t player) {
    uint64_t mgr = VALID(player) ? *(uint64_t *)(uintptr_t)(player + 0x2500) : 0;
    for (int k = 0; k < BOOMS_MAX; k++) {
        if (!g_boom[k].used) continue;
        if (!g_boom[k].actor) {
            int *unit = (int *)(uintptr_t)g_boom[k].h->unit;
            if (!unit) continue;
            if (*unit == 3 || *unit == 5) {
                call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)g_boom[k].h, 0, 0, 0, 0, 0);
                g_boom[k].used = false;
                continue;
            }
            if (*unit != 2) continue;
            g_boom[k].actor = call(c, FN_HANDLE_TAKE, (uint64_t)(uintptr_t)g_boom[k].h, 0, 0, 0, 0, 0);
            if (!g_boom[k].actor) { g_boom[k].used = false; continue; }
            g_boom[k].w = watch(g_boom[k].actor);
            // as the rune does with its bombs (0x7000b3ba04): its xlink holder (+0x558) +0x148 = 5 (without, its
            // blast is only a small ring)
            uint64_t xl = *(uint64_t *)(uintptr_t)(g_boom[k].actor + 0x558);
            if (VALID(xl)) *(uint32_t *)(uintptr_t)(xl + 0x148) = 5;
            float m[12] = {1, 0, 0, g_boom[k].pos[0], 0, 1, 0, g_boom[k].pos[1], 0, 0, 1, g_boom[k].pos[2]};
            call(c, FN_SET_MTX, g_boom[k].actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
            if (g_boom[k].quiet) model_visible(c, g_boom[k].actor, false);
        }
        if (!alive(g_boom[k].actor, g_boom[k].w)) {  // (gone off: deleted)
            if (getenv("SAVAGEWIND_BOOM_LOG")) LOG("[online] boom gone after %d frames\n", g_boom[k].frames);
            g_boom[k].used = false;
            continue;
        }
        int f = g_boom[k].frames++;
        if (getenv("SAVAGEWIND_BOOM_LOG")) {
            const float *m = (const float *)(uintptr_t)(g_boom[k].actor + 0x398);
            LOG("[online] boom f%d st %d AS %s at %.2f %.2f %.2f scale %.2f\n", f, *(uint8_t *)(uintptr_t)(g_boom[k].actor + 0x64),
                playing(c, g_boom[k].actor, 0, 0), m[3], m[7], m[11], m[0]);
        }
        if (f > 600) {  // (never went off)
            call(c, FN_DELETE, g_boom[k].actor, 0, 0, 0, 0, 0);
            g_boom[k].used = false;
            continue;
        }
        if (VALID(mgr) && running(g_boom[k].actor) && f % 15 == 1) {
            uint64_t rx = *(uint64_t *)(uintptr_t)(g_boom[k].actor + 0x5f0);
            uint64_t msg = 0x800000a;
            call(c, FN_SEND_MSG, mgr + 0xd8, rx, (uint64_t)(uintptr_t)&msg, 0, 1, 0);
            if (f == 1 || getenv("SAVAGEWIND_ONLINE_SHOT_LOG")) LOG("[online] boom %llx: go off (frame %d)\n", (unsigned long long)g_boom[k].actor, f);
        }
    }
}

// What the others hold with the Magnesis rune: the same object here (the one of that name nearest where it was when he
// caught it) where it is in his game, each frame while he holds it (then let go there: it falls as it does here), with
// the rune's glow on it ("MagneCatchMoving", the player's own effect: 0x7000b998fc plays it on the player with the
// held actor's place, effect +0x98 / +0x9a bit 8 and +0xbc..c4)
#define P_MAGNE 0x7001e15f18ull
#define FN_XLINK_EFFECT 0x7001395354ull  // x8 out {effect, id}, x0 an actor, x1 a keyword of its XLink, w2 0
#define P_MAGNE_FX 0x7001d66ab8ull       // (**this +0x30: the actor the rune's effects are played on)
#define FN_ACTOR_LOOK_ON 0x700152a08cull   // x0 an actor, w1 bits of its +0x710 to set (how it's drawn: 1 the
#define FN_ACTOR_LOOK_OFF 0x700152a0b4ull  // Magnesis rune's aim on it, 2 held by it: 0x7000b98c40), and to clear
#define FN_SPECIAL_FX_B 0x700124d220ull    // s0 a value, x1 a model: its materials' uk_special_effect_B
// The map the local Link is on: the game's own (0x7001208304: its type, 0x70012082f8: its name; each a SafeString,
// its chars at +8): "MainField/", "CDungeon/Dungeon000", "MainFieldDungeon/RemainsWind", ...
#define FN_MAP_TYPE 0x7001208304ull
#define FN_MAP_NAME 0x70012082f8ull
static char g_map[32];
static void local_map(Cpu *c, char out[32]) {
    uint64_t t = call(c, FN_MAP_TYPE, 0, 0, 0, 0, 0, 0), n = call(c, FN_MAP_NAME, 0, 0, 0, 0, 0, 0);
    const char *ts = VALID(t) ? *(const char **)(uintptr_t)(t + 8) : NULL, *ns = VALID(n) ? *(const char **)(uintptr_t)(n + 8) : NULL;
    char m[32];
    snprintf(m, sizeof m, "%s/%s", ts && VALID((uint64_t)(uintptr_t)ts) ? ts : "", ns && VALID((uint64_t)(uintptr_t)ns) && strcmp(ts ? ts : "", "MainField") ? ns : "");
    if (strcmp(m, g_map)) LOG("[online] map: %s\n", m), snprintf(g_map, sizeof g_map, "%s", m);
    memcpy(out, g_map, 32);
}
// whether another player is on our map (unknown: as on ours)
static bool same_map(const OnlinePlayer *p) {
    return !p->pose.map[0] || !g_map[0] || !strcmp(p->pose.map, g_map);
}

#define MAX_ACTORS 8192
static uint64_t g_actors[MAX_ACTORS];
static int all_actors(uint64_t from, uint64_t *out, int max);
static void boss_sample(Cpu *c, uint64_t player, OnlinePose *pose);

#define FN_BODY_TO 0x70012d4e18ull       // x0 a rigid body, x1 a Matrix34f: its velocities set to reach it in a step
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
    if (strncmp(pose.as[0], "Para", 4) || !strcmp(pose.as[0], "ParaEquipOff")) gear[5] = 0;  // (his paraglider: open)
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
        // (farther from every bone: thrown, dropped; not shown. The paraglider is over his head, a shield he surfs
        // on under his feet)
        float reach = k == 5 ? 3.0f : k == 1 ? 1.5f : 0.6f;
        float best = reach * reach, kept = -1;
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
        if (kept >= 0 && kept < reach * reach && kept < best * 4) pose.gear_bone[k] = last_bone[k];
        if (pose.gear_bone[k]) memcpy(pose.gear_rel[k], rel_of[pose.gear_bone[k] - 1], sizeof pose.gear_rel[k]);
        last_bone[k] = pose.gear_bone[k];
    }
    pose.valid = pose.as[0][0] != 0;
    {   // at a Sheikah pedestal: the Sheikah Slate's (Demo700_0, its first door Demo701_0) or a tower's (Demo004_0: the map
        // downloaded; Demo164_0: the first tower rising): the others near can't start it meanwhile (not a chest, a talk...)
        extern bool framepace_in_event(void);
        extern const char *framepace_event_name(void);
        static const char *const pedestals[] = {"Obj_DRStone_A_01", "Demo700_0", "Demo701_0", "Demo004_0", "Demo164_0"};
        const char *ev = framepace_in_event() ? framepace_event_name() : "";
        for (int k = 0; k < 5; k++)
            if (!strcmp(ev, pedestals[k])) pose.effects |= ONLINE_E_DEMO;
    }
    {   // Daruk's Protection: the player's own effects of it (1.9.0 0x7100cd39fc): "Kago_Shield" (a hit taken on it) at
        // player +0x2350, "Kago_ShieldCount_0<n>" (its ring while he guards, n charges left: player +0x1cd4) at +0x2360;
        // each a handle (the effect, its id: alive while the effect's +0x20 is that id)
        for (int k = 0; k < 2; k++) {
            uint64_t h = *(uint64_t *)(uintptr_t)(player + 0x2350 + 0x10 * k);
            if (!VALID(h) || *(uint32_t *)(uintptr_t)(h + 0x20) != *(uint32_t *)(uintptr_t)(player + 0x2358 + 0x10 * k)) continue;
            int n = *(int *)(uintptr_t)(player + 0x1cd4);
            if (k == 0) pose.effects |= ONLINE_E_KAGO;
            else if (n >= 1 && n <= 3) pose.effects |= (uint8_t)(n << ONLINE_E_KAGO_COUNT_SHIFT);
        }
    }
    {   // the Magnesis rune: what he holds (1.9.0: the rune's object at 0x7001e15f18, its +0x318 the magnetic part of
        // the actor held, 0x7000b993e8 checks it so; that part on while its +0x10 bit 0; its actor at +8)
        static uint64_t held;
        static float from[3];
        uint64_t g = *(uint64_t *)(uintptr_t)P_MAGNE;
        uint64_t part = VALID(g) ? *(uint64_t *)(uintptr_t)(g + 0x318) : 0;
        uint64_t a = VALID(part) && (*(uint32_t *)(uintptr_t)(part + 0x10) & 1) ? *(uint64_t *)(uintptr_t)(part + 8) : 0;
        if (!VALID(a)) a = 0;
        const float *am = a ? (const float *)(uintptr_t)(a + 0x398) : NULL;
        if (a != held) {
            held = a;
            if (a) from[0] = am[3], from[1] = am[7], from[2] = am[11];
            char n[48] = "";
            if (a) actor_name_of(a, n);
            LOG("[online] magnesis: %s\n", a ? n : "let go");
        }
        if (a) {
            char n[48];
            actor_name_of(a, n);
            snprintf(pose.grab.name, sizeof pose.grab.name, "%s", n);
            memcpy(pose.grab.from, from, sizeof from);
            memcpy(pose.grab.m, am, sizeof pose.grab.m);
        }
    }
    {   // the Stasis rune: what he stopped (an actor near him drawn as stopped: its +0x710 bit 3, as the rune sets it;
        // bit 4 while he aims at it)
        static uint64_t stopped;
        static Watch sw;
        static float from[3];
        static int tick;
        if (stopped && (!alive(stopped, sw) || !(*(uint32_t *)(uintptr_t)(stopped + 0x710) & 8))) {
            LOG("[online] stasis: over\n");
            stopped = 0;
        }
        if (!stopped && tick++ % 6 == 0) {
            int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
            const float *pm = (const float *)(uintptr_t)(player + 0x398);
            for (int k = 0; k < na && !stopped; k++) {
                uint64_t a = g_actors[k];
                if (a == player || !(*(uint32_t *)(uintptr_t)(a + 0x710) & 8) || !VALID(*(uint64_t *)(uintptr_t)(a + 0x4e0)) ||
                    !VALID(*(uint64_t *)(uintptr_t)(a + 0x190))) continue;  // (a model and a body: no tag)
                bool theirs = false;  // (one another player stopped, shown here: not ours)
                for (int i = 0; i < ONLINE_MAX && !theirs; i++) theirs = g_act[i].stop == a;
                if (theirs) continue;
                const float *m = (const float *)(uintptr_t)(a + 0x398);
                if ((m[3] - pm[3]) * (m[3] - pm[3]) + (m[11] - pm[11]) * (m[11] - pm[11]) > 60 * 60) continue;
                stopped = a, sw = watch(a), from[0] = m[3], from[1] = m[7], from[2] = m[11];
                char n[48];
                actor_name_of(a, n);
                LOG("[online] stasis: %s\n", n);
            }
        }
        if (stopped) {
            char n[48];
            actor_name_of(stopped, n);
            snprintf(pose.stop.name, sizeof pose.stop.name, "%s", n);
            memcpy(pose.stop.from, from, sizeof from);
            memcpy(pose.stop.m, (const float *)(uintptr_t)(stopped + 0x398), sizeof pose.stop.m);
        }
    }
    {   // the Cryonis rune: his pillars (Obj_IceMakerBlock near him; not those we made for the others)
        static float last[3][12];
        static int n_last, tick;
        if (tick++ % 6 == 0) {
            n_last = 0;
            int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
            const float *pm = (const float *)(uintptr_t)(player + 0x398);
            for (int k = 0; k < na && n_last < 3; k++) {
                uint64_t a = g_actors[k];
                char n[48];
                actor_name_of(a, n);
                if (strcmp(n, "Obj_IceMakerBlock")) continue;
                bool theirs = false;
                for (int i = 0; i < ONLINE_MAX && !theirs; i++)
                    for (int q = 0; q < 3 && !theirs; q++) theirs = g_act[i].ice[q].p.actor == a;
                if (theirs) continue;
                const float *m = (const float *)(uintptr_t)(a + 0x398);
                if ((m[3] - pm[3]) * (m[3] - pm[3]) + (m[11] - pm[11]) * (m[11] - pm[11]) > 100 * 100) continue;
                memcpy(last[n_last++], m, sizeof last[0]);
            }
        }
        pose.ice_n = (uint8_t)n_last;
        memcpy(pose.ice, last, sizeof pose.ice);
    }
    local_map(c, pose.map);
    boss_sample(c, player, &pose);
    g_local_dead = !strncmp(pose.as[0], "Dead", 4);
    sample_shots(c, player, &pose.shots);
    online_set_local_pose(&pose);
    if (getenv("SAVAGEWIND_ONLINE_POSE_LOG")) {
        static OnlinePose last;
        if (memcmp(last.as, pose.as, sizeof pose.as) || memcmp(last.gear_bone, pose.gear_bone, sizeof pose.gear_bone))
            LOG("[online] pose %s|%s|%s|%s gear %d %d %d %d %d %d\n", pose.as[0], pose.as[1], pose.as[2], pose.as[3],
                pose.gear_bone[0], pose.gear_bone[1], pose.gear_bone[2], pose.gear_bone[3], pose.gear_bone[4], pose.gear_bone[5]);
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
static float g_pose_q[ONLINE_BONES][4], g_pose_t[ONLINE_BONES][3];  // (the skeleton pose_link posed his Link by)
static int g_pose_n;  // (its Link bones; 0: none this frame)

static void pose_link(Cpu *c, int i, const OnlinePlayer *p) {
    float (*q)[4] = g_pose_q, (*t)[3] = g_pose_t;
    int n = 0, nbow = 0, nb = 0, nbb = 0;
    g_act[i].frames++;
    g_pose_n = 0;
    // (none lately: paused, in a menu - the last one he sent kept; his armour posed by it too)
    static struct { float q[ONLINE_BONES][4], t[ONLINE_BONES][3]; int n, nbow; } last[ONLINE_MAX];
    bool fresh = online_skeleton(i, &n, &nbow, q, t);
    if (fresh) memcpy(last[i].q, q, sizeof last[i].q), memcpy(last[i].t, t, sizeof last[i].t), last[i].n = n, last[i].nbow = nbow;
    else if (g_act[i].skeleton && last[i].n)
        memcpy(q, last[i].q, sizeof last[i].q), memcpy(t, last[i].t, sizeof last[i].t), n = last[i].n, nbow = last[i].nbow;
    if (fresh || (g_act[i].skeleton && last[i].n)) {
        g_pose_n = n;
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
    // (his armour posed as his Link stands then: his own bones, as his AS left them)
    uint64_t unit = skel_unit(c, g_act[i].link, &nb);
    if (!unit || nb <= 0) return;
    if (nb > ONLINE_BONES) nb = ONLINE_BONES;
    for (int b = 0; b < nb; b++) {
        float m[12], sc[3];
        call(c, FN_BONE_LOCAL_GET, unit, (uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)sc, b, 0, 0);
        mtx_to_quat(m, q[b]);
        t[b][0] = m[3], t[b][1] = m[7], t[b][2] = m[11];
    }
    g_pose_n = nb;
}

// an armour piece posed as his Link: each of its bones Link has, as Link's (its cloth and support bones follow them)
static void pose_armor(Cpu *c, const Piece *p) {
    if (!g_pose_n) return;
    uint64_t model = *(uint64_t *)(uintptr_t)(p->actor + 0x4e0);
    if (!VALID(model)) return;
    int nu = *(int *)(uintptr_t)(model + 0x38);
    uint64_t units = *(uint64_t *)(uintptr_t)(model + 0x40);
    for (int ui = 0; VALID(units) && ui < nu && ui < 8; ui++) {
        uint64_t unit = *(uint64_t *)(uintptr_t)(units + 8 * ui);
        unit = VALID(unit) ? *(uint64_t *)(uintptr_t)unit : 0;
        uint64_t sk = VALID(unit) ? *(uint64_t *)(uintptr_t)(unit + 0x118) : 0;
        if (!VALID(sk)) continue;
        int nb = *(uint16_t *)(uintptr_t)(sk + 0x50), len = 0;
        const uint8_t *map = armor_bones(p->name, ui, &len);
        for (int b = 0; map && b < len && b < nb; b++) {
            int li = map[b];
            if (li == 255 || li >= g_pose_n) continue;
            float m[12], sc[3] = {1, 1, 1};
            quat_to_mtx(g_pose_q[li], m);
            m[3] = g_pose_t[li][0], m[7] = g_pose_t[li][1], m[11] = g_pose_t[li][2];
            call(c, FN_BONE_LOCAL_SET, unit, (uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)sc, b, 0, 0);
        }
    }
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

// ---- The host's enemies (protocol 11, ENEMIES): his game sends those near any player, each by its id, as it is (its
// name, matrix, the AS it plays); the others delete their own near his Link and show his with OnlineEnemy_* actors
// (tools/online_mod.py: an enemy's model, AS list and AS, no AI), each where his is, playing what his plays. When his
// feed stops (he left, the connection went), they stay as they were until it comes again.

// online_mod/enemies.txt: each enemy's online actor ("Enemy_...=OnlineEnemy_...")
static const char *enemy_actor(const char *name) {
    static MapLine *m;
    static int n = -1;
    if (n < 0) n = map_load("enemies.txt", &m), LOG("[online] %d enemies for the other players\n", n);
    return map_find(m, n, name);
}

// Every actor the game has, from one actor's proc unit (actor +0x68): the units are in one list (0x58 each: +0 their
// vtable, +0x10 / +0x18 the previous / next one's +0x10; +0x40 the actor's id, +0x48 the actor), its head no unit
static int all_actors(uint64_t from, uint64_t *out, int max) {
    if (!VALID(from)) return 0;
    uint64_t vt = *(uint64_t *)(uintptr_t)from;
    int n = 0;
    for (int dir = 0; dir < 2; dir++) {
        uint64_t u = from;
        for (int k = 0; k < 16384 && n < max; k++) {
            if (dir == 1 || k > 0) {
                uint64_t a = *(uint64_t *)(uintptr_t)(u + 0x48);
                if (VALID(a) && *(uint32_t *)(uintptr_t)(a + 0x60) == *(uint32_t *)(uintptr_t)(u + 0x40)) out[n++] = a;
            }
            uint64_t nx = *(uint64_t *)(uintptr_t)(u + (dir ? 0x10 : 0x18));
            if (!VALID(nx) || nx - 0x10 == from) break;
            u = nx - 0x10;
            if (*(uint64_t *)(uintptr_t)u != vt) break;
        }
        if (dir == 0 && n) n--;  // (the first one: again going back)
    }
    return n;
}

// the bosses: each game keeps its own (their fights are the game's events and phases); their life is everyone's
// (boss_update)
static bool is_boss(const char *n) {
    return !strcmp(n, "Enemy_Ganon") || !strcmp(n, "Enemy_GanonBeast") || !strncmp(n, "Enemy_SiteBoss", 14);
}

static bool is_enemy(uint64_t a, char name[48]) {
    actor_name_of(a, name);
    return !strncmp(name, "Enemy_", 6) && !is_boss(name) && enemy_actor(name);
}

// what an enemy holds: a weapon actor it links (the link's place in an enemy found once, by looking through one
// holding a Weapon_*; again every second while none is found)
static uint64_t enemy_weapon(uint64_t a) {
    static int off = -1, miss;
    char n[48];
    if (off >= 0) {
        uint64_t w = linked(a + off);
        actor_name_of(w, n);
        return !strncmp(n, "Weapon_", 7) ? w : 0;
    }
    if (miss++ % 30) return 0;
    for (int o = 0x100; o < 0x4000; o += 8) {
        uint64_t u = *(uint64_t *)(uintptr_t)(a + o);
        if (!VALID(u) || (u & 7)) continue;
        uint64_t w = linked(a + o);
        actor_name_of(w, n);
        if (strncmp(n, "Weapon_", 7)) continue;
        off = o;
        LOG("[online] an enemy's weapon: enemy +0x%x\n", o);
        return w;
    }
    return 0;
}

#define ENEMY_LIFE 0x848   // (an actor's life, s32: its General Life to start with; seen going down as it is hit)
#define ENEMY_NEAR 150.0f   // (the host sends his enemies this near a player)
#define ENEMY_AREA 170.0f   // (the others' own ones this near his Link go)

// what an enemy holds (its weapon at +0xb98, its shield at +0xbb0: linked as the player links his), for the others:
// in the space of the hand bone it is in (each game shows its enemy playing its own AS: the weapon goes in its own
// hand there), else where it is in the world (on its back, on the ground near it)
static const char *const k_hands[2] = {"Weapon_R", "Weapon_L"};
static void enemy_held(Cpu *c, uint64_t a, uint64_t g, char name[40], float m[12], uint8_t *bone) {
    char n[48];
    actor_name_of(g, n);
    name[0] = 0, *bone = 0;
    if (strncmp(n, "Weapon_", 7)) return;
    const float *w = (const float *)(uintptr_t)(g + 0x398), *am = (const float *)(uintptr_t)(a + 0x398);
    float dx = w[3] - am[3], dy = w[7] - am[7], dz = w[11] - am[11];
    if (dx * dx + dy * dy + dz * dz > 4 * 4) return;  // (fallen far from it: not its own any more)
    snprintf(name, 40, "%s", n);
    memcpy(m, w, 48);
    for (int h = 0; h < 2; h++) {
        float bm[12], inv[12], rel[12];
        if (!bone_mtx(c, a, k_hands[h], bm)) continue;
        mtx_inv(bm, inv);
        mtx_mul(inv, w, rel);
        if (rel[3] * rel[3] + rel[7] * rel[7] + rel[11] * rel[11] > 0.6f * 0.6f) continue;
        memcpy(m, rel, 48), *bone = (uint8_t)(h + 1);
        return;
    }
}

// an enemy as it is here, for the others
static void enemy_state(Cpu *c, uint64_t a, OnlineEnemy *e) {
    actor_name_of(a, e->name);
    memcpy(e->m, (const float *)(uintptr_t)(a + 0x398), sizeof e->m);
    snprintf(e->as, sizeof e->as, "%s", playing(c, a, 0, 0));
    e->life = *(int32_t *)(uintptr_t)(a + ENEMY_LIFE);
    uint32_t h = *(uint32_t *)(uintptr_t)(a + ACTOR_HASH_ID);  // (one made by us has none: the one it was keeps its)
    if (h) e->hash = h;
    enemy_held(c, a, linked(a + 0xb98), e->weapon, e->wm, &e->wbone);
    enemy_held(c, a, linked(a + 0xbb0), e->shield, e->sm, &e->sbone);
}

// Each of the host's enemies is had for real by one game, its owner's: the player nearest it (another one only when
// he is clearly nearer, and not again for 3 s), whose game runs its AI (it sees him, chases him, is hit as any enemy);
// the others show it (OnlineEnemy_*). The owner's game makes it when it comes to have it (from the state it had),
// and sends its state to the host's game (ESTATE); the one that no longer has it deletes it. The enemies the host's
// game has itself are found there; their ids are everyone's
typedef struct {
    uint32_t gid; int state; uint64_t actor; Watch w; Piece made; InstParams params;
    bool died; float last_pos[3];  // (loot: died here, where it was)
    uint64_t held[2]; Watch held_w[2];  // (loot: its weapon and shield while it lived: they fall when it dies)
} Own;  // (state: OWN_*)
enum { OWN_NONE, OWN_MAKING, OWN_HAVE };
#define OWNS 64
static Own g_own[OWNS];
static struct { uint64_t actor; Watch w; } g_own_left[OWNS];

static Own *own_find(uint32_t gid) {
    for (int k = 0; k < OWNS; k++)
        if (g_own[k].state && g_own[k].gid == gid) return &g_own[k];
    return NULL;
}

static void enemy_delete(Cpu *c, uint64_t a);

static void owns_left(Cpu *c) {
    for (int k = 0; k < OWNS; k++) {
        if (g_own_left[k].actor && alive(g_own_left[k].actor, g_own_left[k].w)) enemy_delete(c, g_own_left[k].actor);
        g_own_left[k].actor = 0;
    }
}

static bool own_is(uint64_t a) {
    for (int k = 0; k < OWNS; k++)
        if (g_own[k].state == OWN_HAVE && g_own[k].actor == a) return true;
    return false;
}

static Own *own_add(uint32_t gid) {
    for (int k = 0; k < OWNS; k++)
        if (!g_own[k].state) {
            memset(&g_own[k], 0, sizeof g_own[k]);
            g_own[k].gid = gid, g_own[k].state = OWN_MAKING;
            return &g_own[k];
        }
    return NULL;
}

// an enemy deleted with what it holds (else its weapon stays on the ground: one more each time it changes hands)
static void enemy_delete(Cpu *c, uint64_t a) {
    uint64_t w = enemy_weapon(a);
    if (w) call(c, FN_DELETE, w, 0, 0, 0, 0, 0);
    for (int k = 1; k < 3; k++) {  // (its shield and bow: linked as the player links his, after the weapon)
        char n[48];
        uint64_t g2 = linked(a + 0xb98 + 0x18 * k);
        actor_name_of(g2, n);
        if (!strncmp(n, "Weapon_", 7)) call(c, FN_DELETE, g2, 0, 0, 0, 0, 0);
    }
    call(c, FN_DELETE, a, 0, 0, 0, 0, 0);
}

static void own_drop(Cpu *c, Own *o) {
    if (o->state == OWN_HAVE && alive(o->actor, o->w)) enemy_delete(c, o->actor);
    delete_piece(c, &o->made);
    memset(o, 0, sizeof *o);
}

// ours, made when it came to us: as it was (where, its life); false while it is being made
static bool own_ready(Cpu *c, Own *o, const OnlineEnemy *e) {
    if (o->state == OWN_HAVE) return true;
    if (!o->made.pending && !o->made.actor && !o->made.name[0]) {  // (asked for with the weapon it holds)
        memset(&o->params, 0, sizeof o->params);
        if (e->weapon[0]) inst_params_string(&o->params, "EquipItem1", e->weapon);
        if (e->shield[0]) inst_params_string(&o->params, "EquipItem2", e->shield);
        o->made.handle = new_handle();
        if (request_p(c, e->name, o->made.handle, o->params.n ? (uint64_t)(uintptr_t)&o->params : 0)) {
            o->made.pending = true;
            snprintf(o->made.name, sizeof o->made.name, "%s", e->name);
        }
    }
    piece_update(c, 0, &o->made, e->name, "own");
    if (!o->made.actor) return false;
    o->actor = o->made.actor, o->w = o->made.w, o->state = OWN_HAVE;
    memset(&o->made, 0, sizeof o->made);
    call(c, FN_SET_MTX, o->actor, (uint64_t)(uintptr_t)e->m, 0, 0, 0, 0);
    if (e->life > 0) *(int32_t *)(uintptr_t)(o->actor + ENEMY_LIFE) = e->life;
    LOG("[online] we have %s %u for real now\n", e->name, o->gid);
    return true;
}

// the others' hits on ours: their life goes down by it (at 0 its own AI has it die)
static void own_hits(void) {
    uint32_t hid[64];
    int32_t hd[64];
    int nh = online_take_hits(hid, hd, 64);
    for (int h = 0; h < nh; h++) {
        Own *o = own_find(hid[h]);
        if (!o || o->state != OWN_HAVE || !alive(o->actor, o->w)) continue;
        int32_t *life = (int32_t *)(uintptr_t)(o->actor + ENEMY_LIFE);
        LOG("[online] a hit on our %u: %d of its %d\n", hid[h], hd[h], *life);
        *life = *life > hd[h] ? *life - hd[h] : 0;
    }
}

// the host's game: every enemy of everyone's, its state and its owner (slot)
#define REG 64
static struct { OnlineEnemy e; int hold, nearer; bool used; } g_reg[REG];  // (nearer: ticks another has been much nearer)
static OnlineEnemy g_reg_out[ONLINE_ENEMIES];  // (the list sent last: the others' are shown here from it each frame)
static int g_reg_n;

// (a load: the game removed them all; when it didn't (a long jump that was no load), those still there are deleted
// on the next frame, not left in the world for no one)
static void owns_forget(void) {
    for (int k = 0; k < OWNS; k++) {
        orphan(&g_own[k].made);
        if (g_own[k].state == OWN_HAVE) g_own_left[k].actor = g_own[k].actor, g_own_left[k].w = g_own[k].w;
    }
    memset(g_own, 0, sizeof g_own);
    memset(g_reg, 0, sizeof g_reg);
    g_reg_n = 0;
}

static void enemies_show(Cpu *c, uint64_t player, OnlineEnemy *e, int n, bool fresh, const float centre[3], int me,
                         bool suppress);

// An enemy that died in another game: its dead flag here (<map>_<actor>_<HashId>, the flag the game sets when one of
// its map's enemies dies: the game doesn't make it again until a Blood Moon, and what waits for its group to be
// beaten, a camp's chest, sees it beaten)
static uint32_t crc32_str(const char *s);
static uint32_t *bool_flag(uint32_t h);
static bool set_bool_flag(Cpu *c, uint32_t h);
static void mark_dead(Cpu *c, const char *name, uint32_t hash) {
    if (!hash || !name[0]) return;
    static const char *const maps[] = {"MainField", "AocField", "MainFieldDungeon", "CDungeon"};
    for (int k = 0; k < 4; k++) {
        char f[128];
        snprintf(f, sizeof f, "%s_%s_%u", maps[k], name, hash);
        uint32_t h = crc32_str(f);
        uint32_t *v = bool_flag(h);
        if (!v) continue;
        if (!(*v & 1) && !set_bool_flag(c, h)) *v |= 1;
        LOG("[online] %s: dead here too\n", f);
        return;
    }
    LOG("[online] %s %u: no dead flag\n", name, hash);
}

// whether another player's game may have enemies for real: in the world (not loading), his Link there and not dead
// (dead, his game stops at its game over, and what it has stops with it; trying again, his world goes)
static bool can_own(const OnlinePlayer *p, int i, int me) {
    return p->used && i != me && !(p->flags & ONLINE_F_LOADING) && p->pose.valid && strncmp(p->pose.as[0], "Dead", 4) &&
           same_map(p);
}

static void enemies_host(Cpu *c, uint64_t player, const float link[3], const OnlinePlayer *pl, int me) {
    own_hits();
    // the owners' states of theirs
    static OnlineEnemy st[64];
    uint8_t from[64];
    int ns = online_take_estates(st, from, 64);
    for (int k = 0; k < ns; k++)
        for (int r = 0; r < REG; r++) {
            if (!g_reg[r].used || g_reg[r].e.id != st[k].id || g_reg[r].e.owner != from[k]) continue;
            if (st[k].life < 0) {  // (gone in his game: died)
                LOG("[online] %s %u gone in slot %d's game\n", g_reg[r].e.name, st[k].id, from[k]);
                if (g_reg[r].e.life <= 0) {  // (it had died there)
                    mark_dead(c, g_reg[r].e.name, g_reg[r].e.hash);
                    g_reg[r].used = false;
                } else {  // (alive, gone with his world: he died and tries again, or loads elsewhere): ours again
                    LOG("[online] %s %u: alive, ours again\n", g_reg[r].e.name, st[k].id);
                    g_reg[r].e.owner = (uint8_t)me, g_reg[r].hold = 100, g_reg[r].nearer = 0;
                    if (!own_find(st[k].id)) own_add(st[k].id);
                }
            } else {
                uint8_t owner = g_reg[r].e.owner;
                uint32_t hash = g_reg[r].e.hash;
                g_reg[r].e = st[k], g_reg[r].e.owner = owner;
                if (!g_reg[r].e.hash) g_reg[r].e.hash = hash;
            }
            break;
        }
    {   // (test: SAVAGEWIND_ONLINE_KILL_TEST=<s>: ours die that many seconds in, all at once)
        static int t;
        const char *kt = getenv("SAVAGEWIND_ONLINE_KILL_TEST");
        if (kt && ++t == atoi(kt) * 10)
            for (int k = 0; k < OWNS; k++)
                if (g_own[k].state == OWN_HAVE && alive(g_own[k].actor, g_own[k].w))
                    *(int32_t *)(uintptr_t)(g_own[k].actor + ENEMY_LIFE) = 0, LOG("[online] kill test: %u\n", g_own[k].gid);
    }
    // ours: their state; gone (died, or the game removed them): gone for everyone
    for (int k = 0; k < OWNS; k++) {
        Own *o = &g_own[k];
        if (o->state != OWN_HAVE) continue;
        int r = 0;
        while (r < REG && !(g_reg[r].used && g_reg[r].e.id == o->gid)) r++;
        if (!alive(o->actor, o->w)) {
            if (r < REG) g_reg[r].used = false;
            memset(o, 0, sizeof *o);
            continue;
        }
        if (r < REG) enemy_state(c, o->actor, &g_reg[r].e), g_reg[r].e.owner = (uint8_t)me;
    }
    // ours in the world, newly near a player: everyone's from now on (ours to start with)
    int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
    for (int k = 0; k < na; k++) {
        char name[48];
        uint64_t a = g_actors[k];
        if (!running(a) || !is_enemy(a, name) || own_is(a)) continue;
        const float *m = (const float *)(uintptr_t)(a + 0x398);
        bool near = false;
        for (int i = -1; i < ONLINE_MAX && !near; i++) {
            const float *q = i < 0 ? link : pl[i].pos;
            if (i >= 0 && (!pl[i].used || i == me || (pl[i].flags & ONLINE_F_LOADING) || !same_map(&pl[i]))) continue;
            float dx = m[3] - q[0], dz = m[11] - q[2];
            near = dx * dx + dz * dz < ENEMY_NEAR * ENEMY_NEAR;
        }
        uint32_t id = *(uint32_t *)(uintptr_t)(a + 0x60);
        if (!near) continue;
        int r = 0, free_r = -1;
        for (; r < REG && !(g_reg[r].used && g_reg[r].e.id == id); r++)
            if (!g_reg[r].used && free_r < 0) free_r = r;
        if (r < REG || free_r < 0) continue;
        Own *o = own_add(id);
        if (!o) continue;
        o->state = OWN_HAVE, o->actor = a, o->w = watch(a);
        memset(&g_reg[free_r], 0, sizeof g_reg[free_r]);
        g_reg[free_r].used = true;
        g_reg[free_r].e.id = id;
        enemy_state(c, a, &g_reg[free_r].e);
        g_reg[free_r].e.owner = (uint8_t)me;
    }
    // owners: the player nearest each one (another only when he is clearly nearer, and not again for 3 s)
    for (int r = 0; r < REG; r++) {
        if (!g_reg[r].used) continue;
        OnlineEnemy *e = &g_reg[r].e;
        float best = 1e30f, cur = 1e30f;
        int nearest = me;
        for (int i = -1; i < ONLINE_MAX; i++) {
            int slot = i < 0 ? me : i;
            const float *q = i < 0 ? link : pl[i].pos;
            if (i >= 0 && !can_own(&pl[i], i, me)) continue;
            float dx = e->m[3] - q[0], dz = e->m[11] - q[2], d = sqrtf(dx * dx + dz * dz);
            if (i < 0 && g_local_dead) d += 1e6f;  // (we're dead: the others' first)
            if (d < best) best = d, nearest = slot;
            if (slot == e->owner) cur = d;
        }
        if (e->owner == me && g_local_dead && best < 1e6f) cur = 1e30f;  // (we died: another has it now)
        if (g_reg[r].hold > 0) g_reg[r].hold--;
        bool gone_owner = cur >= 1e30f;  // (he left, or is loading)
        // (each change makes it anew in one game and deletes it in another: only when its owner is far from it (40 m),
        // or another has been 15 m nearer for 5 s, and not again for 10 s; players fighting it together keep it where
        // it is)
        g_reg[r].nearer = nearest != e->owner && best + 15 < cur ? g_reg[r].nearer + 1 : 0;
        bool far = cur > 40 || g_reg[r].nearer > 50;
        if (nearest == e->owner || (!gone_owner && (g_reg[r].hold > 0 || !far))) continue;
        LOG("[online] %s %u: slot %d's now (was %d's)\n", e->name, e->id, nearest, e->owner);
        if (e->owner == me) {  // (ours no more: it goes here, its state stays for the next one)
            Own *o = own_find(e->id);
            if (o) own_drop(c, o);
        }
        if (nearest == me) own_add(e->id);
        e->owner = (uint8_t)nearest;
        g_reg[r].hold = 100, g_reg[r].nearer = 0;  // (10 s: this runs every 3 frames)
    }
    // ours being made: as they were
    for (int r = 0; r < REG; r++) {
        if (!g_reg[r].used || g_reg[r].e.owner != me) continue;
        Own *o = own_find(g_reg[r].e.id);
        if (o && o->state == OWN_MAKING) own_ready(c, o, &g_reg[r].e);
    }
    // everyone's
    int n = 0;
    for (int r = 0; r < REG && n < ONLINE_ENEMIES; r++)
        if (g_reg[r].used) g_reg_out[n++] = g_reg[r].e;
    g_reg_n = n;
    online_set_local_enemies(link, g_reg_out, n);
}

// the others: his enemies' actors here
#define PUPPETS 64
static struct {
    uint32_t id;
    Piece p, w, sh;  // (the enemy, its weapon, its shield)
    char as[32];   // (the AS it plays: the host's one, once it plays)
    int tries;
    int32_t life;  // (its life as set: what it lost since is a hit)
    char seen[32];  // (the host's last AS for it: a new attack starts)
    int strike;     // (frames until its attack lands, 0 none)
    int dying;      // (frames left of its death here: it died in its owner's game; 0 alive)
    struct { uint64_t p; uint32_t id, pad; } fx;  // (its death's effect)
} g_puppet[PUPPETS];

// the scale an enemy holds a weapon at (online_mod/wscale.txt: its gparam's WeaponScale* per kind; 1 when not there)
static float weapon_scale(const char *enemy, const char *weapon) {
    static MapLine *m;
    static int n = -1;
    if (n < 0) n = map_load("wscale.txt", &m);
    const char *v = map_find(m, n, enemy);
    float s[5] = {1, 1, 1, 1, 1};
    if (!v || sscanf(v, "%f,%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3], &s[4]) != 5) return 1;
    return strstr(weapon, "_Lsword_") ? s[1] : strstr(weapon, "_Spear_") ? s[2] : strstr(weapon, "_Bow_") ? s[3]
         : strstr(weapon, "_Shield_") ? s[4] : s[0];
}

// an enemy's or a weapon's attack power (online_mod/power.txt), 0 unknown
static int attack_power(const char *name) {
    static MapLine *m;
    static int n = -1;
    if (n < 0) n = map_load("power.txt", &m);
    const char *v = map_find(m, n, name);
    return v ? atoi(v) : 0;
}

// The host's enemies hitting the local Link: his enemy starting an attack (an AS of its with Attack or Kick in its name)
// lands STRIKE_FRAMES later on the local Link when he is within its reach in front of it, his shield not up: his life
// (player +0x848, quarter hearts) goes down by its weapon's attack power, its own when it has none
#define STRIKE_FRAMES 10
static void strike(Cpu *c, uint64_t player, const OnlineEnemy *e) {
    int32_t *life = (int32_t *)(uintptr_t)(player + ENEMY_LIFE);
    if (*life <= 0) return;
    const float *pm = (const float *)(uintptr_t)(player + 0x398);
    float dx = pm[3] - e->m[3], dy = pm[7] - e->m[7], dz = pm[11] - e->m[11];
    float d = sqrtf(dx * dx + dz * dz);
    float fx = e->m[2], fz = e->m[10], fl = sqrtf(fx * fx + fz * fz);
    bool big = strstr(e->name, "Moriblin") || strstr(e->name, "Lynel") || strstr(e->name, "Giant") || strstr(e->name, "Golem");
    float reach = (big ? 3.5f : 2.2f) + (strstr(e->weapon, "Spear") || strstr(e->weapon, "Lsword") ? 1.5f : e->weapon[0] ? 0.8f : 0);
    if (d > reach || dy > 2.5f || dy < -2.5f) return;
    if (d > 0.8f && fl > 0 && (dx * fx + dz * fz) / (d * fl) < 0.3f) return;  // (behind it, or to its side)
    const char *up = playing(c, player, 1, 0), *body = playing(c, player, 0, 0);
    if (strstr(up, "Guard") || strstr(body, "Guard")) {
        LOG("[online] the host's %s: blocked\n", e->name);
        return;
    }
    int power = e->weapon[0] ? attack_power(e->weapon) : 0;
    if (!power) power = attack_power(e->name);
    if (power <= 0) return;
    *life = *life > power ? *life - power : 0;
    // a heavy blow (a kick, a big or jumping attack, any of a big one's) shakes him: OnlineShock (an enemy's shock wave,
    // unseen) a metre from him on its side; he flinches (OnlineKnock, a bomb's blast, knocks him down, but its explosion
    // is seen: SAVAGEWIND_ONLINE_KNOCK=OnlineKnock)
    bool heavy = big || strstr(e->as, "Kick") || strstr(e->as, "Large") || strstr(e->as, "Jump");
    if (heavy && *life > 0 && d > 0.01f) {
        float at[3] = {pm[3] - dx / d, pm[7] + 0.3f, pm[11] - dz / d};
        boom_start(c, getenv("SAVAGEWIND_ONLINE_KNOCK") ? getenv("SAVAGEWIND_ONLINE_KNOCK") : "OnlineShock", at);
    }
    LOG("[online] the host's %s hit us: %d (life %d)%s\n", e->name, power, *life, heavy ? ", knocked down" : "");
}

static void puppets_forget(void) {
    for (int k = 0; k < PUPPETS; k++) orphan(&g_puppet[k].p), orphan(&g_puppet[k].w), orphan(&g_puppet[k].sh);
    memset(g_puppet, 0, sizeof g_puppet);
}

// a game not the host's: the host's enemies, each ours for real or shown
static void enemies_client(Cpu *c, uint64_t player, int me) {
    static OnlineEnemy e[ONLINE_ENEMIES];
    float centre[3];
    uint64_t age;
    int n = online_enemies(centre, e, &age);
    if (n < 0) return;  // (no host's enemies ever: his game is older, or none came)
    bool fresh = age < 1000000000ull;
    own_hits();
    if (fresh) {
        // ours: made when they come to us; no longer ours (or no longer anyone's): gone here
        for (int k = 0; k < n; k++)
            if (e[k].owner == me && me >= 0) {
                Own *o = own_find(e[k].id);
                if (!o) o = own_add(e[k].id);
                if (o) own_ready(c, o, &e[k]);
            }
        for (int k2 = 0; k2 < OWNS; k2++) {
            if (!g_own[k2].state) continue;
            bool mine = false;
            for (int k = 0; k < n && !mine; k++) mine = e[k].id == g_own[k2].gid && e[k].owner == me;
            if (!mine) LOG("[online] %u is not ours now\n", g_own[k2].gid), own_drop(c, &g_own[k2]);
        }
        // and their state, for the host's game (gone: died here)
        static int tick;
        if (tick++ % 3 == 0) {
            static OnlineEnemy st[ONLINE_ENEMIES];
            int ns = 0;
            for (int k2 = 0; k2 < OWNS && ns < ONLINE_ENEMIES; k2++) {
                Own *o = &g_own[k2];
                if (o->state != OWN_HAVE) continue;
                memset(&st[ns], 0, sizeof st[ns]);
                st[ns].id = o->gid;
                if (alive(o->actor, o->w)) enemy_state(c, o->actor, &st[ns]);
                else st[ns].life = -1, memset(o, 0, sizeof *o);
                ns++;
            }
            if (ns) online_send_estates(st, ns);
        }
    }
    enemies_show(c, player, e, n, fresh, centre, me, true);
}

static void enemies_show(Cpu *c, uint64_t player, OnlineEnemy *e, int n, bool fresh, const float centre[3], int me,
                         bool suppress) {
    // his area: our own enemies go (each frame: those the game makes there go as they come), but those we have for
    // everyone
    int na = suppress ? all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS) : 0, gone = 0;
    for (int k = 0; k < na; k++) {
        char name[48];
        uint64_t a = g_actors[k];
        if (!running(a) || !is_enemy(a, name) || own_is(a)) continue;  // (one going already: not again)
        const float *m = (const float *)(uintptr_t)(a + 0x398);
        float dx = m[3] - centre[0], dz = m[11] - centre[2];
        if (dx * dx + dz * dz > ENEMY_AREA * ENEMY_AREA) continue;
        enemy_delete(c, a);
        gone++;
    }
    static int gone_logged;
    if (gone && gone_logged++ < 20) LOG("[online] %d of our enemies gone: the host's are there\n", gone);
    // his: each one's actor, where his is, playing what his plays (his feed stopped: they stay as they are)
    for (int k = 0; fresh && k < n; k++) {
        if (e[k].life <= 0 && suppress) {  // (died, wherever: dead here too, once)
            static uint32_t marked[64];
            static int nm;
            bool done = false;
            for (int q = 0; q < 64 && !done; q++) done = marked[q] == e[k].id;
            if (!done) marked[nm++ % 64] = e[k].id, mark_dead(c, e[k].name, e[k].hash);
        }
        Own *ours = own_find(e[k].id);
        if (e[k].owner == me && ours && ours->state == OWN_HAVE) continue;  // (ours for real: not shown too)
        int s2 = -1, free_s = -1;
        for (int j = 0; j < PUPPETS; j++) {
            if (g_puppet[j].id == e[k].id) s2 = j;
            if (!g_puppet[j].id && free_s < 0) free_s = j;
        }
        if (s2 < 0 && (s2 = free_s) < 0) continue;
        if (!g_puppet[s2].id) memset(&g_puppet[s2], 0, sizeof g_puppet[s2]), g_puppet[s2].id = e[k].id;
        if (g_puppet[s2].dying) continue;  // (dying here: as it is, until it goes)
        const char *pa = enemy_actor(e[k].name);
        Piece *p = &g_puppet[s2].p;
        // died in its owner's game (its body there a ragdoll, its root going anywhere): here it stays where it was,
        // plays its own Die, drops its weapon, and goes 2.5 s later with the game's "Dead" effect
        if (e[k].life <= 0 && p->actor) {
            g_puppet[s2].dying = 75;
            delete_piece(c, &g_puppet[s2].w), delete_piece(c, &g_puppet[s2].sh);
            play_as(c, p->actor, "Die", 0, 0, -1, true);
            LOG("[online] the host's %s %u died\n", e[k].name, e[k].id);
            continue;
        }
        if (piece_update(c, 0, p, pa ? pa : "", "host")) {
            g_puppet[s2].as[0] = 0;
            g_puppet[s2].life = 0;  // (a new actor: what it has is its own life, no hit)
            LOG("[online] the host's %s %u at %.1f %.1f %.1f, AS %s\n", e[k].name, e[k].id, e[k].m[3], e[k].m[7], e[k].m[11], e[k].as);
        }
        // its weapon and shield (OnlineGear actors, as the players'): in this one's own hand, as the owner's holds them
        // there (else where his are), at the size it holds them (a Moblin's are bigger)
        for (int h = 0; h < 2; h++) {
            const char *gn = h ? e[k].shield : e[k].weapon;
            const float *gm = h ? e[k].sm : e[k].wm;
            int bone = h ? e[k].sbone : e[k].wbone;
            const char *ga = gn[0] ? gear_actor(gn) : NULL;
            Piece *w = h ? &g_puppet[s2].sh : &g_puppet[s2].w;
            if (w->name[0] && strcmp(w->name, ga ? ga : "")) delete_piece(c, w);
            piece_update(c, 0, w, ga ? ga : "", "host");
            if (!w->actor) continue;
            static int slog2;
            if (getenv("SAVAGEWIND_ONLINE_SCALE_LOG") && slog2++ % 300 == 0) {
                const float *am = (const float *)(uintptr_t)(w->actor + 0x398);
                LOG("[online] copy %s's %s: want %.2f, its matrix %.2f\n", e[k].name, gn, weapon_scale(e[k].name, gn),
                    sqrtf(am[0] * am[0] + am[4] * am[4] + am[8] * am[8]));
            }
            float wm[12], bm[12], sc = weapon_scale(e[k].name, gn);
            if (bone >= 1 && bone <= 2 && p->actor && bone_mtx(c, p->actor, k_hands[bone - 1], bm)) mtx_mul(bm, gm, wm);
            else if (bone) continue;  // (its hand not there yet)
            else memcpy(wm, gm, sizeof wm);
            call(c, FN_SET_MTX, w->actor, (uint64_t)(uintptr_t)wm, 0, 0, 0, 0);
            // (its size: its model's root bone scaled; a scale in its matrix isn't drawn)
            int nb = 0;
            uint64_t unit = sc != 1 ? first_unit(w->actor, &nb) : 0;
            if (unit && nb > 0) {
                float lm[12], lsc[3], ssc[3] = {sc, sc, sc};
                call(c, FN_BONE_LOCAL_GET, unit, (uint64_t)(uintptr_t)lm, (uint64_t)(uintptr_t)lsc, 0, 0, 0);
                call(c, FN_BONE_LOCAL_SET, unit, (uint64_t)(uintptr_t)lm, (uint64_t)(uintptr_t)ssc, 0, 0, 0);
            }
        }
        if (!p->actor) continue;
        static int plog;
        if (getenv("SAVAGEWIND_ONLINE_PUPPET_LOG") && plog++ % 300 == 0) {  // (where a frame of the game left it)
            const float *am = (const float *)(uintptr_t)(p->actor + 0x398);
            LOG("[online] puppet %s: his at %.1f %.1f %.1f, ours at %.1f %.1f %.1f\n", e[k].name, e[k].m[3], e[k].m[7],
                e[k].m[11], am[3], am[7], am[11]);
        }
        call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)e[k].m, 0, 0, 0, 0);
        // its attacks on us
        if (strcmp(e[k].as, g_puppet[s2].seen)) {
            snprintf(g_puppet[s2].seen, sizeof g_puppet[s2].seen, "%s", e[k].as);
            if (getenv("SAVAGEWIND_ONLINE_ENEMY_AS_LOG")) LOG("[online] the host's %s %u: %s\n", e[k].name, e[k].id, e[k].as);
            if (strstr(e[k].as, "Attack") || strstr(e[k].as, "Kick")) g_puppet[s2].strike = STRIKE_FRAMES;
        }
        if (g_puppet[s2].strike > 0 && --g_puppet[s2].strike == 0) strike(c, player, &e[k]);
        // hit here: what it lost since it was set goes to his game; its life is his (never 0 here: his dies there)
        int32_t *life = (int32_t *)(uintptr_t)(p->actor + ENEMY_LIFE);
        if (e[k].life > 0 && g_puppet[s2].life > 0 && *life < g_puppet[s2].life) {  // (dead in his game: no more)
            online_send_hit(e[k].id, g_puppet[s2].life - *life);
            LOG("[online] we hit the host's %s %u: %d\n", e[k].name, e[k].id, g_puppet[s2].life - *life);
        }
        *life = g_puppet[s2].life = e[k].life > 1 ? e[k].life : 1;
        // (again each frame until it plays: a just made actor can't yet; given up after a second)
        if (e[k].as[0] && strcmp(e[k].as, g_puppet[s2].as) && running(p->actor)) {
            if (play_as(c, p->actor, e[k].as, 0, 0, -1, false) || ++g_puppet[s2].tries > 30) {
                if (g_puppet[s2].tries > 30) LOG("[online] the host's %s: no AS %s\n", e[k].name, e[k].as);
                snprintf(g_puppet[s2].as, sizeof g_puppet[s2].as, "%s", e[k].as);
                g_puppet[s2].tries = 0;
            }
        }
    }
    // the dying ones: after their Die, the game's own vanishing (its XLink "Disappear", as 0x70015302c0 has a dying
    // actor's: 0x700159f00c(actor, name, 2, ...)), hidden, gone a second later
    for (int j = 0; j < PUPPETS; j++) {
        if (!g_puppet[j].id || !g_puppet[j].dying) continue;
        int d = --g_puppet[j].dying;
        uint64_t a = g_puppet[j].p.actor;
        bool there = a && alive(a, g_puppet[j].p.w);
        if (d == 30 && there) {
            uint64_t out[4] = {0, 0, 0, 0};
            call(c, FN_XLINK_EMIT, a, (uint64_t)(uintptr_t)"Disappear", 2, (uint64_t)(uintptr_t)out, 0, 0);
        }
        if (d == 20 && there) model_visible(c, a, false);
        if (d > 0) continue;
        delete_piece(c, &g_puppet[j].p), delete_piece(c, &g_puppet[j].w), delete_piece(c, &g_puppet[j].sh);
        g_puppet[j].id = 0;
    }
    // those no one has (killed, gone from the host's world), or we have for real now, go
    for (int j = 0; fresh && j < PUPPETS; j++) {
        if (!g_puppet[j].id) continue;
        bool there = false;
        for (int k = 0; k < n && !there; k++) {
            Own *ours = own_find(e[k].id);
            there = e[k].id == g_puppet[j].id && !(e[k].owner == me && ours && ours->state == OWN_HAVE);
        }
        if (!there && !g_puppet[j].dying)
            delete_piece(c, &g_puppet[j].p), delete_piece(c, &g_puppet[j].w), delete_piece(c, &g_puppet[j].sh), g_puppet[j].id = 0;
    }
}

// ---- Loot (protocol 15): what our enemies drop when they die here (the items that come near where one died in the
// seconds after, its weapon and shield) goes on everyone's list, kept by the host's game (each by an id of its own,
// the game it came from and its actor's id there); the others make each one where it is, a real item they can pick
// up. One picked up anywhere (gone from where it was with a Link next to it) is taken off the list, and goes in every
// game, ours too
static bool is_loot(const char *n) {
    return !strncmp(n, "Item_", 5) || !strncmp(n, "Weapon_", 7) || !strncmp(n, "PutRupee", 8);
}

#define DEATHS 8
static struct { float pos[3]; int frames; uint32_t before[48]; int nbefore; } g_death[DEATHS];  // (frames: watched)
#define MINE 32
static struct { uint32_t lid; uint64_t actor; Watch w; char name[40]; bool sent, listed; uint32_t gid; float pos[3]; } g_mine[MINE];
#define THEIRS ONLINE_LOOT
static struct { uint32_t gid; Piece p; bool placed, gone; float pos[3]; } g_theirs[THEIRS];
static OnlineLoot g_loot_list[ONLINE_LOOT];  // (the host's game: the list)
static int g_nloot_list;
static uint32_t g_next_gid = 1;

static void loot_forget(void) {
    for (int k = 0; k < THEIRS; k++) orphan(&g_theirs[k].p);
    memset(g_death, 0, sizeof g_death), memset(g_mine, 0, sizeof g_mine), memset(g_theirs, 0, sizeof g_theirs);
    g_nloot_list = 0;
}

// one of ours died here (at pos): the items near it now are no loot of its
static void loot_death(const float pos[3], int na) {
    int d = 0;
    while (d < DEATHS && g_death[d].frames) d++;
    if (d == DEATHS) return;
    memcpy(g_death[d].pos, pos, 12);
    g_death[d].frames = 300, g_death[d].nbefore = 0;  // (10 s)
    for (int k = 0; k < na && g_death[d].nbefore < 48; k++) {
        char n[48];
        uint64_t a = g_actors[k];
        actor_name_of(a, n);
        if (!is_loot(n)) continue;
        const float *m = (const float *)(uintptr_t)(a + 0x398);
        float dx = m[3] - pos[0], dy = m[7] - pos[1], dz = m[11] - pos[2];
        if (dx * dx + dy * dy + dz * dz < 8 * 8) g_death[d].before[g_death[d].nbefore++] = *(uint32_t *)(uintptr_t)(a + 0x60);
    }
}

static void loot_mine_add(uint64_t a, const char *n, int me) {
    uint32_t id = *(uint32_t *)(uintptr_t)(a + 0x60);
    for (int k = 0; k < MINE; k++)
        if (g_mine[k].actor && g_mine[k].lid == id) return;
    for (int k = 0; k < MINE; k++)
        if (!g_mine[k].actor) {
            memset(&g_mine[k], 0, sizeof g_mine[k]);
            g_mine[k].lid = id, g_mine[k].actor = a, g_mine[k].w = watch(a);
            snprintf(g_mine[k].name, sizeof g_mine[k].name, "%s", n);
            LOG("[online] loot of ours: %s %u\n", n, id);
            (void)me;
            return;
        }
}

static bool near_link(const float link[3], const float p[3], float r) {
    float dx = p[0] - link[0], dy = p[1] - link[1], dz = p[2] - link[2];
    return dx * dx + dy * dy + dz * dz < r * r;
}

// the host's game: its list, from what came (the others' adds and takes) and its own
static void loot_host_take(uint32_t gid) {
    for (int k = 0; k < g_nloot_list; k++)
        if (g_loot_list[k].gid == gid) {
            LOG("[online] loot %u (%s) taken\n", gid, g_loot_list[k].name);
            g_loot_list[k] = g_loot_list[--g_nloot_list];
            return;
        }
}

static void loot_host_add(const OnlineLoot *l) {
    for (int k = 0; k < g_nloot_list; k++)
        if (g_loot_list[k].origin == l->origin && g_loot_list[k].lid == l->lid) return;
    if (g_nloot_list == ONLINE_LOOT) return;  // (full: this one stays only where it fell; taking one off the list
                                              // would have it go in every game, as one picked up)
    g_loot_list[g_nloot_list] = *l;
    g_loot_list[g_nloot_list++].gid = g_next_gid++;
}

static void loot_update(Cpu *c, uint64_t player, const float link[3], int me, bool host) {
    static int tick;
    if (tick++ % 3) return;
    // our owned enemies dying: watched for what they drop
    int na = 0;
    bool scanned = false;
    for (int k = 0; k < OWNS; k++) {
        Own *o = &g_own[k];
        if (o->state != OWN_HAVE) continue;
        bool there = alive(o->actor, o->w);
        int32_t life = there ? *(int32_t *)(uintptr_t)(o->actor + ENEMY_LIFE) : 0;
        if (there) {
            const float *am = (const float *)(uintptr_t)(o->actor + 0x398);
            o->last_pos[0] = am[3], o->last_pos[1] = am[7], o->last_pos[2] = am[11];
        }
        for (int h = 0; there && life > 0 && h < 2; h++) {  // (what it holds now: dropped as it dies, unlinked then)
            char n[48];
            uint64_t g2 = linked(o->actor + 0xb98 + 0x18 * h);
            actor_name_of(g2, n);
            if (!strncmp(n, "Weapon_", 7) && g2 != o->held[h]) o->held[h] = g2, o->held_w[h] = watch(g2);
        }
        if (o->died || (there && life > 0)) continue;
        o->died = true;
        if (!scanned) na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS), scanned = true;
        loot_death(o->last_pos, na);
        for (int h = 0; h < 2; h++) {  // (its weapon and shield: they fall)
            char n[48];
            if (!o->held[h] || !alive(o->held[h], o->held_w[h])) continue;
            actor_name_of(o->held[h], n);
            if (!strncmp(n, "Weapon_", 7)) loot_mine_add(o->held[h], n, me);
        }
        LOG("[online] %u died here: its loot watched\n", o->gid);
    }
    // the items that come near where one died
    bool watching = false;
    for (int d = 0; d < DEATHS; d++) watching |= g_death[d].frames > 0;
    if (watching) {
        if (!scanned) na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
        for (int k = 0; k < na; k++) {
            char n[48];
            uint64_t a = g_actors[k];
            actor_name_of(a, n);
            if (!is_loot(n)) continue;
            uint32_t id = *(uint32_t *)(uintptr_t)(a + 0x60);
            const float *m = (const float *)(uintptr_t)(a + 0x398);
            float p[3] = {m[3], m[7], m[11]};
            for (int d = 0; d < DEATHS; d++) {
                if (!g_death[d].frames || !near_link(g_death[d].pos, p, 6)) continue;
                bool old = false;
                for (int b = 0; b < g_death[d].nbefore && !old; b++) old = g_death[d].before[b] == id;
                bool theirs = false;  // (one of the others' we made: not ours)
                for (int t = 0; t < THEIRS && !theirs; t++) theirs = g_theirs[t].p.actor == a;
                if (!old && !theirs) loot_mine_add(a, n, me);
                break;
            }
        }
        for (int d = 0; d < DEATHS; d++)
            if (g_death[d].frames) g_death[d].frames = g_death[d].frames > 3 ? g_death[d].frames - 3 : 0;
    }
    // the list as it is now
    static OnlineLoot list[ONLINE_LOOT];
    int nl;
    if (host) {
        OnlineLoot add[32];
        uint32_t tk[32];
        int n = online_take_loot_adds(add, 32);
        for (int k = 0; k < n; k++) loot_host_add(&add[k]);
        n = online_take_loot_takes(tk, 32);
        for (int k = 0; k < n; k++) loot_host_take(tk[k]);
    }
    // ours: on the list once (the host's game adds it at once); picked up here (gone, a Link next to where it was):
    // taken off; off the list after being on it: picked up elsewhere, gone here
    for (int k = 0; k < MINE; k++) {
        if (!g_mine[k].actor) continue;
        bool there = alive(g_mine[k].actor, g_mine[k].w);
        if (there) {
            const float *am = (const float *)(uintptr_t)(g_mine[k].actor + 0x398);
            g_mine[k].pos[0] = am[3], g_mine[k].pos[1] = am[7], g_mine[k].pos[2] = am[11];
        }
        if (!g_mine[k].sent) {
            OnlineLoot l = {0};
            l.lid = g_mine[k].lid, l.origin = (uint8_t)me;
            snprintf(l.name, sizeof l.name, "%s", g_mine[k].name);
            const float *m = (const float *)(uintptr_t)(g_mine[k].actor + 0x398);
            if (there) l.pos[0] = m[3], l.pos[1] = m[7], l.pos[2] = m[11];
            if (host) loot_host_add(&l);
            else online_send_loot_add(&l);
            g_mine[k].sent = true;
        }
        if (!there) {
            // (picked up: a weapon or shield is taken from the others; food, materials: each picks up his own)
            if (g_mine[k].listed && near_link(link, g_mine[k].pos, 4) && !strncmp(g_mine[k].name, "Weapon_", 7)) {
                if (host) loot_host_take(g_mine[k].gid);
                else online_send_loot_take(g_mine[k].gid);
            }
            LOG("[online] loot of ours %s gone here\n", g_mine[k].name);
            g_mine[k].actor = 0;
        }
    }
    nl = host ? g_nloot_list : online_loots(list);
    if (host) memcpy(list, g_loot_list, nl * sizeof *list), online_set_loots(list, nl);
    if (nl < 0) return;
    for (int k = 0; k < MINE; k++) {
        if (!g_mine[k].actor) continue;
        bool on = false;
        for (int j = 0; j < nl && !on; j++)
            if (list[j].origin == me && list[j].lid == g_mine[k].lid) on = true, g_mine[k].gid = list[j].gid;
        if (on) g_mine[k].listed = true;
        else if (g_mine[k].listed) {  // (picked up by another)
            LOG("[online] loot of ours %s picked up elsewhere\n", g_mine[k].name);
            if (alive(g_mine[k].actor, g_mine[k].w)) call(c, FN_DELETE, g_mine[k].actor, 0, 0, 0, 0, 0);
            g_mine[k].actor = 0;
        }
    }
    // the others': made here, where it is; picked up here: taken off; off the list: gone here
    for (int j = 0; j < nl; j++) {
        if (list[j].origin == me) continue;
        int t = -1, free_t = -1;
        for (int k = 0; k < THEIRS; k++) {
            if (g_theirs[k].gid == list[j].gid) t = k;
            if (!g_theirs[k].gid && free_t < 0) free_t = k;
        }
        if (t < 0) {
            if (free_t < 0 || !near_link(link, list[j].pos, 150)) continue;
            t = free_t;
            memset(&g_theirs[t], 0, sizeof g_theirs[t]);
            g_theirs[t].gid = list[j].gid;
            memcpy(g_theirs[t].pos, list[j].pos, 12);
        }
        if (g_theirs[t].gone) continue;
        Piece *pc = &g_theirs[t].p;
        bool had = pc->actor != 0;
        if (had && !alive(pc->actor, pc->w)) {  // (gone here)
            if (near_link(link, g_theirs[t].pos, 4) && !strncmp(list[j].name, "Weapon_", 7)) {
                LOG("[online] loot %u (%s) picked up here\n", list[j].gid, list[j].name);
                if (host) loot_host_take(list[j].gid);
                else online_send_loot_take(list[j].gid);
            }
            g_theirs[t].gone = true;
            memset(pc, 0, sizeof *pc);
            continue;
        }
        if (!had) {
            if (!pc->pending && !pc->name[0]) {
                pc->handle = new_handle();
                if (request(c, list[j].name, pc->handle)) pc->pending = true, snprintf(pc->name, sizeof pc->name, "%s", list[j].name);
                else { g_theirs[t].gone = true; continue; }
            }
            int *unit = (int *)(uintptr_t)pc->handle->unit;
            if (unit && *unit == 2) {
                pc->pending = false;
                pc->actor = call(c, FN_HANDLE_TAKE, (uint64_t)(uintptr_t)pc->handle, 0, 0, 0, 0, 0);
                if (pc->actor) {
                    pc->w = watch(pc->actor);
                    float m[12] = {1, 0, 0, list[j].pos[0], 0, 1, 0, list[j].pos[1] + 0.2f, 0, 0, 1, list[j].pos[2]};
                    call(c, FN_SET_MTX, pc->actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
                    LOG("[online] loot %u (%s) made here at %.2f %.2f %.2f\n", list[j].gid, list[j].name, list[j].pos[0], list[j].pos[1], list[j].pos[2]);
                }
            } else if (unit && (*unit == 3 || *unit == 5)) {
                call(c, FN_HANDLE_RELEASE, (uint64_t)(uintptr_t)pc->handle, 0, 0, 0, 0, 0);
                g_theirs[t].gone = true, pc->pending = false;
            }
        } else {  // (where it is now: if it was picked up a moment from now, near it)
            const float *m = (const float *)(uintptr_t)(pc->actor + 0x398);
            g_theirs[t].pos[0] = m[3], g_theirs[t].pos[1] = m[7], g_theirs[t].pos[2] = m[11];
        }
    }
    for (int k = 0; k < THEIRS; k++) {
        if (!g_theirs[k].gid) continue;
        bool on = false;
        for (int j = 0; j < nl && !on; j++) on = list[j].gid == g_theirs[k].gid;
        if (on) continue;
        if (g_theirs[k].p.actor && alive(g_theirs[k].p.actor, g_theirs[k].p.w)) {
            LOG("[online] loot %u picked up elsewhere\n", g_theirs[k].gid);
            call(c, FN_DELETE, g_theirs[k].p.actor, 0, 0, 0, 0, 0);
        } else if (g_theirs[k].p.pending) orphan(&g_theirs[k].p);
        memset(&g_theirs[k], 0, sizeof g_theirs[k]);
    }
}

// ---- The quests (protocol 17): the flags of online_mod/quests.txt (the main, shrine and side quests' steps and the bool flags
// they depend on) are everyone's: each game sends which it has (every 2 s), the host's game ORs them all with its own
// and sends the union, and each game sets the ones it lacks. A bool flag's place in GameDataMgr (test_warp_gdt_mgr):
// its pending-write buffer (+0xbd8) +8 a count and +0x10 an array of proxies (0x18 each: +8 the flag's definition,
// its name's crc32 at +0xc; +0x10 the value), sorted by that hash
static uint32_t crc32_str(const char *s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        for (int b = 0; b < 8; b++) h = h >> 1 ^ (0xEDB88320u & -(h & 1));
    }
    return ~h;
}

// the value of the bool flag with that hash (NULL: none)
static uint32_t *gdt_value(int off, uint32_t h);
static uint32_t *bool_flag(uint32_t h) { return gdt_value(0xbd8, h); }
// a GameData value by its name's hash: its value in the buffer at mgr+off (+0xbd8 the bool flags), NULL none
static uint32_t *gdt_value(int off, uint32_t h) {
    uint64_t mgr = test_warp_gdt_mgr();
    uint64_t pb = VALID(mgr) ? *(uint64_t *)(uintptr_t)(mgr + off) : 0;
    uint64_t buf = VALID(pb) ? *(uint64_t *)(uintptr_t)pb : 0;
    if (!VALID(buf)) return NULL;
    uint32_t n = *(uint32_t *)(uintptr_t)(buf + 8);
    uint64_t arr = *(uint64_t *)(uintptr_t)(buf + 0x10);
    if (!n || n > 200000 || !VALID(arr)) return NULL;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint64_t f = *(uint64_t *)(uintptr_t)(arr + 8 * mid), pr = VALID(f) ? *(uint64_t *)(uintptr_t)(f + 8) : 0;
        uint32_t mh = VALID(pr) ? *(uint32_t *)(uintptr_t)(pr + 0xc) : 0;
        if (mh < h) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= n) return NULL;
    uint64_t f = *(uint64_t *)(uintptr_t)(arr + 8 * lo), pr = VALID(f) ? *(uint64_t *)(uintptr_t)(f + 8) : 0;
    return VALID(pr) && *(uint32_t *)(uintptr_t)(pr + 0xc) == h ? (uint32_t *)(uintptr_t)(f + 0x10) : NULL;
}

// An event started here, as an EventTag starts its own (1.9.0 0x70011662e0): its metadata (0x198 bytes: 0x7001112508
// makes it, 0x7001112794(meta, flow name, entry point name, "") names it, 0x70011128b4 frees it), then a request for
// the event manager (0x7001d66758's: 0x700110d590(mgr, request)): +0 a matrix (where), +0x30 0x101, +0x38 the
// metadata, +0x48 the actor it is for. true when the manager took it
#define FN_EVT_META_INIT 0x7001112508ull
#define FN_EVT_META_NAME 0x7001112794ull
#define FN_EVT_META_FREE 0x70011128b4ull
#define FN_EVT_CALL 0x700110d590ull
#define P_EVT_MGR 0x7001d66758ull
static bool start_event(Cpu *c, uint64_t actor, const char *flow, const char *entry) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)P_EVT_MGR;
    mgr = VALID(mgr) ? *(uint64_t *)(uintptr_t)mgr : 0;
    if (!VALID(mgr) || !VALID(actor)) return false;
    static uint8_t meta[0x200] __attribute__((aligned(16)));
    static struct { float m[12]; uint32_t flags, pad; uint64_t meta, zero, actor; } req __attribute__((aligned(16)));
    memset(meta, 0, sizeof meta);
    call(c, FN_EVT_META_INIT, (uint64_t)(uintptr_t)meta, 0, 0, 0, 0, 0);
    call(c, FN_EVT_META_NAME, (uint64_t)(uintptr_t)meta, (uint64_t)(uintptr_t)flow, (uint64_t)(uintptr_t)entry,
         (uint64_t)(uintptr_t)"", 0, 0);
    memset(&req, 0, sizeof req);
    memcpy(req.m, (const float *)(uintptr_t)(actor + 0x398), sizeof req.m);
    req.flags = 0x101, req.meta = (uint64_t)(uintptr_t)meta, req.actor = actor;
    bool ok = call(c, FN_EVT_CALL, mgr, (uint64_t)(uintptr_t)&req, 0, 0, 0, 0) & 1;
    call(c, FN_EVT_META_FREE, (uint64_t)(uintptr_t)meta, 0, 0, 0, 0, 0);
    LOG("[online] event %s<%s> started: %d\n", flow, entry, ok);
    return ok;
}

// the Sheikah Slate, another player having taken it: its pedestal's own event here (Obj_DRStone_A_01<Talk>: the
// cutscene Demo700_0, which sets its flags, gives the slate, wakes the HUD and opens the first door), when this Link is
// near its pedestal and in no event; true once started (its flags are then left to it)
static bool slate_event(Cpu *c, uint64_t player) {
    static int tries, wait;
    if (wait > 0) { wait--; return false; }
    extern bool framepace_in_event(void);
    if (framepace_in_event() || tries > 5) return false;
    const float *pm = (const float *)(uintptr_t)(player + 0x398);
    float dx = pm[3] + 1125.0f, dz = pm[11] - 1903.2f;
    if (dx * dx + dz * dz > 40 * 40) return false;  // (not in the Shrine of Resurrection: the flags alone)
    int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
    uint64_t stone = 0;
    for (int k = 0; k < na && !stone; k++) {
        char n[48];
        actor_name_of(g_actors[k], n);
        if (!strcmp(n, "Obj_DRStone_A_01")) stone = g_actors[k];
    }
    tries++, wait = 90;
    return start_event(c, stone ? stone : player, "Obj_DRStone_A_01", "Talk");
}

// A bool flag set as an event sets it (EventFlagONAction, 1.9.0 0x7001187758): its handle (0x7001143950(the
// manager's pending buffer, its name's crc32), the manager's +0xc28 in its top byte), then the manager's setter
// (0x7000730148(manager, value, handle)), which tells the rest of the game (the HUD wakes for the slate); a value
// written in memory alone is seen by nothing. The manager: 0x7001d66478's
#define FN_GDT_HANDLE 0x7001143950ull
#define FN_GDT_SET_BOOL 0x7000730148ull
#define P_GDT_MGR 0x7001d66478ull
// An item into the pouch, as an event's Demo_IncreasePorchItem does (EventIncreasePorchItemAction, 1.9.0 0x7001187e3c):
// 0x700120af80(name, count) whether it fits, then 0x700120af98(name, count) (x0 a sead::SafeString)
#define FN_POUCH_CAN_GET 0x700120af80ull
#define FN_POUCH_GET 0x700120af98ull
static void give_item(Cpu *c, const char *name, int count) {
    uint64_t ss[2] = {*(uint64_t *)(uintptr_t)P_SAFE_STRING_VT + 0x10, (uint64_t)(uintptr_t)name};
    if (!(call(c, FN_POUCH_CAN_GET, (uint64_t)(uintptr_t)ss, (uint64_t)count, 0, 0, 0, 0) & 1)) {
        LOG("[online] %s doesn't fit in the pouch\n", name);
        return;
    }
    call(c, FN_POUCH_GET, (uint64_t)(uintptr_t)ss, (uint64_t)count, 0, 0, 0, 0);
    LOG("[online] %s into the pouch\n", name);
}

// a heart container, as a Divine Beast's gives it: the game's own for it (EventIncreasePorchItemAction,
// 0x7001187e3c, hands Obj_HeartUtuwa_A_01 / Obj_HeartKakera_A_01 to 0x7001211520, not to the pouch)
#define FN_GET_SPECIAL 0x7001211520ull  // x0 0, x1 a SafeString: the item's name
static void give_heart(Cpu *c) {
    static const char name[] = "Obj_HeartUtuwa_A_01";
    uint64_t ss[2] = {*(uint64_t *)(uintptr_t)P_SAFE_STRING_VT + 0x10, (uint64_t)(uintptr_t)name};
    call(c, FN_GET_SPECIAL, 0, (uint64_t)(uintptr_t)ss, 0, 0, 0, 0);
    LOG("[online] a heart container\n");
}

static bool slate_group(const char *n) {
    return !strcmp(n, "IsGet_PortableUnit") || !strcmp(n, "PortableUnit_PowerOn") || !strcmp(n, "OP_DRC") ||
           !strcmp(n, "Open_FirstDoor") || !strcmp(n, "IsPlayed_Demo700_0");
}

// The slate's runes and their upgrades (IsGet_<item>): the flag alone leaves the slate's runes as they were (its rune
// list is made when an item is had: 0x700120af98), so the item is given too, as the shrine's pedestal does
static bool slate_rune(const char *n) {
    static const char *const runes[] = {"IsGet_Obj_RemoteBomb", "IsGet_Obj_RemoteBombCube", "IsGet_Obj_Magnetglove",
        "IsGet_Obj_StopTimer", "IsGet_Obj_IceMaker", "IsGet_Obj_Camera", "IsGet_Obj_RemoteBombLv2",
        "IsGet_Obj_RemoteBombCube2", "IsGet_Obj_StopTimerLv2", "IsGet_Obj_SheikSensor", "IsGet_Obj_SheikSensorLv2",
        "IsGet_Obj_SheikPadLv2", "IsGet_Obj_Motorcycle"};
    for (size_t k = 0; k < sizeof runes / sizeof *runes; k++)
        if (!strcmp(n, runes[k])) return true;
    return false;
}

static bool union_has(const uint8_t *all, char (*names)[48], int n, const char *want) {
    for (int k = 0; k < n; k++)
        if (!strcmp(names[k], want)) return all[k >> 3] >> (k & 7) & 1;
    return false;
}

static bool set_bool_flag(Cpu *c, uint32_t h) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)P_GDT_MGR;
    mgr = VALID(mgr) ? *(uint64_t *)(uintptr_t)mgr : 0;
    uint64_t pb = VALID(mgr) ? *(uint64_t *)(uintptr_t)(mgr + 0xbd8) : 0;
    uint64_t buf = VALID(pb) ? *(uint64_t *)(uintptr_t)pb : 0;
    if (!VALID(buf)) return false;
    uint32_t handle = (uint32_t)call(c, FN_GDT_HANDLE, buf, h, 0, 0, 0, 0);
    handle |= (uint32_t)*(int32_t *)(uintptr_t)(mgr + 0xc28) << 24;
    if (handle == 0xffffffffu) return false;
    return call(c, FN_GDT_SET_BOOL, mgr, 1, handle, 0, 0, 0) & 1;
}

// A Korok another player found, when its Korok is here (Npc_HiddenKorokGround / Fly within 50 m of this Link): its
// cutscene as the finder sees it (Demo017_0<HiddenKorok_Ground / _Air> with the Korok: 0x70008f682c starts it so),
// which makes it appear, gives the seed and sets its flag; false when it isn't here or an event plays
#define P_KOROK_EVT_MGR 0x7001e3a638ull  // (the event manager, as 0x70008f682c reads it)
#define FN_EVT_START 0x700110d500ull     // x0 it, x1 an event's metadata, x2 its actor, x3 0 -> started
static int korok_event(Cpu *c, uint64_t player, const char *flag) {
    extern bool framepace_in_event(void);
    if (!VALID(player) || getenv("SAVAGEWIND_ONLINE_NO_KOROK_EVENT")) return 0;
    bool fly = strstr(flag, "KorokFly") != NULL;
    const char *want = fly ? "Npc_HiddenKorokFly" : "Npc_HiddenKorokGround";
    uint32_t id = (uint32_t)strtoul(strrchr(flag, '_') + 1, NULL, 10);  // (its map object's HashId)
    const float *pm = (const float *)(uintptr_t)(player + 0x398);
    int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
    uint64_t found = 0;
    for (int k = 0; k < na && !found; k++) {
        char n[48];
        actor_name_of(g_actors[k], n);
        if (strcmp(n, want)) continue;
        const float *m = (const float *)(uintptr_t)(g_actors[k] + 0x398);
        float d = (m[3] - pm[3]) * (m[3] - pm[3]) + (m[7] - pm[7]) * (m[7] - pm[7]) + (m[11] - pm[11]) * (m[11] - pm[11]);
        if (d > 60 * 60) continue;
        static int off = -1;  // (where an actor keeps its HashId: looked for once)
        if (off < 0)
            for (int o = 0x100; o < 0x900 && off < 0; o += 4)
                if (*(uint32_t *)(uintptr_t)(g_actors[k] + o) == id) off = o, LOG("[online] an actor's HashId is at +%#x\n", o);
        if (off >= 0 && *(uint32_t *)(uintptr_t)(g_actors[k] + off) == id) found = g_actors[k];
    }
    if (!found) return 0;
    if (framepace_in_event()) return -1;  // (an event plays: later)
    LOG("[online] %s: its Korok is here, its cutscene\n", flag);
    uint64_t mgr = *(uint64_t *)(uintptr_t)P_KOROK_EVT_MGR;
    if (!VALID(mgr)) return 0;
    static uint8_t meta[0x200] __attribute__((aligned(16)));
    memset(meta, 0, sizeof meta);
    call(c, FN_EVT_META_INIT, (uint64_t)(uintptr_t)meta, 0, 0, 0, 0, 0);
    call(c, FN_EVT_META_NAME, (uint64_t)(uintptr_t)meta, (uint64_t)(uintptr_t)"Demo017_0",
         (uint64_t)(uintptr_t)(fly ? "HiddenKorok_Air" : "HiddenKorok_Ground"), (uint64_t)(uintptr_t)"", 0, 0);
    bool ok = call(c, FN_EVT_START, mgr, (uint64_t)(uintptr_t)meta, found, 0, 0, 0) & 1;
    call(c, FN_EVT_META_FREE, (uint64_t)(uintptr_t)meta, 0, 0, 0, 0, 0);
    LOG("[online] Demo017_0 for it: %d\n", ok);
    return ok ? 1 : -1;
}

static void quests_update(Cpu *c, uint64_t player, bool host) {
    static MapLine *m;
    static char names[ONLINE_QUEST_BYTES * 8][48];
    static uint32_t hash[ONLINE_QUEST_BYTES * 8];
    static int n = -1, tick;
    if (n < 0) {  // (quests.txt: a name a line; map_load wants key=value)
        n = 0;
        const char *ov = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char p[1100], line[128];
        snprintf(p, sizeof p, "%s/quests.txt", ov ? ov : "");
        FILE *f = ov && *ov ? fopen(p, "r") : NULL;
        while (f && n < ONLINE_QUEST_BYTES * 8 && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            snprintf(names[n], sizeof names[n], "%s", line);
            hash[n] = crc32_str(line), n++;
        }
        if (f) fclose(f);
        LOG("[online] %d quest flags shared\n", n);
        (void)m;
    }
    if (!n || tick++ % 60) return;
    static bool counted;
    if (!counted && test_warp_gdt_mgr()) {  // (how many aren't bool flags)
        int miss = 0;
        for (int k = 0; k < n; k++)
            if (!bool_flag(hash[k])) {
                if (miss < 5) LOG("[online] quest flag %s: no bool flag\n", names[k]);
                miss++;
            }
        LOG("[online] %d of the quest flags aren't bool flags\n", miss);
        if (getenv("SAVAGEWIND_ONLINE_GDT_PROBE"))  // (test: where MaxHeartValue is)
            for (int off = 0xb80; off < 0xc40; off += 8) {
                uint32_t *v = gdt_value(off, crc32_str("MaxHeartValue"));
                if (v) LOG("[online] MaxHeartValue at mgr+%#x: %d\n", off, (int)*v);
            }
        const char *ql = getenv("SAVAGEWIND_ONLINE_QUEST_LOG");
        for (int k = 0; ql && k < n; k++)
            if (strstr(names[k], ql)) {
                uint32_t *v = bool_flag(hash[k]);
                LOG("[online] quest flag %s = %d\n", names[k], v ? (int)*v : -1);
            }
        counted = true;
    }
    uint8_t mine[ONLINE_QUEST_BYTES] = {0}, all[ONLINE_QUEST_BYTES];
    for (int k = 0; k < n; k++) {
        uint32_t *v = bool_flag(hash[k]);
        if (v && (*v & 1)) mine[k >> 3] |= (uint8_t)(1 << (k & 7));
    }
    bool have;
    if (host) {
        static uint8_t last[ONLINE_QUEST_BYTES];
        static bool sent;
        online_take_quests(all);
        for (int k = 0; k < ONLINE_QUEST_BYTES; k++) all[k] |= mine[k];
        if (!sent || memcmp(all, last, sizeof all)) online_set_quests(all), memcpy(last, all, sizeof last), sent = true;
        have = true;
    } else {
        static uint8_t last[ONLINE_QUEST_BYTES];
        static int every;
        if (memcmp(mine, last, sizeof mine) || every++ % 5 == 0) online_send_quests(mine), memcpy(last, mine, sizeof last);
        have = online_quests(all);
    }
    // (the slate: near its pedestal without it, by its own event once the other has it, whose flags come with it; none of
    // them set meanwhile (with some set, its pedestal is half done); elsewhere its flags alone)
    static bool slate_started;
    bool by_event = slate_started;
    for (int k = 0; have && k < n && VALID(player); k++) {
        if (strcmp(names[k], "IsGet_PortableUnit") || (mine[k >> 3] >> (k & 7) & 1)) continue;
        const float *pm = (const float *)(uintptr_t)(player + 0x398);
        float dx = pm[3] + 1125.0f, dz = pm[11] - 1903.2f;
        if (dx * dx + dz * dz < 40 * 40 && getenv("SAVAGEWIND_ONLINE_SLATE_EVENT")) by_event = true;
        if (by_event && !slate_started && (all[k >> 3] >> (k & 7) & 1) && slate_event(c, player)) slate_started = true;
    }
    extern bool framepace_in_event(void);
    bool korok_started = false;
    for (int k = 0; have && k < n; k++) {
        if (!(all[k >> 3] >> (k & 7) & 1) || (mine[k >> 3] >> (k & 7) & 1)) continue;
        if (by_event && (!strcmp(names[k], "IsGet_PortableUnit") || !strcmp(names[k], "PortableUnit_PowerOn") ||
                         !strcmp(names[k], "OP_DRC") || !strcmp(names[k], "Open_FirstDoor") ||
                         !strcmp(names[k], "IsPlayed_Demo700_0"))) continue;
        uint32_t *v = bool_flag(hash[k]);
        if (!v) continue;
        // (the slate's flags once his cutscene is over: its last one, IsPlayed_Demo700_0, has come)
        if (slate_group(names[k]) && !union_has(all, names, n, "IsPlayed_Demo700_0")) continue;
        // (a Korok found near here: its own cutscene, which gives the seed and sets its flag; once started, the seed isn't
        // given again when its flag is set afterwards)
        bool korok = !strncmp(names[k], "MainField_Npc_HiddenKorok", 25), played = false;
        static uint32_t korok_played[16];
        static int korok_np;
        for (int q = 0; korok && q < 16; q++) played |= korok_played[q] == hash[k];
        if (korok && !played && !korok_started) {
            int r = korok_event(c, player, names[k]);
            static uint32_t wait_h[16];
            static int wait_n[16];
            if (r == 1) {
                korok_played[korok_np++ % 16] = hash[k], korok_started = true;
                continue;
            }
            if (r < 0) {  // (its Korok here but no cutscene yet, another event playing: again, 30 times at most)
                int q = 0;
                while (q < 16 && wait_h[q] != hash[k]) q++;
                if (q == 16) q = (int)(hash[k] % 16), wait_h[q] = hash[k], wait_n[q] = 0;
                korok_started = true;  // (one at a time)
                if (++wait_n[q] < 30) continue;
            }
        }
        if (korok && played && framepace_in_event()) continue;  // (its cutscene playing: its flag comes from it)
        if (!set_bool_flag(c, hash[k])) *v |= 1;  // (the game's setter; else in memory)
        if (!strcmp(names[k], "IsGet_PortableUnit")) give_item(c, "Obj_DRStone_Get", 1);  // (the slate in his key items)
        if (slate_rune(names[k])) give_item(c, names[k] + 6, 1);  // (a rune: the slate's runes as the game gives it)
        if (korok && !played) give_item(c, "Obj_KorokNuts", 1);  // (a Korok found far from here)
        if (!strncmp(names[k], "Clear_Dungeon", 13)) give_item(c, "Obj_DungeonClearSeal", 1);  // (a shrine: its orb)
        // (a Divine Beast: its champion's ability, as the game gives it after the beast; its heart container)
        if (!strncmp(names[k], "IsGet_Obj_HeroSoul_", 19)) give_item(c, names[k] + 6, 1);
        if (!strncmp(names[k], "Get_HeartUtuwa_", 15)) give_heart(c);
        LOG("[online] quest flag %s: set (another player did it)\n", names[k]);
    }
}

// ---- Weapons and shields on the ground (protocol 18): one picked up here (gone from where it lay with this Link
// next to it, not one he carries) goes in the others' games too (PICK: its name, where it was; there, the one of
// that name within 2 m of it is deleted). Food and materials each player picks up his own
#define GROUND 24
static struct { uint64_t actor; Watch w; char name[40]; float pos[3]; } g_ground[GROUND];

// the materials' uk_special_effect_A of a model's units (how the Stasis rune draws what it stopped: 0x700124ca00 sets
// it each frame, pulsing; 0 when it's over)
#define FN_SPECIAL_FX_A 0x700124ca00ull       // x1 a model, w2 0: its pulse now
#define FN_SPECIAL_FX_A_UNIT 0x700124ca7cull  // s0 the value, x0 a model unit
static void stasis_look(Cpu *c, uint64_t a, bool on) {
    uint64_t model = *(uint64_t *)(uintptr_t)(a + 0x4e0);
    call(c, on ? FN_ACTOR_LOOK_ON : FN_ACTOR_LOOK_OFF, a, 8, 0, 0, 0, 0);
    if (!VALID(model)) return;
    // (uk_special_effect_A: the red pulse as it is about to go, 0 meanwhile; uk_special_effect_B: its glow, 1)
    const char *ea = getenv("SAVAGEWIND_ONLINE_STASIS_A"), *eb = getenv("SAVAGEWIND_ONLINE_STASIS_B");
    float va = on && ea ? (float)atof(ea) : 0.0f, vb = on ? (eb ? (float)atof(eb) : 1.0f) : 0.0f;
    call_f(c, FN_SPECIAL_FX_B, 0, model, 0, 0, 0, 0, 0, vb);
    int n = *(int *)(uintptr_t)(model + 0x38);
    uint64_t arr = *(uint64_t *)(uintptr_t)(model + 0x40);
    for (int k = 0; VALID(arr) && k < n && k < 64; k++) {
        uint64_t u = *(uint64_t *)(uintptr_t)(arr + 8 * k);
        u = VALID(u) ? *(uint64_t *)(uintptr_t)u : 0;
        if (VALID(u)) call_f(c, FN_SPECIAL_FX_A_UNIT, u, 0, 0, 0, 0, 0, 0, va);
    }
}

// What the others stopped with the Stasis rune: the same object here (as Magnesis: the one of that name nearest where it
// was), held where it is in his game (its body taken there each frame: it stays still as his does), drawn as stopped,
// with its chains; let go when it's over there
static void stop_update(Cpu *c, uint64_t player, const OnlinePlayer *pl, int me) {
    for (int i = 0; i < ONLINE_MAX; i++) {
        const OnlineGrab *g = i != me && pl[i].used && pl[i].pose.valid ? &pl[i].pose.stop : NULL;
        bool want = g && g->name[0];
        if (g_act[i].stop && (!want || strcmp(g->name, g_act[i].stop_name) || !alive(g_act[i].stop, g_act[i].stop_w))) {
            if (alive(g_act[i].stop, g_act[i].stop_w)) stasis_look(c, g_act[i].stop, false);
            uint64_t cp = g_act[i].chains.p;
            if (VALID(cp) && *(uint32_t *)(uintptr_t)(cp + 0x20) == g_act[i].chains.id)
                call(c, FN_EFFECT_KILL, (uint64_t)(uintptr_t)&g_act[i].chains, 0, 0, 0, 0, 0);
            memset(&g_act[i].chains, 0, sizeof g_act[i].chains);
            LOG("[online] %s's Stasis on %s: over\n", pl[i].name, g_act[i].stop_name);
            g_act[i].stop = 0;
        }
        if (!want) { g_act[i].stop_name[0] = 0, g_act[i].stop_wait = 0; continue; }
        if (!g_act[i].stop && (strcmp(g->name, g_act[i].stop_name) || g_act[i].stop_wait-- <= 0)) {
            snprintf(g_act[i].stop_name, sizeof g_act[i].stop_name, "%s", g->name);
            g_act[i].stop_wait = 30;
            int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
            float best = 4 * 4;
            for (int k = 0; k < na; k++) {
                char n[48];
                actor_name_of(g_actors[k], n);
                if (strcmp(n, g->name)) continue;
                const float *m = (const float *)(uintptr_t)(g_actors[k] + 0x398);
                float d = (m[3] - g->from[0]) * (m[3] - g->from[0]) + (m[7] - g->from[1]) * (m[7] - g->from[1]) +
                          (m[11] - g->from[2]) * (m[11] - g->from[2]);
                if (d < best) best = d, g_act[i].stop = g_actors[k];
            }
            if (g_act[i].stop) {
                g_act[i].stop_w = watch(g_act[i].stop);
                call_sret(c, FN_EFFECT_EMIT, (uint64_t)(uintptr_t)&g_act[i].chains, g_act[i].stop,
                          (uint64_t)(uintptr_t)"BitaLocked_Start");
                uint64_t cp = g_act[i].chains.p;
                LOG("[online] its chains: %d\n", VALID(cp) && *(uint32_t *)(uintptr_t)(cp + 0x20) == g_act[i].chains.id);
            }
            LOG("[online] %s stopped %s with Stasis: %s\n", pl[i].name, g->name, g_act[i].stop ? "here too" : "not here");
        }
        if (!g_act[i].stop) continue;
        uint64_t body = *(uint64_t *)(uintptr_t)(g_act[i].stop + 0x190);
        if (VALID(body)) call(c, FN_BODY_TO, body, (uint64_t)(uintptr_t)g->m, 0, 0, 0, 0);
        stasis_look(c, g_act[i].stop, true);
    }
}

// The others' Cryonis pillars: a pillar of ours where each of his is (made with the game's own actor: it rises as his
// did, and one can stand on it); gone when his goes
static void ice_update(Cpu *c, const OnlinePlayer *pl, int me) {
    for (int i = 0; i < ONLINE_MAX; i++) {
        int n = i != me && pl[i].used && pl[i].pose.valid ? pl[i].pose.ice_n : 0;
        bool keep[3] = {false, false, false};
        for (int k = 0; k < n; k++) {
            const float *m = pl[i].pose.ice[k];
            int found = -1;
            for (int q = 0; q < 3 && found < 0; q++) {
                if (!g_act[i].ice[q].used) continue;
                float dx = g_act[i].ice[q].m[3] - m[3], dy = g_act[i].ice[q].m[7] - m[7], dz = g_act[i].ice[q].m[11] - m[11];
                if (dx * dx + dy * dy + dz * dz < 0.5f * 0.5f) found = q;
            }
            for (int q = 0; q < 3 && found < 0; q++)
                if (!g_act[i].ice[q].used && !keep[q]) {
                    g_act[i].ice[q].used = true, g_act[i].ice[q].rose = false, found = q;
                    memcpy(g_act[i].ice[q].m, m, sizeof g_act[i].ice[q].m);
                    LOG("[online] %s's ice pillar at %.1f %.1f %.1f\n", pl[i].name, m[3], m[7], m[11]);
                }
            if (found >= 0) keep[found] = true;
        }
        for (int q = 0; q < 3; q++) {
            if (!keep[q]) g_act[i].ice[q].used = false;
            Piece *p = &g_act[i].ice[q].p;
            if (!g_act[i].ice[q].used && (p->actor || p->pending)) {
                delete_piece(c, p);
                LOG("[online] an ice pillar of another player: gone\n");
                continue;
            }
            if (!g_act[i].ice[q].used) continue;
            piece_update(c, i, p, "OnlineIce", pl[i].name);
            if (!p->actor) continue;
            call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)g_act[i].ice[q].m, 0, 0, 0, 0);
            if (!g_act[i].ice[q].rose && running(p->actor) && play_as(c, p->actor, "Appear_Linear", 0, 0, -1, true))
                g_act[i].ice[q].rose = true;
        }
    }
}

// The bosses near the local Link and the others': each game sends the damage its own player has done to each boss so far
// (what its life lost here, but for what the other players' damage took: OnlinePose.boss[].life); what another's grows
// by is taken from that boss here too (the same map object: its HashId), so every player's blows add up in every game
// (its phases follow its life, each game its own way; a phase's new life, or a boss made again, takes nothing back)
static struct { uint64_t a; Watch w; uint32_t hash; char name[28]; } g_boss[ONLINE_BOSSES];
#define BOSS_TRACK 4
static struct { uint64_t a; uint32_t hash; int32_t last, applied, done; } g_btrack[BOSS_TRACK];  // (done: ours so far)
static struct { uint32_t hash; int32_t seen; } g_bseen[ONLINE_MAX][BOSS_TRACK];  // (each player's damage as last seen)
static int btrack(uint64_t a, uint32_t hash, int32_t life) {
    for (int k = 0; k < BOSS_TRACK; k++)
        if (g_btrack[k].a == a) return k;
    int k = 0;
    while (k < BOSS_TRACK - 1 && g_btrack[k].a) k++;
    g_btrack[k].a = a, g_btrack[k].hash = hash, g_btrack[k].last = life, g_btrack[k].applied = 0, g_btrack[k].done = 0;
    return k;
}

static void boss_sample(Cpu *c, uint64_t player, OnlinePose *pose) {
    static int tick;
    if (tick++ % 6 == 0) {
        memset(g_boss, 0, sizeof g_boss);
        float best[ONLINE_BOSSES] = {500.0f * 500.0f, 500.0f * 500.0f};
        int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
        const float *pm = (const float *)(uintptr_t)(player + 0x398);
        for (int k = 0; k < na; k++) {
            char n[48];
            actor_name_of(g_actors[k], n);
            if (!is_boss(n)) continue;
            const float *m = (const float *)(uintptr_t)(g_actors[k] + 0x398);
            float d = (m[3] - pm[3]) * (m[3] - pm[3]) + (m[7] - pm[7]) * (m[7] - pm[7]) + (m[11] - pm[11]) * (m[11] - pm[11]);
            int slot = d < best[0] ? 0 : d < best[1] ? 1 : -1;
            if (slot < 0) continue;
            if (slot == 0) g_boss[1] = g_boss[0], best[1] = best[0];
            best[slot] = d;
            g_boss[slot].a = g_actors[k], g_boss[slot].w = watch(g_actors[k]);
            g_boss[slot].hash = *(uint32_t *)(uintptr_t)(g_actors[k] + ACTOR_HASH_ID);
            snprintf(g_boss[slot].name, sizeof g_boss[slot].name, "%s", n);
        }
    }
    {   // (test: SAVAGEWIND_ONLINE_BOSS_HIT=<s>: the nearest boss's life halved that many seconds after it is first here,
        // and again every 20 s, to 0; SAVAGEWIND_ONLINE_BOSS_KILL=<s>: to 0 then, each boss)
        static int t;
        static char last[28];
        if (g_boss[0].a && strcmp(last, g_boss[0].name)) t = 0, snprintf(last, sizeof last, "%s", g_boss[0].name);
        const char *bk = getenv("SAVAGEWIND_ONLINE_BOSS_KILL"), *bh = getenv("SAVAGEWIND_ONLINE_BOSS_HIT");
        bool here = g_boss[0].a && alive(g_boss[0].a, g_boss[0].w);
        if (here && (bk || bh)) t++;
        int32_t *l = here ? (int32_t *)(uintptr_t)(g_boss[0].a + ENEMY_LIFE) : NULL;
        if (bk && here && t % (atoi(bk) * 30) == 0 && *l > 0) *l = 0, LOG("[online] boss kill test: %s\n", g_boss[0].name);
        if (bh && here && t >= atoi(bh) * 30 && (t - atoi(bh) * 30) % 600 == 0) {
            *l = *l > 10 ? *l / 2 : 0;
            LOG("[online] boss hit test: %s at %d\n", g_boss[0].name, *l);
        }
    }
    static int logt;
    for (int k = 0; getenv("SAVAGEWIND_ONLINE_BOSS_LOG") && logt++ % 60 == 0 && k < ONLINE_BOSSES; k++)
        if (g_boss[k].a && alive(g_boss[k].a, g_boss[k].w)) {
            const float *m = (const float *)(uintptr_t)(g_boss[k].a + 0x398);
            LOG("[online] boss %s %u: life %d at %.0f %.0f %.0f\n", g_boss[k].name, g_boss[k].hash,
                *(int32_t *)(uintptr_t)(g_boss[k].a + ENEMY_LIFE), m[3], m[7], m[11]);
        }
    for (int k = 0; k < ONLINE_BOSSES; k++) {
        if (!g_boss[k].a || !alive(g_boss[k].a, g_boss[k].w)) continue;
        int32_t life = *(int32_t *)(uintptr_t)(g_boss[k].a + ENEMY_LIFE);
        int t = btrack(g_boss[k].a, g_boss[k].hash, life);
        if (life < g_btrack[t].last) {  // (lost: ours, but for what the others took)
            int32_t ours = g_btrack[t].last - life - g_btrack[t].applied;
            if (ours > 0) g_btrack[t].done += ours;
        }
        g_btrack[t].last = life, g_btrack[t].applied = 0;  // (more: a new phase, made again: nothing of ours)
        pose->boss[k].hash = g_boss[k].hash;
        memcpy(pose->boss[k].name, g_boss[k].name, sizeof pose->boss[k].name);
        pose->boss[k].life = g_btrack[t].done;
    }
}

static void boss_update(Cpu *c, const OnlinePlayer *pl, int me) {
    for (int k = 0; k < ONLINE_BOSSES; k++) {
        if (!g_boss[k].a || !alive(g_boss[k].a, g_boss[k].w)) continue;
        int32_t *life = (int32_t *)(uintptr_t)(g_boss[k].a + ENEMY_LIFE);
        int t = btrack(g_boss[k].a, g_boss[k].hash, *life);
        for (int i = 0; i < ONLINE_MAX; i++) {
            if (i == me || !pl[i].used || !pl[i].pose.valid || !same_map(&pl[i])) continue;
            for (int q = 0; q < ONLINE_BOSSES; q++) {
                const char *rn = pl[i].pose.boss[q].name;
                if (!rn[0] || strcmp(rn, g_boss[k].name) || pl[i].pose.boss[q].hash != g_boss[k].hash) continue;
                int32_t dmg = pl[i].pose.boss[q].life;
                int s2 = 0;
                while (s2 < BOSS_TRACK - 1 && g_bseen[i][s2].hash && g_bseen[i][s2].hash != g_boss[k].hash) s2++;
                if (g_bseen[i][s2].hash != g_boss[k].hash) {  // (first seen: from now on)
                    g_bseen[i][s2].hash = g_boss[k].hash, g_bseen[i][s2].seen = dmg;
                    continue;
                }
                int32_t d = dmg - g_bseen[i][s2].seen;
                g_bseen[i][s2].seen = dmg;
                if (d <= 0 || *life <= 0) continue;
                if (d > *life) d = *life;
                *life -= d, g_btrack[t].applied += d;
                LOG("[online] %s: %d of %s's damage here (%d left)\n", g_boss[k].name, d, pl[i].name, *life);
            }
        }
    }
}

static void grab_update(Cpu *c, uint64_t player, const OnlinePlayer *pl, int me) {
    for (int i = 0; i < ONLINE_MAX; i++) {
        const OnlineGrab *g = i != me && pl[i].used && pl[i].pose.valid ? &pl[i].pose.grab : NULL;
        bool want = g && g->name[0];
        if (g_act[i].grab) {
            float dx = want ? g->from[0] - g_act[i].grab_from[0] : 0, dz = want ? g->from[2] - g_act[i].grab_from[2] : 0;
            if (!want || strcmp(g->name, g_act[i].grab_name) || dx * dx + dz * dz > 0.25f ||
                !alive(g_act[i].grab, g_act[i].grab_w)) {
                LOG("[online] %s lets go of %s\n", pl[i].name, g_act[i].grab_name);
                g_act[i].grab = 0;
            }
        }
        uint64_t mp = g_act[i].magne.p;
        bool glow = VALID(mp) && *(uint32_t *)(uintptr_t)(mp + 0x20) == g_act[i].magne.id;
        if (!g_act[i].grab && glow) {
            call(c, FN_EFFECT_KILL, (uint64_t)(uintptr_t)&g_act[i].magne, 0, 0, 0, 0, 0);
            memset(&g_act[i].magne, 0, sizeof g_act[i].magne);
            glow = false;
        }
        for (int q = 0; !g_act[i].grab && q < MAGNE_BEAM; q++) {
            uint64_t bp = g_act[i].beam[q].p;
            if (VALID(bp) && *(uint32_t *)(uintptr_t)(bp + 0x20) == g_act[i].beam[q].id)
                call(c, FN_EFFECT_KILL, (uint64_t)(uintptr_t)&g_act[i].beam[q], 0, 0, 0, 0, 0);
            memset(&g_act[i].beam[q], 0, sizeof g_act[i].beam[q]);
        }
        if (!want) {
            g_act[i].grab_name[0] = 0, g_act[i].grab_wait = 0;
            continue;
        }
        if (!g_act[i].grab && (strcmp(g->name, g_act[i].grab_name) || g_act[i].grab_wait-- <= 0)) {
            snprintf(g_act[i].grab_name, sizeof g_act[i].grab_name, "%s", g->name);
            memcpy(g_act[i].grab_from, g->from, sizeof g_act[i].grab_from);
            g_act[i].grab_wait = 30;
            int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
            float best = 4 * 4;
            for (int k = 0; k < na; k++) {
                char n[48];
                actor_name_of(g_actors[k], n);
                if (strcmp(n, g->name)) continue;
                const float *m = (const float *)(uintptr_t)(g_actors[k] + 0x398);
                float d = (m[3] - g->from[0]) * (m[3] - g->from[0]) + (m[7] - g->from[1]) * (m[7] - g->from[1]) +
                          (m[11] - g->from[2]) * (m[11] - g->from[2]);
                if (d < best) best = d, g_act[i].grab = g_actors[k];
            }
            if (g_act[i].grab) {
                g_act[i].grab_w = watch(g_act[i].grab);
                call(c, FN_ACTOR_LOOK_ON, g_act[i].grab, 2, 0, 0, 0, 0);  // (held by Magnesis: its golden glow)
            }
            LOG("[online] %s holds %s with Magnesis: %s\n", pl[i].name, g->name, g_act[i].grab ? "here too" : "not here");
        }
        if (!g_act[i].grab) continue;
        // (its body moved there: the velocities that take it there in a step, 0x70012d4e18; its actor +0x190 the body)
        uint64_t body = *(uint64_t *)(uintptr_t)(g_act[i].grab + 0x190);
        if (VALID(body)) call(c, FN_BODY_TO, body, (uint64_t)(uintptr_t)g->m, 0, 0, 0, 0);
        else call(c, FN_SET_MTX, g_act[i].grab, (uint64_t)(uintptr_t)g->m, 0, 0, 0, 0);
        // its golden glow: the materials' uk_special_effect_B at 1 (the rune keeps it so while it holds; 0 on letting go)
        uint64_t model = *(uint64_t *)(uintptr_t)(g_act[i].grab + 0x4e0);
        if (VALID(model)) call_f(c, FN_SPECIAL_FX_B, 0, model, 0, 0, 0, 0, 0, 1.0f);
        // the rune's sparkles where it is, on the actor the game plays them on (P_MAGNE_FX's +0x30: ReactionHit)
        uint64_t fxo = *(uint64_t *)(uintptr_t)P_MAGNE_FX;
        fxo = VALID(fxo) ? *(uint64_t *)(uintptr_t)fxo : 0;
        uint64_t fxa = VALID(fxo) ? *(uint64_t *)(uintptr_t)(fxo + 0x30) : 0;
        if (!glow && VALID(fxa)) {
            static const char *const kn = "MagneCatchMoving";
            call_sret(c, FN_XLINK_EFFECT, (uint64_t)(uintptr_t)&g_act[i].magne, fxa, (uint64_t)(uintptr_t)kn);
            mp = g_act[i].magne.p;
            glow = VALID(mp) && *(uint32_t *)(uintptr_t)(mp + 0x20) == g_act[i].magne.id;
        }
        if (glow) {
            *(uint16_t *)(uintptr_t)(mp + 0x98) |= 0x100;
            *(float *)(uintptr_t)(mp + 0xbc) = g->m[3];
            *(float *)(uintptr_t)(mp + 0xc0) = g->m[7];
            *(float *)(uintptr_t)(mp + 0xc4) = g->m[11];
            *(uint16_t *)(uintptr_t)(mp + 0x9a) |= 0x100;
        }
        // the beam: a row of the rune's effects from his slate (his Link's right hand) to it, bent up a little as
        // the rune's own beam, each placed again each frame
        float hand[12];
        if (g_act[i].link && VALID(fxa) && bone_mtx(c, g_act[i].link, "Weapon_R", hand)) {
            const char *bn = getenv("SAVAGEWIND_ONLINE_BEAM_FX");
            static const char *kb;
            kb = bn && *bn ? bn : "MagneCatchMoving";
            float a[3] = {hand[3], hand[7], hand[11]}, b[3] = {g->m[3], g->m[7], g->m[11]};
            float d = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
            for (int q = 0; q < MAGNE_BEAM; q++) {
                uint64_t bp = g_act[i].beam[q].p;
                bool on = VALID(bp) && *(uint32_t *)(uintptr_t)(bp + 0x20) == g_act[i].beam[q].id;
                if (!on) {
                    call_sret(c, FN_XLINK_EFFECT, (uint64_t)(uintptr_t)&g_act[i].beam[q], fxa, (uint64_t)(uintptr_t)kb);
                    bp = g_act[i].beam[q].p;
                    on = VALID(bp) && *(uint32_t *)(uintptr_t)(bp + 0x20) == g_act[i].beam[q].id;
                }
                if (!on) break;
                float t = (q + 1.0f) / (MAGNE_BEAM + 1.0f), arc = 4 * t * (1 - t) * d * 0.15f;
                *(uint16_t *)(uintptr_t)(bp + 0x98) |= 0x100;
                *(float *)(uintptr_t)(bp + 0xbc) = a[0] + (b[0] - a[0]) * t;
                *(float *)(uintptr_t)(bp + 0xc0) = a[1] + (b[1] - a[1]) * t + arc;
                *(float *)(uintptr_t)(bp + 0xc4) = a[2] + (b[2] - a[2]) * t;
                *(uint16_t *)(uintptr_t)(bp + 0x9a) |= 0x100;
            }
        }
    }
}

static void ground_update(Cpu *c, uint64_t player, const float link[3]) {
    static int tick;
    if (tick++ % 6) return;
    // the others' picks: gone here too
    char pn[16][40];
    float pp[16][3];
    int np = online_take_picks(pn, pp, 16);
    int na = all_actors(*(uint64_t *)(uintptr_t)(player + 0x68), g_actors, MAX_ACTORS);
    for (int q = 0; q < np; q++)
        for (int k = 0; k < na; k++) {
            char n[48];
            uint64_t a = g_actors[k];
            actor_name_of(a, n);
            if (strcmp(n, pn[q])) continue;
            const float *m = (const float *)(uintptr_t)(a + 0x398);
            float p[3] = {m[3], m[7], m[11]};
            if (!near_link(pp[q], p, 2)) continue;
            LOG("[online] %s picked up by another: gone here\n", n);
            call(c, FN_DELETE, a, 0, 0, 0, 0, 0);
            break;
        }
    // ours, watched: picked up (gone, this Link next to where it lay)
    uint64_t carried[6] = {0};
    for (int k = 0; k < 3; k++) carried[k] = linked(player + 0xb98 + 0x18 * k);
    carried[3] = carried[0] ? linked(carried[0] + 0x958) : 0, carried[4] = carried[2] ? linked(carried[2] + 0x958) : 0;
    for (int g = 0; g < GROUND; g++) {
        if (!g_ground[g].actor) continue;
        if (alive(g_ground[g].actor, g_ground[g].w)) {
            const float *m = (const float *)(uintptr_t)(g_ground[g].actor + 0x398);
            g_ground[g].pos[0] = m[3], g_ground[g].pos[1] = m[7], g_ground[g].pos[2] = m[11];
            bool held = false;
            for (int k = 0; k < 6; k++) held |= carried[k] == g_ground[g].actor;
            if (held || !near_link(link, g_ground[g].pos, 6)) g_ground[g].actor = 0;  // (his now, or far: not watched)
            continue;
        }
        if (near_link(link, g_ground[g].pos, 2.5f)) {
            LOG("[online] %s picked up here\n", g_ground[g].name);
            online_send_pick(g_ground[g].name, g_ground[g].pos);
        }
        g_ground[g].actor = 0;
    }
    for (int k = 0; k < na; k++) {
        char n[48];
        uint64_t a = g_actors[k];
        actor_name_of(a, n);
        if (strncmp(n, "Weapon_", 7)) continue;
        const float *m = (const float *)(uintptr_t)(a + 0x398);
        float p[3] = {m[3], m[7], m[11]};
        if (!near_link(link, p, 4)) continue;
        bool known = false, held = false;
        for (int g = 0; g < GROUND && !known; g++) known = g_ground[g].actor == a;
        for (int j = 0; j < 6; j++) held |= carried[j] == a;
        if (known || held) continue;
        for (int g = 0; g < GROUND; g++)
            if (!g_ground[g].actor) {
                g_ground[g].actor = a, g_ground[g].w = watch(a);
                snprintf(g_ground[g].name, sizeof g_ground[g].name, "%s", n);
                memcpy(g_ground[g].pos, p, 12);
                break;
            }
    }
}

volatile int g_online_block_a;  // (runtime/hle/input.c: A held back)

void rt_online_frame(Cpu *c) {
    if (!online_actors_on()) return;
    int me = online_local_id();
    uint64_t pinfo = g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0;
    float link[3] = {0, 0, 0};
    if (pinfo) memcpy(link, (const void *)(uintptr_t)(pinfo + 0x88), sizeof link);
    bool in_world = pinfo && !framepace_title_ui() && (link[0] != 0 || link[1] != 0 || link[2] != 0);
    bool world = in_world && me >= 0;
    // out of the session (left, or the connection lost) in the world: the players' actors are still there, deleted
    if (in_world && me < 0) {
        bool any = false;
        for (int i = 0; i < ONLINE_MAX; i++) {
            bool has = g_act[i].link != 0;
            for (int k = 0; k < POOL; k++) has |= g_pool[i][k].actor || g_pool[i][k].pending;
            if (has) any = true, drop(c, i, true);
        }
        if (any) LOG("[online] out of the session: the players' actors deleted\n");
        if (pinfo && VALID(*(uint64_t *)(uintptr_t)(pinfo + 0x60)))  // (the host's enemies stay as they were)
            enemies_client(c, *(uint64_t *)(uintptr_t)(pinfo + 0x60), -1);
        booms_forget();
        memcpy(g_last_link, link, sizeof link);
        return;
    }
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
    {   // the time of day: the host's for everyone (his game sends it; the others follow when theirs is off by more than
        // 3 game minutes, a few seconds: the two clocks run at the same speed)
        float mine = test_warp_hour(NULL), host;
        online_set_local_hour(mine);
        if (mine >= 0 && online_host_hour(&host)) {
            float d = host - mine;
            if (d > 12) d -= 24;
            if (d < -12) d += 24;
            if (d > 0.05f || d < -0.05f) {
                test_warp_set_hour(host);
                LOG("[online] the host's time: %.2f (was %.2f)\n", host, mine);
            }
        }
    }
    memcpy(g_pool_at, link, sizeof g_pool_at);
    pool_update(c);
    booms_update(c, *(uint64_t *)(uintptr_t)(pinfo + 0x60));
    orphans_update(c);
    if (g_settle > 0) { g_settle--; return; }
    // dead: game over; nothing made meanwhile, and three seconds more once he is up again. What is still there is kept
    // (an armour piece made again for a player is never drawn): what the game removed is seen gone and made again
    static bool was_dead;
    if (g_local_dead) { was_dead = true; return; }
    if (was_dead) {
        was_dead = false;
        LOG("[online] back after dying\n");
        g_settle = 180;
        return;
    }
    if (getenv("SAVAGEWIND_ONLINE_ENEMY_TEST")) {  // (test: each enemy's online actor made in turn, next to Link)
        static MapLine *m;
        static int n = -1, next, tick;
        static Piece p;
        if (n < 0) n = map_load("enemies.txt", &m);
        if (tick++ % 20 == 0 && next < n) {
            if (p.actor || p.pending) delete_piece(c, &p);
            LOG("[online] enemy test %d: %s\n", next, m[next].val);
            piece_update(c, 0, &p, m[next].val, "test");
            next++;
        } else piece_update(c, 0, &p, p.name, "test");
        if (p.actor) {
            float mm[12] = {1, 0, 0, link[0] + 4, 0, 1, 0, link[1], 0, 0, 1, link[2]};
            call(c, FN_SET_MTX, p.actor, (uint64_t)(uintptr_t)mm, 0, 0, 0, 0);
        }
    }
    if (getenv("SAVAGEWIND_ONLINE_LOOK_LOG")) {  // (test: the actors near whose +0x710 / +0x518 change)
        static uint64_t seen_a[64];
        static uint32_t seen_v[64];
        static int tick;
        uint64_t pl0 = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
        if (VALID(pl0) && tick++ % 3 == 0) {
            int na = all_actors(*(uint64_t *)(uintptr_t)(pl0 + 0x68), g_actors, MAX_ACTORS);
            const float *pm = (const float *)(uintptr_t)(pl0 + 0x398);
            for (int k = 0; k < na; k++) {
                uint64_t a = g_actors[k];
                const float *m = (const float *)(uintptr_t)(a + 0x398);
                if ((m[3] - pm[3]) * (m[3] - pm[3]) + (m[11] - pm[11]) * (m[11] - pm[11]) > 30 * 30) continue;
                uint32_t v = *(uint32_t *)(uintptr_t)(a + 0x710);
                int q = 0;
                while (q < 64 && seen_a[q] != a) q++;
                if (q == 64) { for (q = 0; q < 64 && seen_a[q]; q++) ; if (q == 64) continue; seen_a[q] = a, seen_v[q] = v; continue; }
                if (v != seen_v[q]) {
                    char n[48];
                    actor_name_of(a, n);
                    LOG("[online] look %s: +0x710 %x -> %x\n", n, seen_v[q], v);
                    seen_v[q] = v;
                }
            }
        }
    }
    {   // (test: SAVAGEWIND_ONLINE_DIE_TEST=<s>: the local Link dies that many seconds in)
        static int t;
        const char *dt = getenv("SAVAGEWIND_ONLINE_DIE_TEST");
        uint64_t pl0 = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
        if (dt && VALID(pl0) && ++t == atoi(dt) * 30) *(int32_t *)(uintptr_t)(pl0 + ENEMY_LIFE) = 0, LOG("[online] die test\n");
    }
    if (getenv("SAVAGEWIND_ONLINE_LIFE_LOG")) {  // (test: the local Link's life, as it changes)
        static int32_t last = -12345;
        uint64_t pl0 = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
        int32_t v = VALID(pl0) ? *(int32_t *)(uintptr_t)(pl0 + ENEMY_LIFE) : -1;
        if (v != last) LOG("[online] our life %d\n", v), last = v;
    }
    {   // (test: SAVAGEWIND_ONLINE_KNOCK_TEST=<s>: a heavy blow's knock on the local Link every that many seconds)
        static int t;
        const char *kt = getenv("SAVAGEWIND_ONLINE_KNOCK_TEST");
        uint64_t pl0 = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
        if (kt && VALID(pl0) && ++t % (atoi(kt) * 30) == 0) {
            const float *pm = (const float *)(uintptr_t)(pl0 + 0x398);
            float at[3] = {pm[3] + 1, pm[7] + 0.3f, pm[11]};
            const char *kn = getenv("SAVAGEWIND_ONLINE_KNOCK");
            boom_start(c, kn ? kn : "OnlineShock", at);
            LOG("[online] knock test\n");
        }
    }
    {   // the enemies: the host's for everyone
        uint64_t player = *(uint64_t *)(uintptr_t)(pinfo + 0x60);
        owns_left(c);
        if (VALID(player)) loot_update(c, player, link, me, online_is_host());
        if (getenv("SAVAGEWIND_ONLINE_SLATE_TEST") && VALID(player)) {  // (test: the slate's event here, that many s in)
            static int st;
            if (++st == atoi(getenv("SAVAGEWIND_ONLINE_SLATE_TEST")) * 30) slate_event(c, player);
        }
        quests_update(c, player, online_is_host());
        if (VALID(player)) ground_update(c, player, link);
        if (VALID(player)) {
            OnlinePlayer gp[ONLINE_MAX];
            online_players(gp);
            grab_update(c, player, gp, me);
            stop_update(c, player, gp, me);
            ice_update(c, gp, me);
            boss_update(c, gp, me);
        }
        {   // another player at a Sheikah pedestal within 30 m (not us in an event): our A does nothing meanwhile
            OnlinePlayer bp[ONLINE_MAX];
            online_players(bp);
            int block = 0;
            for (int i = 0; i < ONLINE_MAX; i++) {
                if (i == me || !bp[i].used || !bp[i].pose.valid || !(bp[i].pose.effects & ONLINE_E_DEMO)) continue;
                float dx = bp[i].pos[0] - link[0], dz = bp[i].pos[2] - link[2];
                if (dx * dx + dz * dz < 30 * 30) block = 1;
            }
            extern bool framepace_in_event(void);
            static int dlog;
            if (getenv("SAVAGEWIND_ONLINE_DEMO_LOG") && dlog++ % 150 == 0)
                for (int i = 0; i < ONLINE_MAX; i++)
                    if (bp[i].used && i != me)
                        LOG("[online] demo check: %s valid %d effects %02x at %.0f m, we in event %d\n", bp[i].name, bp[i].pose.valid,
                            bp[i].pose.effects, sqrtf((bp[i].pos[0] - link[0]) * (bp[i].pos[0] - link[0]) +
                                                      (bp[i].pos[2] - link[2]) * (bp[i].pos[2] - link[2])), framepace_in_event());
            if (framepace_in_event()) block = 0;
            if (block != g_online_block_a) LOG("[online] %s\n", block ? "another player is at a Sheikah pedestal near: A held back" : "A back");
            g_online_block_a = block;
        }
        OnlinePlayer epl[ONLINE_MAX];
        online_players(epl);
        static int tick;
        if (VALID(player) && online_is_host()) {
            if (tick++ % 3 == 0) enemies_host(c, player, link, epl, me);
            enemies_show(c, player, g_reg_out, g_reg_n, true, link, me, false);  // (the others': shown here)
        } else if (VALID(player)) enemies_client(c, player, me);
    }

    OnlinePlayer pl[ONLINE_MAX];
    online_players(pl);
    for (int i = 0; i < ONLINE_MAX; i++) {
        bool want = pl[i].used && i != me && !(pl[i].flags & ONLINE_F_LOADING) && same_map(&pl[i]);  // (in a shrine or a
                                                                                                  // Divine Beast: not here)
        if (want) {
            float ex = pl[i].pos[0] - link[0], ez = pl[i].pos[2] - link[2];
            want = ex * ex + ez * ez < 400 * 400;  // (far away: the world there isn't loaded)
            g_dist = sqrtf(ex * ex + ez * ez);
        }
        if (!want) {
            if (g_act[i].link && !framepace_paused()) drop(c, i, !pl[i].used);
            else if (!pl[i].used)  // (left: his pool goes)
                for (int k = 0; k < POOL; k++)
                    if (g_pool[i][k].actor || g_pool[i][k].pending) delete_piece(c, &g_pool[i][k]);
            continue;
        }
        // another player in his slot (one left and another came before it was seen): the first one's actors go
        if (g_act[i].link && strcmp(g_act[i].name, pl[i].name)) {
            LOG("[online] slot %d is %s now, not %s\n", i, pl[i].name, g_act[i].name);
            drop(c, i, true);
            continue;
        }
        // his Link gone, deleted by the game: the world's actors are going (the local Link died: game over; a load
        // not seen by where he is): all forgotten, made again two seconds later
        if (g_act[i].link && !alive(g_act[i].link, g_act[i].link_w)) {
            LOG("[online] the game removed %s's Link: the world's actors are going, made again later (%.0f m away)\n", pl[i].name, g_dist);
            forget_all();
            g_settle = 180;
            return;
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

        // his armour: an actor per piece of its model alone (OnlineArmor_*, tools/online_mod.py) on his Link's matrix,
        // posed bone by bone as his Link (the game's own Armor_* actors, worn on his skeleton, crashed and weren't drawn
        // made a second time)
        for (int k = 0; k < ARMOR; k++) {
            // (none: what the game puts on his Link then: his hair, his shorts; shirtless is his own body)
            const char *w = pl[i].armor[k][0] ? pl[i].armor[k] : k == 0 ? "Armor_Default_Head" : k == 2 ? "Armor_Default_Lower" : "";
            const char *oa = w[0] ? armor_actor(w) : NULL;
            Piece *p = &g_act[i].armor[k];
            bool had = p->actor != 0;
            if (piece_update(c, i, p, oa ? oa : "", pl[i].name)) {
                model_visible(c, p->actor, true);
                g_act[i].covered = -1;
            }
            if (had != (p->actor != 0)) g_act[i].covered = -1;
            if (p->actor) {
                call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
                pose_armor(c, p);
            }
        }
        int pieces = 0;
        bool all_running = true;
        for (int k = 0; k < ARMOR; k++) {
            pieces |= (g_act[i].armor[k].actor != 0) << k;
            if (g_act[i].armor[k].actor) all_running &= running(g_act[i].armor[k].actor);
        }
        if (all_running && pieces != g_act[i].covered) cover(c, i), g_act[i].covered = pieces;

        // Daruk's Protection: the player's own effects on his Link while they are on in his game, "Kago_Shield" (a hit
        // taken) and "Kago_ShieldCount_0<n>" (its ring while he guards) (0x7001100bd0: x8 the handle out, x0 the actor,
        // x1 the effect's name in its ELink; 0x700056d010(handle) ends one)
        for (int k = 0; k < 2; k++) {
            int e = pl[i].pose.valid ? pl[i].pose.effects : 0;
            int n = (e & ONLINE_E_KAGO_COUNT) >> ONLINE_E_KAGO_COUNT_SHIFT;
            bool want_k = k == 0 ? (e & ONLINE_E_KAGO) != 0 : n != 0;
            uint64_t kp = g_act[i].kago[k].p;
            bool on = VALID(kp) && *(uint32_t *)(uintptr_t)(kp + 0x20) == g_act[i].kago[k].id;
            if (on && (!want_k || (k == 1 && n != g_act[i].kago_n))) {
                call(c, FN_EFFECT_KILL, (uint64_t)(uintptr_t)&g_act[i].kago[k], 0, 0, 0, 0, 0);
                memset(&g_act[i].kago[k], 0, sizeof g_act[i].kago[k]);
                on = false;
            }
            if (want_k && !on) {
                static char name[24];
                if (k == 0) snprintf(name, sizeof name, "Kago_Shield");
                else snprintf(name, sizeof name, "Kago_ShieldCount_0%d", n), g_act[i].kago_n = n;
                call_sret(c, FN_EFFECT_EMIT, (uint64_t)(uintptr_t)&g_act[i].kago[k], g_act[i].link, (uint64_t)(uintptr_t)name);
                LOG("[online] %s: %s (%llx)\n", pl[i].name, name, (unsigned long long)g_act[i].kago[k].p);
            }
        }
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
        // what he holds, threw or shot, where it is (out of sight: those he no longer has)
        const OnlineShots *sh = &pl[i].pose.shots;
        bool used[ONLINE_SHOTS] = {false};
        for (int k = 0; pl[i].pose.valid && k < sh->n; k++) {
            int slot = -1;
            for (int j2 = 0; j2 < ONLINE_SHOTS && slot < 0; j2++)
                if (g_act[i].shot_id[j2] == sh->shot[k].id) slot = j2;
            for (int j2 = 0; j2 < ONLINE_SHOTS && slot < 0; j2++) {
                bool in_use = false;
                for (int q = 0; q < sh->n; q++) in_use |= g_act[i].shot_id[j2] == sh->shot[q].id;
                if (!in_use) slot = j2;
            }
            if (slot < 0 || sh->shot[k].kind >= ONLINE_K_COUNT) continue;
            used[slot] = true;
            if (g_act[i].shot_id[slot] != sh->shot[k].id)
                LOG("[online] %s's shot %d (%s) in slot %d\n", pl[i].name, sh->shot[k].id, k_shot_names[sh->shot[k].kind], slot);
            g_act[i].shot_id[slot] = sh->shot[k].id;
            const char *sa = gear_actor(k_shot_names[sh->shot[k].kind]);
            Piece *p = &g_act[i].shot[slot];
            if (piece_update(c, i, p, sa ? sa : "", pl[i].name)) model_visible(c, p->actor, true);
            if (p->actor) call(c, FN_SET_MTX, p->actor, (uint64_t)(uintptr_t)sh->shot[k].m, 0, 0, 0, 0);
            if (getenv("SAVAGEWIND_ONLINE_SHOT_LOG"))
                LOG("[online] shot %d %s at %.2f %.2f %.2f%s\n", sh->shot[k].id, k_shot_names[sh->shot[k].kind], sh->shot[k].m[3],
                    sh->shot[k].m[7], sh->shot[k].m[11], p->actor ? "" : " (being made)");
        }
        for (int k = 0; k < ONLINE_SHOTS; k++) {
            if (used[k]) continue;
            if (g_act[i].shot_id[k])
                LOG("[online] %s's shot %d out of sight (slot %d, actor %llx)\n", pl[i].name, g_act[i].shot_id[k], k,
                    (unsigned long long)g_act[i].shot[k].actor);
            g_act[i].shot_id[k] = 0;
            if (g_act[i].shot[k].actor) {
                float hm[12] = {1, 0, 0, pl[i].pos[0], 0, 1, 0, pl[i].pos[1] - 5000, 0, 0, 1, pl[i].pos[2]};
                call(c, FN_SET_MTX, g_act[i].shot[k].actor, (uint64_t)(uintptr_t)hm, 0, 0, 0, 0);
            }
        }
        // an arrow on his string, in his right hand, while he nocks and draws his bow (the upper body's AS: BowReload,
        // BowCharge...; his bow in a hand), its point (+z) past the bow in his left; out of sight the rest of the time
        {
            Piece *ar = &g_act[i].nock;
            const char *an = pl[i].gear[2][0] ? gear_actor("NormalArrow") : NULL;
            if (piece_update(c, i, ar, an ? an : "", pl[i].name)) model_visible(c, ar->actor, true);
            if (ar->actor) {
                const char *up = pl[i].pose.as[2];
                bool drawn = pl[i].pose.valid && (pl[i].pose.gear_bone[2] == 1 || pl[i].pose.gear_bone[2] == 2) &&
                             (!strncmp(up, "BowCharge", 9) || !strncmp(up, "BowReload", 9));
                float am[12] = {1, 0, 0, pl[i].pos[0], 0, 1, 0, pl[i].pos[1] - 5000, 0, 0, 1, pl[i].pos[2]};
                if (drawn && !(have & 1) && bone_mtx(c, g_act[i].link, k_bones[0], bm[0])) have |= 1;
                if (drawn && !(have & 2) && bone_mtx(c, g_act[i].link, k_bones[1], bm[1])) have |= 2;
                if (drawn && (have & 3) == 3) {
                    float z[3] = {bm[1][3] - bm[0][3], bm[1][7] - bm[0][7], bm[1][11] - bm[0][11]}, l = 0;
                    for (int k = 0; k < 3; k++) l += z[k] * z[k];
                    l = l > 1e-8f ? 1 / sqrtf(l) : 0;
                    for (int k = 0; k < 3; k++) z[k] *= l;
                    float x[3] = {z[2], 0, -z[0]}, lx = sqrtf(x[0] * x[0] + x[2] * x[2]);  // (up x z)
                    if (lx < 1e-4f) x[0] = 1, x[2] = 0, lx = 1;
                    x[0] /= lx, x[2] /= lx;
                    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
                    float m2[12] = {x[0], y[0], z[0], bm[0][3], x[1], y[1], z[1], bm[0][7], x[2], y[2], z[2], bm[0][11]};
                    if (l > 0) memcpy(am, m2, sizeof am);
                }
                call(c, FN_SET_MTX, ar->actor, (uint64_t)(uintptr_t)am, 0, 0, 0, 0);
            }
        }
        // a bomb of his gone off, once by its number: his bomb as shown goes out of sight, and the bomb's own actor
        // without what hurts (OnlineBoom_*, tools/online_mod.py) is made there and set off as the rune sets his off
        for (int k = ONLINE_BOOMS - 1; pl[i].pose.valid && k >= 0; k--) {
            uint16_t seq = sh->boom[k].seq;
            if (!seq || (uint16_t)(seq - g_act[i].boom_seen) > 0x8000u || seq == g_act[i].boom_seen) continue;
            g_act[i].boom_seen = seq;
            if (g_act[i].frames < 60 || sh->boom[k].kind >= ONLINE_K_COUNT) continue;  // (from before he was here)
            const char *ba = gear_actor(k_shot_names[sh->boom[k].kind]);
            for (int q = 0; ba && q < ONLINE_SHOTS; q++)
                if (!used[q] && g_act[i].shot[q].actor && !strcmp(g_act[i].shot[q].name, ba)) {
                    float hm[12] = {1, 0, 0, pl[i].pos[0], 0, 1, 0, pl[i].pos[1] - 5000, 0, 0, 1, pl[i].pos[2]};
                    call(c, FN_SET_MTX, g_act[i].shot[q].actor, (uint64_t)(uintptr_t)hm, 0, 0, 0, 0);
                }
            char key[48];
            snprintf(key, sizeof key, "Boom_%s", k_shot_names[sh->boom[k].kind]);
            const char *boom = getenv("SAVAGEWIND_BOOM_ACTOR") ? getenv("SAVAGEWIND_BOOM_ACTOR") : gear_actor(key);
            if (boom) boom_start(c, boom, sh->boom[k].pos);
            LOG("[online] %s's bomb went off at %.1f %.1f %.1f\n", pl[i].name, sh->boom[k].pos[0], sh->boom[k].pos[1],
                sh->boom[k].pos[2]);
        }
    }
}
