// The compiler's complete x86-64 levels, not a subset of the instructions a game library may use.
#pragma once
#include <stdbool.h>
#include <string.h>

// Other GENARCH values and non-x86 targets keep their existing unchecked behaviour.
static inline bool game_cpu_runs(const char *arch) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (strstr(arch, "x86-64-v3")) return __builtin_cpu_supports("x86-64-v3");
    if (strstr(arch, "x86-64-v2")) return __builtin_cpu_supports("x86-64-v2");
#endif
    (void)arch;
    return true;
}
