// NVIDIA NGX / DLSS Super Resolution and Frame Generation integration.
// The SDK itself is optional and remains outside the Savage Wind repository.
#include "dlss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../paths.h"

#if defined(_WIN32) && DLSS_SDK_PRESENT

#include <math.h>
#include <wchar.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

static bool g_initialized;
static NVSDK_NGX_Parameter *g_capability_params;  // (NGX's: what this driver and GPU run)
static const char **g_instance_exts;
static const char **g_device_exts;
static unsigned g_instance_ext_count, g_device_ext_count;
static bool g_exts_queried;
static bool g_ngx_up;        // NGX initialized (vk_dlss_init)
static bool g_fg_available;  // DLSS Frame Generation runs here (RTX 40 and later)
static bool g_available;     // DLSS Super Resolution runs here (RTX)
static NVSDK_NGX_Parameter *g_params;
static NVSDK_NGX_Handle *g_feature;
static unsigned g_in_w, g_in_h, g_out_w, g_out_h, g_quality;
static VkFormat g_color_format, g_output_format;
static int g_fg_max_mult = 2;  // frames on screen for each drawn it makes here (MultiFrameCountMax + 1; RTX 50: more)

static const char *result_name(NVSDK_NGX_Result r) {
    switch (r) {
    case NVSDK_NGX_Result_Success: return "success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature-not-supported";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "unable-to-initialize-feature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "out-of-date";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "unsupported-input-format";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "missing-input";
    default: return "failure";
    }
}

static bool ok(NVSDK_NGX_Result r, const char *what) {
    if (r == NVSDK_NGX_Result_Success) return true;
    LOG("[dlss] %s: %s (%#x)\n", what, result_name(r), (unsigned)r);
    return false;
}

static NVSDK_NGX_Resource_VK image_resource(TexBackend *b, bool rw) {
    static const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageView v = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id,
                                   b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageSubresourceRange range = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                     0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(v, b->image, range, b->format, b->width, b->height, rw);
}

static void release_feature(void) {
    if (g_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = NULL;
    }
    if (g_params) {
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        g_params = NULL;
    }
    g_in_w = g_in_h = g_out_w = g_out_h = g_quality = 0;
    g_color_format = g_output_format = VK_FORMAT_UNDEFINED;
}

bool vk_dlss_compiled(void) { return true; }

// Project IDs must be GUID-like for custom engines. This ID is stable for
// Savage Wind and does not require an NVIDIA-issued application ID.
#define PROJECT_ID "6f4bb6ce-20d8-4a8d-9c3d-0b7d5df1c3a1"
#define ENGINE_VERSION "SavageWind-1.0"
static NVSDK_NGX_FeatureCommonInfo g_common;  // (where the feature DLLs are: ngx_paths)
static wchar_t g_app_data[1024];

// The data folder (NGX's logs and caches) and where the feature DLLs are, for NGX's init and its requirement queries
// (which come before the device: vk_dlss_required_device_extensions). false: no data folder.
static bool ngx_paths(void) {
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    const char *data = rt_data_root();
    if (!data || mbstowcs(g_app_data, data, sizeof(g_app_data) / sizeof(*g_app_data) - 1) == (size_t)-1) {
        LOG("[dlss] invalid application data path\n");
        return false;
    }
    g_app_data[(sizeof(g_app_data) / sizeof(*g_app_data)) - 1] = 0;

    // Prefer the redistributable next to the executable, then try the sibling
    // SDK checkout used by source builds. Supplying both paths avoids relying
    // on the process working directory (which differs between the launcher and
    // a direct executable launch).
    char root[1024];
    static const wchar_t *paths[2];
    static wchar_t dll_path[1024], sdk_path[1024];
    unsigned npaths = 0;
    if (rt_resource("nvngx_dlss.dll", NULL, root, sizeof(root))) {
        char *slash = strrchr(root, '\\');
        if (!slash) slash = strrchr(root, '/');
        if (slash) {
            *slash = 0;
            if (mbstowcs(dll_path, root, sizeof(dll_path) / sizeof(*dll_path) - 1) != (size_t)-1) {
                dll_path[(sizeof(dll_path) / sizeof(*dll_path)) - 1] = 0;
                paths[npaths++] = dll_path;
                LOG("[dlss] runtime path: %s\n", root);
            }
        }
    }
    if (rt_resource(NULL, "../DLSS/lib/Windows_x86_64/rel", root, sizeof(root)) &&
        mbstowcs(sdk_path, root, sizeof(sdk_path) / sizeof(*sdk_path) - 1) != (size_t)-1) {
        sdk_path[(sizeof(sdk_path) / sizeof(*sdk_path)) - 1] = 0;
        if (npaths < 2) paths[npaths++] = sdk_path;
        LOG("[dlss] SDK path: %s\n", root);
    }
    g_common.PathListInfo.Path = paths;
    g_common.PathListInfo.Length = npaths;
    done = 1;
    return true;
}

