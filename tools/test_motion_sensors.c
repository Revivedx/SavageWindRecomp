// Motion from the controller's sensors (runtime/hle/motion.c): sensitivity and inversion on the world's axes (held
// flat and upright as a handheld), lean to turn, repeated reports, the shrine apparatus's own pose, one controller
// seen twice, Steam Input's hidden controllers and the DSU axes. No game files. make test-gyro
#include <assert.h>
#include <stdio.h>
#include "../runtime/hle/motion.c"

Settings g_settings = {.motion_sens = 1, .motion_sens_y = 1};
static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
static bool in_apparatus;
bool pc_gyro_keyboard_active(void) { return in_apparatus; }
bool settings_handheld(void) { return false; }

static void near(double a, double b, const char *what) {
    if (fabs(a - b) > 1e-6) { fprintf(stderr, "%s: got %.9f, expected %.9f\n", what, a, b); abort(); }
}
static void turn(Mat m, double wx, double wy, double wz, double ex, double ey, double ez, const char *what) {
    double w[3] = {wx, wy, wz}, out[3];
    aim_turn(w, m, out);
    near(out[0], ex, what), near(out[1], ey, what), near(out[2], ez, what);
}
static void settings(float sens, float sens_y, int invert, bool lean) {
    g_settings.motion_sens = sens, g_settings.motion_sens_y = sens_y, g_settings.motion_invert = invert;
    g_settings.motion_lean = lean;
}

// Flat (face up) and upright (face toward the player, a handheld): left / right is the turn about gravity either way
static void world_axes(void) {
    Mat flat, upright;
    quat_mat((Quat){1, 0, 0, 0}, flat);
    quat_mat(euler_quat(0, PI / 2, 0), upright);  // (front up a quarter turn: its Y up, its Z toward the player)
    settings(1, 1, 0, false);
    turn(flat, 1, 2, 3, 1, 2, 3, "flat, unit: unchanged");
    turn(upright, 1, 2, 3, 1, -3, 2, "upright, unit: the same turn in the world");
    settings(2, .5f, 0, false);
    turn(flat, 0, 0, 1, 0, 0, 2, "flat yaw x2");
    turn(flat, 1, 0, 0, 1, 0, 0, "flat pitch x2 x0.5");
    turn(flat, 0, 1, 0, 0, 1, 0, "flat roll kept");
    turn(upright, 0, 1, 0, 0, 0, 2, "upright: its Y is the yaw, x2");  // (it had no sensitivity: #136)
    turn(upright, 1, 0, 0, 1, 0, 0, "upright pitch x2 x0.5");
    turn(upright, 0, 0, -1, 0, 1, 0, "upright lean (about its -Z, forward) kept");
    settings(1, 1, MOTION_INVERT_LR, false);
    turn(upright, 0, 1, 0, 0, 0, -1, "upright, left / right inverted");  // (did nothing: #136)
    turn(upright, 1, 0, 0, 1, 0, 0, "upright pitch not inverted by left / right");
    settings(1, 1, MOTION_INVERT_UD, false);
    turn(upright, 1, 0, 0, -1, 0, 0, "upright, up / down inverted");
    turn(upright, 0, 1, 0, 0, 0, 1, "upright yaw not inverted by up / down");  // (both flipped: #136)
    settings(1, 1, 0, true);
    turn(flat, 0, 1, 0, 0, 0, -1, "flat lean right: turns right");
    turn(upright, 0, 0, -1, 0, 0, -1, "upright lean right: turns right");
    turn(flat, 0, 0, 1, 0, 0, 1, "flat yaw still turns with lean");
    // Tilted 40 degrees (a controller as usually held): a turn about gravity stays a turn about gravity, scaled
    Mat tilted;
    quat_mat(euler_quat(0.3, 40 * PI / 180, 0), tilted);
    settings(3, 1, 0, false);
    double w[3] = {tilted[2][0], tilted[2][1], tilted[2][2]}, out[3];  // (world up in its axes: a pure yaw)
    aim_turn(w, tilted, out);
    near(out[0], 0, "tilted yaw x"), near(out[1], 0, "tilted yaw y"), near(out[2], 3, "tilted yaw x3");
    settings(1, 1, 0, false);
    printf("world axes: flat, upright, tilted; sensitivity, vertical sensitivity, each inversion, lean PASS\n");
}

