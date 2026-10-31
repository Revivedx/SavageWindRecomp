// Performance numbers for the overlay (runtime/perfmon.c): the game's frame rate and frame times, and the machine's
// CPU, memory and GPU load and temperatures. A value the platform doesn't give is negative.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PERF_HISTORY 120  // frame times kept for the graph

typedef struct {
    // the game's frames (presents), over the last second
    float fps, frame_ms_avg, frame_ms_max;
    float fps_generated;           // frames made by frame generation (Video > Frame generation), presented in between
    float frame_ms[PERF_HISTORY];  // oldest first
    // CPU: the whole machine and this process, percent of all logical cores
    char cpu_name[64];
    float cpu_total_pct, cpu_game_pct, cpu_temp_c;
    const char *cpu_temp_source;  // where the temperature comes from ("k10temp", "ACPI thermal zone", ...)
    // memory, MB
    float ram_used_mb, ram_total_mb, ram_game_mb;
    // GPU (NVIDIA through NVML, AMD through sysfs on Linux)
    char gpu_name[96];
    float gpu_pct, gpu_temp_c, gpu_power_w, gpu_clock_mhz, vram_used_mb, vram_total_mb;
} PerfStats;

void perfmon_frame(void);                  // a game frame was presented (any thread)
void perfmon_generated_frame(void);        // a generated frame was presented (vulkan/device.c)
void perfmon_snapshot(PerfStats *out);     // the latest numbers (machine numbers refresh twice a second)
bool perfmon_position(float out[3]);       // render thread: Link's position copy now; false when unavailable
void perfmon_set_gpu_hint(const char *n);  // the Vulkan device's name: which GPU to report

#ifdef __cplusplus
}
#endif
