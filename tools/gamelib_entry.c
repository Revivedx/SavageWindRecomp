// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the module layout,
// function tables and the game version's sites (the generated layout.c), found by name when the app loads the library.
// Everything else in it stays hidden.
#include "gamelib.h"

extern const GameLibModule game_modules[];
extern const uint32_t game_module_count;
extern const char game_version[];
extern const GameLibSite game_sites[];
extern const uint32_t game_site_count;

// simd.h: the compatible target's fma mode, set by the runtime through the tables (exact until then)
#ifndef _WIN32
__attribute__((visibility("hidden")))
#endif
int simd_fma_unfused;

GAMELIB_EXPORT const GameLibTables *swgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, game_modules, &game_module_count,
                                    game_version, game_sites, &game_site_count, &simd_fma_unfused};
    return &t;
}
