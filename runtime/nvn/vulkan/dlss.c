// NVIDIA DLSS Super Resolution (Video > Upscaler) through NGX, loaded at run time. No NVIDIA SDK code, header or library is compiled in or linked: the NGX core is the NVIDIA driver's
// own _nvngx.dll (found through HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore), and the few NGX types, values and
// parameter names used below are declared here from the SDK's documented interface. The DLSS library the core loads for
// it, nvngx_dlss.dll, is NVIDIA's own copy, which the setup downloads next to savagewind.exe from NVIDIA's repository
// (../../dlss_fetch.c); without it, or without an NVIDIA RTX driver, NGX is not loaded at all and DLSS stays greyed
// out.
//
// NGX's parameter object is a C++ class whose virtual functions are called through its vtable: the Set overloads in
// slots 0-7 and the Get overloads in 8-15, each group in reverse declaration order (MSVC's layout, the Windows core's).
// vtable_ok checks that layout with a set-then-get round trip before anything relies on it.
#include "dlss.h"
#include "record.h"

#include <math.h>
#include <stdatomic.h>
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
enum { NGX_FEATURE_SUPERSAMPLING = 1 };
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
typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const NgxParam *);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
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
static wchar_t g_exe_dir[MAX_PATH];  // savagewind.exe's folder, where the DLSS library is (no trailing slash)
static wchar_t g_data_dir[1024];     // NGX's logs and caches: the data folder

// NVIDIA's DLSS library is next to savagewind.exe, where NGX loads it from (NGX itself is loaded only then)
static bool dll_here(void) {
    static int have = -1;
    if (have >= 0) return have;
    have = 0;
    DWORD len = GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
    if (!len || len >= MAX_PATH) return have;
    while (len && g_exe_dir[len - 1] != L'\\' && g_exe_dir[len - 1] != L'/') len--;
    g_exe_dir[len ? len - 1 : 0] = 0;
    wchar_t p[MAX_PATH + 32];
    _snwprintf(p, MAX_PATH + 32, L"%ls\\nvngx_dlss.dll", g_exe_dir);
    p[MAX_PATH + 31] = 0;
    have = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    return have;
}

static bool try_load_core(const char *path) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        LOG("[dlss] cannot load %s (error %lu)\n", path, GetLastError());
        return false;
    }
#define FN(f, name) N.f = (void *)GetProcAddress(h, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
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
        LOG("[dlss] the NGX core %s lacks Vulkan entry points\n", path);
        FreeLibrary(h);
        memset(&N, 0, sizeof N);
        N.tried = 1;
        return false;
    }
    N.core = h;
    return true;
}

// (skip_path may be out itself, the registry's core that failed: it is copied first, as out is written each step)
static bool find_driverstore_core(const char *skip_path, char *out, size_t nout) {
    char sysdir[MAX_PATH] = {0}, pattern[MAX_PATH + 64], skip[MAX_PATH + 32] = {0};
    if (skip_path) snprintf(skip, sizeof skip, "%s", skip_path);
    UINT slen = GetSystemDirectoryA(sysdir, MAX_PATH);
    if (!slen || slen >= MAX_PATH) return false;
    snprintf(pattern, sizeof pattern, "%s\\DriverStore\\FileRepository\\nv*", sysdir);
    WIN32_FIND_DATAA fd;
    HANDLE ff = FindFirstFileA(pattern, &fd);
    if (ff == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        snprintf(out, nout, "%s\\DriverStore\\FileRepository\\%s\\_nvngx.dll", sysdir, fd.cFileName);
        if (skip[0] && !_stricmp(out, skip)) continue;
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES && try_load_core(out)) {
            found = true;
            break;
        }
    } while (FindNextFileA(ff, &fd));
    FindClose(ff);
    return found;
}

