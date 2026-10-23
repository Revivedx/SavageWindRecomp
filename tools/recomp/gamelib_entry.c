// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the module layout
// and function tables (the generated layout.c), found by name when the app loads the library. Everything else in it
// stays hidden.
#include "gamelib.h"

extern const GameLibModule game_modules[];
extern const uint32_t game_module_count;

GAMELIB_EXPORT const GameLibTables *swgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, game_modules, &game_module_count};
    return &t;
}
