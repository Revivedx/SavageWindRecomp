#include "settings.h"
#include "gpu_select.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "language.h"
#include "handheld.h"

bool fsr3_upscaler_enabled(void);  // (nvn/output_scale.c: FSR 3 upscaling held back for now)

#define SETTINGS_FILE settings_file()

// SAVAGEWIND_SETTINGS=<path>: another settings file (test runs never touch the player's savagewind.ini)
const char *settings_file(void) {
    const char *e = getenv("SAVAGEWIND_SETTINGS");
    return e && *e ? e : "savagewind.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .mute_unfocused = false,
    .shrine_sensor_volume = 100,
    .rumble = true,
    .rumble_strength = RUMBLE_STRENGTH_DEFAULT,
    .freecam_sticks = true,
    .damage_taken = 100,
    .damage_dealt = 100,
    .weapon_wear = 100,
    .upscaler = UPSCALE_DEFAULT,  // (Off: the window's resolution; MetalFX on macOS)
    .upscale_quality = UPQ_QUALITY,
    .game_fxaa = true,
    .anisotropy = 1,  // (2x: the Original graphics preset)
    .dynamic_res = true,
    .render_scale = RSCALE_100,
    .mouse_camera = true,
    .pc_cursor = true,
    .mouse_sens = MOUSE_SENS_DEFAULT,
    .mouse_sens_y = 1.0f,
    .motion_sens = 1.0f,
    .pc_gyro = true,
    .gyro_scene = true,
    .stick_cam_x = STICK_CAM_DEFAULT,
    .stick_cam_y = STICK_CAM_DEFAULT,
    .stick_deadzone = {STICK_DEADZONE_DEFAULT, STICK_DEADZONE_DEFAULT},
    .fps_limit = FPS_DEFAULT,
    .precompile_shaders = true,
    .async_shaders = true,
    .vsync = VSYNC_DEFAULT,
    .gpu = -1,
    .wide_view = true,
    .hud_edges = false,
    .fov = 100,
    .hud_size = 100,
    .color_lut_strength = 100,
    .haze = 100,
    .bloom = true,
    .depth_of_field = true,
    .fast_fma = true,
};

static const char *const k_vsync[VSYNC_COUNT] = {"on", "fast", "adaptive", "off"};  // (VSYNC_*: the ini's words)
static const char *const k_hdr[HDR_COUNT] = {"auto", "on", "off"};             // (HDR_*)
static const char *const k_sparse_heap[SPARSE_HEAP_COUNT] = {"auto", "on", "off"};
static const char *const k_tex_lod_bias[TEX_LOD_COUNT] = {"default", "sharp", "sharper"};
static const char *const k_fsmode[FSMODE_COUNT] = {"auto", "borderless", "exclusive"};  // (FSMODE_*)
static const char *const k_audio_out[AUDIO_OUT_COUNT] = {"stereo", "surround_51", "headphones_3d"};  // (AUDIO_OUT_*)

// settings_version 2 (2026-10-19): the upscaler's mode, the window size and the 300% mouse default. Older files: their
// "resolution" (the internal resolution, removed) gives the upscaler's mode, and a mouse sensitivity at the old
// default (100%) becomes the new one.
// settings_version 3 (2026-10-20): the upscaler sets the 3D image's resolution (AMD FSR 1 renders it smaller). Before,
// "fsr1" (the default) only scaled the finished 1920x1080 image to a bigger window: older files load it as Off, so
// nobody drops below the window's resolution unasked, except version-1 files whose internal resolution was a share of
// the display (75%, 50%, 25%: they keep FSR 1 at the nearest mode).
// settings_version 4 (2026-11-01): "Asynchronous shaders" is on by default: with the shaders precompiled, the pipeline
// list pre-warmed at start and the cheap links done inline (exec.c link_now), a draw only skips a frame for a real
// shader compile, and the draws that must not be skipped (one-shot targets, texture arrays, compute) wait. Older files
// have the old default (off) written in them: version 3 files take the new default (a player who turned it on keeps
// it; one who left it off was on the default, since the menu wrote every key).
#define SETTINGS_VERSION 4

static const char *const k_drawdist[DRAWDIST_COUNT] = {"original", "medium", "high", "potato"};  // (DRAWDIST_*)

const char *const g_weather_keys[WEATHER_COUNT] = {"game", "no_rain", "no_storms", "clear", "cloudy", "rain",
                                                   "heavy_rain", "snow", "heavy_snow", "thunderstorm", "thunder_rain",
                                                   "sun_shower"};  // (weather; settings.h)
static const char *const k_icons[ICONS_COUNT] = {"auto", "keyboard", "xbox", "playstation", "nintendo", "steamdeck"};

static const char *const k_button_layout[BUTTON_LAYOUT_COUNT] = {"auto", "nintendo", "swap_ab", "xbox", "swap_xy", "gamecube"};

static const char *const k_quality[UPQ_COUNT] = {"ultra_quality", "quality", "balanced", "performance", "ultra_performance"};

// Debug overrides seed this boot's options; the Controls switches can still change them at once.
static void pc_controls_env(void) {
    const char *cursor = getenv("SAVAGEWIND_PC_CURSOR");
    if (cursor) g_settings.pc_cursor = atoi(cursor) != 0;
}

