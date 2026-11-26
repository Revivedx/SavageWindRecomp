// The NVN copy commands of the command executor (exec.c exec_list).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel.h"
#include "vk.h"
#include "exec/internal.h"
#include "exec_copy.h"
#include "record.h"
#include "../renderer.h"

// (exec.c's draw code and the tests below read these from the executor)
void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void vk_trace_event(NvnTexture *t, uint64_t key, NvnTexture *const *srcs, int nsrcs, const char *fmt, ...);
void vk_trace_dump(NvnTexture *t);

// HDR: remember the float picture a draw (or copy) writes into the game's presented 8-bit image; present_locked uses
// it instead of the clipped copy. Any other draw into that image forgets it.
static void hdr_record_copy(NvnTexture *sn, NvnTexture *dn) {
    if (!vk_hdr_game_enabled() || !sn || !dn || !renderer_texture_presented(dn)) return;
    TexBackend *d = vk_tex(dn, true);
    if (!d || d->hdr_upgraded) return;
    TexBackend *s = vk_tex(sn, false);
    if (s && s->hdr_upgraded && s->image && s->width == d->width && s->height == d->height && s->layers == 1 &&
        s->depth == 1 && s->samples == 1)
        vk_hdr_record(d->image, s->image, s->format, s->width, s->height);
    else
        vk_hdr_record(d->image, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, 0, 0);
}

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    vk_trace_event(t, 0, NULL, 0, "copy from buffer %llx", (unsigned long long)src);
    if (ENV_ONCE("SAVAGEWIND_LOG_COPIES")) {  // (debugging)
        static int n;
        if (n++ < 200)
            LOG("[copy] buffer %llx -> #%llu %ux%ux%u target %u f%#x level %d region %d,%d,%d %dx%dx%d view set %#x layer %u\n",
                (unsigned long long)src, (unsigned long long)t->debug_id, t->width, t->height, t->depth, t->target, t->format,
                level, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], dst->has_view ? dst->view.setmask : 0,
                dst->has_view ? dst->view.min_layer : 0);
    }
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw || b->bc4_cpu) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

// (debugging) the last texture-to-buffer copies: backend_debug_copies_at logs those that wrote a guest address
static struct { uint64_t dst, size, frame; NvnTexture *t; int32_t rg[6]; } t2b_log[16];
static unsigned t2b_next;
void backend_debug_copies_at(uint64_t a) {
    for (int i = 0; i < 16; i++)
        if (t2b_log[i].t && a >= t2b_log[i].dst && a < t2b_log[i].dst + t2b_log[i].size)
            LOG("[debug]   CopyTextureToBuffer frame %llu from #%llu %ux%u format %#x region %d,%d %dx%d -> %llx+%llx\n",
                (unsigned long long)t2b_log[i].frame, (unsigned long long)t2b_log[i].t->debug_id, t2b_log[i].t->width,
                t2b_log[i].t->height, t2b_log[i].t->format, t2b_log[i].rg[0], t2b_log[i].rg[1], t2b_log[i].rg[3],
                t2b_log[i].rg[4], (unsigned long long)t2b_log[i].dst, (unsigned long long)t2b_log[i].size);
}

void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        int32_t sx0 = 0, sy0 = 0, sx1 = (int32_t)b->width, sy1 = (int32_t)b->height;
        if (b->scalable && rg[0] == 0 && rg[1] == 0 && rg[3] == (int32_t)t->width && rg[4] == (int32_t)t->height) {
            // A wide render target keeps its guest dimensions (for example the save caption's 256x144) but its
            // backing image includes the extra sides. Crop those sides before returning it to the guest; scaling the
            // whole backing to the guest's fixed 16:9 size squeezes the save screenshot horizontally.
            double guest_aspect = (double)t->width / t->height;
            double backing_aspect = (double)b->width / b->height;
            if (backing_aspect > guest_aspect) {
                sx1 = (int32_t)lround(b->height * guest_aspect);
                sx0 = ((int32_t)b->width - sx1) / 2;
                sx1 += sx0;
            } else if (backing_aspect < guest_aspect) {
                sy1 = (int32_t)lround(b->width / guest_aspect);
                sy0 = ((int32_t)b->height - sy1) / 2;
                sy1 += sy0;
            }
        }
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{sx0, sy0, 0}, {sx1, sy1, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        rec_CmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    kernel_watch_write("CopyTextureToBuffer", dst, total);
    hc_cpu_wrote(dst, total);
    vk_trace_dump(t);
    {
        unsigned k = t2b_next++ & 15;
        t2b_log[k].dst = dst;
        t2b_log[k].size = total;
        t2b_log[k].frame = renderer_present_count();
        t2b_log[k].t = t;
        memcpy(t2b_log[k].rg, rg, sizeof rg);
    }
    vk_readback_sync();  // (non-coherent host-cached memory: what the GPU just wrote, not what the caches held)
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (SAVAGEWIND_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    if (ENV_ONCE("SAVAGEWIND_LOG_COPIES")) {  // (debugging)
        static int n;
        if (n++ < 300)
            LOG("[copy] tex #%llu %ux%ux%u t%u f%#x lv%u (%d,%d,%d %dx%dx%d) view %#x/%u/%u -> #%llu %ux%ux%u t%u f%#x lv%u "
                "(%d,%d,%d %dx%dx%d) view %#x/%u/%u flags %llx\n",
                (unsigned long long)st_->debug_id, st_->width, st_->height, st_->depth, st_->target, st_->format, st_->levels,
                src->region[0], src->region[1], src->region[2], src->region[3], src->region[4], src->region[5],
                src->has_view ? src->view.setmask : 0, src->view.base_level, src->view.min_layer, (unsigned long long)dt->debug_id,
                dt->width, dt->height, dt->depth, dt->target, dt->format, dt->levels, dst->region[0], dst->region[1],
                dst->region[2], dst->region[3], dst->region[4], dst->region[5], dst->has_view ? dst->view.setmask : 0,
                dst->view.base_level, dst->view.min_layer, (unsigned long long)flags);
    }
    {
        NvnTexture *src1 = st_;
        vk_trace_event(dt, 0, &src1, 1, "copy from #%llu %ux%u f%#x region %d,%d %dx%d -> region %d,%d %dx%d flags %llx",
                (unsigned long long)st_->debug_id, st_->width, st_->height, st_->format, src->region[0], src->region[1],
                src->region[3], src->region[4], dst->region[0], dst->region[1], dst->region[3], dst->region[4],
                (unsigned long long)flags);
    }
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    hdr_record_copy(st_, dt);  // (HDR: the float source the presented image is copied from, for device.c)
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    rec_CmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}
