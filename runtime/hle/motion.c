// Motion controls: nn::hid six-axis sensor states for the game (BOTW's motion aiming and its tilt puzzles).
//
// BOTW 1.8.2 asks for its controller's sensor handles when the controller's style changes (FUN_7100d9e24c:
// GetSixAxisSensorHandles, then StartSixAxisSensor for each) and every frame reads the 16 newest SixAxisSensorStates
// of each handle. FUN_7100feb48c takes all of them: acceleration, angular velocity, angle and direction (turned into
// sead's axes), and counts the new ones from the sampling numbers; a zero acceleration or an identity direction means
// "no data". The camera's motion aiming (FUN_7100c28eac) uses the newest sample's angular velocity and direction, and
// the direction against the previous frame's; the tilt puzzles follow the direction against gravity.
//
// Where the motion comes from: the controller's gyroscope and accelerometer (SDL3 sensor events: DualShock 4,
// DualSense, Switch Pro, Joy-Con, Steam Deck...), fused into an orientation on the main thread (the gyroscope turns it,
// gravity slowly corrects its tilt), with a Linux DSU fallback through the same fusion; or, while the tilt key is
// held, the mouse rotates a virtual controller. Letting go retains its orientation so the mouse can be repositioned;
// the recenter key levels it. A scripted run is another source.
//
// What the game gets: that orientation as yaw / pitch / roll, with the player's sensitivity and inversion applied to
// yaw and pitch (Controls page: g_settings.motion_sens, motion_invert). Samples are made at the Switch's 200 Hz for the
// time that passed; each one's angular velocity is the turn from the previous sample's orientation, its angle the sum
// of those, its acceleration gravity in the controller's axes plus what the real accelerometer measured beyond
// gravity. So everything the game reads agrees with the direction.
//
// Switch axes (the controller held flat in front of the player): X right, Y forward, Z up out of its face. The
// accelerometer reads gravity's direction in G (flat: 0, 0, -1), the gyroscope turns per second (right-hand rule), the
// direction is the controller's X, Y and Z axes in a world whose Z is up. SDL's axes for a controller: X right, Y up
// out of its face, Z toward the player; its accelerometer reads the push against gravity in m/s^2, its gyroscope
// radians per second.
//
// SAVAGEWIND_MOTION_LOG=1 logs the orientation the game gets about once a second, with the sensor stream's health
// (readings per second, readings whose sensor time was unusable, the longest gap) and how often the game read it.
// Always logged (a few dozen lines at most): what the motion comes from whenever it changes, gaps in the sensor
// stream when they end, a sensor clock that stops or jumps.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "../hle.h"
#include "../motion.h"
#include "../platform.h"
#include "../settings.h"
#include "../pc_gyro.h"
#include "../pc_gyro_scene.h"
#ifdef __linux__
#include "../dsu.h"
#endif

#define PI 3.14159265358979323846
#define SAMPLE_NS 5000000ull  // the Switch's sensor rate, 200 Hz
#define NSAMPLES 16           // states kept per handle (BOTW reads 16)
#define KP 0.6                // gravity's pull on the tilt, per radian of error per second
#define TILT_PER_PIXEL (0.2 * PI / 180)
#define PITCH_MAX (85 * PI / 180)

typedef struct {
    int64_t delta_time;       // ns since the previous sample
    int64_t sampling_number;
    float acc[3];             // G
    float gyro[3];            // turns per second
    float angle[3];           // turns
    float dir[3][3];          // the controller's X, Y and Z axes in the world
    uint32_t attributes;      // bit 0: connected
    uint32_t reserved;
} SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "nn::hid::SixAxisSensorState");

typedef struct { double w, x, y, z; } Quat;
typedef double Mat[3][3];  // controller -> world: m[row][col], its columns the controller's axes

