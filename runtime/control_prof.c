// Control endpoint profiling commands. All handlers run on the presenting thread.
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "kernel.h"
#include "perfcount.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

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

_Atomic uint64_t g_perfcount[PC_COUNT];
_Atomic int64_t g_texmem[TM_COUNT], g_texmem_n[TM_COUNT];
int (*g_gpu_heaps)(uint64_t *usage, uint64_t *budget, uint64_t *size, uint32_t *device_local, int max);

// The process's page faults so far (soft and hard: guest memory touched for the first time, memory paged back in)
static uint64_t os_page_faults(uint64_t *major) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc = {.cb = sizeof pmc};
    *major = 0;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PageFaultCount : 0;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) { *major = 0; return 0; }
    *major = (uint64_t)ru.ru_majflt;
    return (uint64_t)(ru.ru_minflt + ru.ru_majflt);
#endif
}

// ---- `frametrace`: per presented frame, its time and what the counters of perfcount.h did in it (hitch attribution:
// tools/perf_routes.py). Off until `frametrace start`; then one snapshot of the counters and one page-fault count a frame.
// Also the CPU time of the game's main thread, the render thread and the recorder in each frame (a spike of one of them
// with no stall: game logic, translation or recording).
#define FT_COLS (PC_COUNT + 5)  // counters, page faults, hard page faults, main / render / recorder CPU (us)
typedef struct { uint64_t end, ns, d[FT_COLS]; } FtRow;
static FtRow *ft_rows;
static unsigned ft_n, ft_cap;
static bool ft_on;
static uint64_t ft_last[FT_COLS];
static pthread_t ft_th;  // (the main thread; render and recorder: ft_named)
static int ft_named[3];
static bool ft_has[3];
static double ft_tsc_per_ms = 1;
static double thread_cpu_ms(pthread_t th, double tsc_per_ms);
static double named_cpu_ms(int i, double tsc_per_ms);
static void ft_snapshot(uint64_t *v) {
    for (int i = 0; i < PC_COUNT; i++) v[i] = atomic_load_explicit(&g_perfcount[i], memory_order_relaxed);
    v[PC_COUNT] = os_page_faults(&v[PC_COUNT + 1]);
    for (int i = 0; i < 3; i++) {
        double ms = !ft_has[i] ? -1 : i ? named_cpu_ms(ft_named[i], ft_tsc_per_ms) : thread_cpu_ms(ft_th, ft_tsc_per_ms);
        v[PC_COUNT + 2 + i] = ms > 0 ? (uint64_t)(ms * 1000) : 0;
    }
}
static uint64_t ft_cost_ns, ft_cost_max_ns;  // (the trace's own time on the presenting thread)
static void ft_frame(uint64_t now, uint64_t ns) {
    uint64_t v[FT_COLS];
    ft_snapshot(v);
    uint64_t cost = plat_now_ns() - now;
    ft_cost_ns += cost;
    if (cost > ft_cost_max_ns) ft_cost_max_ns = cost;
    if (ft_n < ft_cap) {
        FtRow *r = &ft_rows[ft_n++];
        r->end = now;
        r->ns = ns;
        for (int i = 0; i < FT_COLS; i++) r->d[i] = v[i] - ft_last[i];
    }
    memcpy(ft_last, v, sizeof v);
}

