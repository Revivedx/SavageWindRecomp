// Online play: the other players as actors in the world (runtime/online.h). Once per frame, on the game's frame loop
// (tools/patches.py _vfr_ratio, ksys::VFR::updateInterval on the main thread, before the actors' update), each remote
// player gets an actor of its own, created the way the game creates the Remote Bomb rune's bomb, and its matrix is set
// to where that player is.
//
// 1.9.0 (no symbols; read from the code):
//   0x7001535900  create an actor now, by name: x0 the creator (**0x7001d66550), x1 the actor's name, x2 a heap
//                 (*(**0x7001d66558 + 0x28)), x3 0, w4, w5 0 -> the actor or 0 (its resources aren't loaded). The
//                 Remote Bomb rune (0x7000b3bb40) calls it with "RemoteBomb" / "RemoteBombCube" and then
//   0x700152261c  set an actor's matrix (x0 actor, x1 Matrix34f *, x2 scale or 0): the 3x4 at actor +0x398
//   0x7001514bac  delete an actor (x0 actor, w1 0), as 0x7001535900 does with one that failed its check
//
// Experimental, off unless SAVAGEWIND_ONLINE_ACTOR=<name> picks an actor (docs/ONLINE.md: what was tried). Without it
// the other players are only the overlay's figures (runtime/ui/online_ui.mm).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game_sites.h"
#include "online.h"
#include "rt.h"

#define FN_CREATE 0x7001535900ull
#define FN_SET_MTX 0x700152261cull
#define FN_DELETE 0x7001514bacull
#define FN_AS_IN_SLOT 0x70014a0040ull
#define FN_PLAY_AS 0x700151e42cull  // an AI action's "play this AS" (x0 the action: its actor at +8; x1 the AS name, w2
                                    // not again if already playing, w3 w4 0, s0 the blend, -1 its own); the AS
                                    // manager is the actor's +0x560 (ForkASPlay's enter, 0x7000759e74, calls it)
#define FN_FIND_MATERIAL 0x7000f777c4ull  // gsys model, SafeString * -> unit (low 16) and material (high 16)
                                         // (SetMaterialVisible's invoke, 0x7000731828: the model is actor +0x4e0,
                                         // its units at +0x40, unit->vtable[0x1b0](unit, material, visible))
#define P_SAFE_STRING_VT 0x7001d66260ull  // (sead::SafeString's vtable, +0x10)
#define P_CREATOR 0x7001d66550ull
#define P_HEAP_OWNER 0x7001d66558ull

bool framepace_title_ui(void);
bool framepace_paused(void);

// A remote player in the world: his Link and the armour pieces he wears (tools/online_mod.py ARMOR), each an actor
// of its own on the same matrix, playing the same AS from the same frame (so the armour moves with him)
#define PARTS 4  // Link, head, upper body, lower body
static struct {
    uint64_t actor[PARTS];   // [0] Link (OnlinePlayer); 0: none
    char part[PARTS][48];    // the actor each is
    char name[ONLINE_NAME_MAX];
    int fails[PARTS];
    int anim;                // the AS all parts play (ONLINE_A_*; -1: none yet, or a part needs it again)
    int as_fails;
    int skin;                // the body materials shown (SKIN_*; -1: not set yet)
} g_act[ONLINE_MAX];

// tools/online_mod.py ANIMS: the AS names of OnlinePlayer's one-clip animations, in ONLINE_A_* order
static const char *const k_anims[ONLINE_A_COUNT] = {
    "OnlineWait", "OnlineWalk", "OnlineRun", "OnlineDash", "OnlineJump", "OnlineFall", "OnlineGlide",
    "OnlineSwimWait", "OnlineSwim", "OnlineClimbWait", "OnlineClimb", "OnlineSquatWait", "OnlineSquat"};
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
                       float s0) {
    static Cpu saved;  // (the main thread only)
    memcpy(&saved, c, sizeof saved);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5;
    union { float f; uint32_t u; } v = {s0};
    c->v[0] = v.u;
    c->sp = (c->sp - 0x400) & ~0xFull;  // (room below the frame loop's own stack frame)
    guest_call(c, fn);
    uint64_t r = c->x[0];
    memcpy(c, &saved, sizeof saved);
    return r;
}

static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return call_f(c, fn, a0, a1, a2, a3, a4, a5, 0);
}

// The AS actor plays in slot 0 (0x70014a0040: AS manager, slot -> the AS, its name at +8; "" for none)
static const char *playing(Cpu *c, uint64_t actor) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)(actor + 0x560);
    uint64_t as = mgr ? call(c, FN_AS_IN_SLOT, mgr, 0, 0, 0, 0, 0) : 0;
    const char *now = as ? *(const char **)(uintptr_t)(as + 8) : NULL;
    return now ? now : "";
}

