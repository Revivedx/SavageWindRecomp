// Use the original Options row's setter to update AmiiboItemOnOff through the game's own flag manager.
#include <string.h>
#include "amiibo.h"
#include "gamelib.h"
#include "rt.h"

bool amiibo_set_game_enabled(bool enabled) {
    const GameLibTables *tables = gamelib_tables();
    Cpu *cpu = kernel_current_cpu();
    if (!tables || !cpu) return false;
    GuestFn store = NULL;
    for (uint32_t i = 0; i < *tables->nsites; i++) {
        if (strcmp(tables->sites[i].name, "opt_store")) continue;
        store = dispatch_lookup(tables->sites[i].addr);
        break;
    }
    if (!store) return false;
    // Index 9 is AmiiboItemOnOff. In 1.9.0 it is KIND_BUTTON (constructor main+0xe2e7e0),
    // so opt_store reads +0x64, not the toggle's +0x54 (main+0xe2eeb8). Populate all value
    // representations so the native setter can choose the one appropriate to its entry kind.
    uint64_t row[0x98 / 8] = {0};
    uint32_t index = 9;
    memcpy((uint8_t *)row + 0x50, &index, sizeof index);
    *((uint8_t *)row + 0x54) = enabled;
    uint32_t value = enabled;
    memcpy((uint8_t *)row + 0x58, &value, sizeof value);
    *((uint8_t *)row + 0x64) = enabled;
    Cpu saved = *cpu;
    cpu->x[0] = (uintptr_t)row;
    store(cpu);
    bool ok = (cpu->x[0] & 1) != 0;
    *cpu = saved;
    return ok;
}
