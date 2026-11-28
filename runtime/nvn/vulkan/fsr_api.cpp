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
#include <dlfcn.h>
#include "../../paths.h"
#define __declspec(x)  // (ffx_api.h marks its functions dllexport; only their types are used here)
// Linux: AMD ships no library, so this loads fsr4-vk's amd_fidelityfx_upscaler_vk.so, which exports the FSR API itself (no loader) and holds FSR 4 only.
// It differs from AMD's DLLs in what it takes: the device from the SDK's older backend description (make_context),
// VK_EXT_descriptor_buffer on the device (device.c),
// its inputs in SHADER_READ_ONLY_OPTIMAL as COMPUTE_READ, float colour, and an exposure image unless the context works out its own (fsr_api_dispatch)
#define FSR_SO "amd_fidelityfx_upscaler_vk.so"
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
#else
    // (SAVAGEWIND_FSR4_LIB, else next to the executable, else in the data folder: an AppImage's own folder is read-only)
    char path[1200] = "";
    const char *env = getenv("SAVAGEWIND_FSR4_LIB");
    if (env && *env) snprintf(path, sizeof path, "%s", env);
    else if (!rt_resource(FSR_SO, NULL, path, sizeof path)) snprintf(path, sizeof path, "%s/" FSR_SO, rt_data_root());
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        LOG("[fsr] %s did not load (%s)\n", path, dlerror());
        return false;
    }
    create_context = (PfnFfxCreateContext)dlsym(m, "ffxCreateContext");
    destroy_context = (PfnFfxDestroyContext)dlsym(m, "ffxDestroyContext");
    query = (PfnFfxQuery)dlsym(m, "ffxQuery");
    dispatch = (PfnFfxDispatch)dlsym(m, "ffxDispatch");
    if (!create_context || !destroy_context || !query || !dispatch) {
        LOG("[fsr] %s lacks the FSR API's functions\n", path);
        return false;
    }
    LOG("[fsr] loaded %s\n", path);
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

extern "C" bool vk_api12(void);  // (device.c)
extern "C" bool fsr_api_has(int major) { return major >= 1 && major <= 4 && ver_have[major] && !vk_api12(); }
extern "C" bool fsr4_available(void) { return fsr_api_has(4); }
extern "C" bool fsr3_api_wanted(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FSR3_API") && atoi(getenv("SAVAGEWIND_FSR3_API")) != 0;
    return on && fsr_api_has(3);
}

static ffxContext ctx;
static int ctx_major;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;

extern "C" void vk_wait_idle_for_contexts(void);  // (device.c)

#ifndef _WIN32
// Images of the context's own (fsr4-vk): the exposure, a 1x1 R32F of 1 (it takes no frame without one unless the context has auto exposure),
// and the scene as RGBA16F when the game's is a format fsr4-vk does not read (SDR: 10-bit)
struct OwnImage { VkImage image; VkDeviceMemory mem; };
static OwnImage own_exposure, own_colour;
static VkDevice ctx_dev;
static bool exposure_filled;
static VkFormat ctx_format;
static bool ctx_hdr;

static void own_free(VkDevice dev, OwnImage *o) {
    if (o->image) vkDestroyImage(dev, o->image, nullptr);
    if (o->mem) vkFreeMemory(dev, o->mem, nullptr);
    *o = {};
}

static bool own_make(const Fsr3Dispatch *d, VkFormat format, uint32_t w, uint32_t h, OwnImage *o) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(d->dev, &ci, nullptr, &o->image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d->dev, o->image, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & 1u << i) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            ai.memoryTypeIndex = i;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->dev, &ai, nullptr, &o->mem) != VK_SUCCESS ||
        vkBindImageMemory(d->dev, o->image, o->mem, 0) != VK_SUCCESS) {
        own_free(d->dev, o);
        return false;
    }
    return true;
}

