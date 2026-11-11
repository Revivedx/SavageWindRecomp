#pragma once
#include "imgui.h"

// Copies only the needed bitmap glyphs from the player's own Font_EU archive
// into ImGui's atlas. No game font data is shipped or written to disk.
void quick_hint_fonts_load(ImFontAtlas *atlas, ImFont **text, ImFont **buttons);

// The same, for other overlays: each fonts[i] (a font of Font_EU, e.g. "Caption_00.bffnt") with the characters in
// chars only, into out[i] (NULL: not there). Their glyphs' pixels go in with game_fonts_fill, once every font is in the
// atlas (it builds it).
void game_fonts_add(ImFontAtlas *atlas, const char *const *fonts, int n, const unsigned *chars, int nchars, ImFont **out);
void game_fonts_fill(ImFontAtlas *atlas);
