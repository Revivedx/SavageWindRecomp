// (test and development only) Fly mode for the control endpoint (docs/CONTROL.md): Link floats and goes where the
// controls point, through anything, so a spot anywhere on the map can be reached and looked at without walking.
//
// How: while it is on, the left stick, the up / down buttons (X and B: Space and Left Shift by default) and the boost
// (left stick press, Left Ctrl) are taken out of the game's input (control_fly_input, called where the game combines
// keyboard, gamepad and scripted input, so all three work) and, once per presented frame, turned into a step that is
// written to Link's real position (the player.pos cell, control_cells.c). The step follows the camera, read from the
// renderer's view matrix, or the stored orbit camera when unavailable, like `camera`: forward / back along where it looks (mode look) or along the
// ground (mode level), left / right along the ground, up / down straight along y.
//
// The position written is our own running value, not re-read from the game, so gravity or a push the game applies
// between two frames never builds up. If the game moves Link a long way on its own (a load, a teleport, a death) the
// running value is dropped and taken from the game again. Flying counts as falling to the game, whose fall damage is the
// drop from the highest point since Link left the ground, so that highest point (player.fall_top) is kept at his height.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "control_cells.h"
#include "rt.h"

bool control_camera_view(float view[12]);  // runtime/nvn/*

static _Atomic bool on;
static _Atomic int32_t in_x, in_y;       // left stick as the game would have seen it (-32767..32767)
static _Atomic bool in_up, in_down, in_boost;
static bool level;                       // mode: false look, true level
#define SPRINT_SPEED 8.0                 // Link's sprint, units per second: an assumption from memory, NOT measured
#define DEFAULT_SPEED (1.75 * SPRINT_SPEED)  // every entry starts here (units are about metres, at full stick); the mouse wheel changes it while flying
#define MIN_SPEED 1.0
#define MAX_SPEED 2000.0
#define WHEEL_STEP 1.2                   // one wheel notch multiplies the speed by this
static _Atomic double speed = DEFAULT_SPEED;
#define BOOST 6.0
#define RESYNC_DIST 25.0                 // the game moved Link this far on its own: take its value again
static double fp[3];
static uint64_t last_ns;

// Button bits as numbered in control_input.c (names[]).
#define BIT_B 1
#define BIT_X 2
#define BIT_LSTICK 4

// Called where the game's input is assembled (control_input_apply). lx, ly are NULL when only buttons are asked for.
void control_fly_input(uint64_t *b, int32_t *lx, int32_t *ly) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    atomic_store(&in_up, (*b >> BIT_X) & 1);
    atomic_store(&in_down, (*b >> BIT_B) & 1);
    atomic_store(&in_boost, (*b >> BIT_LSTICK) & 1);
    *b &= ~((1ull << BIT_X) | (1ull << BIT_B) | (1ull << BIT_LSTICK));
    if (lx) {
        atomic_store(&in_x, *lx); atomic_store(&in_y, *ly);
        *lx = 0; *ly = 0;
    }
}

