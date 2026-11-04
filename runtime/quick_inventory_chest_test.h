// Developer fixture; every function except reset/enable runs on the guest tick.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"

// Call only after validation of main build cd57b23fa4bbad65.
void quick_inventory_chest_test_enable(void);
// Only Weapon_Sword_001, Weapon_Bow_001 and Weapon_Shield_001 are supported.
// True means queued native creation; no award or save flag is changed here.
bool quick_inventory_chest_test_begin(Cpu *cpu, const char *reward, const char **reason);
// 0 pending, 1 created, -1 failed; actor is a snapshot backed by our native ref.
int quick_inventory_chest_test_poll(Cpu *cpu, uint64_t *actor, const char **reason);
// Guest threads have stopped and memory will be wiped; never calls native code.
void quick_inventory_chest_test_reset(void);
