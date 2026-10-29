// Run twice with the same temporary file to verify persistence across separate boots.
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
    assert(argc == 4);
    setenv("SAVAGEWIND_SETTINGS", argv[1], 1);
    settings_load();
    assert(g_settings.pc_cursor == (atoi(argv[2]) != 0));
    assert(g_settings.pc_bow == (atoi(argv[3]) != 0));
    // UI changes must remain possible even when startup debug overrides were supplied.
    g_settings.pc_cursor = !g_settings.pc_cursor;
    g_settings.pc_bow = !g_settings.pc_bow;
    settings_save();
    printf("PASS: load cursor=%s bow=%s, toggle and save for next boot\n", argv[2], argv[3]);
}
