#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Cheats > Blood moon now and the console's bloodmoon (runtime/blood_moon.c): the game's own blood moon, its cutscene
// and everything it brings back, asked for through the game's memory-relief request. False when no world is loaded.
// The request waits while a menu, a loading screen or a cutscene is up, and goes to the game once play resumes.
bool blood_moon_request(void);
bool blood_moon_pending(void);  // asked for, not handed to the game yet
void blood_moon_frame(void);    // once a presented frame: hands a pending request to the game when play allows it
#ifdef __cplusplus
}
#endif
