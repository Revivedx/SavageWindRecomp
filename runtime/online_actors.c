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
#define P_CREATOR 0x7001d66550ull
#define P_HEAP_OWNER 0x7001d66558ull

bool framepace_title_ui(void);
bool framepace_paused(void);

static struct {
    uint64_t actor;
    char name[ONLINE_NAME_MAX];
    int fails;
} g_act[ONLINE_MAX];
static float g_last_link[3];
static int g_settle;  // frames to wait after a load before creating anything

static const char *actor_name(void) {
    const char *n = getenv("SAVAGEWIND_ONLINE_ACTOR");
    return n && *n ? n : NULL;
}

static bool online_actors_on(void) {
    static int on = -1;
    if (on < 0) on = actor_name() != NULL;
    return on;
}

// a call into the game with the frame loop's registers kept as they were
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    static Cpu saved;  // (the main thread only)
    memcpy(&saved, c, sizeof saved);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5;
    c->sp = (c->sp - 0x400) & ~0xFull;  // (room below the frame loop's own stack frame)
    guest_call(c, fn);
    uint64_t r = c->x[0];
    memcpy(c, &saved, sizeof saved);
    return r;
}

static void forget_all(void) {
    for (int i = 0; i < ONLINE_MAX; i++) g_act[i].actor = 0, g_act[i].fails = 0;
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
            memcpy(g_act[i].name, pl[i].name, sizeof g_act[i].name);
            LOG("[online] %s is a %s (actor %llx)\n", pl[i].name, actor_name(), (unsigned long long)a);
        }
        // the matrix: turned about y by the heading (forward is +z), at the player's feet
        float s = sinf(pl[i].yaw), co = cosf(pl[i].yaw);
        float m[12] = {co, 0, s, pl[i].pos[0], 0, 1, 0, pl[i].pos[1], -s, 0, co, pl[i].pos[2]};
        call(c, FN_SET_MTX, g_act[i].actor, (uint64_t)(uintptr_t)m, 0, 0, 0, 0);
    }
}