// play OnlinePlayer's animation `anim` on actor (blended over a few frames; again: even if it plays it already, from
// its start); true when it plays
static bool play_anim(Cpu *c, uint64_t actor, int anim, bool again) {
    uint64_t fake_action[2] = {0, actor};
    const char *name = k_anims[anim];
    call_f(c, FN_PLAY_AS, (uint64_t)(uintptr_t)fake_action, (uint64_t)(uintptr_t)name, again ? 0 : 1, 0, 0, 0, 6.0f);
    const char *now = playing(c, actor);
    if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG")) LOG("[online] AS %s on %llx: now %s\n", name, (unsigned long long)actor, now);
    return !strcmp(now, name);
}

static void forget_all(void) {
    memset(g_act, 0, sizeof g_act);
    for (int i = 0; i < ONLINE_MAX; i++) g_act[i].anim = -1, g_act[i].skin = -1;
}

static void drop(Cpu *c, int i) {
    for (int k = 0; k < PARTS; k++) {
        if (g_act[i].actor[k]) call(c, FN_DELETE, g_act[i].actor[k], 0, 0, 0, 0, 0);
        g_act[i].actor[k] = 0;
    }
    LOG("[online] removed %s's Link\n", g_act[i].name);
    g_act[i].anim = -1, g_act[i].skin = -1;
}

// online_mod/armor.txt (tools/online_mod.py): each armour piece it has an online actor for, "piece=actor" a line
// (upgrades and dyes share their piece's model, and its actor)
static char (*g_armor_map)[2][48];
static int g_armor_n = -1;
static const char *armor_actor(const char *piece) {
    if (g_armor_n < 0) {
        g_armor_n = 0;
        const char *ov = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char p[1100], line[128];
        snprintf(p, sizeof p, "%s/armor.txt", ov ? ov : "");
        FILE *f = ov && *ov ? fopen(p, "r") : NULL;
        int cap = 0;
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (g_armor_n == cap) g_armor_map = realloc(g_armor_map, sizeof *g_armor_map * (cap = cap ? cap * 2 : 128));
            snprintf(g_armor_map[g_armor_n][0], 48, "%s", line);
            snprintf(g_armor_map[g_armor_n][1], 48, "%s", eq + 1);
            g_armor_n++;
        }
        if (f) fclose(f);
        LOG("[online] %d armour pieces for the other players\n", g_armor_n);
    }
    for (int i = 0; i < g_armor_n; i++)
        if (!strcmp(g_armor_map[i][0], piece)) return g_armor_map[i][1];
    return NULL;
}

// The armour a player wears, as online actors (OnlineArmor_XXX, tools/online_mod.py): what he sent (pieces online_mod
// has), or SAVAGEWIND_ONLINE_ARMOR=<actor>,... (testing)
static int armor_of(const OnlinePlayer *p, char out[PARTS - 1][48]) {
    const char *e = getenv("SAVAGEWIND_ONLINE_ARMOR");
    if (!e || !*e) {
        int n = 0;
        for (int k = 0; k < 3; k++) {
            const char *a = p->armor[k][0] ? armor_actor(p->armor[k]) : NULL;
            // (no helmet, or one online_mod lacks: his hair, Link's default head)
            if (!a && k == 0) a = armor_actor("Armor_Default_Head");
            if (a) snprintf(out[n++], 48, "%s", a);
        }
        return n;
    }
    int n = 0;
    while (*e && n < PARTS - 1) {
        const char *comma = strchr(e, ',');
        size_t l = comma ? (size_t)(comma - e) : strlen(e);
        snprintf(out[n++], 48, "%.*s", (int)(l < 47 ? l : 47), e);
        e += l + (comma ? 1 : 0);
    }
    return n;
}

static uint64_t create(Cpu *c, const char *name) {
    uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
    creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
    owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
    if (!creator || !owner) return 0;
    uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
    return call(c, FN_CREATE, creator, (uint64_t)(uintptr_t)name, heap, 0, 0, 0);
}


// Link's body under his armour: the player's code hides the parts of his model an armour piece covers; so does this
// (materials of the Link model: Mt_Upper_Skin, Mt_Lower_Skin, Mt_Underwear, Mt_Belt_A, Mt_Belt_C)
enum { SKIN_UPPER = 1, SKIN_LOWER = 2 };  // shown

static bool set_material_visible(Cpu *c, uint64_t actor, const char *material, bool visible) {
    uint64_t model = *(uint64_t *)(uintptr_t)(actor + 0x4e0);
    if (!model) return false;
    uint64_t name[2] = {*(uint64_t *)(uintptr_t)P_SAFE_STRING_VT + 0x10, (uint64_t)(uintptr_t)material};
    uint32_t r = (uint32_t)call(c, FN_FIND_MATERIAL, model, (uint64_t)(uintptr_t)name, 0, 0, 0, 0);
    if (r >= 0xffff0000u || (r & 0xffff) == 0xffff) return false;  // (not found)
    int16_t unit_i = (int16_t)(r & 0xffff);
    uint64_t units = *(uint64_t *)(uintptr_t)(model + 0x40);
    uint64_t unit = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(units + 8 * unit_i));
    if (!unit) return false;
    uint64_t fn = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)unit + 0x1b0);
    call(c, fn, unit, r >> 16, visible, 0, 0, 0);
    return true;
}

