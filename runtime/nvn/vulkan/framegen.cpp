// AMD FSR 3 frame generation (Video > Frame generation): the FidelityFX SDK 1.1.4's frame interpolation and optical
// flow (third_party/FidelityFX, MIT) on its Vulkan backend, driven directly instead of through the SDK's frame
// interpolation swapchain (Windows only, and it would own the present: the runtime's present thread, its HDR10 pass
// and VSync modes stay). exec.c vk_fg_generate gives it the frame's images and camera and presents the result
// (device.c), see docs/SAVAGE-WIND.md "Frame generation".
//
// What the SDK's ffx-api provider (ffx_provider_framegeneration.cpp) does, in C: a shared backend context for the
// resources the two effects exchange (optical flow vectors, scene change detection, dilated depth and motion
// vectors, the previous frame's reconstructed depth, two of each that alternate), the optical flow context and the
// frame interpolation context on a second backend interface; per frame, frame interpolation's prepare (dilates the
// depth and motion vectors), the optical flow, then the interpolation.
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>

#include "framegen.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

// ---- the shaders: tools/ffx_shaders.py's blobs (wave32, 32-bit floats; frame interpolation for low-resolution
// motion vectors without jitter, both depth directions; the FSR 3 upscaler of vulkan/fsr3.cpp) in place of the SDK's
// FidelityFX_SC permutation tables
#include "ffx_shaders_vk.h"
#include "ffx_fsr3upscaler_vk.h"

enum { FI_PERM_DEPTH_INVERTED = 1 << 2 };
enum { FU_PERM_FP16 = 1 << 7 };  // (ffx_fsr3upscaler_private.h FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16)
extern "C" bool vk_shader_fp16(void);  // (device.c)  // (ffx_frameinterpolation_private.h FRAMEINTERPOLATION_SHADER_PERMUTATION_*)

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options,
                                                     FfxShaderBlob *out) {
    if (effect == FFX_EFFECT_FRAMEINTERPOLATION && pass < FFX_FRAMEINTERPOLATION_PASS_COUNT) {
        memcpy(out, &ffx_fi_blobs[(options & FI_PERM_DEPTH_INVERTED) ? 1 : 0][pass], sizeof *out);
        return FFX_OK;
    }
    if (effect == FFX_EFFECT_OPTICALFLOW && pass < FFX_OPTICALFLOW_PASS_COUNT) {
        memcpy(out, &ffx_of_blobs[pass], sizeof *out);
        return FFX_OK;
    }
    // (one permutation per pass, the one fsr3.cpp's context asks for: sharpening is a pass of its own,
    // FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN; its 16-bit version where the SDK allows it - the device reports
    // shaderFloat16 - and the device was made with it: vk_shader_fp16)
    if (effect == FFX_EFFECT_FSR3UPSCALER && pass < FFX_FSR3UPSCALER_PASS_COUNT && ffx_fu_blobs[0][pass].data) {
        int half = (options & FU_PERM_FP16) && vk_shader_fp16();
        memcpy(out, &ffx_fu_blobs[half][pass], sizeof *out);
        return FFX_OK;
    }
    LOG("[fg] no shader for effect %d pass %d\n", (int)effect, (int)pass);
    return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;  // (only wave32 blobs: the backend then asks for no subgroup size)
    return FFX_OK;
}

// The backend's hook into its frame interpolation swapchain, which isn't built
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }

// ---- contexts
enum { R_OF_VECTOR, R_OF_SCD, R_DILATED_DEPTH, R_DILATED_MV = R_DILATED_DEPTH + 2, R_PREV_DEPTH = R_DILATED_MV + 2,
       R_COUNT = R_PREV_DEPTH + 2 };

static struct {
    bool live;
    FgConfig cfg;
    FfxInterface shared, fi_if;
    void *scratch_shared, *scratch_fi;
    uint32_t shared_id;
    FfxOpticalflowContext of;
    FfxFrameInterpolationContext fi;
    FfxResourceInternal res[R_COUNT];
    unsigned toggle;
} g;

FgVk g_fg_vk;

#define TRY(x) do { FfxErrorCode e_ = (x); if (e_ != FFX_OK) { LOG("[fg] %s failed: %#x\n", #x, (unsigned)e_); return false; } } while (0)

