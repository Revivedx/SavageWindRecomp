// The per-draw feedback attachment checks of the snapshot path (exec_feedback.c). resolve_feedback and draw_ in
// exec.c ask whether a bound texture is the pass's depth or one of its color targets, and whether the draw writes it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

bool feedback_same_image(NvnTexture *a, NvnTexture *b);
bool feedback_is_attachment(NvnTexture *t, uint8_t det_draw);
bool feedback_draw_writes_color(uint32_t out_mask, uint32_t channel_mask, int i);
