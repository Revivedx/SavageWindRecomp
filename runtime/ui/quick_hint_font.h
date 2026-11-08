#pragma once
#include "imgui.h"

// Copies only the needed bitmap glyphs from the player's own Font_EU archive
// into ImGui's atlas. No game font data is shipped or written to disk.
void quick_hint_fonts_load(ImFontAtlas *atlas, ImFont **text, ImFont **buttons);
