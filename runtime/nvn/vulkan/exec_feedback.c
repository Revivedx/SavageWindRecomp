// The feedback attachment checks of the snapshot path (exec.c resolve_feedback and draw_).
#include <stdlib.h>

#include "exec/internal.h"
#include "exec_feedback.h"

// (Mali: the same image through another texture object counts, it is the image that is bound. SAVAGEWIND_VK_SNAP_BY_OBJECT=1:
// the object only, as before and as elsewhere; =0: by image elsewhere too)
bool feedback_same_image(NvnTexture *a, NvnTexture *b) {
    static int by_obj = -1;
    if (by_obj < 0) by_obj = getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT") ? atoi(getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT")) != 0 : !g_vk.is_mali;
    return a == b || (!by_obj && a && b && a->backend && a->backend == b->backend);
}

bool feedback_is_attachment(NvnTexture *t, uint8_t det_draw) {
    if (!t) return false;
    if (feedback_same_image(t, st.rt.depth)) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (feedback_same_image(t, st.rt.color[i]) && !(det_draw & (1u << i))) return true;
    return false;
}

// The draw writes color target i (the fragment shader outputs it and its channel mask lets some channel through).
bool feedback_draw_writes_color(uint32_t out_mask, uint32_t channel_mask, int i) {
    return ((out_mask >> (i * 4)) & 0xF) && ((channel_mask >> (i * 4)) & 0xF);
}
