// Options > Accessibility (settings.h): the assists, each off by default. Their patches in the game code
// (tools/patches.py) ask here while it runs; a game version whose code has no place for one (tools/game_sites.py
// OPTIONAL_GROUPS) goes without it.
#include <stdint.h>

#include "settings.h"

// Unbreakable weapons (from #296): two places hold a weapon's, bow's or shield's durability, in hundredths, and both are
// patched (tools/patches.py _no_wear_hit, _no_wear). The weapon actor's own copy, which is what breaks: a hit's cost
// is 0 when the call comes from the weapon's update. The pouch's setter for an equipped item's value: a lower value is
// not stored. Arrows (type 2) are counts, not wear, and are left alone.
int rt_no_wear(void) { return g_settings.no_weapon_wear; }
int rt_keep_wear(uint32_t type, int32_t newv, int32_t oldv) {
    return g_settings.no_weapon_wear && type != 2 && newv < oldv;
}

// Infinite arrows: the bow's shot takes its arrows off the pouch through one function, which returns before taking
// any (tools/patches.py _remove_arrows). One arrow is still needed to shoot.
int rt_infinite_arrows(void) { return g_settings.infinite_arrows; }
