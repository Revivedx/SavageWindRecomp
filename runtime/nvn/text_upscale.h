// Gameplay > Sharper text (runtime/nvn/text_upscale.c): the game's bitmap font sheets, enlarged.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which of the game's fonts a sheet is (text_redraw.c draws the glyphs of all but OTHER again)
enum { TEXT_FONT_OTHER, TEXT_FONT_NORMAL, TEXT_FONT_OUTLINE, TEXT_FONT_CAPTION };

// A font's sheets as its archive describes them (FFNT: TGLP)
typedef struct TextSheetInfo {
    int font;  // TEXT_FONT_*
    uint16_t cell_w, cell_h, cols, rows;
    char name[48];  // archive and font, for the log
} TextSheetInfo;

// A font sheet of the game, as stored in its archive: its texture will hold these bytes (fs.c, when the RomFS opens).
// info: the font it belongs to (NULL: unknown), kept as given.
void text_sheet_add(const uint8_t *sheet, size_t bytes, const TextSheetInfo *info);
int text_sheet_count(void);
// The texture memory at texels is one of those sheets
bool text_sheet_known(const uint8_t *texels, size_t bytes);
// ... and the font it belongs to (NULL: not a sheet, or its font unknown)
const TextSheetInfo *text_sheet_info(const uint8_t *texels, size_t bytes);

// Gameplay > Sharper menus: the same for the menus' one-channel textures in the game's layout archives
void menu_tex_add(const uint8_t *texels, size_t bytes);
int menu_tex_count(void);
bool menu_tex_known(const uint8_t *texels, size_t bytes);

// src: w x h texels, tightly packed. dst: room for (w * scale) x (h * scale) texels. scale: 2 to 4.
// edges: they are coverage, whose edges can be drawn again; without it they are only interpolated.
// cell_w, cell_h: a font sheet's cells (a one-texel gutter before each, left and above; rows stored bottom-up): nothing
// is interpolated across them. 0: none.
bool text_upscale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t scale, bool edges, uint32_t cell_w, uint32_t cell_h,
                  uint8_t *dst);