// Integration: declare this and call control_prof_frame(frame) at the start of control_game_frame.
// Both this hook and the commands must run on the presenting thread.
void control_prof_frame(uint64_t frame) {
    uint64_t now = plat_now_ns();
    if (hooked && frame == last_frame) return;
    if (ft_on && hooked && frame > last_frame && now > last_ns) ft_frame(now, now - last_ns);
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
// ---- `threads`: CPU time used so far by each guest thread and by the named host threads (render: the NVN queue thread
// that translates and records the command lists; recorder: the Vulkan recorder). Two calls a few seconds apart give
// each thread's busy time per frame (tools/perf_routes.py), which tells the game thread from the render thread as the
// limit. Windows: QueryThreadCycleTime (TSC ticks, turned into ms with the TSC rate measured since startup).
#define PROF_THREADS 8
static struct { const char *name; pthread_t th; void *h; } named[PROF_THREADS];  // (h: Windows, a handle of its own)
static _Atomic int nnamed;
static uint64_t tsc0, ns0;
void control_prof_thread(const char *name) {  // (any thread, once, at its start)
    int i = atomic_fetch_add(&nnamed, 1);
    if (i >= PROF_THREADS) return;
    named[i].th = pthread_self();
#ifdef _WIN32  // (a detached thread's pthread handle is closed: the recorder's was, and its CPU time read as -1)
    HANDLE h = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        named[i].h = h;
#endif
    named[i].name = name;
}
static double thread_cpu_ms(pthread_t th, double tsc_per_ms) {
#ifdef _WIN32
    ULONG64 cycles;
    HANDLE h = (HANDLE)pthread_gethandle(th);
    if (!h || !QueryThreadCycleTime(h, &cycles)) return -1;
    return (double)cycles / tsc_per_ms;
#else
    (void)tsc_per_ms;
    clockid_t id;
    struct timespec ts;
    if (pthread_getcpuclockid(th, &id) || clock_gettime(id, &ts)) return -1;
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}
static double named_cpu_ms(int i, double tsc_per_ms) {
#ifdef _WIN32
    ULONG64 cycles;
    if (named[i].h) return QueryThreadCycleTime((HANDLE)named[i].h, &cycles) ? (double)cycles / tsc_per_ms : -1;
#endif
    return thread_cpu_ms(named[i].th, tsc_per_ms);
}
static void cmd_threads(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: threads"); return; }
    uint64_t ns = plat_now_ns();
    double tsc_per_ms = 1;
#ifdef _WIN32
    uint64_t tsc = __rdtsc();
    if (ns <= ns0 + 100000000) { control_fail(r, "threads: TSC rate not measured yet; retry in a second"); return; }
    tsc_per_ms = (double)(tsc - tsc0) / ((double)(ns - ns0) / 1e6);
#endif
    extern uint64_t renderer_present_count(void);
    control_field_num(r, "now_ms", ns / 1e6);
    control_field_int(r, "presents", (int64_t)renderer_present_count());
    char key[64];
    int n = atomic_load(&nnamed);
    for (int i = 0; i < n && i < PROF_THREADS; i++) {
        snprintf(key, sizeof key, "host.%s", named[i].name);
        control_field_num(r, key, named_cpu_ms(i, tsc_per_ms));
    }
    pthread_t host[128];
    uint32_t handle[128];
    uint64_t entry[128];
    int m = kernel_running_threads(host, handle, entry, 128);
    for (int i = 0; i < m; i++) {
        if (handle[i] >= 0xf000) continue;  // (host threads the sampler also takes: the named ones above)
        snprintf(key, sizeof key, "guest.%x_%llx", handle[i], (unsigned long long)entry[i]);
        control_field_num(r, key, thread_cpu_ms(host[i], tsc_per_ms));
    }
}

// SAVAGEWIND_GPUPROF_AT's capture of one frame, asked for now (vulkan/device.c takes it on the next present):
// "[gpuprof]" lines in the log
volatile int g_gpuprof_request;

static void cmd_vkprof(int argc, char **argv, ControlReply *r) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "0") && strcmp(argv[1], "1") && strcmp(argv[1], "2"))) {
        control_fail(r, "Usage: vkprof [0|1|2]"); return;
    }
    control_fail(r, "Set at startup with SAVAGEWIND_VK_PROF (1 detailed, 2 coarse)");
}
static void cmd_gpuprof(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: gpuprof"); return; }
    extern uint64_t renderer_present_count(void);
    g_gpuprof_request = 1;
    control_field_str(r, "status", "requested: GPU time per pass of the next frame, as [gpuprof] lines in the log");
    control_field_int(r, "presents", (int64_t)renderer_present_count());
}
static void cmd_frametrace(int argc, char **argv, ControlReply *r) {
    if (argc == 2 && !strcmp(argv[1], "start")) {
        if (!ft_rows) {
            ft_cap = FRAME_CAP;
            ft_rows = malloc(sizeof *ft_rows * ft_cap);
            if (!ft_rows) { ft_cap = 0; control_fail(r, "frametrace: out of memory"); return; }
        }
        ft_n = 0;
        ft_cost_ns = ft_cost_max_ns = 0;
        ft_has[0] = ft_has[1] = ft_has[2] = false;
        pthread_t host[128];
        uint32_t handle[128];
        uint64_t entry[128];
        int m = kernel_running_threads(host, handle, entry, 128);
        for (int i = 0; i < m; i++)
            if (handle[i] == 0x101) ft_th = host[i], ft_has[0] = true;  // (the game's first thread)
        for (int i = 0; i < atomic_load(&nnamed) && i < PROF_THREADS; i++) {
            int k = !strcmp(named[i].name, "render") ? 1 : !strcmp(named[i].name, "recorder") ? 2 : -1;
            if (k > 0) ft_named[k] = i, ft_has[k] = true;
        }
#ifdef _WIN32
        uint64_t ns = plat_now_ns();
        if (ns > ns0 + 100000000) ft_tsc_per_ms = (double)(__rdtsc() - tsc0) / ((double)(ns - ns0) / 1e6);
#endif
        ft_snapshot(ft_last);
        ft_on = true;
        control_field_str(r, "status", "tracing");
        control_field_int(r, "capacity", ft_cap);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "stop")) {
        ft_on = false;
        control_field_int(r, "frames", ft_n);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "dump")) {
        ft_on = false;
        FILE *f = fopen(argv[2], "w");
        if (!f) { control_fail(r, "frametrace: cannot write %s: %s", argv[2], strerror(errno)); return; }
        fprintf(f, "end_ms,frame_ms,builds,build_ms,build_waits,build_wait_ms,upload_mb,tex_mb,astc_ms,astc_mb,gpu_wait_ms,"
                   "file_reads,file_mb,file_ms,valloc_ms,present_ms,faults,hard_faults,main_ms,render_ms,recorder_ms\n");
        for (unsigned i = 0; i < ft_n; i++) {
            const FtRow *w = &ft_rows[i];
            const uint64_t *d = w->d;
            fprintf(f, "%.3f,%.3f,%llu,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%.3f,%.3f,%.3f\n",
                    w->end / 1e6,
                    w->ns / 1e6, (unsigned long long)d[PC_BUILDS], d[PC_BUILD_NS] / 1e6, (unsigned long long)d[PC_BUILD_WAITS],
                    d[PC_BUILD_WAIT_NS] / 1e6, d[PC_UPLOAD_BYTES] / 1048576.0, d[PC_TEX_BYTES] / 1048576.0, d[PC_ASTC_NS] / 1e6,
                    d[PC_ASTC_BYTES] / 1048576.0, d[PC_GPU_WAIT_NS] / 1e6, (unsigned long long)d[PC_FILE_READS],
                    d[PC_FILE_BYTES] / 1048576.0, d[PC_FILE_NS] / 1e6, d[PC_VALLOC_NS] / 1e6, d[PC_PRESENT_NS] / 1e6,
                    (unsigned long long)d[PC_COUNT], (unsigned long long)d[PC_COUNT + 1], d[PC_COUNT + 2] / 1e3,
                    d[PC_COUNT + 3] / 1e3, d[PC_COUNT + 4] / 1e3);
        }
        bool ok = fclose(f) == 0;
        if (!ok) { control_fail(r, "frametrace: writing %s failed", argv[2]); return; }
        control_field_int(r, "frames", ft_n);
        control_field_num(r, "cost_us_avg", ft_n ? ft_cost_ns / 1e3 / ft_n : 0);
        control_field_num(r, "cost_us_max", ft_cost_max_ns / 1e3);
        control_field_bool(r, "capacity_limited", ft_n == ft_cap);
        control_field_str(r, "path", argv[2]);
        return;
    }
    control_fail(r, "Usage: frametrace start | stop | dump <path>");
}

