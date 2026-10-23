// Frame rate (Video > Frame rate): Off (no limit), 30, 60 (the default), 90 or 120 frames per second, and the game's
// clock the same at every one of them. Shared by the macOS, Linux and Windows builds.
//
// How BOTW 1.8.2 keeps time (main module, runtime addresses):
//   - Its frame loop (sead GameFrameworkNx, 0x700101287c) asks for a present interval of 2 vsyncs (30 fps), 3 (20 fps)
//     after three slow frames in a row. The present (nvnQueuePresentTexture: objects.c present_pace) keeps that pace;
//     the frame loop's own vsync wait (0x7000d811fc) only runs for the first frames after start-up.
//   - ksys::VFR (the instance at [0x7001d32010]) turns that interval into the game's time step: initialised with a
//     base interval of 2, so one "delta frame" is 1/30 s. Every frame VFR::updateInterval (0x7001409230) sets the
//     interval ratio (interval / 2: 1.0 at 30 fps, 1.5 at 20 fps), and delta frame = ratio x the slowest active time
//     multiplier (slow motion), delta time = delta frame / 30. About 1600 call sites read them: the time of day
//     (TimeMgr adds its step x delta frame), the Blood Moon timer, the Havok step (the physics world is handed delta
//     time every frame), stamina, animation, AI. A few read the interval itself.
//   - The menus' input timing counts frames: holding a direction on a value steps it every 6 frames on the title
//     screen (0.2 s at 30 fps, 0.1 s at 120). The paused game (pause menu, map: the state at 0x7001cb48a4 is 2, 0
//     while playing) updates VFR twice a frame.
// So the game already runs at any frame rate as long as VFR is told how long a frame really took. Savage Wind:
//   - paces the presents at the chosen rate on a steady grid (present_pace, framepace_period_ns; Off: 300 fps at
//     most, FPS_CEILING), and skips the frame loop's vsync wait (rt_frame_wait, tools/recomp/patches.py);
//   - tells VFR how long the frame really took (rt_vfr_update, inside updateInterval after the game set its ratio):
//     ratio = seconds x 30 (0.5 at 60 fps, 0.25 at 120): one period when the frames keep the pace, the measured time
//     averaged otherwise, changing by a few percent a frame at most, and whatever it gave more or less than the real
//     time is paid back over the next frames, so the game's time follows the real time. The interval follows the frame rate averaged over ~20 frames: 1
//     above ~43 fps (the game's own 60 fps branches, made for its debug viewer), 2 below ~35, 3 below ~23. A step is 1/20 s at most (ratio 1.5, the console's longest):
//     a hitch is paid back over the next frames, but frames slower than that for long (over 1/4 s owed) slow the
//     game down, as on the console, instead of taking steps big enough to break the physics;
//   - holds the frame rate at 30 in the menus (the title screen, the pause menu and the map) and the events
//     (cutscenes, talks: some of their waits count frames), whose timing is the console's, and while a movie plays
//     (movie.c: the pre-rendered cutscenes show a picture per frame);
//   - steps the controllers' hold counters by the frame's length (CONTROLLER_STEP, below).
// Test and debug switches:
//   SAVAGEWIND_FPS=<n>             frame rate instead of the setting (0: no limit)
//   SAVAGEWIND_FRAMEPACE=0         the game's own pacing and clock (its 30 / 20 fps, as before)
//   SAVAGEWIND_MOVIE_FPS=<n>       frame rate while a movie plays (default 30; 0: the chosen one)
//   SAVAGEWIND_MENU_FPS=<n>        frame rate in the menus (default 30; 0: the chosen one)
//   SAVAGEWIND_EVENT_FPS=<n>       frame rate in the events (default 30; 0: the chosen one)
//   SAVAGEWIND_LOG_FRAMEPACE=<n>   every n clock updates: target, frame length, VFR ratio and interval
//   SAVAGEWIND_NO_CONTROLLER_STEP=1  the controllers count presented frames (as the game does)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../framepace.h"

extern volatile int g_movie_playing;  // runtime/hle/movie.c: video decoders running

#define VF(T, base, off) (*(T *)(uintptr_t)((base) + (off)))
#define PAUSE_STATE 0x7001cb48a4ull  // (u32: 0 playing, 2 in the pause menu or the map)

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

bool framepace_on(void) {
    static int on = -1;
    if (on < 0) on = env_int("SAVAGEWIND_FRAMEPACE", 1) != 0;
    return on;
}

// ------------------------------------------------------------------ the frame rate
// The menus: the game is paused (pause menu, map), or the title screen's menu is up (screen 1 in the UI screen
// manager [0x7001c05d88]: +0x28 count, +0x30 screens by id).
static bool in_menu(void) {
    if (*(uint32_t *)(uintptr_t)PAUSE_STATE) return true;
    uint64_t mgr = *(uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)0x7001c05d88ull;
    return mgr && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
           *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
}