static Quat qmul(Quat a, Quat b) {
    return (Quat){a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

static Quat qnorm(Quat q) {
    double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return n > 0 ? (Quat){q.w / n, q.x / n, q.y / n, q.z / n} : (Quat){1, 0, 0, 0};
}

static void quat_mat(Quat q, Mat m) {
    double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = 1 - 2 * (yy + zz), m[0][1] = 2 * (xy - wz), m[0][2] = 2 * (xz + wy);
    m[1][0] = 2 * (xy + wz), m[1][1] = 1 - 2 * (xx + zz), m[1][2] = 2 * (yz - wx);
    m[2][0] = 2 * (xz - wy), m[2][1] = 2 * (yz + wx), m[2][2] = 1 - 2 * (xx + yy);
}

// Yaw about the world's Z, then pitch about the controller's X (front up: positive), then roll about its Y (right
// side down: positive)
static Quat euler_quat(double yaw, double pitch, double roll) {
    Quat z = {cos(yaw / 2), 0, 0, sin(yaw / 2)}, x = {cos(pitch / 2), sin(pitch / 2), 0, 0};
    Quat y = {cos(roll / 2), 0, sin(roll / 2), 0};
    return qmul(qmul(z, x), y);
}

static void euler_of(Mat m, double *yaw, double *pitch, double *roll) {
    double s = m[2][1] > 1 ? 1 : m[2][1] < -1 ? -1 : m[2][1];
    *pitch = asin(s);
    *yaw = atan2(-m[0][1], m[1][1]);
    *roll = atan2(-m[2][0], m[2][2]);
}

static double wrap(double a) { return a - 2 * PI * floor((a + PI) / (2 * PI)); }  // to -pi..pi

// The same orientation has two such triples: (yaw, pitch, roll) and (yaw + pi, +-pi - pitch, roll + pi). euler_of
// keeps the pitch within +-90 degrees, so tilting the controller back past vertical turned yaw and roll by half a turn
// at once (#164). This one takes the triple whose roll is nearest the last one: the pitch goes on past 90 degrees.
static void euler_near(Mat m, double last_roll, double *yaw, double *pitch, double *roll) {
    euler_of(m, yaw, pitch, roll);
    double roll2 = wrap(*roll + PI);
    if (fabs(wrap(roll2 - last_roll)) < fabs(wrap(*roll - last_roll))) {
        *yaw = wrap(*yaw + PI);
        *pitch = (*pitch >= 0 ? PI : -PI) - *pitch;
        *roll = roll2;
    }
}

// Pointing nearly straight up or down (within 10 degrees): yaw and roll mean little there and swing with the
// sensors' noise
static bool steep(double pitch) { return fabs(cos(pitch)) < 0.1736; }

static double sens(void) {
    float k = g_settings.motion_sens;
    return k >= 0.1f && k <= 10 ? k : 1;
}

// ---- the source orientation (main thread writes, the game's controller thread reads)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool sensors;           // the controller has a gyroscope and an accelerometer (turned on)
    bool started;           // its orientation was set from gravity (first accelerometer reading)
    Quat q;                 // its orientation
    double bias[3];         // its gyroscope at rest (rad/s, Switch axes)
    double mean[3], var[3]; // its gyroscope's recent mean and variance (rad/s, (rad/s)^2; ~0.2 s)
    double acc_mean, acc_var;  // its accelerometer's recent strength, mean and variance (G, G^2; ~0.2 s)
    double gyro_sd;         // (the log: the gyroscope's spread just now, rad/s)
    double acc[3];          // last accelerometer reading (G, gravity's direction, Switch axes)
    double lin[3];          // ... beyond gravity
    uint64_t gyro_ts;       // last gyroscope reading's sensor time (ns)
    bool tilt_held;
    // The virtual controller (no sensors needed): continuous angles in radians, so whole turns keep their winding.
    // The tilt key's mouse, the drag at a shrine apparatus and the sticks of a controller without a gyroscope add to
    // tilt_*; the gyro keys turn `keyboard` (degrees). Outside an apparatus a let-go tilt levels again.
    double tilt_pitch, tilt_roll, tilt_yaw;
    PcGyro keyboard;
    int key_pitch, key_roll, key_yaw;  // gyro keys held: -1 / 0 / 1 per axis
    bool recenter;                     // the recenter key: take the nearest level pose, drop the turns still to come
    double sc_pitch, sc_roll, sc_pitch_speed, sc_yaw_speed;  // scripted (radians, radians per second)
} g = {.q = {1, 0, 0, 0}, .acc = {0, 0, -1}};

// The sensor stream's health, for the log (under g_lock)
static struct {
    uint32_t gyro, accel, fused, stalled, jumped;  // readings since the last log line; unusable sensor times
    uint32_t reads;              // GetSixAxisSensorStates calls since the last log line
    uint64_t host_ns;            // when the last gyroscope reading came (SDL's clock, ns)
    uint64_t max_gap;            // the longest time between two of them since the last log line (ns)
    double period;               // the gyroscope's sample period as SDL gives its rate (s; 0: unknown)
    int bad_run;                 // gyroscope readings in a row whose sensor time was unusable
    int notes;                   // (the log: one-off lines so far, capped)
} hs;

#ifdef __linux__
static struct {
    bool enabled;
    uint64_t acc_time, gyro_time, acc_ts, gyro_ts;
    float acc[3];
    int source;  // 0 none, 1 SDL, 2 DSU
    unsigned epoch, dsu_generation;
    DsuClient dsu;
} backend;

static bool native_fresh(uint64_t now) {
    return backend.enabled && backend.acc_time && backend.gyro_time &&
           now - backend.acc_time < DSU_TIMEOUT_NS && now - backend.gyro_time < DSU_TIMEOUT_NS;
}

// Called under g_lock. Never carry bias, gravity, timestamps or integration across devices.
static void select_motion(int source) {
    if (backend.source == source) return;
    LOG("[motion] source: %s -> %s\n", backend.source == 1 ? "SDL" : backend.source == 2 ? "DSU" : "none",
        source == 1 ? "SDL" : source == 2 ? "DSU" : "none");
    backend.source = source;
    backend.epoch++;
    g.sensors = source != 0, g.started = false, g.gyro_ts = 0;
    g.q = (Quat){1, 0, 0, 0};
    memset(g.bias, 0, sizeof g.bias);
    memset(g.mean, 0, sizeof g.mean);
    memset(g.var, 0, sizeof g.var);
    memset(g.lin, 0, sizeof g.lin);
    g.acc[0] = g.acc[1] = 0, g.acc[2] = -1;
    g.acc_mean = 1, g.acc_var = g.gyro_sd = 0;
    hs.host_ns = 0, hs.bad_run = 0;
}
#endif

bool motion_has_sensors(void) {
    pthread_mutex_lock(&g_lock);
    bool sensors = g.sensors;
    pthread_mutex_unlock(&g_lock);
    return sensors;
}

void motion_pad_opened(SDL_Gamepad *pad) {
    bool gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO), accel = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    bool on = gyro && accel && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
              SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    pthread_mutex_lock(&g_lock);
#ifdef __linux__
    backend.enabled = on;
    backend.acc_time = backend.gyro_time = backend.acc_ts = backend.gyro_ts = 0;
    select_motion(0);
    g.sensors = false;
#else
    g.sensors = on;
#endif
    g.started = false, g.gyro_ts = 0;
    memset(g.bias, 0, sizeof g.bias);
    memset(g.mean, 0, sizeof g.mean);
    memset(g.var, 0, sizeof g.var);
    g.acc_mean = 1, g.acc_var = 0;
    memset(g.lin, 0, sizeof g.lin);
    float rate = on ? SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO) : 0;
    hs.period = rate > 1 ? 1.0 / rate : 0, hs.host_ns = 0, hs.bad_run = 0;
    pthread_mutex_unlock(&g_lock);
    if (on)
        LOG("[motion] %s: gyroscope and accelerometer on (%.0f / %.0f Hz)\n", SDL_GetGamepadName(pad),
            SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO), SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    else
        LOG("[motion] %s has no motion sensors%s: the tilt key and the mouse tilt it\n", SDL_GetGamepadName(pad),
            gyro || accel ? " SDL can use" : "");
}

