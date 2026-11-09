// AMD's FSR API: FSR 4 and AMD's FSR 3.1 through the FSR SDK 2.4.0's signed DLLs (fsr_api.h)
#include "fsr_api.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#define __declspec(x)  // (ffx_api.h marks its functions dllexport; only their types are used here)
#endif
#include <FidelityFX/api/api/include/vk/ffx_api_vk.h>
#include <FidelityFX/api/upscalers/include/ffx_upscale.h>

static PfnFfxCreateContext create_context;
static PfnFfxDestroyContext destroy_context;
static PfnFfxQuery query;
static PfnFfxDispatch dispatch;

extern "C" bool fsr_api_present(void) {
    static int have = -1;
    if (have >= 0) return have;
    have = 0;
#ifdef _WIN32
    // (from the game's folder only, by its full path; the loader finds the upscaler DLL next to itself)
    wchar_t path[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy(slash + 1, L"amd_fidelityfx_loader_vk.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        LOG("[fsr] amd_fidelityfx_loader_vk.dll did not load (error %lu)\n", (unsigned long)GetLastError());
        return false;
    }
    create_context = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    destroy_context = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!create_context || !destroy_context || !query || !dispatch) {
        LOG("[fsr] amd_fidelityfx_loader_vk.dll lacks the FSR API's functions\n");
        return false;
    }
    have = 1;
#endif
    return have;
}

// The versions the DLL offers for this device: the highest of each major version (ids from the DLL's own query, as
// AMD asks: never hard-coded; the list has no promised order)
static uint64_t ver_id[5];
static uint32_t ver_key[5];  // (minor and patch number of the version kept, to keep the highest)
static char ver_name[5][32];
static std::atomic<bool> ver_have[5];
static bool amd_gpu;

// The device functions handed to the DLL: an extension's KHR name this device was not made with is the core
// function it was promoted to (as fsr3.cpp's backend wants)
static PFN_vkVoidFunction VKAPI_PTR device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(dev, core);
    }
    return f;
}
static VkDeviceContext dev_ctx;

extern "C" void fsr_api_probe(VkDevice dev, VkPhysicalDevice phys) {
    if (!fsr_api_present()) return;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    amd_gpu = p.vendorID == 0x1002;
    dev_ctx = {dev, phys, device_proc};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &dev_ctx;
    uint64_t count = 0;
    q.outputCount = &count;
    ffxReturnCode_t e = query(nullptr, &q.header);
    if (e != FFX_API_RETURN_OK || !count) {
        LOG("[fsr] AMD FSR API: no upscaler versions (error %u)\n", (unsigned)e);
        return;
    }
    if (count > 16) count = 16;
    uint64_t ids[16] = {};
    const char *names[16] = {};
    q.versionIds = ids;
    q.versionNames = names;
    e = query(nullptr, &q.header);
    if (e != FFX_API_RETURN_OK) {
        LOG("[fsr] AMD FSR API: version query failed (error %u)\n", (unsigned)e);
        return;
    }
    char list[256] = "";
    for (uint64_t i = 0; i < count; i++) {
        const char *s = names[i] ? names[i] : "?";
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", s);
        // ("4.1.2", or a name with the number after a word: the first number is the major version)
        const char *d = s;
        while (*d && (*d < '0' || *d > '9')) d++;
        unsigned maj = 0, min = 0, pat = 0;
        sscanf(d, "%u.%u.%u", &maj, &min, &pat);
        int major = (int)maj;
        uint32_t key = (min & 0xffff) << 16 | (pat & 0xffff);
        if (major < 1 || major > 4 || (ver_have[major] && key <= ver_key[major])) continue;
        ver_id[major] = ids[i];
        ver_key[major] = key;
        snprintf(ver_name[major], sizeof ver_name[major], "%s", s);  // (a copy: AMD says the names may be overwritten)
        ver_have[major] = true;
    }
    // FSR 4 runs on AMD Radeon graphics only
    if (ver_have[4] && !amd_gpu) ver_have[4] = false;
    LOG("[fsr] AMD FSR API: upscaler versions %s%s\n", list,
        ver_have[4] ? "" : amd_gpu ? " (no FSR 4 for this GPU)" : " (FSR 4: AMD Radeon only)");
}

extern "C" bool fsr_api_has(int major) { return major >= 1 && major <= 4 && ver_have[major]; }
extern "C" bool fsr4_available(void) { return fsr_api_has(4); }
extern "C" bool fsr3_api_wanted(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FSR3_API") && atoi(getenv("SAVAGEWIND_FSR3_API")) != 0;
    return on && fsr_api_has(3);
}

static ffxContext ctx;
static int ctx_major;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static VkDevice ctx_dev;

