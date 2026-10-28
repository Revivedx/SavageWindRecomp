#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "language.h"

#define SETTINGS_FILE settings_file()

// SAVAGEWIND_SETTINGS=<path>: another settings file (test runs never touch the player's savagewind.ini)
static const char *settings_file(void) {
    const char *e = getenv("SAVAGEWIND_SETTINGS");
    return e && *e ? e : "savagewind.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .upscaler = UPSCALE_DEFAULT,  // (Off: the window's resolution; MetalFX on macOS)
    .upscale_quality = UPQ_QUALITY,
    .mouse_camera = true,
    .pc_cursor = true,
    .pc_bow = true,
    .mouse_sens = MOUSE_SENS_DEFAULT,
    .mouse_sens_y = 1.0f,
    .motion_sens = 1.0f,
    .stick_deadzone = {STICK_DEADZONE_DEFAULT, STICK_DEADZONE_DEFAULT},
    .fps_limit = FPS_DEFAULT,
    .precompile_shaders = true,
    .vsync = VSYNC_DEFAULT,
    .wide_view = true,
    .fov = 100,
    .color_lut_strength = 100,
};

static const char *const k_vsync[VSYNC_COUNT] = {"on", "fast", "adaptive", "off"};  // (VSYNC_*: the ini's words)
static const char *const k_hdr[HDR_COUNT] = {"auto", "on", "off"};             // (HDR_*)

// settings_version 2 (2026-10-19): the upscaler's mode, the window size and the 300% mouse default. Older files: their
// "resolution" (the internal resolution, removed) gives the upscaler's mode, and a mouse sensitivity at the old
// default (100%) becomes the new one.
// settings_version 3 (2026-10-20): the upscaler sets the 3D image's resolution (AMD FSR 1 renders it smaller). Before,
// "fsr1" (the default) only scaled the finished 1920x1080 image to a bigger window: older files load it as Off, so
// nobody drops below the window's resolution unasked, except version-1 files whose internal resolution was a share of
// the display (75%, 50%, 25%: they keep FSR 1 at the nearest mode).
#define SETTINGS_VERSION 3

static const char *const k_icons[ICONS_COUNT] = {"auto", "keyboard", "xbox", "playstation", "nintendo"};

static const char *const k_quality[UPQ_COUNT] = {"ultra_quality", "quality", "balanced", "performance"};