// An event is running: a cutscene, a talk, an item fanfare (ksys::evt::Manager [0x7001cd8db8]: its active event at
// +0x1d2b8, as hasActiveEvent 0x7000ffa208 reads it; the time of day stands still meanwhile). Some of their waits
// count frames: the new game's opening gave Link control 2 s early at 60 fps, 3 s at 120.
static bool in_event(void) {
    uint64_t evt = *(uint64_t *)(uintptr_t)0x7001cd8db8ull;
    return evt && *(uint64_t *)(uintptr_t)(evt + 0x1d2b8);
}

int framepace_target(void) {
    static int forced = -2, movie = -2, menu = -2, event = -2;
    if (forced == -2) forced = env_int("SAVAGEWIND_FPS", -1);
    if (movie == -2) movie = env_int("SAVAGEWIND_MOVIE_FPS", 30);
    if (menu == -2) menu = env_int("SAVAGEWIND_MENU_FPS", 30);
    if (event == -2) event = env_int("SAVAGEWIND_EVENT_FPS", 30);
    int fps = forced >= 0 ? forced : g_settings.fps_limit;
    if (fps < 0) fps = FPS_DEFAULT;
    if (movie > 0 && __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED) > 0 && (fps == 0 || fps > movie)) fps = movie;
    if (menu > 0 && (fps == 0 || fps > menu) && in_menu()) fps = menu;
    if (event > 0 && (fps == 0 || fps > event) && in_event()) fps = event;
    return fps;
}

// No limit still stops at 300 fps: shorter steps lose precision in the game's float clocks (the time of day, 0..360,
// takes 1/30 of a minute times the step each frame: at 300 fps that is 1% of the value's rounding unit per frame).
#define FPS_CEILING 300

uint64_t framepace_period_ns(void) {
    int fps = framepace_target();
    return 1000000000ull / (uint64_t)(fps > 0 ? fps : FPS_CEILING);
}

// The frame loop's vsync wait: the present keeps the pace instead (1: skipped; 0: the game's own wait).
int rt_frame_wait(void) { return framepace_on(); }

// ------------------------------------------------------------------ presents
// The controllers (sead::ControllerBase::updateDerivativeParams_, 0x7000d9d8a0) count how long each button has been
// held, in frames: a held button's repeat starts after its delay and comes every pulse frames, and the game times
// some holds with the count. The count goes up by a step the game keeps at 1 (a float at 0x7001c4ba40, read there
// only): the runtime sets it to the frame's length in console frames, so holds last as long as on the console.
#define CONTROLLER_STEP 0x7001c4ba40ull
static float g_step = 1;         // the last frame's length between presents, in console frames
static int g_grid_streak;        // presents in a row that kept the pace

void framepace_presented(uint64_t t, bool on_grid) {
    if (!framepace_on()) return;
    static uint64_t prev;
    static double smooth;
    double s = prev && t > prev ? (t - prev) / 1e9 : 1.0 / 30;
    prev = t;
    if (s > 0.25) s = 0.25;
    smooth = smooth > 0 ? smooth + (s - smooth) * 0.25 : s;
    float step = (float)((on_grid ? s : smooth) * 30);
    g_step = step < 0.1f ? 0.1f : step > 1.5f ? 1.5f : step;
    g_grid_streak = on_grid ? g_grid_streak + 1 : 0;
    static int fixed = -1;
    if (fixed < 0) fixed = env_int("SAVAGEWIND_NO_CONTROLLER_STEP", 0);
    if (!fixed) *(float *)(uintptr_t)CONTROLLER_STEP = g_step;
}

// ------------------------------------------------------------------ the game's clock (ksys::VFR)
// VFR fields (1.8.2; the layout of zeldaret's ksys::VFR): +0x28 interval changed, +0x29 interval override on,
// +0x30 new interval ratio, +0x34 interval, +0x3c interval ratio (both read through per-core pointers).
#define VFR_CHANGED 0x28
#define VFR_OVERRIDE 0x29
#define VFR_NEW_RATIO 0x30
#define VFR_INTERVAL 0x34
#define VFR_RATIO 0x3c

static void clock_probe(uint64_t vfr);
static void log_screens(void);

