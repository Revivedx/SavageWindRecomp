// The "Launch options" section of the start screen's first page (runtime/ui/launch_flags_ui.mm): the environment flags
// that testers are asked to set, kept in <data root>/launch-flags.txt (runtime/launch_flags.h, docs/launch-options.md).
#pragma once

// Into the current window, below the section heading drawn by the caller. scale: the UI scale.
void launch_flags_ui(float scale);
// Developer options (the game files page and Launch options): SAVAGEWIND_DEV in the saved launch options, as
// starting with -dev: the console's developer commands and cheats, and the Cheats and Teleport tabs. From the next
// Play (it restarts once with the saved flags).
bool launch_flags_dev(void);
void launch_flags_set_dev(bool on);
// The row's text, for both places
extern const char *const k_dev_options_desc;
