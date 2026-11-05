// (profiling) Cumulative event counters for the benchmark (tools/perf_routes.py, docs/PERF-ROUTES.md): the `frametrace`
// control command takes their difference at every presented frame, `memstats` reads the texture memory ones. Each is
// one relaxed atomic add at an event that costs far more itself (a pipeline build, an upload, an ASTC decode, a file
// read), always on: no clock is read for them unless the event already timed itself. Defined in runtime/control_prof.c.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum {
    PC_BUILDS,         // shader translations, programs, graphics and compute pipelines built (any thread)
    PC_BUILD_NS,       // ... their build time (worker threads included: a worker's build does not stall a frame)
    PC_BUILD_WAITS,    // draws that waited for their pipeline on the render thread (exec.c wait_built)
    PC_BUILD_WAIT_NS,  // ... time waited
    PC_UPLOAD_BYTES,   // staging memory handed out for uploads (textures, buffers, the heap copy: exec.c vk_transient)
    PC_TEX_BYTES,      // guest texture storage read for a full texture upload (textures.c upload_texture)
    PC_ASTC_NS,        // CPU time decoding ASTC to RGBA8 (GPUs without ASTC: textures.c)
    PC_ASTC_BYTES,     // ... RGBA8 bytes made
    PC_GPU_WAIT_NS,    // CPU time waiting for GPU submissions (exec.c vk_wait_seq)
    PC_FILE_READS,     // guest file and RomFS reads (services/fs.c)
    PC_FILE_BYTES,
    PC_FILE_NS,
    PC_VALLOC_NS,      // vkAllocateMemory on the game's threads (vulkan/vram.c)
    PC_PRESENT_NS,     // the back end's present (acquire, blit, present: vulkan/device.c -> exec.c vk_prof_frame)
    PC_COUNT
};
extern _Atomic uint64_t g_perfcount[PC_COUNT];
static inline void perfcount_add(int k, uint64_t v) { atomic_fetch_add_explicit(&g_perfcount[k], v, memory_order_relaxed); }

// Texture memory by kind, live bytes (textures.c: added when an image is made, taken off when it is destroyed)
enum { TM_ASTC_NATIVE, TM_ASTC_RGBA8, TM_BC, TM_RT, TM_OTHER, TM_ASTC_SRC, TM_COUNT };  // (TM_ASTC_SRC: the ASTC size of TM_ASTC_RGBA8's images)
extern _Atomic int64_t g_texmem[TM_COUNT], g_texmem_n[TM_COUNT];
static inline void texmem_add(int k, int64_t bytes, int n) {
    atomic_fetch_add_explicit(&g_texmem[k], bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_texmem_n[k], n, memory_order_relaxed);
}

// GPU memory heaps, filled by the Vulkan back end (vulkan/vram.c) if there is one: returns the heap count
extern int (*g_gpu_heaps)(uint64_t *usage, uint64_t *budget, uint64_t *size, uint32_t *device_local, int max);
