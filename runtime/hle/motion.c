// Motion controls: nn::hid six-axis sensor states for the game (BOTW's motion aiming and its tilt puzzles).
//
// BOTW 1.8.2 asks for its controller's sensor handles when the controller's style changes (FUN_7100d9e24c:
// GetSixAxisSensorHandles, then StartSixAxisSensor for each) and every frame reads the 16 newest SixAxisSensorStates
// of each handle. FUN_7100feb48c takes all of them: acceleration, angular velocity, angle and direction (turned into
// sead's axes), and counts the new ones from the sampling numbers; a zero acceleration or an identity direction means
// "no data". The camera's motion aiming (FUN_7100c28eac) uses the newest sample's angular velocity and direction, and
// the direction against the previous frame's; the tilt puzzles follow the direction against gravity.
//
// Where the motion comes from (Controls > Motion source; Automatic tries them in this order):
// - the gyroscope and accelerometer of the controller in use (SDL3 sensor events: DualShock 4, DualSense, Switch Pro,
//   Joy-Con, 8BitDo and the others SDL reads);
// - the same controller seen twice, when the one in use has none: Steam Input's virtual gamepad and the controller it
//   hides from the game (opened for its sensors only, motion_before_sdl_init), or the virtual Xbox pad of DS4Windows,
//   DSX or Handheld Companion and the real controller (their buttons go down together);
// - the Steam Deck's own sensors read from its HID node when Steam Input's gamepad has none (Linux, runtime/deck_imu.c);
// - a DSU (cemuhook) server: DS4Windows, BetterJoy, Handheld Companion, SteamDeckGyroDSU... (runtime/dsu.c);
// - a PC handheld's own sensors as its system gives them (SDL's sensor API: the Windows sensor framework).
// They all go through the same fusion on the main thread (the gyroscope turns it, gravity slowly corrects its tilt).
// Without any, while the tilt key is held, the mouse rotates a virtual controller. Letting go retains its orientation
// so the mouse can be repositioned; the recenter key levels it. A scripted run is another source.
//
// What the game gets: at a shrine apparatus the controller's orientation as it is. Elsewhere (motion aiming) its turns
// on the world's axes: about gravity (left / right, however the controller is held: flat, tilted, or upright as a
// handheld) and about the view's right (up / down), each with its sensitivity and inversion (Controls page:
// g_settings.motion_sens, motion_sens_y, motion_invert); with motion_lean, leaning it like a steering wheel turns too.
// (They were applied to the controller's own axes: held upright, left / right is its Y, which had neither, #136.)
// Samples are made at the Switch's 200 Hz for the time that passed; each one's angular velocity is the turn from the previous sample's orientation, its angle the sum
// of those, its acceleration gravity in the controller's axes plus what the real accelerometer measured beyond
// gravity. So everything the game reads agrees with the direction.
//
// Switch axes (the controller held flat in front of the player): X right, Y forward, Z up out of its face. The
// accelerometer reads gravity's direction in G (flat: 0, 0, -1), the gyroscope turns per second (right-hand rule), the
// direction is the controller's X, Y and Z axes in a world whose Z is up. SDL's axes for a controller: X right, Y up
// out of its face, Z toward the player; its accelerometer reads the push against gravity in m/s^2, its gyroscope
// radians per second. SDL's axes for a device's own sensors: X right, Y to the top of its screen, Z out of it (the
// Switch's, with the screen as the face).
//
// SAVAGEWIND_MOTION_LOG=1 logs the orientation the game gets about once a second, with the sensor stream's health
// (readings per second, readings whose sensor time was unusable, the longest gap) and how often the game read it.
// Always logged (a few dozen lines at most): what the motion comes from whenever it changes, gaps in the sensor
// stream when they end, a sensor clock that stops or jumps.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../motion.h"
#include "../platform.h"
#include "../settings.h"
#include "../pc_gyro.h"
#include "../pc_gyro_scene.h"
#include "../dsu.h"
#ifdef __linux__
#include "../deck_imu.h"
#endif

#define PI 3.14159265358979323846
#define SAMPLE_NS 5000000ull  // the Switch's sensor rate, 200 Hz
#define NSAMPLES 16           // states kept per handle (BOTW reads 16)
#define KP 0.6                // gravity's pull on the tilt, per radian of error per second
#define TILT_PER_PIXEL (0.2 * PI / 180)
#define PITCH_MAX (85 * PI / 180)
#define VALVE_VENDOR 0x28de

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

static double sens(void) {
    float k = g_settings.motion_sens;
    return k >= 0.1f && k <= 10 ? k : 1;
}
static double sens_y(void) {  // (vertical aiming, a fraction of sens)
    float k = g_settings.motion_sens_y;
    return k >= 0.5f && k <= 2 ? k : 1;
}

// ---- the source orientation (main thread writes, the game's controller thread reads)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool sensors;           // the controller has a gyroscope and an accelerometer (turned on)
    bool started;           // its orientation was set from gravity (first accelerometer reading)
    Quat q;                 // its physical orientation
    Quat aim_q;             // its turns as motion aiming takes them (sensitivity, inversion, lean: aim_turn)
    double bias[3];         // its gyroscope at rest (rad/s, Switch axes)
    double mean[3], var[3]; // its gyroscope's recent mean and variance (rad/s, (rad/s)^2; ~0.2 s)
    double acc_mean, acc_var;  // its accelerometer's recent strength, mean and variance (G, G^2; ~0.2 s)
    double gyro_sd;         // (the log: the gyroscope's spread just now, rad/s)
    double acc[3];          // last accelerometer reading (G, gravity's direction, Switch axes)
    double lin[3];          // ... beyond gravity
    uint64_t gyro_ts;       // last gyroscope reading's sensor time (ns)
    float last_gyro[3];     // last gyroscope reading as SDL gave it (a report repeated: the same time and values)
    bool tilt_held;
    // The virtual controller (no sensors needed): continuous angles in radians, so whole turns keep their winding.
    // The tilt key's mouse, the drag at a shrine apparatus and the sticks of a controller without a gyroscope add to
    // tilt_*; the gyro keys turn `keyboard` (degrees). Outside an apparatus a let-go tilt levels again.
    double tilt_pitch, tilt_roll, tilt_yaw;
    PcGyro keyboard;
    int key_pitch, key_roll, key_yaw;  // gyro keys held: -1 / 0 / 1 per axis
    bool recenter;                     // the recenter key: take the nearest level pose, drop the turns still to come
    double sc_pitch, sc_roll, sc_pitch_speed, sc_yaw_speed;  // scripted (radians, radians per second)
} g = {.q = {1, 0, 0, 0}, .aim_q = {1, 0, 0, 0}, .acc = {0, 0, -1}};

