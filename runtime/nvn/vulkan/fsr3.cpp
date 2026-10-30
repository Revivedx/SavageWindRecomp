// AMD FidelityFX Super Resolution 3.1's upscaler through the FidelityFX SDK's own context and Vulkan backend (fsr3.h)
#include "fsr3.h"

#ifdef SW_FSR3
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#include "../../rt.h"

static FfxFsr3UpscalerContext ctx;
static bool have_ctx;
// the images the upscaler keeps from frame to frame that FSR 3.1 has the application own (dilated depth and motion
// vectors, the previous frame's nearest depth), made by the backend for a context of their own, as ffx_fsr3.cpp does
static FfxInterface iface;
static FfxUInt32 shared_ctx;
static FfxResourceInternal shared[3];
static void *scratch;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static VkDevice ctx_dev;

// The scratch memory the backend lays its contexts out in: 64-byte aligned (they are alignas(32), and the vendored
// ffx_vk.cpp starts its arrays on 64-byte boundaries: third_party/FidelityFX/README.md)
static void *scratch_alloc(size_t n) {
#ifdef _WIN32
    return _aligned_malloc(n, 64);
#else
    return aligned_alloc(64, n);
#endif
}
static void scratch_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static void message(FfxMsgType type, const wchar_t *msg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    LOG("[fsr3] %s: %s\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

extern "C" bool fsr3_available(void) { return true; }

extern "C" void fsr3_destroy(void) {
    if (!have_ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    for (int i = 0; i < 3; i++) iface.fpDestroyResource(&iface, shared[i], shared_ctx);
    iface.fpDestroyBackendContext(&iface, shared_ctx);
    ffxFsr3UpscalerContextDestroy(&ctx);
    have_ctx = false;
    scratch_free(scratch);
    scratch = NULL;
}

// The backend's device functions: an extension's KHR name that this device was not made with is the core function
// it was promoted to (the backend calls vkGetBufferMemoryRequirements2KHR without checking it), and its debug labels
// do nothing without VK_EXT_debug_utils (as framegen.cpp's)
// (its job labels become SAVAGEWIND_GPUPROF_AT marks: the time of each of FSR 3's passes)
extern "C" void vk_gpuprof_mark(VkCommandBuffer cb, const char *what);
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) {
    char buf[96];
    snprintf(buf, sizeof buf, "FSR 3: %s", l && l->pLabelName ? l->pLabelName : "?");
    vk_gpuprof_mark(cb, buf);
}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static PFN_vkVoidFunction VKAPI_PTR device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    if (!f && !strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!f && !strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(dev, core);
    }
    return f;
}

static bool make_context(const Fsr3Dispatch *d) {
    fsr3_destroy();
    enum { CONTEXTS = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1 };  // (+ the shared images')
    size_t n = ffxGetScratchMemorySizeVK(d->phys, CONTEXTS);
    n = (n + 63) & ~(size_t)63;
    scratch = scratch_alloc(n);
    memset(scratch, 0, n);
    static VkDeviceContext dc;  // (the backend keeps a pointer to it)
    dc = {d->dev, d->phys, device_proc};
    FfxFsr3UpscalerContextDescription cd = {};
    FfxErrorCode e = ffxGetInterfaceVK(&cd.backendInterface, ffxGetDeviceVK(&dc), scratch, n, CONTEXTS);
    if (e != FFX_OK) { LOG("[fsr3] backend: error %d\n", (int)e); scratch_free(scratch); scratch = NULL; return false; }
    // (LDR input, low-resolution and jitter-free motion vectors, standard depth; FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING
    // with SAVAGEWIND_FSR3_DEBUG=1: its checks of the inputs, in the log)
    cd.flags = getenv("SAVAGEWIND_FSR3_DEBUG") ? FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING : 0;
    cd.maxRenderSize = {d->color.width, d->color.height};
    cd.maxUpscaleSize = {d->output.width, d->output.height};
    cd.fpMessage = message;
    e = ffxFsr3UpscalerContextCreate(&ctx, &cd);
    if (e != FFX_OK) { LOG("[fsr3] context: error %d\n", (int)e); scratch_free(scratch); scratch = NULL; return false; }
    iface = cd.backendInterface;
    FfxFsr3UpscalerSharedResourceDescriptions sd = {};
    e = ffxFsr3UpscalerGetSharedResourceDescriptions(&ctx, &sd);
    if (e == FFX_OK) e = iface.fpCreateBackendContext(&iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &shared_ctx);
    const FfxCreateResourceDescription *rd[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3 && e == FFX_OK; i++) e = iface.fpCreateResource(&iface, rd[i], shared_ctx, &shared[i]);
    if (e != FFX_OK) {
        LOG("[fsr3] shared images: error %d\n", (int)e);
        ffxFsr3UpscalerContextDestroy(&ctx);
        scratch_free(scratch);
        scratch = NULL;
        return false;
    }
    have_ctx = true;
    ctx_dev = d->dev;
    ctx_rw = d->color.width, ctx_rh = d->color.height, ctx_ow = d->output.width, ctx_oh = d->output.height;
    LOG("[fsr3] context %ux%u -> %ux%u\n", ctx_rw, ctx_rh, ctx_ow, ctx_oh);
    return true;
}

static FfxResource resource(const Fsr3Image *m, bool uav, const wchar_t *name) {
    FfxResourceDescription rd = {};
    rd.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxGetSurfaceFormatVK(m->format);
    rd.width = m->width, rd.height = m->height, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK((void *)m->image, rd, name, FFX_RESOURCE_STATE_COMMON);  // (COMMON: GENERAL, kept)
}

extern "C" bool fsr3_dispatch(const Fsr3Dispatch *d) {
    static bool failed;
    if (failed) return false;
    if ((!have_ctx || d->color.width != ctx_rw || d->color.height != ctx_rh || d->output.width != ctx_ow ||
         d->output.height != ctx_oh) && !make_context(d)) {
        failed = true;  // (no FSR 3 this run: the scene goes on with the game's own composite)
        return false;
    }
    FfxFsr3UpscalerDispatchDescription dd = {};
    dd.commandList = ffxGetCommandListVK(d->cb);
    dd.color = resource(&d->color, false, L"FSR3_Color");
    dd.depth = resource(&d->depth, false, L"FSR3_Depth");
    dd.motionVectors = resource(&d->motion, false, L"FSR3_MotionVectors");
    dd.reactive = resource(&d->reactive, false, L"FSR3_Reactive");
    dd.output = resource(&d->output, true, L"FSR3_Output");
    dd.dilatedDepth = iface.fpGetResource(&iface, shared[0]);
    dd.dilatedMotionVectors = iface.fpGetResource(&iface, shared[1]);
    dd.reconstructedPrevNearestDepth = iface.fpGetResource(&iface, shared[2]);
    dd.jitterOffset = {d->jitter[0], d->jitter[1]};
    dd.motionVectorScale = {d->motion_scale[0], d->motion_scale[1]};
    dd.renderSize = {d->color.width, d->color.height};
    dd.upscaleSize = {d->output.width, d->output.height};
    dd.enableSharpening = d->sharpness > 0;
    dd.sharpness = d->sharpness;
    dd.frameTimeDelta = d->dt_ms;
    dd.preExposure = 1.0f;
    dd.reset = d->reset;
    dd.cameraNear = d->near_;
    dd.cameraFar = d->far_;
    dd.cameraFovAngleVertical = d->fov_y;
    dd.viewSpaceToMetersFactor = 1.0f;  // (BOTW's units are meters)
    static int view = -1;  // SAVAGEWIND_FSR3_VIEW=1: FSR's own debug view of its inputs and state in the output
    if (view < 0) view = getenv("SAVAGEWIND_FSR3_VIEW") != NULL;
    if (view) dd.flags |= FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&ctx, &dd);
    if (e != FFX_OK) {
        static int logged;
        if (logged++ < 8) LOG("[fsr3] dispatch: error %d\n", (int)e);
        return false;
    }
    return true;
}

extern "C" int fsr3_jitter_phases(uint32_t render_w, uint32_t output_w) {
    return ffxFsr3UpscalerGetJitterPhaseCount((int32_t)render_w, (int32_t)output_w);
}

extern "C" void fsr3_jitter_offset(int i, int phases, float *x, float *y) {
    ffxFsr3UpscalerGetJitterOffset(x, y, i, phases);
}

#else
extern "C" bool fsr3_available(void) { return false; }
extern "C" bool fsr3_dispatch(const Fsr3Dispatch *) { return false; }
extern "C" int fsr3_jitter_phases(uint32_t, uint32_t) { return 8; }
extern "C" void fsr3_jitter_offset(int, int, float *x, float *y) { *x = *y = 0; }
extern "C" void fsr3_destroy(void) {}
#endif
