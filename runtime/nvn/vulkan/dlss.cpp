// NVIDIA NGX / DLSS Super Resolution integration.
// The SDK itself is optional and remains outside the Savage Wind repository.
#include "dlss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../paths.h"

#if defined(_WIN32) && DLSS_SDK_PRESENT

#include <wchar.h>
#include <nvsdk_ngx_helpers_vk.h>

static bool g_initialized;
static bool g_available;
static NVSDK_NGX_Parameter *g_params;
static NVSDK_NGX_Parameter *g_capability_params;
static NVSDK_NGX_Handle *g_feature;
static unsigned g_in_w, g_in_h, g_out_w, g_out_h, g_quality;
static VkFormat g_color_format, g_output_format;
static const char **g_instance_exts;
static const char **g_device_exts;
static unsigned g_instance_ext_count, g_device_ext_count;
static bool g_exts_queried;

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

void vk_dlss_required_device_extensions(const char ***names, unsigned *count) {
    query_extensions();
    if (names) *names = g_device_exts;
    if (count) *count = g_device_ext_count;
}

bool vk_dlss_init(void) {
    if (g_initialized) return g_available;
    g_initialized = true;
    if (!g_vk.phys || !g_vk.dev) return false;

    char root[1024];
    const char *data = rt_data_root();
    wchar_t app_data[1024];
    if (!data || mbstowcs(app_data, data, sizeof(app_data) / sizeof(*app_data) - 1) == (size_t)-1) {
        LOG("[dlss] invalid application data path\n");
        return false;
    }
    app_data[(sizeof(app_data) / sizeof(*app_data)) - 1] = 0;

    // Prefer the redistributable next to the executable, then try the sibling
    // SDK checkout used by source builds. Supplying both paths avoids relying
    // on the process working directory (which differs between the launcher and
    // a direct executable launch).
    const wchar_t *paths[2] = {0};
    wchar_t dll_path[1024], sdk_path[1024];
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
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = npaths;

    // Project IDs must be GUID-like for custom engines. This ID is stable for
    // Savage Wind and does not require an NVIDIA-issued application ID.
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        "6f4bb6ce-20d8-4a8d-9c3d-0b7d5df1c3a1", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "SavageWind-1.0",
        app_data, g_vk.inst, g_vk.phys, g_vk.dev, nullptr, nullptr, &common);
    if (!ok(r, "init")) return false;
    if (!ok(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_capability_params), "capabilities")) {
        return false;
    }
    int available = 0;
    r = NVSDK_NGX_Parameter_GetI(g_capability_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    g_available = ok(r, "SuperSampling.Available") && available != 0;
    LOG("[dlss] Super Resolution %s on %s\n", g_available ? "available" : "unavailable", g_vk.props.deviceName);
    if (!g_available) {
        g_capability_params = NULL;
    }
    return g_available;
}

void vk_dlss_shutdown(void) {
    if (!g_initialized) return;
    release_feature();
    if (g_available) NVSDK_NGX_VULKAN_Shutdown1(g_vk.dev);
    g_available = false;
    g_capability_params = NULL;
    g_initialized = false;
}

bool vk_dlss_available(void) { return g_available; }

bool vk_dlss_apply(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *motion,
                   TexBackend *output, unsigned quality, float jitter_x, float jitter_y, bool reset) {
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
    // (the vectors are uv in the previous frame - uv in this one, exec.c cam_motion; NGX takes them in render pixels)
    eval.InMVScaleX = (float)motion->width;
    eval.InMVScaleY = (float)motion->height;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, g_feature, g_params, &eval);
    if (!ok(r, "evaluate")) {
        release_feature();
        return false;
    }
    return true;
}

#else

bool vk_dlss_compiled(void) { return false; }
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
bool vk_dlss_apply(VkCommandBuffer, TexBackend *, TexBackend *, TexBackend *, TexBackend *, unsigned, float, float, bool) {
    return false;
}

#endif