// SAVAGEWIND_PC_GYRO / SAVAGEWIND_GYRO_SCENE=0|1: the two shrine apparatus switches at start (Controls changes them)
static void gyro_startup_overrides(void) {
    const char *v = getenv("SAVAGEWIND_PC_GYRO");
    if (v && *v) g_settings.pc_gyro = atoi(v) != 0;
    v = getenv("SAVAGEWIND_GYRO_SCENE");
    if (v && *v) g_settings.gyro_scene = atoi(v) != 0;
}
void settings_fps_for_display(float hz) {
    if (!g_settings.fps_display || hz < 20) return;
    static const int fps[] = FPS_CHOICES;
    int best = 30;
    for (unsigned i = 0; i < sizeof fps / sizeof fps[0]; i++)
        if (fps[i] && fps[i] <= hz + 1 && fps[i] > best) best = fps[i];
    if (g_settings.fps_limit != best) LOG("[settings] frame rate %d (Match display: %.0f Hz)\n", best, hz);
    g_settings.fps_limit = best;
}

static const Handheld *handheld(void) {
    static Handheld h;
    static bool done;
    if (!done) {
        h = handheld_detect(), done = true;
        if (h.on) LOG("[settings] PC handheld%s, screen %d x %d\n", h.deck ? " (Steam Deck)" : "", h.w, h.h);
    }
    return &h;
}
bool settings_handheld(void) { return handheld()->on; }
bool settings_steam_deck(void) { return handheld()->deck; }
bool settings_handheld_size(int *w, int *h) {
    if (handheld()->w <= 0) return false;
    *w = handheld()->w, *h = handheld()->h;
    return true;
}

int settings_aa_row(void) {
    return g_settings.antialias == AA_TAA ? AA_ROW_TAA : g_settings.antialias == AA_SMAA ? AA_ROW_SMAA
         : g_settings.game_fxaa ? AA_ROW_FXAA : AA_ROW_OFF;
}
void settings_aa_row_set(int row) {
    g_settings.antialias = row == AA_ROW_TAA ? AA_TAA : row == AA_ROW_SMAA ? AA_SMAA : AA_OFF;
    g_settings.game_fxaa = row == AA_ROW_FXAA;
}

int fps_choices(const int **list) {
    static const int pc[] = FPS_CHOICES, hh[] = FPS_CHOICES_HANDHELD;
    _Static_assert(sizeof pc / sizeof pc[0] <= FPS_CHOICES_MAX, "");
    bool h = settings_handheld();
    *list = h ? hh : pc;
    return h ? (int)(sizeof hh / sizeof hh[0]) : (int)(sizeof pc / sizeof pc[0]);
}

