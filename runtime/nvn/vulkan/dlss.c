// NVIDIA DLSS Super Resolution (Video > Upscaler) and Frame Generation (Video > Frame generation) through NGX, loaded
// at run time. No NVIDIA SDK code, header or library is compiled in or linked: the NGX core is the NVIDIA driver's
// own _nvngx.dll (found through HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore), and the few NGX types, values and
// parameter names used below are declared here from the SDK's documented interface. The DLSS libraries the core loads
// for each feature, nvngx_dlss.dll and nvngx_dlssg.dll, are NVIDIA's own copies, which the setup downloads next to
// savagewind.exe from NVIDIA's repository (dlss_fetch.c); without them, or without an NVIDIA RTX driver, NGX is not
// loaded at all and both DLSS choices stay greyed out.
//
// NGX's parameter object is a C++ class whose virtual functions are called through its vtable: the Set overloads in
// slots 0-7 and the Get overloads in 8-15, each group in reverse declaration order (MSVC's layout, the Windows core's).
// vtable_ok checks that layout with a set-then-get round trip before anything relies on it.
#include "dlss.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../paths.h"

#ifdef _WIN32

#include <windows.h>

// ================================================================== NGX's interface (the SDK's documented ABI)
typedef unsigned NgxResult;  // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
enum { NGX_FEATURE_SUPERSAMPLING = 1, NGX_FEATURE_FRAMEGENERATION = 11 };
enum { NGX_PERF_MAXPERF = 0, NGX_PERF_BALANCED = 1, NGX_PERF_MAXQUALITY = 2, NGX_PERF_ULTRAPERF = 3, NGX_PERF_ULTRAQUALITY = 4 };
enum { NGX_DLSS_IS_HDR = 1 << 0, NGX_DLSS_MV_LOWRES = 1 << 1 };
// The application id NVIDIA's own DLSS samples use for applications without an NVIDIA-issued id
#define NGX_APP_ID 231313132ull

typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {  // NVSDK_NGX_Resource_VK, an image view
    union {
        struct {
            VkImageView view;
            VkImage image;
            VkImageSubresourceRange range;
            VkFormat format;
            unsigned width, height;
        } img;
        struct { VkBuffer buffer; unsigned bytes; } buf;
    } r;
    int type;  // 0 image view, 1 buffer
    bool rw;
} NgxResourceVK;
typedef struct {  // NVSDK_NGX_FeatureCommonInfo
    struct { const wchar_t *const *path; unsigned length; } paths;
    void *internal;
    struct { void *callback; int min_level; bool disable_other_sinks; } logging;
} NgxCommonInfo;
typedef struct {  // NVSDK_NGX_FeatureDiscoveryInfo
    int sdk_version;
    int feature;
    struct {
        int type;  // 0 application id, 1 project id
        union { struct { const char *id; int engine; const char *engine_version; } project; unsigned long long app_id; } v;
    } identifier;
    const wchar_t *data_path;
    const NgxCommonInfo *common;
} NgxDiscoveryInfo;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const NgxParam *);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_FeatDevExt)(VkInstance, VkPhysicalDevice, const NgxDiscoveryInfo *, uint32_t *,
                                    VkExtensionProperties **);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);
// (the core's Shutdown1 writes a count through its second argument without a check: the SDK's library passes one)
typedef NgxResult (*PFN_Shutdown)(VkDevice, unsigned *);

static struct {
    HMODULE core;
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_FeatDevExt feat_dev_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    PFN_Shutdown shutdown;
    int tried, loaded;
} N;

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
// The SDK declares Set(u64), Set(float), Set(double), Set(u32), Set(int), Set(ID3D11Resource *), Set(ID3D12Resource *),
// Set(void *), then the Get overloads in the same order, then Reset; MSVC reverses each group of overloads.
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static bool p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static bool p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static bool p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static bool p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

static const char *result_name(NgxResult r) {
    switch (r) {
    case 0x1: return "success";
    case 0xBAD00001: return "feature-not-supported";
    case 0xBAD00002: return "platform-error";
    case 0xBAD00004: return "feature-not-found";
    case 0xBAD00005: return "invalid-parameter";
    case 0xBAD00007: return "not-initialized";
    case 0xBAD00008: return "unsupported-input-format";
    case 0xBAD0000A: return "missing-input";
    case 0xBAD0000B: return "unable-to-initialize-feature";
    case 0xBAD0000C: return "out-of-date";
    default: return "failure";
    }
}

