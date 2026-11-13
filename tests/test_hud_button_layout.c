#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "hle.h"
#include "input_bind.h"
#include "language.h"
#include "nvn/glyphs.h"
#include "settings.h"

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
int input_pad_family(void) { return 0; }
int input_last_device(void) { return 0; }
bool pc_cursor_preview_active(void) { return false; }
bool pc_cursor_sensor_active(void) { return false; }
bool fsr3_upscaler_enabled(void) { return false; }
float winsize_refresh_rate(void) { return 0; }
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    (void)name; (void)dev_rel; (void)out; (void)n;
    return false;
}

int main(void) {
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);

    int pos[4] = {-1, -1, -1, -1};
    g_settings.button_layout = BUTTON_LAYOUT_AUTO;
    glyphs_hud_positions(pos);
    assert(pos[0] == 0 && pos[1] == 1 && pos[2] == 2 && pos[3] == 3);

    // Automatic follows g_bind_pad swaps (e.g. PS5 Cross / Circle swapped)
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_SOUTH;
    g_bind_pad[BIND_B] = SDL_GAMEPAD_BUTTON_EAST;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);

    // Automatic also follows per-action swaps when g_bind_pad is at default
    bind_reset(BIND_DEV_PAD);
    g_act_pad[0] = SDL_GAMEPAD_BUTTON_SOUTH;   // Interact
    g_act_pad[14] = SDL_GAMEPAD_BUTTON_EAST;   // Cancel
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);
    bind_reset(BIND_DEV_PAD);

    // Explicit presets
    g_settings.button_layout = BUTTON_LAYOUT_SWAP_AB;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);

    g_settings.button_layout = BUTTON_LAYOUT_XBOX;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 3 && pos[3] == 2);

    g_settings.button_layout = BUTTON_LAYOUT_SWAP_XY;
    glyphs_hud_positions(pos);
    assert(pos[0] == 0 && pos[1] == 1 && pos[2] == 3 && pos[3] == 2);

    g_settings.button_layout = BUTTON_LAYOUT_GAMECUBE;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 3 && pos[2] == 0 && pos[3] == 2);

    // Settings load/save round-trip for button_layout
    const char *tmp = "build/test_hud_button_layout.ini";
    setenv("SAVAGEWIND_SETTINGS", tmp, 1);
    g_settings.button_layout = BUTTON_LAYOUT_GAMECUBE;
    settings_save();
    FILE *fp = fopen(tmp, "r");
    assert(fp != NULL);
    char buf[4096] = {0};
    fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    unlink(tmp);
    assert(strstr(buf, "button_layout = gamecube") != NULL);

    puts("PASS: HUD button layout positions and settings");
    return 0;
}
