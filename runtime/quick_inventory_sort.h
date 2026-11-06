#pragma once
#include "cpu.h"
#include <stdbool.h>

bool quick_inventory_sort_enabled(void);
bool quick_inventory_sort_init(void);
void rt_quick_inventory_sort_begin(Cpu *c);
void rt_quick_inventory_sort_menu(Cpu *c);
void quick_inventory_sort_publish(uint64_t screen);
bool quick_inventory_sort_hint(void);
