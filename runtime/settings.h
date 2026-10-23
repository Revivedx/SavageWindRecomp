// User settings (savagewind.ini in the data root, runtime/paths.h), edited from the in-game menu.
#pragma once
#include <stdbool.h>

// Upscalers (Video > Upscaler; runtime/nvn/output_scale.c): Off renders at the window's size; the others render
// smaller by their quality mode's factor and scale up to the window. DLSS is listed as coming later.
enum { UPSCALE_OFF = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1, Vulkan */,
       UPSCALE_FSR3 = 3 /* AMD FSR 3.1, Vulkan (Windows: amd_fidelityfx_vk.dll) */, UPSCALE_DLSS = 4 /* NVIDIA DLSS: not yet */,
       UPSCALE_COUNT };
#ifdef __APPLE__
#define UPSCALE_SPATIAL UPSCALE_METALFX  // each OS's spatial upscaler
#define UPSCALE_DEFAULT UPSCALE_METALFX  // (Metal has no internal resolution yet: MetalFX scales the image to the window)
#else
#define UPSCALE_SPATIAL UPSCALE_FSR1
#define UPSCALE_DEFAULT UPSCALE_OFF      // the 3D image at the window's resolution
#endif
// quality modes, AMD's scale factors (per axis): Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0, and
// for FSR 3.1 also Native AA 1.0 (anti-aliasing only) and Ultra Performance 3.0. (The numbers are the settings file's
// order; the menus list them from the sharpest: upscale_mode_order.)
enum { UPQ_ULTRA = 0, UPQ_QUALITY = 1, UPQ_BALANCED = 2, UPQ_PERFORMANCE = 3, UPQ_NATIVE = 4, UPQ_ULTRA_PERF = 5, UPQ_COUNT };
// Anti-aliasing (Video > Anti-aliasing): SMAA on the finished image, TAA on the 3D scene (jittered, with its history
// along the motion vectors: runtime/nvn/vulkan/exec.c). With FSR 3.1 the upscaler anti-aliases and this is not used.
enum { AA_OFF = 0, AA_SMAA = 1, AA_TAA = 2, AA_COUNT };
// Frame generation (F12 > Graphics; runtime/nvn/vulkan/exec.c "frame generation"): images drawn per game frame.
enum { FG_OFF = 0, FG_MAX = 4 };
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
    bool async_shaders;  // draw on while new shaders compile (no stutter; what needs them is skipped meanwhile)
    int text_language;   // index in g_text_languages (runtime/language.h), 0: the host's
    int voice_language;  // index in g_voice_languages, 0: the text language's voices
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    float fsr_sharpness; // FSR 3.1's sharpening, 0..1 (AMD's RCAS strength)
    int frame_gen;       // FG_OFF, or 2..FG_MAX images per game frame (the in-between ones generated)
    bool mv_debug;       // the motion vectors shown in place of the 3D image (debugging view, not saved)
} Settings;

#define MOUSE_SENS_DEFAULT 3.0f  // (300% of the first default, 1; user, 2026-10-19)

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
