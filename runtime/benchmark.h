// In-game fixed-view performance benchmark (runtime/benchmark.c, runtime/ui/benchmark_ui.mm).
// Measures the canonical scenes from tools/perf_routes.py (or the player's current spot) directly from the F1 menu:
// teleport -> hold sky/fly -> aim camera -> warm up -> sample per-frame deltas -> compute percentiles/lows/hitches -> restore.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_ROUTE_COUNT 7
#define BENCH_MAX_RESULTS 8
#define BENCH_SPARK_LEN 120
#define BENCH_MAX_FRAMES 32768

enum BenchMode {
    BENCH_MODE_FULL = 0,    // all 7 canonical routes
    BENCH_MODE_QUICK = 1,   // Plateau, Kakariko, Korok Forest
    BENCH_MODE_SINGLE = 2,  // one chosen canonical route
    BENCH_MODE_SPOT = 3,    // Link's current position and view (no teleport)
};

typedef enum {
    BENCH_IDLE = 0,
    BENCH_TELEPORTING,
    BENCH_AIMING,
    BENCH_SETTLING,
    BENCH_MEASURING,
    BENCH_RESTORING,
    BENCH_DONE,
    BENCH_CANCELLED,
} BenchPhase;

typedef struct {
    int mode;          // BenchMode
    int single_route;  // 0..BENCH_ROUTE_COUNT-1 (BENCH_MODE_SINGLE)
    int duration_s;    // sample window per route (seconds)
    int settle_s;      // warm-up settle window before sampling (seconds)
    bool uncap_fps;    // uncap fps_limit, vsync, frame_gen and perf_overlay during the run
    bool camera_pan;   // gently sweep camera yaw during measurement on canonical routes
} BenchConfig;

typedef struct {
    const char *id;
    const char *name;
    float x, y, z;
    double yaw, pitch;
    float hour;
    bool hover;
} BenchRouteInfo;

typedef struct {
    char id[32];
    char name[64];
    bool valid;
    bool failed;
    char fail_reason[96];
    uint32_t frames;
    double duration_s;
    double avg_fps;
    double low_1pct_fps;
    double low_01pct_fps;
    double avg_ms;
    double median_ms;
    double p95_ms;
    double p99_ms;
    double min_ms;
    double max_ms;
    uint32_t hitches;
    uint32_t hist[4];  // frame-time bins: <8.33ms (>=120fps), 8.33-16.67ms, 16.67-33.33ms, >33.33ms (<30fps)
    float spark_ms[BENCH_SPARK_LEN];
    int spark_n;
    float vram_peak_mb;
    float ram_peak_mb;
    float gpu_load_avg;
    float gpu_temp_peak;
    float gpu_clock_avg;
} BenchRouteStats;

typedef struct {
    BenchPhase phase;
    BenchConfig cfg;
    int route_idx;   // 0-based index in the current suite
    int route_total;
    char current_route_name[64];
    double phase_elapsed_s;
    double phase_target_s;
    float live_fps;
    float live_ms;
    float live_spark[BENCH_SPARK_LEN];
    int live_spark_n;
    bool has_results;
    int nresults;
    BenchRouteStats results[BENCH_MAX_RESULTS];
    BenchRouteStats overall;
    char gpu_name[96];
    char cpu_name[64];
    int width, height;
    char settings_summary[256];
} BenchSnapshot;

int bench_route_count(void);
const BenchRouteInfo *bench_route_info(int i);

bool bench_start(const BenchConfig *cfg);
void bench_cancel(void);
bool bench_active(void);
bool bench_hud_visible(void);
void bench_snapshot(BenchSnapshot *out);
void bench_frame(uint64_t frame);

void bench_compute_stats(const uint32_t *frame_us, uint32_t count, double duration_s, BenchRouteStats *out);
size_t bench_format_markdown(const BenchSnapshot *snap, char *buf, size_t n);
bool bench_save_csv(const BenchSnapshot *snap, char *out_path, size_t out_path_n);

#ifdef __cplusplus
}
#endif
