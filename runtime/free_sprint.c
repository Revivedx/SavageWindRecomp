// Cheats > Free sprint (settings.h free_sprint): sprinting on the ground takes no stamina while no enemy is fighting
// Link. Two patches in the game code ask here (tools/patches.py _sprint_cost, _battle_info): Link's Move action for
// what a second of sprinting and a sprint press cost, and the battle music's controller with the count the field
// battle music starts on (1 or more) and fades out on (0), which is what "in combat" means here.
#include <stdbool.h>
#include <stdint.h>

#include "hle.h"
#include "rt.h"
#include "settings.h"

static int32_t battle;  // the count the battle music last got: 0 out of combat (set on the sound thread)

void rt_battle_info(int32_t n) {
    int32_t was = __atomic_exchange_n(&battle, n, __ATOMIC_RELAXED);
    if (g_settings.free_sprint && (was > 0) != (n > 0)) LOG("[sprint] %s\n", n > 0 ? "in combat" : "out of combat");
}

// cost: the float's bits; 0 (0.0f) takes nothing
uint32_t rt_sprint_cost(uint32_t cost) {
    return g_settings.free_sprint && __atomic_load_n(&battle, __ATOMIC_RELAXED) <= 0 ? 0 : cost;
}

RT_ON_RESET(free_sprint_reset) { __atomic_store_n(&battle, 0, __ATOMIC_RELAXED); }
