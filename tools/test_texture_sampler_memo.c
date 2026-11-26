#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn/nvn.h"
#include "nvn/maxwell/shader.h"

typedef struct { bool in_feedback_layout; } TexBackend;
static struct { uint64_t tex[6][32]; NvnDescPool *tpool, *spool; uint32_t color_state[1]; } st;
static struct { NvnTexture *t; } scene_up;
static uint32_t vk_view_gen;
static int nfeedback;
static struct { int mvec; } ps = {.mvec = -1};  // (exec/internal.h ExecPass: the one field texture_info reads)
static float scene_mip_bias;
static bool rt_jitter_on(void) { return false; }
static uint64_t vk_frame_counter(void) { return 1; }
static unsigned resolutions;
#include "registration.inc"

// Model only the slow resolver. A distinct sampler handle represents each filtering mode.
static void texture_info_(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    (void)stage; (void)si; (void)i;
    const NvnSamplerDesc *slot = st.spool->entries;
    *out = (VkDescriptorImageInfo){.sampler = (VkSampler)(uintptr_t)(1 + slot->mag_filter)};
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 2;
    ++resolutions;
}
#include "lookup.inc"

static VkSampler lookup(ShaderInfo *shader, unsigned blend) {
    st.color_state[0] = blend;
    VkDescriptorImageInfo out;
    float scale[2];
    uint32_t size[2];
    texture_info(1, shader, 0, &out, scale, size);
    return out.sampler;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    bool builder = !strcmp(argv[1], "builder");
    NvnTexture tex = {0};
    NvnTexEntry entries[2] = {0};
    entries[1].tex = &tex;
    NvnSamplerDesc slot = {0}, replacement = {0};
    NvnDescPool textures = {.count=2, .entries=entries}, samplers = {.count=1, .entries=&slot};
    st.tpool = &textures; st.spool = &samplers; st.tex[1][0] = 1;
    ShaderInfo shader = {0};
    shader.tex[0].type = 1;
    shader.tex[0].handle_offset = 0x20;
    uintptr_t pool = (uintptr_t)&samplers, object = (uintptr_t)&replacement;
    NvnSamplerBuilderState b = {0};
    Cpu c = {0};
    c.x[0] = (uintptr_t)&pool;
    c.x[2] = builder ? (uintptr_t)&b : (uintptr_t)&object;
    for (unsigned i=0; i<32; ++i) assert((uintptr_t)lookup(&shader, i & 1) == 1);
    assert(resolutions == 2);
    replacement.mag_filter = b.mag_filter = 1;
    for (unsigned i=0; i<32; ++i) {
        if (builder) hle_nvnSamplerPoolRegisterSamplerBuilder(&c); else hle_nvnSamplerPoolRegisterSampler(&c);
        assert((uintptr_t)lookup(&shader, i & 1) == 2);
    }
    assert(resolutions == 4);
    return 0;
}