// The sensor stream's health, for the log (under g_lock)
static struct {
    uint32_t gyro, accel, fused, stalled, jumped, repeated;  // readings since the last log line; unusable sensor times
    uint32_t reads;              // GetSixAxisSensorStates calls since the last log line
    uint64_t host_ns;            // when the last gyroscope reading came (SDL's clock, ns)
    uint64_t max_gap;            // the longest time between two of them since the last log line (ns)
    double period;               // the gyroscope's sample period as SDL gives its rate (s; 0: unknown)
    int bad_run;                 // gyroscope readings in a row whose sensor time was unusable
    int notes;                   // (the log: one-off lines so far, capped)
    uint64_t total, rate_total, rate_ns;  // gyroscope readings fused in all; the count and time at the last rate
    float rate;                  // ... readings per second, measured (the Controls page's status)
} hs;

// The sensor clock against the PC's (under g_lock). Its readings are timed by the sensor's own clock, which is precise
// when right; a controller that passes for a DualShock 4 (a Gamesir G7 Pro in its DualShock mode over its dongle, #136)
// can move that clock by plausible steps at the wrong speed, and every turn came out several times too far and
// unsteady while the same pad wired, or TotK Recomp timing it by the PC's clock, was fine. Over each second of
// readings the sensor's time is compared with the PC's: off by more than a quarter, the readings are timed by the
// PC's clock instead (its time over the count: one even period each), until a second agrees again.
static struct {
    uint64_t host0, last;  // the window's first reading and the last one (PC's clock, ns)
    double dev;            // the sensor's time between the window's readings (s)
    int n, bad;            // readings since host0; those without a usable sensor time
    bool host;             // timed by the PC's clock now
    double period;         // ... one reading's time (s)
} ck;

// Where the motion comes from now (backend.source)
enum { SRC_NONE, SRC_PAD, SRC_DSU, SRC_DECK, SRC_DEVICE, SRC_COUNT };
// Why that controller's sensors (backend.pad_why)
enum { PAD_NONE, PAD_IN_USE, PAD_TWIN, PAD_STEAM };

static struct {
    SDL_Gamepad *pad;        // the controller whose sensors are on (NULL: none)
    SDL_JoystickID pad_id;   // ... its id: its sensor events are the ones taken
    int pad_why;             // PAD_*
    char pad_name[96];
    bool enabled;            // its gyroscope and accelerometer are on
    // ... with no data rate: the device's own sensor lent to a controller without one by SDL's sensor fusion
    // (SDL_HINT_GAMECONTROLLER_SENSOR_FUSION; a handheld such as the ROG Ally), which can stall for a third of a second
    // at a time. A DSU server that answers comes before it; a controller's own sensors (with a rate) don't (from #860).
    bool unrated;
    uint64_t acc_time, gyro_time, acc_ts, gyro_ts;
    float acc[3];
    int source;              // SRC_*
    unsigned epoch, dsu_generation;
    bool steam_pad;          // the controller in use is Steam Input's (Valve's vendor ID) and has no sensors
    char in_use[96];         // the controller in use (the status, when it has no sensors)
    bool dsu_on;             // the DSU client started (SAVAGEWIND_DSU, the endpoint)
    DsuClient dsu;
#ifdef __linux__
    DeckImu deck;
#endif
    SDL_Sensor *dev_acc, *dev_gyro;  // a PC handheld's own sensors (SRC_DEVICE)
    SDL_SensorID dev_acc_id, dev_gyro_id;
    bool dev_tried;
    uint64_t dev_acc_time, dev_gyro_time;
    float dev_acc_v[3];
} backend;

static bool native_fresh(uint64_t now) {
    return backend.enabled && backend.acc_time && backend.gyro_time &&
           now - backend.acc_time < DSU_TIMEOUT_NS && now - backend.gyro_time < DSU_TIMEOUT_NS;
}

// Whether the controller's sensors come before DSU now
static bool native_first(uint64_t now) {
    return native_fresh(now) && !(backend.unrated && backend.dsu.fresh);
}

// Called under g_lock. Never carry bias, gravity, timestamps or integration across devices.
static void select_motion(int source) {
    if (backend.source == source) return;
    static const char *const names[SRC_COUNT] = {"none", "SDL", "DSU", "Steam Deck", "the handheld's own sensors"};
    LOG("[motion] source: %s -> %s\n", names[backend.source], names[source]);
    backend.source = source;
    backend.epoch++;
    g.sensors = source != SRC_NONE, g.started = false, g.gyro_ts = 0;
    g.q = g.aim_q = (Quat){1, 0, 0, 0};
    memset(g.bias, 0, sizeof g.bias);
    memset(g.mean, 0, sizeof g.mean);
    memset(g.var, 0, sizeof g.var);
    memset(g.lin, 0, sizeof g.lin);
    memset(g.last_gyro, 0, sizeof g.last_gyro);
    g.acc[0] = g.acc[1] = 0, g.acc[2] = -1;
    g.acc_mean = 1, g.acc_var = g.gyro_sd = 0;
    hs.host_ns = 0, hs.bad_run = 0;
    memset(&ck, 0, sizeof ck);
}

bool motion_has_sensors(void) {
    pthread_mutex_lock(&g_lock);
    bool sensors = g.sensors;
    pthread_mutex_unlock(&g_lock);
    return sensors;
}

// ---- the controllers Steam Input hides
// Steam Input hands the game its virtual gamepad, without sensors, and asks SDL (SDL_GAMECONTROLLER_IGNORE_DEVICES) to
// leave the controllers behind it alone, so the game never saw their gyroscopes (#136, #812). They are opened anyway,
// for their sensors only: their buttons never reach the game (motion_pad_sensor_only), Steam's virtual pad keeps
// those. Valve's own stay hidden: SDL's Steam Controller and Deck drivers reset the controller's settings, which Steam
// is using (the Deck's sensors are read from its HID node instead). SAVAGEWIND_STEAM_SENSORS=0: leave them all hidden.
static struct { uint16_t vendor, product; } hidden[16];
static int nhidden;

