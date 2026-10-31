// The in-game console: a command line over the game (` or F12) that runs the control layer's commands
// (runtime/control.h), the player safe ones in a normal build, all of them in developer mode. The lines, history and
// completion are runtime/console_core.c; this draws it with ImGui and is wired into the overlay in menu.mm.
#pragma once
#include <stdbool.h>
#include "imgui.h"

bool console_is_open(void);   // any thread
void console_set_open(bool open);
// Once per frame on the render thread (also while closed: a reply that arrives after closing is kept for the next time).
void console_collect(void);
// In the ImGui frame, when open. mono: the fixed width font for the lines (columns of `help` line up).
void console_draw(ImGuiIO &io, float scale, ImFont *mono);
// Main thread: text a command asked to put on the clipboard (the caller frees it), false when there is none.
bool console_take_copy(char **text);
