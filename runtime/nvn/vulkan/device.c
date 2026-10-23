// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../../window_size.h"
#include "vk.h"
#include "../output_scale.h"
#include "../renderer.h"

#ifdef _WIN32  // (kernel32; windows.h itself clashes with names used here)
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_WRITE_THROUGH 0x8
#endif

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
uint64_t g_addr_report_lo, g_addr_report_hi;  // (SAVAGEWIND_VK_ADDR_REPORT)
uint64_t renderer_present_count(void);

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)user;
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) {  // (SAVAGEWIND_VK_ADDR_REPORT)
        extern uint64_t g_addr_report_lo, g_addr_report_hi;
        const VkDeviceAddressBindingCallbackDataEXT *ab = NULL;
        for (const VkBaseInStructure *x = d->pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) ab = (const void *)x;
        if (ab && ab->baseAddress < g_addr_report_hi && ab->baseAddress + ab->size > g_addr_report_lo)
            LOG("[vkaddr] %s %#llx+%#llx %s obj type %d handle %#llx (frame %llu)\n",
                ab->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? "bind" : "unbind", (unsigned long long)ab->baseAddress,
                (unsigned long long)ab->size, (ab->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) ? "internal" : "",
                d->objectCount ? (int)d->pObjects[0].objectType : -1, d->objectCount ? (unsigned long long)d->pObjects[0].objectHandle : 0ull,
                (unsigned long long)renderer_present_count());
        return VK_FALSE;
    }
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// SAVAGEWIND_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("SAVAGEWIND_VK_VALIDATION") != NULL;
    // (debugging) SAVAGEWIND_VK_ADDR_REPORT=<lo>-<hi> (hex GPU addresses): every bind and unbind of GPU memory in that
    // range ("[vkaddr]"), to name what a device fault's address was
    const char *ar = getenv("SAVAGEWIND_VK_ADDR_REPORT");
    if (ar) sscanf(ar, "%llx-%llx", (unsigned long long *)&g_addr_report_lo, (unsigned long long *)&g_addr_report_hi);
    if (validate || ar) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Savage Wind",
                             .pEngineName = "Savage Wind", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate || ar) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               (ar ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT : 0),
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                           (ar ? VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT : 0),
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (SAVAGEWIND_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("SAVAGEWIND_GPU") ? atoi(getenv("SAVAGEWIND_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i;
            g_vk.qfam_count = qp[i].queueCount;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    { extern void perfmon_set_gpu_hint(const char *n); perfmon_set_gpu_hint(g_vk.props.deviceName); }
}

// Video > Upscaler: NVIDIA DLSS needs an NVIDIA RTX GPU (tensor cores: GeForce / Quadro RTX, the RTX A / Ada
// workstation cards; not the GTX 16 series)
bool renderer_gpu_rtx(void) {
    return g_vk.phys && g_vk.props.vendorID == 0x10DE && strstr(g_vk.props.deviceName, "RTX") != NULL;
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void pipeline_cache_load(void);
static void *pc_saver(void *arg);

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[20];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    bool addr_report = g_addr_report_hi && has_ext(ep, ne, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
    if (addr_report) ext[n_ext++] = VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME;
    if (g_addr_report_hi) LOG("[vk] address binding report %s\n", addr_report ? "on" : "not supported");
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // Color targets a draw samples while it writes them (exec.c resolve_feedback): a feedback loop in GENERAL layout
    // with a barrier inside the pass between such draws, instead of a copy of the target before each one.
    // SAVAGEWIND_VK_NO_FEEDBACK_LOOP=1: copies, as before.
    bool fbl = !getenv("SAVAGEWIND_VK_NO_FEEDBACK_LOOP") && has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
               has_ext(ep, ne, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME) &&
               has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    // Pipelines linked from per-stage parts (exec.c, pipeline libraries): a pipeline whose shaders are known only
    // links. SAVAGEWIND_VK_NO_GPL=1: whole pipelines, as before.
    bool gpl = !getenv("SAVAGEWIND_VK_NO_GPL") && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
               has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    bool eds3 = gpl && has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
                                                               .pNext = &fbl_have};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .pNext = &uil_have};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .pNext = fbl ? &lr_have : NULL};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = &eds3_have};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &gpl_have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gpl_props};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &props2);
    g_vk.gpl = gpl && gpl_have.graphicsPipelineLibrary && gpl_props.graphicsPipelineLibraryFastLinking;
    if (g_vk.gpl) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    eds3 = g_vk.gpl && eds3 && eds3_have.extendedDynamicState3DepthClampEnable && eds3_have.extendedDynamicState3PolygonMode;
    if (eds3) ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    LOG("[vk] pipeline libraries %s%s\n", g_vk.gpl ? "on" : "off (whole pipelines)", eds3 ? ", depth clamp and polygon mode dynamic" : "");
    g_vk.feedback_loop = fbl && fbl_have.attachmentFeedbackLoopLayout && uil_have.unifiedImageLayouts && lr_have.dynamicRenderingLocalRead;
    if (g_vk.feedback_loop) {
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    }
    LOG("[vk] attachment feedback loops %s\n", g_vk.feedback_loop ? "on" : "off (target copies)");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceAddressBindingReportFeaturesEXT abr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
                                                           .reportAddressBinding = VK_TRUE, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, .attachmentFeedbackLoopLayout = VK_TRUE,
        .pNext = addr_report ? (void *)&abr : fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
                                                             .unifiedImageLayouts = VK_TRUE, .pNext = &fbl_on};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .dynamicRenderingLocalRead = VK_TRUE,
        .pNext = &uil_on};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .extendedDynamicState3DepthClampEnable = VK_TRUE,
        .extendedDynamicState3PolygonMode = VK_TRUE,
        .pNext = g_vk.feedback_loop ? (void *)&lr_on : addr_report ? (void *)&abr : fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE,
        .pNext = eds3 ? (void *)&eds3_on : eds3_on.pNext};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = g_vk.gpl ? (void *)&gpl_on : gpl_on.pNext};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    g_vk.wide_lines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio[2] = {1.0f, 1.0f};
    // two queues when the family has them: the second one presents (present thread below)
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = g_vk.qfam_count >= 2 ? 2 : 1, .pQueuePriorities = prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    if (eds3) {
        g_vk.set_depth_clamp = (PFN_vkCmdSetDepthClampEnableEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetDepthClampEnableEXT");
        g_vk.set_polygon_mode = (PFN_vkCmdSetPolygonModeEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetPolygonModeEXT");
        if (!g_vk.set_depth_clamp || !g_vk.set_polygon_mode) g_vk.set_depth_clamp = NULL, g_vk.set_polygon_mode = NULL;
    }
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    g_vk.present_queue = g_vk.queue;
    if (qi.queueCount == 2) vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 1, &g_vk.present_queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    pipeline_cache_load();
    pthread_t saver;
    pthread_create(&saver, NULL, pc_saver, NULL);
}

// ================================================================== pipeline cache
// cache/vk_pipelines.bin: PcHeader, then the driver's pipeline cache data. The header says what the data is good
// for - this GPU, this driver, this build of the shader translator - and checks it is whole: with any of that
// different, or the file damaged or cut short, the cache starts empty (the file is replaced at the next save). The
// driver checks its own header too, and data it rejects starts the cache empty as well.
// Saved at exit and, while the game runs, from a thread of its own a few seconds after new pipelines stop coming
// (at most every 30 s, at least every 2 minutes while there are new ones), so a crash loses little. A temporary
// file replaces the old one only once complete. SAVAGEWIND_VK_NO_PIPELINE_CACHE=1: neither loaded nor saved.
#define PC_VERSION 1
typedef struct {
    char magic[8];  // "SWPCACHE"
    uint32_t version, vendor, device, driver;
    uint8_t uuid[VK_UUID_SIZE];
    char shaders[64];  // vk_shader_version()
    uint64_t size, hash;  // of the data after the header
} PcHeader;

const char *vk_shader_version(void);
void vk_cache_path(char *out, size_t n, const char *name);
static _Atomic int pc_new;         // pipelines made since the last save that the cache did not have
static _Atomic uint64_t pc_new_at; // when the last one was made (ms)
static uint64_t pc_saved_at;
static pthread_mutex_t pc_save_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pc_off;

static uint64_t data_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

static void pc_header(PcHeader *h) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "SWPCACHE", 8);
    h->version = PC_VERSION;
    h->vendor = g_vk.props.vendorID;
    h->device = g_vk.props.deviceID;
    h->driver = g_vk.props.driverVersion;
    memcpy(h->uuid, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    snprintf(h->shaders, sizeof h->shaders, "%s", vk_shader_version());
}