void motion_before_sdl_init(void) {
    const char *list = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES);
    const char *env = getenv("SAVAGEWIND_STEAM_SENSORS");
    if (!list || !*list || (env && !atoi(env)) || g_settings.motion_source == MOTION_SOURCE_OFF ||
        g_settings.motion_source == MOTION_SOURCE_DSU)
        return;
    char keep[512] = "";
    size_t kept = 0;
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        char *end;
        unsigned long vendor = strtoul(p, &end, 0), product = 0;
        bool ok = end != p && *end == '/';
        if (ok) {
            const char *q = end + 1;
            product = strtoul(q, &end, 0);
            ok = end != q && (*end == ',' || *end == ' ' || !*end);
        }
        if (ok && vendor <= 0xffff && product <= 0xffff) {
            if (vendor == VALVE_VENDOR) {
                int n = snprintf(keep + kept, sizeof keep - kept, "%s0x%04lx/0x%04lx", kept ? "," : "", vendor, product);
                if (n > 0 && (size_t)n < sizeof keep - kept) kept += n;
            } else if (nhidden < (int)(sizeof hidden / sizeof *hidden)) {
                hidden[nhidden].vendor = (uint16_t)vendor, hidden[nhidden].product = (uint16_t)product;
                nhidden++;
            }
        }
        p = strchr(p, ',');
    }
    if (!nhidden) return;
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, keep, SDL_HINT_OVERRIDE);
    LOG("[motion] Steam Input hides %d controller%s from the game: opened for their motion sensors only "
        "(SAVAGEWIND_STEAM_SENSORS=0: leave them hidden)\n", nhidden, nhidden == 1 ? "" : "s");
}

bool motion_pad_sensor_only(SDL_Gamepad *p) {
    if (!p || !nhidden) return false;
    uint16_t vendor = SDL_GetGamepadVendor(p), product = SDL_GetGamepadProduct(p);
    for (int i = 0; i < nhidden; i++)
        if (hidden[i].vendor == vendor && hidden[i].product == product) return true;
    return false;
}

// ---- one controller seen twice (main thread)
// DS4Windows, DSX or Handheld Companion show a virtual Xbox pad beside the real controller, and Steam Input its
// virtual gamepad beside the one it hides: a button pressed goes down on both within a few milliseconds. Two such
// presses make them twins: the motion stays on the one with sensors whichever of them is in use (it followed the one
// pressed last and lost the gyroscope with the virtual pad, a likely cause of "it works once, then stops", #220 #387).
#define TWIN_NS 50000000ull
static struct { SDL_JoystickID id; uint64_t ns; } presses[8];
static struct { SDL_JoystickID a, b; int hits; uint64_t ns; } twins[8];

static int twin_slot(SDL_JoystickID a, SDL_JoystickID b) {
    for (int i = 0; i < 8; i++)
        if ((twins[i].a == a && twins[i].b == b) || (twins[i].a == b && twins[i].b == a)) return i;
    return -1;
}

bool motion_pads_twins(SDL_JoystickID a, SDL_JoystickID b) {
    int i = a && b && a != b ? twin_slot(a, b) : -1;
    return i >= 0 && twins[i].hits >= 2;
}

bool motion_pad_pressed(SDL_JoystickID id, uint64_t ns) {
    bool paired = false;
    int own = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
        if (presses[i].id == id) { own = i; continue; }
        if (presses[i].ns < presses[oldest].ns) oldest = i;
        if (!presses[i].id || presses[i].ns + TWIN_NS < ns || ns + TWIN_NS < presses[i].ns) continue;
        int t = twin_slot(id, presses[i].id);
        if (t < 0) {
            t = 0;
            for (int k = 1; k < 8; k++)
                if (twins[k].hits < twins[t].hits) t = k;
            twins[t].a = id, twins[t].b = presses[i].id, twins[t].hits = 0, twins[t].ns = 0;
        }
        if (twins[t].ns != presses[i].ns) {  // (one hit per press of the other one)
            twins[t].ns = presses[i].ns;
            if (++twins[t].hits == 2) paired = true;
        }
    }
    int slot = own >= 0 ? own : oldest;
    for (int i = 0; own < 0 && i < 8; i++)
        if (!presses[i].id) { slot = i; break; }
    presses[slot].id = id, presses[slot].ns = ns;
    return paired;
}

