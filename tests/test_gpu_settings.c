#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/settings.c"
#include "../runtime/hle/bindings.c"

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
int input_pad_family(void) { return 0; }
bool fsr3_upscaler_enabled(void) { return false; }

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
    puts("PASS: GPU parsing and settings load/save");
}
