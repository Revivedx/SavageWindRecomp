// Standalone unit tests for runtime/benchmark.c (stats, percentiles, 1%/0.1% lows, hitches, sparkline, markdown).
#define BENCH_STATS_ONLY
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/benchmark.c"

static int near_d(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void test_steady_60fps(void) {
    uint32_t us[600];
    for (int i = 0; i < 600; i++) us[i] = 16667;
    BenchRouteStats st = {0};
    snprintf(st.id, sizeof st.id, "plateau");
    snprintf(st.name, sizeof st.name, "Great Plateau");
    bench_compute_stats(us, 600, 10.0, &st);
    assert(st.valid && !st.failed);
    assert(st.frames == 600);
    assert(near_d(st.avg_fps, 60.0, 0.05));
    assert(near_d(st.low_1pct_fps, 60.0, 0.05));
    assert(near_d(st.low_01pct_fps, 60.0, 0.05));
    assert(near_d(st.median_ms, 16.667, 0.01));
    assert(near_d(st.p99_ms, 16.667, 0.01));
    assert(st.hitches == 0);
    assert(st.spark_n == BENCH_SPARK_LEN);
}

static void test_spikes_and_hitches(void) {
    // 1000 frames: 988 at 10.0 ms (100 fps), 10 at 40.0 ms, 2 at 100.0 ms
    uint32_t us[1000];
    for (int i = 0; i < 1000; i++) us[i] = 10000;
    for (int i = 0; i < 10; i++) us[100 + i * 50] = 40000;
    us[251] = 100000;
    us[751] = 100000;

    BenchRouteStats st = {0};
    snprintf(st.id, sizeof st.id, "korok");
    snprintf(st.name, sizeof st.name, "Korok Forest");
    st.vram_peak_mb = 2450.0f;
    bench_compute_stats(us, 1000, 10.48, &st);
    assert(st.valid);
    assert(near_d(st.median_ms, 10.0, 0.01));
    assert(near_d(st.min_ms, 10.0, 0.01));
    assert(near_d(st.max_ms, 100.0, 0.01));
    assert(st.p99_ms >= 39.9 && st.p99_ms <= 41.0);
    // 1% low (slowest 10 frames): 8 * 40 ms + 2 * 100 ms = 520 ms / 10 = 52 ms => 19.23 fps
    assert(near_d(st.low_1pct_fps, 1000.0 / 52.0, 0.1));
    // 0.1% low (slowest 1 frame): 100 ms => 10.0 fps
    assert(near_d(st.low_01pct_fps, 10.0, 0.1));
    // Hitches: threshold is max(2 * 10 = 20, 33.333) = 33.333 ms => 10 + 2 = 12 hitches
    assert(st.hitches == 12);
    // Metadata preserved
    assert(near_d(st.vram_peak_mb, 2450.0, 0.01));
    // Max downsampling must preserve the 100 ms spikes in the sparkline
    float max_spark = 0.0f;
    for (int i = 0; i < st.spark_n; i++)
        if (st.spark_ms[i] > max_spark) max_spark = st.spark_ms[i];
    assert(near_d(max_spark, 100.0, 0.01));
}

static void test_markdown_and_routes(void) {
    assert(bench_route_count() == BENCH_ROUTE_COUNT);
    for (int i = 0; i < BENCH_ROUTE_COUNT; i++) {
        const BenchRouteInfo *r = bench_route_info(i);
        assert(r && r->id && r->name);
    }
    assert(bench_route_info(-1) == NULL);
    assert(bench_route_info(BENCH_ROUTE_COUNT) == NULL);

    uint32_t us[120];
    for (int i = 0; i < 120; i++) us[i] = 8333;
    BenchSnapshot snap = {0};
    snprintf(snap.gpu_name, sizeof snap.gpu_name, "NVIDIA GeForce RTX 3050");
    snprintf(snap.cpu_name, sizeof snap.cpu_name, "AMD Ryzen 7");
    snap.width = 1920;
    snap.height = 1080;
    snprintf(snap.settings_summary, sizeof snap.settings_summary, "Upscaler: Off | AA: TAA");
    snap.has_results = true;
    snap.nresults = 1;
    snprintf(snap.results[0].id, sizeof snap.results[0].id, "plateau");
    snprintf(snap.results[0].name, sizeof snap.results[0].name, "Great Plateau");
    snap.results[0].vram_peak_mb = 1800.0f;
    bench_compute_stats(us, 120, 1.0, &snap.results[0]);
    snap.overall = snap.results[0];

    char md[4096];
    size_t len = bench_format_markdown(&snap, md, sizeof md);
    assert(len > 0 && len < sizeof md);
    assert(strstr(md, "## Savage Wind Benchmark") != NULL);
    assert(strstr(md, "NVIDIA GeForce RTX 3050") != NULL);
    assert(strstr(md, "| Great Plateau | 120.0 |") != NULL);
    assert(strstr(md, "| **Overall** | **120.0** |") != NULL);
}

int main(void) {
    test_steady_60fps();
    test_spikes_and_hitches();
    test_markdown_and_routes();
    printf("PASS: bench_compute_stats, hitches, 1%%/0.1%% lows, sparkline, markdown, routes\n");
    return 0;
}