static bool pad_sensors(SDL_Gamepad *p) {
    return p && SDL_GamepadHasSensor(p, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(p, SDL_SENSOR_ACCEL);
}

// The controllers changed (one connected, gone or now in use, or two found to be twins): which one's sensors to take
void motion_pads(SDL_Gamepad *in_use, SDL_Gamepad *const *pads, int n) {
    int mode = g_settings.motion_source;
    SDL_Gamepad *want = NULL;
    int why = PAD_NONE;
    if (mode == MOTION_SOURCE_AUTO || mode == MOTION_SOURCE_CONTROLLER) {
        if (pad_sensors(in_use)) want = in_use, why = PAD_IN_USE;
        if (!want && in_use && mode == MOTION_SOURCE_AUTO) {
            SDL_JoystickID id = SDL_GetGamepadID(in_use);
            SDL_Gamepad *steam = NULL;
            int nsteam = 0, nvirtual = 0;
            for (int i = 0; i < n && !want; i++) {
                if (SDL_GetGamepadVendor(pads[i]) == VALVE_VENDOR && !pad_sensors(pads[i])) nvirtual++;
                if (pads[i] == in_use || !pad_sensors(pads[i])) continue;
                if (motion_pads_twins(id, SDL_GetGamepadID(pads[i]))) want = pads[i], why = PAD_TWIN;
                else if (motion_pad_sensor_only(pads[i])) steam = pads[i], nsteam++;
            }
            // Steam Input's only gamepad in use and one controller it hides with sensors: that one, before any press
            // (with more - the Deck's own controls and a DualSense - the presses tell which is behind which)
            if (!want && nsteam == 1 && nvirtual == 1 && SDL_GetGamepadVendor(in_use) == VALVE_VENDOR)
                want = steam, why = PAD_STEAM;
        }
    }
    static SDL_Gamepad *logged = (SDL_Gamepad *)1;  // (the controller in use the last "no motion sensors" line named)
    pthread_mutex_lock(&g_lock);
    backend.steam_pad = in_use && !pad_sensors(in_use) && SDL_GetGamepadVendor(in_use) == VALVE_VENDOR;
    snprintf(backend.in_use, sizeof backend.in_use, "%s", in_use && SDL_GetGamepadName(in_use) ? SDL_GetGamepadName(in_use) : "");
    if (want == backend.pad) {
        if (want) backend.pad_why = why;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    SDL_Gamepad *old = backend.pad;
    for (int i = 0; old && i < n; i++)
        if (pads[i] == old) {  // (still connected: its sensors off again)
            SDL_SetGamepadSensorEnabled(old, SDL_SENSOR_GYRO, false);
            SDL_SetGamepadSensorEnabled(old, SDL_SENSOR_ACCEL, false);
        }
    bool on = want && SDL_SetGamepadSensorEnabled(want, SDL_SENSOR_GYRO, true) &&
              SDL_SetGamepadSensorEnabled(want, SDL_SENSOR_ACCEL, true);
    float rate = on ? SDL_GetGamepadSensorDataRate(want, SDL_SENSOR_GYRO) : 0;
    backend.pad = on ? want : NULL;
    backend.pad_id = on ? SDL_GetGamepadID(want) : 0;
    backend.pad_why = on ? why : PAD_NONE;
    backend.enabled = on;
    backend.unrated = on && !(rate > 0);
    snprintf(backend.pad_name, sizeof backend.pad_name, "%s", on && SDL_GetGamepadName(want) ? SDL_GetGamepadName(want) : "");
    backend.acc_time = backend.gyro_time = backend.acc_ts = backend.gyro_ts = 0;
    if (backend.source == SRC_PAD) select_motion(SRC_NONE);
    hs.period = rate > 1 ? 1.0 / rate : 0, hs.host_ns = 0, hs.bad_run = 0;
    memset(&ck, 0, sizeof ck);
    pthread_mutex_unlock(&g_lock);
    if (on) {
        LOG("[motion] %s: gyroscope and accelerometer on (%.0f / %.0f Hz)%s\n", SDL_GetGamepadName(want), rate,
            SDL_GetGamepadSensorDataRate(want, SDL_SENSOR_ACCEL),
            why == PAD_TWIN    ? ", the same controller as the one in use, seen twice"
            : why == PAD_STEAM ? ", the controller behind Steam Input's gamepad"
                               : "");
        if (!(rate > 0))
            LOG("[motion] (no rate: probably the device's own sensor through SDL's sensor fusion; a DSU server that "
                "answers comes first)\n");
        logged = (SDL_Gamepad *)1;
    } else if (in_use && in_use != logged) {
        logged = in_use;
        bool some = SDL_GamepadHasSensor(in_use, SDL_SENSOR_GYRO) || SDL_GamepadHasSensor(in_use, SDL_SENSOR_ACCEL);
        LOG("[motion] %s has no motion sensors%s: %s\n", SDL_GetGamepadName(in_use),
            some ? " SDL can use" : "",
            mode == MOTION_SOURCE_OFF ? "motion source Off"
            : mode == MOTION_SOURCE_DSU ? "motion source DSU"
            : SDL_GetGamepadVendor(in_use) == VALVE_VENDOR ? "Steam Input's gamepad; the Deck's own sensors, a "
                                                             "DSU server, or the tilt key and the mouse tilt it"
                                                           : "a DSU server, or the tilt key and the mouse tilt it");
    }
}

// A controller about to be closed (input.c): never touched again
void motion_pad_removed(SDL_Gamepad *p) {
    SDL_JoystickID id = p ? SDL_GetGamepadID(p) : 0;
    for (int i = 0; id && i < 8; i++) {
        if (presses[i].id == id) presses[i].id = 0, presses[i].ns = 0;
        if (twins[i].a == id || twins[i].b == id) memset(&twins[i], 0, sizeof twins[i]);
    }
    pthread_mutex_lock(&g_lock);
    if (p && p == backend.pad) {
        backend.pad = NULL, backend.pad_id = 0, backend.pad_why = PAD_NONE, backend.enabled = backend.unrated = false;
        backend.pad_name[0] = 0;
        backend.acc_time = backend.gyro_time = 0;
        if (backend.source == SRC_PAD) select_motion(SRC_NONE);
    }
    pthread_mutex_unlock(&g_lock);
}

SDL_JoystickID motion_sensor_pad(void) {
    pthread_mutex_lock(&g_lock);
    SDL_JoystickID id = backend.pad_id;
    pthread_mutex_unlock(&g_lock);
    return id;
}

void motion_status(MotionStatus *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_lock);
    out->off = g_settings.motion_source == MOTION_SOURCE_OFF;
    out->steam_pad = backend.steam_pad;
    out->rate = hs.rate;
    snprintf(out->in_use, sizeof out->in_use, "%s", backend.in_use);
    switch (g.sensors ? backend.source : SRC_NONE) {
    case SRC_PAD:
        out->source = MOTION_FROM_CONTROLLER;
        out->twin = backend.pad_why == PAD_TWIN || backend.pad_why == PAD_STEAM;
        snprintf(out->name, sizeof out->name, "%s", backend.pad_name);
        break;
    case SRC_DSU: out->source = MOTION_FROM_DSU; break;
    case SRC_DECK: out->source = MOTION_FROM_DECK; break;
    case SRC_DEVICE: out->source = MOTION_FROM_DEVICE; break;
    default: out->source = MOTION_FROM_NONE; break;
    }
    pthread_mutex_unlock(&g_lock);
}

static Quat rotation_step(const double w[3], double dt) {
    double n = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), h = n * dt / 2;
    double k = n > 1e-12 ? sin(h) / n : dt / 2;
    return (Quat){cos(h), w[0] * k, w[1] * k, w[2] * k};
}

