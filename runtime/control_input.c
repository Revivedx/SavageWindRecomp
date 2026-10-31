// Live test input. Commands and frame publication have one writer (the presenter).
#include "control.h"
#include "control_cells.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

uint64_t renderer_present_count(void);
static const char *const names[] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr",
    "plus", "minus", "left", "up", "right", "down"
};
static uint64_t until[16];
static bool held[16];
static struct { int32_t x, y; uint64_t until; bool held; } sticks[2];
static _Atomic bool enabled;
static _Atomic uint64_t buttons, axes[2];
static uint64_t published_frame;
enum { LOOK_IDLE, LOOK_RUN, LOOK_DONE, LOOK_TIMEOUT, LOOK_FAILED };
static const char *const look_names[] = {"idle", "running", "done", "timeout", "failed"};
static struct { int state; bool use[2]; double target[2], last[2]; bool have_last, snap; uint64_t start, ok, settle; } look;

static bool duration(const char *s, uint64_t *n, ControlReply *r) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (*s < '0' || *s > '9' || *end || errno || !v || v > 1000000) {
        control_fail(r, "Frames must be an integer from 1 to 1000000"); return false;
    }
    *n = v; return true;
}
static int button(const char *s, ControlReply *r) {
    for (int i = 0; i < 16; i++) if (!strcmp(s, names[i])) return i;
    control_fail(r, "Invalid button; valid: a b x y l r zl zr plus minus up down left right lstick rstick");
    return -1;
}
static void queued(ControlReply *r, const char *what) {
    control_field_str(r, "queued", what);
    control_field_int(r, "frame", (int64_t)renderer_present_count());
    control_field_int(r, "apply_frame", (int64_t)(renderer_present_count() + 1));
}
static void cmd_button(int argc, char **argv, ControlReply *r) {
    bool hold = !strcmp(argv[0], "hold");
    if (argc < 2 || argc > (hold ? 2 : 3)) {
        control_fail(r, "Usage: %s", hold ? "hold <button>" : "press <button> [frames]"); return;
    }
    int i = button(argv[1], r);
    uint64_t n = 3;
    if (i < 0 || (argc == 3 && !duration(argv[2], &n, r))) return;
    held[i] = hold;
    until[i] = hold ? 0 : renderer_present_count() + 1 + n;
    queued(r, argv[0]);
    control_field_str(r, "button", names[i]);
    if (!hold) control_field_int(r, "frames", (int64_t)n);
}
static void cmd_release(int argc, char **argv, ControlReply *r) {
    if (argc > 2) { control_fail(r, "Usage: release [<button> | all]"); return; }
    if (argc == 1 || !strcmp(argv[1], "all")) {
        memset(held, 0, sizeof held); memset(until, 0, sizeof until);
        memset(sticks, 0, sizeof sticks);
        if (look.state == LOOK_RUN) look.state = LOOK_IDLE;
    } else {
        int i = button(argv[1], r);
        if (i < 0) return;
        held[i] = false; until[i] = 0;
    }
    queued(r, "release");
    control_field_str(r, "button", argc == 1 ? "all" : argv[1]);
}
static void cmd_stick(int argc, char **argv, ControlReply *r) {
    if ((argc != 4 && argc != 5) || (strcmp(argv[1], "l") && strcmp(argv[1], "r"))) {
        control_fail(r, "Usage: stick <l|r> <x> <y> [frames]"); return;
    }
    double v[2];
    for (int j = 0; j < 2; j++) {
        char *end; errno = 0; v[j] = strtod(argv[j + 2], &end);
        if (end == argv[j + 2] || *end || errno || !(v[j] >= -1 && v[j] <= 1)) {
            control_fail(r, "Axes must be from -1 to 1"); return;
        }
    }
    uint64_t n = 0;
    if (argc == 5 && !duration(argv[4], &n, r)) return;
    int i = !strcmp(argv[1], "r");
    if (i && look.state == LOOK_RUN) look.state = LOOK_IDLE;
    sticks[i].x = (int32_t)(v[0] * 32767); sticks[i].y = (int32_t)(v[1] * 32767);
    sticks[i].held = argc == 4 && (v[0] != 0 || v[1] != 0);
    sticks[i].until = n ? renderer_present_count() + 1 + n : 0;
    queued(r, "stick"); control_field_str(r, "stick", argv[1]);
    control_field_num(r, "x", v[0]); control_field_num(r, "y", v[1]);
    control_field_int(r, "frames", (int64_t)n);
}