static void set_skin(Cpu *c, uint64_t link, int shown) {
    set_material_visible(c, link, "Mt_Upper_Skin", shown & SKIN_UPPER);
    set_material_visible(c, link, "Mt_Lower_Skin", shown & SKIN_LOWER);
    set_material_visible(c, link, "Mt_Underwear", shown & SKIN_LOWER);
    set_material_visible(c, link, "Mt_Belt_A", shown & SKIN_LOWER);
    set_material_visible(c, link, "Mt_Belt_C", shown & SKIN_LOWER);
}

static bool running(uint64_t actor) { return actor && *(uint8_t *)(uintptr_t)(actor + 0x64) == 1; }  // (calc)

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
        for (int i = 0; i < ONLINE_MAX; i++) any |= g_act[i].actor[0] != 0;
        if (any) LOG("[online] a load: the players' actors are gone\n");
        forget_all();
        g_settle = 60;
    }
    memcpy(g_last_link, link, sizeof link);
    for (int i = 0; i < ONLINE_MAX; i++) g_online_actor[i] = world && running(g_act[i].actor[0]);
    if (!world) return;
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
            if (g_act[i].actor[0] && !framepace_paused()) drop(c, i);
            continue;
        }
        // the parts he should have: Link, then his armour; a part that changed is removed and made again
        char want_part[PARTS][48] = {{0}};
        snprintf(want_part[0], 48, "%s", actor_name());
        if (is_online_player()) armor_of(&pl[i], &want_part[1]);
        for (int k = 0; k < PARTS; k++) {
            if (g_act[i].actor[k] && strcmp(g_act[i].part[k], want_part[k])) {
                call(c, FN_DELETE, g_act[i].actor[k], 0, 0, 0, 0, 0);
                g_act[i].actor[k] = 0;
            }
            snprintf(g_act[i].part[k], 48, "%s", want_part[k]);
            if (g_act[i].actor[k] || !want_part[k][0] || g_act[i].fails[k] > 10) continue;
            uint64_t a = create(c, want_part[k]);
            if (!a) {
                if (g_act[i].fails[k]++ == 0) LOG("[online] can't create a %s for %s (not loaded?)\n", want_part[k], pl[i].name);
                continue;
            }
            g_act[i].actor[k] = a;
            g_act[i].anim = -1;  // (all parts start the AS again together: the new one in step)
            if (k == 0) g_act[i].skin = -1;
            memcpy(g_act[i].name, pl[i].name, sizeof g_act[i].name);
            LOG("[online] %s: %s (actor %llx)\n", pl[i].name, want_part[k], (unsigned long long)a);
        }
        // the matrix: turned about y by the heading (forward is +z), at the player's feet
        float s = sinf(pl[i].yaw), co = cosf(pl[i].yaw);
        float m[12] = {co, 0, s, pl[i].pos[0], 0, 1, 0, pl[i].pos[1], -s, 0, co, pl[i].pos[2]};
        bool all_running = true;
        for (int k = 0; k < PARTS; k++)
            if (g_act[i].actor[k]) {
                call(c, FN_SET_MTX, g_act[i].actor[k], (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
                all_running &= running(g_act[i].actor[k]);
            }
        // his body under his armour (once his Link runs: its model is there)
        if (is_online_player() && running(g_act[i].actor[0])) {
            int shown = SKIN_UPPER | SKIN_LOWER;
            for (int k = 1; k < PARTS; k++) {
                const char *pn = g_act[i].actor[k] ? g_act[i].part[k] : "";
                size_t l = strlen(pn);
                if (l > 6 && !strcmp(pn + l - 6, "_Upper")) shown &= ~SKIN_UPPER;
                if (l > 6 && !strcmp(pn + l - 6, "_Lower")) shown &= ~SKIN_LOWER;
            }
            if (shown != g_act[i].skin) set_skin(c, g_act[i].actor[0], shown), g_act[i].skin = shown;
        }
        // the AS: once all parts run (BaseProc state 1, calc; right after its creation, 0, an actor's AS can't play),
        // on every part in the same frame
        if (is_online_player() && all_running && pl[i].anim < ONLINE_A_COUNT && pl[i].anim != g_act[i].anim) {
            bool again = g_act[i].anim < 0, ok = true;
            for (int k = 0; k < PARTS; k++)
                if (g_act[i].actor[k]) ok &= play_anim(c, g_act[i].actor[k], pl[i].anim, again);
            if (ok || ++g_act[i].as_fails > 30) g_act[i].anim = pl[i].anim, g_act[i].as_fails = 0;
        }
    }
}
