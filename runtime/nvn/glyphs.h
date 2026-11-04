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

// Changes whenever the icons to draw change (device, bindings, setting): sheets uploaded under another one go up again.
uint32_t glyphs_generation(void);

// The device the Controls page and the tips draw instead of the Switch (runtime/nvn/hw_illust.c HW_DEVICE_*), -1 for
// the Switch: Button icons' device, or on Automatic the controller in use (the Steam Deck's own in Steam Deck mode).
// glyphs_console: the one they show, first decided when the game files are opened (their layouts are patched then);
// glyphs_console_wanted: what it would be now; glyphs_console_poll (each frame): another one moves them to it while the
// game runs (their drawings follow: textures.c), or, if their panes aren't found, leaves it to a restart.
int glyphs_console(void);
int glyphs_console_wanted(void);
void glyphs_console_poll(void);
bool glyphs_console_stuck(void);  // another device wanted, but its panes weren't found: only a restart shows it
