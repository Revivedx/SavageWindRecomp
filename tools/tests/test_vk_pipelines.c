// GPU test of the Vulkan shader path without game data: programs synthesized by tools/tests/test_translate.c are
// translated to GLSL and pushed through the backend's own compile + SPIR-V reflection + descriptor layouts
// (runtime/nvn/vulkan/shaders.c), then real pipelines are created on the device the way exec.c does
// (vertex inputs per attr_kind, colour targets per rt_kind, D32S8 depth, the same dynamic state).
// The Khronos validation layer is on when available; any message counts as a failure.
//
// usage: test_vk_pipelines <translate.c> <count> <seed>     (tools/tests/test_vk_pipelines.sh builds and runs it)
#define main test_translate_main
#include "test_translate.c"
#undef main

#include <vulkan/vulkan.h>

#include "../../runtime/nvn/vulkan/shaders.h"

// ---- what shaders.c needs from the rest of the runtime
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
ShaderInfo *old_maxwell_translate(const uint8_t *c, size_t m, int s, const ShaderKey *k) { (void)c, (void)m, (void)s, (void)k; return NULL; }
void old_maxwell_free(ShaderInfo *s) { (void)s; }
uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;

static long g_messages;
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT t,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)t, (void)u;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (g_messages++ < 20) fprintf(stderr, "[validation] %s\n", d->pMessage);
    }
    return VK_FALSE;
}

static void init_device(void) {
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *ext[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledLayerCount = 1,
                               .ppEnabledLayerNames = layers, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
    bool validate = vkCreateInstance(&ci, NULL, &g_vk.inst) == VK_SUCCESS;
    if (!validate) {
        ci.enabledLayerCount = ci.enabledExtensionCount = 0;
        if (vkCreateInstance(&ci, NULL, &g_vk.inst) != VK_SUCCESS) { fprintf(stderr, "no Vulkan\n"); exit(1); }
        fprintf(stderr, "(validation layer not available: driver errors only)\n");
    } else {
        VkDebugUtilsMessengerCreateInfoEXT mi = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                 .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                                                 .pfnUserCallback = on_message};
        PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m;
        if (f) f(g_vk.inst, &mi, NULL, &m);
    }
    uint32_t n = 8;
    VkPhysicalDevice pd[8];
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    g_vk.phys = pd[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_vk.phys = pd[i]; break; }
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    fprintf(stderr, "device: %s\n", g_vk.props.deviceName);
    // the feature set device.c enables
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE,
                                            .synchronization2 = VK_TRUE, .maintenance4 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12,
                                            .storageBuffer16BitAccess = VK_TRUE};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11, .features = have.features};
    f.features.robustBufferAccess = VK_FALSE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    if (vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); exit(1); }
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};

typedef struct { VkBindingInfo b[96]; int n; VkDescriptorSetLayout sl[2]; VkPipelineLayout pl; } Layout;

static bool make_layout(Layout *l, const uint32_t *const *spv, const size_t *nw, int nmod, int nsets) {
    l->n = 0;
    for (int i = 0; i < nmod; i++) l->n += vk_reflect(spv[i], nw[i], l->b + l->n, 96 - l->n);
    for (int i = 0; i < l->n; i++) {  // the binding numbers the renderer fills (exec.c stage_set)
        VkBindingInfo *b = &l->b[i];
        bool ok = b->binding >= 32 ? b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : b->binding == 17 ? b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                : b->binding <= 19 ? b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : false;
        if (!ok || (int)b->set >= nsets) { fprintf(stderr, "unexpected binding set %u binding %u type %d\n", b->set, b->binding, b->type); return false; }
    }
    VkShaderStageFlags st[2] = {nsets == 1 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (int s = 0; s < nsets; s++) l->sl[s] = vk_set_layout(l->b, l->n, (uint32_t)s, st[s]);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets, .pSetLayouts = l->sl};
    return vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l->pl) == VK_SUCCESS;
}

static void free_layout(Layout *l, int nsets) {
    vkDestroyPipelineLayout(g_vk.dev, l->pl, NULL);
    for (int s = 0; s < nsets; s++) vkDestroyDescriptorSetLayout(g_vk.dev, l->sl[s], NULL);
}

static long n_pairs, n_cs, n_skip, n_fail;

static void fail(const char *what, const char *a, const char *b) {
    if (n_fail++ < 10) fprintf(stderr, "FAIL %s\n---\n%s\n---\n%s\n", what, a ? a : "", b ? b : "");
}

