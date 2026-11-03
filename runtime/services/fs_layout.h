#ifndef SAVAGEWIND_FS_LAYOUT_H
#define SAVAGEWIND_FS_LAYOUT_H

#include <stdbool.h>

// PauseMenu_00's Inventory clipping pane and the neighboring page's last item,
// in the layout's own coordinates (origin at the top left, before guest/output scaling).
typedef struct {
    float root_w, root_h;
    float clip_x, clip_y, clip_w, clip_h;
    float previous_item_x;
} FsInventoryClipLayout;

bool fs_inventory_clip_layout(FsInventoryClipLayout *out);

#endif
