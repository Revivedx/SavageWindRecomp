// On-screen keyboard (ImGui) for controllers, shared by the game's name entry (runtime/ui/menu.mm keyboard_draw,
// runtime/hle/swkbd.c) and every text field in our menus (in-game overlay, start screen): ui_input_text.
#pragma once
#include <stddef.h>
#include "imgui.h"

// A text field. Activated with a controller (A on it) it opens the on-screen keyboard and the confirmed text goes
// back into buf; with a mouse / keyboard it's typed into directly. Returns true when the text changed (typed, or OK
// on the on-screen keyboard; with ImGuiInputTextFlags_EnterReturnsTrue: Enter / OK). title heads the keyboard.
bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags = 0, const char *hint = NULL,
                   const char *title = NULL, bool open_keyboard = false);
// true (once) after the on-screen keyboard of the field just drawn with ui_input_text was cancelled
bool ui_input_text_cancelled(void);

// Once per frame, after the rest of the UI, at the root (outside any window), in the context the field was drawn in.
void osk_draw(ImFont *big, float scale);
bool osk_is_open(void);
void osk_cancel(void);  // (menu closed / reboot: drop it, the field keeps its text)

// Something is being typed in the current context: a text field has keyboard focus or the on-screen keyboard is up.
// Menu shortcuts (back, tabs, close) must not fire then: the keys are text.
bool ui_text_busy(void);

// The typed text (big font) and the key grid, drawn into the current window: d-pad / stick move, A types, B deletes,
// Start / + OK; typed keys work too (Enter OK, Esc cancel). text: ASCII, at most max_chars. Returns 1 OK, -1 cancel,
// 0 still typing.
int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first = true);