void rt_vfr_update(uint64_t vfr) {
    extern bool nvn_timeline(void);
    if (nvn_timeline()) LOG("[tl] %.3f %x vfr\n", plat_now_ns() / 1e6, kernel_current_thread_handle());
    extern uint64_t g_fs_vfr;  // (SAVAGEWIND_FRAME_LOG=2, nvn/objects.c)
    g_fs_vfr = plat_now_ns();
    clock_probe(vfr);
    log_screens();
    if (!framepace_on()) return;
    static uint64_t last_t;
    static double real, given;  // seconds of real time and of game time handed out (only their gap matters)
    static double smooth;       // seconds between updates, averaged over about 20
    static double last_step;    // the step handed out last, seconds (0: none yet)
    static int last_target = -1;
    static double slow = 1;     // the step in console frames, averaged over about 20 (the interval follows it)
    static uint32_t last_iv;
    static int log_every = -1;
    static uint64_t nframe;
    if (log_every < 0) log_every = env_int("SAVAGEWIND_LOG_FRAMEPACE", 0);
    uint64_t now = plat_now_ns();
    double s = last_t && now > last_t ? (now - last_t) / 1e9 : 1.0 / 30;
    last_t = now;
    float ratio;
    bool paused = *(uint32_t *)(uintptr_t)PAUSE_STATE != 0;
    if (paused) {
        // paused (it updates twice a frame): the frame's length between presents, nothing owed
        ratio = g_step;
        real = given = 0;
        last_target = -1;
    } else {
        if (s > 0.25) s = 0.25;  // (a load or a pause in a debugger: not owed)
        double period = framepace_period_ns() / 1e9;
        int target = framepace_target();
        if (target != last_target) smooth = period, last_step = 0;  // (a new pace: start from it)
        last_target = target;
        smooth += (s - smooth) * 0.05;
        real += s;
        // The step changes slowly: the game reads some deltas a frame late, and a step that jumps between frames
        // moves the camera and Link apart (Link seemed to step back in place, seen from the side). One period once
        // the presents have kept the pace for half a second, else the average of ~20 frames; plus a fiftieth of what
        // is owed; and at most 3% away from the last step.
        double step = (g_grid_streak >= 30 ? period : smooth) + (real - given) * 0.02;
        if (last_step > 0 && step > last_step * 1.03) step = last_step * 1.03;
        if (last_step > 0 && step < last_step * 0.97) step = last_step * 0.97;
        if (step < 1.0 / FPS_CEILING) step = 1.0 / FPS_CEILING;
        if (step > 0.05) step = 0.05;
        last_step = step;
        ratio = (float)(step * 30);
    }
    bool held = VF(uint8_t, vfr, VFR_OVERRIDE) && ratio > 1;  // (stage loading: the game asks for single steps)
    if (held) ratio = 1;
    if (!paused) {
        given += ratio / 30.0;
        if (held || fabs(real - given) > 0.25) real = given;  // (nothing owed for a deliberate slow down)
    }
    // The interval switches some of the game's code between its 60 fps and 30 fps ways: only on a lasting change of
    // the frame rate (above ~43 fps: 1; below ~35: 2; below ~23: 3), never back and forth from one frame to the next.
    slow += (ratio - slow) * 0.05;
    static uint32_t iv = 2;
    if (iv == 1 && slow > 0.85) iv = 2;
    else if (iv == 2 && slow < 0.7) iv = 1;
    else if (iv == 2 && slow > 1.3) iv = 3;
    else if (iv == 3 && slow < 1.15) iv = 2;
    VF(float, vfr, VFR_RATIO) = ratio;
    VF(float, vfr, VFR_NEW_RATIO) = ratio;
    VF(uint32_t, vfr, VFR_INTERVAL) = iv;
    VF(uint8_t, vfr, VFR_CHANGED) = last_iv && iv != last_iv;
    last_iv = iv;
    if (log_every > 0 && nframe++ % (uint64_t)log_every == 0) {
        uint64_t pinfo = *(uint64_t *)(uintptr_t)0x7001cb2658ull;  // (Link's position: PlayerInfo +0x88)
        LOG("[framepace] target %d fps, %.2f ms since the last update (avg %.2f), ratio %.3f, interval %u, owed "
            "%.2f ms, Link %.3f %.3f%s\n", framepace_target(), s * 1e3, smooth * 1e3, ratio, iv, (real - given) * 1e3,
            pinfo ? VF(float, pinfo, 0x88) : 0.0f, pinfo ? VF(float, pinfo, 0x90) : 0.0f,
            g_movie_playing > 0 ? ", movie" : "");
    }
}

