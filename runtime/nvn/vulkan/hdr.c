// HDR support for Breath of the Wild's tonemap (Video > HDR).
//
// BOTW renders the 3D scene and bloom into two B10G11R11 float images, then its tonemap (fragment shader 72cb7ff600)
// adds them, applies the game's own 1 - e^-x curve and saturation grade, and writes a 1600x900 A2B10G10R10 image. Its
// upscale/FXAA pass (72caebd800) brings that to the 1920x1080 final image, the HUD is drawn into it, and the game copies
// it sRGB-encoded into the 8-bit image it presents. Everything above 1.0 is gone after the tonemap. With HDR on:
//   - the scene's 1600x900 A2B10G10R10 tonemap target and the 1920x1080 final image are created as RGBA16F
//     (textures.c, vk_hdr_targets_wanted), so the extended curve and the HUD keep values above white;
//   - the tonemap program, recognised at its first draw by its own curve constants and its two float scene/bloom
//     textures, runs RenoDX's extended vanilla curve (the game's own curve to a pivot, then a straight continuation),
//     the game's own saturation grade and a MacLeod-Boynton hue/purity transfer from the game's SDR result, then
//     either the RenoDX BOTW mod's PsychoV test17 with its per-channel Neutwo peak fit (the default, the mod's
//     "RenoDRT" option) or the game's own clipped tonemap (Vanilla);
//   - the last copy into the presented 8-bit image is recorded, and device.c presents the float image it read with the
//     scene at hdr_game_nits, the HUD at hdr_ui_nits, and everything capped by hdr_peak_nits.
// The tonemap approach follows the RenoDX TotK port by ProSep (valentderah/totk-pc-recomp#281) and the RenoDX BOTW mod
// (github.com/clshortfuse/renodx, src/games/botw, MIT): game mod by souperman9, tonemapper by Musa, RenoDX by
// ShortFuse (Carlos Lopez Jr.), RenoVK fork by Ritsu. The MacLeod-Boynton hue/purity math is adapted from its
// macleod_boynton.hlsli (MIT); the rest of the code here is ours.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"
#include "hdr_psycho.h"
#include "vk.h"

static float nits_or(int value, float fallback) {
    return value >= 80 ? (float)value : fallback;
}

void vk_hdr_nits(float *peak, float *game, float *ui) {
    float g = nits_or(g_settings.hdr_game_nits, 203.0f);
    float p = nits_or(g_settings.hdr_peak_nits, 1000.0f);
    if (g > p) g = p;  // the display peak is the final cap: a brighter scene white is capped by it
    float u = g_settings.hdr_ui_nits >= 80 ? (float)g_settings.hdr_ui_nits : g;  // (0: the scene's white)
    *peak = p, *game = g, *ui = u;
}