static void layout(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

static bool reads_colour(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}
// HDR's scene is linear light past 1 in RGBA16F (docs/hdr.md): an HDR context with auto exposure, as an LDR one is told the colours stay within 0..1.
// SAVAGEWIND_FSR4_HDR=0: the LDR context with HDR too (testing)
static bool hdr_scene(VkFormat f) {
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_FSR4_HDR") || atoi(getenv("SAVAGEWIND_FSR4_HDR")) != 0;
    return on && f == VK_FORMAT_R16G16B16A16_SFLOAT;
}
#endif

extern "C" void fsr_api_destroy(void) {
    if (!ctx) return;
    vk_wait_idle_for_contexts();
    destroy_context(&ctx, nullptr);
    ctx = nullptr;
#ifndef _WIN32
    own_free(ctx_dev, &own_exposure);
    own_free(ctx_dev, &own_colour);
#endif
}

// (the DLL's own messages: a line is logged once in a row, and at most 24 in a run)
static void message(uint32_t type, const wchar_t *msg) {
    static char last[512];
    static int logged;
    char buf[512];
#ifdef _WIN32
    snprintf(buf, sizeof buf, "%ls", msg);
#else
    size_t n = 0;  // (%ls stops at the first character the C locale has no bytes for)
    for (; msg && msg[n] && n < sizeof buf - 1; n++) buf[n] = msg[n] < 0x80 ? (char)msg[n] : '?';
    buf[n] = 0;
#endif
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
#ifndef _WIN32
    bool hdr = hdr_scene(d->color.format);
    if (hdr) {
        cd.flags &= ~(uint32_t)FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        cd.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    }
#endif
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
#ifndef _WIN32
    ffxCreateBackendVKDesc_DEPRECATED old_bd = {};
    old_bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    old_bd.vkDevice = d->dev, old_bd.vkPhysicalDevice = d->phys, old_bd.vkDeviceProcAddr = device_proc;
    (major != 4 ? ov.header.pNext : vd.header.pNext) = &old_bd.header;
#endif
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
#ifndef _WIN32
    ctx_dev = d->dev;
    ctx_format = d->color.format, ctx_hdr = hdr, exposure_filled = false;
    if ((!hdr && !own_make(d, VK_FORMAT_R32_SFLOAT, 1, 1, &own_exposure)) ||
        (!reads_colour(d->color.format) &&
         !own_make(d, VK_FORMAT_R16G16B16A16_SFLOAT, d->color.width, d->color.height, &own_colour))) {
        LOG("[fsr] FSR %s context: no memory for its exposure or colour image\n", ver_name[major]);
        fsr_api_destroy();
        return false;
    }
#endif
    ctx_rw = d->color.width, ctx_rh = d->color.height, ctx_ow = d->output.width, ctx_oh = d->output.height;
    LOG("[fsr] AMD FSR API context %ux%u -> %ux%u (asked for %s, got %s)\n", ctx_rw, ctx_rh, ctx_ow, ctx_oh,
        major == 4 ? "the newest" : ver_name[major], got);
#ifndef _WIN32
    LOG("[fsr] fsr4-vk: %s colour%s\n", hdr ? "HDR, auto exposure," : "LDR, exposure 1,",
        own_colour.image ? " (copied to RGBA16F)" : "");
#endif
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
#ifdef _WIN32
    return ffxApiGetResourceVK((void *)m->image, rd, FFX_API_RESOURCE_STATE_COMMON);
#else
    return ffxApiGetResourceVK((void *)m->image, rd, uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                                                         : FFX_API_RESOURCE_STATE_COMPUTE_READ);
#endif
}

static bool failed[5];  // (a version that failed once is not tried again this run)
extern "C" bool fsr_api_usable(int major) { return fsr_api_has(major) && !failed[major]; }

extern "C" bool fsr_api_dispatch(const Fsr3Dispatch *d, int major) {
    if (!fsr_api_usable(major)) return false;
#ifndef _WIN32
    // (a 10-bit integer scene cannot be copied to float by a blit: the built-in FSR 3.1 takes it)
    if (!reads_colour(d->color.format) && d->color.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        LOG("[fsr] FSR %s: no path for the scene's format %d\n", ver_name[major], (int)d->color.format);
        failed[major] = true;
        return false;
    }
    if (ctx && d->color.format != ctx_format) fsr_api_destroy();  // (HDR turned on or off: another context)
#endif
    if ((!ctx || ctx_major != major || d->color.width != ctx_rw || d->color.height != ctx_rh ||
         d->output.width != ctx_ow || d->output.height != ctx_oh) && !make_context(d, major)) {
        failed[major] = true;
        return false;
    }
    ffxDispatchDescUpscale dd = {};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = (void *)d->cb;
    dd.color = resource(&d->color, false);
#ifndef _WIN32
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        T = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (own_colour.image) {
        Fsr3Image c = {own_colour.image, VK_FORMAT_R16G16B16A16_SFLOAT, d->color.width, d->color.height};
        VkImageBlit blit = {};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {(int32_t)c.width, (int32_t)c.height, 1};
        layout(d->cb, c.image, VK_IMAGE_LAYOUT_UNDEFINED, T);
        vkCmdBlitImage(d->cb, d->color.image, G, c.image, T, 1, &blit, VK_FILTER_NEAREST);
        layout(d->cb, c.image, T, R);
        dd.color = resource(&c, false);
    } else {
        layout(d->cb, d->color.image, G, R);
    }
    if (own_exposure.image) {
        if (!exposure_filled) {
            const VkClearColorValue one = {{1.0f, 0.0f, 0.0f, 0.0f}};
            layout(d->cb, own_exposure.image, VK_IMAGE_LAYOUT_UNDEFINED, T);
            vkCmdClearColorImage(d->cb, own_exposure.image, T, &one, 1, &whole);
            layout(d->cb, own_exposure.image, T, R);
            exposure_filled = true;
        }
        Fsr3Image x = {own_exposure.image, VK_FORMAT_R32_SFLOAT, 1, 1};
        dd.exposure = resource(&x, false);
    }
    layout(d->cb, d->depth.image, G, R);
    layout(d->cb, d->motion.image, G, R);
    layout(d->cb, d->reactive.image, G, R);
#endif
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
#ifndef _WIN32
    if (ctx_hdr) dd.flags &= ~(uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
#endif
    if (debug_view()) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    vk_gpuprof_mark(d->cb, major == 4 ? "AMD FSR 4 upscale" : "AMD FSR API upscale");  // (SAVAGEWIND_GPUPROF_AT)
    ffxReturnCode_t e = dispatch(&ctx, &dd.header);
#ifndef _WIN32
    if (!own_colour.image) layout(d->cb, d->color.image, R, G);
    layout(d->cb, d->depth.image, R, G);
    layout(d->cb, d->motion.image, R, G);
    layout(d->cb, d->reactive.image, R, G);
#endif
    vk_gpuprof_mark(d->cb, "after the AMD FSR API upscale");
    if (e != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 8) LOG("[fsr] FSR %s dispatch: error %u\n", ver_name[major], (unsigned)e);
        failed[major] = true;
        return false;
    }
    return true;
}