// The first start on a handheld, also with settings from before this: the handheld's preset, full screen at its size
static void handheld_first_setup(void) {
    if (!settings_handheld() || g_settings.handheld_setup) return;
    g_settings.handheld_setup = true;
    graphics_preset_apply(GPRESET_HANDHELD);
    g_settings.fullscreen = true;
    g_settings.window_w = g_settings.window_h = 0;
    g_settings.fps_limit = FPS_DEFAULT, g_settings.fps_display = false;
    g_settings.fov = 80, g_settings.hud_edges = true;
    g_settings.sharper_menus = g_settings.sharper_text = 2;  // (small screen: the letters and menus stay crisp)
    LOG("[settings] PC handheld: its own settings, the first time\n");
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        handheld_first_setup();
        gyro_startup_overrides();
        pc_controls_env();
        return;
    }
    char line[256];
    bool tip_key = false;
    int version = 1;
    char old_res[16] = "";
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[128];
        val[0] = 0;
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, val) < 1) continue;
        for (size_t n = strlen(val); n && (val[n - 1] == ' ' || val[n - 1] == '\t'); n--) val[n - 1] = 0;  // (a file
        // edited by hand: Notepad's CRLF line ends and stray spaces would make the names below not match)
        if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "fullscreen_mode")) {
            for (int i = 0; i < FSMODE_COUNT; i++)
                if (!strcmp(val, k_fsmode[i])) g_settings.fullscreen_mode = i;
        }
        else if (!strcmp(key, "display")) {  // (auto, or "<n> <name>": the n-th display, named so)
            int n = 0, at = 0;
            g_settings.display = 0, g_settings.display_name[0] = 0;
            if (strcmp(val, "auto") && sscanf(val, "%d %n", &n, &at) >= 1 && n >= 1 && n <= 16) {
                g_settings.display = n;
                snprintf(g_settings.display_name, sizeof g_settings.display_name, "%s", at ? val + at : "");
            }
        }
        else if (!strcmp(key, "gpu")) g_settings.gpu = gpu_index_parse(val);
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "audio_output")) {
            for (int i = 0; i < AUDIO_OUT_COUNT; i++)
                if (!strcmp(val, k_audio_out[i])) g_settings.audio_output = i;
        }
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "mute_unfocused")) g_settings.mute_unfocused = atoi(val) != 0;
        else if (!strcmp(key, "shrine_sensor_volume")) g_settings.shrine_sensor_volume = atoi(val);
        else if (!strcmp(key, "amiibo_reader")) snprintf(g_settings.amiibo_reader, sizeof g_settings.amiibo_reader, "%s", strcmp(val, "none") ? val : "");
        else if (!strcmp(key, "show_fps")) g_settings.perf_overlay = atoi(val) ? PERF_FPS : PERF_OFF;  // (older files)
        else if (!strcmp(key, "perf_overlay")) g_settings.perf_overlay = !strcmp(val, "full") ? PERF_FULL : !strcmp(val, "fps") ? PERF_FPS : PERF_OFF;
        else if (!strcmp(key, "frame_rate")) {  // (fps / off / display)
            g_settings.fps_display = !strcmp(val, "display");
            if (!g_settings.fps_display) g_settings.fps_limit = !strcmp(val, "off") ? 0 : atoi(val);
        }
        else if (!strcmp(key, "vsync")) {
            for (int i = 0; i < VSYNC_COUNT; i++)
                if (!strcmp(val, k_vsync[i])) g_settings.vsync = i;
        }
        else if (!strcmp(key, "settings_version")) version = atoi(val);
        else if (!strcmp(key, "resolution")) snprintf(old_res, sizeof old_res, "%s", val);  // (version 1)
        else if (!strcmp(key, "upscaler"))  // (metalfx / fsr1: this OS's spatial upscaler; dlss: NGX; fsr3, fsr4: Vulkan,
            // held back for now: FSR 1 with the same quality mode, nvn/output_scale.c fsr3_upscaler_enabled)
            g_settings.upscaler = !strcmp(val, "dlss") ? UPSCALE_DLSS
                                : !strcmp(val, "fsr1") || !strcmp(val, "metalfx") ? UPSCALE_SPATIAL
                                : !strcmp(val, "fsr3") ? (fsr3_upscaler_enabled() ? UPSCALE_FSR3_OR_SPATIAL : UPSCALE_SPATIAL)
                                : !strcmp(val, "fsr4") ? (fsr3_upscaler_enabled() ? UPSCALE_FSR4_OR_SPATIAL : UPSCALE_SPATIAL)
                                                       : UPSCALE_OFF;
        else if (!strcmp(key, "upscaler_quality")) {
            for (int i = 0; i < UPQ_COUNT; i++)
                if (!strcmp(val, k_quality[i])) g_settings.upscale_quality = i;
        }
        else if (!strcmp(key, "window_size")) {  // (WxH in pixels, or default)
            int w = 0, h = 0;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384)
                g_settings.window_w = w, g_settings.window_h = h;
        }
        else if (!strcmp(key, "antialias"))  // ("fxaa": SMAA now; "taa": Vulkan only)
            g_settings.antialias = !strcmp(val, "off") ? AA_OFF : !strcmp(val, "taa") ? AA_TAA_OR_SMAA : AA_SMAA;
        else if (!strcmp(key, "render_scale")) {  // (percent: 50, 75, 100, 125, 150, 200)
            int p = atoi(val);
            g_settings.render_scale = p >= 200 ? RSCALE_200 : p >= 150 ? RSCALE_150 : p >= 125 ? RSCALE_125
                                    : p >= 100 ? RSCALE_100 : p >= 75 ? RSCALE_75 : RSCALE_50;
        }
        else if (!strcmp(key, "anisotropy")) {  // (game, 2x .. 16x)
            int a = atoi(val);
            g_settings.anisotropy = a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
        }
        else if (!strcmp(key, "texture_lod_bias")) {
            for (int i = 0; i < TEX_LOD_COUNT; i++)
                if (!strcmp(val, k_tex_lod_bias[i])) g_settings.tex_lod_bias = i;
        }
        else if (!strcmp(key, "game_fxaa")) g_settings.game_fxaa = atoi(val) != 0;
        else if (!strcmp(key, "wet_fix")) g_settings.wet_fix = atoi(val) != 0;
        else if (!strcmp(key, "frame_generation")) g_settings.frame_gen = atoi(val) != 0;
        else if (!strcmp(key, "sharpening")) {  // (percent, in steps of 10)
            int p = atoi(val);
            g_settings.sharpening = p < 0 ? 0 : p > 100 ? 10 : (p + 5) / 10;
        }
        else if (!strcmp(key, "rumble")) g_settings.rumble = atoi(val) != 0;
        else if (!strcmp(key, "rumble_strength")) g_settings.rumble_strength = atoi(val);
        else if (!strcmp(key, "freecam_sticks")) g_settings.freecam_sticks = atoi(val) != 0;
        else if (!strcmp(key, "quick_drop")) g_settings.quick_drop = atoi(val) != 0;
        else if (!strcmp(key, "quick_sort")) g_settings.quick_sort = atoi(val) != 0;
        else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
        else if (!strcmp(key, "left_stick_deadzone")) g_settings.stick_deadzone[0] = (float)atof(val) / 100;  // (percent)
        else if (!strcmp(key, "right_stick_deadzone")) g_settings.stick_deadzone[1] = (float)atof(val) / 100;
        else if (!strcmp(key, "mouse_bow")) g_settings.pc_bow = atoi(val) != 0;
        else if (!strcmp(key, "mouse_camera")) g_settings.mouse_camera = atoi(val) != 0;
        else if (!strcmp(key, "pc_cursor")) g_settings.pc_cursor = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sensitivity")) g_settings.mouse_sens = (float)atof(val);
        else if (!strcmp(key, "mouse_vertical_sensitivity")) g_settings.mouse_sens_y = (float)atof(val);
        else if (!strcmp(key, "mouse_invert_y")) g_settings.mouse_invert_y = atoi(val) != 0;
        else if (!strcmp(key, "pc_gyro")) g_settings.pc_gyro = atoi(val) != 0;
        else if (!strcmp(key, "gyro_scene")) g_settings.gyro_scene = atoi(val) != 0;
        else if (!strcmp(key, "motion_sensitivity")) g_settings.motion_sens = (float)atof(val);
        else if (!strcmp(key, "motion_invert")) {  // (off, up_down, left_right, both)
            g_settings.motion_invert = !strcmp(val, "both") ? MOTION_INVERT_UD | MOTION_INVERT_LR :
                                       !strcmp(val, "up_down") ? MOTION_INVERT_UD :
                                       !strcmp(val, "left_right") ? MOTION_INVERT_LR : 0;
        }
        else if (!strcmp(key, "button_icons")) {
            for (int i = 0; i < ICONS_COUNT; i++)
                if (!strcmp(val, k_icons[i])) g_settings.button_icons = i;
        }
        else if (!strcmp(key, "button_layout")) {
            for (int i = 0; i < BUTTON_LAYOUT_COUNT; i++)
                if (!strcmp(val, k_button_layout[i])) g_settings.button_layout = i;
        }
        else if (!strcmp(key, "async_shaders")) g_settings.async_shaders = atoi(val) != 0;
        else if (!strcmp(key, "precompile_shaders")) g_settings.precompile_shaders = atoi(val) != 0;
        else if (!strcmp(key, "wide_view")) g_settings.wide_view = atoi(val) != 0;
        else if (!strcmp(key, "hud_edges")) g_settings.hud_edges = atoi(val) != 0;
        else if (!strcmp(key, "hdr")) {
            for (int i = 0; i < HDR_COUNT; i++)
                if (!strcmp(val, k_hdr[i])) g_settings.hdr = i;
        }
        else if (!strcmp(key, "sparse_heap")) {
            for (int i = 0; i < SPARSE_HEAP_COUNT; i++)
                if (!strcmp(val, k_sparse_heap[i])) g_settings.sparse_heap = i;
        }
        else if (!strcmp(key, "hdr_paper_white")) {  // (nits; 0: Windows' SDR content brightness)
            int n = atoi(val);
            g_settings.hdr_white = n >= 80 && n <= 1000 ? n : 0;
        }
        else if (!strcmp(key, "field_of_view")) g_settings.fov = atoi(val);  // (percent)
        else if (!strcmp(key, "hud_size")) g_settings.hud_size = atoi(val);  // (percent)
        else if (!strcmp(key, "handheld_setup")) g_settings.handheld_setup = atoi(val) != 0;
        else if (!strcmp(key, "draw_distance")) {
            for (int i = 0; i < DRAWDIST_COUNT; i++)
                if (!strcmp(val, k_drawdist[i])) g_settings.draw_distance = i;
        }
        else if (!strcmp(key, "no_weapon_wear")) g_settings.no_weapon_wear = atoi(val) != 0;
        else if (!strcmp(key, "weapon_wear")) g_settings.weapon_wear = atoi(val);  // (percent)
        else if (!strcmp(key, "infinite_arrows")) g_settings.infinite_arrows = atoi(val) != 0;
        else if (!strcmp(key, "save_backups")) g_settings.save_backups = atoi(val) != 0;
        else if (!strcmp(key, "stick_camera_speed")) g_settings.stick_cam_on = atoi(val) != 0;
        else if (!strcmp(key, "stick_camera_x")) g_settings.stick_cam_x = atoi(val);  // (percent)
        else if (!strcmp(key, "stick_camera_y")) g_settings.stick_cam_y = atoi(val);  // (percent)
        else if (!strcmp(key, "start_at_continue")) g_settings.auto_continue = atoi(val) != 0;
        else if (!strcmp(key, "infinite_stamina")) g_settings.infinite_stamina = atoi(val) != 0;
        else if (!strcmp(key, "infinite_rupees")) g_settings.infinite_rupees = atoi(val) != 0;
        else if (!strcmp(key, "weather")) {
            g_settings.weather = 0;
            for (int i = 0; i < WEATHER_COUNT; i++)
                if (!strcmp(val, g_weather_keys[i])) g_settings.weather = i;
        }
        else if (!strcmp(key, "god_mode")) g_settings.god_mode = atoi(val) != 0;
        else if (!strcmp(key, "one_hit_kill")) g_settings.one_hit_kill = atoi(val) != 0;
        else if (!strcmp(key, "damage_taken")) g_settings.damage_taken = atoi(val);  // (percent)
        else if (!strcmp(key, "damage_dealt")) g_settings.damage_dealt = atoi(val);
        else if (!strcmp(key, "haze")) {  // (percent; an older ini's 0-50 reads as 75: HAZE_MIN)
            int v = atoi(val);
            g_settings.haze = v < HAZE_MIN ? HAZE_MIN : v > 100 ? 100 : v;
        }
        else if (!strcmp(key, "bloom")) g_settings.bloom = atoi(val) != 0;
        else if (!strcmp(key, "depth_of_field")) g_settings.depth_of_field = atoi(val) != 0;
        else if (!strcmp(key, "dynamic_resolution")) g_settings.dynamic_res = atoi(val) != 0;
        else if (!strcmp(key, "fast_fma")) g_settings.fast_fma = atoi(val) != 0;
        else if (!strcmp(key, "colour_exposure")) g_settings.color_exposure = (int)lroundf((float)atof(val) * 10);  // (EV)
        else if (!strcmp(key, "colour_contrast")) g_settings.color_contrast = atoi(val);  // (percent)
        else if (!strcmp(key, "colour_saturation")) g_settings.color_saturation = atoi(val);
        else if (!strcmp(key, "colour_temperature")) g_settings.color_temperature = atoi(val);
        else if (!strcmp(key, "colour_tint")) g_settings.color_tint = atoi(val);
        else if (!strcmp(key, "colour_lut")) {  // (none: no LUT)
            snprintf(g_settings.color_lut, sizeof g_settings.color_lut, "%s", strcmp(val, "none") ? val : "");
            g_settings.color_lut_set = true;
        }
        else if (!strcmp(key, "colour_lut_strength")) g_settings.color_lut_strength = atoi(val);  // (percent)
        else if (!strcmp(key, "sharper_menus"))  // (off / 2x / 4x)
            g_settings.sharper_menus = !strcmp(val, "4x") ? 4 : !strcmp(val, "2x") ? 2 : 0;
        else if (!strcmp(key, "sharper_text"))  // (off / 2x / 4x)
            g_settings.sharper_text = !strcmp(val, "4x") ? 4 : !strcmp(val, "2x") ? 2 : 0;
        else if (!strcmp(key, "shadow_resolution"))  // (console / high / ultra / extreme)
            g_settings.shadow_res = !strcmp(val, "extreme") ? SHADOW_EXTREME : !strcmp(val, "ultra") ? SHADOW_ULTRA
                                  : !strcmp(val, "high") ? SHADOW_HIGH : !strcmp(val, "potato") ? SHADOW_POTATO
                                  : SHADOW_CONSOLE;
        else if (!strcmp(key, "text_language")) {  // (a region language: USen, EUes, ...; auto)
            g_settings.text_language = 0;
            for (int i = 1; i < g_ntext_languages; i++)
                if (!strcmp(val, g_text_languages[i].folder)) g_settings.text_language = i;
        }
        else if (!strcmp(key, "voice_language")) {  // (a Voice/ folder; text)
            g_settings.voice_language = 0;
            for (int i = 1; i < g_nvoice_languages; i++)
                if (!strcmp(val, g_voice_languages[i].folder)) g_settings.voice_language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (version < 2) {
        // internal resolution: a share of the display is now the upscaler's nearest mode (75%: Ultra Quality,
        // 1 / 1.3; 50% and 25%: Performance, 1 / 2); the upscaler itself is kept (bilinear: Off)
        bool q75 = !strcmp(old_res, "75%") || !strcmp(old_res, "67%");
        bool low = q75 || !strcmp(old_res, "50%") || !strcmp(old_res, "25%");
        g_settings.upscale_quality = q75 ? UPQ_ULTRA : low ? UPQ_PERFORMANCE : UPQ_QUALITY;
        if (fabsf(g_settings.mouse_sens - 1.0f) < 0.005f) g_settings.mouse_sens = MOUSE_SENS_DEFAULT;
        LOG("[settings] %s is from an older version (resolution %s): upscaler %d, mode %d, mouse %.2f\n",
            SETTINGS_FILE, old_res[0] ? old_res : "-", g_settings.upscaler, g_settings.upscale_quality,
            g_settings.mouse_sens);
    }
#ifndef __APPLE__
    if (version < 3 && g_settings.upscaler == UPSCALE_FSR1) {
        bool lower = version < 2 && (!strcmp(old_res, "75%") || !strcmp(old_res, "67%") || !strcmp(old_res, "50%") ||
                                     !strcmp(old_res, "25%"));
        if (!lower) g_settings.upscaler = UPSCALE_OFF;
        LOG("[settings] %s: version %d upscaler fsr1 -> %s\n", SETTINGS_FILE, version, lower ? "fsr1" : "off");
    }
#endif
    if (version < 4 && !g_settings.async_shaders) {
        g_settings.async_shaders = true;
        LOG("[settings] %s: version %d async_shaders 0 -> 1 (the new default)\n", SETTINGS_FILE, version);
    }
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    if (g_settings.shrine_sensor_volume < 0) g_settings.shrine_sensor_volume = 0;
    if (g_settings.shrine_sensor_volume > 100) g_settings.shrine_sensor_volume = 100;
    if (g_settings.antialias != AA_OFF) g_settings.game_fxaa = false;  // (one anti-aliasing at a time: settings_aa_row)
    handheld_first_setup();
    if (settings_handheld()) {  // (neither is offered on one)
        g_settings.fps_display = false;
        if (g_settings.fps_limit <= 0) g_settings.fps_limit = FPS_DEFAULT;
    }
    if (g_settings.fps_limit < 0) g_settings.fps_limit = 0;
    if (g_settings.fps_limit > 0) {  // (a frame rate not offered: the nearest one that is)
        const int *fps;
        int nfps = fps_choices(&fps), best = FPS_DEFAULT;
        for (int i = 0; i < nfps; i++)
            if (fps[i] && abs(fps[i] - g_settings.fps_limit) < abs(best - g_settings.fps_limit)) best = fps[i];
        g_settings.fps_limit = best;
    }
    { extern float winsize_refresh_rate(void); settings_fps_for_display(winsize_refresh_rate()); }  // (a reboot: known)
    if (!(g_settings.mouse_sens >= 0.1f && g_settings.mouse_sens <= 10.0f)) g_settings.mouse_sens = MOUSE_SENS_DEFAULT;
    if (!(g_settings.mouse_sens_y >= 0.5f && g_settings.mouse_sens_y <= 2.0f)) g_settings.mouse_sens_y = 1.0f;
    if (!(g_settings.motion_sens >= 0.1f && g_settings.motion_sens <= 10.0f)) g_settings.motion_sens = 1.0f;
    if (g_settings.fov < 50 || g_settings.fov > 200) g_settings.fov = 100;
    if (g_settings.hud_size < 50 || g_settings.hud_size > 150) g_settings.hud_size = 100;
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    g_settings.color_exposure = CLAMP(g_settings.color_exposure, -20, 20);
    g_settings.color_contrast = CLAMP(g_settings.color_contrast, -50, 50);
    g_settings.color_saturation = CLAMP(g_settings.color_saturation, -100, 100);
    g_settings.color_temperature = CLAMP(g_settings.color_temperature, -100, 100);
    g_settings.color_tint = CLAMP(g_settings.color_tint, -100, 100);
    g_settings.color_lut_strength = CLAMP(g_settings.color_lut_strength, 0, 100);
    g_settings.stick_cam_x = CLAMP(g_settings.stick_cam_x, STICK_CAM_MIN, STICK_CAM_MAX);
    g_settings.stick_cam_y = CLAMP(g_settings.stick_cam_y, STICK_CAM_MIN, STICK_CAM_MAX);
    g_settings.rumble_strength = CLAMP(g_settings.rumble_strength, RUMBLE_STRENGTH_MIN, RUMBLE_STRENGTH_MAX);
    g_settings.weapon_wear = CLAMP(g_settings.weapon_wear, WEAPON_WEAR_MIN, WEAPON_WEAR_MAX);
    if (g_settings.no_weapon_wear) g_settings.weapon_wear = 0;  // (Unbreakable weapons is Weapon wear's Fairy, 0x)
#undef CLAMP
    for (int i = 0; i < 2; i++)
        if (!(g_settings.stick_deadzone[i] >= 0 && g_settings.stick_deadzone[i] <= 0.5f))
            g_settings.stick_deadzone[i] = STICK_DEADZONE_DEFAULT;
    pc_controls_env();
    gyro_startup_overrides();
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# Savage Wind settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "settings_version = %d\n", SETTINGS_VERSION);
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    int fm = g_settings.fullscreen_mode;
    fprintf(f, "fullscreen_mode = %s\n", k_fsmode[fm >= 0 && fm < FSMODE_COUNT ? fm : FSMODE_AUTO]);
    if (g_settings.display > 0) fprintf(f, "display = %d %s\n", g_settings.display, g_settings.display_name);
    else fprintf(f, "display = auto\n");
    fprintf(f, "# Vulkan GPU index from the startup log, or auto; restart after changing\n");
    if (g_settings.gpu >= 0) fprintf(f, "gpu = %d\n", g_settings.gpu);
    else fprintf(f, "gpu = auto\n");
    if (g_settings.window_w > 0 && g_settings.window_h > 0)
        fprintf(f, "window_size = %dx%d\n", g_settings.window_w, g_settings.window_h);
    else
        fprintf(f, "window_size = default\n");
    if (g_settings.fps_display)
        fprintf(f, "frame_rate = display\n");
    else if (g_settings.fps_limit > 0)
        fprintf(f, "frame_rate = %d\n", g_settings.fps_limit);
    else
        fprintf(f, "frame_rate = off\n");
    int vs = g_settings.vsync >= 0 && g_settings.vsync < VSYNC_COUNT ? g_settings.vsync : VSYNC_DEFAULT;
    fprintf(f, "vsync = %s\n", k_vsync[vs]);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    int ao = g_settings.audio_output >= 0 && g_settings.audio_output < AUDIO_OUT_COUNT ? g_settings.audio_output : AUDIO_OUT_STEREO;
    fprintf(f, "audio_output = %s\n", k_audio_out[ao]);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "mute_unfocused = %d\n", g_settings.mute_unfocused);
    fprintf(f, "shrine_sensor_volume = %d\n", g_settings.shrine_sensor_volume);
    fprintf(f, "amiibo_reader = %s\n", g_settings.amiibo_reader[0] ? g_settings.amiibo_reader : "none");
    fprintf(f, "perf_overlay = %s\n", g_settings.perf_overlay == PERF_FULL ? "full" : g_settings.perf_overlay == PERF_FPS ? "fps" : "off");
    static const char *const ups[UPSCALE_COUNT] = {"off", "metalfx", "fsr1", "fsr3", "dlss", "fsr4"};
    int u = g_settings.upscaler, uq = g_settings.upscale_quality;
    fprintf(f, "upscaler = %s\n", u >= 0 && u < UPSCALE_COUNT ? ups[u] : "off");
    fprintf(f, "upscaler_quality = %s\n", k_quality[uq >= 0 && uq < UPQ_COUNT ? uq : UPQ_QUALITY]);
    {
        static const int rs[RSCALE_COUNT] = {50, 75, 100, 125, 150, 200};
        int r = g_settings.render_scale;
        fprintf(f, "render_scale = %d\n", rs[r >= 0 && r < RSCALE_COUNT ? r : RSCALE_100]);
    }
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_TAA ? "taa" : g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "game_fxaa = %d\n", g_settings.game_fxaa ? 1 : 0);
    fprintf(f, "wet_fix = %d\n", g_settings.wet_fix ? 1 : 0);
    int an = g_settings.anisotropy;
    if (an >= 1 && an <= 4) fprintf(f, "anisotropy = %dx\n", 1 << an);
    else fprintf(f, "anisotropy = game\n");
    int lb = g_settings.tex_lod_bias;
    fprintf(f, "texture_lod_bias = %s\n", k_tex_lod_bias[lb >= 0 && lb < TEX_LOD_COUNT ? lb : TEX_LOD_DEFAULT]);
    fprintf(f, "sharpening = %d\n", g_settings.sharpening * 10);
    fprintf(f, "frame_generation = %d\n", g_settings.frame_gen ? 1 : 0);
    fprintf(f, "rumble = %d\n", g_settings.rumble);
    fprintf(f, "rumble_strength = %d\n", g_settings.rumble_strength);
    fprintf(f, "freecam_sticks = %d\n", g_settings.freecam_sticks);
    fprintf(f, "quick_drop = %d\n", g_settings.quick_drop);
    fprintf(f, "quick_sort = %d\n", g_settings.quick_sort);
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "left_stick_deadzone = %.0f\n", g_settings.stick_deadzone[0] * 100);
    fprintf(f, "right_stick_deadzone = %.0f\n", g_settings.stick_deadzone[1] * 100);
    fprintf(f, "mouse_camera = %d\n", g_settings.mouse_camera);
    fprintf(f, "mouse_bow = %d\n", g_settings.pc_bow);
    fprintf(f, "pc_cursor = %d\n", g_settings.pc_cursor);
    fprintf(f, "mouse_sensitivity = %.2f\n", g_settings.mouse_sens);
    fprintf(f, "mouse_vertical_sensitivity = %.2f\n", g_settings.mouse_sens_y);
    fprintf(f, "mouse_invert_y = %d\n", g_settings.mouse_invert_y);
    fprintf(f, "pc_gyro = %d\n", g_settings.pc_gyro);
    fprintf(f, "gyro_scene = %d\n", g_settings.gyro_scene);
    fprintf(f, "motion_sensitivity = %.2f\n", g_settings.motion_sens);
    static const char *const inv[4] = {"off", "up_down", "left_right", "both"};
    fprintf(f, "motion_invert = %s\n", inv[g_settings.motion_invert & 3]);
    fprintf(f, "button_icons = %s\n", k_icons[g_settings.button_icons >= 0 && g_settings.button_icons < ICONS_COUNT ?
                                                g_settings.button_icons : ICONS_AUTO]);
    fprintf(f, "button_layout = %s\n",
            k_button_layout[g_settings.button_layout >= 0 && g_settings.button_layout < BUTTON_LAYOUT_COUNT ?
                                g_settings.button_layout : BUTTON_LAYOUT_AUTO]);
    fprintf(f, "async_shaders = %d\n", g_settings.async_shaders);
    fprintf(f, "precompile_shaders = %d\n", g_settings.precompile_shaders);
    static const char *const shadow[SHADOW_COUNT] = {"console", "high", "ultra", "extreme", "potato"};
    int sr = g_settings.shadow_res;
    fprintf(f, "shadow_resolution = %s\n", shadow[sr >= 0 && sr < SHADOW_COUNT ? sr : SHADOW_CONSOLE]);
    fprintf(f, "sharper_menus = %s\n", g_settings.sharper_menus == 4 ? "4x" : g_settings.sharper_menus == 2 ? "2x" : "off");
    fprintf(f, "sharper_text = %s\n", g_settings.sharper_text == 4 ? "4x" : g_settings.sharper_text == 2 ? "2x" : "off");
    fprintf(f, "wide_view = %d\n", g_settings.wide_view);
    fprintf(f, "hud_edges = %d\n", g_settings.hud_edges);
    fprintf(f, "hdr = %s\n", k_hdr[g_settings.hdr >= 0 && g_settings.hdr < HDR_COUNT ? g_settings.hdr : HDR_AUTO]);
    fprintf(f, "sparse_heap = %s\n", k_sparse_heap[g_settings.sparse_heap >= 0 && g_settings.sparse_heap < SPARSE_HEAP_COUNT ?
                                                       g_settings.sparse_heap : SPARSE_HEAP_AUTO]);
    if (g_settings.hdr_white) fprintf(f, "hdr_paper_white = %d\n", g_settings.hdr_white);
    fprintf(f, "field_of_view = %d\n", g_settings.fov);
    fprintf(f, "hud_size = %d\n", g_settings.hud_size);
    if (g_settings.handheld_setup) fprintf(f, "handheld_setup = 1\n");
    int dd = g_settings.draw_distance;
    fprintf(f, "draw_distance = %s\n", k_drawdist[dd >= 0 && dd < DRAWDIST_COUNT ? dd : DRAWDIST_ORIGINAL]);
    fprintf(f, "no_weapon_wear = %d\n", g_settings.no_weapon_wear);
    fprintf(f, "weapon_wear = %d\n", g_settings.weapon_wear);
    fprintf(f, "infinite_arrows = %d\n", g_settings.infinite_arrows);
    fprintf(f, "infinite_stamina = %d\n", g_settings.infinite_stamina);
    fprintf(f, "infinite_rupees = %d\n", g_settings.infinite_rupees);
    int wx = g_settings.weather;
    fprintf(f, "weather = %s\n", g_weather_keys[wx >= 0 && wx < WEATHER_COUNT ? wx : 0]);
    fprintf(f, "god_mode = %d\n", g_settings.god_mode);
    fprintf(f, "one_hit_kill = %d\n", g_settings.one_hit_kill);
    fprintf(f, "damage_taken = %d\n", g_settings.damage_taken);
    fprintf(f, "damage_dealt = %d\n", g_settings.damage_dealt);
    fprintf(f, "haze = %d\n", g_settings.haze);
    fprintf(f, "bloom = %d\n", g_settings.bloom ? 1 : 0);
    fprintf(f, "depth_of_field = %d\n", g_settings.depth_of_field ? 1 : 0);
    fprintf(f, "dynamic_resolution = %d\n", g_settings.dynamic_res ? 1 : 0);
    fprintf(f, "fast_fma = %d\n", g_settings.fast_fma ? 1 : 0);
    fprintf(f, "colour_exposure = %.1f\n", g_settings.color_exposure / 10.0);
    fprintf(f, "colour_contrast = %d\n", g_settings.color_contrast);
    fprintf(f, "colour_saturation = %d\n", g_settings.color_saturation);
    fprintf(f, "colour_temperature = %d\n", g_settings.color_temperature);
    fprintf(f, "colour_tint = %d\n", g_settings.color_tint);
    fprintf(f, "colour_lut = %s\n", g_settings.color_lut[0] ? g_settings.color_lut : "none");
    fprintf(f, "colour_lut_strength = %d\n", g_settings.color_lut_strength);
    int tl = g_settings.text_language, vl = g_settings.voice_language;
    fprintf(f, "text_language = %s\n", tl > 0 && tl < g_ntext_languages ? g_text_languages[tl].folder : "auto");
    fprintf(f, "voice_language = %s\n", vl > 0 && vl < g_nvoice_languages ? g_voice_languages[vl].folder : "text");
    fprintf(f, "save_backups = %d\n", g_settings.save_backups);
    fprintf(f, "stick_camera_speed = %d\n", g_settings.stick_cam_on);
    fprintf(f, "stick_camera_x = %d\n", g_settings.stick_cam_x);
    fprintf(f, "stick_camera_y = %d\n", g_settings.stick_cam_y);
    fprintf(f, "start_at_continue = %d\n", g_settings.auto_continue);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    bind_settings_save(f);
    fclose(f);
}

