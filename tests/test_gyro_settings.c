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
    assert(argc == 4 || argc == 5);
    setenv("SAVAGEWIND_SETTINGS", argv[1], 1);
    settings_load();
    assert(g_settings.pc_gyro == (atoi(argv[2]) != 0));
    assert(g_settings.gyro_scene == (atoi(argv[3]) != 0));
    if(argc>4)assert(g_bind_key[BIND_GYRO_UP]==SDL_SCANCODE_H);
    // UI changes must remain possible even when startup debug overrides were supplied.
    g_settings.pc_gyro = !g_settings.pc_gyro;
    g_settings.gyro_scene = !g_settings.gyro_scene;
    g_bind_key[BIND_GYRO_UP]=SDL_SCANCODE_H;
    settings_save();
    printf("PASS: load keyboard=%s drag=%s, toggle and save for next boot\n", argv[2], argv[3]);
}