// The aim's turn, in the world's axes (rad/s), from the controller's turn w in its own axes (m: its orientation):
// about gravity (left / right) and about the view's right (up / down), each with its sensitivity and inversion. The
// view's forward is the level part of the controller's Y (held flat) minus its Z (held upright, the face toward the
// player): forward either way, and in between. Leaning about it is the controller's roll, kept as it is, or with
// motion_lean a turn left / right too (right side down: right).
static void aim_turn(const double w[3], Mat m, double out[3]) {
    double ww[3];
    for (int i = 0; i < 3; i++) ww[i] = m[i][0] * w[0] + m[i][1] * w[1] + m[i][2] * w[2];
    double kx = sens() * (g_settings.motion_invert & MOTION_INVERT_LR ? -1 : 1);
    double ky = sens() * sens_y() * (g_settings.motion_invert & MOTION_INVERT_UD ? -1 : 1);
    double f[2] = {m[0][1] - m[0][2], m[1][1] - m[1][2]}, fn = sqrt(f[0] * f[0] + f[1] * f[1]);
    if (fn < 1e-6) {  // (no level forward: an odd pose; the rest as up / down)
        out[0] = ww[0] * ky, out[1] = ww[1] * ky, out[2] = ww[2] * kx;
        return;
    }
    f[0] /= fn, f[1] /= fn;
    double r[2] = {f[1], -f[0]};  // the view's right: forward x up
    double pitch = ww[0] * r[0] + ww[1] * r[1], lean = ww[0] * f[0] + ww[1] * f[1], yaw = ww[2];
    if (g_settings.motion_lean) yaw -= lean, lean = 0;
    pitch *= ky, yaw *= kx;
    out[0] = r[0] * pitch + f[0] * lean, out[1] = r[1] * pitch + f[1] * lean, out[2] = yaw;
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
    g.q = qnorm(qmul(g.q, rotation_step(w, dt)));
    // Each sensor increment is split and scaled here, before a window or game frame can batch turns about several
    // axes (scaling their sum depended on the frame rate), and in the world's axes: composed on the left.
    double aim[3];
    aim_turn(w, m, aim);
    g.aim_q = qnorm(qmul(rotation_step(aim, dt), g.aim_q));
}

// Shared fusion entry points: acceleration is gravity in G, gyro rad/s, both in Switch axes.
static void sensor_accel(const double acc[3]) {
    memcpy(g.acc, acc, sizeof g.acc);
    hs.accel++;
    double an = sqrt(g.acc[0] * g.acc[0] + g.acc[1] * g.acc[1] + g.acc[2] * g.acc[2]);
    if (!g.started && an > 0.5) {
        double v = -g.acc[1] / an;
        g.q = euler_quat(0, asin(v > 1 ? 1 : v < -1 ? -1 : v), atan2(g.acc[0], -g.acc[2]));
        g.aim_q = g.q;
        g.started = true;
    }
    Mat m;
    quat_mat(g.q, m);
    for (int i = 0; i < 3; i++) g.lin[i] = g.acc[i] + m[2][i];
}

// One gyroscope reading's time (not the first's): the sensor's (dt; dev_ok: usable) or, while the sensor's clock
// disagrees with the PC's (ck), one even period of the PC's. host: when it came, by the PC's clock (ns).
static double clock_dt(double dt, bool dev_ok, uint64_t host) {
    if (!ck.host0 || host < ck.last || host - ck.last > 250000000ull) {  // (the first reading, or after a gap: a new window)
        ck.host0 = ck.last = host, ck.dev = 0, ck.n = ck.bad = 0;
        return ck.host && dev_ok ? ck.period : dt;
    }
    ck.last = host;
    ck.n++;
    if (dev_ok) ck.dev += dt;
    else ck.bad++;
    double span = (double)(host - ck.host0) / 1e9;
    if (span >= 1 && ck.n >= 30) {
        if (ck.bad * 10 < ck.n) {  // (a clock that stands still or jumps: those readings are timed already)
            double ratio = ck.dev / span;
            bool off = ratio < 0.8 || ratio > 1.25;
            if (off != ck.host && hs.notes < 64) {
                hs.notes++;
                LOG(off ? "[motion] the controller's sensor clock runs at %.2f times the PC's: readings timed by the PC's clock "
                          "(%.1f ms each)\n"
                        : "[motion] the controller's sensor clock agrees with the PC's again (%.2f times): readings timed by "
                          "it (%.1f ms each)\n",
                    ratio, span / ck.n * 1e3);
            }
            ck.host = off;
            ck.period = span / ck.n;
        }
        ck.host0 = host, ck.dev = 0, ck.n = ck.bad = 0;
    }
    return ck.host && dev_ok ? ck.period : dt;
}

static void sensor_gyro(const double raw[3], uint64_t ts) {
    if (!g.started) return;
    bool first = !g.gyro_ts;
    double dt = g.gyro_ts && ts > g.gyro_ts ? (double)(ts - g.gyro_ts) / 1e9 : 0;
    g.gyro_ts = ts;
    hs.gyro++;
    hs.host_ns = SDL_GetTicksNS();
    if (!first) dt = clock_dt(dt, dt > 0 && dt < 0.1, hs.host_ns);
    if (dt > 0 && dt < 0.1) {
        fuse_gyro(raw, dt);
        hs.fused++, hs.total++;
    }
}

