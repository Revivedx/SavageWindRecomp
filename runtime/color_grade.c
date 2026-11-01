#include "color_grade.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "imageload.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ---- presets: set by eye on BOTW's own picture (Great Plateau, day and evening), from the sliders alone
const ColorPreset g_color_presets[] = {
    {"Natural", 0, 0, 0, 0, 0},
    {"Warm", 0, 5, 5, 35, 5},
    {"Cool", 0, 5, -5, -35, 0},
    {"Vivid", 0, 15, 30, 0, 0},
    {"Cinematic", -2, 25, -15, 15, -5},
    {"Muted", 1, -10, -40, 5, 0},
};
const int g_color_preset_count = (int)(sizeof g_color_presets / sizeof g_color_presets[0]);

int color_preset_current(void) {
    for (int i = 0; i < g_color_preset_count; i++) {
        const ColorPreset *p = &g_color_presets[i];
        if (p->exposure == g_settings.color_exposure && p->contrast == g_settings.color_contrast &&
            p->saturation == g_settings.color_saturation && p->temperature == g_settings.color_temperature &&
            p->tint == g_settings.color_tint)
            return i;
    }
    return -1;
}

void color_preset_apply(int i) {
    if (i < 0 || i >= g_color_preset_count) return;
    const ColorPreset *p = &g_color_presets[i];
    g_settings.color_exposure = p->exposure, g_settings.color_contrast = p->contrast;
    g_settings.color_saturation = p->saturation, g_settings.color_temperature = p->temperature;
    g_settings.color_tint = p->tint;
}

bool color_grade_get(ColorGrade *g) {
    const Settings *s = &g_settings;
    // white balance: temperature moves red against blue, tint green against both (+: magenta), at the same brightness
    float t = s->color_temperature / 100.0f, m = s->color_tint / 100.0f;
    float wb[3] = {1 + 0.3f * t, 1 - 0.2f * m, 1 - 0.3f * t};
    float y = 0.2126f * wb[0] + 0.7152f * wb[1] + 0.0722f * wb[2];
    float ev = exp2f(s->color_exposure / 10.0f);
    for (int i = 0; i < 3; i++) g->gain[i] = ev * wb[i] / y;
    g->contrast = 1 + s->color_contrast / 100.0f;
    g->saturation = 1 + s->color_saturation / 100.0f;
    g->lut_strength = s->color_lut[0] ? s->color_lut_strength / 100.0f : 0;
    return s->color_exposure || s->color_contrast || s->color_saturation || s->color_temperature || s->color_tint ||
           g->lut_strength > 0;
}

// ---- the LUT folders
typedef struct { char name[96], id[128]; } LutEntry;
static LutEntry g_luts[COLOR_LUT_MAX];
static int g_nluts;
static char g_user_dir[1024];

const char *color_lut_folder(void) {
    if (!g_user_dir[0]) snprintf(g_user_dir, sizeof g_user_dir, "%s" SEP "luts", rt_data_root());
    return g_user_dir;
}

static bool lut_file(const char *f) {
    const char *x = strrchr(f, '.');
    if (!x || f[0] == '.') return false;
    char e[8] = "";
    for (int i = 0; x[i] && i < 7; i++) e[i] = (char)tolower((unsigned char)x[i]);
    return !strcmp(e, ".cube") || !strcmp(e, ".png");
}

static int by_name(const void *a, const void *b) { return strcmp(((const LutEntry *)a)->name, ((const LutEntry *)b)->name); }

// dir's LUT files into g_luts, sorted by name (prefix: the id's, "" for the player's)
static void scan(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) return;
    int first = g_nluts;
    struct dirent *e;
    while ((e = readdir(d)) && g_nluts < COLOR_LUT_MAX) {
        if (!lut_file(e->d_name)) continue;
        LutEntry *l = &g_luts[g_nluts++];
        snprintf(l->id, sizeof l->id, "%s%s", prefix, e->d_name);
        snprintf(l->name, sizeof l->name, "%s", e->d_name);
        char *x = strrchr(l->name, '.');
        if (x) *x = 0;
        for (char *c = l->name; *c; c++) if (*c == '_') *c = ' ';
    }
    closedir(d);
    qsort(g_luts + first, (size_t)(g_nluts - first), sizeof g_luts[0], by_name);
}

int color_lut_list(void) {
    g_nluts = 1;
    snprintf(g_luts[0].name, sizeof g_luts[0].name, "None");
    g_luts[0].id[0] = 0;
    char dir[1100];
    if (rt_resource("colour-luts", "third_party/luts", dir, sizeof dir)) scan(dir, "sw:");
    for (int i = 2; i < g_nluts; i++)  // (the recommended one first)
        if (!strcmp(g_luts[i].id, COLOR_LUT_RECOMMENDED)) {
            LutEntry t = g_luts[i];
            memmove(&g_luts[2], &g_luts[1], sizeof g_luts[0] * (size_t)(i - 1));
            g_luts[1] = t;
        }
    const char *user = color_lut_folder();
    mkdir(user, 0755);  // (made for the player to drop files into)
    scan(user, "");
    return g_nluts;
}