extern "C" void fsr_api_destroy(void) {
    if (!ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    destroy_context(&ctx, nullptr);
    ctx = nullptr;
}

// (the DLL's own messages: a line is logged once in a row, and at most 24 in a run)
static void message(uint32_t type, const wchar_t *msg) {
    static char last[512];
    static int logged;
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    if (!strcmp(buf, last) || logged >= 24) return;
    memcpy(last, buf, sizeof last);
    logged++;
    LOG("[fsr] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

extern "C" void vk_gpuprof_mark(VkCommandBuffer cb, const char *what);  // (exec.c)

// (testing switches, read once)
static bool env_on(const char *name) {
    const char *v = getenv(name);
    return v && atoi(v) != 0;
}
static bool linear_colour(void) { static int on = -1; if (on < 0) on = env_on("SAVAGEWIND_FSR4_LINEAR"); return on; }
static bool debug_view(void) { static int on = -1; if (on < 0) on = env_on("SAVAGEWIND_FSR3_VIEW"); return on; }

static bool make_context(const Fsr3Dispatch *d, int major) {
    fsr_api_destroy();
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // (LDR input, low-resolution and jitter-free motion vectors, standard depth, as fsr3.cpp. FSR 4 is told the
    // colours are perceptual, sRGB-like: the scene is the game's finished, graded image; SAVAGEWIND_FSR4_LINEAR=1 does
    // not. SAVAGEWIND_FSR3_DEBUG=1: the DLL's checks of the inputs in the log; SAVAGEWIND_FSR3_VIEW=1: its debug view)
    cd.flags = env_on("SAVAGEWIND_FSR3_DEBUG") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0;
    if (debug_view()) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    if (major == 4 && !linear_colour()) cd.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    cd.maxRenderSize = {d->color.width, d->color.height};
    cd.maxUpscaleSize = {d->output.width, d->output.height};
    cd.fpMessage = message;
    dev_ctx = {d->dev, d->phys, device_proc};
    ffxCreateBackendVKDesc bd = {};
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    bd.deviceContext = dev_ctx;
    ffxCreateContextDescUpscaleVersion vd = {};
    vd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    vd.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ov = {};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = ver_id[major];
    cd.header.pNext = &bd.header;
    bd.header.pNext = &vd.header;
    // (FSR 3: the 3.1.6 asked for by id. FSR 4: no version asked for, so the DLL picks its newest, which may be a
    // driver's own newer build; AMD's docs: an override turns that choice off)
    if (major != 4) vd.header.pNext = &ov.header;
    ffxReturnCode_t e = create_context(&ctx, &cd.header, nullptr);
    if (e != FFX_API_RETURN_OK) {
        LOG("[fsr] FSR %s context %ux%u -> %ux%u: error %u\n", ver_name[major], d->color.width, d->color.height,
            d->output.width, d->output.height, (unsigned)e);
        ctx = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const char *got = query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName ? pv.versionName : "?";
    ctx_major = major;
    ctx_dev = d->dev;
    ctx_rw = d->color.width, ctx_rh = d->color.height, ctx_ow = d->output.width, ctx_oh = d->output.height;
    LOG("[fsr] AMD FSR API context %ux%u -> %ux%u (asked for %s, got %s)\n", ctx_rw, ctx_rh, ctx_ow, ctx_oh,
        major == 4 ? "the newest" : ver_name[major], got);
    return true;
}

// SWR's images are all left in VK_IMAGE_LAYOUT_GENERAL: the COMMON state, which the DLL transitions from and back to
static FfxApiResource resource(const Fsr3Image *m, bool uav) {
    FfxApiResourceDescription rd = {};
    rd.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxApiGetSurfaceFormatVK(m->format);
    rd.width = m->width, rd.height = m->height, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_API_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return ffxApiGetResourceVK((void *)m->image, rd, FFX_API_RESOURCE_STATE_COMMON);
}

static bool failed[5];  // (a version that failed once is not tried again this run)
extern "C" bool fsr_api_usable(int major) { return fsr_api_has(major) && !failed[major]; }

extern "C" bool fsr_api_dispatch(const Fsr3Dispatch *d, int major) {
    if (!fsr_api_usable(major)) return false;
    if ((!ctx || ctx_major != major || d->color.width != ctx_rw || d->color.height != ctx_rh ||
         d->output.width != ctx_ow || d->output.height != ctx_oh) && !make_context(d, major)) {
        failed[major] = true;
        return false;
    }
    ffxDispatchDescUpscale dd = {};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = (void *)d->cb;
    dd.color = resource(&d->color, false);
    dd.depth = resource(&d->depth, false);
    dd.motionVectors = resource(&d->motion, false);
    dd.reactive = resource(&d->reactive, false);
    dd.output = resource(&d->output, true);
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
    if (major == 4 && !linear_colour()) dd.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (debug_view()) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    vk_gpuprof_mark(d->cb, major == 4 ? "AMD FSR 4 upscale" : "AMD FSR API upscale");  // (SAVAGEWIND_GPUPROF_AT)
    ffxReturnCode_t e = dispatch(&ctx, &dd.header);
    vk_gpuprof_mark(d->cb, "after the AMD FSR API upscale");
    if (e != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 8) LOG("[fsr] FSR %s dispatch: error %u\n", ver_name[major], (unsigned)e);
        failed[major] = true;
        return false;
    }
    return true;
}