void motion_sensor_event(const SDL_Event *e) {
    const float *d = e->gsensor.data;
    if (e->gsensor.sensor != SDL_SENSOR_ACCEL && e->gsensor.sensor != SDL_SENSOR_GYRO) return;
    if (!isfinite(d[0]) || !isfinite(d[1]) || !isfinite(d[2])) return;
    uint64_t ts = e->gsensor.sensor_timestamp ? e->gsensor.sensor_timestamp : e->gsensor.timestamp;
    pthread_mutex_lock(&g_lock);
    if (!backend.enabled || e->gsensor.which != backend.pad_id) { pthread_mutex_unlock(&g_lock); return; }
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
    if (native_first(now) && g_settings.motion_source != MOTION_SOURCE_DSU && g_settings.motion_source != MOTION_SOURCE_OFF) {
        bool changed = backend.source != SRC_PAD;
        select_motion(SRC_PAD);
        if (changed) {
            const float *a = backend.acc;
            double acc[3] = {-a[0] / SDL_STANDARD_GRAVITY, a[2] / SDL_STANDARD_GRAVITY, -a[1] / SDL_STANDARD_GRAVITY};
            sensor_accel(acc);
        }
    }
    if (backend.source != SRC_PAD) { pthread_mutex_unlock(&g_lock); return; }
    if (e->gsensor.sensor == SDL_SENSOR_ACCEL) {
        double acc[3] = {-d[0] / SDL_STANDARD_GRAVITY, d[2] / SDL_STANDARD_GRAVITY, -d[1] / SDL_STANDARD_GRAVITY};
        sensor_accel(acc);
    } else if (g.started) {
        double raw[3] = {d[0], -d[2], d[1]};
        uint64_t host = e->gsensor.timestamp;
        bool first = !g.gyro_ts;
        double dt = !first && ts > g.gyro_ts ? (double)(ts - g.gyro_ts) / 1e9 : 0;
        double host_dt = hs.host_ns && host > hs.host_ns ? (double)(host - hs.host_ns) / 1e9 : 0;
        // The same report again: the sensor's time and its values unchanged. A controller polled faster than its
        // sensor runs (a DualSense at 1000-8000 Hz, #759, and some that pass for one) repeats its last report; each
        // copy was taken for a reading one sample period long, and the aim turned several times too far.
        if (!first && ts == g.gyro_ts && !memcmp(d, g.last_gyro, sizeof g.last_gyro)) {
            hs.repeated++;
            pthread_mutex_unlock(&g_lock);
            return;
        }
        memcpy(g.last_gyro, d, sizeof g.last_gyro);
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
        // without a usable time is timed by when it came (SDL's event time), else by one sample period (SDL's rate).
        // One that runs at another speed than the PC's is caught by clock_dt.
        bool dev_ok = !first && dt > 0 && dt < 0.1;
        if (!first && !(dt > 0 && dt < 0.1)) {
            if (dt == 0) hs.stalled++;
            else hs.jumped++;
            if (++hs.bad_run == 8 && hs.notes < 64) {
                hs.notes++;
                LOG("[motion] the controller's sensor clock %s (%.4f s from one reading to the next): readings timed "
                    "by when they come\n", dt == 0 ? "stands still or goes back" : "jumps", dt);
            }
            dt = host_dt > 0 && host_dt < 0.1 ? host_dt : hs.period;
        } else if (!first) {
            if (hs.bad_run >= 8 && hs.notes < 64) {
                hs.notes++;
                LOG("[motion] the controller's sensor clock runs again (after %d readings)\n", hs.bad_run);
            }
            hs.bad_run = 0;
        }
        if (!first) dt = clock_dt(dt, dev_ok, host);
        if (dt > 0 && dt < 0.1) {
            fuse_gyro(raw, dt);
            hs.fused++, hs.total++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- DSU (all systems) and the Steam Deck's HID node (Linux)
// Controls' motion_dsu_axes (or SAVAGEWIND_DSU_AXES): the server's three turns as the game should take them, pitch,
// yaw and roll; a server that packs them in another order or sign ("pitch roll -yaw") is set right here (#136: a
// relay on a Legion Go sent roll in yaw's place).
static int dsu_axis[3] = {0, 1, 2}, dsu_sign[3] = {1, 1, 1};

static void dsu_axes_parse(void) {
    const char *s = getenv("SAVAGEWIND_DSU_AXES");
    if (!s || !*s) s = g_settings.motion_dsu_axes;
    if (!s || !*s) return;
    static const char *const names[3] = {"pitch", "yaw", "roll"};
    int axis[3], sign[3], k = 0, used = 0;
    for (const char *p = s; *p && k <= 3;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        int sg = 1, a = -1;
        if (*p == '-' || *p == '+') sg = *p == '-' ? -1 : 1, p++;
        for (int i = 0; i < 3; i++)
            if (!strncmp(p, names[i], strlen(names[i]))) a = i;
        if (a < 0 || k == 3 || used >> a & 1) { k = -1; break; }
        used |= 1 << a;
        axis[k] = a, sign[k] = sg, k++;
        p += strlen(names[a]);
    }
    if (k != 3) {
        LOG("[dsu] motion_dsu_axes = %s: not three of pitch, yaw, roll (each once, - flips one); kept as they come\n", s);
        return;
    }
    memcpy(dsu_axis, axis, sizeof axis), memcpy(dsu_sign, sign, sizeof sign);
    LOG("[dsu] the server's axes taken as %s\n", s);
}

static void dsu_sample(const DsuPacket *p, uint32_t generation, void *user) {
    (void)user;
    if (native_first(plat_now_ns()) && g_settings.motion_source != MOTION_SOURCE_DSU) return;
    if (backend.dsu_generation != generation && backend.source == SRC_DSU) select_motion(SRC_NONE);
    backend.dsu_generation = generation;
    select_motion(SRC_DSU);
    sensor_accel(p->acc);
    // (the packet's turns back in the server's order - pitch, yaw, roll - for motion_dsu_axes)
    double t[3] = {p->gyro[0], -p->gyro[2], p->gyro[1]}, o[3];
    for (int i = 0; i < 3; i++) o[i] = dsu_sign[i] * t[dsu_axis[i]];
    double gyro[3] = {o[0], o[2], -o[1]};
    // Microseconds -> ns, without overflowing on a malformed timestamp.
    sensor_gyro(gyro, p->timestamp <= UINT64_MAX / 1000 ? p->timestamp * 1000 : 0);
}

#ifdef __linux__
static void deck_sample(const DsuPacket *p, uint32_t generation, void *user) {
    (void)generation, (void)user;
    select_motion(SRC_DECK);
    sensor_accel(p->acc);
    sensor_gyro(p->gyro, p->timestamp * 1000);
}
#endif

// ---- a PC handheld's own sensors (SDL's sensor API; Windows: the system's sensor framework)
// Opened once, the first time a handheld has nothing better (SAVAGEWIND_DEVICE_SENSORS=0: never). Called under g_lock.
static void device_open(void) {
    if (backend.dev_tried) return;
    backend.dev_tried = true;
    const char *env = getenv("SAVAGEWIND_DEVICE_SENSORS");
    if (env && !atoi(env)) return;
    if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) {
        LOG("[motion] the handheld's own sensors: SDL's sensors unavailable (%s)\n", SDL_GetError());
        return;
    }
    int n = 0;
    SDL_SensorID *ids = SDL_GetSensors(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
        if (type == SDL_SENSOR_ACCEL && !backend.dev_acc) backend.dev_acc = SDL_OpenSensor(ids[i]), backend.dev_acc_id = ids[i];
        if (type == SDL_SENSOR_GYRO && !backend.dev_gyro) backend.dev_gyro = SDL_OpenSensor(ids[i]), backend.dev_gyro_id = ids[i];
    }
    SDL_free(ids);
    if (backend.dev_acc && backend.dev_gyro) {
        LOG("[motion] the handheld's own sensors: %s and %s\n", SDL_GetSensorName(backend.dev_acc),
            SDL_GetSensorName(backend.dev_gyro));
        return;
    }
    LOG("[motion] the handheld's own sensors: %s (%d sensor%s); a DSU server (Handheld Companion...) can give them\n",
        backend.dev_acc ? "no gyroscope" : backend.dev_gyro ? "no accelerometer" : "none the system shows", n,
        n == 1 ? "" : "s");
    if (backend.dev_acc) SDL_CloseSensor(backend.dev_acc);
    if (backend.dev_gyro) SDL_CloseSensor(backend.dev_gyro);
    backend.dev_acc = backend.dev_gyro = NULL;
}

void motion_device_sensor_event(const SDL_Event *e) {
    const float *d = e->sensor.data;
    if (!isfinite(d[0]) || !isfinite(d[1]) || !isfinite(d[2])) return;
    pthread_mutex_lock(&g_lock);
    bool acc = backend.dev_acc && e->sensor.which == backend.dev_acc_id;
    bool gyro = backend.dev_gyro && e->sensor.which == backend.dev_gyro_id;
    uint64_t now = plat_now_ns(), ts = e->sensor.sensor_timestamp ? e->sensor.sensor_timestamp : e->sensor.timestamp;
    if (acc) {
        backend.dev_acc_time = now;
        memcpy(backend.dev_acc_v, d, sizeof backend.dev_acc_v);
        double a[3] = {-d[0] / SDL_STANDARD_GRAVITY, -d[1] / SDL_STANDARD_GRAVITY, -d[2] / SDL_STANDARD_GRAVITY};
        if (backend.source == SRC_DEVICE) sensor_accel(a);
    } else if (gyro) {
        backend.dev_gyro_time = now;
        double w[3] = {d[0], d[1], d[2]};
        if (backend.source == SRC_DEVICE) sensor_gyro(w, ts);
    }
    pthread_mutex_unlock(&g_lock);
}

void motion_poll(void) {
    static bool initialized, dsu_force;
#ifdef __linux__
    static bool deck_on;
#endif
    if (!initialized) {
        initialized = true;
#ifdef __linux__
        // SAVAGEWIND_DECK_IMU=0: not the Steam Deck's own sensors
        deck_on = !getenv("SAVAGEWIND_DECK_IMU") || atoi(getenv("SAVAGEWIND_DECK_IMU"));
        deck_imu_init(&backend.deck);
#endif
        const char *mode = getenv("SAVAGEWIND_DSU");
        bool enabled = !mode || strcmp(mode, "off");
        dsu_force = mode && !strcmp(mode, "on");
        if (mode && strcmp(mode, "auto") && strcmp(mode, "off") && strcmp(mode, "on"))
            LOG("[dsu] unknown SAVAGEWIND_DSU=%s; using auto\n", mode);
        if (enabled) backend.dsu_on = dsu_init(&backend.dsu, getenv("SAVAGEWIND_DSU_ENDPOINT"), (uint32_t)plat_now_ns());
        else LOG("[dsu] disabled (SAVAGEWIND_DSU=off)\n");
        dsu_axes_parse();
    }
    int setting = g_settings.motion_source;
    bool force = dsu_force || setting == MOTION_SOURCE_DSU;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    if (now - hs.rate_ns >= 1000000000ull) {  // (the readings per second, for the Controls page)
        hs.rate = hs.rate_ns && now > hs.rate_ns ? (float)((hs.total - hs.rate_total) * 1e9 / (double)(now - hs.rate_ns)) : 0;
        hs.rate_total = hs.total, hs.rate_ns = now;
    }
    bool off = setting == MOTION_SOURCE_OFF;
    bool native = native_first(now) && !off && setting != MOTION_SOURCE_DSU;
    if (backend.source == SRC_PAD && !native) {
        if (native_fresh(now) && !off && setting != MOTION_SOURCE_DSU)
            LOG("[motion] a DSU server answers: it comes before %s's sensor, which has no rate\n", backend.pad_name);
        else
            LOG("[motion] SDL motion timeout; trying fallback\n");
        select_motion(SRC_NONE);
    }
    bool deck_fresh = false;
#ifdef __linux__
    // With Steam Input's gamepad (no sensors), the Deck's own sensors, unless DSU is asked for (SAVAGEWIND_DSU=on)
    bool deck = deck_on && !native && !force && backend.steam_pad && setting == MOTION_SOURCE_AUTO;
    if (deck) deck_imu_poll(&backend.deck, now, deck_sample, NULL);
    else if (backend.deck.fd >= 0) deck_imu_close(&backend.deck);
    if (backend.source == SRC_DECK && (!backend.deck.fresh || !deck)) select_motion(SRC_NONE);
    deck_fresh = deck && backend.deck.fresh;
#endif
    bool dsu = backend.dsu_on && (setting == MOTION_SOURCE_AUTO || setting == MOTION_SOURCE_DSU) &&
               (force || backend.unrated || (!native && !deck_fresh));
    if (dsu) dsu_poll(&backend.dsu, now, dsu_sample, NULL);
    else if (backend.dsu_on && backend.dsu.fd >= 0) dsu_close(&backend.dsu);
    if (backend.source == SRC_DSU && (!backend.dsu.fresh || !dsu)) select_motion(SRC_NONE);
    // A PC handheld's own sensors, when nothing else gives motion
    bool dev = setting == MOTION_SOURCE_AUTO && !native && !deck_fresh && !(dsu && backend.dsu.fresh) && settings_handheld();
    if (dev) device_open();
    bool dev_fresh = dev && backend.dev_acc && backend.dev_acc_time && backend.dev_gyro_time &&
                     now - backend.dev_acc_time < DSU_TIMEOUT_NS && now - backend.dev_gyro_time < DSU_TIMEOUT_NS;
    if (dev_fresh && backend.source == SRC_NONE) {
        select_motion(SRC_DEVICE);
        const float *d = backend.dev_acc_v;
        double a[3] = {-d[0] / SDL_STANDARD_GRAVITY, -d[1] / SDL_STANDARD_GRAVITY, -d[2] / SDL_STANDARD_GRAVITY};
        sensor_accel(a);
    }
    if (backend.source == SRC_DEVICE && !dev_fresh) select_motion(SRC_NONE);
    if (off && backend.source != SRC_NONE) select_motion(SRC_NONE);
    pthread_mutex_unlock(&g_lock);
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
    Quat sensor_q, output_q;     // physical pose last consumed, and delivered pose (no Euler singularity)
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

// The virtual controller's horizontal turn with sensitivity and inversion applied.
static double game_turn(double turn) { return sens() * (g_settings.motion_invert & MOTION_INVERT_LR ? -turn : turn); }
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

// The sticks at a shrine apparatus (runtime/hle/input.c): the right stick turns the table (its X) and tilts it
// front / back (its Y), the left stick banks it (X) and tilts it front / back too (Y); 100 degrees per second at full
// tilt, times the motion sensitivity; the inversion options flip them. reset: recenter. With a gyroscope they take over
// once pushed past half way (a gyroscope lying still, a handheld's on its dock, left no way to solve it), until the
// recenter or the end of the apparatus gives it back.
void motion_dualstick(float lx, float ly, float rx, float ry, double dt, bool reset) {
    if (!isfinite(lx) || !isfinite(ly) || !isfinite(rx) || !isfinite(ry)) return;
    pthread_mutex_lock(&g_lock);
    if (!reset && g.sensors && !g.keyboard.active && !g.tilt_pitch && !g.tilt_roll && !g.tilt_yaw &&
        fmax(fmax(fabs(lx), fabs(ly)), fmax(fabs(rx), fabs(ry))) < 0.5) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
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
        // (a shrine apparatus: the controller as it is held, no sensitivity or lean; a change of source for the game)
        bool apparatus = pc_gyro_keyboard_active();
        quat_mat(apparatus ? g.q : g.aim_q, m);
        return (int)(backend.epoch & 0x3fffffff) * 2 + 1 + apparatus;
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

// Physical motion is an incremental rotation in controller axes. Euler yaw/roll become
// ambiguous at vertical, and clamping Euler pitch makes an overhead controller stop
// responding. Keep the full pose; sensitivity/inversion was already applied at sensor rate.
static void sensor_samples(int kind, uint64_t n, Quat pose) {
    Quat current = qnorm(pose);
    double v[3] = {0};
    if (kind != s.src) {
        Mat m;
        quat_mat(current, m);
        double yaw, pitch, roll;
        euler_of(m, &yaw, &pitch, &roll);
        Quat heading = euler_quat(s.yaw - yaw, 0, 0);
        s.output_q = qnorm(qmul(heading, current));
    } else {
        Quat prev = s.sensor_q;
        Quat delta = qnorm(qmul((Quat){prev.w, -prev.x, -prev.y, -prev.z}, current));
        if (delta.w < 0) delta = (Quat){-delta.w, -delta.x, -delta.y, -delta.z};
        double sn = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        double k = sn > 1e-12 ? 2 * atan2(sn, delta.w) / sn : 2;
        v[0] = delta.x * k;
        v[1] = delta.y * k;
        v[2] = delta.z * k;
    }
    double a = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    double h = a / (2 * n), k = a > 1e-12 ? sin(h) / a : 1.0 / (2 * n);
    Quat step = {cos(h), v[0] * k, v[1] * k, v[2] * k};
    double gyro[3];
    for (int i = 0; i < 3; i++) gyro[i] = v[i] / (n * (SAMPLE_NS / 1e9) * 2 * PI);
    for (uint64_t j = 0; j < n; j++) {
        s.output_q = qnorm(qmul(s.output_q, step));
        quat_mat(s.output_q, s.m);
        for (int i = 0; i < 3; i++) s.angle[i] += gyro[i] * (SAMPLE_NS / 1e9);
        s.head = (s.head + 1) % NSAMPLES;
        SixAxisState *st = &s.ring[s.head];
        st->sampling_number = ++s.sampling;
        fill(st, s.m, gyro, g.lin);
    }
    s.sensor_q = current;
    s.src = kind;
    euler_of(s.m, &s.yaw, &s.pitch, &s.roll);  // diagnostics / transition to virtual controls only
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
        euler_of(m, &s.src_yaw, &s.pitch, &s.roll);
        if (!s.src) s.src_yaw = virtual_yaw(), s.pitch = virtual_pitch(), s.roll = virtual_roll();
        s.yaw = 0;
        euler_mat(s.yaw, s.pitch, s.roll, s.m);
        if (s.src) {
            s.sensor_q = qnorm(s.src & 1 ? g.aim_q : g.q);
            s.output_q = qnorm(qmul(euler_quat(-s.src_yaw, 0, 0), s.sensor_q));
            quat_mat(s.output_q, s.m);
        }
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
    if (kind) {
        sensor_samples(kind, n, kind & 1 ? g.aim_q : g.q);
        goto motion_log;
    }
    double yaw = virtual_yaw(), pitch = virtual_pitch(), roll = virtual_roll();
    s.src = 0;
    // the virtual controller goes to its pose at a bounded speed, every turn of the way (pc_gyro_follow)
    double virtual_now[3] = {s.yaw, s.pitch, s.roll}, virtual_target[3] = {game_turn(yaw), pitch, roll};
    if (g.recenter) {  // (the recenter key: the nearest level pose, not back through every turn made)
        for (int i = 0; i < 3; i++) virtual_now[i] = wrap(virtual_now[i]);
        g.recenter = false;
    }
    const double *lin = zero;
    for (uint64_t j = 1; j <= n; j++) {  // (in between: the way there in equal steps)
        Mat m;
        pc_gyro_follow(virtual_now, virtual_target, SAMPLE_NS / 1e9);
        euler_mat(virtual_now[0], virtual_now[1], virtual_now[2], m);
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
    s.yaw = virtual_now[0], s.pitch = virtual_now[1], s.roll = virtual_now[2];
motion_log:
    if (ENV("SAVAGEWIND_MOTION_LOG") && now - s.logged > 1000000000ull) {
        s.logged = now;
        const SixAxisState *st = &s.ring[s.head];
        LOG("[motion] %s: yaw %.1f pitch %.1f roll %.1f deg | gyro %.3f %.3f %.3f turns/s | acc %.2f %.2f %.2f G | "
            "bias %.2f %.2f %.2f deg/s (spread %.4f rad/s, %.4f G) | sample %lld | readings %u gyro %u accel, "
            "%u fused, %u stalled %u jumped %u repeated, longest gap %.0f ms, last %.0f ms ago | game reads %u\n",
            kind ? "sensors" : g.keyboard.active ? "virtual gyro" : g.tilt_held || g.tilt_pitch || g.tilt_roll ? "mouse tilt"
                                                                                                    : "still/script",
            s.yaw * 180 / PI, s.pitch * 180 / PI, s.roll * 180 / PI, st->gyro[0], st->gyro[1], st->gyro[2], st->acc[0],
            st->acc[1], st->acc[2], g.bias[0] * 180 / PI, g.bias[1] * 180 / PI, g.bias[2] * 180 / PI, g.gyro_sd,
            sqrt(g.acc_var), (long long)st->sampling_number, hs.gyro, hs.accel, hs.fused, hs.stalled, hs.jumped, hs.repeated,
            hs.max_gap / 1e6, hs.host_ns ? (double)(SDL_GetTicksNS() - hs.host_ns) / 1e6 : -1.0, hs.reads);
        hs.gyro = hs.accel = hs.fused = hs.stalled = hs.jumped = hs.repeated = hs.reads = 0, hs.max_gap = 0;
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