// ------------------------------------------------------------------ test scripts, the clock probe, the screens
// SAVAGEWIND_SCRIPT_CLOCK=1: SAVAGEWIND_INPUT and SAVAGEWIND_SHOT_AT count 30ths of a second (the console's frames)
// instead of presented frames, so one script drives the game the same way at any frame rate: each presented frame
// adds its length x 30, at most 1.5 (a load's long frames count like the console's).
uint64_t framepace_script_frame(uint64_t presented) {
    static int on = -1;
    static double t;
    static uint64_t last;
    if (on < 0) on = env_int("SAVAGEWIND_SCRIPT_CLOCK", 0) != 0;
    if (!on) return presented;
    uint64_t now = plat_now_ns();
    double units = last ? (now - last) / 1e9 * 30 : 1;
    last = now;
    t += units > 1.5 ? 1.5 : units;
    {   // (debug) SAVAGEWIND_DUMP_DATA_AT=<frame>,...: main's .data and .bss -> build/data_<frame>.bin
        static const char *next;
        static int init;
        if (!init) init = 1, next = getenv("SAVAGEWIND_DUMP_DATA_AT");
        if (next && *next && (uint64_t)t >= strtoull(next, NULL, 10)) {
            char path[64];
            snprintf(path, sizeof path, "build/data_%llu.bin", (unsigned long long)t);
            FILE *f = fopen(path, "wb");
            if (f) fwrite((void *)(uintptr_t)0x7001a1f000ull, 1, 0x7001d37000ull - 0x7001a1f000ull, f), fclose(f);
            next = strchr(next, ',');
            if (next) next++;
        }
    }
    return (uint64_t)t;
}

// SAVAGEWIND_CLOCK_PROBE=<ms>: every <ms> of real time, a "[clock]" line with what the frame rate must not change:
// seconds since the probe started, frames, the game's own time (its delta frames summed, 1/30 s each, slow motion
// included), the time of day (TimeMgr, 0..360 = 24 h: one in-game minute is 0.25), Link's position and stamina
// (1000 a wheel). 1.8.2 addresses: ksys::world::Manager [0x7001cf5d00] (+0x5c0 count, +0x5c8 its managers; [0] =
// TimeMgr, the time at +0xb8); uking::act::PlayerInfo [0x7001cb2658] (+0x88 Link's position, +0xa4 the stamina).
static void clock_probe(uint64_t vfr) {
    static int every = -1;
    static uint64_t t0, next, frames;
    static double game;
    if (every < 0) every = env_int("SAVAGEWIND_CLOCK_PROBE", 0);
    if (every <= 0) return;
    frames++;
    game += VF(float, vfr, 0x98);  // (core 0's delta frame of the frame that just ended)
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now, next = now;
    if (now < next) return;
    next += (uint64_t)every * 1000000ull;
    if (next < now) next = now + (uint64_t)every * 1000000ull;
    float tod = -1, pos[3] = {0, 0, 0}, stamina = -1;
    uint64_t wm = *(uint64_t *)(uintptr_t)0x7001cf5d00ull;
    if (wm && VF(uint32_t, wm, 0x5c0)) {
        uint64_t mgrs = VF(uint64_t, wm, 0x5c8), tm = mgrs ? *(uint64_t *)(uintptr_t)mgrs : 0;
        if (tm) tod = VF(float, tm, 0xb8);
    }
    uint64_t pinfo = *(uint64_t *)(uintptr_t)0x7001cb2658ull;
    if (pinfo) {
        for (int i = 0; i < 3; i++) pos[i] = VF(float, pinfo, 0x88 + 4 * i);
        stamina = VF(float, pinfo, 0xa4);
    }
    LOG("[clock] t %.3f frames %llu game %.2f tod %.4f pos %.3f %.3f %.3f stamina %.1f fps %d at %.3f\n",
        (now - t0) / 1e9, (unsigned long long)frames, game, tod, pos[0], pos[1], pos[2], stamina, framepace_target(),
        now / 1e9);
}

// SAVAGEWIND_LOG_SCREENS=1: the pause state and the game's UI screens that exist (their ids), each time they change
static void log_screens(void) {
    static int on = -1;
    static char last[1024];
    if (on < 0) on = env_int("SAVAGEWIND_LOG_SCREENS", 0);
    if (!on) return;
    uint64_t mgr = *(uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)0x7001c05d88ull;
    if (!mgr) return;
    uint32_t n = VF(uint32_t, mgr, 0x28);
    uint64_t arr = VF(uint64_t, mgr, 0x30);
    char cur[1024];
    int k = snprintf(cur, sizeof cur, " (pause state %u):", *(uint32_t *)(uintptr_t)0x7001cb48a4ull);
    for (uint32_t i = 0; i < n && i < 256 && arr && k < (int)sizeof cur - 8; i++)
        if (*(uint64_t *)(uintptr_t)(arr + 8 * i)) k += snprintf(cur + k, sizeof cur - k, " %x", i);
    if (strcmp(cur, last)) {
        LOG("[screens]%s\n", cur);
        strcpy(last, cur);
    }
}