// The live HDR tonemap parameters: hdr.c's generated shader reads them from the push constant block, so changing
// a setting or a preset applies on the next HDR tonemap draw instead of at the next start.
void vk_hdr_params(VkHdrParams *p) {
    float peak, game, ui;
    vk_hdr_nits(&peak, &game, &ui);
    int hp = g_settings.hdr_hue;
    if (hp < 0) hp = 0;
    if (hp > 100) hp = 100;
    int bp = g_settings.hdr_blowout;
    if (bp < 0) bp = 0;
    if (bp > 100) bp = 100;
    int sp = g_settings.hdr_sdr_blend;
    if (sp < 0) sp = 0;
    if (sp > 60) sp = 60;
    int vp = g_settings.hdr_vanilla_saturation;
    if (vp < 0) vp = 0;
    if (vp > 100) vp = 100;
    int bl = g_settings.hdr_bloom;
    if (bl < 0) bl = 0;
    if (bl > 100) bl = 100;
    int ep = g_settings.hdr_exposure;
    if (ep < 50) ep = 50;
    if (ep > 200) ep = 200;
    int hp2 = g_settings.hdr_highlights;
    if (hp2 < 0) hp2 = 0;
    if (hp2 > 100) hp2 = 100;
    int shp = g_settings.hdr_shadows;
    if (shp < 0) shp = 0;
    if (shp > 100) shp = 100;
    int cp = g_settings.hdr_contrast;
    if (cp < 0) cp = 0;
    if (cp > 100) cp = 100;
    int sap = g_settings.hdr_saturation;
    if (sap < 0) sap = 0;
    if (sap > 100) sap = 100;
    int hsp = g_settings.hdr_highlight_saturation;
    if (hsp < 0) hsp = 0;
    if (hsp > 100) hsp = 100;
    int fp = g_settings.hdr_flare;
    if (fp < 0) fp = 0;
    if (fp > 100) fp = 100;
    int gm = g_settings.hdr_gamma;
    if (gm < 0 || gm > 2) gm = 0;
    p->v[0] = ui > 0.0f ? game / ui : 1.0f;  // (the scene's white relative to the HUD's: 1.0 at both 203)
    p->v[1] = game > 0.0f ? peak / game : 1.0f;
    p->v[2] = sp / 100.0f;
    p->v[3] = hp / 100.0f;
    p->v[4] = bp / 100.0f;
    p->v[5] = vp / 100.0f;
    p->v[6] = bl / 100.0f;
    p->v[7] = g_settings.hdr_clamp == 1 ? 1.0f : 0.0f;
    p->v[8] = g_settings.hdr_tonemapper == 0 ? 0.0f : 1.0f;
    p->v[9] = ep / 100.0f;
    p->v[10] = hp2 * 0.02f;
    p->v[11] = shp * 0.02f;
    p->v[12] = cp * 0.02f;
    p->v[13] = sap * 0.02f;
    p->v[14] = 1.0f - hsp * 0.02f;  // (the mod's -(ColorGradeHighlightSaturation - 1))
    // (the mod's BuildPsycho17Config: config.flare = 0.10 * pow(RENODX_TONE_MAP_FLARE, 10), parsed 0..2)
    p->v[15] = 0.10f * powf(fp * 0.02f, 10.0f);
    p->v[16] = (float)gm;
    p->v[17] = p->v[18] = p->v[19] = 0.0f;
}

// ---- the float picture the game's present copy reads

typedef struct { VkImage win, src; VkFormat fmt; uint32_t w, h; } Rec;
static Rec recs[16];
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;

void vk_hdr_record(VkImage win, VkImage src, VkFormat fmt, uint32_t w, uint32_t h) {
    if (!win) return;
    pthread_mutex_lock(&rec_lock);
    int at = -1, spare = -1;
    for (int i = 0; i < 16; i++) {
        if (recs[i].win == win) at = i;
        if (!recs[i].win && spare < 0) spare = i;
    }
    if (!src) {
        if (at >= 0) recs[at] = (Rec){0};
    } else {
        if (at < 0) at = spare;
        if (at >= 0) recs[at] = (Rec){win, src, fmt, w, h};
    }
    pthread_mutex_unlock(&rec_lock);
}

bool vk_hdr_source(VkImage win, VkImage *src, VkFormat *fmt, uint32_t *w, uint32_t *h) {
    bool found = false;
    pthread_mutex_lock(&rec_lock);
    for (int i = 0; i < 16; i++)
        if (recs[i].win == win && recs[i].src) {
            *src = recs[i].src, *fmt = recs[i].fmt, *w = recs[i].w, *h = recs[i].h;
            found = true;
            break;
        }
    pthread_mutex_unlock(&rec_lock);
    return found;
}

void vk_hdr_image_gone(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&rec_lock);
    for (int i = 0; i < 16; i++) {
        if (recs[i].src == img) recs[i].src = VK_NULL_HANDLE;
        if (recs[i].win == img) recs[i] = (Rec){0};
    }
    pthread_mutex_unlock(&rec_lock);
}

// ---- the replacement tonemap

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static bool has_register(const char *s) {
    for (; s && *s; s++)
        if (*s == 'r' && s[1] >= '0' && s[1] <= '9') return true;
    return false;
}

