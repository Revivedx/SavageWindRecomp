// Built-in fixed-view performance benchmark (runtime/benchmark.h, runtime/ui/benchmark_ui.mm).
#include "benchmark.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static const BenchRouteInfo k_routes[BENCH_ROUTE_COUNT] = {
    {"plateau",  "Great Plateau",    -1041.6f, 250.2f,  1812.1f, 150.0,  -3.0, 10.0f, false},
    {"kakariko", "Kakariko Village",  1811.7f, 218.5f,   982.6f, 180.0, -10.0, 10.0f, false},
    {"field",    "Hyrule Field",       384.8f, 121.7f,  1035.6f, 180.0,  -3.0, 10.0f, false},
    {"rito",     "Rito Village",     -3610.0f, 300.0f, -1730.0f, 180.0,  -8.0, 10.0f, true},
    {"castle",   "Hyrule Castle",     -254.0f, 260.0f,  -400.0f, 180.0,   0.0, 10.0f, true},
    {"hateno",   "Hateno Village",    3437.5f, 235.9f,  2101.5f,  90.0,  -8.0, 10.0f, false},
    {"korok",    "Korok Forest",       462.6f, 249.6f, -2110.1f,   0.0,  -8.0, 10.0f, false},
};

int bench_route_count(void) { return BENCH_ROUTE_COUNT; }

const BenchRouteInfo *bench_route_info(int i) {
    return (i >= 0 && i < BENCH_ROUTE_COUNT) ? &k_routes[i] : NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a, vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

static double sorted_percentile_ms(const uint32_t *s, uint32_t n, double p) {
    if (!n) return 0.0;
    double idx = p * (double)(n - 1);
    uint32_t lo = (uint32_t)idx, hi = lo + 1 < n ? lo + 1 : lo;
    double frac = idx - (double)lo;
    return ((double)s[lo] * (1.0 - frac) + (double)s[hi] * frac) / 1000.0;
}

static double top_tail_fps(const uint32_t *s, uint32_t n, double frac) {
    if (!n) return 0.0;
    uint32_t k = (uint32_t)ceil((double)n * frac);
    if (k < 1) k = 1;
    if (k > n) k = n;
    uint64_t sum = 0;
    for (uint32_t i = n - k; i < n; i++) sum += s[i];
    double mean_ms = (double)sum / ((double)k * 1000.0);
    return mean_ms > 0.0 ? 1000.0 / mean_ms : 0.0;
}

void bench_compute_stats(const uint32_t *frame_us, uint32_t count, double duration_s, BenchRouteStats *out) {
    if (!out) return;
    char saved_id[32], saved_name[64];
    memcpy(saved_id, out->id, sizeof saved_id);
    memcpy(saved_name, out->name, sizeof saved_name);
    float vram = out->vram_peak_mb, ram = out->ram_peak_mb;
    float gload = out->gpu_load_avg, gtemp = out->gpu_temp_peak, gclk = out->gpu_clock_avg;
    memset(out, 0, sizeof *out);
    memcpy(out->id, saved_id, sizeof out->id);
    memcpy(out->name, saved_name, sizeof out->name);
    out->vram_peak_mb = vram;
    out->ram_peak_mb = ram;
    out->gpu_load_avg = gload;
    out->gpu_temp_peak = gtemp;
    out->gpu_clock_avg = gclk;

    if (!frame_us || count == 0) {
        out->failed = true;
        snprintf(out->fail_reason, sizeof out->fail_reason, "No frames recorded");
        return;
    }
    uint32_t *s = (uint32_t *)malloc((size_t)count * sizeof *s);
    if (!s) {
        out->failed = true;
        snprintf(out->fail_reason, sizeof out->fail_reason, "Out of memory");
        return;
    }
    uint64_t sum_us = 0;
    for (uint32_t i = 0; i < count; i++) {
        s[i] = frame_us[i];
        sum_us += frame_us[i];
    }
    qsort(s, count, sizeof *s, cmp_u32);

    out->valid = true;
    out->frames = count;
    double elapsed_s = duration_s > 0.0 ? duration_s : (double)sum_us / 1000000.0;
    out->duration_s = elapsed_s;
    out->avg_ms = (double)sum_us / ((double)count * 1000.0);
    out->avg_fps = elapsed_s > 0.0 ? (double)count / elapsed_s : (out->avg_ms > 0.0 ? 1000.0 / out->avg_ms : 0.0);
    out->min_ms = (double)s[0] / 1000.0;
    out->max_ms = (double)s[count - 1] / 1000.0;
    out->median_ms = sorted_percentile_ms(s, count, 0.50);
    out->p95_ms = sorted_percentile_ms(s, count, 0.95);
    out->p99_ms = sorted_percentile_ms(s, count, 0.99);
    out->low_1pct_fps = top_tail_fps(s, count, 0.01);
    out->low_01pct_fps = top_tail_fps(s, count, 0.001);

    uint32_t hitches = 0;
    double thresh = out->median_ms * 2.0;
    if (thresh < 33.333) thresh = 33.333;
    for (uint32_t i = 0; i < count; i++) {
        double ms = (double)frame_us[i] / 1000.0;
        if (ms > thresh) hitches++;
        if (ms < 8.333) out->hist[0]++;
        else if (ms < 16.667) out->hist[1]++;
        else if (ms <= 33.333) out->hist[2]++;
        else out->hist[3]++;
    }
    out->hitches = hitches;
    free(s);

    // Downsample raw frame times into BENCH_SPARK_LEN buckets keeping the bucket max so spikes stay visible.
    int nbins = count < BENCH_SPARK_LEN ? (int)count : BENCH_SPARK_LEN;
    out->spark_n = nbins;
    for (int b = 0; b < nbins; b++) {
        uint32_t start = (uint32_t)(((uint64_t)b * count) / (uint64_t)nbins);
        uint32_t end = (uint32_t)(((uint64_t)(b + 1) * count) / (uint64_t)nbins);
        if (end <= start) end = start + 1;
        uint32_t peak = 0;
        for (uint32_t j = start; j < end && j < count; j++)
            if (frame_us[j] > peak) peak = frame_us[j];
        out->spark_ms[b] = (float)peak / 1000.0f;
    }
}

static size_t appendf(char *buf, size_t cap, size_t off, const char *fmt, ...) {
    if (!buf || off >= cap) return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n <= 0) return off;
    size_t add = (size_t)n;
    return off + add < cap ? off + add : cap - 1;
}

