// User settings (savagewind.ini in the data root, runtime/paths.h), edited from the in-game menu.
#pragma once
#include <stdbool.h>

// Upscalers (Video > Upscaler; runtime/nvn/output_scale.c): Off renders at the window's size; the others render
// smaller by their quality mode's factor and scale up to the window. DLSS is listed as coming later.
enum { UPSCALE_OFF = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1, Vulkan */,
       UPSCALE_FSR3 = 3 /* AMD FSR 3.1, Vulkan where built in (vulkan/fsr3.h) */, UPSCALE_DLSS = 4 /* NVIDIA DLSS: not yet */,
       UPSCALE_COUNT };
#ifdef __APPLE__
#define UPSCALE_SPATIAL UPSCALE_METALFX  // each OS's spatial upscaler
#define UPSCALE_FSR3_OR_SPATIAL UPSCALE_METALFX  // (a settings file from another OS)
#define UPSCALE_DEFAULT UPSCALE_METALFX  // (Metal has no internal resolution yet: MetalFX scales the image to the window)
#else
#define UPSCALE_SPATIAL UPSCALE_FSR1
#define UPSCALE_FSR3_OR_SPATIAL UPSCALE_FSR3
#define UPSCALE_DEFAULT UPSCALE_OFF      // the 3D image at the window's resolution
#endif
// quality modes, AMD's scale factors (per axis): Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0,
// Ultra Performance 3.0
enum { UPQ_ULTRA = 0, UPQ_QUALITY = 1, UPQ_BALANCED = 2, UPQ_PERFORMANCE = 3, UPQ_ULTRA_PERFORMANCE = 4, UPQ_COUNT };
// TAA (Vulkan): the game's own motion vectors (vulkan/mvec.c) and a jittered camera (output_scale.c rt_jitter_on)
// blend each frame with the ones before it (vulkan/exec.c mvec_taa)
enum { AA_OFF = 0, AA_SMAA = 1, AA_TAA = 2 };
#ifdef __APPLE__
#define AA_TAA_OR_SMAA AA_SMAA
#else
#define AA_TAA_OR_SMAA AA_TAA
#endif
// Shadow resolution (Video > Shadow resolution; runtime/nvn/output_scale.c shadow_map_size): the sun's shadow maps at
// the console's size (BOTW: 896 x 896) or made bigger, for sharper shadows at some GPU time.
enum { SHADOW_CONSOLE = 0, SHADOW_HIGH = 1 /* 2048 */, SHADOW_ULTRA = 2 /* 4096 */, SHADOW_EXTREME = 3 /* 8192 */, SHADOW_COUNT };
enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };
// Frame rate (Video > Frame rate; runtime/hle/framepace.c): frames per second, 0 = no limit. The game's clock is the
// same at every rate. The console runs the game at 30.
#define FPS_DEFAULT 60
#define FPS_CHOICES {0, 30, 60, 90, 120}
// VSync (Video > VSync; runtime/nvn/vulkan/device.c picks the present mode): On waits for the display (FIFO; with
// G-SYNC / FreeSync the display waits for the game instead), Fast shows the newest frame at each refresh (MAILBOX),
// Adaptive is On but shows a late frame at once (FIFO_RELAXED), Off shows each frame at once (IMMEDIATE). With On and
// Adaptive the frame rate stays a little under the display's refresh rate (runtime/hle/framepace.c).
enum { VSYNC_ON = 0, VSYNC_FAST = 1, VSYNC_ADAPTIVE = 2, VSYNC_OFF = 3, VSYNC_COUNT };
#define VSYNC_DEFAULT VSYNC_FAST
// Video > HDR: Auto presents in HDR10 while Windows HDR is on for the window's monitor, On always (where the display
// takes it), Off never (an SDR picture, which Windows shows dim and dull while its HDR is on)
enum { HDR_AUTO = 0, HDR_ON = 1, HDR_OFF = 2, HDR_COUNT };
// Video > Fullscreen mode (runtime/window_size.c winsize_apply): Automatic is borderless at the desktop's size and the
// display's own mode at any other size it has; Borderless never changes the display's mode (another size is scaled to
// the screen); Exclusive always uses a display mode (the desktop's size too, at its highest refresh rate)
enum { FSMODE_AUTO = 0, FSMODE_BORDERLESS = 1, FSMODE_EXCLUSIVE = 2, FSMODE_COUNT };
typedef struct {
    bool fullscreen;
    int fullscreen_mode;     // FSMODE_*
    // Video > Monitor (runtime/window_size.c): 0 the window's own (where it is; the primary display at start), n the
    // n-th display connected; display_name is its name, which finds it again when the displays come in another order
    int display;
    char display_name[64];
    int window_w, window_h;  // Video > Window size, pixels (0: not chosen; runtime/window_size.c)
    int upscaler;        // UPSCALE_*: Off (render at the window's size), or render smaller and scale up
    int upscale_quality; // UPQ_*: the upscaler's mode (how much smaller it renders)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int anisotropy;      // texture filtering: 0 the game's own, 1..4 = 2x, 4x, 8x, 16x (vulkan/textures.c)
    int sharpening;      // AMD CAS sharpening of the 3D image, 0 (off) .. 10 (100%) (vulkan/exec.c scene_post)
    bool frame_gen;      // AMD FSR 3 frame generation: a frame made between each two the game draws (vulkan/framegen.cpp)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    int perf_overlay;    // PERF_*: the performance overlay (F10 cycles it)
    int fps_limit;       // frame rate, frames per second (FPS_CHOICES; 0: no limit)
    int vsync;           // VSYNC_*
    bool dpad_move;      // the d-pad (and arrow keys) also act as the left stick while it is at rest
    float stick_deadzone[2];  // left, right stick: a controller's stick reads 0 within this much of full tilt (radial)
    bool mouse_camera;   // the mouse turns the camera (right stick); the cursor is held in the window while playing
    bool pc_cursor;      // mouse pointer in menus, inventory, quick selectors and the map
    float mouse_sens;    // mouse camera speed, 1 = 100% (default MOUSE_SENS_DEFAULT)
    float mouse_sens_y;  // vertical mouse camera speed, a fraction of mouse_sens (1: the same as horizontal)
    bool mouse_invert_y; // mouse up looks down
    bool pc_gyro;        // keyboard gyro only during shrine apparatus interaction
    bool gyro_scene;     // left-drag rotation during a shrine apparatus interaction
    float motion_sens;   // motion controls: how far turning the controller turns the aim, 1 = 100%
    int motion_invert;   // MOTION_INVERT_* bits: turning the controller up / down, left / right goes the other way
    int button_icons;    // ICONS_*: the game's button prompts show this device's buttons (Automatic: the one in use)
    bool async_shaders;  // draw on while new shaders compile (no stutter; what needs them is skipped meanwhile)
    bool precompile_shaders;  // setting the game files up also precompiles every shader (runtime/nvn/precompile.h)
    int shadow_res;      // SHADOW_*: shadow maps at the console's size or bigger
    bool wide_view;      // a window that isn't 16:9 shows more of the world instead of bars (runtime/nvn/output_scale.c)
    int hdr;             // HDR_* (runtime/nvn/vulkan/device.c hdr_*)
    int hdr_white;       // nits of SDR white in HDR (0: what Windows gives SDR content on that monitor)
    int fov;             // field of view in play, percent of the game's own (FOV_CHOICES)
    int draw_distance;   // DRAWDIST_*: how far placed actors are kept (runtime/draw_distance.c)
    // Video > Colour (runtime/color_grade.h): 0 everywhere and no LUT = the game's own colours (nothing runs)
    int color_exposure;      // tenths of an EV, -20..20
    int color_contrast;      // percent, -50..50
    int color_saturation;    // percent, -100..100 (-100: black and white)
    int color_temperature;   // -100 (cooler, blue) .. 100 (warmer, orange)
    int color_tint;          // -100 (green) .. 100 (magenta)
    char color_lut[128];     // color_lut_id(): "" none, "sw:<file>" shipped, "<file>" the player's luts/ folder
    int color_lut_strength;  // percent, 0..100
    bool color_lut_set;      // the ini names a LUT (or none): the player chose; the first setup offers COLOR_LUT_RECOMMENDED
    int text_language;   // index in g_text_languages (runtime/language.h), 0: the host's
    int voice_language;  // index in g_voice_languages, 0: the text language's voices
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
} Settings;

#define STICK_DEADZONE_DEFAULT 0.10f  // (the Switch's own input service has one: the game expects 0 at rest)
enum { DRAWDIST_ORIGINAL, DRAWDIST_MEDIUM, DRAWDIST_HIGH, DRAWDIST_COUNT };  // (Original: the game's own distances)
float draw_distance_factor(void);  // the setting's factor on the game's distances (1: Original)
#define FOV_CHOICES {70, 80, 90, 100, 110, 120, 130, 140, 150}  // (percent; 100: the game's own)
#define MOUSE_SENS_DEFAULT 3.0f  // (300% of the first default, 1; user, 2026-10-19)
#define MOUSE_SENS_Y_CHOICES {50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200}  // (percent of the horizontal)
enum { MOTION_INVERT_UD = 1, MOTION_INVERT_LR = 2 };
enum { ICONS_AUTO, ICONS_KEYBOARD, ICONS_XBOX, ICONS_PLAYSTATION, ICONS_NINTENDO, ICONS_COUNT };

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