// The coordinate argument of the first texture()/textureLod() read of t<slot> in body. The translator emits
// `texture(t0, vec2(...))` (the source is straight-line GLSL), so the second argument is the UV expression.
static char *sample_arg(const char *body, int slot, const char **at) {
    char tex[32], lod[32];
    snprintf(tex, sizeof tex, "texture(t%d", slot);
    snprintf(lod, sizeof lod, "textureLod(t%d", slot);
    const char *p = strstr(body, lod), *needle = lod;
    int is_lod = p != NULL;
    if (!p) p = strstr(body, tex), needle = tex;
    while (p && p[strlen(needle)] >= '0' && p[strlen(needle)] <= '9') p = strstr(p + 1, needle);
    if (!p) return NULL;
    if (at) *at = p;
    const char *open = strchr(p, '(');
    if (!open) return NULL;
    int depth = 1;
    const char *comma = NULL, *second = NULL, *end = NULL;
    for (const char *q = open + 1; *q; q++) {
        if (*q == '(') depth++;
        else if (*q == ')') {
            if (--depth == 0) { end = q; break; }
        } else if (*q == ',' && depth == 1) {
            if (!comma) comma = q;
            else if (!second) second = q;
        }
    }
    if (!comma || !end) return NULL;
    const char *a = skip_space(comma + 1), *b = is_lod && second ? second : end;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
    size_t n = (size_t)(b - a);
    if (!n) return NULL;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, a, n);
    out[n] = 0;
    return out;
}

