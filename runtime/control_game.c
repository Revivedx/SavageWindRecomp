// The game's commands of the control endpoint (runtime/control.h, docs/CONTROL.md): the player safe ones (state, pos,
// camera, version, report) and the developer ones (time, shot, goto). Add a command here by writing a handler and putting it in the table at the bottom.
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "control.h"
#include "control_cells.h"
#include "perfmon.h"
#include "rt.h"
#include "game_sites.h"

uint64_t renderer_present_count(void);  // presented frames (the backend)
bool control_camera_view(float view[12]);  // the world camera's view matrix (the backend; false: none)
bool control_shot_now(const char *path);  // the image of the frame being presented -> PNG (the backend; false: can't)
void test_warp_set_hour(float hour);  // runtime/services/test_warp.c
void test_warp_hold_hour(float hour);
float test_warp_hour(bool *held);

// ---- fps over the last second, kept every frame
#define FPS_WINDOW_NS 1000000000ull
static uint64_t win_start_ns, win_start_frame;
static double fps_now = -1;
static uint64_t boot_ns;

void control_prof_frame(uint64_t frame);  // runtime/control_prof.c
void control_tp_frame(uint64_t frame);    // runtime/control_tp.c

void control_game_frame(uint64_t frame) {
    if (control_enabled()) {  // (the profiler and teleport are developer mode only)
        control_prof_frame(frame);
        control_tp_frame(frame);
    }
    uint64_t now = plat_now_ns();
    if (!boot_ns) boot_ns = now;
    if (!win_start_ns) { win_start_ns = now; win_start_frame = frame; return; }
    if (now - win_start_ns >= FPS_WINDOW_NS) {
        fps_now = (double)(frame - win_start_frame) * 1e9 / (double)(now - win_start_ns);
        win_start_ns = now;
        win_start_frame = frame;
    }
}

// ---- state: frame number, uptime, fps over the last second, hour of day
static void cmd_state(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    control_field_int(r, "frame", (int64_t)renderer_present_count());
    control_field_num(r, "uptime_s", boot_ns ? (double)(plat_now_ns() - boot_ns) / 1e9 : 0);
    if (fps_now >= 0) control_field_num(r, "fps", fps_now);
    bool held = false;
    float hour = test_warp_hour(&held);
    if (hour >= 0) control_field_num(r, "hour", hour);  // (left out before the world exists)
    control_field_bool(r, "hour_held", held);
}

// ---- time [freeze | unfreeze | set <0-24> | <0-24>]: with no argument just the hour; a bare number is the same as set
static void cmd_time(int argc, char **argv, ControlReply *r) {
    static const char *usage = "Usage: time [freeze | unfreeze | set <0-24> | <0-24>]";
    bool set = argc == 3 && !strcmp(argv[1], "set");
    if (argc > 3 || (argc == 3 && !set)) { control_fail(r, "%s", usage); return; }
    if (argc == 2 && (!strcmp(argv[1], "unfreeze") || !strcmp(argv[1], "free"))) {
        test_warp_hold_hour(-1);
    } else if (argc == 2 && !strcmp(argv[1], "freeze")) {
        float hour = test_warp_hour(NULL);
        if (hour < 0) { control_fail(r, "No world loaded"); return; }
        test_warp_hold_hour(hour);
    } else if (argc >= 2) {
        const char *arg = argv[argc - 1];  // time <hour> or time set <hour>
        char *end;
        errno = 0;
        double h = strtod(arg, &end);
        if (set && !*arg) { control_fail(r, "%s", usage); return; }
        if (end == arg || *end || errno || !(h >= 0 && h <= 24)) {  // (also rejects nan)
            control_fail(r, "%s", usage);
            return;
        }
        test_warp_set_hour((float)h);
    }
    bool held = false;
    float hour = test_warp_hour(&held);
    if (hour >= 0) control_field_num(r, "hour", hour);
    control_field_bool(r, "hour_held", held);
}

// ---- pos: Link's position (x y z) and stamina (uking::act::PlayerInfo +0x88 / +0xa4, as the clock probe in framepace.c reads them)
static uint64_t player_info(void) { return g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0; }

