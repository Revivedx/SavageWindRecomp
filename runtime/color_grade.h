// Colour grading of the 3D image (Video > Colour; runtime/nvn/vulkan/exec.c scene_post): exposure, contrast,
// saturation, white balance (temperature, tint) and a look-up table (LUT), applied to the 3D scene before the game
// draws its HUD over it, so the HUD keeps its colours. With every setting neutral nothing runs.
//
// LUTs: .cube files (LUT_3D_SIZE 2..64, DOMAIN_MIN / DOMAIN_MAX) and ReShade-style PNG strips (N*N x N: 1024 x 32,
// 4096 x 64), from two folders: the ones that ship with the app (colour-luts/ next to the executable, CC0: README
// "Credits") and the player's own (<data root>/luts/, made at the first listing).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ready-made looks (Video > Colour preset): slider values only, set by eye on BOTW's own picture. 0 = Natural (all
// neutral: grading off).
typedef struct {
    const char *name;
    int exposure, contrast, saturation, temperature, tint;  // (as in Settings: tenths of an EV, percent)
} ColorPreset;
extern const ColorPreset g_color_presets[];
extern const int g_color_preset_count;
int color_preset_current(void);  // the preset the sliders match, or -1 (custom)
void color_preset_apply(int i);  // its slider values into g_settings (the LUT stays)

// The grading the settings ask for: false when it changes nothing (the pass doesn't run).
// gain: linear RGB multipliers (exposure x white balance), contrast: the S curve's slope at mid-grey (1 = none),
// saturation: 1 = as is, 0 = grey; lut_strength 0..1 (0: no LUT).
typedef struct { float gain[3], contrast, saturation, lut_strength; } ColorGrade;
bool color_grade_get(ColorGrade *g);

// The LUTs offered: the shipped ones first (their own names), then the player's (file name without extension),
// rescanned on each call. Index 0 is "None". The setting stores color_lut_id().
enum { COLOR_LUT_MAX = 200 };
#define COLOR_LUT_RECOMMENDED "sw:Savage_Wind.cube"  // Savage Wind's own look (third_party/luts), offered at the setup
#define COLOR_LUT_RECOMMENDED_STRENGTH 50  // (percent) its LUT intensity in the recommended setup
int color_lut_list(void);            // how many (with None)
const char *color_lut_name(int i);   // as shown
const char *color_lut_id(int i);     // as stored in the ini ("" for None; shipped: "sw:<file>", the player's: "<file>")
int color_lut_index(const char *id); // 0 (None) when not listed
const char *color_lut_folder(void);  // the player's folder (absolute)

// A LUT read from its file: n^3 RGB triples, red fastest, then green, then blue (as in .cube files); inputs map from
// dmin..dmax. NULL with a log line when the file can't be read. Free with color_lut_free.
typedef struct { int n; float dmin[3], dmax[3]; float *rgb; } ColorLut;
ColorLut *color_lut_load(const char *id);
void color_lut_free(ColorLut *l);

#ifdef __cplusplus
}
#endif