// ---- `memstats`: process memory, GPU memory heaps (VK_EXT_memory_budget: the whole process's use of each heap) and
// texture memory by kind
static void cmd_memstats(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: memstats"); return; }
    const double MB = 1048576.0;
    uint64_t t0 = plat_now_ns();
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc = {.cb = sizeof pmc};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
        control_field_num(r, "rss_mb", pmc.WorkingSetSize / MB);
        control_field_num(r, "rss_peak_mb", pmc.PeakWorkingSetSize / MB);
        control_field_num(r, "private_mb", pmc.PrivateUsage / MB);
    }
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        control_field_num(r, "sys_total_mb", ms.ullTotalPhys / MB);
        control_field_num(r, "sys_used_mb", (ms.ullTotalPhys - ms.ullAvailPhys) / MB);
    }
#else
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    static const char *const keys[][2] = {{"VmRSS:", "rss_mb"}, {"VmHWM:", "rss_peak_mb"}, {"RssAnon:", "private_mb"},
                                          {"RssFile:", "rss_file_mb"}, {"RssShmem:", "rss_shmem_mb"}, {"VmSwap:", "swap_mb"}};
    while (f && fgets(line, sizeof line, f))
        for (size_t k = 0; k < sizeof keys / sizeof *keys; k++)
            if (!strncmp(line, keys[k][0], strlen(keys[k][0]))) control_field_num(r, keys[k][1], atof(line + strlen(keys[k][0])) / 1024.0);
    if (f) fclose(f);
    f = fopen("/proc/meminfo", "r");
    double total = -1, avail = -1;
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemTotal:", 9)) total = atof(line + 9) / 1024.0;
        if (!strncmp(line, "MemAvailable:", 13)) avail = atof(line + 13) / 1024.0;
    }
    if (f) fclose(f);
    if (total > 0 && avail >= 0) control_field_num(r, "sys_total_mb", total), control_field_num(r, "sys_used_mb", total - avail);