static void norm(double v[3]) {
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

void control_fly_frame(uint64_t frame) {
    (void)frame;
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    uint64_t now = plat_now_ns();
    double dt = last_ns ? (double)(now - last_ns) / 1e9 : 0;
    last_ns = now;
    if (dt > 0.1) dt = 0.1;  // (a stall must not become a jump)
    double cur[3];
    if (!control_cell_get(&control_cell_player_pos, cur)) return;  // (Link is loading: wait)
    if (!(fabs(cur[0] - fp[0]) + fabs(cur[2] - fp[2]) < RESYNC_DIST) || fabs(cur[1] - fp[1]) > 200) memcpy(fp, cur, sizeof fp);
    float v[12];
    double fwd[3], right[3];
    if (control_camera_view(v)) {
        // camera basis in world space: the rows of the view matrix are right, up and backwards
        for (int i = 0; i < 3; i++) fwd[i] = -v[8 + i];
        right[0] = v[0]; right[1] = 0; right[2] = v[2];
        if (level) {
            fwd[1] = 0;
            if (fwd[0] * fwd[0] + fwd[2] * fwd[2] < 1e-6) { fwd[0] = v[4]; fwd[2] = v[6]; }  // looking straight up or down: the camera's up points ahead
        }
    } else {
        double eye[3], target[3], yaw, pitch;
        if (!control_cell_camera_angles(eye, target, &yaw, &pitch)) return;
        for (int i = 0; i < 3; i++) fwd[i] = target[i] - eye[i];
        double rad = yaw / 57.29577951308232;
        right[0] = -cos(rad); right[1] = 0; right[2] = sin(rad);
        if (level) fwd[1] = 0;
    }
    norm(fwd); norm(right);
    double sx = atomic_load(&in_x) / 32767.0, sy = atomic_load(&in_y) / 32767.0;
    double m = hypot(sx, sy);
    if (m > 1) { sx /= m; sy /= m; }
    double vy = (atomic_load(&in_up) ? 1.0 : 0.0) - (atomic_load(&in_down) ? 1.0 : 0.0);
    double k = atomic_load(&speed) * (atomic_load(&in_boost) ? BOOST : 1.0) * dt;
    for (int i = 0; i < 3; i++) fp[i] += k * (sy * fwd[i] + sx * right[i]);
    fp[1] += k * vy;
    control_cell_set(&control_cell_player_pos, fp);
    // Fall damage is the drop from the highest point since Link left the ground; flying counts as falling, so land from a
    // long flight and the game sees a 100 m fall. Keep that highest point at his height: landing while flying costs nothing.
    control_cell_set(&control_cell_player_fall_top, &fp[1]);
    control_cell_set(&control_cell_player_fall_top2, &fp[1]);
}

// The hotkey and the `fly` command both come here. Returns the new state; false (and on stays off) when Link is not there.
bool control_fly_set(bool want) {
    if (want == atomic_load(&on)) return want;
    if (want) {
        if (!control_cell_get(&control_cell_player_pos, fp)) {
            LOG("[control] fly mode: not started, Link has no position yet (title screen or loading)\n");
            return false;
        }
        last_ns = 0;
        atomic_store(&speed, DEFAULT_SPEED);  // (every entry starts at the normal speed)
        atomic_store(&in_x, 0); atomic_store(&in_y, 0);
        atomic_store(&in_up, false); atomic_store(&in_down, false); atomic_store(&in_boost, false);
        LOG("[control] fly mode: on at %.1f %.1f %.1f, speed %g, mode %s\n", fp[0], fp[1], fp[2], DEFAULT_SPEED, level ? "level" : "look");
    } else {
        LOG("[control] fly mode: off (speed was %g)\n", atomic_load(&speed));
    }
    atomic_store(&on, want);
    return want;
}
// The mouse wheel (runtime/hle/input.c): up is faster, down slower, each notch a fixed ratio, so it works the same from 1 to 2000.
void control_fly_wheel(float notches) {
    if (!atomic_load(&on)) return;
    double s = atomic_load(&speed) * pow(WHEEL_STEP, notches);
    atomic_store(&speed, s < MIN_SPEED ? MIN_SPEED : s > MAX_SPEED ? MAX_SPEED : s);
}
bool control_fly_toggle(void) { return control_fly_set(!atomic_load(&on)); }

static void fields(ControlReply *r) {
    control_field_bool(r, "fly", atomic_load(&on));
    control_field_str(r, "mode", level ? "level" : "look");
    control_field_num(r, "speed", atomic_load(&speed));
}

// fly | fly on | fly off | fly toggle | fly mode look|level | fly speed <units per second>
static void cmd_fly(int argc, char **argv, ControlReply *r) {
    if (argc == 1) { fields(r); return; }
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off") || !strcmp(argv[1], "toggle"))) {
        bool want = !strcmp(argv[1], "toggle") ? !atomic_load(&on) : !strcmp(argv[1], "on");
        if (control_fly_set(want) != want) { control_fail(r, "No world loaded"); return; }
        fields(r); return;
    }
    if (argc == 3 && !strcmp(argv[1], "mode") && (!strcmp(argv[2], "look") || !strcmp(argv[2], "level"))) {
        level = !strcmp(argv[2], "level"); fields(r); return;
    }
    if (argc == 3 && !strcmp(argv[1], "speed")) {
        char *end; double s = strtod(argv[2], &end);
        if (end == argv[2] || *end || !isfinite(s) || s <= 0 || s > MAX_SPEED) { control_fail(r, "Speed must be above 0 and at most 2000"); return; }
        atomic_store(&speed, s < MIN_SPEED ? MIN_SPEED : s); fields(r); return;
    }
    control_fail(r, "Usage: fly [on | off | toggle | mode look|level | speed <n>]");
}

void control_fly_init(void) {
    static const ControlCmd cmds[] = {
        {"fly", "[on|off|toggle | mode look|level | speed <n>]",
         "Free flight: left stick moves along the camera, X and B move up and down, left stick press boosts", cmd_fly},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
