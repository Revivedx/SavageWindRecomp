// Separate invocations verify persistence and startup overrides; the switch
// must remain editable after an override has seeded the boot's settings.
#include <assert.h>
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
    settings_load();
    assert(g_settings.quick_inventory == (atoi(argv[2]) != 0));
    g_settings.quick_inventory = !g_settings.quick_inventory;
    settings_save();
    puts("ok: quick inventory load, live toggle and save");
}