void motion_pad_closed(void) {
    pthread_mutex_lock(&g_lock);
#ifdef __linux__
    backend.enabled = false;
    backend.acc_time = backend.gyro_time = 0;
    if (backend.source == 1) select_motion(0);
#else
    g.sensors = g.started = false;
#endif
    pthread_mutex_unlock(&g_lock);
}

// One gyroscope reading (rad/s, Switch axes) over dt seconds: learn its bias at rest, pull the tilt toward gravity,
// turn the orientation.
// At rest a gyroscope reads its bias, not 0 (a few degrees per second on many controllers), and nothing corrects the
// yaw (gravity only levels the tilt): an unlearnt bias turns the aim on its own. Lying still (only the sensor's noise
// left over ~0.2 s) the reading is the bias, whatever its size: learnt within a quarter second. Held still in the
// hands (a steady reading close to the bias) it is refined slowly, so a slow steady turn isn't taken for bias. (It
// was only learnt in the second case, from 0: a bias above ~3 degrees/s never was, and the aim drifted.)
static void fuse_gyro(const double raw[3], double dt) {
    double an = sqrt(g.acc[0] * g.acc[0] + g.acc[1] * g.acc[1] + g.acc[2] * g.acc[2]);
    double ka = dt / 0.2 > 1 ? 1 : dt / 0.2, sd = 0;
    for (int i = 0; i < 3; i++) {
        double d = raw[i] - g.mean[i];
        g.mean[i] += d * ka;
        g.var[i] += (d * d - g.var[i]) * ka;
        sd = fmax(sd, sqrt(g.var[i]));
    }
    double ad = an - g.acc_mean;  // (the accelerometer: steady, and about gravity's strength - many read a few % off)
    g.acc_mean += ad * ka;
    g.acc_var += (ad * ad - g.acc_var) * ka;
    g.gyro_sd = sd;
    bool only_gravity = sqrt(g.acc_var) < 0.01 && fabs(an - 1) < 0.15;
    double off = sqrt((g.mean[0] - g.bias[0]) * (g.mean[0] - g.bias[0]) + (g.mean[1] - g.bias[1]) * (g.mean[1] - g.bias[1]) +
                      (g.mean[2] - g.bias[2]) * (g.mean[2] - g.bias[2]));
    double tau = 0;
    if (only_gravity) {
        if (sd < 0.006) tau = 0.25;                   // lying still: the sensor's noise only
        else if (sd < 0.02 && off < 0.05) tau = 4.0;  // held still, near the bias (~3 degrees/s)
    }
    if (tau > 0) {
        double k = dt / tau > 1 ? 1 : dt / tau;
        for (int i = 0; i < 3; i++) g.bias[i] += (g.mean[i] - g.bias[i]) * k;
    }
    double w[3] = {raw[0] - g.bias[0], raw[1] - g.bias[1], raw[2] - g.bias[2]};
    Mat m;
    quat_mat(g.q, m);
    if (an > 0.75 && an < 1.25) {  // (shaken or swung hard: the accelerometer isn't gravity, no pull)
        double v[3] = {-m[2][0], -m[2][1], -m[2][2]};  // gravity in the controller's axes, as the orientation has it
        double a[3] = {g.acc[0] / an, g.acc[1] / an, g.acc[2] / an};
        w[0] += KP * (a[1] * v[2] - a[2] * v[1]);
        w[1] += KP * (a[2] * v[0] - a[0] * v[2]);
        w[2] += KP * (a[0] * v[1] - a[1] * v[0]);
    }
    double n = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), h = n * dt / 2;
    double s = n > 1e-12 ? sin(h) / n : dt / 2;
    g.q = qnorm(qmul(g.q, (Quat){cos(h), w[0] * s, w[1] * s, w[2] * s}));
}

// Shared fusion entry points: acceleration is gravity in G, gyro rad/s, both in Switch axes.
static void sensor_accel(const double acc[3]) {
    memcpy(g.acc, acc, sizeof g.acc);
    hs.accel++;
    double an = sqrt(g.acc[0] * g.acc[0] + g.acc[1] * g.acc[1] + g.acc[2] * g.acc[2]);
    if (!g.started && an > 0.5) {
        double v = -g.acc[1] / an;
        g.q = euler_quat(0, asin(v > 1 ? 1 : v < -1 ? -1 : v), atan2(g.acc[0], -g.acc[2]));
        g.started = true;
    }
    Mat m;
    quat_mat(g.q, m);
    for (int i = 0; i < 3; i++) g.lin[i] = g.acc[i] + m[2][i];
}

static void sensor_gyro(const double raw[3], uint64_t ts) {
    if (!g.started) return;
    double dt = g.gyro_ts && ts > g.gyro_ts ? (double)(ts - g.gyro_ts) / 1e9 : 0;
    g.gyro_ts = ts;
    hs.gyro++;
    hs.host_ns = SDL_GetTicksNS();
    if (dt > 0 && dt < 0.1) {
        fuse_gyro(raw, dt);
        hs.fused++;
    }
}

