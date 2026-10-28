#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "cpu.h"
bool pc_cursor_enabled(void);
bool pc_cursor_active(void);
bool pc_cursor_event(const SDL_Event *e);
void pc_cursor_window(SDL_Window *window);
void pc_cursor_frame(void);
void rt_pc_button_group(Cpu *c);
void rt_pc_cursor_manager(Cpu *c, uint64_t activate, uint64_t bounds);
uint64_t pc_cursor_buttons(uint64_t buttons);
bool pc_cursor_hover_box(float box[4]);
bool pc_cursor_map_navigation(void);
bool pc_cursor_hide_system(void);
int rt_pc_map_focus(Cpu *c, int position);
