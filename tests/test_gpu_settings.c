#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/settings.c"
#include "../runtime/draw_distance.c"
#include "../runtime/hle/bindings.c"

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
int input_pad_family(void) { return 0; }
bool fsr3_upscaler_enabled(void) { return false; }
uint64_t framepace_script_now(void) { return 0; }

int main(int argc, char **argv) {
    assert(argc == 3);
    setenv("SAVAGEWIND_SETTINGS", argv[1], 1);
    assert(g_settings.gpu == -1);
    const char *values[] = {"auto", "0", "1", "2147483647", "-1", "nvidia", "1junk", "2147483648",
                            "99999999999999999999999999999", "", "1.0"};
    const int expected[] = {-1, 0, 1, INT_MAX, -1, -1, -1, -1, -1, -1, -1};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++)
        assert(gpu_index_parse(values[i]) == expected[i]);
    settings_load();
    assert(g_settings.gpu == atoi(argv[2]));
    settings_save();

    // Unified AA row tests
    settings_aa_row_set(AA_ROW_FXAA);
    assert(g_settings.game_fxaa == true);
    assert(g_settings.antialias == AA_OFF);
    assert(settings_aa_row() == AA_ROW_FXAA);

    settings_aa_row_set(AA_ROW_SMAA);
    assert(g_settings.game_fxaa == false);
    assert(g_settings.antialias == AA_SMAA);
    assert(settings_aa_row() == AA_ROW_SMAA);

    // Graphics preset tests with fork field preservation
    g_settings.mvec_mode = MVEC_MODE_HYBRID;
    g_settings.upscale_quality = UPQ_NATIVE;
    g_settings.transparent_guides = true;
    graphics_preset_apply(GPRESET_HIGH);
    assert(g_settings.draw_distance == DRAWDIST_MEDIUM);
    assert(g_settings.shadow_res == SHADOW_HIGH);
    assert(g_settings.anisotropy == 3);
    assert(g_settings.render_scale == RSCALE_125);
    assert(g_settings.bloom == true);
    assert(g_settings.depth_of_field == true);
    assert(g_settings.mvec_mode == MVEC_MODE_HYBRID);
    assert(g_settings.upscale_quality == UPQ_NATIVE);
    assert(g_settings.transparent_guides == true);

    // Potato draw distance factor
    g_settings.draw_distance = DRAWDIST_POTATO;
    assert(fabsf(draw_distance_factor() - 0.5f) < 0.001f);

    // Legacy INI haze clamping to HAZE_MIN (75)
    const char *haze_ini = "build/test_haze_legacy.ini";
    FILE *f = fopen(haze_ini, "w");
    assert(f);
    fputs("settings_version = 3\nhaze = 25\n", f);
    fclose(f);
    setenv("SAVAGEWIND_SETTINGS", haze_ini, 1);
    s_settings_loaded = false;
    settings_load();
    assert(g_settings.haze == HAZE_MIN);
    assert(HAZE_MIN == 75);
    remove(haze_ini);
    setenv("SAVAGEWIND_SETTINGS", argv[1], 1);

    puts("PASS: GPU parsing and settings load/save");
}
