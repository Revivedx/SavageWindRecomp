// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the module layout,
// function tables and the game's addresses for this build (the generated layout.c), found by name when the app loads
// the library. Everything else in it stays hidden.
#include "gamelib.h"

extern const GameLibModule game_modules[];
extern const uint32_t game_module_count;
extern const GameLibSymbol game_symbols[];
extern const uint32_t game_symbol_count;

GAMELIB_EXPORT const GameLibTables *swgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI,   GAMELIB_ID,       game_modules,
                                   &game_module_count, game_symbols, &game_symbol_count};
    return &t;
}
