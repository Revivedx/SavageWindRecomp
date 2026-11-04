// Options > Accessibility (settings.h): the assists, each off by default. Their patches in the game code
// (tools/patches.py) ask here while it runs; a game version whose code has no place for one (tools/game_sites.py
// OPTIONAL_GROUPS) goes without it.
#include <stdint.h>
#include <string.h>

#include "settings.h"

// The damage multipliers: d percent of a loss, rounded, and at least 1 while there is a loss
static int32_t scaled(int32_t d, int pct) {
    if (d <= 0 || pct == 100) return d;
    int32_t s = (int32_t)(((int64_t)d * pct + 50) / 100);
    return s < 1 ? 1 : s;
}

// The cost a hit takes off a life (tools/patches.py _no_wear_hit: every life goes through that function), by whose life
// it is: a weapon's, bow's or shield's durability (LIFE_WEAPON: Unbreakable weapons, from #296), Link's hearts
// (LIFE_LINK: left to rt_link_life, which sees every loss of his), or anything else's (One-hit kill, the damage dealt).
enum { LIFE_OTHER, LIFE_WEAPON, LIFE_LINK };
uint32_t rt_life_cost(uint32_t cost, int32_t life, int whose) {
    if (whose == LIFE_WEAPON) return g_settings.no_weapon_wear ? 0 : cost;
    if (whose == LIFE_LINK || (int32_t)cost <= 0) return cost;
    if (g_settings.one_hit_kill) return life > (int32_t)cost ? (uint32_t)life : cost;
    return (uint32_t)scaled((int32_t)cost, g_settings.damage_dealt);
}

// Unbreakable weapons (from #296), the pouch's copy: its setter for an equipped item's value doesn't store a lower one
// (tools/patches.py _no_wear). Arrows (type 2) are counts, not wear, and are left alone.
int rt_keep_wear(uint32_t type, int32_t newv, int32_t oldv) {
    return g_settings.no_weapon_wear && type != 2 && newv < oldv;
}

// God mode, the damage taken: Link's damage handling stores his new life, in quarter hearts, through here (tools/patches.py
// _link_life): God mode keeps the old one, the multiplier scales the loss.
int32_t rt_link_life(int32_t oldv, int32_t newv) {
    if (newv >= oldv) return newv;
    if (g_settings.god_mode) return oldv;
    int32_t v = oldv - scaled(oldv - newv, g_settings.damage_taken);
    return v < 0 ? 0 : v;
}

// Infinite stamina: the stamina setter keeps the old value instead of storing a lower one (tools/patches.py _stamina)
uint32_t rt_stamina(uint32_t oldv, uint32_t newv) {
    float o, n;
    memcpy(&o, &oldv, 4), memcpy(&n, &newv, 4);
    return g_settings.infinite_stamina && n < o ? oldv : newv;
}

// Infinite arrows: the bow's shot takes its arrows off the pouch through one function, which returns before taking
// any (tools/patches.py _remove_arrows). One arrow is still needed to shoot.
int rt_infinite_arrows(void) { return g_settings.infinite_arrows; }