// The extended-vanilla curve: the game's 1 - e^-x up to the pivot, its tangent there above. BOTW's own tonemap is
// exactly the RenoDX TotK commit's vanilla: luminance and each channel by 1 - e^-x, blended by the luminance curve
// squared. The pivot is hdr_sdr_blend (18%: RenoDX BOTW's default).
//
// The hue/purity transfer is a compact port of RenoDX's MacLeod-Boynton correction (macleod_boynton.hlsli,
// CorrectHueAndPurityMBBT709WithBT2020, MIT): the extended colour takes its hue angle and purity from the game's own
// SDR result, in MacLeod-Boynton chromaticity, holding its luminance. The BT.709/2020 and XYZ_TO_LMS matrices are the
// reference's; the purity/gamut interval is its ApplyBT2020 normalized-purity path.
static const char k_mbbt[] =
    "const mat3 SW_709_TO_XYZ = mat3(\n"
    "  0.41239080, 0.21263901, 0.01933082,\n"
    "  0.35758434, 0.71516868, 0.11919478,\n"
    "  0.18048079, 0.07219232, 0.95053215);\n"
    "const mat3 SW_XYZ_TO_709 = mat3(\n"
    "   3.240969956887, -0.969243657972,  0.055630078074,\n"
    "  -1.537383184825,  1.875967510611, -0.203976958694,\n"
    "  -0.498610761712,  0.041555053406,  1.056971517940);\n"
    "const mat3 SW_2020_TO_XYZ = mat3(\n"
    "  0.63695805, 0.26270021, 0.00000000,\n"
    "  0.14461690, 0.67799807, 0.02807269,\n"
    "  0.16888098, 0.05930172, 1.06098506);\n"
    "const mat3 SW_XYZ_TO_2020 = mat3(\n"
    "   1.716651178394, -0.666684344427, 0.017639855295,\n"
    "  -0.355670774379,  1.616481235365, -0.042770608486,\n"
    "  -0.253366286469,  0.015768542332,  0.942103119340);\n"
    "const mat3 SW_XYZ_TO_LMS = mat3(\n"
    "   0.185082982238733, -0.134433056469973,  0.000789456671966863,\n"
    "   0.584081279463687,  0.405752392775348, -0.000912281325916184,\n"
    "  -0.0240722415044404,  0.0358252602217631, 0.0198490812339463);\n"
    "const mat3 SW_LMS_TO_XYZ = mat3(\n"
    "   2.628474773948,  0.876534283734, -0.064255925692,\n"
    "  -3.761263499280,  1.200303808552,  0.204763599211,\n"
    "   9.976357133974, -1.103378592865, 49.932664133489);\n"
    "const vec3 SW_D65_XYZ = vec3(0.950455927, 1.0, 1.089057751);\n"
    "vec2 sw_mb_from_lms(vec3 lms) {\n"
    "  float t = lms.x + lms.y;\n"
    "  return t > 0.0 ? vec2(lms.x / t, lms.z / t) : vec2(0.0);\n"
    "}\n"
    "vec3 sw_lms_from_mb(vec2 mb, float t) {\n"
    "  return vec3(mb.x * t, (1.0 - mb.x) * t, mb.y * t);\n"
    "}\n"
    "vec2 sw_interval_lower0(float a, float b) {\n"
    "  if (abs(a) < 1e-20) return b >= 0.0 ? vec2(-1e30, 1e30) : vec2(1.0, 0.0);\n"
    "  float t0 = -b / a;\n"
    "  return a > 0.0 ? vec2(t0, 1e30) : vec2(-1e30, t0);\n"
    "}\n"
    "// The colour at purity01 along its own white -> hue ray to the BT.2020 gamut edge (the reference's ApplyBT2020\n"
    "// normalized-purity path, curve gamma 1); purity_cur is the input's own purity in 0..1.\n"
    "vec3 sw_mb_apply(vec3 rgb2020, float purity01, out float purity_cur) {\n"
    "  purity_cur = 0.0;\n"
    "  vec3 lms = SW_XYZ_TO_LMS * (SW_2020_TO_XYZ * rgb2020);\n"
    "  float t = lms.x + lms.y;\n"
    "  if (t <= 1e-6) return max(rgb2020, 0.0);\n"
    "  vec2 white = sw_mb_from_lms(SW_XYZ_TO_LMS * SW_D65_XYZ);\n"
    "  vec2 dir = sw_mb_from_lms(lms) - white;\n"
    "  if (dot(dir, dir) < 1e-14) return max(rgb2020, 0.0);\n"
    "  vec3 lms_t0 = sw_lms_from_mb(white, t);\n"
    "  vec3 rgb_t0 = SW_XYZ_TO_2020 * (SW_LMS_TO_XYZ * lms_t0);\n"
    "  vec3 a = rgb2020 - rgb_t0;\n"
    "  vec2 i0 = sw_interval_lower0(a.x, rgb_t0.x);\n"
    "  vec2 i1 = sw_interval_lower0(a.y, rgb_t0.y);\n"
    "  vec2 i2 = sw_interval_lower0(a.z, rgb_t0.z);\n"
    "  float t_hi = min(1e30, min(i0.y, min(i1.y, i2.y)));\n"
    "  float t_lo = max(0.0, max(i0.x, max(i1.x, i2.x)));\n"
    "  if (t_hi < t_lo) return max(rgb2020, 0.0);\n"
    "  float t_max = max(0.0, t_hi);\n"
    "  purity_cur = t_max > 1e-6 ? clamp(1.0 / t_max, 0.0, 1.0) : 0.0;\n"
    "  float t_final = clamp(purity01, 0.0, 1.0) * t_max;\n"
    "  vec2 mb_final = white + t_final * dir;\n"
    "  vec3 lms_final = sw_lms_from_mb(mb_final, t);\n"
    "  return max(SW_XYZ_TO_2020 * (SW_LMS_TO_XYZ * lms_final), 0.0);\n"
    "}\n"
    "// Hue and purity from donor709 onto target709, holding the target's luminance (MacLeod-Boynton).\n"
    "vec3 sw_mbbt(vec3 target709, vec3 donor709, float hue_strength, float purity_strength) {\n"
    "  if (hue_strength <= 0.0 && purity_strength <= 0.0) return target709;\n"
    "  vec3 target2020 = SW_XYZ_TO_2020 * (SW_709_TO_XYZ * target709);\n"
    "  vec3 donor2020 = SW_XYZ_TO_2020 * (SW_709_TO_XYZ * donor709);\n"
    "  float tp = 0.0, dp = 0.0;\n"
    "  vec3 tdummy = sw_mb_apply(target2020, 1.0, tp);\n"
    "  vec3 ddummy = sw_mb_apply(donor2020, 1.0, dp);\n"
    "  float purity = mix(tp, dp, clamp(purity_strength, 0.0, 1.0));\n"
    "  vec3 lms_t = SW_XYZ_TO_LMS * (SW_2020_TO_XYZ * target2020);\n"
    "  float target_t = lms_t.x + lms_t.y;\n"
    "  if (target_t <= 1e-6) return target709;\n"
    "  vec2 white = sw_mb_from_lms(SW_XYZ_TO_LMS * SW_D65_XYZ);\n"
    "  vec2 mt = sw_mb_from_lms(lms_t) - white;\n"
    "  vec3 lms_d = SW_XYZ_TO_LMS * (SW_2020_TO_XYZ * donor2020);\n"
    "  vec2 md = sw_mb_from_lms(lms_d) - white;\n"
    "  float lt = length(mt), ld = length(md);\n"
    "  vec2 tu = lt > 1e-14 ? mt / lt : vec2(0.0);\n"
    "  vec2 ru = ld > 1e-14 ? md / ld : tu;\n"
    "  if (lt <= 1e-14) tu = ru;\n"
    "  vec2 bu = mix(tu, ru, clamp(hue_strength, 0.0, 1.0));\n"
    "  float bl = length(bu);\n"
    "  bu = bl > 1e-14 ? bu / bl : (hue_strength >= 0.5 ? ru : tu);\n"
    "  float seed_len = max(lt > 1e-6 ? lt : ld, 1e-6);\n"
    "  vec3 lms_seed = sw_lms_from_mb(white + bu * seed_len, target_t);\n"
    "  vec3 seed2020 = SW_XYZ_TO_2020 * (SW_LMS_TO_XYZ * lms_seed);\n"
    "  float unused = 0.0;\n"
    "  vec3 out2020 = sw_mb_apply(seed2020, purity, unused);\n"
    "  return SW_XYZ_TO_709 * (SW_2020_TO_XYZ * out2020);\n"
    "}\n";