size_t bench_format_markdown(const BenchSnapshot *snap, char *buf, size_t n) {
    if (!snap || !buf || !n) return 0;
    buf[0] = 0;
    size_t o = 0;
    o = appendf(buf, n, o, "## Savage Wind Benchmark\n\n");
    if (snap->gpu_name[0]) o = appendf(buf, n, o, "- **GPU**: %s\n", snap->gpu_name);
    if (snap->cpu_name[0]) o = appendf(buf, n, o, "- **CPU**: %s\n", snap->cpu_name);
    if (snap->width > 0 && snap->height > 0) o = appendf(buf, n, o, "- **Resolution**: %d x %d\n", snap->width, snap->height);
    if (snap->settings_summary[0]) o = appendf(buf, n, o, "- **Settings**: %s%s\n", snap->settings_summary,
                                               snap->cfg.camera_pan ? " | Camera sweep: On" : "");
    o = appendf(buf, n, o, "\n| Route | Avg FPS | 1%% Low | 0.1%% Low | Avg ms | p99 ms | Max ms | Hitches | VRAM MB |\n");
    o = appendf(buf, n, o, "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (int i = 0; i < snap->nresults && i < BENCH_MAX_RESULTS; i++) {
        const BenchRouteStats *r = &snap->results[i];
        if (!r->valid) {
            o = appendf(buf, n, o, "| %s | FAILED | - | - | - | - | - | - | - |\n", r->name[0] ? r->name : r->id);
            continue;
        }
        char vram[24] = "-";
        if (r->vram_peak_mb > 0) snprintf(vram, sizeof vram, "%.0f", r->vram_peak_mb);
        o = appendf(buf, n, o, "| %s | %.1f | %.1f | %.1f | %.2f | %.2f | %.1f | %u | %s |\n",
                    r->name, r->avg_fps, r->low_1pct_fps, r->low_01pct_fps, r->avg_ms, r->p99_ms, r->max_ms, r->hitches, vram);
    }
    if (snap->overall.valid) {
        const BenchRouteStats *r = &snap->overall;
        char vram[24] = "-";
        if (r->vram_peak_mb > 0) snprintf(vram, sizeof vram, "%.0f", r->vram_peak_mb);
        o = appendf(buf, n, o, "| **Overall** | **%.1f** | **%.1f** | **%.1f** | **%.2f** | **%.2f** | **%.1f** | **%u** | **%s** |\n",
                    r->avg_fps, r->low_1pct_fps, r->low_01pct_fps, r->avg_ms, r->p99_ms, r->max_ms, r->hitches, vram);
        if (r->frames > 0) {
            double inv = 100.0 / (double)r->frames;
            o = appendf(buf, n, o,
                        "\n- **Frame-time distribution**: `<8.3ms`: %.1f%% | `8.3-16.7ms`: %.1f%% | `16.7-33.3ms`: %.1f%% | `>33.3ms`: %.1f%%\n",
                        r->hist[0] * inv, r->hist[1] * inv, r->hist[2] * inv, r->hist[3] * inv);
        }
        if (r->gpu_load_avg > 0.0f || r->gpu_temp_peak > 0.0f || r->gpu_clock_avg > 0.0f) {
            o = appendf(buf, n, o, "- **GPU telemetry**: load %.0f%% | peak temp %.0f C | avg clock %.0f MHz\n",
                        r->gpu_load_avg, r->gpu_temp_peak, r->gpu_clock_avg);
        }
    }
    return o;
}

