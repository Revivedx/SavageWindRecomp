// Options > Accessibility (settings.h): the assists, each off by default. Their patches in the game code
// (tools/patches.py) ask here while it runs; a game version whose code has no place for one (tools/game_sites.py
// OPTIONAL_GROUPS) goes without it.
#include <stdint.h>
#include <string.h>

#include "hle.h"
#include "platform.h"
#include "settings.h"
#include "rt.h"

static uint64_t link_damage_ns;
RT_ON_RESET(assists_reset) { __atomic_store_n(&link_damage_ns, 0, __ATOMIC_RELAXED); }
uint64_t rt_link_damage_ns(void) { return __atomic_load_n(&link_damage_ns, __ATOMIC_RELAXED); }

// The damage multipliers: d percent of a loss, rounded, and at least 1 while there is a loss
static int32_t scaled(int32_t d, int pct) {
    if (d <= 0 || pct == 100) return d;
    int32_t s = (int32_t)(((int64_t)d * pct + 50) / 100);
    return s < 1 ? 1 : s;
}

// The cost a hit takes off a life (tools/patches.py _no_wear_hit: every life goes through that function), by whose life
// it is: a weapon's, bow's or shield's durability (LIFE_WEAPON: Unbreakable weapons, from #296, else Weapon wear's share
// of the game's cost, rounded down: a light hit at a low setting can cost nothing), Link's hearts
// (LIFE_LINK: left to rt_link_life, which sees every loss of his), or anything else's (One-hit kill, the damage dealt).
enum { LIFE_OTHER, LIFE_WEAPON, LIFE_LINK };
uint32_t rt_life_cost(uint32_t cost, int32_t life, int whose) {
    if (whose == LIFE_WEAPON) {
        if (g_settings.no_weapon_wear || g_settings.weapon_wear <= 0) return 0;
        int pct = g_settings.weapon_wear;
        return pct == 100 || (int32_t)cost <= 0 ? cost : (uint32_t)((uint64_t)cost * (uint32_t)pct / 100);
    }
    if (whose == LIFE_LINK || (int32_t)cost <= 0) return cost;
    if (g_settings.one_hit_kill) return life > (int32_t)cost ? (uint32_t)life : cost;
    return (uint32_t)scaled((int32_t)cost, g_settings.damage_dealt);
}

// Unbreakable weapons (from #296), the pouch's copy: its setter for an equipped item's value doesn't store a lower one
// (tools/patches.py _no_wear). Arrows (type 2) are counts, not wear, and are left alone.
int rt_keep_wear(uint32_t type, int32_t newv, int32_t oldv) {
    return (g_settings.no_weapon_wear || g_settings.weapon_wear <= 0) && type != 2 && newv < oldv;
}

// God mode, the damage taken: Link's damage handling stores his new life, in quarter hearts, through here (tools/patches.py
// _link_life): God mode keeps the old one, the multiplier scales the loss.
int32_t rt_link_life(int32_t oldv, int32_t newv) {
    if (newv >= oldv) return newv;
    __atomic_store_n(&link_damage_ns, plat_now_ns(), __ATOMIC_RELAXED);
    if (g_settings.god_mode) return oldv;
    int32_t v = oldv - scaled(oldv - newv, g_settings.damage_taken);
    return v < 0 ? 0 : v;
}

// Infinite stamina and stamina drain multiplier: the stamina setter keeps the old value or scales the drain (#968).
uint32_t rt_stamina(uint32_t oldv, uint32_t newv) {
    float o, n;
    memcpy(&o, &oldv, 4), memcpy(&n, &newv, 4);
    if (n >= o) return newv;
    if (g_settings.infinite_stamina) return oldv;
    int pct = g_settings.stamina_drain;
    if (pct <= 0) return oldv;
    if (pct == 100) return newv;
    float s = o - (o - n) * ((float)pct / 100.0f);
    if (s < 0.0f) s = 0.0f;
    uint32_t out;
    memcpy(&out, &s, 4);
    return out;
}

// Infinite arrows: the bow's shot takes its arrows off the pouch through one function, which returns before taking
// any (tools/patches.py _remove_arrows). One arrow is still needed to shoot.
int rt_infinite_arrows(void) { return g_settings.infinite_arrows; }

// Silent Link: the game's noise setter, called with (object, channel, value) for one of an actor's four noise channels
// (tools/patches.py _silent_link). Channel 1 is the one whistling and the sounds like it go through; the HUD's sound
// wave and the enemies' hearing both read it, so keeping it at zero is what makes Link quiet. The other channels and
// the other actors are left alone.
int rt_silent_link(uint64_t object, uint32_t channel) {
    if (!g_settings.silent_link || channel != 1) return 0;
    if (object < GUEST_LO || object >= GUEST_HI) return 0;
    *(volatile float *)(uintptr_t)(object + 0x1c) = 0.0f;  // (+0x18: the four channels, one float each)
    return 1;
}

// Champion abilities (tools/patches.py _hero_soul_*): the game's own "can this be used" checks return 0 in shrines, in
// the Trial of the Sword and while the ability recharges. With the option on the check passes, so the ability works
// from the start of the game and in those places; off, the check runs as the game wrote it.
int rt_hero_soul(int which) {
    switch (which) {
    case 0: return g_settings.hero_soul_wind;
    case 1: return g_settings.hero_soul_electric;
    case 2: return g_settings.hero_soul_fire;
    case 3: return g_settings.hero_soul_water;
    }
    return 0;
}
