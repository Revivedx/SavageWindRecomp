// Synthetic unit tests for runtime/assists.c (including stamina_drain multiplier, #968).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

Settings g_settings = {.weapon_wear = 100, .damage_taken = 100, .damage_dealt = 100, .stamina_drain = 100};

uint64_t plat_now_ns(void) { return 0; }  // (assists.c times Link's last damage for the Dynamic HUD)

uint32_t rt_stamina(uint32_t oldv, uint32_t newv);
int32_t rt_link_life(int32_t oldv, int32_t newv);
uint32_t rt_life_cost(uint32_t cost, int32_t life, int whose);

static uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static float u2f(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int checks, fails;
static void check(bool ok, const char *name) {
    checks++;
    if (!ok) {
        fails++;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

int main(void) {
    // 100% passthrough
    g_settings.stamina_drain = 100;
    g_settings.infinite_stamina = false;
    check(rt_stamina(f2u(100.0f), f2u(90.0f)) == f2u(90.0f), "100% drain exact passthrough");

    // 50% drain: 100 -> 90 becomes 100 -> 95
    g_settings.stamina_drain = 50;
    check(fabsf(u2f(rt_stamina(f2u(100.0f), f2u(90.0f))) - 95.0f) < 1e-5f, "50% drain halves loss");

    // 25% drain: 100 -> 80 becomes 100 -> 95
    g_settings.stamina_drain = 25;
    check(fabsf(u2f(rt_stamina(f2u(100.0f), f2u(80.0f))) - 95.0f) < 1e-5f, "25% drain quarters loss");

    // 200% drain: 100 -> 90 becomes 100 -> 80, and clamps at 0.0f
    g_settings.stamina_drain = 200;
    check(fabsf(u2f(rt_stamina(f2u(100.0f), f2u(90.0f))) - 80.0f) < 1e-5f, "200% drain doubles loss");
    check(fabsf(u2f(rt_stamina(f2u(5.0f), f2u(1.0f))) - 0.0f) < 1e-5f, "200% drain clamps at 0");

    // Recovery (new > old) is never scaled
    check(rt_stamina(f2u(50.0f), f2u(80.0f)) == f2u(80.0f), "recovery passes through unchanged");

    // Infinite stamina overrides drain multiplier
    g_settings.infinite_stamina = true;
    check(rt_stamina(f2u(100.0f), f2u(90.0f)) == f2u(100.0f), "infinite_stamina blocks drain");

    // Damage multipliers sanity
    g_settings.damage_taken = 50;
    check(rt_link_life(20, 12) == 16, "50% damage_taken");
    g_settings.damage_dealt = 200;
    check(rt_life_cost(10, 100, 0) == 20, "200% damage_dealt");

    printf("test_assists: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
