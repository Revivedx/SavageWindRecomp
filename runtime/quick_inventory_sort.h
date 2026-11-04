#pragma once
#include "rt.h"

// Initialization follows quick_drop's exact NSO build check on the guest setup thread.
bool quick_inventory_sort_init(void);
void quick_inventory_sort_begin(Cpu *c);
void quick_inventory_sort_menu(Cpu *c);
// Called under the native inventory lock; publishes a render-thread heartbeat.
void quick_inventory_sort_publish(uint64_t screen);
bool quick_inventory_sort_hint(void);
