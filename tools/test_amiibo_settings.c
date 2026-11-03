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
    const char *own_wolf = getenv("SAVAGEWIND_TEST_OWN_WOLF");
    if (own_wolf && !strcmp(own_wolf, "write")) {
        assert(!g_settings.amiibo_own_wolf);
        g_settings.amiibo_own_wolf = true;
    } else if (own_wolf && !strcmp(own_wolf, "read")) {
        assert(g_settings.amiibo_own_wolf);
        g_settings.amiibo_own_wolf = false;
    } else assert(!g_settings.amiibo_own_wolf);
    const char *automatic = getenv("SAVAGEWIND_TEST_AMIIBO_AUTO");
    if (automatic && !strcmp(automatic, "write")) {
        assert(!g_settings.amiibo_auto_scan);
        g_settings.amiibo_auto_scan = true;
    } else if (automatic && !strcmp(automatic, "read")) {
        assert(g_settings.amiibo_auto_scan);
        g_settings.amiibo_auto_scan = false;
    } else assert(!g_settings.amiibo_auto_scan);
    assert(g_settings.virtual_amiibo == atoi(argv[2]));
    assert(g_settings.amiibo_figure == atoi(argv[3]));
    if (argc == 8) assert(g_settings.amiibo_repeat == atoi(argv[6]));
    else assert(!g_settings.amiibo_repeat);
    g_settings.amiibo_repeat = argc == 8 && atoi(argv[7]);
    g_settings.virtual_amiibo = atoi(argv[4]);
    g_settings.amiibo_figure = atoi(argv[5]);
    const char *library = getenv("SAVAGEWIND_TEST_AMIIBO_LIBRARY");
    if (library && !strcmp(library, "write")) {
        g_settings.amiibo_favorites[AMIIBO_GUARDIAN] = true;
        g_settings.amiibo_favorites[AMIIBO_MARIO] = true;
        g_settings.amiibo_favorites[AMIIBO_COUNT - 1] = true;
        amiibo_recent_push(g_settings.amiibo_recent, &g_settings.amiibo_recent_count, AMIIBO_ARCHER);
        amiibo_recent_push(g_settings.amiibo_recent, &g_settings.amiibo_recent_count, AMIIBO_GUARDIAN);
        amiibo_recent_push(g_settings.amiibo_recent, &g_settings.amiibo_recent_count, AMIIBO_ARCHER);
    } else if (library && !strcmp(library, "read")) {
        assert(g_settings.amiibo_favorites[AMIIBO_GUARDIAN] && g_settings.amiibo_favorites[AMIIBO_MARIO]);
        assert(!g_settings.amiibo_favorites[AMIIBO_ARCHER]);
        assert(g_settings.amiibo_favorites[AMIIBO_COUNT - 1]);
        assert(g_settings.amiibo_recent_count == 2);
        assert(g_settings.amiibo_recent[0] == AMIIBO_ARCHER && g_settings.amiibo_recent[1] == AMIIBO_GUARDIAN);
        g_settings.amiibo_favorites[AMIIBO_GUARDIAN] = false;
        amiibo_recent_push(g_settings.amiibo_recent, &g_settings.amiibo_recent_count, AMIIBO_MARIO);
    } else if (library && !strcmp(library, "removed")) {
        assert(!g_settings.amiibo_favorites[AMIIBO_GUARDIAN] && g_settings.amiibo_favorites[AMIIBO_MARIO]);
        assert(g_settings.amiibo_recent_count == 3 && g_settings.amiibo_recent[0] == AMIIBO_MARIO);
        assert(g_settings.amiibo_recent[1] == AMIIBO_ARCHER && g_settings.amiibo_recent[2] == AMIIBO_GUARDIAN);
    }
    settings_save();
    puts("PASS: amiibo settings loaded and saved");
}