bool control_camera_angles(double *yaw, double *pitch);

// The line `pos` puts on the clipboard and `goto` reads back, so a place travels through an issue:
//   swr:1 x=-1234.56 y=250.00 z=789.01 yaw=90.0 pitch=-10.0 hour=18.50
// (yaw and pitch work in any AA mode; the hour needs the world). x y z are Link's position.
static void place_string(char *buf, size_t n, const float p[3]) {
    int len = snprintf(buf, n, "swr:1 x=%.2f y=%.2f z=%.2f", p[0], p[1], p[2]);
    double yaw, pitch;
    if (len > 0 && (size_t)len < n && control_camera_angles(&yaw, &pitch)) len += snprintf(buf + len, n - (size_t)len, " yaw=%.1f pitch=%.1f", yaw, pitch);
    float hour = test_warp_hour(NULL);
    if (len > 0 && (size_t)len < n && hour >= 0) snprintf(buf + len, n - (size_t)len, " hour=%.2f", hour);
}

// Link's position, false before there is a Link in the world: no object yet, or one that still reads 0 0 0 (the title
// screen has the object but not a place; nobody stands at exactly 0 0 0).
static bool link_position(float p[3]) {
    uint64_t pinfo = player_info();
    if (!pinfo) return false;
    p[0] = *(float *)(uintptr_t)(pinfo + 0x88); p[1] = *(float *)(uintptr_t)(pinfo + 0x8c); p[2] = *(float *)(uintptr_t)(pinfo + 0x90);
    return p[0] != 0 || p[1] != 0 || p[2] != 0;
}

// The place in savagewind.log: the `pos` line every PLACE_LOG_NS while Link moves (no line while he stands still: the
// last one still says where), so a report's log says where its bug happened and `goto` takes the line back.
// SAVAGEWIND_PLACE_LOG=0: no lines.
#define PLACE_LOG_NS 30000000000ull
void control_place_frame(void) {
    static int on = -1;
    static uint64_t last_ns;
    static float last[3];
    if (on < 0) on = !getenv("SAVAGEWIND_PLACE_LOG") || atoi(getenv("SAVAGEWIND_PLACE_LOG")) != 0;
    if (!on) return;
    uint64_t now = plat_now_ns();
    if (last_ns && now - last_ns < PLACE_LOG_NS) return;
    float p[3];
    if (!link_position(p)) return;
    last_ns = now;
    float dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
    if (dx * dx + dy * dy + dz * dz < 1.0f) return;
    memcpy(last, p, sizeof last);
    char place[160];
    place_string(place, sizeof place, p);
    LOG("[place] %s\n", place);
}

static void cmd_pos(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: pos"); return; }
    uint64_t pinfo = player_info();
    float p[3];
    if (!link_position(p)) { control_fail(r, "No world loaded"); return; }
    control_field_num(r, "x", p[0]);
    control_field_num(r, "y", p[1]);
    control_field_num(r, "z", p[2]);
    control_field_num(r, "stamina", *(float *)(uintptr_t)(pinfo + 0xa4));
    char a[32]; snprintf(a, sizeof a, "0x%llx", (unsigned long long)pinfo);
    control_field_str(r, "player_info", a);  // (the object, for finding pointer paths with mem read)
    char place[160];
    place_string(place, sizeof place, p);
    control_field_str(r, "place", place);
    control_text(r, "%s", place);
    control_copy(r, place);
}