const char *color_lut_name(int i) { return i >= 0 && i < g_nluts ? g_luts[i].name : ""; }
const char *color_lut_id(int i) { return i >= 0 && i < g_nluts ? g_luts[i].id : ""; }

int color_lut_index(const char *id) {
    if (!g_nluts) color_lut_list();
    for (int i = 1; id && *id && i < g_nluts; i++)
        if (!strcmp(g_luts[i].id, id)) return i;
    return 0;
}

// ---- reading LUTs
static ColorLut *lut_new(int n) {
    ColorLut *l = calloc(1, sizeof *l);
    l->n = n;
    l->dmax[0] = l->dmax[1] = l->dmax[2] = 1;
    l->rgb = malloc(sizeof(float) * 3 * (size_t)n * n * n);
    return l;
}

void color_lut_free(ColorLut *l) {
    if (!l) return;
    free(l->rgb);
    free(l);
}

// Adobe's .cube: keywords (TITLE, LUT_3D_SIZE, DOMAIN_MIN / MAX), # comments, then n^3 lines of three numbers
#define BAD(why) do { LOG("[colour] %s: " why "\n", path); goto fail; } while (0)
static ColorLut *read_cube(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ColorLut *l = NULL;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    size_t got = 0, want = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\xef' || *s == '\xbb' || *s == '\xbf') s++;  // (a UTF-8 BOM)
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') continue;
        float a, b, c;
        if (!strncmp(s, "LUT_3D_SIZE", 11)) {
            int n = atoi(s + 11);
            if (l || n < 2 || n > 64) BAD("LUT_3D_SIZE not 2..64");
            l = lut_new(n), want = (size_t)n * n * n;
        } else if (!strncmp(s, "LUT_1D_SIZE", 11)) {
            BAD("a 1D LUT (only 3D LUTs are read)");
        } else if (!strncmp(s, "DOMAIN_MIN", 10)) {
            if (sscanf(s + 10, "%f %f %f", &dmin[0], &dmin[1], &dmin[2]) != 3) BAD("bad DOMAIN_MIN");
        } else if (!strncmp(s, "DOMAIN_MAX", 10)) {
            if (sscanf(s + 10, "%f %f %f", &dmax[0], &dmax[1], &dmax[2]) != 3) BAD("bad DOMAIN_MAX");
        } else if (isalpha((unsigned char)*s)) {
            continue;  // (TITLE and other keywords)
        } else if (sscanf(s, "%f %f %f", &a, &b, &c) == 3) {
            if (!l) BAD("data before LUT_3D_SIZE");
            if (got == want) BAD("more data than LUT_3D_SIZE");
            l->rgb[got * 3] = a, l->rgb[got * 3 + 1] = b, l->rgb[got * 3 + 2] = c, got++;
        } else {
            BAD("a line that isn't a keyword or three numbers");
        }
    }
    if (!l) BAD("no LUT_3D_SIZE");
    if (got != want) BAD("less data than LUT_3D_SIZE");
    for (int i = 0; i < 3; i++)
        if (!(dmax[i] > dmin[i])) BAD("DOMAIN_MAX not above DOMAIN_MIN");
    fclose(f);
    memcpy(l->dmin, dmin, sizeof dmin), memcpy(l->dmax, dmax, sizeof dmax);
    return l;
fail:
    fclose(f);
    color_lut_free(l);
    return NULL;
}
#undef BAD

// ReShade's strip: n slices side by side (n*n x n pixels), slice = blue, x in a slice = red, y = green
static ColorLut *read_strip(const char *path) {
    int w, h;
    unsigned char *px = rt_image_rgba(path, &w, &h);
    if (!px) {
        LOG("[colour] %s: %s\n", path, rt_image_error());
        return NULL;
    }
    int n = h;
    if (n < 2 || n > 64 || w != n * n) {
        LOG("[colour] %s: %d x %d is not a LUT strip (N*N x N, e.g. 1024 x 32 or 4096 x 64)\n", path, w, h);
        rt_image_free(px);
        return NULL;
    }
    ColorLut *l = lut_new(n);
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const unsigned char *p = px + ((size_t)g * w + (size_t)b * n + r) * 4;
                float *o = l->rgb + (((size_t)b * n + g) * n + r) * 3;
                o[0] = p[0] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[2] / 255.0f;
            }
    rt_image_free(px);
    return l;
}

// (the path from the id alone: the render thread loads it while the menus may be listing the folders again)
ColorLut *color_lut_load(const char *id) {
    char p[1300], dir[1100];
    if (!id || !*id || strchr(id, '/') || strchr(id, '\\')) return NULL;
    if (!strncmp(id, "sw:", 3)) {
        if (!rt_resource("colour-luts", "third_party/luts", dir, sizeof dir)) return NULL;
        snprintf(p, sizeof p, "%s" SEP "%s", dir, id + 3);
    } else {
        snprintf(p, sizeof p, "%s" SEP "%s", color_lut_folder(), id);
    }
    const char *x = strrchr(p, '.');
    ColorLut *l = x && (x[1] == 'p' || x[1] == 'P') ? read_strip(p) : read_cube(p);
    if (l) LOG("[colour] LUT %s: %d^3\n", p, l->n);
    return l;
}