// Debug overrides seed this boot's options; the Controls switches can still change them at once.
static void pc_controls_env(void) {
    const char *cursor = getenv("SAVAGEWIND_PC_CURSOR"), *bow = getenv("SAVAGEWIND_PC_BOW");
    if (cursor) g_settings.pc_cursor = atoi(cursor) != 0;
    if (bow) g_settings.pc_bow = atoi(bow) != 0;
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) { pc_controls_env(); return; }
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
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.perf_overlay = atoi(val) ? PERF_FPS : PERF_OFF;  // (older files)
        else if (!strcmp(key, "perf_overlay")) g_settings.perf_overlay = !strcmp(val, "full") ? PERF_FULL : !strcmp(val, "fps") ? PERF_FPS : PERF_OFF;
        else if (!strcmp(key, "frame_rate")) g_settings.fps_limit = !strcmp(val, "off") ? 0 : atoi(val);  // (fps / off)
        else if (!strcmp(key, "vsync")) {
            for (int i = 0; i < VSYNC_COUNT; i++)
                if (!strcmp(val, k_vsync[i])) g_settings.vsync = i;
        }
        else if (!strcmp(key, "settings_version")) version = atoi(val);
        else if (!strcmp(key, "resolution")) snprintf(old_res, sizeof old_res, "%s", val);  // (version 1)
        else if (!strcmp(key, "upscaler"))  // (metalfx / fsr1: this OS's spatial upscaler; fsr3 / dlss: not yet)
            g_settings.upscaler = !strcmp(val, "fsr1") || !strcmp(val, "metalfx") ? UPSCALE_SPATIAL : UPSCALE_OFF;
        else if (!strcmp(key, "upscaler_quality")) {
            for (int i = 0; i < UPQ_COUNT; i++)
                if (!strcmp(val, k_quality[i])) g_settings.upscale_quality = i;
        }
        else if (!strcmp(key, "window_size")) {  // (WxH in pixels, or default)
            int w = 0, h = 0;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384)
                g_settings.window_w = w, g_settings.window_h = h;
        }
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;  // ("fxaa": SMAA now)
        else if (!strcmp(key, "anisotropy")) {  // (game, 2x .. 16x)
            int a = atoi(val);
            g_settings.anisotropy = a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
        }
        else if (!strcmp(key, "frame_generation")) g_settings.frame_gen = atoi(val) != 0;
        else if (!strcmp(key, "sharpening")) {  // (percent, in steps of 10)
            int p = atoi(val);
            g_settings.sharpening = p < 0 ? 0 : p > 100 ? 10 : (p + 5) / 10;
        }
        else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
        else if (!strcmp(key, "left_stick_deadzone")) g_settings.stick_deadzone[0] = (float)atof(val) / 100;  // (percent)
        else if (!strcmp(key, "right_stick_deadzone")) g_settings.stick_deadzone[1] = (float)atof(val) / 100;
        else if (!strcmp(key, "mouse_camera")) g_settings.mouse_camera = atoi(val) != 0;
        else if (!strcmp(key, "pc_cursor")) g_settings.pc_cursor = atoi(val) != 0;
        else if (!strcmp(key, "pc_bow")) g_settings.pc_bow = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sensitivity")) g_settings.mouse_sens = (float)atof(val);
        else if (!strcmp(key, "mouse_vertical_sensitivity")) g_settings.mouse_sens_y = (float)atof(val);
        else if (!strcmp(key, "mouse_invert_y")) g_settings.mouse_invert_y = atoi(val) != 0;
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
        else if (!strcmp(key, "async_shaders")) g_settings.async_shaders = atoi(val) != 0;
        else if (!strcmp(key, "precompile_shaders")) g_settings.precompile_shaders = atoi(val) != 0;
        else if (!strcmp(key, "wide_view")) g_settings.wide_view = atoi(val) != 0;
        else if (!strcmp(key, "hdr")) {
            for (int i = 0; i < HDR_COUNT; i++)
                if (!strcmp(val, k_hdr[i])) g_settings.hdr = i;
        }
        else if (!strcmp(key, "hdr_paper_white")) {  // (nits; 0: Windows' SDR content brightness)
            int n = atoi(val);
            g_settings.hdr_white = n >= 80 && n <= 1000 ? n : 0;
        }
        else if (!strcmp(key, "field_of_view")) g_settings.fov = atoi(val);  // (percent)
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
        else if (!strcmp(key, "shadow_resolution"))  // (console / high / ultra / extreme)
            g_settings.shadow_res = !strcmp(val, "extreme") ? SHADOW_EXTREME : !strcmp(val, "ultra") ? SHADOW_ULTRA
                                  : !strcmp(val, "high") ? SHADOW_HIGH : SHADOW_CONSOLE;
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
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    if (g_settings.fps_limit < 0) g_settings.fps_limit = 0;
    if (g_settings.fps_limit > 0) {  // (a frame rate not offered: the nearest one that is)
        static const int fps[] = FPS_CHOICES;
        int best = FPS_DEFAULT;
        for (unsigned i = 1; i < sizeof fps / sizeof fps[0]; i++)
            if (abs(fps[i] - g_settings.fps_limit) < abs(best - g_settings.fps_limit)) best = fps[i];
        g_settings.fps_limit = best;
    }
    if (!(g_settings.mouse_sens >= 0.1f && g_settings.mouse_sens <= 10.0f)) g_settings.mouse_sens = MOUSE_SENS_DEFAULT;
    if (!(g_settings.mouse_sens_y >= 0.5f && g_settings.mouse_sens_y <= 2.0f)) g_settings.mouse_sens_y = 1.0f;
    if (!(g_settings.motion_sens >= 0.1f && g_settings.motion_sens <= 10.0f)) g_settings.motion_sens = 1.0f;
    if (g_settings.fov < 50 || g_settings.fov > 200) g_settings.fov = 100;
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    g_settings.color_exposure = CLAMP(g_settings.color_exposure, -20, 20);
    g_settings.color_contrast = CLAMP(g_settings.color_contrast, -50, 50);
    g_settings.color_saturation = CLAMP(g_settings.color_saturation, -100, 100);
    g_settings.color_temperature = CLAMP(g_settings.color_temperature, -100, 100);
    g_settings.color_tint = CLAMP(g_settings.color_tint, -100, 100);
    g_settings.color_lut_strength = CLAMP(g_settings.color_lut_strength, 0, 100);