// ---- camera: prefer the last finished frame's view matrix, then the game's stored orbit camera in any AA mode.
// view = [R | t]: a world point p is R*p + t in camera space; the rows of R are right, up and backwards in world space.
static bool camera_read(double eye[3], double fwd[3], double *yaw, double *pitch) {
    float v[12];
    if (!control_camera_view(v)) {
        double target[3];
        if (!control_cell_camera_angles(eye, target, yaw, pitch)) return false;
        double len = 0;
        for (int i = 0; i < 3; i++) { fwd[i] = target[i] - eye[i]; len += fwd[i] * fwd[i]; }
        len = sqrt(len);
        for (int i = 0; i < 3; i++) fwd[i] /= len;
        return true;
    }
    for (int i = 0; i < 3; i++) fwd[i] = -v[8 + i];
    for (int i = 0; i < 3; i++) eye[i] = -(v[0 + i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    const double deg = 57.29577951308232;
    *yaw = atan2(fwd[0], fwd[2]) * deg;
    *pitch = asin(fwd[1] < -1 ? -1 : fwd[1] > 1 ? 1 : fwd[1]) * deg;
    return true;
}

static void cmd_camera(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: camera"); return; }
    double eye[3], fwd[3], yaw, pitch;
    if (!camera_read(eye, fwd, &yaw, &pitch)) { control_fail(r, "No world loaded"); return; }
    control_field_num(r, "x", eye[0]); control_field_num(r, "y", eye[1]); control_field_num(r, "z", eye[2]);
    control_field_num(r, "dx", fwd[0]); control_field_num(r, "dy", fwd[1]); control_field_num(r, "dz", fwd[2]);
    control_field_num(r, "yaw", yaw);
    control_field_num(r, "pitch", pitch);
}

// yaw and pitch in degrees (yaw 0 looks along +z and 90 along +x, pitch positive looks up); false when neither source exists.
bool control_camera_angles(double *yaw, double *pitch) {
    double eye[3], fwd[3];
    return camera_read(eye, fwd, yaw, pitch);
}

// ---- shot <file>: a PNG of the frame being presented (written before the reply)
// Only a .png inside the game's folder: a relative path without "..", so a command can neither write nor delete a
// file anywhere else.
static bool shot_path_ok(const char *p) {
    size_t n = strlen(p);
    if (n < 5 || n > 260 || strcmp(p + n - 4, ".png") || p[0] == '/' || p[0] == '\\' || strchr(p, ':')) return false;
    for (const char *q = p; *q; q++) {
        if ((unsigned char)*q < 0x20) return false;
        bool start = q == p || q[-1] == '/' || q[-1] == '\\';
        if (start && q[0] == '.' && q[1] == '.' && (!q[2] || q[2] == '/' || q[2] == '\\')) return false;
    }
    return true;
}
static void cmd_shot(int argc, char **argv, ControlReply *r) {
    if (argc != 2) { control_fail(r, "Usage: shot <file>"); return; }
    if (!shot_path_ok(argv[1])) {
        control_fail(r, "The file must be a .png in the game's folder (a relative path without \"..\")");
        return;
    }
    remove(argv[1]);  // (so a failed save can't pass for a new file)
    if (!control_shot_now(argv[1])) { control_fail(r, "No image available"); return; }
    struct stat st;
    if (stat(argv[1], &st) || st.st_size <= 0) { control_fail(r, "Cannot write \"%s\"", argv[1]); return; }
    control_field_str(r, "path", argv[1]);
    control_field_int(r, "bytes", (int64_t)st.st_size);
    control_field_int(r, "frame", (int64_t)renderer_present_count());
}

// ---- version / report: what to paste into an issue
static const char *os_name(void) {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}
static const char *arch_name(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}
const char *rt_version(void);  // runtime/ui/launcher.mm

static void cmd_version(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: version"); return; }
    control_field_str(r, "version", rt_version());
    control_field_str(r, "os", os_name());
    control_field_str(r, "arch", arch_name());
    control_text(r, "Savage Wind %s (%s %s)", rt_version(), os_name(), arch_name());
}

// report: one block for an issue (also copied): version, machine, the place, how it runs, every setting.
static void cmd_report(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: report"); return; }
    char block[4096];
    size_t n = 0;
#define ADD(...) do { if (n < sizeof block) { int w = snprintf(block + n, sizeof block - n, __VA_ARGS__); if (w > 0) n += (size_t)w < sizeof block - n ? (size_t)w : sizeof block - n - 1; } } while (0)
    ADD("Savage Wind %s (%s %s)\n", rt_version(), os_name(), arch_name());
    PerfStats ps;
    perfmon_snapshot(&ps);
    if (ps.gpu_name[0] || ps.cpu_name[0]) ADD("GPU: %s | CPU: %s\n", ps.gpu_name[0] ? ps.gpu_name : "unknown", ps.cpu_name[0] ? ps.cpu_name : "unknown");
    if (ps.ram_total_mb > 0) ADD("RAM: %.0f of %.0f MB used", ps.ram_used_mb, ps.ram_total_mb);
    if (ps.vram_total_mb > 0) ADD(" | VRAM: %.0f of %.0f MB used", ps.vram_used_mb, ps.vram_total_mb);
    if (ps.ram_total_mb > 0 || ps.vram_total_mb > 0) ADD("\n");
    if (ps.fps > 0) ADD("Frame rate: %.1f fps (average %.1f ms, worst %.1f ms)\n", ps.fps, ps.frame_ms_avg, ps.frame_ms_max);
    float p[3];
    if (link_position(p)) {
        char place[160];
        place_string(place, sizeof place, p);
        ADD("Place: %s\n", place);
        control_field_str(r, "place", place);
    } else ADD("Place: not in the world yet\n");
    ControlConsoleResult g;
    control_exec_line("get", false, &g);
    if (!g.failed && g.text) {
        ADD("Settings:");
        for (const char *c = g.text; *c;) {
            const char *eol = strchr(c, '\n');
            size_t len = eol ? (size_t)(eol - c) : strlen(c);
            char item[128];
            if (len >= sizeof item) len = sizeof item - 1;
            memcpy(item, c, len); item[len] = 0;
            char *sep = strstr(item, ": ");
            if (sep) { sep[0] = '='; memmove(sep + 1, sep + 2, strlen(sep + 2) + 1); }
            ADD(" %s", item);
            c += len + (eol ? 1 : 0);
            if (!eol) break;
        }
        ADD("\n");
    }
    control_console_free(&g);
#undef ADD
    while (n && block[n - 1] == '\n') block[--n] = 0;
    control_field_str(r, "report", block);
    control_text(r, "%s", block);
    control_copy(r, block);
}

// ---- goto <x> <y> <z> | goto <swr:1 line>: back to a place `pos` or `report` gave (developer mode)
void control_tp_run(int argc, char **argv, ControlReply *r);  // runtime/control_tp.c: the teleport command
static bool parse_number(const char *s, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(v)) return false;
    *out = v;
    return true;
}
bool control_look_start(bool use_yaw, double yaw, bool use_pitch, double pitch, int settle);  // runtime/control_input.c
void control_tp_then_look(bool use_yaw, double yaw, bool use_pitch, double pitch);           // runtime/control_tp.c
static void cmd_goto(int argc, char **argv, ControlReply *r) {
    double v[3];
    double hour = -1, yaw = 0, pitch = 0;
    bool have_look[2] = {false, false};
    bool have[3] = {false, false, false};
    static const char *usage = "goto: use goto <x> <y> <z>, or paste a line from pos or report (swr:1 x=.. y=.. z=.. [hour=..])";
    int numbers = 0;
    for (int i = 1; i < argc; i++) {
        // A quoted line (swctl goto "swr:1 x=.. y=.. z=..") arrives as one argument, so split every argument at spaces.
        char word[256];
        if (snprintf(word, sizeof word, "%s", argv[i]) >= (int)sizeof word) { control_fail(r, "%s", usage); return; }
        for (char *a = strtok(word, " \t"); a; a = strtok(NULL, " \t")) {
            if (!strcmp(a, "swr:1")) continue;
            const char *eq = strchr(a, '=');
            if (eq) {
                double x;
                if (!parse_number(eq + 1, &x)) { control_fail(r, "Not a number: \"%s\"", a); return; }
                size_t k = (size_t)(eq - a);
                if (k == 1 && a[0] >= 'x' && a[0] <= 'z') { v[a[0] - 'x'] = x; have[a[0] - 'x'] = true; }
                else if (k == 4 && !strncmp(a, "hour", 4)) hour = x;
                else if (k == 3 && !strncmp(a, "yaw", 3)) { yaw = x; have_look[0] = true; }
                else if (k == 5 && !strncmp(a, "pitch", 5)) { pitch = x; have_look[1] = true; }
                // (anything else is ignored)
            } else if (numbers < 3 && parse_number(a, &v[numbers])) have[numbers++] = true;
            else { control_fail(r, "Cannot read \"%s\"", a); return; }
        }
    }
    if (!have[0] || !have[1] || !have[2]) { control_fail(r, "%s", usage); return; }
    if (hour > 24) { control_fail(r, "Hour must be from 0 to 24"); return; }
    uint64_t pinfo = player_info();
    if (!pinfo) { control_fail(r, "No world loaded"); return; }
    float cur[3] = {*(float *)(uintptr_t)(pinfo + 0x88), *(float *)(uintptr_t)(pinfo + 0x8c), *(float *)(uintptr_t)(pinfo + 0x90)};
    bool already = fabs(v[0] - cur[0]) < 0.05 && fabs(v[2] - cur[2]) < 0.05;  // (teleport refuses a move this small)
    if (!already) {
        char xs[40], ys[40], zs[40];
        snprintf(xs, sizeof xs, "%.4f", v[0]); snprintf(ys, sizeof ys, "%.4f", v[1]); snprintf(zs, sizeof zs, "%.4f", v[2]);
        char *tp[] = {(char *)"teleport", xs, ys, zs};
        control_tp_run(4, tp, r);  // (answers with the teleport's own fields: state, x, y, z, ...)
        if (control_reply_failed(r)) return;
    } else control_field_str(r, "state", "already_there");
    if (hour >= 0) test_warp_set_hour((float)hour);
    control_text(r, already ? "Already there" : "Teleporting to %.2f %.2f %.2f", v[0], v[1], v[2]);
    if (hour >= 0) control_text(r, "Time set to %.2f", hour);
    if (have_look[0] || have_look[1]) {   // the camera turns once Link is there (look status shows when it holds)
        if (already ? control_look_start(have_look[0], yaw, have_look[1], pitch, 30) : (control_tp_then_look(have_look[0], yaw, have_look[1], pitch), true))
            control_text(r, "Camera yaw %.1f pitch %.1f", yaw, pitch);
        else
            control_text(r, "Camera not found");
    }
}

void control_settings_init(void);  // runtime/control_settings.c
void control_prof_init(void);      // runtime/control_prof.c
void control_input_init(void);     // runtime/control_input.c
void control_mem_init(void);       // runtime/control_mem.c
void control_tp_init(void);        // runtime/control_tp.c
void control_cells_init(void);     // runtime/control_cells.c
void control_fly_init(void);       // runtime/control_fly.c

void control_game_init(void) {
    static const ControlCmd player[] = {
        {"state", "", "Frame number, uptime, fps and hour", cmd_state, CONTROL_PLAYER},
        {"pos", "", "Show the position and copy it as a line for goto or an issue", cmd_pos, CONTROL_PLAYER},
        {"camera", "", "Show the camera position and direction", cmd_camera, CONTROL_PLAYER},
        {"version", "", "Show the game version and system", cmd_version, CONTROL_PLAYER},
        {"report", "", "Build an issue report and copy it", cmd_report, CONTROL_PLAYER},
    };
    control_register(player, (int)(sizeof player / sizeof *player));
    control_settings_init();  // (get for everyone, set only in developer mode)
    if (!control_enabled()) return;
    static const ControlCmd dev[] = {
        {"time", "[freeze | unfreeze | set <0-24> | <0-24>]", "Show or change the hour of the day", cmd_time, 0},
        {"shot", "<file>", "Save a screenshot as PNG", cmd_shot, 0},
        {"goto", "<x> <y> <z> | <swr:1 line>", "Go to a place from pos or report", cmd_goto, 0},
    };
    control_register(dev, (int)(sizeof dev / sizeof *dev));
    control_prof_init();
    control_input_init();
    control_mem_init();
    control_cells_init();
    control_fly_init();
    control_tp_init();
}
