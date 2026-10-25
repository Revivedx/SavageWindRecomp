// Shader precompilation (Vulkan: runtime/nvn/vulkan/precompile.c): every shader of the game's own RomFS translated and
// compiled ahead of time into the shader caches (cache/xlate, cache/spirv), and pipelines for them built into the
// pipeline cache, so that nothing compiles the first time it is drawn, also in places never visited. Optional: a
// choice at game setup and an action in Savage Wind Configuration (runtime/ui/launcher.mm), with a progress bar.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SP_IDLE, SP_READING, SP_COMPILING, SP_PIPELINES, SP_DONE, SP_FAILED, SP_CANCELLED };

typedef struct {
    _Atomic int state;        // SP_*
    _Atomic int done, total;  // the current step's progress
    _Atomic bool cancel;      // set to stop (the caches keep what is done)
    // the result
    int shaders, keys, made, cached, failed, pipelines;
    double seconds;
    char why[256];            // SP_FAILED: what went wrong
} ShaderPrecompile;

// Runs it on the calling thread (with worker threads of its own); romfs: extracted/romfs. Pipelines only
// when the Vulkan device is up (the start screen's window).
bool shader_precompile_run(ShaderPrecompile *p, const char *romfs);
float shader_precompile_progress(const ShaderPrecompile *p);  // 0..1 over the whole run
// The last run's record (cache/precompiled.txt): 0 never run, 1 done for these shaders, 2 done for an older
// translator (run again after an update). shaders, date (may be NULL): how many, when (YYYY-MM-DD).
int shader_precompile_status(int *shaders, char *date, int n);
bool shader_precompile_available(void);  // (false on macOS: Metal compiles its shaders itself)

#ifdef __cplusplus
}
#endif