#endif
    uint64_t major, faults = os_page_faults(&major);
    control_field_int(r, "page_faults", (int64_t)faults);
    if (g_gpu_heaps) {
        uint64_t use[16], bud[16], size[16];
        uint32_t local[16];
        int n = g_gpu_heaps(use, bud, size, local, 16);
        double dl = 0, host = 0;
        char key[48];
        for (int i = 0; i < n; i++) {
            snprintf(key, sizeof key, "heap%d_%s_used_mb", i, local[i] ? "local" : "host");
            control_field_num(r, key, use[i] / MB);
            snprintf(key, sizeof key, "heap%d_budget_mb", i);
            control_field_num(r, key, bud[i] / MB);
            snprintf(key, sizeof key, "heap%d_size_mb", i);
            control_field_num(r, key, size[i] / MB);
            if (local[i]) dl += use[i] / MB; else host += use[i] / MB;
        }
        control_field_num(r, "gpu_local_mb", dl);
        control_field_num(r, "gpu_host_mb", host);
    }
    static const char *const kinds[TM_COUNT] = {"astc_native", "astc_rgba8", "bc", "rt", "other", "astc_src"};
    char key[48];
    for (int k = 0; k < TM_COUNT; k++) {
        snprintf(key, sizeof key, "tex_%s_mb", kinds[k]);
        control_field_num(r, key, atomic_load(&g_texmem[k]) / MB);
        snprintf(key, sizeof key, "tex_%s_n", kinds[k]);
        control_field_int(r, key, atomic_load(&g_texmem_n[k]));
    }
    control_field_num(r, "astc_decode_ms", atomic_load(&g_perfcount[PC_ASTC_NS]) / 1e6);
    control_field_num(r, "astc_decoded_mb", atomic_load(&g_perfcount[PC_ASTC_BYTES]) / MB);
    control_field_num(r, "self_us", (plat_now_ns() - t0) / 1e3);  // (this command's own time on the presenting thread)
}

void control_prof_init(void) {
    static const ControlCmd cmds[] = {
        {"frametrace", "start | stop | dump <path>", "Per-frame counters (builds, uploads, ASTC, waits, file reads, faults) to a CSV", cmd_frametrace},
        {"memstats", "", "Process RAM, GPU memory heaps and texture memory by kind", cmd_memstats},
        {"frames", "[seconds]", "Frame times over the last seconds (default 5, maximum 60)", cmd_frames},
        {"sample", "start <seconds> [interval_us] | status", "Sample guest stacks to build/samples.txt; show status", cmd_sample},
        {"vkprof", "[0|1|2]", "Vulkan timers (startup setting)", cmd_vkprof},
        {"gpuprof", "", "GPU time per pass of the next frame, to the log", cmd_gpuprof},
        {"threads", "", "CPU time so far of each game thread and the render threads (ms)", cmd_threads},
    };
    ns0 = plat_now_ns();
#ifdef _WIN32
    tsc0 = __rdtsc();
#endif
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
