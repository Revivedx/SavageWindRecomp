// Synthetic CPU/OS feature sets exercise the production target selection without requiring old hardware.
// make test-game-cpu
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__)
static const char *const features[] = {
    "sse3", "ssse3", "sse4.1", "sse4.2", "popcnt", "cx16", "lahf_lm",
    "avx", "avx2", "bmi", "bmi2", "f16c", "fma", "lzcnt", "movbe", "os_ymm"};
#define V2_MASK ((1u << 7) - 1)
#define V3_MASK ((1u << 16) - 1)
static unsigned present, initialised;
static void cpu_init(void) { ++initialised; }
static int cpu_supports(const char *name) {
    assert(initialised);
    // Non-one return values also match GCC's bitmask-valued builtin.
    if (!strcmp(name, "x86-64-v2")) return (present & V2_MASK) == V2_MASK ? 16 : 0;
    if (!strcmp(name, "x86-64-v3")) return (present & V3_MASK) == V3_MASK ? 32 : 0;
    // Support the old partial checks too: removing F16C, MOVBE, etc. must catch their false positives.
    for (unsigned i = 0; i < sizeof features / sizeof *features; ++i)
        if (!strcmp(name, features[i])) return present & (1u << i);
    assert(!"unexpected CPU feature queried");
    return 0;
}
#define __builtin_cpu_init cpu_init
#define __builtin_cpu_supports cpu_supports
#endif
#include "game_cpu.h"

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)
int main(void) {
#if defined(__x86_64__)
    present = V3_MASK;
    CHECK(game_cpu_runs("-march=x86-64-v3"));
    CHECK(game_cpu_runs("-march=x86-64-v2"));
    CHECK(initialised == 2);
    for (unsigned i = 0; i < sizeof features / sizeof *features; ++i) {
        present = V3_MASK & ~(1u << i);
        CHECK(!game_cpu_runs("-march=x86-64-v3"));
        CHECK(game_cpu_runs("-march=x86-64-v2") == (i >= 7));
    }
    present = V2_MASK;
    CHECK(game_cpu_runs("x86-64-v2"));
    CHECK(!game_cpu_runs("x86-64-v3"));
    present = 0;
    CHECK(!game_cpu_runs("x86-64-v2"));
    CHECK(!game_cpu_runs("x86-64-v3"));
#else
    CHECK(game_cpu_runs("x86-64-v2"));
    CHECK(game_cpu_runs("x86-64-v3"));
#endif
    CHECK(game_cpu_runs(""));
    CHECK(game_cpu_runs("-march=native"));
    CHECK(game_cpu_runs("-march=haswell"));
    puts("game CPU target checks passed");
    printf("%u checks\n", checks);
    return 0;
}