// The data of a cache file, or NULL with *why saying what is wrong with it.
static void *pc_read(FILE *f, size_t *size, const char **why) {
    PcHeader h, want;
    pc_header(&want);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < (long)sizeof h || fread(&h, sizeof h, 1, f) != 1) { *why = "too short"; return NULL; }
    if (memcmp(h.magic, "SWPC", 4) && n > 16 + VK_UUID_SIZE) {
        // the format before the header (the driver's data alone, saved at exit only): taken once if the driver's own
        // header says it is for this GPU and driver; the next save writes the new format
        uint8_t *d = malloc((size_t)n);
        uint32_t w[4];
        fseek(f, 0, SEEK_SET);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n && (memcpy(w, d, 16), w[0] >= 16 + VK_UUID_SIZE) &&
            w[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && w[2] == want.vendor && w[3] == want.device &&
            !memcmp(d + 16, want.uuid, VK_UUID_SIZE)) {
            *size = (size_t)n;
            *why = "older format, taken";
            atomic_store(&pc_new, 1);  // (rewritten in the new format at the next save)
            return d;
        }
        free(d);
        *why = "older format, for another GPU or driver";
        return NULL;
    }
    if (memcmp(h.magic, want.magic, 8) || h.version != want.version) { *why = "older format"; return NULL; }
    if (h.vendor != want.vendor || h.device != want.device) { *why = "made on another GPU"; return NULL; }
    if (h.driver != want.driver || memcmp(h.uuid, want.uuid, VK_UUID_SIZE)) { *why = "made with another driver"; return NULL; }
    if (memcmp(h.shaders, want.shaders, sizeof h.shaders)) { *why = "made by another shader translator build"; return NULL; }
    if (h.size != (uint64_t)n - sizeof h) { *why = "cut short"; return NULL; }
    uint8_t *d = malloc(h.size ? h.size : 1);
    if (!d || fread(d, 1, h.size, f) != h.size || data_hash(d, h.size) != h.hash) { free(d); *why = "damaged"; return NULL; }
    // the driver's own header (VkPipelineCacheHeaderVersionOne): length, version 1, vendor, device, UUID
    uint32_t w[4];
    if (h.size < 16 + VK_UUID_SIZE) { free(d); *why = "empty"; return NULL; }
    memcpy(w, d, 16);
    if (w[0] < 16 + VK_UUID_SIZE || w[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || w[2] != want.vendor || w[3] != want.device ||
        memcmp(d + 16, want.uuid, VK_UUID_SIZE)) { free(d); *why = "driver header does not match"; return NULL; }
    *size = h.size;
    return d;
}

