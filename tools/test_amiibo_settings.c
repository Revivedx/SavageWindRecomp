// Separate processes verify defaults, stable figure keys, malformed input and startup override persistence.
#include <assert.h>
#include "../runtime/settings.c"
#include "../runtime/hle/bindings.c"

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
int input_pad_family(void) { return 0; }
bool fsr3_upscaler_enabled(void) { return false; }

int main(int argc, char **argv) {
    assert(argc == 6 || argc == 8);
    setenv("SAVAGEWIND_SETTINGS", argv[1], 1);
    settings_load();
    assert(g_settings.virtual_amiibo == atoi(argv[2]));
    assert(g_settings.amiibo_figure == atoi(argv[3]));
    if (argc == 8) assert(g_settings.amiibo_repeat == atoi(argv[6]));
    else assert(!g_settings.amiibo_repeat);
    g_settings.amiibo_repeat = argc == 8 && atoi(argv[7]);
    g_settings.virtual_amiibo = atoi(argv[4]);
    g_settings.amiibo_figure = atoi(argv[5]);
    settings_save();
    puts("PASS: amiibo settings loaded and saved");
}
