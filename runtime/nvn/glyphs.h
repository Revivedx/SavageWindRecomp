// Button icons for the device in use (runtime/nvn/glyphs.c): the game's button glyph font sheet, redrawn.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The texture at addr (format: NVN format, w x h texels) is a button glyph sheet: lin holds its texels as uploaded
// (linear rows of 4x4 BC4 blocks, wb x hb); they are replaced by the glyphs for the current device. Returns true when
// the texture is such a sheet (whether or not it changed it).
bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb);

// The same for the HUD's button textures (R8 masks, w x h texels, rows `row` bytes apart in lin): true when the texture
// is one of them.
bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row);
// One of those textures (R8 texels, deswizzled)?
bool glyphs_is_layout_icon(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row);

// Changes whenever the icons to draw change (device, bindings, setting): sheets uploaded under another one go up again.
uint32_t glyphs_generation(void);
// The same without the menu pointer's state: what the overlay's fonts are made for (their icons are the face buttons).
uint32_t glyphs_font_generation(void);

// The diamond vertex (0: East, 1: South, 2: North, 3: West) for each Switch face button (A, B, X, Y) on the HUD.
void glyphs_hud_positions(int pos[4]);

// The device the Controls page and the tips draw instead of the Switch (runtime/nvn/hw_illust.c HW_DEVICE_*), -1 for
// the Switch: Button icons' device, or on Automatic the controller connected. Decided when the game files are opened
// (the layouts are patched then): another one shows from the next start.
int glyphs_console(void);
