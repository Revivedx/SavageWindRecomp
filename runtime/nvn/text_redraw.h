// Gameplay > Sharper text (runtime/nvn/text_redraw.c): the game's font sheets drawn again from outlines made out of
// their own glyphs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "text_upscale.h"

// The glyphs of sheet `layer` of a font, drawn into its enlarged image. info: as text_sheet_info gives it (kept: the
// outlines are made from it later). orig: the sheet as the game has it (pw x ph texels, rows bottom-up, as stored).
// big: (pw * s) x (ph * s), the same.
// cache_dir: where the glyphs' outlines are kept (in its text/ folder) once made (NULL: not kept on disk).
// Returns how many glyphs were drawn (0: not one of the game's fonts, or its outlines could not be made). -1: they are
// being made, on a thread of their own, and big is left as it was: once text_redraw_generation() has moved on from
// what it was before this call, the sheet is to go up again.
int text_redraw_sheet(const TextSheetInfo *info, uint32_t layer, const uint8_t *orig, uint32_t pw, uint32_t ph, uint8_t *big,
                      uint32_t s, const char *cache_dir);
uint32_t text_redraw_generation(void);
