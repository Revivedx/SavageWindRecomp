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
// present_pace: a frame was presented at t (plat_now_ns), on its pace's grid or not.
void framepace_presented(uint64_t t, bool on_grid);

// Called by the game code (tools/recomp/patches.py): the frame loop's vsync wait (1: skip it, 0: run the game's own), and
// its clock's update (x19 of ksys::VFR::updateInterval).
int rt_frame_wait(void);
void rt_vfr_update(uint64_t vfr);

// SAVAGEWIND_SCRIPT_CLOCK=1: the frame number test scripts use, in 30ths of a second (once per presented frame);
// otherwise the presented frames.
uint64_t framepace_script_frame(uint64_t presented);