static bool ok(NgxResult r, const char *what) {
    if (NGX_OK(r)) return true;
    LOG("[dlss] %s: %s (%#x)\n", what, result_name(r), r);
    return false;
}

// ================================================================== loading NGX
static wchar_t g_exe_dir[MAX_PATH];  // savagewind.exe's folder, where the DLSS libraries are (no trailing slash)
static wchar_t g_data_dir[1024];     // NGX's logs and caches: the data folder

// What NGX would load next to savagewind.exe (bit 0: nvngx_dlss.dll, Super Resolution; bit 1: nvngx_dlssg.dll,
// Frame Generation). NGX is loaded only when one is there.
static int dlls_here(void) {
    static int have = -1;
    if (have >= 0) return have;
    have = 0;
    DWORD len = GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
    if (!len || len >= MAX_PATH) return have;
    while (len && g_exe_dir[len - 1] != L'\\' && g_exe_dir[len - 1] != L'/') len--;
    g_exe_dir[len ? len - 1 : 0] = 0;
    static const wchar_t *const names[2] = {L"nvngx_dlss.dll", L"nvngx_dlssg.dll"};
    for (int i = 0; i < 2; i++) {
        wchar_t p[MAX_PATH + 32];
        _snwprintf(p, MAX_PATH + 32, L"%ls\\%ls", g_exe_dir, names[i]);
        p[MAX_PATH + 31] = 0;
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) have |= 1 << i;
    }
    return have;
}

static bool core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
    if (!dlls_here()) {
        LOG("[dlss] NVIDIA's DLSS libraries are not next to savagewind.exe (the setup downloads them): DLSS off\n");
        return false;
    }
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS) {
        LOG("[dlss] no NVIDIA driver with NGX (registry NGXCore missing): DLSS off\n");
        return false;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        LOG("[dlss] cannot load %s (error %lu): DLSS off\n", path, GetLastError());
        return false;
    }
#define FN(f, name) N.f = (void *)GetProcAddress(N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(feat_dev_ext, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
    FN(shutdown, "NVSDK_NGX_VULKAN_Shutdown1");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.destroy_params || !N.create || !N.evaluate ||
        !N.release) {
        LOG("[dlss] the NGX core %s lacks Vulkan entry points: DLSS off\n", path);
        return false;
    }
    const char *data = rt_data_root();
    if (!data || !MultiByteToWideChar(CP_UTF8, 0, data, -1, g_data_dir, (int)(sizeof g_data_dir / sizeof *g_data_dir))) {
        LOG("[dlss] invalid application data path: DLSS off\n");
        return false;
    }
    N.loaded = 1;
    LOG("[dlss] NGX core %s\n", path);
    return true;
}

// ================================================================== extensions (before the instance and the device)
static const char **g_instance_exts, **g_device_exts;
static unsigned g_instance_ext_count, g_device_ext_count;

static void query_extensions(void) {
    static bool done;
    if (done) return;
    done = true;
    if (!core_load()) return;
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    NgxResult r = N.req_ext(&ni, &ie, &nd, &de);
    if (!ok(r, "required extension query")) return;
    g_instance_exts = ie, g_instance_ext_count = ni;
    g_device_exts = de, g_device_ext_count = nd;
    LOG("[dlss] required extensions: %u instance, %u device\n", ni, nd);
}

void vk_dlss_required_instance_extensions(const char ***names, unsigned *count) {
    query_extensions();
    if (names) *names = g_instance_exts;
    if (count) *count = g_instance_ext_count;
}

