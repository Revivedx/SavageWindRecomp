// User settings (savagewind.ini in the data root, runtime/paths.h), edited from the in-game menu.
#pragma once
#include <stdbool.h>

// Upscalers (Video > Upscaler; runtime/nvn/output_scale.c): Off renders at the window's size; the others render
// smaller by their quality mode's factor and scale up to the window. DLSS is listed as coming later.
enum { UPSCALE_OFF = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1, Vulkan */,
       UPSCALE_FSR3 = 3 /* AMD FSR 3.1, Vulkan where built in (vulkan/fsr3.h) */, UPSCALE_DLSS = 4 /* NVIDIA DLSS: not yet */,
       UPSCALE_FSR4 = 5 /* AMD FSR 4, Vulkan on Windows with AMD's DLLs and an AMD Radeon (vulkan/fsr_api.h) */,
       UPSCALE_COUNT };
#ifdef __APPLE__
#define UPSCALE_SPATIAL UPSCALE_METALFX  // each OS's spatial upscaler
#define UPSCALE_FSR3_OR_SPATIAL UPSCALE_METALFX  // (a settings file from another OS)
#define UPSCALE_FSR4_OR_SPATIAL UPSCALE_METALFX
#define UPSCALE_DEFAULT UPSCALE_METALFX  // (Metal has no internal resolution yet: MetalFX scales the image to the window)
#else
#define UPSCALE_SPATIAL UPSCALE_FSR1
#define UPSCALE_FSR3_OR_SPATIAL UPSCALE_FSR3
#define UPSCALE_FSR4_OR_SPATIAL UPSCALE_FSR4  // (without FSR 4 on this PC: vulkan/device.c moves it to FSR 3 or 1)
#define UPSCALE_DEFAULT UPSCALE_OFF      // the 3D image at the window's resolution
#endif
// quality modes, AMD's scale factors (per axis): Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0,
// Ultra Performance 3.0
enum { UPQ_ULTRA = 0, UPQ_QUALITY = 1, UPQ_BALANCED = 2, UPQ_PERFORMANCE = 3, UPQ_ULTRA_PERFORMANCE = 4, UPQ_COUNT };
// TAA (Vulkan): the game's own motion vectors (vulkan/mvec.c) and a jittered camera (output_scale.c rt_jitter_on)
// blend each frame with the ones before it (vulkan/exec.c mvec_taa)
enum { AA_OFF = 0, AA_SMAA = 1, AA_TAA = 2 };
// Video > Anti-aliasing's row: the game's own FXAA (game_fxaa) is one of its choices, never on with SMAA or TAA
enum { AA_ROW_OFF, AA_ROW_FXAA, AA_ROW_SMAA, AA_ROW_TAA, AA_ROW_COUNT };
int settings_aa_row(void);
void settings_aa_row_set(int row);
// Video > Render scale: 50%, 75%, 100% (the window's size), 125%, 150%, 200%
enum { RSCALE_50 = 0, RSCALE_75 = 1, RSCALE_100 = 2, RSCALE_125 = 3, RSCALE_150 = 4, RSCALE_200 = 5, RSCALE_COUNT };
#ifdef __APPLE__
#define AA_TAA_OR_SMAA AA_SMAA
#else
#define AA_TAA_OR_SMAA AA_TAA
#endif
// Shadow resolution (Video > Shadow resolution; runtime/nvn/output_scale.c shadow_map_size): the sun's shadow maps at
// the console's size (BOTW: 896 x 896) or made bigger, for sharper shadows at some GPU time.
enum { SHADOW_CONSOLE = 0, SHADOW_HIGH = 1 /* 2048 */, SHADOW_ULTRA = 2 /* 4096 */, SHADOW_EXTREME = 3 /* 8192 */,
       SHADOW_POTATO = 4 /* 448 */, SHADOW_COUNT };
enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };
// Frame rate (Video > Frame rate; runtime/hle/framepace.c): frames per second, 0 = no limit. The game's clock is the
// same at every rate. The console runs the game at 30.
#define FPS_DEFAULT 60
#define FPS_CHOICES {0, 30, 60, 90, 120, 144, 165, 240, 360}
// PC handhelds (settings_handheld): 40 for a screen at 40 or 80 Hz, 45 at 90 Hz; no Off, no Match display
#define FPS_CHOICES_HANDHELD {30, 40, 45, 60}
#define FPS_CHOICES_MAX 9
int fps_choices(const int **list);  // this machine's frame rates (FPS_CHOICES, or FPS_CHOICES_HANDHELD): their count
// VSync (Video > VSync; runtime/nvn/vulkan/device.c picks the present mode): On waits for the display (FIFO; with
// G-SYNC / FreeSync the display waits for the game instead), Fast shows the newest frame at each refresh (MAILBOX),
// Adaptive is On but shows a late frame at once (FIFO_RELAXED), Off shows each frame at once (IMMEDIATE). With On and
// Adaptive the frame rate stays a little under the display's refresh rate (runtime/hle/framepace.c).
enum { VSYNC_ON = 0, VSYNC_FAST = 1, VSYNC_ADAPTIVE = 2, VSYNC_OFF = 3, VSYNC_COUNT };
#define VSYNC_DEFAULT VSYNC_FAST
// Video > HDR: Auto presents in HDR10 while Windows HDR is on for the window's monitor, On always (where the display
// takes it), Off never (an SDR picture, which Windows shows dim and dull while its HDR is on)
enum { HDR_AUTO = 0, HDR_ON = 1, HDR_OFF = 2, HDR_COUNT };
enum { SPARSE_HEAP_AUTO = 0, SPARSE_HEAP_ON = 1, SPARSE_HEAP_OFF = 2, SPARSE_HEAP_COUNT };
// Video > Fullscreen mode (runtime/window_size.c winsize_apply): Automatic is borderless at the desktop's size and the
// display's own mode at any other size it has; Borderless never changes the display's mode (another size is scaled to
// the screen); Exclusive always uses a display mode (the desktop's size too, at its highest refresh rate)
enum { FSMODE_AUTO = 0, FSMODE_BORDERLESS = 1, FSMODE_EXCLUSIVE = 2, FSMODE_COUNT };
// Texture LOD bias (Video > Texture sharpness): pulls forward mipmaps for sharper ground/object textures.
enum { TEX_LOD_DEFAULT = 0, TEX_LOD_SHARP = 1, TEX_LOD_SHARPER = 2, TEX_LOD_COUNT };

