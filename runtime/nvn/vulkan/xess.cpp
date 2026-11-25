// Intel XeSS super resolution over Vulkan (xess.h): libxess.dll loaded at run time from the game's folder, as AMD's
// FSR DLLs (fsr_api.cpp). XeSS-SR runs on every GPU with DP4a (SM 6.4); on Intel Arc through its own XMX cores.
#include "xess.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"  // (UPQ_*: the game's quality mode, handed to XeSS as its own setting)
#include "vk.h"
#include "record.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
// (no libxess outside Windows: Intel ships none; everything below compiles to stubs)
#define LoadLibraryW(x) ((void *)0)
#endif

#define XESS_TYPES_ONLY  // (the functions are fetched with GetProcAddress: only the types are used here)
#include <xess/xess.h>
#include <xess/xess_vk.h>

// The API functions used (xess.h / xess_vk.h's signatures, fetched by name)
typedef xess_result_t (*pfn_xessGetVersion)(xess_version_t *);
typedef xess_result_t (*pfn_xessVKGetRequiredInstanceExtensions)(uint32_t *, const char *const **, uint32_t *);
typedef xess_result_t (*pfn_xessVKGetRequiredDeviceExtensions)(VkInstance, VkPhysicalDevice, uint32_t *,
                                                               const char *const **);
typedef xess_result_t (*pfn_xessVKGetRequiredDeviceFeatures)(VkInstance, VkPhysicalDevice, void **);
typedef xess_result_t (*pfn_xessVKCreateContext)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t *);
typedef xess_result_t (*pfn_xessVKInit)(xess_context_handle_t, const xess_vk_init_params_t *);
typedef xess_result_t (*pfn_xessVKExecute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t *);
typedef xess_result_t (*pfn_xessDestroyContext)(xess_context_handle_t);
typedef xess_result_t (*pfn_xessSetLoggingCallback)(xess_context_handle_t, xess_logging_level_t,
                                                    xess_app_log_callback_t);
typedef xess_result_t (*pfn_xessGetJitterScale)(xess_context_handle_t, float *, float *);
typedef xess_result_t (*pfn_xessGetVelocityScale)(xess_context_handle_t, float *, float *);
typedef xess_result_t (*pfn_xessSetVelocityScale)(xess_context_handle_t, float, float);

static pfn_xessGetVersion f_get_version;
static pfn_xessVKGetRequiredInstanceExtensions f_inst_ext;
static pfn_xessVKGetRequiredDeviceExtensions f_dev_ext;
static pfn_xessVKGetRequiredDeviceFeatures f_dev_feat;
static pfn_xessVKCreateContext f_create;
static pfn_xessVKInit f_init;
static pfn_xessVKExecute f_execute;
static pfn_xessDestroyContext f_destroy;
static pfn_xessSetLoggingCallback f_log;
static pfn_xessGetJitterScale f_jitter_scale;
static pfn_xessGetVelocityScale f_velocity_scale;
static pfn_xessSetVelocityScale f_set_velocity_scale;

static void *lib;      // HMODULE
static bool lib_tried;  // (the DLL looked for once; found or not)
static xess_context_handle_t ctx;

#define PROC(name) (GetProcAddress((HMODULE)lib, name))

// (the library's own messages: a line at most once in a row, 24 in a run)
static void message(const char *msg, xess_logging_level_t level) {
    static char last[512];
    static int logged;
    if (!msg || !strcmp(msg, last) || logged >= 24) return;
    snprintf(last, sizeof last, "%s", msg);
    logged++;
    LOG("[xess] %s: %s\n",
        level >= XESS_LOGGING_LEVEL_ERROR ? "error" : level >= XESS_LOGGING_LEVEL_WARNING ? "warning" : "info", msg);
}

static const char *result_name(xess_result_t r) {
    switch (r) {
    case XESS_RESULT_SUCCESS: return "success";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "old driver";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device (SM 6.4 / DP4a needed)";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
    case XESS_RESULT_ERROR_DEVICE: return "device error";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load library";
    default: return "error";
    }
}

