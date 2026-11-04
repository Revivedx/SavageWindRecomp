// Developer fixture: control commands queue work; only a guest tick calls native code.
#pragma once
#include "control.h"
#include "cpu.h"

void quick_inventory_test_command(int argc, char **argv, ControlReply *reply);
void quick_inventory_test_tick(Cpu *cpu);
// Call only after the exact main NSO build has been validated.
void quick_inventory_test_enable(void);