#ifndef BENCH_STATS_ONLY
#include <pthread.h>
#include "control.h"
#include "framepace.h"
#include "hle.h"
#include "nvn/output_scale.h"
#include "paths.h"
#include "perfmon.h"
#include "rt.h"
#include "settings.h"

extern bool control_fly_set(bool want);
extern bool control_look_start(bool use_yaw, double yaw, bool use_pitch, double pitch, int settle);
extern bool control_look_active(void);
extern float test_warp_hour(bool *held);
extern void test_warp_set_hour(float hour);
extern void test_warp_hold_hour(float hour);
extern void menu_open_page(int id);
#ifndef SW_METAL
extern void renderer_get_scale(float *sx, float *sy);
#endif
#endif

static void make_dir(const char *p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

static void csv_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

bool bench_save_csv(const BenchSnapshot *snap, char *out_path, size_t out_path_n) {
    if (!snap || !snap->has_results) return false;
#ifdef BENCH_STATS_ONLY
    const char *root = ".";
#else
    const char *root = rt_data_root();
    if (!root || !root[0]) root = ".";
#endif
    char dir[512];
    snprintf(dir, sizeof dir, "%s/benchmarks", root);
    make_dir(dir);

    time_t now = time(NULL);
    struct tm tmbuf;
#ifdef _WIN32
    localtime_s(&tmbuf, &now);
#else
    localtime_r(&now, &tmbuf);
#endif
    char path[600];
    snprintf(path, sizeof path, "%s/benchmark_%04d%02d%02d_%02d%02d%02d.csv",
             dir, tmbuf.tm_year + 1900, tmbuf.tm_mon + 1, tmbuf.tm_mday,
             tmbuf.tm_hour, tmbuf.tm_min, tmbuf.tm_sec);
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "route_id,route_name,valid,frames,duration_s,avg_fps,low_1pct_fps,low_01pct_fps,avg_ms,median_ms,p95_ms,p99_ms,min_ms,max_ms,hitches,hist_lt8ms,hist_8_17ms,hist_17_33ms,hist_gt33ms,vram_peak_mb,ram_peak_mb,gpu_load_avg,gpu_temp_peak,gpu_clock_avg,gpu,width,height,settings\n");
    for (int i = 0; i < snap->nresults && i < BENCH_MAX_RESULTS; i++) {
        const BenchRouteStats *r = &snap->results[i];
        csv_escape(f, r->id); fputc(',', f);
        csv_escape(f, r->name); fputc(',', f);
        fprintf(f, "%d,%u,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,",
                r->valid ? 1 : 0, r->frames, r->duration_s,
                r->avg_fps, r->low_1pct_fps, r->low_01pct_fps,
                r->avg_ms, r->median_ms, r->p95_ms, r->p99_ms, r->min_ms, r->max_ms, r->hitches,
                r->hist[0], r->hist[1], r->hist[2], r->hist[3],
                r->vram_peak_mb, r->ram_peak_mb, r->gpu_load_avg, r->gpu_temp_peak, r->gpu_clock_avg);
        csv_escape(f, snap->gpu_name); fputc(',', f);
        fprintf(f, "%d,%d,", snap->width, snap->height);
        csv_escape(f, snap->settings_summary); fputc('\n', f);
    }
    if (snap->overall.valid) {
        const BenchRouteStats *r = &snap->overall;
        fprintf(f, "overall,Overall,1,%u,%.3f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,",
                r->frames, r->duration_s,
                r->avg_fps, r->low_1pct_fps, r->low_01pct_fps,
                r->avg_ms, r->median_ms, r->p95_ms, r->p99_ms, r->min_ms, r->max_ms, r->hitches,
                r->hist[0], r->hist[1], r->hist[2], r->hist[3],
                r->vram_peak_mb, r->ram_peak_mb, r->gpu_load_avg, r->gpu_temp_peak, r->gpu_clock_avg);
        csv_escape(f, snap->gpu_name); fputc(',', f);
        fprintf(f, "%d,%d,", snap->width, snap->height);
        csv_escape(f, snap->settings_summary); fputc('\n', f);
    }
    fclose(f);
    if (out_path && out_path_n) snprintf(out_path, out_path_n, "%s", path);
    return true;
}

#ifndef BENCH_STATS_ONLY

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BenchSnapshot g_snap;
static bool g_pending_start;
static BenchConfig g_pending_cfg;
static bool g_pending_cancel;

static int g_queue[BENCH_ROUTE_COUNT];
static int g_queue_len;
static int g_queue_pos;

static uint64_t g_phase_t0_ns;
static uint64_t g_prev_frame_ns;
static uint64_t g_last_perf_ns;