// NGX's general list, and what frame generation asks for on this GPU besides (VK_KHR_buffer_device_address, where the
// general list has the EXT one: the two can't both be on, and the KHR one is core in Vulkan 1.2, so it replaces it;
// device.c then turns on bufferDeviceAddress). Before the device is made: g_vk.inst and g_vk.phys.
void vk_dlss_required_device_extensions(const char ***names, unsigned *count) {
    query_extensions();
    static const char *merged[32];
    static char fg_names[16][VK_MAX_EXTENSION_NAME_SIZE];
    static unsigned n;
    static bool done;
    if (!done) {
        done = true;
        for (unsigned i = 0; i < g_device_ext_count && n < 32; i++) merged[n++] = g_device_exts[i];
        uint32_t nf = 0;
        VkExtensionProperties *ep = NULL;
        NgxCommonInfo common = {{NULL, 0}, NULL, {NULL, 0, false}};
        const wchar_t *paths[1] = {g_exe_dir};
        common.paths.path = paths, common.paths.length = 1;
        NgxDiscoveryInfo di = {NGX_VERSION_API, NGX_FEATURE_FRAMEGENERATION, {0, {.app_id = NGX_APP_ID}}, g_data_dir,
                               &common};
        NgxResult r = (dlls_here() & 2) && N.loaded && N.feat_dev_ext && g_vk.inst && g_vk.phys
                           ? N.feat_dev_ext(g_vk.inst, g_vk.phys, &di, &nf, &ep)
                           : 0xBAD00000u;
        if (!NGX_OK(r)) nf = 0;
        // (the driver's core may refuse this query from outside NVIDIA's SDK, as the 617 driver does; what the SDK got
        // from it, 310.9.16 on an RTX 50, is the one below, which Vulkan 1.2 makes core and which that core's general
        // list already has: asked for in its place when the query gives nothing)
        static VkExtensionProperties known = {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, 1};
        if (!nf && (dlls_here() & 2) && N.loaded) ep = &known, nf = 1;
        for (uint32_t i = 0; i < nf && i < 16 && n < 32; i++) {
            const char *e = ep[i].extensionName;
            bool have = false;
            for (unsigned j = 0; j < n; j++) have |= !strcmp(merged[j], e);
            if (have) continue;
            snprintf(fg_names[i], sizeof fg_names[i], "%s", e);
            if (!strcmp(e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
                unsigned k = 0;
                for (unsigned j = 0; j < n; j++)
                    if (strcmp(merged[j], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) merged[k++] = merged[j];
                n = k;
            }
            merged[n++] = fg_names[i];
            LOG("[dlss] frame generation also needs %s\n", e);
        }
    }
    if (names) *names = merged;
    if (count) *count = n;
}

// ================================================================== init
static bool g_initialized, g_ngx_up;
static NgxParam *g_capability_params;  // (NGX's: what this driver and GPU run)
static bool g_available;               // DLSS Super Resolution runs here (RTX)
static bool g_fg_available;            // DLSS Frame Generation runs here (RTX 40 and later)
static int g_fg_max_mult = 2;          // frames on screen for each drawn it makes here (MultiFrameCountMax + 1)

static bool vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return false;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "SavageWind.Test.I", -12345);
    p_setu(p, "SavageWind.Test.U", 4000000000u);
    p_setf(p, "SavageWind.Test.F", 1.25f);
    p_setp(p, "SavageWind.Test.P", (void *)p);
    bool good = p_geti(p, "SavageWind.Test.I", &i) && i == -12345 && p_getu(p, "SavageWind.Test.U", &u) &&
                u == 4000000000u && p_getf(p, "SavageWind.Test.F", &f) && f == 1.25f &&
                p_getp(p, "SavageWind.Test.P", &v) && v == (void *)p;
    N.destroy_params(p);
    if (!good) LOG("[dlss] NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLSS off\n", i, u, f, v);
    return good;
}

bool vk_dlss_init(void) {
    if (g_initialized) return g_ngx_up;
    g_initialized = true;
    if (!g_vk.phys || !g_vk.dev || !core_load()) return false;
    if (!ok(N.init(NGX_APP_ID, g_data_dir, g_vk.inst, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                   NGX_VERSION_API, NULL),
            "init"))
        return false;
    if (!vtable_ok()) return false;
    g_ngx_up = true;
    if (!ok(N.caps(&g_capability_params), "capabilities") || !g_capability_params) return true;
    int sr = 0, fg = 0, sr_driver = 0, fg_driver = 0;
    g_available = (dlls_here() & 1) && p_geti(g_capability_params, "SuperSampling.Available", &sr) && sr != 0;
    g_fg_available = (dlls_here() & 2) && p_geti(g_capability_params, "FrameGeneration.Available", &fg) && fg == 1;
    p_geti(g_capability_params, "SuperSampling.NeedsUpdatedDriver", &sr_driver);
    p_geti(g_capability_params, "FrameGeneration.NeedsUpdatedDriver", &fg_driver);
    unsigned mfc = 0;
    if (g_fg_available && p_getu(g_capability_params, "DLSSG.MultiFrameCountMax", &mfc) && mfc > 1)
        g_fg_max_mult = mfc + 1 > 4 ? 4 : (int)mfc + 1;  // (Video > Frame generation multiplier: up to 4x)
    LOG("[dlss] Super Resolution %s on %s%s\n", g_available ? "available" : "unavailable", g_vk.props.deviceName,
        sr_driver ? " (the driver needs an update)" : "");
    LOG("[dlss] Frame Generation %s on %s (up to %dx)%s\n", g_fg_available ? "available" : "unavailable",
        g_vk.props.deviceName, g_fg_max_mult, fg_driver ? " (the driver needs an update)" : "");
    return true;
}

