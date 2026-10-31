// Control endpoint profiling commands. All handlers run on the presenting thread.
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "platform.h"

// sampler.c: status is a static string; start returns NULL on success, an error otherwise.
const char *sampler_status(void);
const char *sampler_request(double seconds, unsigned interval_us);

// 60 seconds at the frame pacer's 300 fps ceiling, with headroom. No work when control is off.
#define FRAME_CAP 32768
static struct { uint64_t end, ns; } frames[FRAME_CAP];
static uint64_t sorted[FRAME_CAP];
static unsigned head, used;
static uint64_t last_ns, last_frame;
static bool hooked;

// Integration: declare this and call control_prof_frame(frame) at the start of control_game_frame.
// Both this hook and the commands must run on the presenting thread.
void control_prof_frame(uint64_t frame) {
    uint64_t now = plat_now_ns();
    if (hooked && frame == last_frame) return;
    if (hooked && frame > last_frame && now > last_ns) {
        frames[head].end = now;
        frames[head].ns = now - last_ns;
        head = (head + 1) % FRAME_CAP;
        if (used < FRAME_CAP) used++;
    } else if (hooked) {
        head = used = 0;  // a frame counter or clock reset starts a new history
    }
    hooked = true;
    last_ns = now;
    last_frame = frame;
}

static bool number(const char *s, double lo, double hi, double *out) {
    char *end;
    errno = 0;
    double n = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(n) || n < lo || n > hi) return false;
    *out = n;
    return true;
}
static int compare_ns(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void cmd_frames(int argc, char **argv, ControlReply *r) {
    double seconds = 5;
    if (argc > 2 || (argc == 2 && (!number(argv[1], 0, 60, &seconds) || seconds <= 0))) {
        control_fail(r, "Usage: frames [seconds] (up to 60)"); return;
    }
    if (!hooked) {
        control_fail(r, "Frame hook missing"); return;
    }
    uint64_t now = plat_now_ns(), window = (uint64_t)(seconds * 1e9);
    unsigned n = 0, over33 = 0, over50 = 0;
    double total = 0;
    for (unsigned i = 0; i < used; i++) {
        unsigned at = (head + FRAME_CAP - 1 - i) % FRAME_CAP;
        if (now < frames[at].end || now - frames[at].end >= window) break;
        uint64_t ns = frames[at].ns;
        sorted[n++] = ns;
        total += (double)ns;
        over33 += ns > 33000000;
        over50 += ns > 50000000;
    }
    if (!n) { control_fail(r, "No frames in this window"); return; }
    qsort(sorted, n, sizeof *sorted, compare_ns);
    control_field_num(r, "seconds", seconds);
    control_field_int(r, "count", n);
    control_field_num(r, "covered_s", total / 1e9);
    control_field_bool(r, "capacity_limited", used == FRAME_CAP && now >= frames[head].end && now - frames[head].end < window);
    control_field_num(r, "fps_avg", n * 1e9 / total);
    control_field_num(r, "frame_ms_avg", total / n / 1e6);
    // Nearest-rank percentiles, including the full interval ending inside the window.
    control_field_num(r, "frame_ms_p50", sorted[(n * 50 + 99) / 100 - 1] / 1e6);
    control_field_num(r, "frame_ms_p95", sorted[(n * 95 + 99) / 100 - 1] / 1e6);
    control_field_num(r, "frame_ms_p99", sorted[(n * 99 + 99) / 100 - 1] / 1e6);
    control_field_num(r, "frame_ms_max", sorted[n - 1] / 1e6);
    control_field_int(r, "over_33_ms", over33);
    control_field_int(r, "over_50_ms", over50);
}
static void cmd_sample(int argc, char **argv, ControlReply *r) {
    if (argc == 2 && !strcmp(argv[1], "status")) {
        control_field_str(r, "status", sampler_status());
        control_field_str(r, "path", "build/samples.txt");
        return;
    }
    double seconds, interval = 1000;
    if ((argc != 3 && argc != 4) || strcmp(argv[1], "start") ||
        !number(argv[2], 0.001, 3600, &seconds) ||
        (argc == 4 && (!number(argv[3], 100, 1000000, &interval) || interval != floor(interval)))) {
        control_fail(r, "Usage: sample start <seconds> [interval_us] | status"); return;
    }
    const char *error = sampler_request(seconds, (unsigned)interval);
    if (error) { control_fail(r, "sample: %s", error); return; }
    control_field_str(r, "status", "started");
    control_field_num(r, "seconds", seconds);
    control_field_int(r, "interval_us", (unsigned)interval);
    control_field_str(r, "path", "build/samples.txt");
}
static void cmd_vkprof(int argc, char **argv, ControlReply *r) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "0") && strcmp(argv[1], "1") && strcmp(argv[1], "2"))) {
        control_fail(r, "Usage: vkprof [0|1|2]"); return;
    }
    control_fail(r, "Set at startup with SAVAGEWIND_VK_PROF (1 detailed, 2 coarse)");
}
static void cmd_gpuprof(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: gpuprof"); return; }
    control_fail(r, "Set at startup with SAVAGEWIND_GPUPROF_AT=<frame>");
}
void control_prof_init(void) {
    static const ControlCmd cmds[] = {
        {"frames", "[seconds]", "Frame times over the last seconds (default 5, maximum 60)", cmd_frames},
        {"sample", "start <seconds> [interval_us] | status", "Sample guest stacks to build/samples.txt; show status", cmd_sample},
        {"vkprof", "[0|1|2]", "Vulkan timers (startup setting)", cmd_vkprof},
        {"gpuprof", "", "GPU capture (startup setting)", cmd_gpuprof},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