static float g_orig_pos[3];
static float g_orig_hour;
static bool g_orig_hour_held;
static int g_orig_fps_limit;
static int g_orig_vsync;
static int g_orig_frame_gen;
static int g_orig_perf_overlay;
static bool g_overrides_saved;
static bool g_tp_sent;
static bool g_cancelled_run;

static uint32_t g_route_us[BENCH_MAX_FRAMES];
static uint32_t g_route_frames;
static uint32_t *g_all_us;
static uint32_t g_all_frames;
static uint32_t g_all_cap;
static double g_all_duration_s;

// Per-route hardware telemetry accumulators sampled once per second while measuring
static float g_vram_peak, g_ram_peak, g_temp_peak;
static double g_load_sum, g_clk_sum;
static int g_load_n, g_clk_n;

static void push_live_sample(float ms) {
    if (g_snap.live_spark_n < BENCH_SPARK_LEN) {
        g_snap.live_spark[g_snap.live_spark_n++] = ms;
    } else {
        memmove(g_snap.live_spark, g_snap.live_spark + 1, (BENCH_SPARK_LEN - 1) * sizeof(float));
        g_snap.live_spark[BENCH_SPARK_LEN - 1] = ms;
    }
    // Exponential smoothing for readable live FPS/ms readout
    if (ms > 0.01f) {
        if (g_snap.live_ms <= 0.01f) g_snap.live_ms = ms;
        else g_snap.live_ms = g_snap.live_ms * 0.90f + ms * 0.10f;
        g_snap.live_fps = g_snap.live_ms > 0.01f ? 1000.0f / g_snap.live_ms : 0.0f;
    }
}

static void capture_metadata(void) {
    PerfStats ps;
    perfmon_snapshot(&ps);
    snprintf(g_snap.gpu_name, sizeof g_snap.gpu_name, "%s", ps.gpu_name);
    snprintf(g_snap.cpu_name, sizeof g_snap.cpu_name, "%s", ps.cpu_name);

    int w = g_settings.window_w > 0 ? g_settings.window_w : 1920;
    int h = g_settings.window_h > 0 ? g_settings.window_h : 1080;
#ifndef SW_METAL
    float ox = 0.0f, oy = 0.0f;
    renderer_get_scale(&ox, &oy);
    if (ox > 0.0f && oy > 0.0f) {
        w = (int)lroundf(1920.0f * ox);
        h = (int)lroundf(1080.0f * oy);
    }
#endif
    g_snap.width = w;
    g_snap.height = h;

    static const char *const up_names[] = {"Off", "MetalFX", "FSR 1", "FSR 3", "DLSS", "FSR 4", "XeSS"};
    static const char *const sh_names[] = {"Console (896)", "High (2048)", "Ultra (4096)", "Extreme (8192)", "Potato (448)"};
    static const char *const dd_names[] = {"Original", "Medium", "High", "Potato"};
    int up = g_settings.upscaler >= 0 && g_settings.upscaler < UPSCALE_COUNT ? g_settings.upscaler : 0;
    int sh = g_settings.shadow_res >= 0 && g_settings.shadow_res < SHADOW_COUNT ? g_settings.shadow_res : 0;
    int dd = g_settings.draw_distance >= 0 && g_settings.draw_distance < DRAWDIST_COUNT ? g_settings.draw_distance : 0;
    int aa = rt_antialias();
    const char *aa_s = rt_temporal_upscaler() ? rt_temporal_upscaler_name()
                     : aa == AA_TAA ? "TAA" : aa == AA_SMAA ? "SMAA"
                     : g_settings.game_fxaa ? "FXAA" : "Off";
    snprintf(g_snap.settings_summary, sizeof g_snap.settings_summary,
             "Upscaler: %s | AA: %s | Shadows: %s | Draw distance: %s",
             up_names[up], aa_s, sh_names[sh], dd_names[dd]);
}

static void save_and_apply_overrides(const BenchConfig *cfg) {
    if (!g_overrides_saved) {
        g_orig_fps_limit = g_settings.fps_limit;
        g_orig_vsync = g_settings.vsync;
        g_orig_frame_gen = g_settings.frame_gen;
        g_orig_perf_overlay = g_settings.perf_overlay;
        g_overrides_saved = true;
    }
    if (cfg->uncap_fps) {
        g_settings.fps_limit = 0;
        g_settings.vsync = VSYNC_OFF;
        g_settings.frame_gen = FG_OFF;
        g_settings.perf_overlay = PERF_OFF;
    }
}

static void restore_overrides(void) {
    if (g_overrides_saved) {
        g_settings.fps_limit = g_orig_fps_limit;
        g_settings.vsync = g_orig_vsync;
        g_settings.frame_gen = g_orig_frame_gen;
        g_settings.perf_overlay = g_orig_perf_overlay;
        g_overrides_saved = false;
    }
    control_fly_set(false);
    if (g_snap.cfg.mode != BENCH_MODE_SPOT)
        test_warp_hold_hour(g_orig_hour_held && g_orig_hour >= 0.0f ? g_orig_hour : -1.0f);
}