static void pipeline_cache_load(void) {
    pc_off = getenv("SAVAGEWIND_VK_NO_PIPELINE_CACHE") != NULL;
    char path[512];
    vk_cache_path(path, sizeof path, "vk_pipelines.bin");
    size_t sz = 0;
    void *data = NULL;
    const char *why = pc_off ? "off (SAVAGEWIND_VK_NO_PIPELINE_CACHE)" : "no file yet";
    FILE *fp = pc_off ? NULL : fopen(path, "rb");
    if (fp) {
        why = NULL;
        data = pc_read(fp, &sz, &why);
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = data ? sz : 0,
                                    .pInitialData = data};
    VkResult r = vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache);
    if (r != VK_SUCCESS && data) {
        why = "rejected by the driver";
        free(data);
        data = NULL;
        pc.initialDataSize = 0;
        pc.pInitialData = NULL;
        r = vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache);
    }
    VK_CHECK(r);
    if (data) LOG("[vk] pipeline cache: %s, %.1f MB%s%s%s\n", path, sz / 1048576.0, why ? " (" : "", why ? why : "", why ? ")" : "");
    else LOG("[vk] pipeline cache: starts empty (%s: %s)\n", path, why);
    free(data);
    pc_saved_at = SDL_GetTicks();
}

