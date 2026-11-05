#pragma once
#include "cpu.h"
#include <stdbool.h>

typedef enum { QUICK_DROP_REJECTED, QUICK_DROP_PENDING, QUICK_DROP_COMPLETE } QuickDropResult;
bool quick_drop_action_enabled(void);
bool quick_drop_action_init(void);
void quick_drop_action_request(Cpu *, uint64_t screen, uint64_t mgr, uint64_t item, int tab, int slot, const char *name);
void quick_drop_action_close(Cpu *);
// Pump only from the existing native e68168 UI-manager guest callback (x0).
void quick_drop_action_tick(Cpu *);
QuickDropResult quick_drop_action_result(void);
bool quick_drop_action_busy(void);
// Final HID gate, after every injection/retry. Retains analog direction bits;
// consumes other held buttons until release, even if the setting is disabled.
uint64_t quick_drop_action_filter_buttons(uint64_t buttons);

uint64_t rt_quick_drop_actor_begin(uint64_t actor, uint64_t method);
void rt_quick_drop_actor_end(uint64_t token, uint64_t actor, uint64_t result);
