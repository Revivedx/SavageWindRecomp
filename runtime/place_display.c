// Cheats > Map (runtime/place_display.h): shows every shrine, tower, stable and lab on the map, and makes the places it
// shows travel destinations. No game data is written: nothing is saved and no tower is activated, so the map fog stays
// as it is.
//
// Three decisions of the game's own marker logic are hooked:
//  - the visit count a marker is drawn from (record +0x1c): a place that was never visited reads as 1
//    (patches.py _place_count_read), so the game marks and draws it itself. Only that read changes - the record in
//    memory keeps its 0, so what the game records elsewhere (completion and the like) is untouched;
//  - the travel bit of a marker's flags (marker +0x3c, bit 0x200): kept on for the places above (patches.py
//    _place_travel), so they can be travelled to without being activated. Stables are left out: the game itself
//    never travels to one;
//  - the draw's colour decision (patches.py _place_icon): it tests that same travel bit, which is what makes a tower
//    look activated. For a place whose record still has count 0 - travelable only because of this option - the bit is
//    hidden from that read, so the marker keeps the look it has for a place that was never visited.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "place_display.h"
#include "rt.h"  // GUEST_LO/GUEST_HI (guest addresses are the process's own addresses)
#include "settings.h"

#define RECORD_NAME_OFF 0x10
#define RECORD_COUNT_OFF 0x1c
#define MARKER_RECORD_OFF 0x58
#define MARKER_TRAVEL_BIT 0x200u

// The groups the option covers, by the place record's name (the record's name is a plain char * at +0x10).
static bool is_named(const char *name, const char *prefix, size_t len) { return strncmp(name, prefix, len) == 0; }
static bool is_shrine(const char *name) { return is_named(name, "Location_Dungeon", 16); }
static bool is_tower(const char *name) { return is_named(name, "Location_MapTower", 17); }
static bool is_stable(const char *name) { return is_named(name, "Location_", 9) && strstr(name, "Hatago") != NULL; }
static bool is_lab(const char *name) { return is_named(name, "Location_", 9) && strstr(name, "Labo") != NULL; }
// Shown: shrines, towers, stables and the ancient labs. Travelled to: the same except the stables.
static bool shown(const char *name) {
    return name && (is_shrine(name) || is_tower(name) || is_stable(name) || is_lab(name));
}
static bool travels(const char *name) { return shown(name) && !is_stable(name); }

// The place record a marker belongs to, and its name; 0/NULL when this is not one of the map's markers.
static uint64_t marker_record(uint64_t marker) {
    if (marker < GUEST_LO || marker >= GUEST_HI || (marker & 3)) return 0;
    uint64_t record = *(volatile uint64_t *)(uintptr_t)(marker + MARKER_RECORD_OFF);
    return record >= GUEST_LO && record < GUEST_HI ? record : 0;
}

static const char *record_name(uint64_t record) {
    if (record < GUEST_LO || record >= GUEST_HI) return NULL;
    const char *name = *(const char *const volatile *)(uintptr_t)(record + RECORD_NAME_OFF);
    return (uint64_t)(uintptr_t)name >= GUEST_LO && (uint64_t)(uintptr_t)name < GUEST_HI ? name : NULL;
}

// The visit count the marker update reads for a place: never-visited places of the groups above read as visited.
uint32_t rt_place_count(uint64_t record, uint32_t count) {
    if (!g_settings.show_all_places || count != 0 || record < GUEST_LO || record >= GUEST_HI) return count;
    const char *name = record_name(record);
    return shown(name) ? 1 : count;
}

// The travel bit of a marker's flags, as the marker update computes it: kept on while the option is on.
uint32_t rt_place_travel(uint64_t marker, uint32_t bit) {
    if (!g_settings.show_all_places) return bit;
    const char *name = record_name(marker_record(marker));
    return travels(name) ? MARKER_TRAVEL_BIT : bit;
}

// The draw's view of a marker's flags: the travel bit is hidden for places that are only travelable because of this
// option (their record still has count 0), so they keep the normal icon instead of the activated-looking one.
uint32_t rt_place_icon(uint64_t marker, uint32_t flags) {
    if (!g_settings.show_all_places || !(flags & MARKER_TRAVEL_BIT)) return flags;
    uint64_t record = marker_record(marker);
    const char *name = record_name(record);
    if (!travels(name)) return flags;
    return *(volatile uint32_t *)(uintptr_t)(record + RECORD_COUNT_OFF) == 0 ? (flags & ~MARKER_TRAVEL_BIT) : flags;
}