char *vk_hdr_tonemap_source(const char *orig, int scene_texture, int glow_texture) {
    if (!orig || scene_texture < 0 || glow_texture < 0 || glow_texture == scene_texture) return NULL;
    const char *main = strstr(orig, "void main()");
    if (!main) return NULL;
    const char *body = main + strlen("void main()");
    if (!strstr(orig, "out vec4 c0;")) return NULL;
    char tex_decl[48];
    snprintf(tex_decl, sizeof tex_decl, "uniform sampler2D t%d;", scene_texture);
    if (!strstr(orig, tex_decl)) return NULL;
    snprintf(tex_decl, sizeof tex_decl, "uniform sampler2D t%d;", glow_texture);
    if (!strstr(orig, tex_decl)) return NULL;
    const char *scene_at = NULL, *glow_at = NULL;
    char *scene_uv = sample_arg(body, scene_texture, &scene_at);
    char *glow_uv = sample_arg(body, glow_texture, &glow_at);
    if (!scene_uv || !glow_uv) {
        free(scene_uv);
        free(glow_uv);
        return NULL;
    }
    // Keep the statements before the first sample: they compute the texture coordinates. (The translated fragment
    // shaders wrap main's body in `do { ... } while (false);`: skip that one brace.)
    const char *first = scene_at < glow_at ? scene_at : glow_at;
    const char *brace = strchr(body, '{');
    const char *prefix_at = brace ? brace + 1 : body;
    const char *line = first;
    while (line > prefix_at && line[-1] != '\n' && line[-1] != '\r') line--;
    const char *prefix_end = line > prefix_at ? line : prefix_at;
    const char *do_at = NULL;
    int depth = 0, opens = 0, closes = 0;
    for (const char *q = prefix_at; q < prefix_end; q++) {
        if (q[0] == 'd' && q[1] == 'o' && q[2] == ' ' && q[3] == '{') do_at = q;
        if (*q == '{') depth++, opens++;
        else if (*q == '}') depth--, closes++;
    }
    if (!(do_at && opens == 1 && closes == 0) && (depth != 0 || opens != 0)) {
        free(scene_uv);
        free(glow_uv);
        return NULL;
    }
    const char *pre1 = prefix_at, *pre2 = NULL;
    size_t pre1_len = (size_t)(prefix_end - prefix_at), pre2_len = 0;
    if (do_at) {  // (declarations before `do {`, then the statements after it, at main's scope)
        const char *open = strchr(do_at, '{');
        pre1_len = (size_t)(do_at - prefix_at);
        pre2 = open + 1;
        pre2_len = (size_t)(prefix_end - pre2);
    }
    size_t prefix_len = pre1_len + pre2_len;
    if ((prefix_len == 0 && (has_register(scene_uv) || has_register(glow_uv))) || !strstr(orig, "cb3")) {
        free(scene_uv);
        free(glow_uv);
        return NULL;
    }
    char functions[65536];
    int flen = snprintf(functions, sizeof functions,
                        "layout(push_constant) uniform VkHdrParams { vec4 p[5]; } sw_hdr;\n"
                        "float sw_maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }\n"
                        "float sw_curve(float x) {\n"
                        "  float pivot = sw_hdr.p[0].z;\n"
                        "  float y0 = 1.0 - exp(-pivot);\n"
                        "  float m0 = exp(-pivot);\n"
                        "  return x <= pivot ? 1.0 - exp(-x) : y0 + m0 * (x - pivot);\n"
                        "}\n"
                        "vec3 sw_curve3(vec3 x) { return vec3(sw_curve(x.r), sw_curve(x.g), sw_curve(x.b)); }\n"
                        "float psy_srgb_decode(float x) {\n"
                        "  return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);\n"
                        "}\n"
                        "float psy_gamma_safe(float x, float g) {\n"
                        "  return psy_srgb_decode(pow(max(x, 0.0), 1.0 / g));\n"
                        "}\n"
                        "%s%s", k_mbbt, vk_hdr_psycho_glsl);
    if (flen <= 0 || flen >= (int)sizeof functions) {
        free(scene_uv);
        free(glow_uv);
        return NULL;
    }
    const char *weights = strstr(orig, "cb1")
                              ? "  float sw_wg = uintBitsToFloat(cb1[0].x), sw_wb = uintBitsToFloat(cb1[0].y);\n"
                                "  float sw_wl = uintBitsToFloat(cb1[0].z);\n"
                              : "  float sw_wg = 0.5866, sw_wb = 0.1144, sw_wl = 0.666667;\n";
    char grade[256];
    snprintf(grade, sizeof grade,
             "  float sw_bias = uintBitsToFloat(cb3[0].x), sw_scale = uintBitsToFloat(cb3[0].y) * sw_hdr.p[1].y;\n");
    size_t prologue = (size_t)(main - orig);
    size_t cap = prologue + prefix_len + strlen(functions) + strlen(scene_uv) + strlen(glow_uv) + 16384;
    char *out = calloc(cap, 1);
    if (!out) {
        free(scene_uv);
        free(glow_uv);
        return NULL;
    }
    char samples[1200];
    snprintf(samples, sizeof samples,
             "  vec3 sw_scene = min(texture(t%d, %s).rgb, vec3(10000.0));\n"
             "  vec3 sw_bloom = min(texture(t%d, %s).rgb, vec3(10000.0)) * sw_hdr.p[1].z;\n"
             "  vec3 sw_x = max(sw_scene + sw_bloom, vec3(0.0));\n",
             scene_texture, scene_uv, glow_texture, glow_uv);
    int n = snprintf(out, cap, "%.*s", (int)prologue, orig);
    n += snprintf(out + n, cap - (size_t)n,
                  "// (SavageWind: BOTW's own tonemap with RenoDX's extended vanilla curve; the picture is kept above white)\n"
                  "%s"
                  "void main() {\n"
                  "%.*s%.*s"
                  "%s"
                  "%s"
                  "%s"
                  "  float sw_game = sw_hdr.p[0].x, sw_peak = sw_hdr.p[0].y;\n"
                  "  float sw_hue = sw_hdr.p[0].w, sw_blowout = sw_hdr.p[1].x, sw_clamp = sw_hdr.p[1].w;\n"
                  "  float sw_tm = sw_hdr.p[2].x, sw_exp = sw_hdr.p[2].y, sw_hl = sw_hdr.p[2].z, sw_sh = sw_hdr.p[2].w;\n"
                  "  float sw_ct = sw_hdr.p[3].x, sw_sat = sw_hdr.p[3].y, sw_ph = sw_hdr.p[3].z, sw_fl = sw_hdr.p[3].w;\n"
                  "  float sw_gamma = sw_hdr.p[4].x, sw_npeak = sw_peak;\n"
                  "  if (sw_gamma > 0.5) sw_npeak = psy_gamma_safe(sw_peak, sw_gamma > 1.5 ? 2.4 : 2.2);\n"
                  "  float sw_lum = max(sw_x.r * 0.2989 + sw_x.g * sw_wg + sw_x.b * sw_wb, 0.0);\n"
                  "  float sw_sdr = 1.0 - exp(-sw_lum);\n"
                  "  vec3 sw_by_lum = sw_lum > 0.0 ? sw_x * (sw_curve(sw_lum) / sw_lum) : sw_x;\n"
                  "  vec3 sw_ext = max(mix(sw_by_lum, sw_curve3(sw_x), clamp(sw_sdr, 0.0, 1.0) * clamp(sw_sdr, 0.0, 1.0)),"
                  " vec3(0.0));\n"
                  "  // The game's own SDR result (vanilla, no grade) is the hue/purity donor: bright saturated colours\n"
                  "  // such as a fire's core read yellow-white there, but the extended curve keeps their ratios, so they\n"
                  "  // would stay deep red here. The transfer is MacLeod-Boynton hue/purity, as RenoDX's BOTW mod.\n"
                  "  vec3 sw_sdr_c = clamp(mix(sw_lum > 0.0 ? sw_x * (sw_sdr / sw_lum) : sw_x,"
                  " vec3(1.0) - exp(-sw_x), sw_sdr * sw_sdr), 0.0, 1.0);\n"
                  "  float sw_dmax = sw_maxc(sw_sdr_c), sw_dmin = min(sw_sdr_c.r, min(sw_sdr_c.g, sw_sdr_c.b));\n"
                  "  float sw_chroma = sw_dmax > 1e-6 ? (sw_dmax - sw_dmin) / sw_dmax : 0.0;\n"
                  "  float sw_hue_strength = sw_hue * smoothstep(0.02, 0.12, sw_chroma);\n"
                  "  if (sw_hue_strength > 0.0 || sw_blowout > 0.0)\n"
                  "    sw_ext = sw_mbbt(sw_ext, sw_sdr_c, sw_hue_strength, sw_blowout);\n"
                  "  // The game's grade, normalized into 0..1 by RenoDX's Neutwo max-channel scale (1/sqrt(max^2+1)).\n"
                  "  float sw_mx = max(sw_maxc(sw_ext), 0.0);\n"
                  "  float sw_norm = 1.0 / sqrt(sw_mx * sw_mx + 1.0);\n"
                  "  vec3 sw_n = sw_ext * sw_norm;\n"
                  "  float sw_l = (sw_n.r + sw_n.g + sw_n.b) * sw_wl;\n"
                  "  float sw_grade = sw_bias + sw_scale * (1.0 - (sw_l - 1.0) * (sw_l - 1.0));\n"
                  "  float sw_m = sw_maxc(sw_n);\n"
                  "  vec3 sw_c = (sw_m + (sw_n - sw_m) * sw_grade) / sw_norm;\n"
                  "  if (sw_clamp > 0.5)\n"
                  "    sw_c = SW_XYZ_TO_709 * (SW_2020_TO_XYZ * max(SW_XYZ_TO_2020 * (SW_709_TO_XYZ * sw_c), vec3(0.0)));\n"
                  "  else\n"
                  "    sw_c = max(sw_c, vec3(0.0));\n"
                  "  vec3 sw_out;\n"
                  "  if (sw_tm < 0.5) {\n"
                  "    // Vanilla: the game's own tonemap and grade, clipped as its A2B10G10R10 target was\n"
                  "    float sw_vl = (sw_sdr_c.r + sw_sdr_c.g + sw_sdr_c.b) * sw_wl;\n"
                  "    float sw_vg = sw_bias + sw_scale * (1.0 - (sw_vl - 1.0) * (sw_vl - 1.0));\n"
                  "    float sw_vm = sw_maxc(sw_sdr_c);\n"
                  "    sw_out = clamp(vec3(sw_vm) + (sw_sdr_c - sw_vm) * sw_vg, 0.0, 1.0);\n"
                  "  } else {\n"
                  "    // RenoDX PsychoV (test17): the mod's ApplyTest17BT2020, then its per-channel Neutwo to peak\n"
                  "    vec3 sw_p = PSY_709_TO_2020 * sw_c;\n"
                  "    sw_p = psy_apply(sw_p, sw_exp, sw_hl, sw_sh, sw_ct, sw_fl, sw_sat, sw_ph);\n"
                  "    sw_c = PSY_2020_TO_709 * sw_p;\n"
                  "    sw_out = psy_neutwo(sw_c, sw_npeak);\n"
                  "  }\n"
                  "  c0 = vec4(max(sw_out, vec3(0.0)) * sw_game, 1.0);\n"
                  "}\n", functions, (int)pre1_len, pre1, (int)pre2_len, pre2 ? pre2 : "", samples, weights, grade);
    free(scene_uv);
    free(glow_uv);
    if (n <= 0 || (size_t)n >= cap) {
        free(out);
        return NULL;
    }
    return out;
}