// Audio > Audio output (runtime/services/audio.c): Stereo mixes the game's 6 channels (FL, FR, FC, LFE, BL, BR) down
// to 2; Surround 5.1 sends all 6 channels to the audio device; 3D Headphones mixes the 6 channels to stereo with
// binaural head-shadow and ear-delay cues for headphones.
enum { AUDIO_OUT_STEREO = 0, AUDIO_OUT_SURROUND_51 = 1, AUDIO_OUT_HEADPHONES_3D = 2, AUDIO_OUT_COUNT };
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
    bool game_fxaa;      // the game's own FXAA, in its scene -> final image composite (vulkan/exec.c fxaa_patch)
    bool wet_fix;        // Video > Wet grass fix: tames the white glare of rain in sunshine (maxwell/translate.c maxwell_wet_lighting)
    int render_scale;    // 50%, 75%, 100% (RSCALE_100: the window's size), 125%, 150%, 200% (nvn/output_scale.c)
    int anisotropy;      // texture filtering: 0 the game's own, 1..4 = 2x, 4x, 8x, 16x (vulkan/textures.c)
    int tex_lod_bias;    // TEX_LOD_*: texture LOD bias, 0 = default (0.0), 1 = sharp (-0.5), 2 = sharper (-1.0)
    int sharpening;      // AMD CAS sharpening of the 3D image, 0 (off) .. 10 (100%) (vulkan/exec.c scene_post)
    bool frame_gen;      // AMD FSR 3 frame generation: a frame made between each two the game draws (vulkan/framegen.cpp)
    float volume;        // master volume, 1 = 100%
    int audio_output;    // AUDIO_OUT_*: Stereo, Surround 5.1, or 3D Headphones
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool mute_unfocused; // mute audio while the window is in the background or minimized
    int shrine_sensor_volume; // Sheikah sensor volume: 0% (mute) .. 100% (normal)
    char amiibo_reader[128]; // the figure on the virtual reader: its dump as "<series>/<name>" ("": none; runtime/amiibo.h)
    int perf_overlay;    // PERF_*: the performance overlay (F10 cycles it)
    int fps_limit;       // frame rate, frames per second (FPS_CHOICES; 0: no limit)
    bool fps_display;    // Frame rate > Match display: fps_limit follows the display (settings_fps_for_display)
    int vsync;           // VSYNC_*
    bool rumble;         // controller vibration
    int rumble_strength; // vibration intensity percent: 10..100 (default 100)
    bool freecam_sticks; // both stick clicks together turn the free camera on and off (hle/input.c)
    int gpu;             // Vulkan adapter index from the startup log; -1: automatic (restart required)
    bool quick_drop;      // native dropping from the quick equipment selector
    bool quick_sort;      // native sorting in the quick equipment selector
    bool dpad_move;      // the d-pad (and arrow keys) also act as the left stick while it is at rest
    bool stick_cam_on;   // Controls > Controller camera speed: stick_cam_x and _y apply (off: the game's own speed)
    int stick_cam_x, stick_cam_y;  // the right stick's camera speed, left and right / up and down, percent of the game's
    float stick_deadzone[2];  // left, right stick: a controller's stick reads 0 within this much of full tilt (radial)
    bool pc_bow;         // Mouse bow aiming: right button held aims the bow, a left click fires (runtime/pc_bow.h)
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
    int button_layout;   // BUTTON_LAYOUT_*: where A, B, X and Y sit on the HUD's four-button diamond (Automatic: bindings)
    bool async_shaders;  // draw on while new shaders compile (no stutter; what needs them is skipped meanwhile; on by
                         // default since settings_version 4; off: every draw waits for its shaders, as before)
    bool precompile_shaders;  // setting the game files up also precompiles every shader (runtime/nvn/precompile.h)
    int shadow_res;      // SHADOW_*: shadow maps at the console's size, bigger, or (Potato) smaller
    int sharper_menus;   // the menus' one-channel textures enlarged this many times (2 or 4; 0: off), runtime/nvn/text_upscale.c
    int sharper_text;    // the game's font sheets enlarged this many times (2 or 4; 0: off), runtime/nvn/text_upscale.c
    bool wide_view;      // a window that isn't 16:9 shows more of the world instead of bars (runtime/nvn/output_scale.c)
    bool hud_edges;      // on windows taller than 16:9 with wide_view, anchor HUD to the window edges
    int hdr;             // HDR_* (runtime/nvn/vulkan/device.c hdr_*)
    int hdr_white;       // nits of SDR white in HDR (0: what Windows gives SDR content on that monitor)
    int sparse_heap;     // SPARSE_HEAP_*: guest heap backing (Windows Vulkan; applied on next start)
    int fov;             // field of view in play, percent of the game's own (FOV_CHOICES)
    bool handheld_setup; // a PC handheld's own settings were taken once (settings_load)
    int hud_size;        // the HUD's size in play, percent of the game's own (HUD_SIZE_CHOICES, runtime/nvn/hud_scale.h)
    int draw_distance;   // DRAWDIST_*: how far placed actors are kept (runtime/draw_distance.c)
    int haze;            // the air's haze, percent of the game's (services/fs.c haze_patch; from the next start)
    bool bloom;          // the glow around bright light (off: its draws clear their images: vulkan/exec.c)
    bool depth_of_field; // the distance blur over far pixels (off: its draw is skipped: vulkan/exec.c)
    bool dynamic_res;    // the game lowers its 3D resolution (1600x900 down to 1280x720) when a frame takes long
                         // (vulkan/exec.c CMD_REPORT_COUNTER); off: it keeps 1600x900
    bool fast_fma;       // CPUs without FMA instructions (gamelib_compat): the game's fused multiply-adds rounded twice,
                         // faster (simd.h simd_fma_unfused); off: exact, as the console
    // Video > Colour (runtime/color_grade.h): 0 everywhere and no LUT = the game's own colours (nothing runs)
    int color_exposure;      // tenths of an EV, -20..20
    int color_contrast;      // percent, -50..50
    int color_saturation;    // percent, -100..100 (-100: black and white)
    int color_temperature;   // -100 (cooler, blue) .. 100 (warmer, orange)
    int color_tint;          // -100 (green) .. 100 (magenta)
    char color_lut[128];     // color_lut_id(): "" none, "sw:<file>" shipped, "<file>" the player's luts/ folder
    int color_lut_strength;  // percent, 0..100
    bool color_lut_set;      // the ini names a LUT (or none): the player chose; the first setup offers COLOR_LUT_RECOMMENDED
    // Options > Accessibility (runtime/assists.c): assists, each off by default
    bool no_weapon_wear;     // weapons, bows and shields lose no durability and never break
    int weapon_wear;         // how fast they wear out otherwise, percent of the game's (WEAPON_WEAR_MIN..MAX)
    bool infinite_arrows;    // shooting a bow takes no arrows from the pouch
    bool infinite_stamina;   // the stamina wheel never goes down
    bool infinite_rupees;    // the wallet is kept at 9999 rupees (runtime/rupees.c)
    int weather;             // WEATHER_COUNT: the game's, rain or thunderstorms kept out, or one weather held (weather.c)
    bool god_mode;           // Link takes no damage
    bool one_hit_kill;       // whatever else takes damage loses all its life at once
    int damage_taken;        // percent of the damage Link takes (DAMAGE_CHOICES)
    int damage_dealt;        // percent of the damage everything else takes (DAMAGE_CHOICES)
    int text_language;   // index in g_text_languages (runtime/language.h), 0: the host's
    int voice_language;  // index in g_voice_languages, 0: the text language's voices
    bool save_backups;   // Saves > Back up at each start: a copy of the save folder as the game starts (runtime/save_backup.c)
    bool auto_continue;  // Gameplay > Start at Continue: past the start screen into the latest save (hle/input.c)
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
} Settings;