// Gyroscope readings at 250 Hz turning about SDL's Y (the yaw, held flat), each repeated `copies` times as a
// controller polled faster than its sensor sends them: the turn must not depend on the copies (#759)
static double yaw_after(int copies) {
    motion_reboot_reset();
    memset(&g, 0, sizeof g), g.q.w = g.aim_q.w = 1, g.acc[2] = -1;
    memset(&hs, 0, sizeof hs);
    hs.period = 0.004;  // (SDL's rate for the sensor, 250 Hz: what each copy was timed by)
    memset(&backend, 0, sizeof backend);
    backend.enabled = true, backend.pad_id = 7;
    SDL_Event e = {0};
    e.gsensor.which = 7;
    e.gsensor.sensor = SDL_SENSOR_ACCEL, e.gsensor.data[1] = SDL_STANDARD_GRAVITY;
    e.gsensor.timestamp = clock_ns, e.gsensor.sensor_timestamp = 1000;
    motion_sensor_event(&e);
    for (int i = 0; i <= 250; i++) {
        double t = i / 250.0;
        e.gsensor.sensor = SDL_SENSOR_GYRO;
        e.gsensor.data[0] = 0, e.gsensor.data[2] = 0, e.gsensor.data[1] = (float)(2 * sin(6 * t) + 1);
        e.gsensor.sensor_timestamp = 1000 + (uint64_t)i * 4000000;
        for (int c = 0; c <= copies; c++) {
            clock_ns += 4000000 / (copies + 1);
            e.gsensor.timestamp = clock_ns;
            motion_sensor_event(&e);
        }
        e.gsensor.sensor = SDL_SENSOR_ACCEL;  // (an accelerometer reading now and then keeps the source fresh)
        e.gsensor.timestamp = ++clock_ns, e.gsensor.sensor_timestamp += 1;
        motion_sensor_event(&e);
    }
    assert(backend.source == SRC_PAD && g.started);
    Mat m;
    quat_mat(g.q, m);
    double yaw, pitch, roll;
    euler_of(m, &yaw, &pitch, &roll);
    return yaw;
}
static void repeated_reports(void) {
    double once = yaw_after(0);
    assert(fabs(once) > 0.5);
    double eight = yaw_after(7), thirty = yaw_after(31);
    near(eight, once, "8 copies of each report");
    near(thirty, once, "32 copies of each report");
    assert(hs.repeated > 0);
    printf("repeated reports: 1, 8 and 32 copies of each give the same turn PASS\n");
}

// Gyroscope readings at 250 Hz by the PC's clock whose sensor clock runs `speed` times as fast (a controller that passes
// for a DualShock 4 over its dongle, #136): the yaw turned from 1.2 s to 3 s, once the first second has shown the clock
// is off, must be the one a right clock gives.
static double yaw_turned(double speed) {
    motion_reboot_reset();
    memset(&g, 0, sizeof g), g.q.w = g.aim_q.w = 1, g.acc[2] = -1;
    memset(&hs, 0, sizeof hs), memset(&ck, 0, sizeof ck);
    hs.period = 0.004;
    memset(&backend, 0, sizeof backend);
    backend.enabled = true, backend.pad_id = 7;
    SDL_Event e = {0};
    e.gsensor.which = 7;
    e.gsensor.sensor = SDL_SENSOR_ACCEL, e.gsensor.data[1] = SDL_STANDARD_GRAVITY;
    e.gsensor.timestamp = clock_ns, e.gsensor.sensor_timestamp = 1000;
    motion_sensor_event(&e);
    double from = 0;
    for (int i = 0; i <= 750; i++) {
        double t = i / 250.0;
        e.gsensor.sensor = SDL_SENSOR_GYRO;
        e.gsensor.data[0] = 0, e.gsensor.data[2] = 0, e.gsensor.data[1] = (float)(0.6 * sin(5 * t) + 0.3);
        e.gsensor.sensor_timestamp = 1000 + (uint64_t)(i * 4000000.0 * speed);
        clock_ns += 4000000;
        e.gsensor.timestamp = clock_ns;
        motion_sensor_event(&e);
        e.gsensor.sensor = SDL_SENSOR_ACCEL;
        e.gsensor.timestamp = ++clock_ns, e.gsensor.sensor_timestamp += 1;
        motion_sensor_event(&e);
        if (i == 300 || i == 750) {
            Mat m;
            quat_mat(g.q, m);
            double yaw, pitch, roll;
            euler_of(m, &yaw, &pitch, &roll);
            if (i == 300) from = yaw;
            else return yaw - from;
        }
    }
    return 0;
}
static void clock_speed(void) {
    double right = yaw_turned(1);
    assert(fabs(right) > 0.2 && !ck.host);
    double fast = yaw_turned(4);
    assert(ck.host);
    double slow = yaw_turned(0.25);
    assert(ck.host);
    if (fabs(fast - right) > 0.01 || fabs(slow - right) > 0.01) {
        fprintf(stderr, "sensor clock x4: %.4f, x0.25: %.4f, right: %.4f rad\n", fast, slow, right);
        abort();
    }
    printf("sensor clock at 4 and 0.25 times the PC's: timed by the PC's clock, the same turn as a right one PASS\n");
}