#undef CLAMP
    for (int i = 0; i < 2; i++)
        if (!(g_settings.stick_deadzone[i] >= 0 && g_settings.stick_deadzone[i] <= 0.5f))
            g_settings.stick_deadzone[i] = STICK_DEADZONE_DEFAULT;
    pc_controls_env();
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# Savage Wind settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "settings_version = %d\n", SETTINGS_VERSION);
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    if (g_settings.window_w > 0 && g_settings.window_h > 0)
        fprintf(f, "window_size = %dx%d\n", g_settings.window_w, g_settings.window_h);
    else
        fprintf(f, "window_size = default\n");
    if (g_settings.fps_limit > 0)
        fprintf(f, "frame_rate = %d\n", g_settings.fps_limit);
    else
        fprintf(f, "frame_rate = off\n");
    int vs = g_settings.vsync >= 0 && g_settings.vsync < VSYNC_COUNT ? g_settings.vsync : VSYNC_DEFAULT;
    fprintf(f, "vsync = %s\n", k_vsync[vs]);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "perf_overlay = %s\n", g_settings.perf_overlay == PERF_FULL ? "full" : g_settings.perf_overlay == PERF_FPS ? "fps" : "off");
    static const char *const ups[UPSCALE_COUNT] = {"off", "metalfx", "fsr1", "fsr3", "dlss"};
    int u = g_settings.upscaler, uq = g_settings.upscale_quality;
    fprintf(f, "upscaler = %s\n", u >= 0 && u < UPSCALE_COUNT ? ups[u] : "off");
    fprintf(f, "upscaler_quality = %s\n", k_quality[uq >= 0 && uq < UPQ_COUNT ? uq : UPQ_QUALITY]);
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    int an = g_settings.anisotropy;
    if (an >= 1 && an <= 4) fprintf(f, "anisotropy = %dx\n", 1 << an);
    else fprintf(f, "anisotropy = game\n");
    fprintf(f, "sharpening = %d\n", g_settings.sharpening * 10);
    fprintf(f, "frame_generation = %d\n", g_settings.frame_gen ? 1 : 0);
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "left_stick_deadzone = %.0f\n", g_settings.stick_deadzone[0] * 100);
    fprintf(f, "right_stick_deadzone = %.0f\n", g_settings.stick_deadzone[1] * 100);
    fprintf(f, "mouse_camera = %d\n", g_settings.mouse_camera);
    fprintf(f, "pc_cursor = %d\n", g_settings.pc_cursor);
    fprintf(f, "pc_bow = %d\n", g_settings.pc_bow);
    fprintf(f, "mouse_sensitivity = %.2f\n", g_settings.mouse_sens);
    fprintf(f, "mouse_vertical_sensitivity = %.2f\n", g_settings.mouse_sens_y);
    fprintf(f, "mouse_invert_y = %d\n", g_settings.mouse_invert_y);
    fprintf(f, "motion_sensitivity = %.2f\n", g_settings.motion_sens);
    static const char *const inv[4] = {"off", "up_down", "left_right", "both"};
    fprintf(f, "motion_invert = %s\n", inv[g_settings.motion_invert & 3]);
    fprintf(f, "button_icons = %s\n", k_icons[g_settings.button_icons >= 0 && g_settings.button_icons < ICONS_COUNT ?
                                                g_settings.button_icons : ICONS_AUTO]);
    fprintf(f, "async_shaders = %d\n", g_settings.async_shaders);
    fprintf(f, "precompile_shaders = %d\n", g_settings.precompile_shaders);
    static const char *const shadow[SHADOW_COUNT] = {"console", "high", "ultra", "extreme"};
    int sr = g_settings.shadow_res;
    fprintf(f, "shadow_resolution = %s\n", shadow[sr >= 0 && sr < SHADOW_COUNT ? sr : SHADOW_CONSOLE]);
    fprintf(f, "wide_view = %d\n", g_settings.wide_view);
    fprintf(f, "hdr = %s\n", k_hdr[g_settings.hdr >= 0 && g_settings.hdr < HDR_COUNT ? g_settings.hdr : HDR_AUTO]);
    if (g_settings.hdr_white) fprintf(f, "hdr_paper_white = %d\n", g_settings.hdr_white);
    fprintf(f, "field_of_view = %d\n", g_settings.fov);
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
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    bind_settings_save(f);
    fclose(f);
}