// (settings.h GPRESET_*). The upscaler's mode counts only with an upscaler. Original is also the defaults above.
static const struct {
    int draw_distance, shadow_res;
    bool bloom, depth_of_field, dynamic_res;
    int anisotropy, antialias;
    bool game_fxaa;
    int upscaler, upscale_quality;
    bool async_shaders;
    int render_scale;
} k_gpreset[GPRESET_COUNT] = {
    {DRAWDIST_POTATO, SHADOW_POTATO, false, false, true, 0, AA_OFF, true, UPSCALE_SPATIAL, UPQ_QUALITY, true, RSCALE_50},
    {DRAWDIST_ORIGINAL, SHADOW_CONSOLE, true, true, true, 1 /* 2x */, AA_OFF, true, UPSCALE_DEFAULT, UPQ_QUALITY, true, RSCALE_100},
    {DRAWDIST_MEDIUM, SHADOW_HIGH, true, true, false, 3 /* 8x */, AA_SMAA, false, UPSCALE_DEFAULT, UPQ_QUALITY, false, RSCALE_125},
    {DRAWDIST_HIGH, SHADOW_EXTREME, true, true, false, 4 /* 16x */, AA_TAA_OR_SMAA, false, UPSCALE_DEFAULT, UPQ_QUALITY, false, RSCALE_150},
    {DRAWDIST_ORIGINAL, SHADOW_CONSOLE, true, true, true, 1 /* 2x */, AA_OFF, true, UPSCALE_SPATIAL, UPQ_ULTRA, true, RSCALE_100},
};
const char *const g_gpreset_names[GPRESET_COUNT] = {"Potato", "Original", "High", "Savage", "PC Handheld"};