// At a shrine apparatus the game gets the controller as it is held (gravity agrees), whatever the sensitivity
static void apparatus_pose(void) {
    settings(3, 1, MOTION_INVERT_UD, false);
    motion_reboot_reset();
    memset(&g, 0, sizeof g);
    g.sensors = g.started = true;
    g.q = euler_quat(0.2, 35 * PI / 180, -20 * PI / 180);
    g.aim_q = euler_quat(0.6, 105 * PI / 180, -20 * PI / 180);  // (sensitivity x3 and inverted: not the real tilt)
    Mat m;
    quat_mat(g.q, m);
    for (int i = 0; i < 3; i++) g.acc[i] = -m[2][i], g.lin[i] = 0;
    in_apparatus = true;
    update(clock_ns += 1000000);
    update(clock_ns += 20000000);
    const SixAxisState *st = &s.ring[s.head];
    for (int i = 0; i < 3; i++) near(st->acc[i], g.acc[i], "apparatus: the controller's own gravity");
    in_apparatus = false;
    update(clock_ns += 20000000);
    st = &s.ring[s.head];
    double d = 0;
    for (int i = 0; i < 3; i++) d += fabs(st->acc[i] - g.acc[i]);
    assert(d > 0.1);  // (motion aiming: the aim's pose)
    for (int i = 0; i < 3; i++) assert(fabs(st->gyro[i]) < 1e-6);  // (a change of pose, no turn made up)
    settings(1, 1, 0, false);
    printf("shrine apparatus: the controller's own pose at any sensitivity, no turn made up on the change PASS\n");
}

static void twins_found(void) {
    memset(presses, 0, sizeof presses), memset(twins, 0, sizeof twins);
    uint64_t t = 5000000000ull;
    assert(!motion_pad_pressed(1, t));
    assert(!motion_pad_pressed(2, t + 6000000));  // (the virtual pad, 6 ms later: one hit)
    assert(!motion_pads_twins(1, 2));
    assert(!motion_pad_pressed(2, t + 7000000));  // (the same press seen again: no second hit)
    assert(!motion_pad_pressed(3, t + 900000000));
    assert(!motion_pad_pressed(1, t + 1000000000));
    assert(motion_pad_pressed(2, t + 1004000000));  // (second press: twins)
    assert(motion_pads_twins(1, 2) && motion_pads_twins(2, 1));
    assert(!motion_pads_twins(1, 3) && !motion_pads_twins(2, 3));
    assert(!motion_pad_pressed(3, t + 2000000000));
    assert(!motion_pad_pressed(4, t + 2100000000));  // (100 ms apart: two players, not a twin)
    assert(!motion_pad_pressed(3, t + 3000000000ull));
    assert(!motion_pad_pressed(4, t + 3200000000ull));
    assert(!motion_pads_twins(3, 4));
    printf("twins: two presses within 50 ms pair two controllers, apart they don't PASS\n");
}

static void steam_hidden(void) {
    nhidden = 0;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "0x054c/0x0ce6,0x28de/0x1205, 0x057e/0x2009");
    motion_before_sdl_init();
    assert(nhidden == 2);
    assert(hidden[0].vendor == 0x054c && hidden[0].product == 0x0ce6);
    assert(hidden[1].vendor == 0x057e && hidden[1].product == 0x2009);
    assert(!strcmp(SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES), "0x28de/0x1205"));  // (Valve's stay hidden)
    printf("Steam Input: its hidden controllers opened for their sensors, Valve's kept hidden PASS\n");
}

static void dsu_axes(void) {
    snprintf(g_settings.motion_dsu_axes, sizeof g_settings.motion_dsu_axes, "pitch roll -yaw");
    dsu_axes_parse();
    assert(dsu_axis[0] == 0 && dsu_axis[1] == 2 && dsu_axis[2] == 1);
    assert(dsu_sign[0] == 1 && dsu_sign[1] == 1 && dsu_sign[2] == -1);
    snprintf(g_settings.motion_dsu_axes, sizeof g_settings.motion_dsu_axes, "pitch pitch roll");
    dsu_axes_parse();  // (refused: kept)
    assert(dsu_axis[1] == 2 && dsu_sign[2] == -1);
    g_settings.motion_dsu_axes[0] = 0;
    printf("DSU axes: a server's order and signs set right, a wrong setting refused PASS\n");
}

int main(void) {
    world_axes();
    repeated_reports();
    clock_speed();
    apparatus_pose();
    twins_found();
    steam_hidden();
    dsu_axes();
    return 0;
}
