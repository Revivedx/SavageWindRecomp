// C shim over ARM astcenc (reference codec) for tools/tests/test_astc.c. Built by tools/tests/test_astc.sh
// against a local clone in build/astc-encoder (never committed).
#include "astcenc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Ctx { unsigned bw, bh; bool srgb, dec; float quality; astcenc_context *ctx; };
std::vector<Ctx> g_ctx;

astcenc_context *get_ctx(unsigned bw, unsigned bh, bool srgb, bool dec, float quality) {
    for (auto &c : g_ctx)
        if (c.bw == bw && c.bh == bh && c.srgb == srgb && c.dec == dec && (dec || c.quality == quality)) return c.ctx;
    astcenc_config cfg;
    // decode_unorm8 = the 8-bit decode the CLI uses for -dl / -ds into 8-bit images
    unsigned flags = ASTCENC_FLG_USE_DECODE_UNORM8 | (dec ? ASTCENC_FLG_DECOMPRESS_ONLY : 0);
    astcenc_profile prof = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    if (astcenc_config_init(prof, bw, bh, 1, dec ? ASTCENC_PRE_MEDIUM : quality, flags, &cfg) != ASTCENC_SUCCESS) {
        fprintf(stderr, "astcenc_config_init failed\n");
        return nullptr;
    }
    astcenc_context *ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1, &ctx, nullptr) != ASTCENC_SUCCESS) {
        fprintf(stderr, "astcenc_context_alloc failed\n");
        return nullptr;
    }
    g_ctx.push_back({bw, bh, srgb, dec, quality, ctx});
    return ctx;
}

}  // namespace

extern "C" int ref_encode(const uint8_t *rgba, unsigned w, unsigned h, unsigned bw, unsigned bh, int srgb,
                          float quality, uint8_t *blocks) {
    astcenc_context *ctx = get_ctx(bw, bh, srgb != 0, false, quality);
    if (!ctx) return -1;
    void *slices[1] = {(void *)rgba};
    astcenc_image img = {w, h, 1, ASTCENC_TYPE_U8, slices};
    astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    size_t len = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    astcenc_error e = astcenc_compress_image(ctx, &img, &swz, blocks, len, 0);
    astcenc_compress_reset(ctx);
    return e == ASTCENC_SUCCESS ? 0 : -1;
}

// Decodes to RGBA8 (U8 output: astcenc infers decode_unorm8 from it, as its CLI does for 8-bit images).
// The U8 store clamps the error colour (NaN) to 0, so a second F32 decode finds the error texels: magenta.
extern "C" int ref_decode(const uint8_t *blocks, unsigned w, unsigned h, unsigned bw, unsigned bh, int srgb,
                          uint8_t *rgba) {
    astcenc_context *ctx = get_ctx(bw, bh, srgb != 0, true, 0);
    if (!ctx) return -1;
    astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    size_t len = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    void *slices8[1] = {rgba};
    astcenc_image img8 = {w, h, 1, ASTCENC_TYPE_U8, slices8};
    astcenc_error e = astcenc_decompress_image(ctx, blocks, len, &img8, &swz, 0);
    astcenc_decompress_reset(ctx);
    if (e != ASTCENC_SUCCESS) return -1;
    std::vector<float> f((size_t)w * h * 4);
    void *slices[1] = {f.data()};
    astcenc_image img = {w, h, 1, ASTCENC_TYPE_F32, slices};
    e = astcenc_decompress_image(ctx, blocks, len, &img, &swz, 0);
    astcenc_decompress_reset(ctx);
    if (e != ASTCENC_SUCCESS) return -1;
    static const uint8_t magenta[4] = {0xFF, 0x00, 0xFF, 0xFF};
    for (size_t i = 0; i < (size_t)w * h; i++) {
        bool nan = false;
        for (int c = 0; c < 4; c++) nan |= std::isnan(f[4 * i + c]);
        if (nan) memcpy(rgba + 4 * i, magenta, 4);
    }
    return 0;
}

// Debug dump of the reference's view of one block.
extern "C" void ref_block_dump(const uint8_t block[16], unsigned bw, unsigned bh, int srgb) {
    astcenc_context *ctx = get_ctx(bw, bh, srgb != 0, true, 0);
    astcenc_block_info bi;
    if (!ctx || astcenc_get_block_info(ctx, block, &bi) != ASTCENC_SUCCESS) return;
    printf("ref: error %d const %d hdr %d dual %d (comp %u) partitions %u (index %u) color levels %u weight levels %u "
           "grid %ux%u\n", bi.is_error_block, bi.is_constant_block, bi.is_hdr_block, bi.is_dual_plane_block,
           bi.dual_plane_component, bi.partition_count, bi.partition_index, bi.color_level_count,
           bi.weight_level_count, bi.weight_x, bi.weight_y);
    for (unsigned p = 0; p < bi.partition_count; p++)
        printf("  cem %u ep0 %g %g %g %g ep1 %g %g %g %g\n", bi.color_endpoint_modes[p],
               bi.color_endpoints[p][0][0], bi.color_endpoints[p][0][1], bi.color_endpoints[p][0][2],
               bi.color_endpoints[p][0][3], bi.color_endpoints[p][1][0], bi.color_endpoints[p][1][1],
               bi.color_endpoints[p][1][2], bi.color_endpoints[p][1][3]);
    for (unsigned i = 0; i < bi.texel_count; i++)
        printf("  t%u p%u w %g %g\n", i, bi.partition_assignment[i], bi.weight_values_plane1[i],
               bi.weight_values_plane2[i]);
}

// Block statistics for coverage: returns 0 error, 1 constant, 2 normal; fills partitions / dual / CEMs.
extern "C" int ref_block_info(const uint8_t block[16], unsigned bw, unsigned bh, int srgb, int *partitions,
                              int *dual, int cems[4], int *hdr) {
    astcenc_context *ctx = get_ctx(bw, bh, srgb != 0, true, 0);
    astcenc_block_info bi;
    if (!ctx || astcenc_get_block_info(ctx, block, &bi) != ASTCENC_SUCCESS) return -1;
    *hdr = bi.is_hdr_block;
    if (bi.is_error_block) return 0;
    if (bi.is_constant_block) return 1;
    *partitions = (int)bi.partition_count;
    *dual = bi.is_dual_plane_block;
    for (int i = 0; i < 4; i++) cems[i] = i < (int)bi.partition_count ? (int)bi.color_endpoint_modes[i] : -1;
    return 2;
}
