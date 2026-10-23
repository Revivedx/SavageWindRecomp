// The recompiled game code: a library built from the player's own game files (`make gen` + `make` in a source
// checkout, the start screen's setup otherwise; runtime/gamebuild.c) and loaded at startup (runtime/gamelib.c).
// Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "swgame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "swgame.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "swgame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library built with another one is rebuilt, not loaded.
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 3

// One exefs module as the game code was generated for it: where it goes (tools/layout.py, the generated layout.c)
// and its functions. runtime/loader.c loads the module there.
typedef struct {
    const char *name;        // its exefs file: main, subsdk0..9, sdk
    uint64_t base, size;     // guest address, image size (page-rounded)
    const char *build_id;    // NSO build id (64 hex digits) of the file the code was generated from
    const uint64_t *addrs;   // sorted guest addresses
    const GuestFn *fns;      // the functions at them
    const uint64_t *count;
} GameLibModule;

// One game address the runtime uses (tools/game_symbols.py, the generated layout.c): they belong to the build the
// code was generated from, so the runtime asks for them by name (rt_game_sym) instead of knowing a version.
typedef struct {
    const char *name;
    uint64_t value;
} GameLibSymbol;

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    const GameLibModule *modules;  // in load order, main first
    const uint32_t *nmodules;
    const GameLibSymbol *symbols;  // the game's addresses for this build
    const uint32_t *nsymbols;
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "swgame_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// SAVAGEWIND_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// A game address of the loaded library's build by name (tools/game_symbols.py), 0 when it has none or no library is
// loaded. The game code carries the values of the build it was generated from: nothing here knows a version.
uint64_t rt_game_sym(const char *name);
// True when a library usable with this build is in one of those places, built from the game files of main_build_id
// (the exefs/main NSO's build id: a library from another version's files is not it).
bool gamelib_present(const char *game_dir, const char *main_build_id);

#ifdef __cplusplus
}
#endif
