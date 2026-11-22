// Cheats > Collectible hints on the map: the Korok seeds, shrines and towers to read, built from the player's own game
// files the first time they are needed (runtime/collectibles_table.c; runtime/collectibles.c reads their flags).
// Nothing of it is in the build: tools/gen_collectibles.py builds the same table, to compare (make test-collectibles).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "collectibles.h"

#define COLLECT_KOROKS 900
#define COLLECT_SHRINES 120

// A flag is known by its hash: the crc32 of its name, which the game keeps with it
typedef struct { uint8_t region; uint32_t found; float x, z; } CollectKorok;            // bool, set when found
typedef struct { uint8_t region; uint32_t found, cleared; float x, z; } CollectShrine;  // s32 (not 0 once found), bool
typedef struct {
    CollectKorok korok[COLLECT_KOROKS];     // by region, then x, z
    CollectShrine shrine[COLLECT_SHRINES];  // (the same)
    uint32_t tower[COLLECT_REGIONS];        // the bool its tower's activation sets, by region
    uint32_t koroks_found;                  // HiddenKorok_Number (s32): the game's own count of the Koroks found
    char tower_msg[COLLECT_REGIONS][16];    // the tower's marker (TowerNN): its name's label in LocationMarker.msbt
    // The tower areas (Ecosystem/MapTower.sbeco): a row of runs (u16 area, u16 length) per row_h units of z, offsets to
    // them in u16s; area[r]: region r's (the regions in the order of their areas)
    uint8_t *eco;
    size_t eco_n;
    uint32_t rows, row_h;
    uint16_t area[COLLECT_REGIONS];
} CollectTable;

// From Pack/Bootup.pack (the flags in GameData/gamedata.ssarc, the shrine and tower markers in
// Map/MainField/Static.smubin, the tower areas in Ecosystem/MapTower.sbeco) and the main field's map units
// (Map/MainField/<unit>/<unit>_Static.smubin and _Dynamic.smubin: the hidden Koroks). False when a file is missing or
// doesn't read as expected (900 Koroks, 120 shrines, 15 towers, each flag of its kind and hashed by its name): the log
// says why.
bool collect_table_build(CollectTable *t);
// The region at a world point (x, z) on the main field, -1: none
int collect_table_region(const CollectTable *t, float x, float z);
// The regions' names in a text language (its folder: USen, EUde, ...; Pack/Bootup_<it>.pack): the name of each
// region's tower, without "Tower" in English and Spanish ("Lake Tower" -> "Lake"). False if the files lack them.
bool collect_table_names(const CollectTable *t, const char *folder, char names[COLLECT_REGIONS][64]);