static void release_feature(void);

void vk_dlss_shutdown(void) {
    if (!g_initialized) return;
    vk_dlssg_release();
    release_feature();
    unsigned left = 0;
    if (g_ngx_up && N.shutdown) N.shutdown(g_vk.dev, &left);
    g_ngx_up = g_fg_available = g_available = false;
    g_capability_params = NULL;
    g_initialized = false;
}

static NgxResourceVK image_resource(TexBackend *b, VkFormat f, bool rw) {
    static const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageAspectFlags a = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img.view = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, a);
    r.r.img.image = b->image;
    r.r.img.range = (VkImageSubresourceRange){a, 0, 1, 0, 1};
    r.r.img.format = f;
    r.r.img.width = b->width, r.r.img.height = b->height;
    r.type = 0, r.rw = rw;
    return r;
}

// ================================================================== DLSS Super Resolution (Video > Upscaler)
static NgxParam *g_params;
static NgxHandle *g_feature;
static unsigned g_in_w, g_in_h, g_out_w, g_out_h, g_quality;
static VkFormat g_color_format, g_output_format;

static void release_feature(void) {
    if (g_feature) N.release(g_feature);
    if (g_params) N.destroy_params(g_params);
    g_feature = NULL, g_params = NULL;
    g_in_w = g_in_h = g_out_w = g_out_h = g_quality = 0;
    g_color_format = g_output_format = VK_FORMAT_UNDEFINED;
}

bool vk_dlss_available(void) { return g_available; }

bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion,
                   TexBackend *output, unsigned quality, float jitter_x, float jitter_y, float mv_scale_x,
                   float mv_scale_y, bool reset) {
    if (!g_available || !cb || !color || !depth || !motion || !output ||
        !color->image || !depth->image || !motion->image || !output->image ||
        color->width == 0 || color->height == 0 || output->width == 0 || output->height == 0)
        return false;

    if (!g_feature || g_in_w != color->width || g_in_h != color->height || g_out_w != output->width ||
        g_out_h != output->height || g_quality != quality || g_color_format != color->format ||
        g_output_format != output->format) {
        release_feature();
        static const int q[] = {NGX_PERF_ULTRAQUALITY, NGX_PERF_MAXQUALITY, NGX_PERF_BALANCED, NGX_PERF_MAXPERF,
                                NGX_PERF_ULTRAPERF};  // (UPQ_*: Ultra Performance's 3.0 is DLSS's own)
        unsigned requested = quality < 5 ? quality : 1;
        // Some driver/NGX combinations reject Ultra Quality at a particular
        // render size. Retry the next conservative mode before disabling DLSS.
        static const unsigned fallback[] = {0, 1, 2, 3};
        bool created = false;
        for (unsigned attempt = 0; attempt < 4 && !created; attempt++) {
            unsigned mode = attempt == 0 ? requested : fallback[attempt];
            if (!ok(N.alloc_params(&g_params), "allocate parameters")) return false;
            NgxParam *p = g_params;
            p_setu(p, "CreationNodeMask", 1);
            p_setu(p, "VisibilityNodeMask", 1);
            p_setu(p, "Width", color->width);
            p_setu(p, "Height", color->height);
            p_setu(p, "OutWidth", output->width);
            p_setu(p, "OutHeight", output->height);
            p_seti(p, "PerfQualityValue", q[mode]);
            p_seti(p, "DLSS.Feature.Create.Flags", NGX_DLSS_IS_HDR | NGX_DLSS_MV_LOWRES);
            p_seti(p, "DLSS.Enable.Output.Subrects", 0);
            NgxResult r = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &g_feature);
            if (NGX_OK(r)) {
                created = true;
                if (attempt) LOG("[dlss] quality %u rejected; using fallback mode %u\n", requested, mode);
            } else {
                LOG("[dlss] quality mode %u rejected: %s (%#x)\n", mode, result_name(r), r);
                release_feature();
            }
        }
        if (!created) {
            g_available = false;
            return false;
        }
        g_in_w = color->width; g_in_h = color->height; g_out_w = output->width; g_out_h = output->height;
        g_quality = quality; g_color_format = color->format; g_output_format = output->format;
    }

    NgxResourceVK in_color = image_resource(color, color->format, false);
    NgxResourceVK in_depth = image_resource(depth, depth->format, false);
    NgxResourceVK in_motion = image_resource(motion, motion->format, false);
    NgxResourceVK out = image_resource(output, output->format, true);
    NgxParam *p = g_params;
    p_setp(p, "Color", &in_color);
    p_setp(p, "Output", &out);
    p_setp(p, "Depth", &in_depth);
    p_setp(p, "MotionVectors", &in_motion);
    p_setf(p, "Jitter.Offset.X", jitter_x);
    p_setf(p, "Jitter.Offset.Y", jitter_y);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", reset ? 1 : 0);
    p_setf(p, "MV.Scale.X", mv_scale_x == 0.0f ? 1.0f : mv_scale_x);  // (NGX takes them in render pixels)
    p_setf(p, "MV.Scale.Y", mv_scale_y == 0.0f ? 1.0f : mv_scale_y);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", color->width);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", color->height);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    if (!ok(N.evaluate(cb, g_feature, p, NULL), "evaluate")) {
        release_feature();
        return false;
    }
    return true;
}

// ================================================================== DLSS Frame Generation (Video > Frame generation)
// NGX's own frame generation, without Streamline: the frames between the previous call's and this one's (2x: one;
// multi frame generation 3x, 4x: two, three), recorded into the frame's command buffer as AMD FSR 3's is (exec.c
// vk_fg_generate gives both the same inputs; device.c presents and paces them). One feature, made again when a size
// or the final image's format changes.
void vk_fg_wait_idle(void);  // (device.c)
static NgxHandle *g_fg;
static NgxParam *g_fg_params;  // (its own map: NGX's maps keep what was set)
static uint32_t g_fg_w, g_fg_h, g_fg_rw, g_fg_rh;
static VkFormat g_fg_format;

bool vk_dlssg_available(void) { return g_fg_available; }
int vk_dlssg_max_mult(void) { return g_fg_available ? g_fg_max_mult : 2; }

void vk_dlssg_release(void) {
    if (g_fg) N.release(g_fg);
    if (g_fg_params) N.destroy_params(g_fg_params);
    g_fg = NULL, g_fg_params = NULL;
    g_fg_w = g_fg_h = g_fg_rw = g_fg_rh = 0;
    g_fg_format = VK_FORMAT_UNDEFINED;
}

bool vk_dlssg_live(void) { return g_fg != NULL; }

// 4x4 matrices, row-major (m[r * 4 + c]), column vectors (exec.c's): o = a b; inverse (false: singular)
static void mul4(const double *a, const double *b, double *o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double t = 0;
            for (int k = 0; k < 4; k++) t += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = t;
        }
}

static bool inv4(const double *m, double *o) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : c - 4 == r;
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double t = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= t * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][c + 4];
    return true;
}

// NGX's camera matrix (float[4][4], row-major, a row vector times it: D3D's way, as Streamline's) of m (column
// vectors). SAVAGEWIND_DLSSG_MTX=col hands them over as they are (testing the convention).
static void to_ngx(const double *m, float o[4][4]) {
    static int col = -1;
    if (col < 0) col = getenv("SAVAGEWIND_DLSSG_MTX") && !strcmp(getenv("SAVAGEWIND_DLSSG_MTX"), "col");
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r][c] = (float)(col ? m[r * 4 + c] : m[c * 4 + r]);
}