static void build_route_queue(const BenchConfig *cfg) {
    g_queue_len = 0;
    g_queue_pos = 0;
    if (cfg->mode == BENCH_MODE_FULL) {
        for (int i = 0; i < BENCH_ROUTE_COUNT; i++) g_queue[g_queue_len++] = i;
    } else if (cfg->mode == BENCH_MODE_QUICK) {
        g_queue[g_queue_len++] = 0;  // Great Plateau
        g_queue[g_queue_len++] = 1;  // Kakariko Village
        g_queue[g_queue_len++] = 6;  // Korok Forest
    } else if (cfg->mode == BENCH_MODE_SINGLE) {
        int r = cfg->single_route;
        if (r < 0 || r >= BENCH_ROUTE_COUNT) r = 0;
        g_queue[g_queue_len++] = r;
    } else {
        g_queue[g_queue_len++] = -1;  // Current spot
    }
}

static void begin_phase(BenchPhase p, uint64_t now_ns, double target_s) {
    g_snap.phase = p;
    g_phase_t0_ns = now_ns;
    g_snap.phase_elapsed_s = 0.0;
    g_snap.phase_target_s = target_s;
}

static void start_current_route(uint64_t now_ns) {
    int ridx = g_queue[g_queue_pos];
    g_snap.route_idx = g_queue_pos;
    g_snap.route_total = g_queue_len;
    g_tp_sent = false;
    if (ridx < 0) {
        snprintf(g_snap.current_route_name, sizeof g_snap.current_route_name, "Current spot");
        begin_phase(BENCH_SETTLING, now_ns, (double)g_snap.cfg.settle_s);
    } else {
        snprintf(g_snap.current_route_name, sizeof g_snap.current_route_name, "%s", k_routes[ridx].name);
        begin_phase(BENCH_TELEPORTING, now_ns, 45.0);
    }
}

static void compute_overall_stats(void) {
    if (g_all_frames == 0) {
        memset(&g_snap.overall, 0, sizeof g_snap.overall);
        return;
    }
    snprintf(g_snap.overall.id, sizeof g_snap.overall.id, "overall");
    snprintf(g_snap.overall.name, sizeof g_snap.overall.name, "Overall");
    float vram_max = 0, ram_max = 0, temp_max = 0;
    double gload_sum = 0, gclk_sum = 0;
    int gload_n = 0, gclk_n = 0;
    for (int i = 0; i < g_snap.nresults; i++) {
        const BenchRouteStats *r = &g_snap.results[i];
        if (!r->valid) continue;
        if (r->vram_peak_mb > vram_max) vram_max = r->vram_peak_mb;
        if (r->ram_peak_mb > ram_max) ram_max = r->ram_peak_mb;
        if (r->gpu_temp_peak > temp_max) temp_max = r->gpu_temp_peak;
        if (r->gpu_load_avg >= 0) { gload_sum += r->gpu_load_avg; gload_n++; }
        if (r->gpu_clock_avg > 0) { gclk_sum += r->gpu_clock_avg; gclk_n++; }
    }
    g_snap.overall.vram_peak_mb = vram_max;
    g_snap.overall.ram_peak_mb = ram_max;
    g_snap.overall.gpu_temp_peak = temp_max;
    g_snap.overall.gpu_load_avg = gload_n ? (float)(gload_sum / gload_n) : -1.0f;
    g_snap.overall.gpu_clock_avg = gclk_n ? (float)(gclk_sum / gclk_n) : -1.0f;
    bench_compute_stats(g_all_us, g_all_frames, g_all_duration_s, &g_snap.overall);
    // bench_compute_stats preserves metadata we set on out above before memsetting frame stats
}

static void enter_restoring(uint64_t now_ns) {
    compute_overall_stats();
    g_snap.has_results = g_snap.nresults > 0;
    restore_overrides();
    g_tp_sent = false;
    if (g_snap.cfg.mode == BENCH_MODE_SPOT) {
        g_snap.phase = g_cancelled_run ? BENCH_CANCELLED : BENCH_DONE;
        menu_open_page(15);
        return;
    }
    begin_phase(BENCH_RESTORING, now_ns, 15.0);
}

static void record_failed_route(int ridx, const char *reason) {
    if (g_snap.nresults >= BENCH_MAX_RESULTS) return;
    BenchRouteStats *st = &g_snap.results[g_snap.nresults++];
    memset(st, 0, sizeof *st);
    snprintf(st->id, sizeof st->id, "%s", ridx >= 0 ? k_routes[ridx].id : "spot");
    snprintf(st->name, sizeof st->name, "%s", ridx >= 0 ? k_routes[ridx].name : "Current spot");
    st->failed = true;
    snprintf(st->fail_reason, sizeof st->fail_reason, "%s", reason);
}