// The DLL next to the game (its full path only, like fsr_api.cpp's: not from the DLL search path)
static bool load_library(void) {
    if (lib_tried) return lib != NULL;
    lib_tried = true;
#ifdef _WIN32
    wchar_t path[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy(slash + 1, L"libxess.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    lib = (void *)LoadLibraryW(path);
    if (!lib) {
        LOG("[xess] libxess.dll did not load (error %lu)\n", (unsigned long)GetLastError());
        return false;
    }
#define GOT(name, var)                                                          \
    if (!(var = (decltype(var))PROC(name))) {                                   \
        LOG("[xess] libxess.dll lacks " name "\n");                             \
        FreeLibrary((HMODULE)lib), lib = NULL;                                  \
        return false;                                                           \
    }
    GOT("xessGetVersion", f_get_version)
    GOT("xessVKGetRequiredInstanceExtensions", f_inst_ext)
    GOT("xessVKGetRequiredDeviceExtensions", f_dev_ext)
    GOT("xessVKGetRequiredDeviceFeatures", f_dev_feat)
    GOT("xessVKCreateContext", f_create)
    GOT("xessVKInit", f_init)
    GOT("xessVKExecute", f_execute)
    GOT("xessDestroyContext", f_destroy)
    GOT("xessSetLoggingCallback", f_log)
    GOT("xessGetJitterScale", f_jitter_scale)
    GOT("xessGetVelocityScale", f_velocity_scale)
    GOT("xessSetVelocityScale", f_set_velocity_scale)
#undef GOT
    xess_version_t v = {};
    if (f_get_version(&v) == XESS_RESULT_SUCCESS)
        LOG("[xess] libxess.dll %u.%u.%u loaded\n", v.major, v.minor, v.patch);
#endif
    return lib != NULL;
}

void xess_required_instance_extensions(const char *const **names, uint32_t *count, uint32_t *min_api) {
    if (names) *names = NULL;
    if (count) *count = 0;
    if (min_api) *min_api = 0;
    if (!load_library()) return;
    uint32_t n = 0;
    const char *const *e = NULL;
    uint32_t api = 0;
    if (f_inst_ext(&n, &e, &api) != XESS_RESULT_SUCCESS) return;
    if (names) *names = e;
    if (count) *count = n;
    if (min_api) *min_api = api;
}

bool xess_required_device_extensions(VkInstance inst, VkPhysicalDevice phys, const char *const **names,
                                     uint32_t *count, void **features) {
    if (names) *names = NULL;
    if (count) *count = 0;
    if (features) *features = NULL;
    if (!load_library()) return false;
    uint32_t n = 0;
    const char *const *e = NULL;
    if (f_dev_ext(inst, phys, &n, &e) != XESS_RESULT_SUCCESS) return false;
    if (names) *names = e;
    if (count) *count = n;
    if (features) f_dev_feat(inst, phys, features);  // (the feature chain the device is made with)
    return true;
}

// The context's state: the output size xessVKInit ran for (0: none yet) and the quality mode it ran with, and whether
// it failed this run. ctx_iw/ctx_ih are the input size the last dispatch used, only so a change of it can be logged:
// the Dynamic mode moves the render size every few seconds, and without a line per change the log's one
// "initialized ... -> ..." reads as if the scene were still at the size it was first made for.
static uint32_t ctx_ow, ctx_oh;
static uint32_t ctx_iw, ctx_ih;
static int ctx_quality = -1;
static bool failed;
static VkDevice ctx_dev;

void xess_destroy(void) {
    if (!ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    f_destroy(ctx);
    ctx = NULL;
    ctx_ow = ctx_oh = 0;
}

bool xess_probe(VkInstance inst, VkDevice dev, VkPhysicalDevice phys) {
    if (!load_library()) return false;
    if (ctx) return true;
    xess_result_t r = f_create(inst, phys, dev, &ctx);
    if (r == XESS_RESULT_WARNING_OLD_DRIVER)
        LOG("[xess] the driver is older than XeSS wants: it runs, but updating it improves XeSS\n");
    else if (r != XESS_RESULT_SUCCESS) {
        LOG("[xess] context: %s (%d)\n", result_name(r), (int)r);
        ctx = NULL;
        return false;
    }
    f_log(ctx, XESS_LOGGING_LEVEL_WARNING, message);
    ctx_dev = dev;
    float jx = 0, jy = 0;
    if (f_jitter_scale(ctx, &jx, &jy) == XESS_RESULT_SUCCESS)
        LOG("[xess] context made (jitter scale %.3g %.3g)\n", jx, jy);
    else
        LOG("[xess] context made\n");
    return true;
}

bool xess_available(void) { return ctx != NULL; }
bool xess_usable(void) { return !failed && ctx != NULL; }

bool xess_output_changed(const Fsr3Dispatch *d) {
    return ctx != NULL &&
           (ctx_ow != d->output.width || ctx_oh != d->output.height || ctx_quality != d->quality);
}

// The game's quality mode as XeSS's own setting. XeSS picks its internal counts from the render size it is given,
// but the setting still names a scale ratio its docs expect and its own tuning reads; the mode the player picked is
// the one to ask for, so that the menu's factor and XeSS's idea of it agree (they did not, once: the setting was a
// fixed Ultra Quality whatever the menu said, and Performance ran exactly as fast as Quality).
static xess_quality_settings_t xess_quality(int game_mode) {
    switch (game_mode) {
    case UPQ_ULTRA: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case UPQ_QUALITY: return XESS_QUALITY_SETTING_QUALITY;
    case UPQ_BALANCED: return XESS_QUALITY_SETTING_BALANCED;
    case UPQ_PERFORMANCE: return XESS_QUALITY_SETTING_PERFORMANCE;
    case UPQ_ULTRA_PERFORMANCE: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    default: return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;  // (Native: 1x, the sharpest end XeSS has)
    }
}

// XeSS has no context-level size: xessVKInit is run for a new output size (and once per run per size after that).
// The API asks for the device to be idle when it runs again between frames, which xess_dispatch takes care of (it
// drains the recorder first: see there).
static bool init_for(const Fsr3Dispatch *d) {
    xess_vk_init_params_t ip = {};
    ip.outputResolution = {d->output.width, d->output.height};
    // (LDR: the scene is the game's finished, graded image. Low-res vectors + depth: XeSS dilates them itself. Not
    // inverted depth: R32F [0, 1], 1 far, as copied. The responsive mask on: the reactive mask FSR 3 gets is handed
    // over too (mvec's .z, shaders.c fsr3_reactive.frag).)
    ip.qualitySetting = xess_quality(d->quality);
    ip.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    xess_result_t r = f_init(ctx, &ip);
    if (r != XESS_RESULT_SUCCESS) {
        static int logged;
        if (logged++ < 8)
            LOG("[xess] init %ux%u: %s (%d)\n", d->output.width, d->output.height, result_name(r), (int)r);
        return false;
    }
    ctx_ow = d->output.width, ctx_oh = d->output.height, ctx_quality = d->quality;
    ctx_iw = d->color.width, ctx_ih = d->color.height;
    LOG("[xess] initialized %ux%u -> %ux%u (mode %d, setting %d)\n", d->color.width, d->color.height, ctx_ow, ctx_oh,
        d->quality, (int)ip.qualitySetting);
    return true;
}

extern "C" void vk_gpuprof_mark(VkCommandBuffer cb, const char *what);  // (exec.c)

// One XeSS view per image this frame's dispatch touches (the library wants image + view pairs): made here, dropped
// after. The translating thread defers its drops to textures.c (vk_defer_view), which that thread owns; the recorder
// thread (xess_run) cannot queue anything and must not touch that unlocked list, so it destroys the views of the
// frame before instead - the submission that used them was waited on by the translating thread between the two frames
// (a submission a frame waits for the recorder), so the GPU is done with them by then.
struct XessView {
    VkImageView view;
    VkImage image;
};
static XessView frame_views[5];
static int n_frame_views;
static VkImageView rec_stale[5];  // (the recorder thread's views of the frame before, destroyed by the next dispatch)
static int n_rec_stale;
static VkImageView xess_view(VkImage image, VkFormat fmt, uint32_t w, uint32_t h) {
    for (int i = 0; i < n_frame_views; i++)
        if (frame_views[i].image == image) return frame_views[i].view;
    if (n_frame_views >= 5) return VK_NULL_HANDLE;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v;
    VK_CHECK(vkCreateImageView(ctx_dev, &ci, NULL, &v));
    frame_views[n_frame_views++] = {v, image};
    return v;
}
static void drop_frame_views(void) {
    if (rec_on_recorder()) {
        for (int i = 0; i < n_rec_stale; i++) vkDestroyImageView(ctx_dev, rec_stale[i], NULL);
        n_rec_stale = 0;
        for (int i = 0; i < n_frame_views; i++) rec_stale[n_rec_stale++] = frame_views[i].view;
    } else {
        for (int i = 0; i < n_frame_views; i++) vk_defer_view(frame_views[i].view);
    }
    n_frame_views = 0;
}

bool xess_dispatch(const Fsr3Dispatch *d) {
    if (!ctx || failed) return false;
    bool resize = ctx_ow != d->output.width || ctx_oh != d->output.height || ctx_quality != d->quality;
    if (resize && !init_for(d)) {  // (xessVKInit wants the device idle; the caller drains and idles for that frame)
        failed = true;
        return false;
    }
    // The Dynamic mode changes the render size without touching the output size or the mode, so this is not a
    // re-init - XeSS is told the input's size per execute call (ep.inputWidth below). Logged once per change so the
    // "initialized ..." line above is not the only size in the file, and to keep SAVAGEWIND_DYNAMIC_LOG's steps
    // readable against what XeSS was actually given.
    if (d->color.width != ctx_iw || d->color.height != ctx_ih) {
        LOG("[xess] render size %ux%u -> %ux%u (Dynamic: the output stays %ux%u)\n", ctx_iw, ctx_ih, d->color.width,
            d->color.height, ctx_ow, ctx_oh);
        ctx_iw = d->color.width, ctx_ih = d->color.height;
    }
    VkCommandBuffer cb = d->cb;
    vk_full_barrier(cb);  // (the passes before wrote the inputs; the library is given clean layouts after it)
    n_frame_views = 0;
    // The game's images are left in GENERAL (textures.c); XeSS wants its inputs in SHADER_READ_ONLY_OPTIMAL and the
    // output in GENERAL. The inputs' transitions are recorded here, around the library's own commands (it changes no
    // layouts); they go back after, and the output stays GENERAL, as XeSS leaves it.
    struct In { VkImage img; VkFormat fmt; uint32_t w, h; };
    const In ins[4] = {{d->color.image, d->color.format, d->color.width, d->color.height},
                       {d->motion.image, d->motion.format, d->motion.width, d->motion.height},
                       {d->depth.image, d->depth.format, d->depth.width, d->depth.height},
                       {d->reactive.image, d->reactive.format, d->reactive.width, d->reactive.height}};
    VkImageMemoryBarrier2 in_bar[4];
    uint32_t n_in = 0;
    for (const In &i : ins) {
        if (!i.img) continue;
        in_bar[n_in++] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .image = i.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n_in,
                           .pImageMemoryBarriers = in_bar};
    // (the recorder thread records straight into the command buffer: queuing a command from it would deadlock, so the
    // calls below pick their path by the thread they run on, as record.c's own do)
    if (rec_on_recorder()) vkCmdPipelineBarrier2(cb, &di);
    else rec_CmdPipelineBarrier2(cb, &di);
    // (the reactive mask only when the context was made for it: it is part of init_flags; we don't use it, XeSS's
    // reactive handling is its own. depth drives the dilation instead.)
    xess_vk_execute_params_t ep = {};
    ep.colorTexture = {xess_view(d->color.image, d->color.format, d->color.width, d->color.height), d->color.image,
                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, d->color.format, d->color.width, d->color.height};
    ep.velocityTexture = {xess_view(d->motion.image, d->motion.format, d->motion.width, d->motion.height),
                          d->motion.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, d->motion.format, d->motion.width,
                          d->motion.height};
    ep.depthTexture = {xess_view(d->depth.image, d->depth.format, d->depth.width, d->depth.height), d->depth.image,
                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, d->depth.format, d->depth.width, d->depth.height};
    if (d->reactive.image)
        ep.responsivePixelMaskTexture = {xess_view(d->reactive.image, d->reactive.format, d->reactive.width,
                                                   d->reactive.height),
                                         d->reactive.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, d->reactive.format,
                                         d->reactive.width, d->reactive.height};
    ep.outputTexture = {xess_view(d->output.image, d->output.format, d->output.width, d->output.height),
                        d->output.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, d->output.format, d->output.width,
                        d->output.height};
    ep.jitterOffsetX = d->jitter[0];  // (both in [-0.5, 0.5] pixels of the render size, y down: FSR's convention too)
    ep.jitterOffsetY = d->jitter[1];
    ep.exposureScale = 1.0f;
    ep.resetHistory = d->reset;
    ep.inputWidth = d->color.width;
    ep.inputHeight = d->color.height;
    // Velocity scale: XeSS takes pixel vectors (this frame to the previous), the game's are NDC differences. The
    // same clip-space values FSR gets with motion_scale -0.5 w / +0.5 h go to XeSS through its own setter (its y
    // points down, the game's up: the sign flips with it, as FSR's own mapping does). Set every frame: init_for()
    // has already stored the new output size in ctx_ow/ctx_oh by now, so a "size changed" test here is never true
    // (the scale stayed at XeSS's default 1, 1), and the render size can change without the output size changing.
    f_set_velocity_scale(ctx, -0.5f * (float)d->color.width, 0.5f * (float)d->color.height);
    vk_gpuprof_mark(cb, "XeSS upscale");
    if (!rec_on_recorder()) rec_drain();  // (the library records into cb itself: what was queued before goes first; on
                                          //  the recorder thread everything before it is already recorded)
    xess_result_t r = f_execute(ctx, cb, &ep);
    vk_gpuprof_mark(cb, "after XeSS");
    drop_frame_views();
    if (r != XESS_RESULT_SUCCESS) {
        static int logged;
        if (logged++ < 8) LOG("[xess] execute: %s (%d)\n", result_name(r), (int)r);
        failed = true;  // (no XeSS this run: the game's own composite from here on)
        return false;
    }
    // The inputs back to GENERAL (the rest of the frame writes and reads them as images; the output stays GENERAL)
    VkImageMemoryBarrier2 back[4];
    uint32_t n_back = 0;
    for (const In &i : ins) {
        if (!i.img) continue;
        back[n_back++] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                          .image = i.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    VkDependencyInfo di2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n_back,
                            .pImageMemoryBarriers = back};
    if (rec_on_recorder()) vkCmdPipelineBarrier2(cb, &di2);
    else rec_CmdPipelineBarrier2(cb, &di2);
    if (!rec_on_recorder()) vk_dyn_forget();  // (XeSS's own pipelines and sets were bound; on the recorder thread the
                                              //  binding state the translating thread keeps is not its to reset)
    return true;
}

uint32_t xess_jitter_phases(uint32_t render_w, uint32_t output_w) {
    // XeSS's SR guide: the sequence at least 8 x the upscale's area. The quality modes scale per axis (the game's
    // scene keeps its 16:9 shape), so the height ratio equals the width's.
    if (!render_w || !output_w || output_w <= render_w) return 8;
    float area = (float)output_w / (float)render_w;
    area *= area;
    return (uint32_t)ceilf(8.0f * area);
}