// The parameters NGX's frame generation reads at each evaluation that Savage Wind leaves at zero: set each time (the
// map keeps what was set before)
static const char *const k_fg_zero_u[] = {
    "DLSSG.ColorBuffersHDR", "DLSSG.DepthInverted", "DLSSG.AutomodeOverrideReset", "DLSSG.NotRenderingGameFrames",
    "DLSSG.OrthoProjection", "DLSSG.MvecDilated", "DLSSG.MenuDetectionEnabled",
    "DLSSG.MVecsSubrectBaseX", "DLSSG.MVecsSubrectBaseY", "DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight",
    "DLSSG.DepthSubrectBaseX", "DLSSG.DepthSubrectBaseY", "DLSSG.DepthSubrectWidth", "DLSSG.DepthSubrectHeight",
    "DLSSG.HUDLessSubrectBaseX", "DLSSG.HUDLessSubrectBaseY", "DLSSG.HUDLessSubrectWidth", "DLSSG.HUDLessSubrectHeight",
    "DLSSG.UISubrectBaseX", "DLSSG.UISubrectBaseY", "DLSSG.UISubrectWidth", "DLSSG.UISubrectHeight",
    "DLSSG.UIAlphaSubrectBaseX", "DLSSG.UIAlphaSubrectBaseY", "DLSSG.UIAlphaSubrectWidth", "DLSSG.UIAlphaSubrectHeight",
    "DLSSG.BidirectionalDistortionFieldSubrectBaseX", "DLSSG.BidirectionalDistortionFieldSubrectBaseY",
    "DLSSG.BidirectionalDistortionFieldSubrectWidth", "DLSSG.BidirectionalDistortionFieldSubrectHeight",
    "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision",
    "DLSSG.InputBackbufferSubrectBaseX", "DLSSG.InputBackbufferSubrectBaseY", "DLSSG.InputBackbufferSubrectWidth",
    "DLSSG.InputBackbufferSubrectHeight",
    "DLSSG.OutputInterpolatedSubrectBaseX", "DLSSG.OutputInterpolatedSubrectBaseY",
    "DLSSG.OutputInterpolatedSubrectWidth", "DLSSG.OutputInterpolatedSubrectHeight",
    "DLSSG.OutputRealSubrectBaseX", "DLSSG.OutputRealSubrectBaseY", "DLSSG.OutputRealSubrectWidth",
    "DLSSG.OutputRealSubrectHeight"};
static const char *const k_fg_zero_f[] = {
    "DLSSG.JitterOffsetX", "DLSSG.JitterOffsetY", "DLSSG.CameraPinholeOffsetX", "DLSSG.CameraPinholeOffsetY",
    "DLSSG.BidirectionalDistortionFieldLowPrecision.Bias", "DLSSG.BidirectionalDistortionFieldLowPrecision.Scale",
    "DLSSG.MinRelativeLinearDepthObjectSeparation"};
static const char *const k_fg_null[] = {"DLSSG.UI", "DLSSG.UIAlpha", "DLSSG.BidirectionalDistortionField",
                                        "DLSSG.OutputReal", "DLSSG.OutputDisableInterpolation"};

