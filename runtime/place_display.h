#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Cheats > Map (runtime/place_display.c): shows every shrine, tower, stable and lab on the map, and makes the places it
// shows travel destinations, by hooking three decisions of the game's own marker logic. No game data is written:
// nothing is saved and no tower is activated, so the map fog stays as it is.
uint32_t rt_place_count(uint64_t record, uint32_t count);  // the visit count a marker is drawn from
uint32_t rt_place_travel(uint64_t marker, uint32_t bit);   // the travel bit (0x200) of a marker's flags
uint32_t rt_place_icon(uint64_t marker, uint32_t flags);   // the draw's view of those flags (the icon's colour)
#ifdef __cplusplus
}
#endif