static void finish_measuring_route(uint64_t now_ns, double elapsed_s) {
    int ridx = g_queue[g_queue_pos];
    if (g_snap.nresults < BENCH_MAX_RESULTS) {
        BenchRouteStats *st = &g_snap.results[g_snap.nresults++];
        memset(st, 0, sizeof *st);
        snprintf(st->id, sizeof st->id, "%s", ridx >= 0 ? k_routes[ridx].id : "spot");
        snprintf(st->name, sizeof st->name, "%s", ridx >= 0 ? k_routes[ridx].name : "Current spot");
        st->vram_peak_mb = g_vram_peak;
        st->ram_peak_mb = g_ram_peak;
        st->gpu_temp_peak = g_temp_peak;
        st->gpu_load_avg = g_load_n ? (float)(g_load_sum / g_load_n) : -1.0f;
        st->gpu_clock_avg = g_clk_n ? (float)(g_clk_sum / g_clk_n) : -1.0f;
        bench_compute_stats(g_route_us, g_route_frames, elapsed_s, st);
    }
    // Append to combined buffer for accurate overall percentiles & 1% / 0.1% lows
    if (g_route_frames > 0) {
        uint32_t need = g_all_frames + g_route_frames;
        if (need > g_all_cap) {
            uint32_t new_cap = g_all_cap ? g_all_cap : 4096;
            while (new_cap < need) new_cap *= 2;
            uint32_t *p = (uint32_t *)realloc(g_all_us, (size_t)new_cap * sizeof *p);
            if (p) { g_all_us = p; g_all_cap = new_cap; }
        }
        if (g_all_us && g_all_frames + g_route_frames <= g_all_cap) {
            memcpy(g_all_us + g_all_frames, g_route_us, (size_t)g_route_frames * sizeof(uint32_t));
            g_all_frames += g_route_frames;
            g_all_duration_s += elapsed_s;
        }
    }
    g_queue_pos++;
    if (g_queue_pos < g_queue_len) start_current_route(now_ns);
    else enter_restoring(now_ns);
}

bool bench_start(const BenchConfig *cfg) {
    if (!cfg) return false;
    if (framepace_title_ui() || framepace_loading_ui() || framepace_menu_ui()) return false;
    float pos[3];
    if (!control_tp_link_pos(pos)) return false;
    pthread_mutex_lock(&g_lock);
    bool busy = (g_snap.phase >= BENCH_TELEPORTING && g_snap.phase <= BENCH_RESTORING) || g_pending_start;
    if (!busy) {
        g_pending_cfg = *cfg;
        if (g_pending_cfg.duration_s < 3) g_pending_cfg.duration_s = 3;
        if (g_pending_cfg.duration_s > 60) g_pending_cfg.duration_s = 60;
        if (g_pending_cfg.settle_s < 1) g_pending_cfg.settle_s = 1;
        if (g_pending_cfg.settle_s > 20) g_pending_cfg.settle_s = 20;
        g_pending_start = true;
        g_pending_cancel = false;
    }
    pthread_mutex_unlock(&g_lock);
    return !busy;
}

void bench_cancel(void) {
    pthread_mutex_lock(&g_lock);
    if (g_pending_start) {
        g_pending_start = false;
    } else if (g_snap.phase >= BENCH_TELEPORTING && g_snap.phase <= BENCH_RESTORING) {
        g_pending_cancel = true;
    }
    pthread_mutex_unlock(&g_lock);
}

bool bench_active(void) {
    pthread_mutex_lock(&g_lock);
    bool a = g_snap.phase >= BENCH_TELEPORTING && g_snap.phase <= BENCH_RESTORING;
    pthread_mutex_unlock(&g_lock);
    return a;
}

bool bench_hud_visible(void) {
    return bench_active();
}

void bench_snapshot(BenchSnapshot *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_snap;
    pthread_mutex_unlock(&g_lock);
}

