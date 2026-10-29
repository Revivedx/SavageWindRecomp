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
#define P_CREATOR 0x7001d66550ull
#define P_HEAP_OWNER 0x7001d66558ull

bool framepace_title_ui(void);
bool framepace_paused(void);

static struct {
    uint64_t actor;
    char name[ONLINE_NAME_MAX];
    int fails;
    int anim;  // the AS playing (ONLINE_A_*; -1 none yet)
    int as_fails;
} g_act[ONLINE_MAX];

// tools/online_mod.py ANIMS: the AS names of OnlinePlayer's one-clip animations, in ONLINE_A_* order
static const char *const k_anims[ONLINE_A_COUNT] = {
    "OnlineWait", "OnlineWalk", "OnlineRun", "OnlineDash", "OnlineJump", "OnlineFall", "OnlineGlide",
    "OnlineSwimWait", "OnlineSwim", "OnlineClimbWait", "OnlineClimb", "OnlineSquatWait", "OnlineSquat"};
static float g_last_link[3];
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

// play OnlinePlayer's animation `anim` on actor (blended over a few frames); true when it plays
static bool play_anim(Cpu *c, uint64_t actor, int anim) {
    uint64_t fake_action[2] = {0, actor};
    const char *name = k_anims[anim];
    call_f(c, FN_PLAY_AS, (uint64_t)(uintptr_t)fake_action, (uint64_t)(uintptr_t)name, 1, 0, 0, 0, 6.0f);
    const char *now = playing(c, actor);
    bool ok = !strcmp(now, name);
    if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG")) {
        uint64_t mgr = *(uint64_t *)(uintptr_t)(actor + 0x560);
        LOG("[online] AS %s on %llx: now %s (manager %llx: +d8 %llx, slots %u, +c0 %llx)\n", name,
            (unsigned long long)actor, now, (unsigned long long)mgr,
            mgr ? (unsigned long long)*(uint64_t *)(uintptr_t)(mgr + 0xd8) : 0ull,
            mgr ? *(uint32_t *)(uintptr_t)(mgr + 0xb8) : 0u,
            mgr ? (unsigned long long)*(uint64_t *)(uintptr_t)(mgr + 0xc0) : 0ull);
        static int dumped;
        uint64_t param = *(uint64_t *)(uintptr_t)(actor + 0x570);
        uint64_t asl = param ? *(uint64_t *)(uintptr_t)(param + 0x98) : 0;
        if (asl && !dumped++) {
            uint32_t n = *(uint32_t *)(uintptr_t)(asl + 0x2b8);
            uint64_t e = *(uint64_t *)(uintptr_t)(asl + 0x2c0);
            LOG("[online] AS list %llx: %u, file %s\n", (unsigned long long)asl, n, *(const char **)(uintptr_t)(asl + 0x1e8));
            for (uint32_t k = 0; k < n && k < 40; k++) {
                const uint8_t *en = (const uint8_t *)(uintptr_t)(e + k * 0x88);
                LOG("[online]   %u: %s\n", k, *(const char **)(en + 0x18 + 8));
            }
        }
    }
    return ok;
}

static void forget_all(void) {
    for (int i = 0; i < ONLINE_MAX; i++) g_act[i].actor = 0, g_act[i].fails = 0, g_act[i].anim = -1, g_act[i].as_fails = 0;
}

static void drop(Cpu *c, int i) {
    if (g_act[i].actor) {
        LOG("[online] removing %s's actor %llx\n", g_act[i].name, (unsigned long long)g_act[i].actor);
        call(c, FN_DELETE, g_act[i].actor, 0, 0, 0, 0, 0);
    }
    g_act[i].actor = 0;
}