static void test_graphics(void) {
    static uint8_t vp[0x80 + 64 * 32 + 128], fp[0x80 + 64 * 32 + 128];
    size_t vl = gen_program(vp, sizeof vp, STAGE_VERTEX), fl = gen_program(fp, sizeof fp, STAGE_FRAGMENT);
    ShaderKey vk_, fk;
    gen_key(&vk_, STAGE_VERTEX);
    gen_key(&fk, STAGE_FRAGMENT);
    vk_.glsl = fk.glsl = 1;
    ShaderInfo *vi = maxwell_translate(vp, vl, STAGE_VERTEX, &vk_);
    // (fuzz artifact: 31+ varyings plus position exceed every GPU's output limit; real shaders can't)
    if (!vi->msl || (vi->out_mask >> 30)) { maxwell_free(vi); n_skip++; return; }
    fk.vs_out_mask = vi->out_mask;
    memcpy(fk.attr_kind, vk_.attr_kind, sizeof fk.attr_kind);
    ShaderInfo *fi = maxwell_translate(fp, fl, STAGE_FRAGMENT, &fk);
    if (!fi->msl) { maxwell_free(vi); maxwell_free(fi); n_skip++; return; }
    n_pairs++;
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "vs", &nv), *sf = vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "fs", &nf);
    Layout l;
    const uint32_t *mods[2] = {sv, sf};
    size_t nws[2] = {nv, nf};
    VkShaderModule vm = sv ? vk_module(sv, nv) : VK_NULL_HANDLE, fm = sf ? vk_module(sf, nf) : VK_NULL_HANDLE;
    if (!vm || !fm || !make_layout(&l, mods, nws, 2, 2)) { fail("compile / layout", vi->msl, fi->msl); goto out; }
    {
        long before = g_messages;
        VkVertexInputAttributeDescription va[32];
        VkVertexInputBindingDescription vb = {0, 64, VK_VERTEX_INPUT_RATE_VERTEX};
        uint32_t na = 0;
        for (int i = 0; i < 32; i++)
            if (vi->attr_mask & (1u << i))
                va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, 0,
                    vk_.attr_kind[i] == 1 ? VK_FORMAT_R32G32B32A32_SINT : vk_.attr_kind[i] == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                    .vertexBindingDescriptionCount = na ? 1 : 0, .pVertexBindingDescriptions = &vb,
                                                    .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
        VkFormat cf[8];
        VkPipelineColorBlendAttachmentState cba[8] = {0};
        uint32_t nc = 0, omap = *(const uint32_t *)(fp + 0x30 + 0x48);
        for (int rt = 0; rt < 8; rt++) if ((omap >> (rt * 4)) & 0xF) nc = (uint32_t)rt + 1;
        for (uint32_t rt = 0; rt < nc; rt++) {
            cf[rt] = fk.rt_kind[rt] == 1 ? VK_FORMAT_R32G32B32A32_SINT : fk.rt_kind[rt] == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R8G8B8A8_UNORM;
            cba[rt].colorWriteMask = 0xF;
            cba[rt].blendEnable = fk.rt_kind[rt] == 0 && (rt & 1);
            cba[rt].srcColorBlendFactor = cba[rt].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba[rt].dstColorBlendFactor = cba[rt].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = nc, .pAttachments = cba};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = sizeof DYN / sizeof DYN[0], .pDynamicStates = DYN};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = nc,
                                            .pColorAttachmentFormats = cf, .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT,
                                            .stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                           .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                           .pColorBlendState = &cb, .pDynamicState = &dy, .layout = l.pl};
        VkPipeline p;
        VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
        if (r != VK_SUCCESS || g_messages != before) fail("graphics pipeline", vi->msl, fi->msl);
        if (r == VK_SUCCESS) vkDestroyPipeline(g_vk.dev, p, NULL);
        free_layout(&l, 2);
    }
out:
    if (vm) vkDestroyShaderModule(g_vk.dev, vm, NULL);
    if (fm) vkDestroyShaderModule(g_vk.dev, fm, NULL);
    free(sv); free(sf);
    maxwell_free(vi); maxwell_free(fi);
}

static void test_compute(void) {
    static uint8_t cp[0x80 + 64 * 32 + 128];
    size_t cl = gen_program(cp, sizeof cp, STAGE_COMPUTE);
    ShaderKey k;
    gen_key(&k, STAGE_COMPUTE);
    k.glsl = 1;
    ShaderInfo *ci = maxwell_translate(cp, cl, STAGE_COMPUTE, &k);
    if (!ci->msl) { maxwell_free(ci); n_skip++; return; }
    n_cs++;
    size_t n = 0;
    uint32_t *s = vk_glsl_to_spirv(ci->msl, STAGE_COMPUTE, "cs", &n);
    VkShaderModule m = s ? vk_module(s, n) : VK_NULL_HANDLE;
    Layout l;
    const uint32_t *mods[1] = {s};
    size_t nws[1] = {n};
    if (!m || !make_layout(&l, mods, nws, 1, 1)) { fail("compute compile / layout", ci->msl, NULL); goto out; }
    {
        long before = g_messages;
        uint32_t local[3] = {1 + rnd(64), 1 + rnd(4), 1};
        VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
        VkSpecializationInfo spec = {3, me, 12, local};
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = m, .pName = "main", .pSpecializationInfo = &spec},
                                          .layout = l.pl};
        VkPipeline p;
        VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
        if (r != VK_SUCCESS || g_messages != before) fail("compute pipeline", ci->msl, NULL);
        if (r == VK_SUCCESS) vkDestroyPipeline(g_vk.dev, p, NULL);
        free_layout(&l, 1);
    }
out:
    if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    free(s);
    maxwell_free(ci);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed>\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    maxwell_gmem_base = 0x7240000000ull;
    maxwell_gmem_size = 0xD0000000ull;
    init_device();
    vk_shaders_init();
    for (long i = 0; i < count; i++) {
        if (rnd(4) == 0) test_compute(); else test_graphics();
        if ((i + 1) % 500 == 0) fprintf(stderr, "%ld: %ld pairs, %ld compute, %ld skipped (translation failed), %ld failures\n",
                                        i + 1, n_pairs, n_cs, n_skip, n_fail);
    }
    printf("%ld graphics pairs, %ld compute programs, %ld skipped, %ld failures, %ld validation messages\n", n_pairs, n_cs,
           n_skip, n_fail, g_messages);
    return n_fail != 0;
}