// A finished temporary file in place of path, in one step (the old file stays whole until then).
bool vk_replace_file(const char *tmp, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

// exec.c, after each pipeline made: hit = the pipeline cache had it (creation feedback; false when unknown).
void vk_pipeline_cache_note(bool hit) {
    if (hit) return;
    atomic_fetch_add(&pc_new, 1);
    atomic_store(&pc_new_at, SDL_GetTicks());
}

void vk_pipeline_cache_save(void) {
    if (pc_off || !g_vk.pcache) return;
    pthread_mutex_lock(&pc_save_lock);
    int news = atomic_exchange(&pc_new, 0);
    if (!news) { pthread_mutex_unlock(&pc_save_lock); return; }
    uint64_t t0 = SDL_GetTicksNS();
    PcHeader h;
    pc_header(&h);
    size_t sz = 0;
    uint8_t *buf = NULL;
    VkResult r = VK_INCOMPLETE;
    for (int tries = 0; tries < 4 && r == VK_INCOMPLETE; tries++) {  // (pipelines made meanwhile make it grow)
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) break;
        sz += sz / 16;
        free(buf);
        buf = malloc(sizeof h + sz);
        if (!buf) break;
        r = vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, buf + sizeof h);
    }
    char path[512], tmp[540];
    vk_cache_path(path, sizeof path, "vk_pipelines.bin");
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    bool ok = r == VK_SUCCESS && buf;
    if (ok) {
        h.size = sz;
        h.hash = data_hash(buf + sizeof h, sz);
        memcpy(buf, &h, sizeof h);
        FILE *f = fopen(tmp, "wb");
        ok = f && fwrite(buf, 1, sizeof h + sz, f) == sizeof h + sz;
        if (f && fclose(f)) ok = false;
        if (!ok || !vk_replace_file(tmp, path)) { ok = false; remove(tmp); }
    }
    free(buf);
    if (!ok) atomic_fetch_add(&pc_new, news);  // (try again later)
    pc_saved_at = SDL_GetTicks();
    LOG("[vk] pipeline cache %s: %.1f MB, %d new pipelines, %.0f ms\n", ok ? "saved" : "NOT saved", sz / 1048576.0, news,
        (SDL_GetTicksNS() - t0) / 1e6);
    pthread_mutex_unlock(&pc_save_lock);
}

static void *pc_saver(void *arg) {
    (void)arg;
    for (;;) {
        SDL_Delay(1000);
        extern void vk_pso_flush(void);
        vk_pso_flush();  // (exec.c: the pipeline list's new records)
        if (!atomic_load(&pc_new)) continue;
        uint64_t now = SDL_GetTicks(), quiet = now - atomic_load(&pc_new_at), since = now - pc_saved_at;
        if ((quiet >= 3000 && since >= 30000) || since >= 120000) vk_pipeline_cache_save();
    }
    return NULL;
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. SAVAGEWIND_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("SAVAGEWIND_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    vk_present_idle();
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ================================================================== present thread
// vkQueuePresentKHR waits until the GPU has finished the frame (NVIDIA on Windows, windowed). On the game's GPU thread
// that wait (13-18 ms on a fifth of the open-world frames: the frame's last submission goes to the GPU just before)
// came on top of translating the next frame, and BOTW's frame timer dropped the game to 20 fps (interval 3) whenever
// three frames in a row ran over 31.7 ms. On the console a present only queues the frame. Here a thread of its own
// presents, on a second queue of the same family (queues are externally synchronized: the GPU thread keeps submitting
// on the first one meanwhile). One present at a time; the next acquire, a swapchain change and teardown wait for it.
// SAVAGEWIND_VK_PRESENT_THREAD=0 (or a family with one queue): presents on the calling thread, as before.
static pthread_mutex_t pres_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cv = PTHREAD_COND_INITIALIZER;
static bool pres_busy;           // a present is queued or in progress
static VkPresentInfoKHR pres_info;
static VkSwapchainKHR pres_sc;
static VkSemaphore pres_sem;
static uint32_t pres_img;
static _Atomic int pres_stale;   // that present said the swapchain is out of date or suboptimal

static void *present_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pres_m);
    for (;;) {
        while (!pres_busy) pthread_cond_wait(&pres_cv, &pres_m);
        VkPresentInfoKHR pi = pres_info;
        pthread_mutex_unlock(&pres_m);
        VkResult r = vkQueuePresentKHR(g_vk.present_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store(&pres_stale, 1);
        else if (r != VK_SUCCESS) { static int n; if (n++ < 5) LOG("[vk] present failed: %d\n", r); }
        pthread_mutex_lock(&pres_m);
        pres_busy = false;
        pthread_cond_broadcast(&pres_cv);
    }
    return NULL;
}

static bool present_threaded(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_VK_PRESENT_THREAD");
        on = g_vk.present_queue != g_vk.queue && !(e && !strcmp(e, "0"));
        if (on) { pthread_t t; pthread_create(&t, NULL, present_thread, NULL); }
        LOG("[vk] presents on %s\n", on ? "a thread of their own (second queue)" : "the calling thread");
    }
    return on;
}

