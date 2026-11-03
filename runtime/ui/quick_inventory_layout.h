#ifndef QUICK_INVENTORY_LAYOUT_H
#define QUICK_INVENTORY_LAYOUT_H

#include <stdbool.h>

// MainShortCut_00.bflyt uses a centered 1280x720 canvas. Keep the native
// selector translation and the host-rendered controls in the same units.
enum {
    QUICK_INVENTORY_WIDTH = 1280,
    QUICK_INVENTORY_HEIGHT = 720,
    QUICK_INVENTORY_CHEST_SHIFT = 260,
};

typedef struct QuickInventoryLayout {
    float scale;
    float font_size;
    float controls_y;
} QuickInventoryLayout;

static inline QuickInventoryLayout quick_inventory_layout(float width, float height, bool chest) {
    float scale = width / QUICK_INVENTORY_WIDTH;
    if (height / QUICK_INVENTORY_HEIGHT < scale) scale = height / QUICK_INVENTORY_HEIGHT;
    // Native right-stick guide: N_CaptureText_00.y=98, Pa_Rstick_00.y=-58,
    // height=66. Reserve its entire bounds, then a 12-unit gap and a 36-unit
    // controls row (including the controller glyph's transparent margins).
    const float guide_bottom = QUICK_INVENTORY_HEIGHT / 2.0f - (98 - 58) + 66 / 2.0f;
    const float controls_center = guide_bottom + 12 + 36 / 2.0f;
    QuickInventoryLayout layout = {
        scale,
        (32 / 1.5f) * scale,
        height / 2 + (controls_center - QUICK_INVENTORY_HEIGHT / 2.0f +
                      (chest ? QUICK_INVENTORY_CHEST_SHIFT : 0)) * scale,
    };
    return layout;
}

#endif
