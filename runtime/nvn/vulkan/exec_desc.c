// Descriptor sets of the Vulkan command executor: the constant buffers a draw binds, the shared sets of identical
// contents, and the two stage sets a draw writes (exec.c calls vk_exec_stage_set).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "exec/internal.h"
#include "mvec.h"
#include "raytrace.h"
#include "record.h"

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                uint64_t ua = gsnap_cs.addr[i] ? gsnap_cs.addr[i] : st.ubuf[5][i].addr;
                u[0] = (uint32_t)ua; u[1] = (uint32_t)(ua >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        VkDescriptorBufferInfo direct;
        if (ubo_direct(ss->code_addr + ss->const_offset, size, &direct)) return direct;  // (constants: never rewritten)
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    size = cbuf_copy_size(si, k, r);
    // An address outside the game's memory ended the game in the copy below after two hours of play (#640: 0x5000000000
    // on the render thread). Read as an unbound buffer, and logged: where the address comes from is still unknown.
    if (r.addr < GUEST_LO || r.addr >= GUEST_HI || size > GUEST_HI - r.addr) {
        static int nbad;
        if (nbad++ < 16)
            LOG("[vk] constant buffer %d of stage %d at %llx (%llu bytes) is outside the game's memory: read as zeros\n", k,
                stage, (unsigned long long)r.addr, (unsigned long long)size);
        return ubo_span(vk_transient_copy(zeros, size, 256), size);
    }
    VkDescriptorBufferInfo direct;
    if (vk_exec_cbuf_patch(stage, k, r.addr, size, &direct)) return direct;
    if (ubo_direct(r.addr, size, &direct)) return direct;  // (an integrated GPU reads the game's memory itself)
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Descriptor sets of identical contents within one submission are one set (the open world draws many objects with
// the same material textures and constants): saves the allocation and the update. SAVAGEWIND_VK_NO_SET_CACHE=1: off.
// Entries are told apart by two independent 64-bit hashes of the contents (no stored key: comparing ~700-byte keys
// from a 3 MB table cost more than the hashing).
#define SETC_TAB 4096
typedef struct { uint64_t seq, hash, hash2; VkDescriptorSetLayout layout; VkDescriptorSet set; uint32_t n; } SetEnt;
static SetEnt *setc;

// A 64x64 multiply folded to 64 bits (both halves of the product mixed in one mulq).
static inline uint64_t mum64(uint64_t a, uint64_t b) {
    unsigned __int128 r = (unsigned __int128)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

static VkDescriptorSet set_finish(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, int n) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_SET_CACHE") != NULL;
    stat_sets++;
    SetEnt *e = NULL;
    uint64_t seq = vk_pending_seq() + (setc_gen << 40), h = 0, h2 = 0;
    bool fast = (vk_cpu_opts() & 16) != 0;
    if (!off) {
        h = (uint64_t)(uintptr_t)layout;
        h2 = h ^ 0x6A09E667F3BCC909ull;
        for (int i = 0; i < n; i++) {
            uint64_t v[4];
            v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
            if (w[i].pImageInfo) {
                v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
                v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
                v[3] = w[i].pImageInfo->imageLayout;
            } else {
                v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
                v[2] = w[i].pBufferInfo->offset;
                v[3] = w[i].pBufferInfo->range;
            }
            if (fast) {
                // (mask 16) the four words of a write are folded by two independent 64x64->128 multiplies (they run
                // side by side), so a write adds one multiply to the chain through the hash instead of four multiply-shift
                // steps for each of two hashes (llvm-mca, Zen 3: ~21 cycles a write before, ~6 now; a 10-write set was
                // ~210 cycles, 0.5-0.64 ms a frame in the open world). The second hash is the same fold mixed another way,
                // so the key is effectively 64 bits: a wrong hit needs a 2^-64 collision (~1e6 lookups a second).
                uint64_t t = mum64(v[1] ^ 0xA0761D6478BD642Full, v[2] ^ 0xE7037ED1A0B428DBull) ^
                             mum64(v[3] ^ 0x8EBC6AF09C88C6E3ull, v[0] ^ 0x589965CC75374CC3ull);
                h = mum64(h ^ t, 0x9E3779B97F4A7C15ull);
                h2 = (h2 ^ t) * 0xC2B2AE3D27D4EB4Full;
            } else
            for (int k = 0; k < 4; k++) {
                h = (h ^ v[k]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
                h2 = (h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; h2 ^= h2 >> 31;
            }
        }
        h ^= h >> 32;
        if (!setc) setc = calloc(SETC_TAB, sizeof *setc);
        e = &setc[(h ^ (h >> 31)) & (SETC_TAB - 1)];
        if (e->seq == seq && e->hash == h && e->hash2 == h2 && e->layout == layout && e->n == (uint32_t)n) {
            stat_sets_reused++;
            return e->set;
        }
    }
    VkDescriptorSet ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) rec_UpdateDescriptorSets((uint32_t)n, w);
    if (e) {
        e->seq = vk_pending_seq() + (setc_gen << 40);  // (the set lives in this submission's pools)
        e->hash = h; e->hash2 = h2; e->layout = layout; e->set = ds; e->n = (uint32_t)n;
    }
    return ds;
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
// dyn / ndyn: the offsets of its dynamic uniform buffers, by binding (ubo_dynamic_mark), appended.
// idxs / nb: the bindings of this set; has_rscale / has_tint: whether it binds cbuf 18 / 19 (prog_index_sets).
// The descriptor writes of one set: 11 KB, which made the frame of stage_set 15 KB, and a frame over one page is entered
// through ___chkstk_ms on Windows, a call that probes every page, for each of the 8,900 sets a frame (0.09 ms of the
// queue thread in a profile capture, with draw_'s own). With cpuopts bit 128 they live in static memory instead (the draw
// path runs on one thread, like the other statics here); a call made while one is in use (not known to happen) takes
// the stack copy.
typedef struct {
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
} PrepScratch;

static inline __attribute__((always_inline)) VkDescriptorSet stage_set_prepare_in(PrepScratch *scratch, const VkBindingInfo *binds, const uint8_t *idxs, int nb,
                                 bool has_rscale, bool has_tint, VkDescriptorSetLayout layout, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, int *ndyn) {
    vk_exec_gmem_stage_prepare(stage, si);
    struct { uint32_t binding, off; } dv[32];
    int nd = 0;
    VkWriteDescriptorSet *w = scratch->w;
    VkDescriptorBufferInfo *bi = scratch->bi;
    VkDescriptorImageInfo *ii = scratch->ii;
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    for (int k = 0; k < nb; k++) {
        const VkBindingInfo *b = &binds[idxs[k]];
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = drw.tex_sc[sg][ti][0]; sc[1] = drw.tex_sc[sg][ti][1]; wh[0] = drw.tex_wh[sg][ti][0]; wh[1] = drw.tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else if (b->binding == RT_MASK_BINDING) { ii[n] = rtr_mask_info(); sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            else if (b->binding == RT_REFL_BINDING) { ii[n] = rtr_refl_info(); sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            else if (b->binding == RT_RADD_BINDING) { ii[n] = rtr_radd_info(); sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                drw.tex_sc[sg][ti][0] = sc[0]; drw.tex_sc[sg][ti][1] = sc[1];
                drw.tex_wh[sg][ti][0] = wh[0]; drw.tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == GMEM_BUFFER_INDEX && maxwell_gmem_bda) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && nd < 32) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            int dk = nd++;  // (by binding: the order vkCmdBindDescriptorSets takes them in)
            while (dk > 0 && dv[dk - 1].binding > b->binding) { dv[dk] = dv[dk - 1]; dk--; }
            dv[dk].binding = b->binding;
            dv[dk].off = (uint32_t)bi[n].offset;
            bi[n].offset = 0;
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == MVEC_HISTORY_BINDING ? mvec_history_info()
                  : b->binding == RTR_CAP_BINDING      ? rtr_cap_info()
                  : b->binding == GSNAP_BUFFER_INDEX   ? gsnap_info(stage)
                  : b->binding == GMEM2_BUFFER_INDEX   ? gmem_info_hi()
                                                       : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;
        }
        n++;
    }
    if (has_rscale) {
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = 18,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        if (!rscale_row0 && !si->ntex) {
            static uint64_t id_seq;
            static VkDescriptorBufferInfo id_bi;
            uint64_t seq = vk_pending_seq();
            if (id_seq != seq) {
                // (room for the declared 65 rows, as copy_dedupe_in's alloc: the descriptor's range must stay in the
                // buffer; only row 0 is read with no textures)
                static const float rs1[4] = {1, 1, 0, 0};
                VkSpan sp = vk_transient(sizeof(float[65][4]), 256);
                memcpy(sp.ptr, rs1, sizeof rs1);
                id_bi = ubo_span(sp, sizeof(float[65][4]));
                id_seq = vk_pending_seq();
            }
            bi[n] = id_bi;
        } else {
            int rows = 1 + (si->ntex < 64 ? si->ntex : 64);
            float rs[65][4];
            if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
            else rs[0][0] = rs[0][1] = 1, rs[0][2] = rs[0][3] = 0;
            for (int t = 0; t < si->ntex && t < 64; t++)
                rs[1 + t][0] = drw.tex_sc[sg][t][0], rs[1 + t][1] = drw.tex_sc[sg][t][1], rs[1 + t][2] = rs[1 + t][3] = 0;
            bi[n] = ubo_span(copy_dedupe_in(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, (uint64_t)rows * 16, sizeof rs, 256), sizeof rs);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (has_tint) {
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = 19,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        if (!tint) {
            static uint64_t zero_seq;
            static VkDescriptorBufferInfo zero_bi;
            uint64_t seq = vk_pending_seq();
            if (zero_seq != seq) {
                float zero[8] = {0};
                zero_bi = ubo_span(vk_transient_copy(zero, 32, 256), 32);
                zero_seq = vk_pending_seq();
            }
            bi[n] = zero_bi;
        } else {
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    for (int i = 0; i < nd && dyn; i++) dyn[(*ndyn)++] = dv[i].off;
    return set_finish(layout, w, n);
}

static __attribute__((noinline)) VkDescriptorSet stage_set_prepare_stack(const VkBindingInfo *binds, const uint8_t *idxs, int nb,
                                 bool has_rscale, bool has_tint, VkDescriptorSetLayout layout, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, int *ndyn) {
    PrepScratch scratch;
    return stage_set_prepare_in(&scratch, binds, idxs, nb, has_rscale, has_tint, layout, stage, si, rscale_row0, tint, gx, gy, gz, pre_ii, dyn, ndyn);
}

VkDescriptorSet vk_exec_stage_set(const VkBindingInfo *binds, const uint8_t *idxs, int nb, bool has_rscale, bool has_tint,
                                  VkDescriptorSetLayout layout, int stage, ShaderInfo *si,
                                  const float *rscale_row0, const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz,
                                  const VkDescriptorImageInfo *pre_ii, uint32_t *dyn, int *ndyn) {
    static PrepScratch shared __attribute__((aligned(64)));
    static bool busy;
    if ((vk_cpu_opts() & 128) && !busy) {
        busy = true;
        VkDescriptorSet d = stage_set_prepare_in(&shared, binds, idxs, nb, has_rscale, has_tint, layout, stage, si, rscale_row0, tint, gx, gy, gz, pre_ii, dyn, ndyn);
        busy = false;
        return d;
    }
    return stage_set_prepare_stack(binds, idxs, nb, has_rscale, has_tint, layout, stage, si, rscale_row0, tint, gx, gy, gz, pre_ii, dyn, ndyn);
}