int graphics_preset_count(void) { return settings_handheld() ? GPRESET_COUNT : GPRESET_HANDHELD; }

int graphics_preset_current(void) {
    for (int i = 0; i < graphics_preset_count(); i++)
        if (g_settings.draw_distance == k_gpreset[i].draw_distance && g_settings.shadow_res == k_gpreset[i].shadow_res &&
            g_settings.bloom == k_gpreset[i].bloom && g_settings.depth_of_field == k_gpreset[i].depth_of_field &&
            g_settings.dynamic_res == k_gpreset[i].dynamic_res && g_settings.anisotropy == k_gpreset[i].anisotropy &&
            g_settings.antialias == k_gpreset[i].antialias && g_settings.game_fxaa == k_gpreset[i].game_fxaa &&
            g_settings.upscaler == k_gpreset[i].upscaler &&
            (g_settings.upscaler == UPSCALE_OFF || g_settings.upscale_quality == k_gpreset[i].upscale_quality) &&
            g_settings.async_shaders == k_gpreset[i].async_shaders && g_settings.render_scale == k_gpreset[i].render_scale)
            return i;
    return -1;
}

void graphics_preset_apply(int preset) {
    if (preset < 0 || preset >= GPRESET_COUNT) return;
    g_settings.draw_distance = k_gpreset[preset].draw_distance;
    g_settings.shadow_res = k_gpreset[preset].shadow_res;
    g_settings.bloom = k_gpreset[preset].bloom;
    g_settings.depth_of_field = k_gpreset[preset].depth_of_field;
    g_settings.dynamic_res = k_gpreset[preset].dynamic_res;
    g_settings.anisotropy = k_gpreset[preset].anisotropy;
    g_settings.antialias = k_gpreset[preset].antialias;
    g_settings.game_fxaa = k_gpreset[preset].game_fxaa;
    g_settings.upscaler = k_gpreset[preset].upscaler;
    if (g_settings.upscaler != UPSCALE_OFF) g_settings.upscale_quality = k_gpreset[preset].upscale_quality;
    g_settings.async_shaders = k_gpreset[preset].async_shaders;
    g_settings.render_scale = k_gpreset[preset].render_scale;
    if (preset == GPRESET_HANDHELD) g_settings.sharpening = 7, g_settings.tex_lod_bias = TEX_LOD_SHARP;  // (the upscaled image's softness)
}
