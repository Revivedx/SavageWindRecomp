// The "Launch options" section of the start screen's first page (runtime/ui/launch_flags_ui.mm): the environment flags
// that testers are asked to set, kept in <data root>/launch-flags.txt (runtime/launch_flags.h, docs/launch-options.md).
#pragma once

// Into the current window, below the section heading drawn by the caller. scale: the UI scale.
void launch_flags_ui(float scale);