void rt_online_frame(Cpu *c) {
    if (!online_actors_on()) return;
    static unsigned calls;
    if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG") && calls++ % 600 == 0) LOG("[online] frame hook %u\n", calls);
    int me = online_local_id();
    uint64_t pinfo = g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0;
    float link[3] = {0, 0, 0};
    if (pinfo) memcpy(link, (const void *)(uintptr_t)(pinfo + 0x88), sizeof link);
    bool world = pinfo && me >= 0 && !framepace_title_ui() && (link[0] != 0 || link[1] != 0 || link[2] != 0);
    // a load (no Link, or Link somewhere else at once): the game has removed the world's actors, ours with them
    float dx = link[0] - g_last_link[0], dy = link[1] - g_last_link[1], dz = link[2] - g_last_link[2];
    if (!world || dx * dx + dy * dy + dz * dz > 100 * 100) {
        bool any = false;
        for (int i = 0; i < ONLINE_MAX; i++) any |= g_act[i].actor != 0;
        if (any) LOG("[online] a load: the players' actors are gone\n");
        forget_all();
        g_settle = 60;
    }
    memcpy(g_last_link, link, sizeof link);
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
            if (g_act[i].actor && !framepace_paused()) drop(c, i);
            continue;
        }
        if (!g_act[i].actor) {
            if (g_act[i].fails > 10) continue;
            uint64_t creator = *(uint64_t *)(uintptr_t)P_CREATOR, owner = *(uint64_t *)(uintptr_t)P_HEAP_OWNER;
            creator = creator ? *(uint64_t *)(uintptr_t)creator : 0;
            owner = owner ? *(uint64_t *)(uintptr_t)owner : 0;
            if (!creator || !owner) continue;
            uint64_t heap = *(uint64_t *)(uintptr_t)(owner + 0x28);
            uint64_t a = call(c, FN_CREATE, creator, (uint64_t)(uintptr_t)actor_name(), heap, 0, 0, 0);
            if (!a) {
                if (g_act[i].fails++ == 0) LOG("[online] can't create a %s for %s (not loaded?)\n", actor_name(), pl[i].name);
                continue;
            }
            g_act[i].actor = a;
            g_act[i].anim = -1, g_act[i].as_fails = 0;
            memcpy(g_act[i].name, pl[i].name, sizeof g_act[i].name);
            LOG("[online] %s is a %s (actor %llx)\n", pl[i].name, actor_name(), (unsigned long long)a);
        }
        // the matrix: turned about y by the heading (forward is +z), at the player's feet
        float s = sinf(pl[i].yaw), co = cosf(pl[i].yaw);
        float m[12] = {co, 0, s, pl[i].pos[0], 0, 1, 0, pl[i].pos[1], -s, 0, co, pl[i].pos[2]};
        static unsigned n;
        if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG") && n < 3) LOG("[online] setting the matrix of %llx\n", (unsigned long long)g_act[i].actor);
        call(c, FN_SET_MTX, g_act[i].actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
        // (once it runs: BaseProc state 1, calc; right after its creation, 0, its AS cannot play yet)
        if (is_online_player() && pl[i].anim != g_act[i].anim && pl[i].anim < ONLINE_A_COUNT &&
            *(uint8_t *)(uintptr_t)(g_act[i].actor + 0x64) == 1) {
            if (play_anim(c, g_act[i].actor, pl[i].anim) || ++g_act[i].as_fails > 30) g_act[i].anim = pl[i].anim;
        }
        if (getenv("SAVAGEWIND_ONLINE_ACTOR_LOG") && n++ % 120 == 0) {
            const float *a = (const float *)(uintptr_t)(g_act[i].actor + 0x398);
            LOG("[online] actor %llx mtx t %.2f %.2f %.2f (want %.2f %.2f %.2f) state %02x flags %08x\n",
                (unsigned long long)g_act[i].actor, a[3], a[7], a[11], m[3], m[7], m[11],
                *(uint8_t *)(uintptr_t)(g_act[i].actor + 0x64), *(uint32_t *)(uintptr_t)(g_act[i].actor + 0x78));
        }
    }
}