void vk_present_idle(void) {
    pthread_mutex_lock(&pres_m);
    while (pres_busy) pthread_cond_wait(&pres_cv, &pres_m);
    pthread_mutex_unlock(&pres_m);
}

// Present window image img once sem is signalled. Returns false when the swapchain must be made again.
static bool present_image(uint32_t img, VkSemaphore sem) {
    if (!present_threaded()) {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
                               .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
        return !(r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR);
    }
    pthread_mutex_lock(&pres_m);
    while (pres_busy) pthread_cond_wait(&pres_cv, &pres_m);
    pres_sc = g_vk.swapchain;
    pres_sem = sem;
    pres_img = img;
    pres_info = (VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &pres_sem,
                                   .swapchainCount = 1, .pSwapchains = &pres_sc, .pImageIndices = &pres_img};
    pres_busy = true;
    pthread_cond_signal(&pres_cv);
    pthread_mutex_unlock(&pres_m);
    return true;
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

uint64_t g_present_split[6];  // (VK_PROF) acquire, end pass, draw + menu, submit, present, slot wait
typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    // minimised (Windows keeps the window's size then, but its surface is 0 x 0: every acquire was out of date)
    if (!w || !h || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) return false;
    uint64_t tp = SDL_GetTicksNS();
    vk_present_idle();  // (the swapchain is externally synchronized: no acquire while the present thread presents)
    g_present_split[4] += SDL_GetTicksNS() - tp;
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        atomic_exchange(&pres_stale, 0))
        recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("SAVAGEWIND_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    uint64_t tw = SDL_GetTicksNS();
    vk_wait_seq(slot_seq[slot]);
    g_present_split[5] += SDL_GetTicksNS() - tw;
    uint64_t ta = SDL_GetTicksNS();
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    g_present_split[0] += SDL_GetTicksNS() - ta;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        static unsigned failed;  // (a few, then every 1000th: one a frame filled the log)
        if (failed++ < 5 || failed % 1000 == 0) LOG("[vk] acquire failed: %d (%u times)\n", r, failed);
        return false;
    }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vk_dyn_forget();
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vk_dyn_forget();
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    uint64_t ts = SDL_GetTicksNS();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    g_present_split[3] += SDL_GetTicksNS() - ts;
    ts = SDL_GetTicksNS();
    if (!present_image(img, done_sem[img])) recreate_swapchain();
    g_present_split[4] += SDL_GetTicksNS() - ts;
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

// Saved opaque: render targets often keep data in alpha (BOTW's G-buffer), which image viewers turn into black.
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    memcpy(px, rgba, (size_t)w * h * 4);
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4) px[i] = 255;
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, px, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
    free(px);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