// ---- look <yaw> <pitch>: turn the camera to these angles. The orbit camera keeps its eye position and its pitch as real
// state (camera.eye, camera.target, camera.pitch cells, control_cells.c): yaw is the direction eye -> target, so a write
// that moves the eye around the target turns the camera and it stays there. look reads the angles from those cells, writes
// the eye / pitch and checks again each frame, since the game moves the eye a few degrees after a write (wall
// collision, a follow correction); it settles in a few frames. When the cells do not resolve (another game version, no
// camera object yet) it falls back to moving the right stick until the camera (the `camera` command's matrix, needs
// Anti-aliasing TAA) points there: stick right lowers yaw, stick up raises pitch, proportional on the error, damped by how
// fast the camera is already turning (the game answers a frame or two late), with a floor so the stick leaves its dead zone.
bool control_camera_angles(double *yaw, double *pitch);  // runtime/control_game.c
void control_fly_input(uint64_t *b, int32_t *lx, int32_t *ly);  // runtime/control_fly.c
void control_fly_frame(uint64_t frame);
#define LOOK_TOL 0.6          // degrees
#define LOOK_FRAMES 900       // give up after this many presented frames
#define LOOK_SETTLE 6         // frames within tolerance in a row before "done"
static double wrap180(double a) { while (a > 180) a -= 360; while (a <= -180) a += 360; return a; }
#define LOOK_TOL_SNAP 0.15    // degrees, when the cells are used
#define LOOK_SETTLE_SNAP 3
static bool look_angles(double *yaw, double *pitch) {
    double e[3], t[3];
    return control_cell_camera_angles(e, t, yaw, pitch) || control_camera_angles(yaw, pitch);
}
static void look_end(int state) {
    look.state = state;
    sticks[1].x = sticks[1].y = 0; sticks[1].held = false; sticks[1].until = 0;
}
static void look_snap_step(uint64_t frame) {
    double e[3], t[3], cur[2];
    if (!control_cell_camera_angles(e, t, &cur[0], &cur[1])) { look_end(LOOK_FAILED); return; }
    if (frame - look.start > LOOK_FRAMES) { look_end(LOOK_TIMEOUT); return; }
    double err0 = look.use[0] ? wrap180(look.target[0] - cur[0]) : 0, err1 = look.use[1] ? look.target[1] - cur[1] : 0;
    if (fabs(err0) <= LOOK_TOL_SNAP && fabs(err1) <= LOOK_TOL_SNAP) {
        if (++look.ok >= (look.settle ? look.settle : LOOK_SETTLE_SNAP)) look_end(LOOK_DONE);
        return;
    }
    look.ok = 0;
    if (fabs(err0) > LOOK_TOL_SNAP) {   // put the eye on the circle around the target, aimed at the yaw we want
        double r = hypot(t[0] - e[0], t[2] - e[2]);
        if (r < 0.01) { look_end(LOOK_FAILED); return; }
        double y = (cur[0] + err0) * (M_PI / 180.0), ne[3] = {t[0] - sin(y) * r, e[1], t[2] - cos(y) * r};
        if (!control_cell_set(&control_cell_camera_eye, ne)) { look_end(LOOK_FAILED); return; }
    }
    if (fabs(err1) > LOOK_TOL_SNAP) {
        double np = -look.target[1];
        if (!control_cell_set(&control_cell_camera_pitch, &np)) { look_end(LOOK_FAILED); return; }
    }
}
static void look_step(uint64_t frame) {
    if (look.snap) { look_snap_step(frame); return; }
    double cur[2];
    if (!control_camera_angles(&cur[0], &cur[1])) { look_end(LOOK_FAILED); return; }
    if (frame - look.start > LOOK_FRAMES) { look_end(LOOK_TIMEOUT); return; }
    double err[2], vel[2] = {0, 0}, u[2];
    bool inside = true;
    for (int a = 0; a < 2; a++) {
        err[a] = look.use[a] ? (a == 0 ? wrap180(look.target[a] - cur[a]) : look.target[a] - cur[a]) : 0;
        if (look.have_last) vel[a] = a == 0 ? wrap180(cur[a] - look.last[a]) : cur[a] - look.last[a];
        look.last[a] = cur[a];
        if (fabs(err[a]) > LOOK_TOL) inside = false;
        static const double kp[2] = {0.05, 0.15}, floor_[2] = {0.2, 0.3};  // (pitch answers more slowly than yaw)
        double o = kp[a] * err[a] - 0.5 * vel[a];
        if (fabs(err[a]) > LOOK_TOL && fabs(o) < floor_[a]) o = err[a] < 0 ? -floor_[a] : floor_[a];
        if (fabs(err[a]) <= LOOK_TOL) o = 0;
        u[a] = o > 1 ? 1 : o < -1 ? -1 : o;
    }
    look.have_last = true;
    look.ok = inside ? look.ok + 1 : 0;
    if (look.ok >= LOOK_SETTLE) { look_end(LOOK_DONE); return; }
    sticks[1].x = (int32_t)(-u[0] * 32767);   // right lowers yaw
    sticks[1].y = (int32_t)(u[1] * 32767);    // up raises pitch
    sticks[1].held = true; sticks[1].until = 0;
}
static bool angle_arg(const char *s, int a, ControlReply *r) {
    if (!strcmp(s, "keep")) { look.use[a] = false; return true; }
    char *end; double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v) || fabs(v) > 1000) { control_fail(r, "Angles must be degrees or keep"); return false; }
    look.use[a] = true; look.target[a] = v;
    return true;
}
static void look_fields(ControlReply *r) {
    control_field_str(r, "state", look_names[look.state]);
    control_field_str(r, "mode", look.snap ? "cells" : "stick");
    double y, p;
    if (look_angles(&y, &p)) {
        control_field_num(r, "yaw", y); control_field_num(r, "pitch", p);
        if (look.use[0]) control_field_num(r, "yaw_error", wrap180(look.target[0] - y));
        if (look.use[1]) control_field_num(r, "pitch_error", look.target[1] - p);
    }
}
static void cmd_look(int argc, char **argv, ControlReply *r) {
    if (argc == 2 && !strcmp(argv[1], "status")) { look_fields(r); return; }
    if (argc == 2 && !strcmp(argv[1], "stop")) { if (look.state == LOOK_RUN) look_end(LOOK_IDLE); look_fields(r); return; }
    if (argc != 3) { control_fail(r, "Usage: look <yaw> <pitch> | status | stop"); return; }
    double y, p, e[3], t[3];
    bool snap = control_cell_camera_angles(e, t, &y, &p);   // the camera cells; else the stick loop, which reads the TAA matrix
    if (!snap && !control_camera_angles(&y, &p)) { control_fail(r, "No camera"); return; }
    if (!angle_arg(argv[1], 0, r) || !angle_arg(argv[2], 1, r)) return;
    if (look.use[1] && (look.target[1] > 89 || look.target[1] < -89)) { control_fail(r, "Pitch must be from -89 to 89"); return; }
    look.snap = snap; look.settle = 0;
    look.state = LOOK_RUN; look.have_last = false; look.ok = 0; look.start = renderer_present_count();
    queued(r, "look");
    look_fields(r);
}