#define RUMBLE_STRENGTH_MIN 10
#define RUMBLE_STRENGTH_MAX 100
#define RUMBLE_STRENGTH_DEFAULT 100
#define STICK_CAM_MIN 100      // (Controller camera speed, percent, in steps of 25; 200 to start with)
#define STICK_CAM_MAX 600
#define STICK_CAM_DEFAULT 200
#define STICK_DEADZONE_DEFAULT 0.10f  // (the Switch's own input service has one: the game expects 0 at rest)
#define HAZE_CHOICES {75, 90, 100}  // (percent of the game's haze: below 75 the whole picture turns teal)
#define HAZE_MIN 75
enum { DRAWDIST_ORIGINAL, DRAWDIST_MEDIUM, DRAWDIST_HIGH, DRAWDIST_POTATO, DRAWDIST_COUNT };  // (Original: the game's own
                                                                                  // distances; Potato: nearer)
float draw_distance_factor(void);  // the setting's factor on the game's distances (1: Original)
// Video > Graphics preset: the Advanced options' draw distance, shadows, bloom, depth of field, dynamic resolution,
// anisotropic filtering, anti-aliasing, the game's FXAA, upscaler, asynchronous shaders and render scale at once
// (settings.c k_gpreset). Potato: for weak PCs; Original: as on the console (the defaults); High; Savage.
// PC Handheld: only on one (settings_handheld), taken by itself the first time the game starts there.
enum { GPRESET_POTATO, GPRESET_ORIGINAL, GPRESET_HIGH, GPRESET_SAVAGE, GPRESET_HANDHELD, GPRESET_COUNT };
extern const char *const g_gpreset_names[GPRESET_COUNT];
int graphics_preset_current(void);  // the preset the settings match, or -1 (Custom)
void graphics_preset_apply(int preset);
int graphics_preset_count(void);    // the presets offered on this machine
// This machine is a PC handheld (runtime/handheld.h), a Steam Deck; the size of its screen (false: the desktop's)
bool settings_handheld(void);
bool settings_steam_deck(void);
bool settings_handheld_size(int *w, int *h);
const char *settings_file(void);  // savagewind.ini, or SAVAGEWIND_SETTINGS
#define WEAPON_WEAR_MIN 0    // (Accessibility > Weapon wear, percent: 0 = never wear out, as no_weapon_wear)
#define WEAPON_WEAR_MAX 300
#define WEATHER_COUNT 12  // (weather: 0 the game's, 1 no rain, 2 no thunderstorms, 3 + the game's weather type, held)
extern const char *const g_weather_keys[WEATHER_COUNT];  // (the ini and `weather` names: game, no_rain, ..., sun_shower)
#define DAMAGE_CHOICES {25, 50, 75, 100, 150, 200, 300, 400}  // (Accessibility's damage multipliers, percent)
#define FOV_CHOICES {70, 80, 90, 100, 110, 120, 130, 140, 150}  // (percent; 100: the game's own)
#define HUD_SIZE_CHOICES {50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150}  // (percent; 100: the game's own)
#define MOUSE_SENS_DEFAULT 3.0f  // (300% of the first default, 1; user, 2026-10-19)
#define MOUSE_SENS_Y_CHOICES {50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200}  // (percent of the horizontal)
enum { MOTION_INVERT_UD = 1, MOTION_INVERT_LR = 2 };
enum { ICONS_AUTO, ICONS_KEYBOARD, ICONS_XBOX, ICONS_PLAYSTATION, ICONS_NINTENDO, ICONS_STEAMDECK, ICONS_COUNT };
enum {
    BUTTON_LAYOUT_AUTO,
    BUTTON_LAYOUT_NINTENDO,
    BUTTON_LAYOUT_SWAP_AB,
    BUTTON_LAYOUT_XBOX,
    BUTTON_LAYOUT_SWAP_XY,
    BUTTON_LAYOUT_GAMECUBE,
    BUTTON_LAYOUT_COUNT
};

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Frame rate > Match display: fps_limit becomes the highest offered rate the display (hz) can show, at least 30 (a
// 280 Hz display: 240; a 60 Hz one: 60). Nothing with another rate chosen or hz unknown (runtime/window_size.c).
void settings_fps_for_display(float hz);
