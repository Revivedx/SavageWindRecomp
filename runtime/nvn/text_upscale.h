// Video > Sharper text (runtime/nvn/text_upscale.c): the game's bitmap font sheets, enlarged.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A font sheet of the game, as stored in its archive: its texture will hold these bytes (fs.c, when the RomFS opens)
void text_sheet_add(const uint8_t *sheet, size_t bytes);
int text_sheet_count(void);
// The texture memory at texels is one of those sheets
bool text_sheet_known(const uint8_t *texels, size_t bytes);

// src: w x h texels, tightly packed. dst: room for (w * scale) x (h * scale) texels. scale: 2 to 4.
// edges: they are coverage, whose edges can be drawn again; without it they are only interpolated.
bool text_upscale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t scale, bool edges, uint8_t *dst);
