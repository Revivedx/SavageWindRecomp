// AMD FidelityFX Super Resolution 3.1.6 (built-in SDK) and FSR 4 (FidelityFX API ML upscaler via dynamic Vulkan DLL/SO,
// automatically falling back to built-in FSR 3.1.6 when FSR 4 ML is unavailable on the GPU or DLLs are absent)
#include "fsr3.h"

#ifdef SW_FSR3
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#define FFX_VK_DEVICE_CONTEXT_DEFINED
#include <FidelityFX/ffx_api/vk/ffx_api_vk.h>
#include <FidelityFX/ffx_api/ffx_upscale.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#include "../../rt.h"
#include "../../settings.h"

static FfxFsr3UpscalerContext ctx;
static bool have_ctx;
// the images the upscaler keeps from frame to frame that FSR 3.1 has the application own (dilated depth and motion
// vectors, the previous frame's nearest depth), made by the backend for a context of their own, as ffx_fsr3.cpp does
static FfxInterface iface;
static FfxUInt32 shared_ctx;
static FfxResourceInternal shared[3];
static void *scratch;

// FidelityFX API (FSR 4 ML upscaler) dynamic loader & context state
static SDL_SharedObject *ffx_api_so;
static SDL_SharedObject *ffx_sub_so;
static bool ffx_api_probed;
static PfnFfxCreateContext pfn_create;
static PfnFfxDestroyContext pfn_destroy;
static PfnFfxConfigure pfn_configure;
static PfnFfxQuery pfn_query;
static PfnFfxDispatch pfn_dispatch;
static ffxContext api_ctx;
static bool have_api_ctx;

static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static int ctx_mode = -1;
static VkDevice ctx_dev;
static char active_provider[64] = "";

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

