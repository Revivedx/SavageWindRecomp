// The focus/gyro tests exercise input.c with pointer menus disabled. Keep that
// boundary explicit rather than linking the guest UI adapters into host tests.
#pragma once
#include "pc_cursor.h"
bool pc_cursor_active(void) { return false; }
bool pc_cursor_event(const SDL_Event *e) { (void)e;return false; }
void pc_cursor_frame(void) {}
void pc_cursor_window(SDL_Window *w) { (void)w; }
bool pc_cursor_hide_system(void) { return false; }
bool pc_cursor_map_navigation(void) { return false; }
uint64_t pc_cursor_buttons(uint64_t buttons) { return buttons; }
