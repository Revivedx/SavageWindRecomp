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
#define GAMELIB_ABI 4

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

// An address in the main module the runtime uses, by name (tools/game_sites.py finds them for the game version the
// code was generated from; runtime/game_sites.h).
typedef struct {
    const char *name;
    uint64_t addr;
} GameLibSite;

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    const GameLibModule *modules;  // in load order, main first
    const uint32_t *nmodules;
    const char *version;  // the game version ("1.8.2")
    const GameLibSite *sites;
    const uint32_t *nsites;
    int *fma_unfused;  // the library's simd_fma_unfused (simd.h; only read by the compatible target's code)
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
// Once a game frame: passes settings.h fast_fma on to the compatible target's game code (logs each change)
void gamelib_sync(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);

// The game code's target on this CPU (#9). x86-64 builds the game code for x86-64-v3 (AVX2, FMA, BMI2: the Makefile's
// GENARCH) where the CPU has those, else for the compatible target (GENARCH_COMPAT: x86-64-v2, no FMA instructions -
// runtime/simd_softfma.h instead, slower). Each has its own id, so a library is never loaded on a CPU that can't run
// it, and a game folder moved to a PC of the other kind gets its game code built again. SAVAGEWIND_COMPAT_CPU=1 picks
// the compatible target on any CPU (to compare the two). Other hosts have one target.
bool gamelib_compat(void);
const char *gamelib_id(void);  // the id a library for this CPU has (GAMELIB_ID or GAMELIB_ID_COMPAT)
// NULL when the CPU runs the game code's target, else what it lacks ("a CPU with ...") - checked at startup
const char *gamelib_cpu_problem(void);
// True when game_dir holds a library built for the other target (a game folder moved from a PC of the other kind)
bool gamelib_other_cpu(const char *game_dir);

#ifdef __cplusplus
}
#endif
