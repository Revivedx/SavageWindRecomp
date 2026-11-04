// Where the player is, as text for Discord (runtime/discord_rpc.h): from Link's position and the packs the game reads.
// The names: runtime/game_place_table.inc, made by tools/gen_place_table.py.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The language of the text: a game language's folder (USen, EUde, JPja...; language.c) or en, es. English unless told
// otherwise; one it doesn't know changes nothing.
void game_place_set_language(const char *language);

// A RomFS file the game reads (any thread): each interior has its own pack, the main field its map files
void game_place_on_read(const char *path);

// The two lines for what the player is doing now; false: nothing new to say (no Link while loading: the last text stays)
bool game_place_text(float x, float z, bool has_link, char *details, size_t details_size, char *state, size_t state_size);

// On the main field: the name that holds at x, z and the tower region (NULL: none)
const char *game_place_name(float x, float z);
const char *game_place_region(float x, float z);
