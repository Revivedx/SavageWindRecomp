// The start screen's background (runtime/ui/splash.mm): the player's own picture, splash.jpg / splash.png in the data
// folder (or a screenshot or the latest save's, as Settings say), else Savage Wind's art (a forest clearing with the
// icon's sword in a stone, drawn, the grass moving). The boot picture, while the game starts, has Savage Wind's own
// picture instead of the art (default_splash.jpg, shipped beside the executable: a blurred, green-tinted field).
#pragma once
#include "imgui.h"

void splash_load(void);    // after the ImGui renderer backend is up
void splash_load_boot(void);  // the same for the boot picture: Savage Wind's own picture where the art would be
void splash_unload(void);  // before it shuts down
void splash_draw(ImDrawList *dl, ImVec2 size, float time);
// The same in a rectangle at pos (Configuration's Home banner), its corners rounded over the colour behind it
void splash_draw_rect(ImDrawList *dl, ImVec2 pos, ImVec2 size, float time, float rounding, ImU32 behind);
bool splash_is_user(void);  // the player's picture is shown (not Savage Wind's own picture or art)
