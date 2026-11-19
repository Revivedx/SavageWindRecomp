// Shared state of the Vulkan command executor (exec.c) and the modules it is being split into.
//
// exec.c stays the command-list interpreter and owns the state below; an extracted module includes this header and
// reads the state directly or calls the entry points declared here as they move. This first header exposes only the
// state every later module needs.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../vk.h"

// The render targets bound by the command list (begin_pass reads them).
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

// What the last commands bound: the game's NVN state, written by exec_list and read by draws, passes, clears and
// copies.
typedef struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    float line_width;
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    bool depth_clamp;
    bool rasterizer_discard;  // nvnCommandBufferSetRasterizerDiscard: draw() skips draws (no transform feedback here)
    uint64_t copy_row_stride, copy_image_stride;
} ExecState;

// Command buffer state a draw recorded last, so a draw records only what differs (see dyn_check).
typedef struct {
    uint64_t epoch;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t ndyn, dyn[32];  // dynamic uniform-buffer offsets bound with them
    uint32_t dt, dw, dop, sten, cull, front, topo, restart, bias_on;
    uint32_t sten_face[2][8];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float blend[4], bias[3], line_w;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ibuf;
    VkDeviceSize ioff;
    uint32_t itype;
    VkBuffer vbuf[17];
    VkDeviceSize voff[17];
} ExecDrawCache;

extern ExecState st;
extern ExecDrawCache dc;

// Generations that invalidate cached command-buffer state.
extern uint64_t dyn_epoch;
extern int no_dyn_cache;
extern uint64_t key_gen;   // program and pipeline keys
extern uint64_t setc_gen;  // shared descriptor sets of the submission in progress

// (after vk_cmd) a new command buffer or foreign state: nothing is known to be bound.
static inline void dyn_check(void) {
    if (dc.epoch == dyn_epoch) return;
    memset(&dc, 0xFF, sizeof dc);  // matches no value (floats: NaN)
    dc.epoch = dyn_epoch;
}
#define DYN_SAME(field, val) (dc.field == (val) ? true : (dc.field = (val), false))
