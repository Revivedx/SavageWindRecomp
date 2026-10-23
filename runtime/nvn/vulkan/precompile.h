// Shaders of the game files compiled before the game draws with them (precompile.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../maxwell/shader.h"
#include "../shader_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int shaders, compiled, failed_translations, failed_compiles, failed_driver, undeclared_inputs, threads;
    uint64_t scan_ms, compile_ms;  // compile: every step, wall clock
    uint64_t translate_cpu_ms, spirv_cpu_ms, driver_cpu_ms;  // each step's time on all threads
} PrecompileStats;

// Scans the game files in root (romfs/, exefs/main) and fills the shader caches; driver: also the driver's pipeline
// cache, on the game's device (the start screen's) or, without one, a device of its own (vk_offscreen_init). progress (may be NULL): shaders done of all, from any thread.
bool precompile_shaders(const char *root, bool driver, void (*progress)(int done, int total), PrecompileStats *st, char *why,
                        size_t why_size);

// The memory the driver's compiles may take at once, in MB (0: a quarter of the RAM; SAVAGEWIND_PRECOMPILE_MEM_MB).
// Any thread, any time: a running precompile follows it (the start screen gives it less while the game code builds).
void precompile_memory_budget(int64_t mb);
// stop: a running precompile (or the next one, until called again with false) ends after the shaders it is
// compiling; those done stay in the caches.
void precompile_stop(bool stop);

// (exec.c, device.c) The driver's part, for a precompile on a device of its own.
void vk_offscreen_init(void);
void vk_pipeline_cache_save(void);
bool vk_prebuild_stage(const uint32_t *spv, size_t nwords, int stage);
bool vk_prebuild_compute(const uint32_t *spv, size_t nwords, const uint32_t local[3]);

#ifdef __cplusplus
}
#endif
