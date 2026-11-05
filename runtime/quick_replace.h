#pragma once
#include "cpu.h"
#include <stdbool.h>

bool quick_replace_init(void);
bool quick_replace_enabled(void);
bool quick_replace_active(void);
bool quick_replace_bottom_layout(void);
bool quick_replace_menu(Cpu *);
bool quick_replace_card_drop_allowed(void);
bool quick_replace_card_sort_allowed(uint64_t screen);
uint64_t quick_replace_buttons(uint64_t buttons);

// Checked native hooks. Original callbacks are supplied for this invocation;
// no pointer into the generated game library survives a reboot.
void rt_quick_replace_popup(Cpu *);
void rt_quick_replace_item_popup(Cpu *);
void rt_quick_replace_query(Cpu *, uint64_t query);
void rt_quick_replace_full_info(Cpu *, GuestFn native);
bool rt_quick_replace_receive(Cpu *);
bool rt_quick_replace_card_close(Cpu *);
bool rt_quick_replace_card_action(Cpu *);
unsigned rt_quick_replace_chest_begin(Cpu *);
void rt_quick_replace_chest_end(Cpu *, uint64_t ai, unsigned mode);
void rt_quick_replace_fill(Cpu *, GuestFn native);
void rt_quick_replace_tick(Cpu *, GuestFn native);
void rt_quick_replace_ui_draw(Cpu *, GuestFn native);
int rt_quick_replace_toast_begin(Cpu *);
void rt_quick_replace_toast_end(uint64_t toast, int saved);
bool rt_quick_replace_pane(Cpu *);
bool rt_quick_replace_anim(Cpu *);
