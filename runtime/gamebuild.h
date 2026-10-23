// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/recomp/gen.py; shipped frozen as swgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/swgame.*. The C and the objects are deleted afterwards.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);

#ifdef __cplusplus
}
#endif