static void api_message(uint32_t type, const wchar_t *msg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    LOG("[fsr4] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

extern "C" bool fsr3_available(void) { return true; }
extern "C" bool fsr4_available(void) { return true; }
extern "C" const char *fsr_active_provider(void) { return active_provider; }

extern "C" void fsr3_destroy(void) {
    if (!have_ctx && !have_api_ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    if (have_api_ctx) {
        if (pfn_destroy && api_ctx) pfn_destroy(&api_ctx, nullptr);
        api_ctx = nullptr;
        have_api_ctx = false;
    }
    if (have_ctx) {
        for (int i = 0; i < 3; i++) iface.fpDestroyResource(&iface, shared[i], shared_ctx);
        iface.fpDestroyBackendContext(&iface, shared_ctx);
        ffxFsr3UpscalerContextDestroy(&ctx);
        have_ctx = false;
        scratch_free(scratch);
        scratch = NULL;
    }
    active_provider[0] = 0;
}

// The backend's device functions: an extension's KHR name that this device was not made with is the core function
// it was promoted to (the backend calls vkGetBufferMemoryRequirements2KHR without checking it), and its debug labels
// do nothing without VK_EXT_debug_utils (as framegen.cpp's)
// (its job labels become SAVAGEWIND_GPUPROF_AT marks: the time of each of FSR's passes)
extern "C" void vk_gpuprof_mark(VkCommandBuffer cb, const char *what);
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) {
    char buf[96];
    snprintf(buf, sizeof buf, "FSR: %s", l && l->pLabelName ? l->pLabelName : "?");
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

static SDL_SharedObject *try_load_lib(const char *dir, const char *name) {
    char path[1024];
    if (dir && *dir) snprintf(path, sizeof path, "%s%s", dir, name);
    else snprintf(path, sizeof path, "%s", name);
    return SDL_LoadObject(path);
}

static bool load_ffx_api(void) {
    if (ffx_api_probed) return pfn_create != nullptr;
    ffx_api_probed = true;
#ifdef _WIN32
    const char *sub_name = "amd_fidelityfx_upscaler_vk.dll";
    const char *names[3] = {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_vk.dll", "amd_fidelityfx_upscaler_vk.dll"};
#else
    const char *sub_name = "libamd_fidelityfx_upscaler_vk.so";
    const char *names[3] = {"libamd_fidelityfx_loader_vk.so", "libamd_fidelityfx_vk.so", "libamd_fidelityfx_upscaler_vk.so"};
#endif
    const char *base = SDL_GetBasePath();
    const char *dirs[3] = {base ? base : "", "build/", ""};
    // Preload the upscaler module when placed next to the executable or in build/ so the loader DLL finds it
    // regardless of the process working directory.
    for (int d = 0; d < 3 && !ffx_sub_so; d++) {
        if (d == 0 && (!base || !*base)) continue;
        ffx_sub_so = try_load_lib(dirs[d], sub_name);
    }
    const char *override_dll = getenv("SAVAGEWIND_FFX_DLL");
    if (override_dll && *override_dll) {
        ffx_api_so = SDL_LoadObject(override_dll);
        if (ffx_api_so) LOG("[fsr4] loaded %s\n", override_dll);
    }
    for (int i = 0; i < 3 && !ffx_api_so; i++) {
        for (int d = 0; d < 3 && !ffx_api_so; d++) {
            if (d == 0 && (!base || !*base)) continue;
            ffx_api_so = try_load_lib(dirs[d], names[i]);
            if (ffx_api_so) LOG("[fsr4] loaded %s%s\n", dirs[d], names[i]);
        }
    }
    if (!ffx_api_so) {
        LOG("[fsr4] FidelityFX Vulkan library (%s) not found; using built-in FSR 3.1.6\n", names[0]);
        return false;
    }
    pfn_create = (PfnFfxCreateContext)SDL_LoadFunction(ffx_api_so, "ffxCreateContext");
    pfn_destroy = (PfnFfxDestroyContext)SDL_LoadFunction(ffx_api_so, "ffxDestroyContext");
    pfn_configure = (PfnFfxConfigure)SDL_LoadFunction(ffx_api_so, "ffxConfigure");
    pfn_query = (PfnFfxQuery)SDL_LoadFunction(ffx_api_so, "ffxQuery");
    pfn_dispatch = (PfnFfxDispatch)SDL_LoadFunction(ffx_api_so, "ffxDispatch");
    if (!pfn_create || !pfn_destroy || !pfn_query || !pfn_dispatch) {
        LOG("[fsr4] missing ffx_api entry points in loaded library; using built-in FSR 3.1.6\n");
        pfn_create = nullptr;
        return false;
    }
    return true;
}

static bool is_fsr4_name(const char *name) {
    if (!name) return false;
    if (name[0] == '4' && name[1] == '.') return true;
    return strstr(name, "4.") != nullptr || strstr(name, "FSR 4") != nullptr || strstr(name, "FSR4") != nullptr;
}

static bool make_api_context(const Fsr3Dispatch *d) {
    if (!load_ffx_api()) return false;
    static VkDeviceContext api_dc;
    api_dc = {d->dev, d->phys, device_proc};

    bool force_api = getenv("SAVAGEWIND_FFX_FORCE_API") && atoi(getenv("SAVAGEWIND_FFX_FORCE_API")) != 0;
    uint64_t chosen_id = 0;
    const char *chosen_name = nullptr;

    uint64_t count = 0;
    ffxQueryDescGetVersions qv = {};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    qv.device = &api_dc;
    qv.outputCount = &count;
    if (pfn_query(nullptr, &qv.header) == FFX_API_RETURN_OK && count > 0 && count <= 32) {
        uint64_t ids[32] = {};
        const char *names[32] = {};
        uint64_t cap = count;
        qv.outputCount = &cap;
        qv.versionIds = ids;
        qv.versionNames = names;
        if (pfn_query(nullptr, &qv.header) == FFX_API_RETURN_OK) {
            static bool logged_versions;
            for (uint64_t i = 0; i < cap; i++) {
                if (!logged_versions)
                    LOG("[fsr4] provider %llu: %s (id %#llx)\n", (unsigned long long)i,
                        names[i] ? names[i] : "?", (unsigned long long)ids[i]);
                if (!chosen_id && is_fsr4_name(names[i])) {
                    chosen_id = ids[i];
                    chosen_name = names[i];
                }
            }
            logged_versions = true;
            if (!chosen_id && force_api && cap > 0) {
                chosen_id = ids[0];
                chosen_name = names[0];
            }
        }
    }

    ffxOverrideVersion ov_desc = {};
    ov_desc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov_desc.versionId = chosen_id;

    ffxCreateBackendVKDesc vk_desc = {};
    vk_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    vk_desc.header.pNext = chosen_id ? &ov_desc.header : nullptr;
    vk_desc.deviceContext = api_dc;

    ffxCreateContextDescUpscaleVersion ver_desc = {};
    ver_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver_desc.header.pNext = &vk_desc.header;
    ver_desc.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale up_desc = {};
    up_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up_desc.header.pNext = &ver_desc.header;
    up_desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                    (getenv("SAVAGEWIND_FSR3_DEBUG") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
    up_desc.maxRenderSize = {d->color.width, d->color.height};
    up_desc.maxUpscaleSize = {d->output.width, d->output.height};
    up_desc.fpMessage = api_message;

    api_ctx = nullptr;
    ffxReturnCode_t rc = pfn_create(&api_ctx, &up_desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK && chosen_id) {
        vk_desc.header.pNext = nullptr;
        api_ctx = nullptr;
        rc = pfn_create(&api_ctx, &up_desc.header, nullptr);
        chosen_name = nullptr;
    }
    if (rc != FFX_API_RETURN_OK || !api_ctx) {
        LOG("[fsr4] ffxCreateContext returned %u; falling back to built-in FSR 3.1.6\n", (unsigned)rc);
        api_ctx = nullptr;
        return false;
    }

    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const char *prov_name = chosen_name;
    if (pfn_query(&api_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        prov_name = pv.versionName;

    if (!force_api && !is_fsr4_name(prov_name)) {
        LOG("[fsr4] GPU/driver returned non-FSR-4 provider '%s'; using built-in anti-flicker FSR 3.1.6\n",
            prov_name ? prov_name : "unknown");
        pfn_destroy(&api_ctx, nullptr);
        api_ctx = nullptr;
        return false;
    }

    have_api_ctx = true;
    ctx_dev = d->dev;
    ctx_rw = d->color.width, ctx_rh = d->color.height, ctx_ow = d->output.width, ctx_oh = d->output.height;
    ctx_mode = d->mode;
    snprintf(active_provider, sizeof active_provider, "FSR %s", prov_name ? prov_name : "4");
    LOG("[fsr4] active provider '%s' context %ux%u -> %ux%u\n", active_provider, ctx_rw, ctx_rh, ctx_ow, ctx_oh);
    return true;
}

static bool make_context(const Fsr3Dispatch *d) {
    fsr3_destroy();
    if (d->mode == UPSCALE_FSR4 && make_api_context(d)) return true;

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
    // Linear-light HDR scene input + internal auto-exposure (stops specular/grass luminance flicker), low-resolution
    // jitter-free motion vectors, standard [0, 1] depth; FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING with SAVAGEWIND_FSR3_DEBUG=1
    cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE |
               (getenv("SAVAGEWIND_FSR3_DEBUG") ? FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING : 0);
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
    ctx_mode = d->mode;
    snprintf(active_provider, sizeof active_provider,
             d->mode == UPSCALE_FSR4 ? "FSR 3.1.6 (fallback)" : "FSR 3.1.6");
    LOG("[fsr3] %s context %ux%u -> %ux%u\n", active_provider, ctx_rw, ctx_rh, ctx_ow, ctx_oh);
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

static FfxApiResource api_resource(const Fsr3Image *m, bool uav) {
    FfxApiResourceDescription rd = {};
    rd.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxApiGetSurfaceFormatVK(m->format);
    rd.width = m->width, rd.height = m->height, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_API_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return ffxApiGetResourceVK((void *)m->image, rd,
                               uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

static void transition_api_inputs(VkCommandBuffer cb, const Fsr3Dispatch *d, bool to_read) {
    const VkImage imgs[4] = {d->color.image, d->depth.image, d->motion.image, d->reactive.image};
    VkImageMemoryBarrier b[4] = {};
    for (int i = 0; i < 4; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcAccessMask = to_read ? (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                                     : VK_ACCESS_SHADER_READ_BIT;
        b[i].dstAccessMask = to_read ? VK_ACCESS_SHADER_READ_BIT
                                     : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        b[i].oldLayout = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].newLayout = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = imgs[i];
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cb,
                         to_read ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         to_read ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 4, b);
}

extern "C" bool fsr3_dispatch(const Fsr3Dispatch *d) {
    static bool failed;
    if (failed) return false;
    if (((!have_ctx && !have_api_ctx) || d->mode != ctx_mode || d->color.width != ctx_rw || d->color.height != ctx_rh ||
         d->output.width != ctx_ow || d->output.height != ctx_oh) && !make_context(d)) {
        failed = true;  // (no FSR this run: the scene goes on with the game's own composite)
        return false;
    }
    static int view = -1;  // SAVAGEWIND_FSR3_VIEW=1: FSR's own debug view of its inputs and state in the output
    if (view < 0) view = getenv("SAVAGEWIND_FSR3_VIEW") != NULL;

    if (have_api_ctx) {
        transition_api_inputs(d->cb, d, true);
        ffxDispatchDescUpscale dd = {};
        dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dd.commandList = (void *)d->cb;
        dd.color = api_resource(&d->color, false);
        dd.depth = api_resource(&d->depth, false);
        dd.motionVectors = api_resource(&d->motion, false);
        dd.reactive = api_resource(&d->reactive, false);
        dd.output = api_resource(&d->output, true);
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
        dd.viewSpaceToMetersFactor = 1.0f;
        if (view) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
        ffxReturnCode_t rc = pfn_dispatch(&api_ctx, &dd.header);
        transition_api_inputs(d->cb, d, false);
        if (rc != FFX_API_RETURN_OK) {
            static int logged;
            if (logged++ < 8) LOG("[fsr4] dispatch: error %u\n", (unsigned)rc);
            return false;
        }
        return true;
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
    if (have_api_ctx && pfn_query && api_ctx) {
        int32_t phases = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        q.renderWidth = render_w;
        q.displayWidth = output_w;
        q.pOutPhaseCount = &phases;
        if (pfn_query(&api_ctx, &q.header) == FFX_API_RETURN_OK && phases > 0) return phases;
    }
    return ffxFsr3UpscalerGetJitterPhaseCount((int32_t)render_w, (int32_t)output_w);
}

extern "C" void fsr3_jitter_offset(int i, int phases, float *x, float *y) {
    if (have_api_ctx && pfn_query && api_ctx) {
        ffxQueryDescUpscaleGetJitterOffset q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        q.index = i;
        q.phaseCount = phases;
        q.pOutX = x;
        q.pOutY = y;
        if (pfn_query(&api_ctx, &q.header) == FFX_API_RETURN_OK) return;
    }
    ffxFsr3UpscalerGetJitterOffset(x, y, i, phases);
}

#else
extern "C" bool fsr3_available(void) { return false; }
extern "C" bool fsr4_available(void) { return false; }
extern "C" const char *fsr_active_provider(void) { return ""; }
extern "C" bool fsr3_dispatch(const Fsr3Dispatch *) { return false; }
extern "C" int fsr3_jitter_phases(uint32_t, uint32_t) { return 8; }
extern "C" void fsr3_jitter_offset(int, int, float *x, float *y) { *x = *y = 0; }
extern "C" void fsr3_destroy(void) {}
#endif