// Start a look from inside the runtime (goto, after its teleport): the same snap as the command, cells only (the stick loop
// is too slow and fragile to run unattended), and `settle` frames in tolerance in a row before it counts as done: the game
// may still turn the camera for a moment after a teleport, and the loop keeps correcting it until it holds.
bool control_look_start(bool use_yaw, double yaw, bool use_pitch, double pitch, int settle) {
    double y, p, e[3], t[3];
    if (!control_cell_camera_angles(e, t, &y, &p) || !isfinite(yaw) || !isfinite(pitch)) return false;
    if (pitch > 89) pitch = 89;
    if (pitch < -89) pitch = -89;
    look.use[0] = use_yaw; look.target[0] = yaw;
    look.use[1] = use_pitch; look.target[1] = pitch;
    look.snap = true; look.settle = (uint64_t)settle;
    look.state = LOOK_RUN; look.have_last = false; look.ok = 0; look.start = renderer_present_count();
    return true;
}

// Called even when no script exists. Expiry uses presented frames, never script time or poll count.
void control_input_frame(uint64_t frame) {
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    if (look.state == LOOK_RUN) look_step(frame);
    control_fly_frame(frame);
    uint64_t b = 0;
    for (int i = 0; i < 16; i++) if (held[i] || frame < until[i]) b |= 1ull << i;
    atomic_store(&buttons, b);
    for (int i = 0; i < 2; i++) {
        bool active = sticks[i].held || frame < sticks[i].until;
        uint64_t a = active ? (uint16_t)sticks[i].x | ((uint64_t)(uint16_t)sticks[i].y << 16) |
                     ((uint64_t)sticks[i].held << 32) : 0;
        atomic_store(&axes[i], a);
    }
    published_frame = frame;
}
static int32_t clamp(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
void control_input_apply(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    *b |= atomic_load(&buttons);
    if (!lx) { control_fly_input(b, NULL, NULL); return; }
    int32_t *x[] = {lx, rx}, *y[] = {ly, ry};
    for (int i = 0; i < 2; i++) {
        uint64_t a = atomic_load(&axes[i]);
        int32_t ax = (int16_t)a, ay = (int16_t)(a >> 16);
        *x[i] = clamp((a >> 32 ? 0 : *x[i]) + ax);
        *y[i] = clamp((a >> 32 ? 0 : *y[i]) + ay);
    }
    control_fly_input(b, lx, ly);
}
static void cmd_input(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: input"); return; }
    control_field_int(r, "frame", (int64_t)published_frame);
    uint64_t b = atomic_load(&buttons);
    for (int i = 0; i < 16; i++) control_field_bool(r, names[i], (b >> i) & 1);
    const char *keys[][3] = {{"l.x", "l.y", "l.held"}, {"r.x", "r.y", "r.held"}};
    for (int i = 0; i < 2; i++) {
        uint64_t a = atomic_load(&axes[i]);
        control_field_num(r, keys[i][0], (int16_t)a / 32767.0);
        control_field_num(r, keys[i][1], (int16_t)(a >> 16) / 32767.0);
        control_field_bool(r, keys[i][2], a >> 32);
    }
}
void control_input_init(void) {
    static const ControlCmd cmds[] = {
        {"press", "<button> [frames]", "Press a button (3 frames by default)", cmd_button},
        {"hold", "<button>", "Hold a button until released", cmd_button},
        {"release", "[<button>|all]", "Release a button, or all input", cmd_release},
        {"stick", "<l|r> <x> <y> [frames]", "Move a stick (axes -1 to 1)", cmd_stick},
        {"look", "<yaw> <pitch> | status | stop", "Turn the camera to a yaw and pitch in degrees (either may be keep)", cmd_look},
        {"input", "", "Show the input currently applied", cmd_input},
    };
    control_register(cmds, sizeof cmds / sizeof *cmds);
    atomic_store(&enabled, true);
}
