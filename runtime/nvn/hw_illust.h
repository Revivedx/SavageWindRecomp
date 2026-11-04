// Console illustrations for the device in use (runtime/nvn/hw_illust.c): the Controls page's and the tips' drawings of
// the Switch.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Controls page's drawings (front, top) and the tips' close-ups of the Switch (its shoulders, its two sides)
enum { HW_ILLUST_FRONT, HW_ILLUST_TOP, HW_ILLUST_TIP_ZL, HW_ILLUST_TIP_ZR, HW_ILLUST_TIP_FACE, HW_ILLUST_TIP_STICK,
       HW_ILLUST_COUNT };
// The devices drawn instead of the Switch
enum { HW_DEVICE_DECK, HW_DEVICE_XBOX, HW_DEVICE_PS5, HW_DEVICE_COUNT };

// The illustration of kind `which` (HW_ILLUST_*) the texture holds, -1 when it is none: an R8G8 (grey, alpha) texture
// w x h texels, rows `row` bytes apart in lin, recognised by its size and contents.
int hw_illust_which(uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row);

// Draws device's (HW_DEVICE_*) illustration `which` over lin (w x h R8G8 texels, rows `row` bytes apart), in the
// game's style: the same greys, outlines and size as the drawing it replaces, the buttons where they are on it.
void hw_illust_draw(int device, int which, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row);

// The layouts over the drawings (Layout/Common.sblarc): the Controls page's (blyt/PaAllControllerTipsNN_00.bflyt), its
// callout lines; the tips' (blyt/PaMessageTipsDrcImageNN_00.bflyt), the blue marks on the close-ups' buttons
enum { HW_LAYOUT_CONTROLS, HW_LAYOUT_TIPS };
// Layout `layout` (HW_LAYOUT_*, n bytes, edited in place) moved to device's (HW_DEVICE_*) buttons. Returns the panes
// moved, 0 when the layout is not the one expected (then it is left as it is).
int hw_illust_layout(int device, int layout, uint8_t *flyt, size_t n);

// Another device while the game runs (runtime/nvn/glyphs.c): the layouts' panes are objects in guest memory by then.
// hw_illust_keep: layout `layout` as the game has it (n bytes, before hw_illust_layout), and the device it shows (-1:
// the Switch's). hw_illust_panes: how many panes move for some device; each one's name, and its 40 bytes from its
// translation (translation, rotation, scale and size: so in the file and in the game's pane objects) for a device (-1:
// the Switch's) and for the device its layout shows now. hw_illust_shown: they were moved to device's.
#define HW_MOVE_BYTES 40
void hw_illust_keep(int layout, const uint8_t *flyt, size_t n, int shown);
int hw_illust_panes(void);
const char *hw_illust_pane_name(int i);
const uint8_t *hw_illust_pane_for(int i, int device);
const uint8_t *hw_illust_pane_now(int i);
void hw_illust_shown(int device);