void bench_frame(uint64_t frame) {
    (void)frame;
    uint64_t now = plat_now_ns();
    uint64_t dt_ns = (g_prev_frame_ns && now > g_prev_frame_ns) ? (now - g_prev_frame_ns) : 0;
    g_prev_frame_ns = now;

    pthread_mutex_lock(&g_lock);
    if (g_pending_start) {
        g_pending_start = false;
        g_cancelled_run = false;
        if (control_tp_link_pos(g_orig_pos)) {
            g_orig_hour = test_warp_hour(&g_orig_hour_held);
            memset(&g_snap.results, 0, sizeof g_snap.results);
            memset(&g_snap.overall, 0, sizeof g_snap.overall);
            g_snap.nresults = 0;
            g_snap.has_results = false;
            g_snap.live_spark_n = 0;
            g_snap.live_fps = 0.0f;
            g_snap.live_ms = 0.0f;
            g_snap.cfg = g_pending_cfg;
            g_all_frames = 0;
            g_all_duration_s = 0.0;
            capture_metadata();
            save_and_apply_overrides(&g_snap.cfg);
            build_route_queue(&g_snap.cfg);
            start_current_route(now);
            LOG("[benchmark] started mode=%d routes=%d duration=%ds settle=%ds\n",
                g_snap.cfg.mode, g_queue_len, g_snap.cfg.duration_s, g_snap.cfg.settle_s);
        }
    }

    if (g_pending_cancel) {
        g_pending_cancel = false;
        if (g_snap.phase == BENCH_RESTORING) {
            restore_overrides();
            g_snap.phase = BENCH_CANCELLED;
            menu_open_page(15);
            LOG("[benchmark] force-cancelled during restore\n");
        } else if (g_snap.phase >= BENCH_TELEPORTING && g_snap.phase < BENCH_RESTORING) {
            g_cancelled_run = true;
            LOG("[benchmark] cancelled by user\n");
            enter_restoring(now);
        }
    }

    if (g_snap.phase < BENCH_TELEPORTING || g_snap.phase > BENCH_RESTORING) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (dt_ns > 0) push_live_sample((float)((double)dt_ns / 1e6));
    double elapsed = (double)(now - g_phase_t0_ns) / 1e9;
    g_snap.phase_elapsed_s = elapsed;

    int ridx = (g_queue_pos >= 0 && g_queue_pos < g_queue_len) ? g_queue[g_queue_pos] : -1;

    switch (g_snap.phase) {
    case BENCH_TELEPORTING: {
        if (ridx < 0 || ridx >= BENCH_ROUTE_COUNT) { enter_restoring(now); break; }
        const BenchRouteInfo *r = &k_routes[ridx];
        if (!g_tp_sent) {
            if (control_tp_request(r->x, r->y, r->z)) {
                g_tp_sent = true;
            } else if (elapsed > 5.0) {
                record_failed_route(ridx, "Teleport unavailable");
                g_queue_pos++;
                if (g_queue_pos < g_queue_len) start_current_route(now);
                else enter_restoring(now);
            }
            break;
        }
        int st = control_tp_status(NULL, 0);
        if (st == 2) {
            control_fly_set(true);
            test_warp_set_hour(r->hour);
            test_warp_hold_hour(r->hour);
            control_look_start(true, r->yaw, true, r->pitch, 15);
            begin_phase(BENCH_AIMING, now, 4.0);
        } else if (st == 3 || elapsed >= 45.0) {
            record_failed_route(ridx, st == 3 ? "Teleport failed" : "Teleport timed out");
            g_queue_pos++;
            if (g_queue_pos < g_queue_len) start_current_route(now);
            else enter_restoring(now);
        }
        break;
    }
    case BENCH_AIMING: {
        if (!control_look_active() || elapsed >= 4.0) {
            begin_phase(BENCH_SETTLING, now, (double)g_snap.cfg.settle_s);
        }
        break;
    }
    case BENCH_SETTLING: {
        if (elapsed >= (double)g_snap.cfg.settle_s) {
            g_route_frames = 0;
            g_vram_peak = 0.0f;
            g_ram_peak = 0.0f;
            g_temp_peak = 0.0f;
            g_load_sum = 0.0;
            g_clk_sum = 0.0;
            g_load_n = 0;
            g_clk_n = 0;
            g_last_perf_ns = 0;
            begin_phase(BENCH_MEASURING, now, (double)g_snap.cfg.duration_s);
        }
        break;
    }
    case BENCH_MEASURING: {
        if (dt_ns > 0 && g_route_frames < BENCH_MAX_FRAMES) {
            uint32_t us = (uint32_t)(dt_ns / 1000ull);
            g_route_us[g_route_frames++] = us ? us : 1;
        }
        if (!g_last_perf_ns || now - g_last_perf_ns >= 1000000000ull) {
            g_last_perf_ns = now;
            PerfStats ps;
            perfmon_snapshot(&ps);
            if (ps.vram_used_mb > g_vram_peak) g_vram_peak = ps.vram_used_mb;
            if (ps.ram_game_mb > g_ram_peak) g_ram_peak = ps.ram_game_mb;
            if (ps.gpu_temp_c > g_temp_peak) g_temp_peak = ps.gpu_temp_c;
            if (ps.gpu_pct >= 0.0f) { g_load_sum += ps.gpu_pct; g_load_n++; }
            if (ps.gpu_clock_mhz > 0.0f) { g_clk_sum += ps.gpu_clock_mhz; g_clk_n++; }
        }
        if (g_snap.cfg.camera_pan && ridx >= 0 && !control_look_active()) {
            double pan_yaw = k_routes[ridx].yaw + 30.0 * sin(elapsed * 0.5);
            control_look_start(true, pan_yaw, true, k_routes[ridx].pitch, 1);
        }
        if (elapsed >= (double)g_snap.cfg.duration_s || g_route_frames >= BENCH_MAX_FRAMES) {
            finish_measuring_route(now, elapsed);
        }
        break;
    }
    case BENCH_RESTORING: {
        if (!g_tp_sent) {
            if (control_tp_request(g_orig_pos[0], g_orig_pos[1], g_orig_pos[2])) g_tp_sent = true;
            else if (elapsed >= 2.0) {
                g_snap.phase = g_cancelled_run ? BENCH_CANCELLED : BENCH_DONE;
                menu_open_page(15);
            }
            break;
        }
        int st = control_tp_status(NULL, 0);
        if (st == 2 || st == 3 || elapsed >= 15.0) {
            g_snap.phase = g_cancelled_run ? BENCH_CANCELLED : BENCH_DONE;
            menu_open_page(15);
            LOG("[benchmark] finished (%s), %d routes recorded\n",
                g_cancelled_run ? "cancelled" : "done", g_snap.nresults);
        }
        break;
    }
    default:
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

RT_ON_RESET(bench_reset) {
    pthread_mutex_lock(&g_lock);
    if (g_overrides_saved) {
        g_settings.fps_limit = g_orig_fps_limit;
        g_settings.vsync = g_orig_vsync;
        g_settings.frame_gen = g_orig_frame_gen;
        g_settings.perf_overlay = g_orig_perf_overlay;
        g_overrides_saved = false;
    }
    g_snap.phase = BENCH_IDLE;
    g_pending_start = false;
    g_pending_cancel = false;
    g_tp_sent = false;
    g_cancelled_run = false;
    g_prev_frame_ns = 0;
    free(g_all_us);
    g_all_us = NULL;
    g_all_cap = 0;
    g_all_frames = 0;
    pthread_mutex_unlock(&g_lock);
}

static void cmd_bench(int argc, char **argv, ControlReply *r) {
    if (argc < 2 || !strcmp(argv[1], "status")) {
        BenchSnapshot snap;
        bench_snapshot(&snap);
        static const char *const phase_names[] = {
            "idle", "teleporting", "aiming", "settling", "measuring", "restoring", "done", "cancelled"
        };
        const char *pn = (snap.phase >= 0 && snap.phase <= BENCH_CANCELLED) ? phase_names[snap.phase] : "unknown";
        control_field_str(r, "phase", pn);
        control_field_bool(r, "has_results", snap.has_results);
        if (snap.phase >= BENCH_TELEPORTING && snap.phase <= BENCH_RESTORING) {
            control_field_str(r, "route", snap.current_route_name);
            control_field_num(r, "live_fps", snap.live_fps);
            control_text(r, "Benchmark %s: %s (%d/%d, %.1f fps)", pn, snap.current_route_name,
                         snap.route_idx + 1, snap.route_total, snap.live_fps);
        } else if (snap.has_results && snap.overall.valid) {
            control_field_num(r, "avg_fps", snap.overall.avg_fps);
            control_field_num(r, "low_1pct_fps", snap.overall.low_1pct_fps);
            control_field_int(r, "hitches", (int64_t)snap.overall.hitches);
            control_text(r, "Benchmark %s: %.1f avg fps, %.1f 1%% low, %u hitches (%d routes)",
                         pn, snap.overall.avg_fps, snap.overall.low_1pct_fps, snap.overall.hitches, snap.nresults);
        } else {
            control_text(r, "Benchmark %s", pn);
        }
        return;
    }
    if (!strcmp(argv[1], "cancel")) {
        bench_cancel();
        control_text(r, "Benchmark cancel requested");
        return;
    }
    BenchConfig cfg = {BENCH_MODE_FULL, 0, 10, 5, true, false};
    if (!strcmp(argv[1], "full")) cfg.mode = BENCH_MODE_FULL;
    else if (!strcmp(argv[1], "quick")) cfg.mode = BENCH_MODE_QUICK;
    else if (!strcmp(argv[1], "spot")) cfg.mode = BENCH_MODE_SPOT;
    else if (!strcmp(argv[1], "route")) {
        if (argc < 3) {
            control_fail(r, "bench route: specify one of plateau, kakariko, field, rito, castle, hateno, korok");
            return;
        }
        int found = -1;
        for (int i = 0; i < BENCH_ROUTE_COUNT; i++)
            if (!strcmp(argv[2], k_routes[i].id)) { found = i; break; }
        if (found < 0) {
            control_fail(r, "bench route: unknown route '%s'", argv[2]);
            return;
        }
        cfg.mode = BENCH_MODE_SINGLE;
        cfg.single_route = found;
    } else {
        control_fail(r, "bench: use bench [status | full | quick | spot | route <name> | cancel]");
        return;
    }
    if (!bench_start(&cfg)) {
        control_fail(r, "bench: cannot start (must be in active gameplay, not in menus or already running)");
        return;
    }
    control_text(r, "Benchmark started (%s)", argv[1]);
}

void bench_control_init(void) {
    static const ControlCmd cmds[] = {
        {"bench", "[status | full | quick | spot | route <name> | cancel]", "Run or inspect the built-in performance benchmark", cmd_bench, CONTROL_PLAYER},
    };
    control_register(cmds, 1);
}

#endif