// The backend calls a few extension functions unchecked: the debug labels (VK_EXT_debug_utils, an instance
// extension the runtime enables only with validation) and vkGetBufferMemoryRequirements2KHR (core since 1.1 without
// the suffix)
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return f;
    if (!strcmp(name, "vkGetBufferMemoryRequirements2KHR")) return vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2");
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    return nullptr;
}

// (the backend's context in it has members the compiler aligns past malloc's 16 bytes)
static void *scratch_alloc(size_t n) {
#ifdef _WIN32
    void *p = _aligned_malloc(n, 256);
#else
    void *p = aligned_alloc(256, n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

static void scratch_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static bool make_backend(FfxInterface *i, void **scratch, size_t contexts) {
    size_t n = (ffxGetScratchMemorySizeVK(g_fg_vk.phys, contexts) + 255) & ~(size_t)255;
    *scratch = scratch_alloc(n);
    VkDeviceContext dc = {g_fg_vk.dev, g_fg_vk.phys, device_proc};
    TRY(ffxGetInterfaceVK(i, ffxGetDeviceVK(&dc), *scratch, n, contexts));
    return true;
}

extern "C" void fg_destroy(void) {
    if (!g.live) return;
    for (int i = 0; i < R_COUNT; i++) g.shared.fpDestroyResource(&g.shared, g.res[i], g.shared_id);
    ffxFrameInterpolationContextDestroy(&g.fi);
    ffxOpticalflowContextDestroy(&g.of);
    g.shared.fpDestroyBackendContext(&g.shared, g.shared_id);
    scratch_free(g.scratch_shared);
    scratch_free(g.scratch_fi);
    memset(&g, 0, sizeof g);
}

static bool create(const FgConfig *c) {
    g.cfg = *c;
    if (!make_backend(&g.shared, &g.scratch_shared, 1) || !make_backend(&g.fi_if, &g.scratch_fi, 2)) return false;
    TRY(g.shared.fpCreateBackendContext(&g.shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &g.shared_id));
    FfxOpticalflowContextDescription od = {};
    od.backendInterface = g.fi_if;
    od.resolution = {c->display_w, c->display_h};
    TRY(ffxOpticalflowContextCreate(&g.of, &od));
    FfxFrameInterpolationContextDescription fd = {};
    fd.backendInterface = g.fi_if;
    fd.flags = c->depth_inverted ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0;
    fd.maxRenderSize = {c->render_w, c->render_h};
    fd.displaySize = {c->display_w, c->display_h};
    fd.backBufferFormat = ffxGetSurfaceFormatVK(c->color_format);
    fd.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(c->hudless_format);
    TRY(ffxFrameInterpolationContextCreate(&g.fi, &fd));
    FfxOpticalflowSharedResourceDescriptions ord = {};
    TRY(ffxOpticalflowGetSharedResourceDescriptions(&g.of, &ord));
    TRY(g.shared.fpCreateResource(&g.shared, &ord.opticalFlowVector, g.shared_id, &g.res[R_OF_VECTOR]));
    TRY(g.shared.fpCreateResource(&g.shared, &ord.opticalFlowSCD, g.shared_id, &g.res[R_OF_SCD]));
    FfxFrameInterpolationSharedResourceDescriptions frd = {};
    TRY(ffxFrameInterpolationGetSharedResourceDescriptions(&g.fi, &frd));
    for (int i = 0; i < 2; i++) {
        TRY(g.shared.fpCreateResource(&g.shared, &frd.dilatedDepth, g.shared_id, &g.res[R_DILATED_DEPTH + i]));
        TRY(g.shared.fpCreateResource(&g.shared, &frd.dilatedMotionVectors, g.shared_id, &g.res[R_DILATED_MV + i]));
        TRY(g.shared.fpCreateResource(&g.shared, &frd.reconstructedPrevNearestDepth, g.shared_id, &g.res[R_PREV_DEPTH + i]));
    }
    g.live = true;
    LOG("[fg] FSR 3 frame interpolation %ux%u (render %ux%u, color format %d, hudless %d)\n", c->display_w, c->display_h,
        c->render_w, c->render_h, (int)c->color_format, (int)c->hudless_format);
    return true;
}

static FfxResource image(const FgImage *im, const wchar_t *name, bool write) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->w, im->h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    // (every image of the runtime is in GENERAL: vk.h; the backend puts it back in the state given here)
    return ffxGetResourceVK((void *)im->image, ffxGetImageResourceDescriptionVK(im->image, ci, write ? FFX_RESOURCE_USAGE_UAV
                            : FFX_RESOURCE_USAGE_READ_ONLY), name, write ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
                            : FFX_RESOURCE_STATE_GENERIC_READ);
}

static FfxResource shared(int i) { return g.shared.fpGetResource(&g.shared, g.res[i]); }

extern "C" bool fg_dispatch(VkCommandBuffer cb, const FgConfig *c, const FgFrame *f) {
    if (g.live && memcmp(&g.cfg, c, sizeof *c)) {
        g_fg_vk.wait_idle();
        fg_destroy();
    }
    if (!g.live && !create(c)) {
        fg_destroy();
        return false;
    }
    FfxCommandList cl = ffxGetCommandListVK(cb);
    g.toggle ^= 1;
    FfxFrameInterpolationPrepareDescription pd = {};
    pd.commandList = cl;
    pd.renderSize = {c->render_w, c->render_h};
    pd.motionVectorScale = f->motion_scale[0] || f->motion_scale[1] ? FfxFloatCoords2D{f->motion_scale[0], f->motion_scale[1]}
                                                                    : FfxFloatCoords2D{(float)c->render_w, (float)c->render_h};
    pd.frameTimeDelta = f->frame_ms;
    pd.cameraNear = f->near_;
    pd.cameraFar = f->far_;
    pd.viewSpaceToMetersFactor = 1;  // (BOTW's units are metres)
    pd.cameraFovAngleVertical = f->fovy;
    pd.depth = image(&f->depth, L"FG_Depth", false);
    pd.motionVectors = image(&f->motion, L"FG_MotionVectors", false);
    pd.frameID = f->frame_id;
    pd.dilatedDepth = shared(R_DILATED_DEPTH + g.toggle);
    pd.dilatedMotionVectors = shared(R_DILATED_MV + g.toggle);
    pd.reconstructedPrevDepth = shared(R_PREV_DEPTH + g.toggle);
    memcpy(pd.cameraPosition, f->cam_pos, sizeof pd.cameraPosition);
    memcpy(pd.cameraUp, f->cam_up, sizeof pd.cameraUp);
    memcpy(pd.cameraRight, f->cam_right, sizeof pd.cameraRight);
    memcpy(pd.cameraForward, f->cam_fwd, sizeof pd.cameraForward);
    TRY(ffxFrameInterpolationPrepare(&g.fi, &pd));

    FfxResource hudless = image(&f->hudless, L"FG_HudlessColor", false);
    FfxOpticalflowDispatchDescription od = {};
    od.commandList = cl;
    od.color = hudless;
    od.reset = f->reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.opticalFlowVector = shared(R_OF_VECTOR);
    od.opticalFlowSCD = shared(R_OF_SCD);
    TRY(ffxOpticalflowContextDispatch(&g.of, &od));

    FfxFrameInterpolationDispatchDescription dd = {};
    dd.flags = f->debug_view ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW : 0;
    dd.commandList = cl;
    dd.displaySize = {c->display_w, c->display_h};
    dd.renderSize = {c->render_w, c->render_h};
    dd.currentBackBuffer = image(&f->color, L"FG_Color", false);
    dd.currentBackBuffer_HUDLess = hudless;
    dd.output = image(&f->out, L"FG_Output", true);
    dd.interpolationRect = {0, 0, (int)c->display_w, (int)c->display_h};
    dd.opticalFlowVector = shared(R_OF_VECTOR);
    dd.opticalFlowSceneChangeDetection = shared(R_OF_SCD);
    dd.opticalFlowBufferSize = {c->display_w / 8, c->display_h / 8};  // (unused by the shaders: the SDK's provider leaves it too)
    dd.opticalFlowScale = {1.0f / c->display_w, 1.0f / c->display_h};
    dd.opticalFlowBlockSize = 8;
    dd.cameraNear = f->near_;
    dd.cameraFar = f->far_;
    dd.cameraFovAngleVertical = f->fovy;
    dd.viewSpaceToMetersFactor = 1;
    dd.frameTimeDelta = f->frame_ms;
    dd.reset = f->reset;
    dd.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dd.minMaxLuminance[0] = 0;
    dd.minMaxLuminance[1] = 1;
    dd.frameID = f->frame_id;
    dd.dilatedDepth = pd.dilatedDepth;
    dd.dilatedMotionVectors = pd.dilatedMotionVectors;
    dd.reconstructedPrevDepth = pd.reconstructedPrevDepth;
    TRY(ffxFrameInterpolationDispatch(&g.fi, &dd));
    return true;
}
