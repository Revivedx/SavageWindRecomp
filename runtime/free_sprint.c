// Cheats > Free sprint (settings.h free_sprint): sprinting on the ground takes no stamina while no enemy is fighting
// Link. Patches in the game code ask here (tools/patches.py _sprint_cost, _battle_info): Link's Move action for what a
// second of sprinting and a sprint press cost, and the three battle music controllers with the count their music
// starts on (1 or more) and fades out on (0). "In combat" is any of them at 1 or more.
#include <stdbool.h>
#include <stdint.h>

#include "hle.h"
#include "settings.h"

#define BATTLE_SETTERS 3
static int32_t battle[BATTLE_SETTERS];  // the count each controller last got (set on the sound thread)

static bool in_combat(void) {
    for (int i = 0; i < BATTLE_SETTERS; i++)
        if (__atomic_load_n(&battle[i], __ATOMIC_RELAXED) > 0) return true;
    return false;
}

void rt_battle_info(int which, int32_t n) {
    if (which >= 0 && which < BATTLE_SETTERS) __atomic_store_n(&battle[which], n, __ATOMIC_RELAXED);
}

// cost: the float's bits; 0 (0.0f) takes nothing
uint32_t rt_sprint_cost(uint32_t cost) { return g_settings.free_sprint && !in_combat() ? 0 : cost; }

RT_ON_RESET(free_sprint_reset) {
    for (int i = 0; i < BATTLE_SETTERS; i++) __atomic_store_n(&battle[i], 0, __ATOMIC_RELAXED);
}