static NVSDK_NGX_FeatureDiscoveryInfo discovery(NVSDK_NGX_Feature f) {
    NVSDK_NGX_FeatureDiscoveryInfo d = {};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = f;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc = {PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_VERSION};
    d.ApplicationDataPath = g_app_data;
    d.FeatureInfo = &g_common;
    return d;
}

static void query_extensions(void) {
    if (g_exts_queried) return;
    g_exts_queried = true;
    unsigned ni = 0, nd = 0;
    const char **i = NULL, **d = NULL;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &i, &nd, &d);
    if (r == NVSDK_NGX_Result_Success) {
        g_instance_exts = i; g_instance_ext_count = ni;
        g_device_exts = d; g_device_ext_count = nd;
        LOG("[dlss] required extensions: %u instance, %u device\n", ni, nd);
    } else {
        LOG("[dlss] required extension query: %s (%#x)\n", result_name(r), (unsigned)r);
    }
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
        NVSDK_NGX_FeatureDiscoveryInfo di = discovery(NVSDK_NGX_Feature_FrameGeneration);
        NVSDK_NGX_Result r = g_vk.inst && g_vk.phys && ngx_paths()
                                 ? NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_vk.inst, g_vk.phys, &di, &nf, &ep)
                                 : NVSDK_NGX_Result_Fail;
        if (r != NVSDK_NGX_Result_Success) nf = 0;
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

bool vk_dlss_init(void) {
    if (g_initialized) return g_ngx_up;
    g_initialized = true;
    if (!g_vk.phys || !g_vk.dev || !ngx_paths()) return false;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_VERSION,
        g_app_data, g_vk.inst, g_vk.phys, g_vk.dev, nullptr, nullptr, &g_common);
    if (!ok(r, "init")) return false;
    g_ngx_up = true;
    if (!ok(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_capability_params), "capabilities")) return true;
    int sr = 0;
    g_available = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_capability_params,
                                                             NVSDK_NGX_Parameter_SuperSampling_Available, &sr)) && sr != 0;
    LOG("[dlss] Super Resolution %s on %s\n", g_available ? "available" : "unavailable", g_vk.props.deviceName);
    int fg = 0;
    g_fg_available = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_capability_params,
                                                                NVSDK_NGX_Parameter_FrameGeneration_Available, &fg)) && fg == 1;
    unsigned mfc = 0;
    if (g_fg_available && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_capability_params,
                                                                      NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfc)) &&
        mfc > 1)
        g_fg_max_mult = mfc + 1 > 4 ? 4 : (int)mfc + 1;  // (Video > Frame generation multiplier: up to 4x)
    LOG("[dlss] Frame Generation %s on %s (up to %dx)\n", g_fg_available ? "available" : "unavailable",
        g_vk.props.deviceName, g_fg_max_mult);
    return true;
}

void vk_dlss_shutdown(void) {
    if (!g_initialized) return;
    vk_dlssg_release();
    release_feature();
    if (g_ngx_up) NVSDK_NGX_VULKAN_Shutdown1(g_vk.dev);
    g_ngx_up = g_fg_available = g_available = false;
    g_capability_params = NULL;
    g_initialized = false;
}

