// Frame rate and the game's clock (runtime/hle/framepace.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The frame rate the runtime paces to now, frames per second (0: no limit): the setting (Video > Frame rate), 30
// in the menus and while a movie plays.
int framepace_target(void);
// The time between presents for it, nanoseconds (no limit: 1/300 s).
uint64_t framepace_period_ns(void);
// false: the game paces itself and keeps its own clock (SAVAGEWIND_FRAMEPACE=0)
bool framepace_on(void);
// An event is running: a cutscene, a talk, an item fanfare (the game's event manager has an active event)
bool framepace_in_event(void);
// The start screen or one of its menus is up (a 16:9 layout over pictures of its own)
bool framepace_title_ui(void);
// The loading screen is up (a 16:9 layout: its tip, the logo)
bool framepace_loading_ui(void);
// The pause menu's pages or the map are up (16:9 layouts that cover the whole picture)
bool framepace_menu_ui(void);
// The game is paused (the pause menu, the map) or the start screen's menu is up
bool framepace_paused(void);
// present_pace: a frame was presented at t (plat_now_ns), on its pace's grid or not.
void framepace_presented(uint64_t t, bool on_grid);

// Called by the game code (tools/patches.py): the frame loop's vsync wait (1: skip it, 0: run the game's own), and
// its clock's update (x19 of ksys::VFR::updateInterval).
int rt_frame_wait(void);
void rt_vfr_update(uint64_t vfr);
// The game's frames so far (rt_vfr_update calls, game thread): what the matrices of one frame share (the jitter)
uint64_t framepace_game_frame(void);
uint64_t framepace_shown_frame(void);  // (the game's frames that were presented: one per present, paused too)

// SAVAGEWIND_SCRIPT_CLOCK=1: the frame number test scripts use, in 30ths of a second (once per presented frame);
// otherwise the presented frames.
uint64_t framepace_script_frame(uint64_t presented);