static bool core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
    if (!dll_here()) {
        LOG("[dlss] NVIDIA's DLSS library is not next to savagewind.exe (the setup downloads it): DLSS off\n");
        return false;
    }
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32] = {0};
    DWORD n = sizeof dir;
    bool have_reg = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath",
                                 RRF_RT_REG_SZ, NULL, dir, &n) == ERROR_SUCCESS;
    if (have_reg) snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    else LOG("[dlss] registry NGXCore missing, scanning DriverStore\n");
    if ((!have_reg || !try_load_core(path)) && !find_driverstore_core(have_reg ? path : NULL, path, sizeof path)) {
        LOG("[dlss] no usable NGX core found: DLSS off\n");
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

void vk_dlss_required_device_extensions(const char ***names, unsigned *count) {
    query_extensions();
    if (names) *names = g_device_exts;
    if (count) *count = g_device_ext_count;
}

// ================================================================== init
static bool g_initialized, g_ngx_up;
static NgxParam *g_capability_params;  // (NGX's: what this driver and GPU run)
static bool g_available;               // DLSS Super Resolution runs here (RTX)

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
    if (g_initialized) return g_available;
    g_initialized = true;
    if (!g_vk.phys || !g_vk.dev || !core_load()) return false;
    if (!ok(N.init(NGX_APP_ID, g_data_dir, g_vk.inst, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                   NGX_VERSION_API, NULL),
            "init"))
        return false;
    if (!vtable_ok()) return false;
    g_ngx_up = true;
    if (!ok(N.caps(&g_capability_params), "capabilities") || !g_capability_params) return false;
    int sr = 0, driver = 0;
    g_available = p_geti(g_capability_params, "SuperSampling.Available", &sr) && sr != 0;
    p_geti(g_capability_params, "SuperSampling.NeedsUpdatedDriver", &driver);
    LOG("[dlss] Super Resolution %s on %s%s\n", g_available ? "available" : "unavailable", g_vk.props.deviceName,
        driver ? " (the driver needs an update)" : "");
    return g_available;
}

static void release_feature(void);