bool vk_dlssg_dispatch(VkCommandBuffer cb, const DlssgFrame *f) {
    if (!g_fg_available || !cb) return false;
    uint32_t w = f->color->width, h = f->color->height, rw = f->depth->width, rh = f->depth->height;
    if (!g_fg || g_fg_w != w || g_fg_h != h || g_fg_rw != rw || g_fg_rh != rh || g_fg_format != f->view_format) {
        if (g_fg) vk_fg_wait_idle();  // (frames in flight still use it)
        vk_dlssg_release();
        if (!ok(N.alloc_params(&g_fg_params), "frame generation: parameters")) return false;
        NgxParam *p = g_fg_params;
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", w);
        p_setu(p, "Height", h);
        p_setu(p, "DLSSG.BackbufferFormat", (unsigned)f->view_format);
        p_setu(p, "DLSSG.Width", w);  // (the generic width / height alone are deprecated for it)
        p_setu(p, "DLSSG.Height", h);
        NgxResult r = N.create(cb, NGX_FEATURE_FRAMEGENERATION, p, &g_fg);
        LOG("[dlss] frame generation %ux%u (depth and motion %ux%u), format %d: %s\n", w, h, rw, rh, f->view_format,
            result_name(r));
        if (!NGX_OK(r)) {
            g_fg = NULL;
            vk_dlssg_release();
            g_fg_available = false;  // (FSR 3 from then on: device.c fg_wanted)
            return false;
        }
        g_fg_w = w, g_fg_h = h, g_fg_rw = rw, g_fg_rh = rh, g_fg_format = f->view_format;
    }

    // exec.c's clip space is OpenGL's (y up, z -1..1), its depth image's rows go ysign's way and its depth values are
    // (z / w + 1) / 2 (gl_depth) or z / w. NGX's (D3D's): uv.y = (1 - y) / 2 and depth = z / w. s takes the first
    // to the second, so that a depth sample and its pixel give NGX the point exec.c's matrices put there.
    const double g = f->gl_depth ? 0.5 : 1;
    const double s[16] = {1, 0, 0, 0, 0, -f->ysign, 0, 0, 0, 0, g, f->gl_depth ? 0.5 : 0, 0, 0, 0, 1};
    double si[16], vpi[16], pvpi[16], pi[16], t[16], t2[16];
    if (!inv4(s, si) || !inv4(f->vp, vpi) || !inv4(f->prev_vp, pvpi) || !inv4(f->proj, pi)) return false;
    // (read by NGX at the evaluation, through the pointers set below)
    static float clip_to_prev[4][4], prev_to_clip[4][4], view_to_clip[4][4], clip_to_view[4][4], to_lens[4][4];
    mul4(s, f->prev_vp, t), mul4(t, vpi, t2), mul4(t2, si, t), to_ngx(t, clip_to_prev);
    mul4(s, f->vp, t), mul4(t, pvpi, t2), mul4(t2, si, t), to_ngx(t, prev_to_clip);
    mul4(s, f->proj, t), to_ngx(t, view_to_clip);
    mul4(pi, si, t), to_ngx(t, clip_to_view);
    static const double id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_ngx(id, to_lens);
    // (exec.c's motion vectors: uv in the previous frame minus uv in this one; NGX takes pixels of their image)
    float msx_px = (float)rw, msy_px = (float)rh;
    static float msx = 0, msy = 0;  // (testing) SAVAGEWIND_DLSSG_MVSIGN=<x>,<y>: the vectors' signs
    if (!msx) {
        msx = msy = 1;
        if (getenv("SAVAGEWIND_DLSSG_MVSIGN")) sscanf(getenv("SAVAGEWIND_DLSSG_MVSIGN"), "%f,%f", &msx, &msy);
    }
    bool camera_motion = true;
    static int mtx_test = -1;  // (testing the matrices) SAVAGEWIND_DLSSG_MV_TEST=1: no motion vectors, camera motion
    if (mtx_test < 0) mtx_test = getenv("SAVAGEWIND_DLSSG_MV_TEST") != NULL;  // from depth and clipToPrevClip alone
    if (mtx_test) {
        VkClearColorValue zero;
        memset(&zero, 0, sizeof zero);
        VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, f->motion->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &cr);
        vk_full_barrier(cb);
        camera_motion = false;
    }
    bool reset = f->reset;
    // Multi frame generation: one evaluation per frame, multiFrameIndex 1 .. count (the later ones reuse the first's
    // work, NVIDIA's guide). A change of count starts over.
    static int last_count;
    if (f->count != last_count) reset = true;
    last_count = f->count;

    NgxResourceVK color = image_resource(f->color, f->view_format, false);
    NgxResourceVK hudless = image_resource(f->hudless, f->view_format, false);
    NgxResourceVK depth = image_resource(f->depth, f->depth->format, false);
    NgxResourceVK motion = image_resource(f->motion, f->motion->format, false);
    NgxParam *p = g_fg_params;
    p_setp(p, "DLSSG.Backbuffer", &color);
    p_setp(p, "DLSSG.MVecs", &motion);
    p_setp(p, "DLSSG.Depth", &depth);
    p_setp(p, "DLSSG.HUDLess", &hudless);
    for (size_t i = 0; i < sizeof k_fg_null / sizeof *k_fg_null; i++) p_setp(p, k_fg_null[i], NULL);
    for (size_t i = 0; i < sizeof k_fg_zero_u / sizeof *k_fg_zero_u; i++) p_setu(p, k_fg_zero_u[i], 0);
    for (size_t i = 0; i < sizeof k_fg_zero_f / sizeof *k_fg_zero_f; i++) p_setf(p, k_fg_zero_f[i], 0);
    p_setp(p, "DLSSG.CameraViewToClip", view_to_clip);
    p_setp(p, "DLSSG.ClipToCameraView", clip_to_view);
    p_setp(p, "DLSSG.ClipToLensClip", to_lens);
    p_setp(p, "DLSSG.ClipToPrevClip", clip_to_prev);
    p_setp(p, "DLSSG.PrevClipToClip", prev_to_clip);
    p_setf(p, "DLSSG.MvecScaleX", msx_px * msx);
    p_setf(p, "DLSSG.MvecScaleY", msy_px * msy);
    p_setf(p, "DLSSG.CameraPosX", f->cam_pos[0]), p_setf(p, "DLSSG.CameraPosY", f->cam_pos[1]);
    p_setf(p, "DLSSG.CameraPosZ", f->cam_pos[2]);
    p_setf(p, "DLSSG.CameraUpX", f->cam_up[0]), p_setf(p, "DLSSG.CameraUpY", f->cam_up[1]);
    p_setf(p, "DLSSG.CameraUpZ", f->cam_up[2]);
    p_setf(p, "DLSSG.CameraRightX", f->cam_right[0]), p_setf(p, "DLSSG.CameraRightY", f->cam_right[1]);
    p_setf(p, "DLSSG.CameraRightZ", f->cam_right[2]);
    p_setf(p, "DLSSG.CameraFwdX", f->cam_fwd[0]), p_setf(p, "DLSSG.CameraFwdY", f->cam_fwd[1]);
    p_setf(p, "DLSSG.CameraFwdZ", f->cam_fwd[2]);
    p_setf(p, "DLSSG.CameraNear", f->near_), p_setf(p, "DLSSG.CameraFar", f->far_);
    p_setf(p, "DLSSG.CameraFOV", f->fovy), p_setf(p, "DLSSG.CameraAspectRatio", (float)rw / rh);
    p_setu(p, "DLSSG.CameraMotionIncluded", camera_motion);
    p_setu(p, "DLSSG.Reset", reset);
    p_setf(p, "DLSSG.MvecInvalidValue", 3.0e38f);  // (none are)
    p_setu(p, "DLSSG.MultiFrameCount", (unsigned)f->count);
    for (int k = 0; k < f->count; k++) {
        NgxResourceVK out = image_resource(f->out[k], f->out[k]->format, true);
        p_setp(p, "DLSSG.OutputInterpolated", &out);
        p_setu(p, "DLSSG.MultiFrameIndex", (unsigned)k + 1);
        if (k) vk_full_barrier(cb);
        NgxResult r = N.evaluate(cb, g_fg, p, NULL);
        if (!NGX_OK(r)) {
            static int n;
            if (n++ < 8) LOG("[dlss] frame generation: evaluate %d of %d: %s (%#x)\n", k + 1, f->count, result_name(r), r);
            return false;
        }
    }
    return true;
}

#else

void vk_dlss_required_instance_extensions(const char ***names, unsigned *count) {
    if (names) *names = NULL;
    if (count) *count = 0;
}
void vk_dlss_required_device_extensions(const char ***names, unsigned *count) {
    if (names) *names = NULL;
    if (count) *count = 0;
}
bool vk_dlss_init(void) { return false; }
void vk_dlss_shutdown(void) {}
bool vk_dlss_available(void) { return false; }
bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion, TexBackend *output,
                   unsigned quality, float jitter_x, float jitter_y, float mv_scale_x, float mv_scale_y, bool reset) {
    (void)cb, (void)color, (void)depth, (void)motion, (void)output, (void)quality, (void)jitter_x, (void)jitter_y;
    (void)mv_scale_x, (void)mv_scale_y, (void)reset;
    return false;
}
bool vk_dlssg_available(void) { return false; }
int vk_dlssg_max_mult(void) { return 2; }
bool vk_dlssg_live(void) { return false; }
void vk_dlssg_release(void) {}
bool vk_dlssg_dispatch(VkCommandBuffer cb, const DlssgFrame *f) {
    (void)cb, (void)f;
    return false;
}

#endif