void renderer_set_scene_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void) {
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

// ---- pipeline pre-warm (exec.c): the first frame that looks like gameplay waits until the pipelines of the list are
// built. When that takes more than a moment, the window shows a bar of how far it is meanwhile (black, the bar a
// third of the width near the bottom; clears only, nothing to compile).
bool vk_pso_wait_needed(void);
bool vk_pso_wait(int ms, float *done);

static void present_progress(float done) {
    static const float black[4] = {0, 0, 0, 1};
    if (!vk_frame_begin(black)) return;
    VkCommandBuffer cb = vk_cmd();
    int32_t w = (int32_t)g_vk.sc_extent.width, h = (int32_t)g_vk.sc_extent.height;
    int32_t bw = w / 3, bh = h / 90 > 4 ? h / 90 : 4, x = (w - bw) / 2, y = h - h / 8;
    int32_t fill = (int32_t)(done * (float)bw);
    struct { VkRect2D r; float c; } parts[3] = {{{{x - 2, y - 2}, {(uint32_t)bw + 4, (uint32_t)bh + 4}}, 0.4f},
                                                {{{x, y}, {(uint32_t)bw, (uint32_t)bh}}, 0.06f},
                                                {{{x, y}, {(uint32_t)(fill > 0 ? fill : 0), (uint32_t)bh}}, 0.85f}};
    for (int i = 0; i < 3; i++) {
        if (!parts[i].r.extent.width || x < 2 || y < 2) continue;
        VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                               .clearValue.color.float32 = {parts[i].c, parts[i].c, parts[i].c, 1}};
        VkClearRect cr = {parts[i].r, 0, 1};
        vkCmdClearAttachments(cb, 1, &a, 1, &cr);
    }
    static int shot;  // (SAVAGEWIND_PSO_SLOW_MS, exec.c: the first bar frame past a quarter)
    bool save = !shot && done > 0.25f && getenv("SAVAGEWIND_PSO_SLOW_MS");
    if (save) { shot = 1; mkdir("build", 0755); }
    vk_frame_end(save ? "build/pso_wait.png" : NULL);
}

static uint64_t pso_wait(void) {  // ns waited
    if (!vk_pso_wait_needed()) return 0;
    uint64_t t0 = SDL_GetTicksNS();
    float done = 0;
    int bars = 0;
    while (!vk_pso_wait(100, &done))
        if (SDL_GetTicksNS() - t0 > 300000000ull) { present_progress(done); bars++; }
    uint64_t dt = SDL_GetTicksNS() - t0;
    LOG("[pso] frame %llu waited %.0f ms for the pipeline pre-warm (%d frames with the bar)\n", (unsigned long long)total_frames + 1,
        dt / 1e6, bars);
    return dt;
}