void vk_dlss_shutdown(void) {
    if (!g_initialized) return;
    release_feature();
    unsigned left = 0;
    if (g_ngx_up && N.shutdown) N.shutdown(g_vk.dev, &left);
    g_ngx_up = g_available = false;
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

// The model NGX runs (its render presets). Left to NGX, DLSS 310 picks its transformer models, and for Performance a
// heavier one still: at 4K on an RTX 3060, 5.1 ms a frame in Quality and 11 ms in Performance, against 8.1 ms for the
// whole frame without an upscaler, so DLSS cost frames instead of making them. RTX 20 and 30 cards (no FP8 in their
// tensor cores) get the CNN model, preset E: 2.0 ms in every mode, the frame 6.4 ms in Performance. RTX 40 and later
// get the transformer model K in every mode, not the heavier one NGX picks for Performance.
// SAVAGEWIND_DLSS_PRESET=<n>: another preset for every mode (0: NGX's own choice, 5 E, 11 K).
enum { DLSS_PRESET_DEFAULT = 0, DLSS_PRESET_E = 5, DLSS_PRESET_K = 11 };
static bool dlss_older_rtx(const char *name) {
    static const char *const older[] = {"RTX 20", "RTX 30", "TITAN RTX", "Quadro RTX", "RTX A1", "RTX A2", "RTX A3",
                                        "RTX A4", "RTX A5", "RTX A6", "MX5", "CMP 30"};
    for (unsigned i = 0; i < sizeof older / sizeof *older; i++)
        if (strstr(name, older[i])) return true;
    return false;
}
static void p_dlss_preset(NgxParam *p) {
    static int preset = -2;
    if (preset == -2) {
        const char *e = getenv("SAVAGEWIND_DLSS_PRESET");
        preset = e ? atoi(e) : dlss_older_rtx(g_vk.props.deviceName) ? DLSS_PRESET_E : DLSS_PRESET_K;
        LOG("[dlss] model: preset %d (%s)\n", preset,
            e ? "SAVAGEWIND_DLSS_PRESET" : preset == DLSS_PRESET_E ? "CNN: an RTX 20 / 30 card" : "transformer");
    }
    if (preset <= DLSS_PRESET_DEFAULT) return;
    static const char *const hints[] = {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
                                        "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
                                        "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
    for (unsigned i = 0; i < sizeof hints / sizeof *hints; i++) p_setu(p, hints[i], (unsigned)preset);
}

// One frame's DLSS, as the recorder thread runs it (dlss_run): the images' views made on the queue thread
// (vk_image_view), NGX's calls in the recorder's turn.
typedef struct {
    VkCommandBuffer cb;
    NgxResourceVK color, depth, motion, out;
    unsigned quality;
    float jitter_x, jitter_y, mv_scale_x, mv_scale_y;
    bool reset;
} DlssJob;
static _Atomic int g_failed;  // (a job failed: known to the queue thread a frame late, as FSR 3's)

// The recorder thread (or the queue thread, with the recorder off): NGX's feature made again when the sizes, the
// formats or the mode changed, and the frame evaluated
static void dlss_run(void *arg) {
    const DlssJob *j = arg;
    unsigned cw = j->color.r.img.width, ch = j->color.r.img.height, ow = j->out.r.img.width, oh = j->out.r.img.height;
    VkFormat cf = j->color.r.img.format, of = j->out.r.img.format;
    if (!g_available) { atomic_store(&g_failed, 1); return; }
    if (!g_feature || g_in_w != cw || g_in_h != ch || g_out_w != ow || g_out_h != oh || g_quality != j->quality ||
        g_color_format != cf || g_output_format != of) {
        release_feature();
        static const int q[] = {NGX_PERF_ULTRAQUALITY, NGX_PERF_MAXQUALITY, NGX_PERF_BALANCED, NGX_PERF_MAXPERF,
                                NGX_PERF_ULTRAPERF};  // (UPQ_*: Ultra Performance's 3.0 is DLSS's own)
        static bool uq_rejected;
        unsigned requested = j->quality < 5 ? j->quality : 1;
        unsigned start = (requested == 0 && uq_rejected) ? 1 : requested;
        // Some driver/NGX combinations reject Ultra Quality at a particular render size. Remember that
        // rejection so dynamic-resolution steps go straight to Quality, and step down without duplicate retries.
        bool created = false;
        unsigned tries = start < 4 ? 4 : 5;
        for (unsigned mode = start; mode < tries && !created; mode++) {
            if (!ok(N.alloc_params(&g_params), "allocate parameters")) { atomic_store(&g_failed, 1); return; }
            NgxParam *p = g_params;
            p_setu(p, "CreationNodeMask", 1);
            p_setu(p, "VisibilityNodeMask", 1);
            p_setu(p, "Width", cw);
            p_setu(p, "Height", ch);
            p_setu(p, "OutWidth", ow);
            p_setu(p, "OutHeight", oh);
            p_seti(p, "PerfQualityValue", q[mode]);
            p_seti(p, "DLSS.Feature.Create.Flags", NGX_DLSS_IS_HDR | NGX_DLSS_MV_LOWRES);
            p_seti(p, "DLSS.Enable.Output.Subrects", 0);
            p_dlss_preset(p);
            NgxResult r = N.create(j->cb, NGX_FEATURE_SUPERSAMPLING, p, &g_feature);
            if (NGX_OK(r)) {
                created = true;
                if (mode != requested) LOG("[dlss] quality %u rejected; using fallback mode %u\n", requested, mode);
            } else {
                if (mode == 0) uq_rejected = true;
                LOG("[dlss] quality mode %u rejected: %s (%#x)\n", mode, result_name(r), r);
                release_feature();
            }
        }
        if (!created) {
            g_available = false;
            atomic_store(&g_failed, 1);
            return;
        }
        g_in_w = cw; g_in_h = ch; g_out_w = ow; g_out_h = oh;
        g_quality = j->quality; g_color_format = cf; g_output_format = of;
    }
    NgxResourceVK in_color = j->color, in_depth = j->depth, in_motion = j->motion, out = j->out;
    NgxParam *p = g_params;
    p_setp(p, "Color", &in_color);
    p_setp(p, "Output", &out);
    p_setp(p, "Depth", &in_depth);
    p_setp(p, "MotionVectors", &in_motion);
    p_setf(p, "Jitter.Offset.X", j->jitter_x);
    p_setf(p, "Jitter.Offset.Y", j->jitter_y);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", j->reset ? 1 : 0);
    p_setf(p, "MV.Scale.X", j->mv_scale_x == 0.0f ? 1.0f : j->mv_scale_x);  // (NGX takes them in render pixels)
    p_setf(p, "MV.Scale.Y", j->mv_scale_y == 0.0f ? 1.0f : j->mv_scale_y);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", cw);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", ch);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    if (!ok(N.evaluate(j->cb, g_feature, p, NULL), "evaluate")) {
        release_feature();
        atomic_store(&g_failed, 1);
    }
}

// NGX records into cb itself and may wait for the device: like FSR 3, the recorder thread makes the calls, in their
// turn among the commands queued before. Draining the recorder and calling NGX on the queue thread instead cost that
// thread, where it limits the frame rate, the whole recorder backlog each frame (RTX 3060, 1080p Savage: DLSS 3-6 fps
// under FSR 3). Whether it worked is known a frame late: a failure leaves this frame's output undefined and the
// caller falls back from the next one.
bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion,
                   TexBackend *output, unsigned quality, float jitter_x, float jitter_y, float mv_scale_x,
                   float mv_scale_y, bool reset) {
    if (!g_available || atomic_exchange(&g_failed, 0) || !cb || !color || !depth || !motion || !output ||
        !color->image || !depth->image || !motion->image || !output->image ||
        color->width == 0 || color->height == 0 || output->width == 0 || output->height == 0)
        return false;
    static DlssJob jobs[8];  // (the recorder is never 8 frames behind: a submission a frame waits for it)
    static unsigned nj;
    DlssJob *j = &jobs[nj++ & 7];
    *j = (DlssJob){cb, image_resource(color, color->format, false), image_resource(depth, depth->format, false),
                   image_resource(motion, motion->format, false), image_resource(output, output->format, true),
                   quality, jitter_x, jitter_y, mv_scale_x, mv_scale_y, reset};
    if (rec_on()) rec_custom(dlss_run, j);
    else dlss_run(j);
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
#endif
