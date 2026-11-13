#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "language.h"
#include "settings.h"
#include "window_size.h"

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
int input_pad_family(void) { return 0; }
bool fsr3_upscaler_enabled(void) { return false; }
bool settings_handheld_size(int *w, int *h) { (void)w; (void)h; return false; }

int main(void) {
    const char *aspect = winsize_aspect(1600, 900);
    assert(aspect != NULL);
    assert(!strcmp(aspect, "16:9"));

    char name[64];
    winsize_name((WinSize){1600, 900}, NULL, name, sizeof name);
    assert(!strcmp(name, "1600 x 900  (16:9)"));

    WinSize list[WINSIZE_MAX];
    int n = winsize_list(list, false);
    assert(n > 0);

    bool found = false;
    for (int i = 0; i < n; i++) {
        if (list[i].w == 1600 && list[i].h == 900) {
            found = true;
            break;
        }
    }
    assert(found);

    // Match display real Hz tests
    g_settings.fps_display = true;
    settings_fps_for_display(239.77f);
    assert(g_settings.fps_limit == 240);
    settings_fps_for_display(72.0f);
    assert(g_settings.fps_limit == 72);

    printf("test_window_size: ok\n");
    return 0;
}