static void present_locked(NvnTexture *t) {
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    uint64_t t0 = SDL_GetTicksNS();
    vk_end_pass();
    g_present_split[1] += SDL_GetTicksNS() - t0;
    t0 += pso_wait();  // (not the acquire's time)
    static const float black[4] = {0, 0, 0, 1};
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video page "Upscaler"). The game keeps
        // its own 16:9 framing, fitted into the window with bars (a widened camera needs a game-specific hook).
        // Fullscreen on a size the display has no mode for (runtime/window_size.c): that size, scaled to the screen.
        OutScale os;
        int iw, ih;
        if (winsize_virtual(&iw, &ih) && (iw != (int)dw || ih != (int)dh)) {
            output_scale(iw, ih, false, &os);
            output_scale_screen(dw, dh, iw, ih, &os);
        } else {
            output_scale(dw, dh, false, &os);
        }
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh;
        float crop[4] = {0, 0, 1, 1};  // uv origin and size: the whole image
        renderer_set_scale((float)sx, (float)sy);
        renderer_set_scene_scale((float)os.scene_sx, (float)os.scene_sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        uint64_t td = SDL_GetTicksNS();
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("SAVAGEWIND_MENU_AT") ? atol(getenv("SAVAGEWIND_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_present_split[2] += SDL_GetTicksNS() - td;
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("SAVAGEWIND_WINDOW_SHOT") ? atol(getenv("SAVAGEWIND_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        vk_frame_end(shot ? "build/window.png" : NULL);
        if (shot && getenv("SAVAGEWIND_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    {   // SAVAGEWIND_FRAME_LOG=1: "[ft] <ms> <frame> <target fps>" at every present (frame time statistics)
        static int ft = -1;
        if (ft < 0) ft = getenv("SAVAGEWIND_FRAME_LOG") != NULL;
        extern int framepace_target(void);
        if (ft) LOG("[ft] %.3f %llu %d\n", SDL_GetTicksNS() / 1e6, (unsigned long long)total_frames, framepace_target());
    }
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void input_frame(uint64_t frame);
    extern uint64_t framepace_script_frame(uint64_t presented);
    uint64_t script_frame = framepace_script_frame(total_frames);  // (SAVAGEWIND_SCRIPT_CLOCK: 30ths of a second)
    input_frame(script_frame);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("SAVAGEWIND_SLOW_FRAME_MS") ? atol(getenv("SAVAGEWIND_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (SAVAGEWIND_REBOOT_AT)
    reboot_test_frame(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("SAVAGEWIND_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("SAVAGEWIND_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // SAVAGEWIND_DEBUG_FRAMES=<n> (with SAVAGEWIND_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("SAVAGEWIND_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    if (!g_debug_frame) { extern void vk_pick_report(void); vk_pick_report(); }  // (SAVAGEWIND_DEBUG_PICK)
    {   // SAVAGEWIND_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("SAVAGEWIND_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // (testing) SAVAGEWIND_UPSCALE_AT=<frame>:<off|ultra|quality|balanced|performance>,... (ascending): the
        // upscaler from that frame on, as the Video page sets it (A/B of the same view a few frames apart)
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("SAVAGEWIND_UPSCALE_AT"); }
        while (next && *next) {
            char *end;
            long at = strtol(next, &end, 10);
            if ((long)total_frames < at) break;
            const char *m = *end == ':' ? end + 1 : end;
            static const char *const names[] = {"ultra", "quality", "balanced", "performance"};
            g_settings.upscaler = UPSCALE_OFF;
            for (int i = 0; i < 4; i++)
                if (!strncmp(m, names[i], strlen(names[i]))) g_settings.upscaler = UPSCALE_FSR1, g_settings.upscale_quality = i;
            LOG("[test] frame %llu: upscaler %d mode %d\n", (unsigned long long)total_frames, g_settings.upscaler,
                g_settings.upscale_quality);
            next = strchr(m, ',');
            if (next) next++;
        }
    }
    {   // SAVAGEWIND_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("SAVAGEWIND_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)script_frame < at) { while (next > getenv("SAVAGEWIND_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)script_frame >= at) {  // (the script clock can step over a number)
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05ld.png", at);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    {   // F12 (runtime/hle/input.c): every other game image -> build/frames/rec_N.png until F12 again (catches flicker
        // too short to see; the saves slow the game down meanwhile)
        extern volatile int g_record_frames;
        static int recording;
        if (g_record_frames != recording) {
            recording = g_record_frames;
            if (recording) mkdir("build/frames", 0755);
            LOG("[shot] recording %s at frame %llu\n", recording ? "on" : "off", (unsigned long long)total_frames);
        }
        if (recording && !(total_frames & 1)) {
            char path[256];
            snprintf(path, sizeof path, "build/frames/rec_%05llu.png", (unsigned long long)total_frames);
            renderer_save_texture_tex(b, path);
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("SAVAGEWIND_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until SAVAGEWIND_WINDOW_SHOT exits
        if (getenv("SAVAGEWIND_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("SAVAGEWIND_FPS_INTERVAL") ? atof(getenv("SAVAGEWIND_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("SAVAGEWIND_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vk_present_idle();
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
        extern void vk_pso_flush(void);
        vk_pso_flush();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    // Video > Window size (runtime/window_size.c: pixels; SDL's window size is pixels / the display's pixel density)
    winsize_refresh(NULL);
    WinSize ws = winsize_effective(false);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    float density = dm && dm->pixel_density > 0 ? dm->pixel_density : 1;
    int win_w = (int)lroundf(ws.w / density), win_h = (int)lroundf(ws.h / density);
    if (getenv("SAVAGEWIND_WINDOW")) sscanf(getenv("SAVAGEWIND_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("SAVAGEWIND_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Savage Wind", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    winsize_apply(window);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    vk_present_idle();
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        extern void input_mouse_update(SDL_Window *w);  // runtime/hle/input.c: hold the cursor while the game has it
        input_mouse_update(window);
    }
}

// (game threads present while others submit; the window loop re-presents)
void backend_present(NvnTexture *t) {
    gpu_lock();
    present_locked(t);
    gpu_unlock();
}
