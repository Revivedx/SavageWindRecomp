// Cheats > Collectible hints on the map (runtime/collectibles.c): the Korok seeds and shrines found in each tower region
// and the towers activated, read from the game's own records while the map shows them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COLLECT_NO_TABLE,   // the game files don't have the Korok seeds and shrines as expected (the log says why)
    COLLECT_READING,    // reading the game files, then looking for the records in the game's memory (the first
                        // time: a few seconds)
    COLLECT_READY,      // collectibles_counts has them
    COLLECT_NOT_FOUND,  // the records weren't found, or didn't read as expected: nothing to show (the log says why)
} CollectState;

typedef struct {
    int koroks, koroks_total, shrines_found, shrines_cleared, shrines_total;
    bool tower;  // the region's tower is activated (a region only; all of Hyrule: false)
} CollectCounts;

// Each frame the counts are shown: keeps them read while asked (a second or so behind the game) and says how it went
CollectState collectibles_poll(void);
// The tower regions, numbered in the order of their areas in the game files; one's counts (region -1: all of them)
#define COLLECT_REGIONS 15
bool collectibles_counts(int region, CollectCounts *out);
// The region at a world point (x, z) on the main field (-1: none, or not read yet), and its name in the game's text
// language: its tower's, without "Tower" in English and Spanish (NULL: not read yet, or the files lack it; a name stays
// valid). The generation goes up with each language whose names are read, for the fonts that need their letters.
int collectibles_region_at(float x, float z);
const char *collectibles_region_name(int region);
unsigned collectibles_names_generation(void);

// The Korok seeds not found yet and the shrines not found yet in the regions whose tower is activated, where they are
// (world x, z), as last read: up to max; how many (none until collectibles_poll says COLLECT_READY). seed: the same
// number for the same one each time (its flag's hash)
typedef enum { COLLECT_KOROK, COLLECT_SHRINE } CollectKind;
typedef struct { float x, z; CollectKind kind; uint32_t seed; } CollectPoint;
int collectibles_missing(CollectPoint *out, int max);

// The game's map as drawn now, for Cheats > Collectible hints on the map (settings.h collectibles_map;
// runtime/collectibles_map.c): a world point (x, z) is at layout (x * scale[0] * zoom + translate[0],
// -z * scale[1] * zoom + translate[1]), in the 1280x720 units of the pointer (centred, y up); the map's canvas is
// |x| <= half[0], |y| <= half[1]; the map's cursor is on the world point cursor (x, z); bar: the picture of the leftmost
// prompt on the map's prompt bar (left, bottom, right, top; bar[2] < bar[0]: none shown), its label and button
// right-aligned in it, and bar_text its label (UTF-8). False: the option is off, the full map isn't up, a panel covers
// it or it is in Hero's Path mode (game 1.9.0 only).
typedef struct { float translate[2], zoom, scale[2], half[2], cursor[2], bar[4]; char bar_text[96]; } CollectMapView;
bool collectibles_map_view(CollectMapView *v);

// The ground under the hints (runtime/collectibles_map.c): a small copy of the game image (without the overlay), which
// the presenter takes when the hints ask, so that a hint on light ground (Eldin's rock, snow) can be drawn stronger.
// All on the render thread. collectibles_ground_ask: the hints show this frame on this view (each frame). The
// presenter: collectibles_ground_wanted says whether to take a copy now (a few a second while asked), and
// collectibles_ground_put hands it over when it arrives (RGBA, 8 bits a channel as the game image stores them, w x h
// covering the whole game image; the view is the one asked for when the copy was taken). collectibles_ground: the
// latest copy and its view (false: none, or asked for more than a second ago); gen goes up with each copy.
typedef struct { const uint8_t *rgba; int w, h; unsigned gen; CollectMapView view; } CollectGround;
void collectibles_ground_ask(const CollectMapView *v);
bool collectibles_ground_wanted(void);
void collectibles_ground_put(const uint8_t *rgba, int w, int h);
bool collectibles_ground(CollectGround *out);

// (runtime/main.c boot: each game start)
void collectibles_start(void);

#ifdef __cplusplus
}
#endif