// ================================================================== DLSS Super Resolution (Video > Upscaler)
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
        NVSDK_NGX_DLSS_Create_Params create = {};
        create.Feature.InWidth = color->width;
        create.Feature.InHeight = color->height;
        create.Feature.InTargetWidth = output->width;
        create.Feature.InTargetHeight = output->height;
        static const NVSDK_NGX_PerfQuality_Value q[] = {
            NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_MaxQuality,
            NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
            NVSDK_NGX_PerfQuality_Value_UltraPerformance};  // (UPQ_*: Ultra Performance's 3.0 is DLSS's own)
        unsigned requested = quality < 5 ? quality : 1;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        // Some driver/NGX combinations reject Ultra Quality at a particular
        // render size. Retry the next conservative mode before disabling DLSS.
        static const unsigned fallback[] = {0, 1, 2, 3};
        bool created = false;
        for (unsigned attempt = 0; attempt < 4 && !created; attempt++) {
            unsigned mode = attempt == 0 ? requested : fallback[attempt];
            create.Feature.InPerfQualityValue = q[mode];
            if (!ok(NVSDK_NGX_VULKAN_AllocateParameters(&g_params), "allocate parameters")) return false;
            NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_vk.dev, cb, 1, 1, &g_feature, g_params, &create);
            if (r == NVSDK_NGX_Result_Success) {
                created = true;
                if (attempt) LOG("[dlss] quality %u rejected; using fallback mode %u\n", requested, mode);
            } else {
                LOG("[dlss] quality mode %u rejected: %s (%#x)\n", mode, result_name(r), (unsigned)r);
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

    NVSDK_NGX_Resource_VK in_color = image_resource(color, false);
    NVSDK_NGX_Resource_VK in_depth = image_resource(depth, false);
    NVSDK_NGX_Resource_VK in_motion = image_resource(motion, false);
    NVSDK_NGX_Resource_VK out = image_resource(output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = &in_color;
    eval.Feature.pInOutput = &out;
    eval.pInDepth = &in_depth;
    eval.pInMotionVectors = &in_motion;
    eval.InJitterOffsetX = jitter_x;
    eval.InJitterOffsetY = jitter_y;
    eval.InRenderSubrectDimensions.Width = color->width;
    eval.InRenderSubrectDimensions.Height = color->height;
    eval.InReset = reset ? 1 : 0;
    eval.InMVScaleX = mv_scale_x;  // (NGX takes them in render pixels)
    eval.InMVScaleY = mv_scale_y;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, g_feature, g_params, &eval);
    if (!ok(r, "evaluate")) {
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
extern "C" void vk_fg_wait_idle(void);  // (device.c)
static NVSDK_NGX_Handle *g_fg;
static NVSDK_NGX_Parameter *g_fg_params;  // (its own map: NGX's maps keep what was set)
static uint32_t g_fg_w, g_fg_h, g_fg_rw, g_fg_rh;
static VkFormat g_fg_format;

bool vk_dlssg_available(void) { return g_fg_available; }
int vk_dlssg_max_mult(void) { return g_fg_available ? g_fg_max_mult : 2; }

void vk_dlssg_release(void) {
    if (g_fg) NVSDK_NGX_VULKAN_ReleaseFeature(g_fg);
    if (g_fg_params) NVSDK_NGX_VULKAN_DestroyParameters(g_fg_params);
    g_fg = NULL, g_fg_params = NULL;
    g_fg_w = g_fg_h = g_fg_rw = g_fg_rh = 0;
    g_fg_format = VK_FORMAT_UNDEFINED;
}

bool vk_dlssg_live(void) { return g_fg != NULL; }

static NVSDK_NGX_Resource_VK fg_resource(TexBackend *b, VkFormat f, bool rw) {
    static const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageAspectFlags a = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView v = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, a);
    VkImageSubresourceRange range = {a, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(v, b->image, range, f, b->width, b->height, rw);
}

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

bool vk_dlssg_dispatch(VkCommandBuffer cb, const DlssgFrame *f) {
    if (!g_fg_available || !cb) return false;
    uint32_t w = f->color->width, h = f->color->height, rw = f->depth->width, rh = f->depth->height;
    if (!g_fg || g_fg_w != w || g_fg_h != h || g_fg_rw != rw || g_fg_rh != rh || g_fg_format != f->view_format) {
        if (g_fg) vk_fg_wait_idle();  // (frames in flight still use it)
        vk_dlssg_release();
        if (!ok(NVSDK_NGX_VULKAN_AllocateParameters(&g_fg_params), "frame generation: parameters")) return false;
        NVSDK_NGX_DLSSG_Create_Params cp = {};
        cp.Width = w, cp.Height = h, cp.NativeBackbufferFormat = f->view_format, cp.RenderWidth = rw, cp.RenderHeight = rh;
        // (NGX's helper sets only the generic, deprecated width / height)
        NVSDK_NGX_Parameter_SetUI(g_fg_params, NVSDK_NGX_DLSSG_Parameter_Width, w);
        NVSDK_NGX_Parameter_SetUI(g_fg_params, NVSDK_NGX_DLSSG_Parameter_Height, h);
        NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(cb, 1, 1, &g_fg, g_fg_params, &cp);
        LOG("[dlss] frame generation %ux%u (depth and motion %ux%u), format %d: %s\n", w, h, rw, rh, f->view_format,
            result_name(r));
        if (r != NVSDK_NGX_Result_Success) {
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
    NVSDK_NGX_DLSSG_Opt_Eval_Params op = {};
    mul4(s, f->prev_vp, t), mul4(t, vpi, t2), mul4(t2, si, t), to_ngx(t, op.clipToPrevClip);
    mul4(s, f->vp, t), mul4(t, pvpi, t2), mul4(t2, si, t), to_ngx(t, op.prevClipToClip);
    mul4(s, f->proj, t), to_ngx(t, op.cameraViewToClip);
    mul4(pi, si, t), to_ngx(t, op.clipToCameraView);
    static const double id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_ngx(id, op.clipToLensClip);
    // (exec.c's motion vectors: uv in the previous frame minus uv in this one; NGX takes pixels of their image)
    op.mvecScale[0] = (float)rw, op.mvecScale[1] = (float)rh;
    static float msx = 0, msy = 0;  // (testing) SAVAGEWIND_DLSSG_MVSIGN=<x>,<y>: the vectors' signs
    if (!msx) {
        msx = msy = 1;
        if (getenv("SAVAGEWIND_DLSSG_MVSIGN")) sscanf(getenv("SAVAGEWIND_DLSSG_MVSIGN"), "%f,%f", &msx, &msy);
    }
    op.mvecScale[0] *= msx, op.mvecScale[1] *= msy;
    memcpy(op.cameraPos, f->cam_pos, sizeof op.cameraPos), memcpy(op.cameraUp, f->cam_up, sizeof op.cameraUp);
    memcpy(op.cameraRight, f->cam_right, sizeof op.cameraRight), memcpy(op.cameraFwd, f->cam_fwd, sizeof op.cameraFwd);
    op.cameraNear = f->near_, op.cameraFar = f->far_, op.cameraFOV = f->fovy, op.cameraAspectRatio = (float)rw / rh;
    op.cameraMotionIncluded = true;
    static int mtx_test = -1;  // (testing the matrices) SAVAGEWIND_DLSSG_MV_TEST=1: no motion vectors, camera motion
    if (mtx_test < 0) mtx_test = getenv("SAVAGEWIND_DLSSG_MV_TEST") != NULL;  // from depth and clipToPrevClip alone
    if (mtx_test) {
        VkClearColorValue zero = {};
        VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, f->motion->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &cr);
        vk_full_barrier(cb);
        op.cameraMotionIncluded = false;
    }
    op.reset = f->reset;
    op.motionVectorsInvalidValue = 3.0e38f;  // (none are)

    NVSDK_NGX_Resource_VK color = fg_resource(f->color, f->view_format, false);
    NVSDK_NGX_Resource_VK hudless = fg_resource(f->hudless, f->view_format, false);
    NVSDK_NGX_Resource_VK depth = fg_resource(f->depth, f->depth->format, false);
    NVSDK_NGX_Resource_VK motion = fg_resource(f->motion, f->motion->format, false);
    // Multi frame generation: one evaluation per frame, multiFrameIndex 1 .. count (the later ones reuse the first's
    // work, NVIDIA's guide). A change of count starts over.
    static int last_count;
    if (f->count != last_count) op.reset = true;
    last_count = f->count;
    op.multiFrameCount = (unsigned)f->count;
    for (int k = 0; k < f->count; k++) {
        NVSDK_NGX_Resource_VK out = fg_resource(f->out[k], f->out[k]->format, true);
        NVSDK_NGX_VK_DLSSG_Eval_Params ev = {};
        ev.pBackbuffer = &color, ev.pDepth = &depth, ev.pMVecs = &motion, ev.pHudless = &hudless;
        ev.pOutputInterpFrame = &out;
        op.multiFrameIndex = (unsigned)k + 1;
        if (k) vk_full_barrier(cb);
        NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(cb, g_fg, g_fg_params, &ev, &op);
        if (r != NVSDK_NGX_Result_Success) {
            static int n;
            if (n++ < 8) LOG("[dlss] frame generation: evaluate %d of %d: %s (%#x)\n", k + 1, f->count, result_name(r), (unsigned)r);
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
bool vk_dlss_compiled(void) { return false; }
bool vk_dlss_available(void) { return false; }
bool vk_dlss_apply(VkCommandBuffer, TexBackend *, TexBackend *, TexBackend *, TexBackend *, unsigned, float, float, float, float,
                   bool) {
    return false;
}
bool vk_dlssg_available(void) { return false; }
int vk_dlssg_max_mult(void) { return 2; }
bool vk_dlssg_live(void) { return false; }
void vk_dlssg_release(void) {}
bool vk_dlssg_dispatch(VkCommandBuffer, const DlssgFrame *) { return false; }

#endif
