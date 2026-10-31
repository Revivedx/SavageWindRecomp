// Control settings use the same field updates as ui/game_options.mm. Window changes are queued to the
// window loop; Vulkan notices VSync/HDR changes at frame begin and recreates its swapchain there.
#include "control.h"
#include "settings.h"
#include "language.h"
#include "nvn/output_scale.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void menu_request_window_apply(void);

typedef enum { BOOL, INT, FLOAT, TEXT } Kind;
typedef struct {
    const char *key;
    void *value;
    Kind kind;
    double min, max;
    const char *names; // enum names in numeric order, separated by '|'
} Setting;
#define B(k) {#k, &g_settings.k, BOOL, 0, 1, "off|on"}
#define I(k, lo, hi, names) {#k, &g_settings.k, INT, lo, hi, names}
#define F(k, lo, hi) {#k, &g_settings.k, FLOAT, lo, hi, NULL}
static const Setting settings[] = {
    B(fullscreen), I(window_w, 0, 16384, NULL), I(window_h, 0, 16384, NULL),
    I(upscaler, 0, UPSCALE_COUNT - 1, "off|metalfx|fsr1|fsr3|dlss"),
    I(upscale_quality, 0, UPQ_COUNT - 1, "ultra_quality|quality|balanced|performance|ultra_performance"),
    I(antialias, 0, AA_TAA, "off|smaa|taa"),
    I(anisotropy, 0, 4, "game|2x|4x|8x|16x"), I(sharpening, 0, 10, NULL),
    B(frame_gen), F(volume, 0, 2), B(reverb), B(movie_audio),
    I(perf_overlay, 0, PERF_FULL, "off|fps|full"), I(fps_limit, 0, 120, NULL),
    I(vsync, 0, VSYNC_COUNT - 1, "on|fast|adaptive|off"), B(dpad_move),
    {"stick_deadzone[0]", &g_settings.stick_deadzone[0], FLOAT, 0, .5, NULL},
    {"stick_deadzone[1]", &g_settings.stick_deadzone[1], FLOAT, 0, .5, NULL},
    B(mouse_camera), F(mouse_sens, .1, 10), F(mouse_sens_y, .5, 2), B(mouse_invert_y),
    F(motion_sens, .1, 10), I(motion_invert, 0, 3, "off|up_down|left_right|both"),
    I(button_icons, 0, ICONS_COUNT - 1, "auto|keyboard|xbox|playstation|nintendo"),
    B(async_shaders), B(precompile_shaders),
    I(shadow_res, 0, SHADOW_COUNT - 1, "console|high|ultra|extreme"), B(wide_view),
    I(hdr, 0, HDR_COUNT - 1, "auto|on|off"), I(hdr_white, 0, 1000, NULL),
    I(fov, 50, 200, NULL), I(color_exposure, -20, 20, NULL), I(color_contrast, -50, 50, NULL),
    I(color_saturation, -100, 100, NULL), I(color_temperature, -100, 100, NULL),
    I(color_tint, -100, 100, NULL),
    {"color_lut", g_settings.color_lut, TEXT, 0, 0, NULL},
    I(color_lut_strength, 0, 100, NULL), B(color_lut_set),
    I(text_language, 0, 0, NULL), I(voice_language, 0, 0, NULL), B(menu_tip_seen),
};
#undef B
#undef I
#undef F

static const Setting *find(const char *key) {
    if (!strcmp(key, "fps")) key = "fps_limit";
    if (!strcmp(key, "left_stick_deadzone")) key = "stick_deadzone[0]";
    if (!strcmp(key, "right_stick_deadzone")) key = "stick_deadzone[1]";
    for (size_t i = 0; i < sizeof settings / sizeof *settings; i++)
        if (!strcmp(key, settings[i].key)) return &settings[i];
    return NULL;
}
static bool is(const Setting *s, const char *key) { return !strcmp(s->key, key); }