void motion_sensor_event(const SDL_Event *e) {
    const float *d = e->gsensor.data;
    if (e->gsensor.sensor != SDL_SENSOR_ACCEL && e->gsensor.sensor != SDL_SENSOR_GYRO) return;
    if (!isfinite(d[0]) || !isfinite(d[1]) || !isfinite(d[2])) return;
    uint64_t ts = e->gsensor.sensor_timestamp ? e->gsensor.sensor_timestamp : e->gsensor.timestamp;
    pthread_mutex_lock(&g_lock);
#ifdef __linux__
    if (!backend.enabled) { pthread_mutex_unlock(&g_lock); return; }
    uint64_t now = plat_now_ns();
    // Use event time for freshness: the native path below also supports a stalled device clock.
    uint64_t fresh_ts = e->gsensor.timestamp ? e->gsensor.timestamp : ts;
    if (e->gsensor.sensor == SDL_SENSOR_ACCEL) {
        double an = (double)d[0]*d[0] + (double)d[1]*d[1] + (double)d[2]*d[2];
        if (an < 0.01 || an > 100000 || (backend.acc_time && fresh_ts == backend.acc_ts)) {
            pthread_mutex_unlock(&g_lock); return;
        }
        backend.acc_time = now, backend.acc_ts = fresh_ts;
        memcpy(backend.acc, d, sizeof backend.acc);
    } else {
        if (backend.gyro_time && fresh_ts == backend.gyro_ts) { pthread_mutex_unlock(&g_lock); return; }
        backend.gyro_time = now, backend.gyro_ts = fresh_ts;
    }
    if (native_fresh(now)) {
        bool changed = backend.source != 1;
        select_motion(1);
        if (changed) {
            const float *a = backend.acc;
            double acc[3] = {-a[0] / SDL_STANDARD_GRAVITY, a[2] / SDL_STANDARD_GRAVITY, -a[1] / SDL_STANDARD_GRAVITY};
            sensor_accel(acc);
        }
    }
    if (backend.source != 1) { pthread_mutex_unlock(&g_lock); return; }
#else
    if (!g.sensors) { pthread_mutex_unlock(&g_lock); return; }
#endif
    if (e->gsensor.sensor == SDL_SENSOR_ACCEL) {
        double acc[3] = {-d[0] / SDL_STANDARD_GRAVITY, d[2] / SDL_STANDARD_GRAVITY, -d[1] / SDL_STANDARD_GRAVITY};
        sensor_accel(acc);
    } else if (g.started) {
        double raw[3] = {d[0], -d[2], d[1]};
        uint64_t host = e->gsensor.timestamp;
        bool first = !g.gyro_ts;
        double dt = !first && ts > g.gyro_ts ? (double)(ts - g.gyro_ts) / 1e9 : 0;
        double host_dt = hs.host_ns && host > hs.host_ns ? (double)(host - hs.host_ns) / 1e9 : 0;
        g.gyro_ts = ts;
        hs.gyro++;
        if (host_dt * 1e9 > hs.max_gap) hs.max_gap = (uint64_t)(host_dt * 1e9);
        if (host_dt > 0.25 && hs.notes < 64) {
            hs.notes++;
            LOG("[motion] no gyroscope readings for %.2f s\n", host_dt);
        }
        hs.host_ns = host;
        // The reading's time is the sensor's own clock (SDL's sensor timestamp). A controller that passes for a
        // DualShock 4 or a DualSense may keep that clock still or move it in jumps (a still 16-bit DualShock 4 tick
        // reads as a third of a second through SDL's wrap): every reading then looked 0 s or 0.1 s and more after the
        // last one and was dropped, and the aim stood still while the readings went on (suspected in #220). A reading
        // without a usable time counts as one sample period (SDL's rate for the sensor) instead.
        if (!first && !(dt > 0 && dt < 0.1)) {
            if (dt == 0) hs.stalled++;
            else hs.jumped++;
            if (++hs.bad_run == 8 && hs.notes < 64) {
                hs.notes++;
                LOG("[motion] the controller's sensor clock %s (%.4f s from one reading to the next): readings timed "
                    "by its rate, %.1f ms\n", dt == 0 ? "stands still or goes back" : "jumps", dt, hs.period * 1e3);
            }
            dt = hs.period > 0 ? hs.period : host_dt;
        } else if (!first) {
            if (hs.bad_run >= 8 && hs.notes < 64) {
                hs.notes++;
                LOG("[motion] the controller's sensor clock runs again (after %d readings)\n", hs.bad_run);
            }
            hs.bad_run = 0;
        }
        if (dt > 0 && dt < 0.1) {
            fuse_gyro(raw, dt);
            hs.fused++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

#ifdef __linux__
static void dsu_sample(const DsuPacket *p, uint32_t generation, void *user) {
    (void)user;
    if (native_fresh(plat_now_ns())) return;
    if (backend.dsu_generation != generation && backend.source == 2) select_motion(0);
    backend.dsu_generation = generation;
    select_motion(2);
    sensor_accel(p->acc);
    // Microseconds -> ns, without overflowing on a malformed timestamp.
    sensor_gyro(p->gyro, p->timestamp <= UINT64_MAX / 1000 ? p->timestamp * 1000 : 0);
}
#endif

void motion_poll(void) {
#ifdef __linux__
    static bool initialized, enabled, force;
    if (!initialized) {
        initialized = true;
        const char *mode = getenv("SAVAGEWIND_DSU");
        enabled = !mode || strcmp(mode, "off");
        force = mode && !strcmp(mode, "on");
        if (mode && strcmp(mode, "auto") && strcmp(mode, "off") && strcmp(mode, "on"))
            LOG("[dsu] unknown SAVAGEWIND_DSU=%s; using auto\n", mode);
        if (enabled) enabled = dsu_init(&backend.dsu, getenv("SAVAGEWIND_DSU_ENDPOINT"), (uint32_t)plat_now_ns());
        else LOG("[dsu] disabled (SAVAGEWIND_DSU=off)\n");
    }
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    bool native = native_fresh(now);
    if (backend.source == 1 && !native) {
        LOG("[motion] SDL motion timeout; trying fallback\n");
        select_motion(0);
    }
    if (enabled && (force || !native)) dsu_poll(&backend.dsu, now, dsu_sample, NULL);
    else if (enabled && backend.dsu.fd >= 0) dsu_close(&backend.dsu);
    if (backend.source == 2 && !backend.dsu.fresh) select_motion(0);
    pthread_mutex_unlock(&g_lock);
#endif
}

void motion_tilt_held(bool held) {
    pthread_mutex_lock(&g_lock);
    g.tilt_held = held;
    pthread_mutex_unlock(&g_lock);
}

// Mouse forward tilts the controller's front down, mouse right its right side down (as a tilt maze leans)
void motion_mouse(float dx, float dy) {
    pthread_mutex_lock(&g_lock);
    if (g.tilt_held && isfinite(dx) && isfinite(dy)) g.tilt_pitch += dy * TILT_PER_PIXEL, g.tilt_roll += dx * TILT_PER_PIXEL;
    pthread_mutex_unlock(&g_lock);
}

// The tilt key and the mouse wheel: the third axis, six degrees per wheel step
void motion_mouse_yaw(float steps) {
    pthread_mutex_lock(&g_lock);
    if (g.tilt_held && isfinite(steps)) g.tilt_yaw += steps * 6 * PI / 180;
    pthread_mutex_unlock(&g_lock);
}

void motion_script(float pitch_deg, float roll_deg, float pitch_speed, float yaw_speed) {
    pthread_mutex_lock(&g_lock);
    g.sc_pitch = pitch_deg * PI / 180, g.sc_roll = roll_deg * PI / 180;
    g.sc_pitch_speed = pitch_speed * PI / 180, g.sc_yaw_speed = yaw_speed * PI / 180;
    pthread_mutex_unlock(&g_lock);
}

static void virtual_recenter(void) {  // (under g_lock)
    memset(&g.keyboard, 0, sizeof g.keyboard);
    g.tilt_pitch = g.tilt_roll = g.tilt_yaw = 0;
    g.recenter = true;
}

void motion_keyboard(int pitch, int roll, int yaw, bool reset) {
    pthread_mutex_lock(&g_lock);
    g.key_pitch = pitch, g.key_roll = roll, g.key_yaw = yaw;
    if (reset) virtual_recenter();
    pthread_mutex_unlock(&g_lock);
}

// ---- the game's samples (its controller thread, under g_lock)
static struct {
    bool init;
    uint64_t t;                  // time of the newest sample
    int64_t sampling;            // its sampling number
    int head;                    // its place in ring
    SixAxisState ring[NSAMPLES];
    int src;                     // last source: 0 still / mouse / script, positive = sensor source epoch
    double src_yaw;              // ... and its yaw then
    double yaw, pitch, roll;     // the newest sample's orientation as the game gets it
    Mat m;                       // ... as a matrix
    double angle[3];             // turns, summed
    uint64_t last_ns;            // last update (the mouse tilt's levelling, scripted turning)
    double sc_yaw, sc_pitch;     // scripted turning so far
    uint64_t logged;
} s;
RT_ON_RESET(motion_reboot_reset) {
    pthread_mutex_lock(&g_lock);
    memset(&s, 0, sizeof s);
    memset(&g.keyboard, 0, sizeof g.keyboard);
    g.key_pitch = g.key_roll = g.key_yaw = 0;
    g.tilt_held = false;
    g.tilt_pitch = g.tilt_roll = g.tilt_yaw = 0;
    g.recenter = false;
    pthread_mutex_unlock(&g_lock);
}

static void euler_mat(double yaw, double pitch, double roll, Mat m) { quat_mat(euler_quat(yaw, pitch, roll), m); }

// The player's sensitivity and inversion: the game's turn for the source's, its pitch for the source's
static double game_turn(double turn) { return sens() * (g_settings.motion_invert & MOTION_INVERT_LR ? -turn : turn); }
static double game_pitch(double pitch) {
    double p = sens() * (g_settings.motion_invert & MOTION_INVERT_UD ? -pitch : pitch);
    return p > PITCH_MAX ? PITCH_MAX : p < -PITCH_MAX ? -PITCH_MAX : p;
}

// The virtual controller as the game gets it (sensitivity and inversion applied; no pitch limit, it turns over)
static double virtual_pitch(void) {
    double p = g.tilt_pitch + g.sc_pitch + s.sc_pitch + g.keyboard.pitch * PI / 180;
    return sens() * (g_settings.motion_invert & MOTION_INVERT_UD ? -p : p);
}
static double virtual_roll(void) { return g.tilt_roll + g.sc_roll + g.keyboard.roll * PI / 180; }
static double virtual_yaw(void) { return s.sc_yaw + g.tilt_yaw + g.keyboard.yaw * PI / 180; }

// Turn the virtual controller by `delta`, a rotation in the table's axes (X right, Y forward, Z up) as the game sees
// it: composed as quaternions, so any axis works at any pose, then kept as continuous angles. Of the two angle
// triples of the new pose, the one nearest the old is taken, so turning over the top goes on smoothly. (under g_lock)
static void virtual_turn(Quat delta) {
    double oldp = virtual_pitch(), oldr = virtual_roll(), oldy = game_turn(virtual_yaw());
    Mat m;
    quat_mat(qnorm(qmul(delta, euler_quat(oldy, oldp, oldr))), m);
    double yaw, pitch, roll;
    euler_of(m, &yaw, &pitch, &roll);
    double p2 = (pitch >= 0 ? PI : -PI) - pitch, r2 = wrap(roll + PI), y2 = wrap(yaw + PI);
    double d0 = pow(wrap(pitch - oldp), 2) + pow(wrap(roll - oldr), 2) + pow(wrap(yaw - oldy), 2);
    double d1 = pow(wrap(p2 - oldp), 2) + pow(wrap(r2 - oldr), 2) + pow(wrap(y2 - oldy), 2);
    if (d1 < d0) pitch = p2, roll = r2, yaw = y2;
    double invp = g_settings.motion_invert & MOTION_INVERT_UD ? -1 : 1, invy = g_settings.motion_invert & MOTION_INVERT_LR ? -1 : 1;
    g.tilt_pitch += wrap(pitch - oldp) / sens() * invp;
    g.tilt_roll += wrap(roll - oldr);
    g.tilt_yaw += wrap(yaw - oldy) / sens() * invy;
    g.keyboard.active = true;
}

// The drag at a shrine apparatus (runtime/ui/gyro_drag.h): an incremental quaternion in the virtual controller's axes
void motion_gizmo_arcball(float w, float x, float y, float z) {
    if (!isfinite(w) || !isfinite(x) || !isfinite(y) || !isfinite(z)) return;
    double length = sqrt((double)w * w + (double)x * x + (double)y * y + (double)z * z);
    if (length < 1e-8) return;
    pthread_mutex_lock(&g_lock);
    virtual_turn((Quat){w / length, x / length, y / length, z / length});
    pthread_mutex_unlock(&g_lock);
}

// A controller without a gyroscope at a shrine apparatus (runtime/hle/input.c): the right stick turns the table
// (its X) and tilts it front / back (its Y), the left stick banks it (X) and tilts it front / back too (Y); 100
// degrees per second at full tilt, times the motion sensitivity; the inversion options flip them. reset: recenter.
void motion_dualstick(float lx, float ly, float rx, float ry, double dt, bool reset) {
    if (!isfinite(lx) || !isfinite(ly) || !isfinite(rx) || !isfinite(ry)) return;
    pthread_mutex_lock(&g_lock);
    if (reset) virtual_recenter();
    else if (isfinite(dt) && dt > 0) {
        double k = 100 * PI / 180 * (dt > 0.25 ? 0.25 : dt) * sens();
        double p = (ry + ly) * k * (g_settings.motion_invert & MOTION_INVERT_UD ? -1 : 1), r = lx * k;
        double y = -rx * k * (g_settings.motion_invert & MOTION_INVERT_LR ? -1 : 1);
        double n = sqrt(p * p + r * r + y * y);
        if (n > 1e-9) virtual_turn((Quat){cos(n / 2), p * sin(n / 2) / n, r * sin(n / 2) / n, y * sin(n / 2) / n});
    }
    pthread_mutex_unlock(&g_lock);
}

// Leaving the apparatus (runtime/pc_gyro_scene.c): the virtual controller lies flat again, nothing queued
void motion_virtual_end(void) {
    pthread_mutex_lock(&g_lock);
    memset(&g.keyboard, 0, sizeof g.keyboard);
    g.key_pitch = g.key_roll = g.key_yaw = 0;
    g.tilt_held = false;
    g.tilt_pitch = g.tilt_roll = g.tilt_yaw = 0;
    g.sc_pitch = g.sc_roll = g.sc_pitch_speed = g.sc_yaw_speed = 0;
    g.recenter = false;
    if (s.src == 0) {
        int64_t sampling = s.sampling;
        memset(&s, 0, sizeof s);
        s.sampling = sampling;
    }
    pthread_mutex_unlock(&g_lock);
}

// The source's orientation now: the virtual controller when something tilts or turns it (the mouse, the gyro keys, a
// drag, the sticks, a script), else the controller's sensors, else flat
static int source(double dt, Mat m) {
    pc_gyro_step(&g.keyboard, g.key_pitch, g.key_roll, g.key_yaw, dt);
    // At a shrine apparatus the tilt key is a clutch: letting go keeps the pose, so the mouse can take a new grip;
    // only the recenter key (or leaving) levels it. Anywhere else a let-go tilt levels again (~0.15 s), as before, so
    // it never stays in front of the controller's own gyroscope (motion aiming).
    if (!g.tilt_held && !pc_gyro_keyboard_active() && (g.tilt_pitch || g.tilt_roll || g.tilt_yaw)) {
        double k = exp(-dt / 0.15);
        g.tilt_pitch = wrap(g.tilt_pitch) * k, g.tilt_roll = wrap(g.tilt_roll) * k, g.tilt_yaw = wrap(g.tilt_yaw) * k;
        if (fabs(g.tilt_pitch) < 1e-4 && fabs(g.tilt_roll) < 1e-4 && fabs(g.tilt_yaw) < 1e-4)
            g.tilt_pitch = g.tilt_roll = g.tilt_yaw = 0;
    }
    s.sc_yaw += g.sc_yaw_speed * dt;
    s.sc_pitch += g.sc_pitch_speed * dt;
    if (s.sc_pitch > PITCH_MAX) s.sc_pitch = PITCH_MAX;
    if (s.sc_pitch < -PITCH_MAX) s.sc_pitch = -PITCH_MAX;
    bool virt = g.keyboard.active || g.tilt_held || g.tilt_pitch || g.tilt_roll || g.tilt_yaw || g.sc_pitch || g.sc_roll ||
                s.sc_yaw || s.sc_pitch;
    if (g.sensors && g.started && !virt) {
        quat_mat(g.q, m);
#ifdef __linux__
        return (int)backend.epoch + 1;
#else
        return 1;
#endif
    }
    euler_mat(virtual_yaw(), g.tilt_pitch + g.sc_pitch + s.sc_pitch + g.keyboard.pitch * PI / 180,
              g.tilt_roll + g.sc_roll + g.keyboard.roll * PI / 180, m);
    return 0;
}

static void fill(SixAxisState *st, Mat m, const double gyro[3], const double lin[3]) {
    for (int i = 0; i < 3; i++) {
        st->acc[i] = (float)(-m[2][i] + lin[i]);
        st->gyro[i] = (float)gyro[i];
        st->angle[i] = (float)s.angle[i];
        for (int k = 0; k < 3; k++) st->dir[i][k] = (float)m[k][i];
    }
    // (an identity direction means "no data" to the game: lying exactly flat and facing ahead, turn it a hair)
    static const float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (!memcmp(st->dir, id, sizeof id)) st->dir[0][1] = 1e-6f, st->dir[1][0] = -1e-6f;
    st->delta_time = (int64_t)SAMPLE_NS;
    st->attributes = 1;  // IsConnected
}

// The samples due by now (200 Hz)
static void update(uint64_t now) {
    double dt = s.last_ns && now > s.last_ns ? (double)(now - s.last_ns) / 1e9 : 0;
    if (dt > 0.25) dt = 0.25;
    static const double zero[3];
    if (!s.init) {  // (first call: 16 samples of the controller at rest)
        s.last_ns = now;
        Mat m;
        s.src = source(0, m);
        euler_near(m, 0, &s.src_yaw, &s.pitch, &s.roll);
        if (!s.src) s.src_yaw = virtual_yaw(), s.pitch = virtual_pitch(), s.roll = virtual_roll();
        else s.pitch = game_pitch(s.pitch);
        s.yaw = 0;
        euler_mat(s.yaw, s.pitch, s.roll, s.m);
        for (int i = 0; i < NSAMPLES; i++) {
            s.ring[i].sampling_number = ++s.sampling;
            fill(&s.ring[i], s.m, zero, zero);
        }
        s.head = NSAMPLES - 1;
        s.t = now;
        s.init = true;
        return;
    }
    if (now < s.t + SAMPLE_NS) return;
    // (the time since the last sample, not the last poll: polls come more often than samples, from several handles,
    // and a virtual turn would slow down with them)
    s.last_ns = now;
    uint64_t n = (now - s.t) / SAMPLE_NS;
    if (n > NSAMPLES) {
        s.t = now - NSAMPLES * SAMPLE_NS;
        n = NSAMPLES;
    }
    s.t += n * SAMPLE_NS;
    Mat src;
    int kind = source(dt, src);
    if (kind != s.src && hs.notes < 64) {  // (what the motion comes from now, and why)
        hs.notes++;
        LOG("[motion] the game's motion now comes from %s\n",
            kind                                                ? "the controller's sensors"
            : g.tilt_held                                       ? "the mouse (tilt key held)"
            : g.tilt_pitch || g.tilt_roll                       ? "the mouse tilt, levelling"
            : g.sc_pitch || g.sc_roll || s.sc_yaw || s.sc_pitch ? "a script"
            : !g.sensors                                        ? "nothing: no controller sensors (lying still)"
                                                                : "nothing yet: no accelerometer reading (lying still)");
    }
    double yaw, pitch, roll;
    euler_near(src, s.roll, &yaw, &pitch, &roll);
    // (the virtual controller's own continuous angles: from its matrix the pitch would fold at 90 degrees)
    if (!kind) yaw = virtual_yaw(), pitch = virtual_pitch(), roll = virtual_roll();
    // (switching between sources, or pointing nearly straight up or down where yaw means little: no turn)
    bool hold = kind && kind == s.src && steep(pitch);
    double turn = kind == s.src && !hold ? wrap(yaw - s.src_yaw) : 0;
#ifdef __linux__
    if (kind && kind != s.src) {
        // Rebase tilt too: changing physical devices must not synthesize angular velocity. (A virtual source eases
        // there from the last pose instead, pc_gyro_follow.)
        s.pitch = game_pitch(pitch), s.roll = roll;
        euler_mat(s.yaw, s.pitch, s.roll, s.m);
    }
#endif
    if (!hold) s.src_yaw = yaw;
    s.src = kind;
    double yaw1 = s.yaw + game_turn(turn), pitch1 = kind ? game_pitch(pitch) : pitch;
    double dpitch = kind ? pitch1 - s.pitch : wrap(pitch1 - s.pitch);
    double droll = hold ? 0 : wrap(roll - s.roll);
    // the virtual controller goes to its pose at a bounded speed, every turn of the way (pc_gyro_follow)
    double virtual_now[3] = {s.yaw, s.pitch, s.roll}, virtual_target[3] = {game_turn(yaw), pitch1, roll};
    if (!kind && g.recenter) {  // (the recenter key: the nearest level pose, not back through every turn made)
        for (int i = 0; i < 3; i++) virtual_now[i] = wrap(virtual_now[i]);
        g.recenter = false;
    }
    const double *lin = kind ? g.lin : zero;
    for (uint64_t j = 1; j <= n; j++) {  // (in between: the way there in equal steps)
        double f = (double)j / (double)n;
        Mat m;
        if (!kind) {
            pc_gyro_follow(virtual_now, virtual_target, SAMPLE_NS / 1e9);
            euler_mat(virtual_now[0], virtual_now[1], virtual_now[2], m);
        } else euler_mat(s.yaw + (yaw1 - s.yaw) * f, s.pitch + dpitch * f, s.roll + droll * f, m);
        // the turn from the last sample: R_last^T R_now as a rotation vector, per second, in turns
        Mat d;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) d[r][c] = s.m[0][r] * m[0][c] + s.m[1][r] * m[1][c] + s.m[2][r] * m[2][c];
        double v[3] = {(d[2][1] - d[1][2]) / 2, (d[0][2] - d[2][0]) / 2, (d[1][0] - d[0][1]) / 2};
        double sn = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), cs = (d[0][0] + d[1][1] + d[2][2] - 1) / 2;
        double a = sn > 1e-12 ? atan2(sn, cs) / sn : 1;
        double gyro[3];
        for (int i = 0; i < 3; i++) {
            gyro[i] = v[i] * a / (SAMPLE_NS / 1e9) / (2 * PI);
            s.angle[i] += gyro[i] * (SAMPLE_NS / 1e9);
        }
        memcpy(s.m, m, sizeof m);
        s.head = (s.head + 1) % NSAMPLES;
        SixAxisState *st = &s.ring[s.head];
        st->sampling_number = ++s.sampling;
        fill(st, m, gyro, lin);
    }
    if (!kind) s.yaw = virtual_now[0], s.pitch = virtual_now[1], s.roll = virtual_now[2];
    else s.yaw = wrap(yaw1), s.pitch = pitch1, s.roll = wrap(s.roll + droll);
    if (ENV("SAVAGEWIND_MOTION_LOG") && now - s.logged > 1000000000ull) {
        s.logged = now;
        const SixAxisState *st = &s.ring[s.head];
        LOG("[motion] %s: yaw %.1f pitch %.1f roll %.1f deg | gyro %.3f %.3f %.3f turns/s | acc %.2f %.2f %.2f G | "
            "bias %.2f %.2f %.2f deg/s (spread %.4f rad/s, %.4f G) | sample %lld | readings %u gyro %u accel, "
            "%u fused, %u stalled %u jumped, longest gap %.0f ms, last %.0f ms ago | game reads %u\n",
            kind ? "sensors" : g.keyboard.active ? "virtual gyro" : g.tilt_held || g.tilt_pitch || g.tilt_roll ? "mouse tilt"
                                                                                                    : "still/script",
            s.yaw * 180 / PI, s.pitch * 180 / PI, s.roll * 180 / PI, st->gyro[0], st->gyro[1], st->gyro[2], st->acc[0],
            st->acc[1], st->acc[2], g.bias[0] * 180 / PI, g.bias[1] * 180 / PI, g.bias[2] * 180 / PI, g.gyro_sd,
            sqrt(g.acc_var), (long long)st->sampling_number, hs.gyro, hs.accel, hs.fused, hs.stalled, hs.jumped,
            hs.max_gap / 1e6, hs.host_ns ? (double)(SDL_GetTicksNS() - hs.host_ns) / 1e6 : -1.0, hs.reads);
        hs.gyro = hs.accel = hs.fused = hs.stalled = hs.jumped = hs.reads = 0, hs.max_gap = 0;
    }
}

// ---- nn::hid
// SixAxisSensorHandle: {u8 npad style index (FullKey 3, Handheld 4, JoyDual 5, JoyLeft 6, JoyRight 7), u8 npad id,
// u8 device (0 left, 1 right, 2 none), u8}
// int GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &id, NpadStyleSet style)
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
         hid_sixaxis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2], style = (uint32_t)c->x[3];
    int type = style & 1 ? 3 : style & 2 ? 4 : style & 4 ? 5 : style & 8 ? 6 : style & 16 ? 7 : 3;
    uint32_t dev[2] = {type == 5 || type == 6 ? 0u : type == 7 ? 1u : 2u, 1};
    int n = type == 5 ? 2 : 1;
    if (n > count) n = count < 0 ? 0 : count;
    for (int i = 0; i < n; i++) out[i] = (uint32_t)type | (id & 0xff) << 8 | dev[i] << 16;
    static int nlog;
    if (nlog++ < 4) LOG("[motion] six-axis sensor handles for npad %#x (style %#x): %d\n", id, style, n);
    c->x[0] = (uint64_t)n;
}

HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_start) { (void)c; }
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
         hid_sixaxis_zero_drift) { (void)c; }

// int GetSixAxisSensorStates(SixAxisSensorState *out, int count, const SixAxisSensorHandle &handle): the newest
// `count` samples, newest first. Player 1's controller only (the others are disconnected).
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
         hid_sixaxis_states) {
    SixAxisState *out = (SixAxisState *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[2];
    if (!out || count <= 0) {
        c->x[0] = 0;
        return;
    }
    if (count > NSAMPLES) count = NSAMPLES;
    pthread_mutex_lock(&g_lock);
    hs.reads++;
    update(plat_now_ns());
    for (int i = 0; i < count; i++) {
        if ((h >> 8 & 0xff) == 0) {
            out[i] = s.ring[(s.head - i + NSAMPLES) % NSAMPLES];
        } else {
            memset(&out[i], 0, sizeof out[i]);
            out[i].sampling_number = s.sampling - i;
        }
    }
    pthread_mutex_unlock(&g_lock);
    c->x[0] = (uint64_t)count;
}
