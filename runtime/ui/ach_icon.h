// The picture of each achievement (runtime/ui/ach_icon.mm): achievements/<id>.png in the data root, next to the exe.
#pragma once
#include "imgui.h"

// Render thread. The texture of achievement `id`, or 0 when there is no usable file for it (the UI then draws its diamond).
// Each file is looked for once and kept; PNG / JPEG up to 1024x1024.
ImTextureID ach_icon(const char *id);
// Forget them all (the next ach_icon() loads again).
void ach_icons_free(void);