static int named(const char *names, const char *value) {
    if (!names) return -1;
    for (int i = 0; ; i++) {
        const char *end = strchr(names, '|');
        size_t n = end ? (size_t)(end - names) : strlen(names);
        if (strlen(value) == n && !strncmp(names, value, n)) return i;
        if (!end) return -1;
        names = end + 1;
    }
}
static void emit(const Setting *s, ControlReply *r) {
    if (s->kind == TEXT) { control_field_str(r, s->key, s->value); return; }
    if (s->kind == BOOL) { control_field_bool(r, s->key, *(bool *)s->value); return; }
    if (s->kind == FLOAT) { control_field_num(r, s->key, *(float *)s->value); return; }
    int v = *(int *)s->value;
    const char *name = s->names;
    if (name && v >= 0 && v <= s->max) {
        for (int i = 0; i < v; i++) name = strchr(name, '|') + 1;
        size_t n = strcspn(name, "|");
        char buf[64];
        memcpy(buf, name, n); buf[n] = 0;
        control_field_str(r, s->key, buf);
    } else control_field_int(r, s->key, v);
}
static void window_size_reply(ControlReply *r) {
    char buf[48];
    if (!g_settings.window_w && !g_settings.window_h) strcpy(buf, "default");
    else snprintf(buf, sizeof buf, "%dx%d", g_settings.window_w, g_settings.window_h);
    control_field_str(r, "window_size", buf);
}
static void cmd_get(int argc, char **argv, ControlReply *r) {
    if (argc > 2) { control_fail(r, "Usage: get [key]"); return; }
    if (argc == 2) {
        if (!strcmp(argv[1], "window_size")) { window_size_reply(r); return; }
        const Setting *s = find(argv[1]);
        if (!s) control_fail(r, "Unknown setting: %s", argv[1]);
        else emit(s, r);
    } else {
        for (size_t i = 0; i < sizeof settings / sizeof *settings; i++) emit(&settings[i], r);
        window_size_reply(r);
    }
}
static void cmd_set(int argc, char **argv, ControlReply *r) {
    if (argc != 3) { control_fail(r, "Usage: set <key> <value>"); return; }
    if (!strcmp(argv[1], "window_size")) {
        int w = 0, h = 0;
        if (strcmp(argv[2], "default")) {
            char *end;
            errno = 0;
            long width = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != 'x' || errno || width < 320 || width > 16384) {
                control_fail(r, "window_size must be default or WIDTHxHEIGHT (320 to 16384 by 200 to 16384)"); return;
            }
            const char *height_start = end + 1;
            errno = 0;
            long height = strtol(height_start, &end, 10);
            if (end == height_start || *end || errno || height < 200 || height > 16384) {
                control_fail(r, "window_size must be default or WIDTHxHEIGHT (320 to 16384 by 200 to 16384)"); return;
            }
            w = (int)width; h = (int)height;
        }
        g_settings.window_w = w; g_settings.window_h = h;
        menu_request_window_apply();
        window_size_reply(r);
        control_field_str(r, "applied", "live");
        return;
    }
    const Setting *s = find(argv[1]);
    if (!s) { control_fail(r, "Unknown setting: %s", argv[1]); return; }
    // These need paired window dimensions, file discovery, or are bookkeeping rather than menu options.
    if (is(s, "window_w") || is(s, "window_h")) {
        control_fail(r, "Width and height must change together: use window_size WIDTHxHEIGHT or default"); return;
    }
    if (s->kind == TEXT) {
        control_fail(r, "LUT selection is only available in the Colour menu"); return;
    }
    if (is(s, "color_lut_set") || is(s, "menu_tip_seen")) {
        control_fail(r, "%s is read-only", s->key); return;
    }
#ifdef __APPLE__
    if (is(s, "anisotropy") || is(s, "sharpening") || is(s, "frame_gen") ||
        !strncmp(s->key, "color_", 6) || is(s, "hdr") || is(s, "hdr_white") || is(s, "vsync")) {
        control_fail(r, "%s is not supported by this backend", s->key); return;
    }
#endif
    double value;
    int n = named(s->names, argv[2]);
    if (s->kind == BOOL && !strcmp(argv[2], "true")) n = 1;
    if (s->kind == BOOL && !strcmp(argv[2], "false")) n = 0;
    if (is(s, "fps_limit") && (!strcmp(argv[2], "off") || !strcmp(argv[2], "unlimited"))) n = 0;
    bool text = is(s, "text_language"), voice = is(s, "voice_language");
    int languages = text ? language_text_count() : voice ? g_nvoice_languages : 0;
    if (text || voice) {
        if (!strcmp(argv[2], text ? "auto" : "text")) n = 0;
        for (int i = 1; i < languages; i++) {
            if (!strcmp(argv[2], text ? g_text_languages[i].folder : g_voice_languages[i].folder) ||
                !strcmp(argv[2], text ? g_text_languages[i].name : g_voice_languages[i].name)) n = i;
        }
    }
    if (n >= 0) value = n;
    else {
        char *end;
        errno = 0;
        value = strtod(argv[2], &end);
        if (end == argv[2] || *end || errno || !isfinite(value)) {
            control_fail(r, "%s must be %s", s->key, s->names ? s->names :
                         text ? "a text language index or folder (auto for system)" :
                         voice ? "a voice language index or folder (text to follow text)" : "a finite number"); return;
        }
    }
    double max = languages ? languages - 1 : s->max;
    if (value < s->min || value > max || (s->kind != FLOAT && value != floor(value))) {
        control_fail(r, "%s must be %s from %g to %g%s%s", s->key,
                     s->kind == FLOAT ? "a number" : "an integer", s->min, max,
                     s->names ? " or " : "", s->names ? s->names : ""); return;
    }
    int v = (int)value;
    if (is(s, "fps_limit") && v != 0 && v != 30 && v != 60 && v != 90 && v != 120) {
        control_fail(r, "fps must be 0 (unlimited), 30, 60, 90 or 120"); return;
    }
    if (is(s, "hdr_white") && v != 0 && v < 80) {
        control_fail(r, "hdr_white must be 0 (automatic) or 80 to 1000 nits"); return;
    }
    if (is(s, "upscaler") && !upscaler_available(v)) {
        control_fail(r, "Upscaler unavailable"); return;
    }
    if (is(s, "upscale_quality") && !upscale_modes_available()) {
        control_fail(r, "Quality modes unavailable"); return;
    }
#ifdef __APPLE__
    if (is(s, "antialias") && v == AA_TAA) {
        control_fail(r, "TAA requires the Vulkan backend"); return;
    }
#endif
    if (s->kind == BOOL) *(bool *)s->value = value != 0;
    else if (s->kind == FLOAT) *(float *)s->value = (float)value;
    else *(int *)s->value = v;
    if (is(s, "fullscreen")) menu_request_window_apply();
    // Do not call settings_save(): file I/O on the presenting thread would block. A menu save may persist these.
    emit(s, r);
    control_field_str(r, "applied", text || voice || is(s, "precompile_shaders") ? "needs_restart" : "live");
}
void control_settings_init(void) {
    static const ControlCmd get[] = {
        {"get", "[key]", "List the settings or show one", cmd_get, CONTROL_PLAYER},
    };
    static const ControlCmd set[] = {
        {"set", "<key> <value>", "Change a setting", cmd_set, 0},
    };
    control_register(get, 1);
    if (control_enabled()) control_register(set, 1);
}
