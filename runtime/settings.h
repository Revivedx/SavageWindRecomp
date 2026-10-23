// User settings (savagewind.ini in the data root, runtime/paths.h), edited from the in-game menu.
#pragma once
#include <stdbool.h>

// Upscalers (Video > Upscaler; runtime/nvn/output_scale.c): Off renders at the window's size; the others render
// smaller by their quality mode's factor and scale up to the window. FSR 3 and DLSS are listed as coming later.
enum { UPSCALE_OFF = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1, Vulkan */,
       UPSCALE_FSR3 = 3 /* AMD FSR 3: not yet */, UPSCALE_DLSS = 4 /* NVIDIA DLSS: not yet */, UPSCALE_COUNT };
#ifdef __APPLE__
#define UPSCALE_SPATIAL UPSCALE_METALFX  // each OS's spatial upscaler
#define UPSCALE_DEFAULT UPSCALE_METALFX  // (Metal has no internal resolution yet: MetalFX scales the image to the window)
#else
#define UPSCALE_SPATIAL UPSCALE_FSR1
#define UPSCALE_DEFAULT UPSCALE_OFF      // the 3D image at the window's resolution
#endif
// quality modes, AMD's scale factors (per axis): Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0
enum { UPQ_ULTRA = 0, UPQ_QUALITY = 1, UPQ_BALANCED = 2, UPQ_PERFORMANCE = 3, UPQ_COUNT };
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };
// Frame rate (Video > Frame rate; runtime/hle/framepace.c): frames per second, 0 = no limit. The game's clock is the
// same at every rate. The console runs the game at 30.
#define FPS_DEFAULT 60
#define FPS_CHOICES {0, 30, 60, 90, 120}

typedef struct {
    bool fullscreen;
    int window_w, window_h;  // Video > Window size, pixels (0: not chosen; runtime/window_size.c)
    int upscaler;        // UPSCALE_*: Off (render at the window's size), or render smaller and scale up
    int upscale_quality; // UPQ_*: the upscaler's mode (how much smaller it renders)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    int perf_overlay;    // PERF_*: the performance overlay (F10 cycles it)
    int fps_limit;       // frame rate, frames per second (FPS_CHOICES; 0: no limit)
    bool dpad_move;      // the d-pad (and arrow keys) also act as the left stick while it is at rest
    bool mouse_camera;   // the mouse turns the camera (right stick); the cursor is held in the window while playing
    float mouse_sens;    // mouse camera speed, 1 = 100% (default MOUSE_SENS_DEFAULT)
    bool mouse_invert_y; // mouse up looks down
    float lstick_deadzone, rstick_deadzone;  // controller sticks: tilt ignored, 0..STICK_DEADZONE_MAX (0: the game's own only)
    bool async_shaders;  // draw on while new shaders compile (no stutter; what needs them is skipped meanwhile)
    int text_language;   // index in g_text_languages (runtime/language.h), 0: the host's
    int voice_language;  // index in g_voice_languages, 0: the text language's voices
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
} Settings;

#define MOUSE_SENS_DEFAULT 3.0f  // (300% of the first default, 1; user, 2026-10-19)
#define STICK_DEADZONE_MAX 0.3f  // (past it, walking slowly is hardly possible)

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
