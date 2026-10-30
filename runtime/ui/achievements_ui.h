// Achievements in the overlay (runtime/ui/achievements_ui.mm); the logic is in runtime/achievements.h.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Render thread, inside an ImGui frame: the notice of each new unlock, a panel that slides down from the top edge.
void ach_ui_toasts(float display_w, float display_h, float scale);
#ifdef __cplusplus
// The toasts' fonts (C++ only: ImFont): the achievement's name, the header and description (NULL: the frame's font)
struct ImFont;
void ach_ui_fonts(struct ImFont *title, struct ImFont *body);
#endif
// A toast is on screen or waiting: the overlay has to keep drawing even with the menu closed.
bool ach_ui_active(void);
// The "Achievements" page of the menu: the list with progress.
void ach_ui_tab(void);

#ifdef __cplusplus
}
#endif
