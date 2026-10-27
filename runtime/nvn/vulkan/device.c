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
#ifdef _WIN32
#include <malloc.h>
#endif

#include "../../framepace.h"
#include "../../imageload.h"
#include "../../paths.h"
#include "../../perfmon.h"
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
static bool g_m5;     // maintenance5 enabled (the pipeline cache's header: pc_header)
static bool g_cs_ext;  // VK_EXT_swapchain_colorspace: HDR10 swapchains can be made (the HDR section below)

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
    const char *ext[24];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    {   // the HDR10 colour space of an HDR swapchain (Video > HDR) comes with VK_EXT_swapchain_colorspace
        uint32_t nie = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
        VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie);
        vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
        for (uint32_t i = 0; i < nie && !g_cs_ext; i++)
            if (!strcmp(ie[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) g_cs_ext = true;
        if (g_cs_ext) ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        free(ie);
    }
    bool validate = getenv("SAVAGEWIND_VK_VALIDATION") != NULL;
    // (debugging) SAVAGEWIND_VK_ADDR_REPORT=<lo>-<hi> (hex GPU addresses): every bind and unbind of GPU memory in that
    // range ("[vkaddr]"), to name what a device fault's address was
    const char *ar = getenv("SAVAGEWIND_VK_ADDR_REPORT");
    if (ar) sscanf(ar, "%llx-%llx", (unsigned long long *)&g_addr_report_lo, (unsigned long long *)&g_addr_report_hi);
    if (validate || ar) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    // The release as the application's and engine's version ("0.3.1-alpha.3": 0.3.103): drivers keep their shader
    // caches per application, and with one version for every release AMD's Windows driver reused what it had cached
    // for Alpha 2 in Alpha 3, built for another setup - a black window until the player reset that cache (#85)
    extern const char *rt_version(void);  // runtime/ui/launcher.mm
    unsigned vmaj = 0, vmin = 0, vpatch = 0, vpre = 0;
    sscanf(rt_version(), "%u.%u.%u-%*[a-z].%u", &vmaj, &vmin, &vpatch, &vpre);
    uint32_t ver = VK_MAKE_API_VERSION(0, vmaj, vmin, vpatch * 100 + vpre);
    char engine[64];  // (the release in the engine's name too: whichever of the two a driver keys its cache on)
    snprintf(engine, sizeof engine, "Savage Wind %s", rt_version());
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Savage Wind",
                             .applicationVersion = ver, .pEngineName = engine, .engineVersion = ver,
                             .apiVersion = VK_API_VERSION_1_3};
    LOG("[vk] application version %u.%u.%u (Savage Wind %s)\n", vmaj, vmin, vpatch * 100 + vpre, rt_version());
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
        LOG("[vk] queue family %u: flags %#x, queues %u, present %s, sparse binding %s\n", i, qp[i].queueFlags,
            qp[i].queueCount, present ? "yes" : "no",
            (qp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? "yes" : "no");
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i;
            g_vk.qfam_count = qp[i].queueCount;
            g_vk.sparse_queue = (qp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    { extern void perfmon_set_gpu_hint(const char *n); perfmon_set_gpu_hint(g_vk.props.deviceName); }
}

static void sparse_host_import_probe(void) {
    if (!g_vk.sparse_buffer || !g_vk.sparse_queue || !g_vk.host_import) {
        LOG("[vk] sparse host-import binding probe: unavailable (requires sparse buffer, sparse queue, and external host memory)\n");
        return;
    }

    VkPhysicalDeviceExternalBufferInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExternalBufferProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(g_vk.phys, &ext_info, &ext_props);
    if (!(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        LOG("[vk] sparse host-import binding probe: sparse storage buffers cannot import host memory on this driver\n");
        return;
    }

    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .size = 16 * 1024 * 1024,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_vk.dev, &buffer_info, NULL, &buffer);
    if (r != VK_SUCCESS) {
        LOG("[vk] sparse host-import binding probe: sparse buffer creation failed (%d)\n", r);
        return;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, buffer, &req);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_props};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    VkDeviceSize block_size = req.alignment;
    VkDeviceSize pointer_alignment = host_props.minImportedHostPointerAlignment;
    if (!block_size || block_size > SIZE_MAX || !pointer_alignment || pointer_alignment > SIZE_MAX) {
        LOG("[vk] sparse host-import binding probe: invalid block/alignment (%llu/%llu)\n",
            (unsigned long long)block_size, (unsigned long long)pointer_alignment);
        vkDestroyBuffer(g_vk.dev, buffer, NULL);
        return;
    }
    size_t alignment = (size_t)(block_size > pointer_alignment ? block_size : pointer_alignment);
    size_t allocation_size = (size_t)block_size;
#ifdef _WIN32
    void *host = _aligned_malloc(allocation_size, alignment);
#else
    void *host = NULL;
    if (posix_memalign(&host, alignment, allocation_size) != 0) host = NULL;
#endif
    if (!host) {
        LOG("[vk] sparse host-import binding probe: aligned host allocation failed (%llu bytes)\n",
            (unsigned long long)block_size);
        vkDestroyBuffer(g_vk.dev, buffer, NULL);
        return;
    }
    memset(host, 0, allocation_size);

    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT host_memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!get_host_props || get_host_props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                          &host_memory) != VK_SUCCESS) {
        LOG("[vk] sparse host-import binding probe: host pointer memory-type query failed\n");
        goto cleanup;
    }
    VkPhysicalDeviceMemoryProperties memory_props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &memory_props);
    uint32_t memory_type = UINT32_MAX;
    uint32_t compatible = req.memoryTypeBits & host_memory.memoryTypeBits;
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
        if (compatible & (1u << i)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        LOG("[vk] sparse host-import binding probe: no compatible memory type (buffer %#x, host %#x)\n",
            req.memoryTypeBits, host_memory.memoryTypeBits);
        goto cleanup;
    }

    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .pNext = &import,
                                  .allocationSize = block_size,
                                  .memoryTypeIndex = memory_type};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = vkAllocateMemory(g_vk.dev, &alloc, NULL, &memory);
    if (r != VK_SUCCESS) {
        LOG("[vk] sparse host-import binding probe: imported memory allocation failed (%d)\n", r);
        goto cleanup;
    }

    VkSparseMemoryBind bind = {.resourceOffset = 0, .size = block_size, .memory = memory, .memoryOffset = 0};
    VkSparseBufferMemoryBindInfo buffer_bind = {.buffer = buffer, .bindCount = 1, .pBinds = &bind};
    VkBindSparseInfo bind_info = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                                  .bufferBindCount = 1,
                                  .pBufferBinds = &buffer_bind};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    bool bind_submitted = false;
    r = vkCreateFence(g_vk.dev, &fence_info, NULL, &fence);
    if (r == VK_SUCCESS) {
        r = vkQueueBindSparse(g_vk.queue, 1, &bind_info, fence);
        bind_submitted = r == VK_SUCCESS;
    }
    if (r == VK_SUCCESS) r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX);
    if (bind_submitted && r != VK_SUCCESS) vkQueueWaitIdle(g_vk.queue);
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (r == VK_SUCCESS) {
        g_vk.sparse_host_import = true;
        LOG("[vk] sparse host-import binding probe: PASS (block %llu bytes, memory type %u)\n",
            (unsigned long long)block_size, memory_type);
    } else {
        LOG("[vk] sparse host-import binding probe: bind/wait failed (%d)\n", r);
    }
    vkDestroyBuffer(g_vk.dev, buffer, NULL);
    vkFreeMemory(g_vk.dev, memory, NULL);
    buffer = VK_NULL_HANDLE;
cleanup:
#ifdef _WIN32
    _aligned_free(host);
#else
    free(host);
#endif
    if (buffer) vkDestroyBuffer(g_vk.dev, buffer, NULL);
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

// RADV on a GCN chip (before RDNA): its name ends with the chip, "AMD Radeon RX 560 Series (RADV POLARIS11)"
static bool radv_gcn(void) {
    static const char *const gcn[] = {"TAHITI", "PITCAIRN", "VERDE", "OLAND", "HAINAN", "BONAIRE", "KAVERI", "KABINI",
                                      "MULLINS", "HAWAII", "TONGA", "ICELAND", "CARRIZO", "FIJI", "STONEY", "POLARIS",
                                      "VEGAM", "VEGA", "RAVEN", "RENOIR", "GFX8", "GFX9"};
    const char *r = strstr(g_vk.props.deviceName, "(RADV ");
    if (!r) return false;
    for (size_t i = 0; i < sizeof gcn / sizeof *gcn; i++)
        if (!strncmp(r + 6, gcn[i], strlen(gcn[i]))) return true;
    return false;
}

static void create_device(void) {
    uint32_t ne = 0;
    LOG("[vk] sparse heap %s (SAVAGEWIND_VK_SPARSE_HEAP)\n",
        plat_guest_sparse_heap_enabled() ? "enabled" : "disabled");
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    VkPhysicalDeviceFeatures2 sparse_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &sparse_features);
    LOG("[vk] sparse buffer capability: sparseBinding %s, sparseResidencyBuffer %s, address space %llu MB\n",
        sparse_features.features.sparseBinding ? "yes" : "no",
        sparse_features.features.sparseResidencyBuffer ? "yes" : "no",
        (unsigned long long)(g_vk.props.limits.sparseAddressSpaceSize >> 20));
#ifdef VK_EXT_external_memory_host
    if (has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
        LOG("[vk] external host memory import: supported, minimum pointer alignment %llu bytes\n",
            (unsigned long long)host_props.minImportedHostPointerAlignment);
    } else {
        LOG("[vk] external host memory import: not supported\n");
    }
#endif
    {
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &memory);
        for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
            LOG("[vk] memory heap %u: %llu MB, flags %#x\n", i,
                (unsigned long long)(memory.memoryHeaps[i].size >> 20), memory.memoryHeaps[i].flags);
        }
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
            LOG("[vk] memory type %u: heap %u, flags %#x\n", i,
                memory.memoryTypes[i].heapIndex, memory.memoryTypes[i].propertyFlags);
        }
    }
    const char *ext[24];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    const char *memory_priority_env = getenv("SAVAGEWIND_VK_MEMORY_PRIORITY");
    bool request_memory_priority = memory_priority_env && !strcmp(memory_priority_env, "1");

#ifdef VK_EXT_memory_budget
    g_vk.memory_budget =
        request_memory_priority &&
        has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    if (g_vk.memory_budget) {
        ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    }

    LOG("[vk] memory budget extension %s\n",
        !request_memory_priority ? "off" :
        g_vk.memory_budget ? "on" : "not supported");
#endif
#ifdef VK_EXT_memory_priority
    if (request_memory_priority && has_ext(ep, ne, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME)) {
        VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_have = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 priority_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &priority_have};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &priority_query);
        g_vk.memory_priority = priority_have.memoryPriority;
        if (g_vk.memory_priority) ext[n_ext++] = VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME;
    }
    const char *memory_priority_status = g_vk.memory_priority ? "on" :
                                        request_memory_priority ? "not supported" :
                                                                  "off (set SAVAGEWIND_VK_MEMORY_PRIORITY=1 to enable)";
    LOG("[vk] memory priority %s\n", memory_priority_status);
#endif
#ifdef VK_EXT_pageable_device_local_memory
    if (plat_guest_sparse_heap_enabled() &&
            has_ext(ep, ne, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME)) {
            VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_have = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 pageable_query = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &pageable_have};
            vkGetPhysicalDeviceFeatures2(g_vk.phys, &pageable_query);
            g_vk.pageable_device_local_memory = pageable_have.pageableDeviceLocalMemory;
            if (g_vk.pageable_device_local_memory) ext[n_ext++] = VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME;
        }
    LOG("[vk] pageable device-local memory %s\n",
        !plat_guest_sparse_heap_enabled() ? "off" :
        g_vk.pageable_device_local_memory ? "on" : "not supported");
#endif
    bool addr_report = g_addr_report_hi && has_ext(ep, ne, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
    if (addr_report) ext[n_ext++] = VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME;
    if (g_addr_report_hi) LOG("[vk] address binding report %s\n", addr_report ? "on" : "not supported");
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
#ifdef __linux__
    // the guest heap as GPU memory mapped at the heap's addresses (exec.c backend_heap_set)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
#endif
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    {
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    }
#ifdef __linux__
    g_vk.radv = drv.driverID == VK_DRIVER_ID_MESA_RADV;
#endif
    // SAVAGEWIND_VK_LEGACY=1: Alpha 2's setup, none of maintenance5, graphics pipeline libraries and attachment
    // feedback loops (SAVAGEWIND_VK_M5=1 / _GPL=1 / _FEEDBACK_LOOP=1 turn one back on). The 0.3.1 hotfix made it the
    // default on AMD's and Intel's Windows drivers, for a black window from Alpha 3 on (#85); that was their shader
    // caches, from another release or another setup (an RX 9070 XT and a ROG Ally run with all three once the caches
    // are cleared), which each release and setup now keeps apart. Without the three the ROG Ally ran at 12 fps.
    bool win_legacy = getenv("SAVAGEWIND_VK_LEGACY");
    if (win_legacy) LOG("[vk] %s: maintenance5, pipeline libraries and feedback loops left off\n", drv.driverName);
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // Point lists drawn with vertex shaders that write no point size (the game has them): without maintenance5 the
    // size is undefined, and on RADV with graphics pipeline libraries the GPU hung within the first frames (Steam Deck).
    // With it the size is 1.0, as on the Switch.
    bool m5 = false;
    if (has_ext(ep, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME) && !(win_legacy && !getenv("SAVAGEWIND_VK_M5"))) {
        VkPhysicalDeviceMaintenance5FeaturesKHR m5_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &m5_have};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        m5 = m5_have.maintenance5;
    }
    if (m5) ext[n_ext++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    g_m5 = m5;
    LOG("[vk] maintenance5 %s\n", m5 ? "on (unwritten point size is 1.0)" : win_legacy ? "off" : "not supported");
    // Color targets a draw samples while it writes them (exec.c resolve_feedback): a feedback loop with a barrier
    // inside the pass between such draws, instead of a copy of the target before each one. With
    // VK_KHR_unified_image_layouts the target stays in GENERAL layout. Without it (RADV, AMD's Windows driver; Vulkan
    // headers before 1.4.321) it is in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL for the passes that loop (g_vk.feedback_layout,
    // exec.c begin_pass): on the Steam Deck the copies were half of the GPU's frame. SAVAGEWIND_VK_NO_FEEDBACK_LOOP=1:
    // copies, as before; SAVAGEWIND_VK_FEEDBACK_LAYOUT=1: the layout path where unified layouts exist too (testing).
    // RADV on GCN chips (Polaris, Vega and older) drew the loops wrong: the pause menus' blurred background came out
    // black and their cursor off its slot (RX 560, issue #90); RDNA (the Steam Deck, a 660M) is fine. Those take the
    // copies; SAVAGEWIND_VK_FEEDBACK_LOOP=1 turns the loops on there anyway.
    bool gcn_off = (radv_gcn() || win_legacy) && !getenv("SAVAGEWIND_VK_FEEDBACK_LOOP");
    bool fbl = !getenv("SAVAGEWIND_VK_NO_FEEDBACK_LOOP") && !gcn_off &&
               has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
               has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
#ifdef VK_KHR_unified_image_layouts
    bool uil = fbl && !getenv("SAVAGEWIND_VK_FEEDBACK_LAYOUT") && has_ext(ep, ne, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
#else
    bool uil = false;
#endif
    // Pipelines linked from shaders compiled apart from the fixed state (exec.c, graphics pipeline libraries), when
    // the driver links them fast. SAVAGEWIND_VK_NO_GPL=1: whole pipelines, as before.
    bool gpl = !getenv("SAVAGEWIND_VK_NO_GPL") && !(win_legacy && !getenv("SAVAGEWIND_VK_GPL")) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
               has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    // when each frame reached the display (SAVAGEWIND_LOG_PRESENT, tests: "present timing" below)
    bool pwait = getenv("SAVAGEWIND_LOG_PRESENT") && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                 has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .pNext = &fbl_have};
#ifdef VK_KHR_unified_image_layouts
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
                                                               .pNext = &fbl_have};
    if (uil) lr_have.pNext = &uil_have;
#endif
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = fbl ? &lr_have : NULL};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                      .pNext = gpl ? (void *)&gpl_have : fbl ? (void *)&lr_have : NULL};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    g_vk.sparse_buffer = false;
    g_vk.sparse_nonresident_strict = false;

    if (plat_guest_sparse_heap_enabled()) {
        g_vk.sparse_buffer =
            have.features.sparseBinding &&
            have.features.sparseResidencyBuffer;

        g_vk.sparse_nonresident_strict =
            g_vk.props.sparseProperties.residencyNonResidentStrict;
    }
    LOG("[vk] sparse buffer feature enablement %s; nonresident strict %s\n",
        g_vk.sparse_buffer && g_vk.sparse_queue ? "available on selected queue" : "unavailable on selected queue/device",
        g_vk.sparse_nonresident_strict ? "yes" : "no");
    if (gpl) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.gpl = gpl_have.graphicsPipelineLibrary && gp.graphicsPipelineLibraryFastLinking;
    }
    if (g_vk.gpl) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    LOG("[vk] graphics pipeline libraries %s\n", g_vk.gpl ? "on (shaders compiled apart from the fixed state)" : "off");
    g_vk.feedback_loop = fbl && fbl_have.attachmentFeedbackLoopLayout && lr_have.dynamicRenderingLocalRead;
#ifdef VK_KHR_unified_image_layouts
    uil = uil && uil_have.unifiedImageLayouts;
#endif
    g_vk.feedback_layout = g_vk.feedback_loop && !uil;
    if (g_vk.feedback_loop) {
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
#ifdef VK_KHR_unified_image_layouts
        if (uil) ext[n_ext++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
#endif
    }
    LOG("[vk] attachment feedback loops %s\n", gcn_off ? win_legacy ? "off (target copies)" : "off on this GCN chip (target copies)"
                                               : !g_vk.feedback_loop ? "off (target copies)"
                                               : g_vk.feedback_layout ? "on (feedback loop layout)" : "on");
    // What the driver has of the features asked for below: only those are asked for (a driver refuses the device for
    // one it lacks, VK_ERROR_FEATURE_NOT_PRESENT: Turnip on an Adreno 830, #207), and each one missing is logged
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = g_vk.divisor ? &div_have : NULL};
    VkPhysicalDeviceFaultFeaturesEXT ff_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
                                                .pNext = g_vk.custom_border ? (void *)&cbc_have
                                                         : g_vk.divisor ? (void *)&div_have : NULL};
    VkPhysicalDeviceVulkan13Features f13_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                 .pNext = fault ? &ff_have : ff_have.pNext};
    VkPhysicalDeviceVulkan12Features f12_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13_have};
    VkPhysicalDeviceVulkan11Features f11_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12_have};
    VkPhysicalDeviceFeatures2 have2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11_have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have2);
#define WANT(have, name) (have.name ? VK_TRUE : (LOG("[vk] warning: the driver lacks " #name "\n"), VK_FALSE))
    if (fault && !ff_have.deviceFault) fault = g_fault_ext = false;
    if (g_vk.custom_border && !(cbc_have.customBorderColors && cbc_have.customBorderColorWithoutFormat)) {
        LOG("[vk] custom border colors without a format not supported: standard border colors\n");
        g_vk.custom_border = false;
    }
    if (g_vk.divisor && !div_have.vertexAttributeInstanceRateDivisor) {
        LOG("[vk] vertex attribute divisors not supported\n");
        g_vk.divisor = false;
    }
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
                                                     .maintenance5 = VK_TRUE,
                                                     .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = WANT(f13_have, dynamicRendering),
                                            .synchronization2 = WANT(f13_have, synchronization2),
                                            .shaderDemoteToHelperInvocation = WANT(f13_have, shaderDemoteToHelperInvocation),
                                            .shaderTerminateInvocation = WANT(f13_have, shaderTerminateInvocation),
                                            .maintenance4 = WANT(f13_have, maintenance4),
                                            .pipelineCreationCacheControl = g_vk.gpl && f13_have.pipelineCreationCacheControl,
                                            .pNext = m5 ? (void *)&m5_on : g_vk.custom_border ? (void *)&cbc
                                                                         : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = WANT(f12_have, timelineSemaphore),
                                            .shaderInt8 = WANT(f12_have, shaderInt8),
                                            .samplerMirrorClampToEdge = WANT(f12_have, samplerMirrorClampToEdge),
                                            .uniformAndStorageBuffer8BitAccess = WANT(f12_have, uniformAndStorageBuffer8BitAccess),
                                            .storageBuffer8BitAccess = WANT(f12_have, storageBuffer8BitAccess),
                                            .scalarBlockLayout = WANT(f12_have, scalarBlockLayout), .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = WANT(f11_have, storageBuffer16BitAccess), .pNext = &f12};
#undef WANT
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceAddressBindingReportFeaturesEXT abr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
                                                           .reportAddressBinding = VK_TRUE, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, .attachmentFeedbackLoopLayout = VK_TRUE,
        .pNext = addr_report ? (void *)&abr : fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .dynamicRenderingLocalRead = VK_TRUE,
        .pNext = &fbl_on};
#ifdef VK_KHR_unified_image_layouts
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
                                                             .unifiedImageLayouts = VK_TRUE, .pNext = &fbl_on};
    if (uil) lr_on.pNext = &uil_on;
#endif
    void *chain = g_vk.feedback_loop ? (void *)&lr_on : addr_report ? (void *)&abr : fault ? (void *)&ff : (void *)&f11;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE,
        .pNext = chain};
    void *next_feature = g_vk.gpl ? (void *)&gpl_on : chain;
#ifdef VK_EXT_pageable_device_local_memory
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,
        .pageableDeviceLocalMemory = VK_TRUE, .pNext = next_feature};
    if (g_vk.pageable_device_local_memory) next_feature = &pageable_on;
#endif
    VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, .memoryPriority = VK_TRUE,
        .pNext = next_feature};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = g_vk.memory_priority ? (void *)&priority_on : next_feature};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    g_vk.feat_aniso = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->occlusionQueryPrecise = h->occlusionQueryPrecise;
    g_vk.occlusion_precise = h->occlusionQueryPrecise;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    g_vk.wide_lines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    b->sparseBinding = g_vk.sparse_buffer && g_vk.sparse_queue;
    b->sparseResidencyBuffer = b->sparseBinding;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    VkPhysicalDevicePresentWaitFeaturesKHR pw_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR pid_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                     .pNext = &pw_have};
    if (pwait) {
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pid_have};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        pwait = pid_have.presentId && pw_have.presentWait;
    }
    VkPhysicalDevicePresentWaitFeaturesKHR pw_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .presentWait = VK_TRUE, .pNext = f.pNext};
    VkPhysicalDevicePresentIdFeaturesKHR pid_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                   .presentId = VK_TRUE, .pNext = &pw_on};
    if (pwait) {
        f.pNext = &pid_on;
        ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    }
    g_vk.present_wait = pwait;
    float prio[2] = {1.0f, 1.0f};
    // two queues when the family has them: the second one presents (present thread below)
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = g_vk.qfam_count >= 2 ? 2 : 1, .pQueuePriorities = prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    g_vk.present_queue = g_vk.queue;
    if (qi.queueCount == 2) vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 1, &g_vk.present_queue);
    if (plat_guest_sparse_heap_enabled()) {
        sparse_host_import_probe();
    }

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef VK_EXT_memory_budget
    if (g_vk.memory_budget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
#endif
#ifdef VK_EXT_memory_priority
    if (g_vk.memory_priority) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
#endif
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
// cache/vk_precompiled.bin (with graphics pipeline libraries): the libraries of every shader of the game, made by the
// precompilation (precompile.c) and only read while playing - ~0.85 GB that each save of vk_pipelines.bin would
// otherwise write again. exec.c looks a shader's library up there first (failing instead of compiling when it is not
// there) and makes it into pcache when missing. Same header and checks.
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
    // and the device setup the pipelines were built for: maintenance5, pipeline libraries, feedback loops (in the
    // layout or not). Pipelines of one setup read back in another hung AMD's Windows driver at the first frame (#85).
    snprintf(h->shaders, sizeof h->shaders, "%s/%c%c%c", vk_shader_version(), g_m5 ? 'm' : '-', g_vk.gpl ? 'g' : '-',
             !g_vk.feedback_loop ? '-' : g_vk.feedback_layout ? 'L' : 'f');
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
    if (memcmp(h.shaders, want.shaders, sizeof h.shaders)) { *why = "made by another shader translator build or device setup"; return NULL; }
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

// A pipeline cache with the data of the file name in the cache folder: empty when there is none or it is no good
// (*why says why), NULL with open_only and no data
static VkPipelineCache pc_open(const char *name, bool load, bool open_only, char *path, size_t npath, size_t *size, const char **why) {
    vk_cache_path(path, npath, name);
    *size = 0;
    void *data = NULL;
    FILE *fp = load ? fopen(path, "rb") : NULL;
    if (fp) {
        *why = NULL;
        data = pc_read(fp, size, why);
        fclose(fp);
    }
    if (!data && open_only) return VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = data ? *size : 0,
                                    .pInitialData = data};
    VkPipelineCache c = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineCache(g_vk.dev, &pc, NULL, &c);
    if (r != VK_SUCCESS && data) {
        *why = "rejected by the driver";
        free(data);
        data = NULL;
        if (open_only) return VK_NULL_HANDLE;
        pc.initialDataSize = 0;
        pc.pInitialData = NULL;
        r = vkCreatePipelineCache(g_vk.dev, &pc, NULL, &c);
    }
    VK_CHECK(r);
    if (!data) *size = 0;
    free(data);
    return c;
}

static void pc_log(const char *what, const char *path, size_t sz, const char *why) {
    if (sz) LOG("[vk] %s: %s, %.1f MB%s%s%s\n", what, path, sz / 1048576.0, why ? " (" : "", why ? why : "", why ? ")" : "");
    else LOG("[vk] %s: starts empty (%s: %s)\n", what, path, why);
}

static void pipeline_cache_load(void) {
    pc_off = getenv("SAVAGEWIND_VK_NO_PIPELINE_CACHE") != NULL;
    char path[512];
    size_t sz;
    const char *why = pc_off ? "off (SAVAGEWIND_VK_NO_PIPELINE_CACHE)" : "no file yet";
    g_vk.pcache = pc_open("vk_pipelines.bin", !pc_off, false, path, sizeof path, &sz, &why);
    pc_log("pipeline cache", path, sz, why);
    if (g_vk.gpl && !pc_off) {
        why = "no file yet";
        g_vk.pcache_pre = pc_open("vk_precompiled.bin", true, true, path, sizeof path, &sz, &why);
        if (g_vk.pcache_pre) pc_log("precompiled shaders", path, sz, why);
        else LOG("[vk] precompiled shaders: none (%s: %s)\n", path, why);
    }
    pc_saved_at = SDL_GetTicks();
}

// precompile.c: the cache the shader libraries go into (made empty when there is none yet), NULL when they are not
// kept (no graphics pipeline libraries, or SAVAGEWIND_VK_NO_PIPELINE_CACHE)
VkPipelineCache vk_precompiled_cache(void) {
    if (!g_vk.gpl || pc_off) return VK_NULL_HANDLE;
    pthread_mutex_lock(&pc_save_lock);
    if (!g_vk.pcache_pre) {
        VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache_pre));
    }
    pthread_mutex_unlock(&pc_save_lock);
    return g_vk.pcache_pre;
}

// precompile.c: whether vk_precompiled.bin is there for this GPU, driver and shader translator (its header only)
bool vk_precompiled_cache_ok(void) {
    if (!g_vk.dev || !g_vk.gpl || pc_off) return true;
    char path[512];
    vk_cache_path(path, sizeof path, "vk_precompiled.bin");
    FILE *f = fopen(path, "rb");
    PcHeader h, want;
    pc_header(&want);
    bool ok = f && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, want.magic, 8) && h.version == want.version &&
              h.vendor == want.vendor && h.device == want.device && h.driver == want.driver &&
              !memcmp(h.uuid, want.uuid, VK_UUID_SIZE) && !memcmp(h.shaders, want.shaders, sizeof h.shaders) && h.size;
    if (f) fclose(f);
    return ok;
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

// The cache's data into the file name in the cache folder, through a temporary file
static bool pc_write(VkPipelineCache c, const char *name, size_t *size) {
    PcHeader h;
    pc_header(&h);
    size_t sz = 0;
    uint8_t *buf = NULL;
    VkResult r = VK_INCOMPLETE;
    for (int tries = 0; tries < 4 && r == VK_INCOMPLETE; tries++) {  // (pipelines made meanwhile make it grow)
        if (vkGetPipelineCacheData(g_vk.dev, c, &sz, NULL) != VK_SUCCESS || !sz) break;
        sz += sz / 16;
        free(buf);
        buf = malloc(sizeof h + sz);
        if (!buf) break;
        r = vkGetPipelineCacheData(g_vk.dev, c, &sz, buf + sizeof h);
    }
    char path[512], tmp[540];
    vk_cache_path(path, sizeof path, name);
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
    *size = sz;
    return ok;
}

void vk_pipeline_cache_save(void) {
    if (pc_off || !g_vk.pcache) return;
    pthread_mutex_lock(&pc_save_lock);
    int news = atomic_exchange(&pc_new, 0);
    if (!news) { pthread_mutex_unlock(&pc_save_lock); return; }
    uint64_t t0 = SDL_GetTicksNS();
    size_t sz;
    bool ok = pc_write(g_vk.pcache, "vk_pipelines.bin", &sz);
    if (!ok) atomic_fetch_add(&pc_new, news);  // (try again later)
    pc_saved_at = SDL_GetTicks();
    LOG("[vk] pipeline cache %s: %.1f MB, %d new pipelines, %.0f ms\n", ok ? "saved" : "NOT saved", sz / 1048576.0, news,
        (SDL_GetTicksNS() - t0) / 1e6);
    pthread_mutex_unlock(&pc_save_lock);
}

// exec.c, after each library made in the precompiled shaders' cache: hit = it had it
static _Atomic int pre_new;
void vk_precompiled_cache_note(bool hit) {
    if (!hit) atomic_fetch_add(&pre_new, 1);
}

// precompile.c, at its end (not written again when nothing in it is new)
bool vk_precompiled_cache_save(void) {
    if (pc_off || !g_vk.pcache_pre) return false;
    if (!atomic_exchange(&pre_new, 0) && vk_precompiled_cache_ok()) return true;
    pthread_mutex_lock(&pc_save_lock);
    uint64_t t0 = SDL_GetTicksNS();
    size_t sz;
    bool ok = pc_write(g_vk.pcache_pre, "vk_precompiled.bin", &sz);
    LOG("[vk] precompiled shaders %s: %.1f MB, %.0f ms\n", ok ? "saved" : "NOT saved", sz / 1048576.0, (SDL_GetTicksNS() - t0) / 1e6);
    pthread_mutex_unlock(&pc_save_lock);
    return ok;
}

static void *pc_saver(void *arg) {
    (void)arg;
    for (;;) {
        SDL_Delay(1000);
        extern void vk_pso_flush(void);
        vk_pso_flush();  // (exec.c: the pipeline list's new records)
        extern void vk_keylog_flush(void);
        vk_keylog_flush();  // (shaders.c: the shader key log's)
        if (!atomic_load(&pc_new)) continue;
        uint64_t now = SDL_GetTicks(), quiet = now - atomic_load(&pc_new_at), since = now - pc_saved_at;
        if ((quiet >= 3000 && since >= 30000) || since >= 120000) vk_pipeline_cache_save();
    }
    return NULL;
}

// ================================================================== swapchain
// The present mode of Video > VSync (settings.h VSYNC_*): On FIFO, Fast MAILBOX, Adaptive FIFO_RELAXED, Off
// IMMEDIATE. SAVAGEWIND_VK_PRESENT=fifo|mailbox|relaxed|immediate overrides it (tests). Fast is the default: the
// runtime paces the frames itself (runtime/hle/framepace.c), and with FIFO a compositor that slows down (window
// captured, covered, busy) slows the game with it (4-21 fps with the GPU mostly idle, before the present thread).
// With G-SYNC / FreeSync, On is the one that lets the display follow the game without tearing (framepace.c keeps the
// frame rate a little under the refresh rate then).
static VkPresentModeKHR wanted_present_mode(void) {
    static const VkPresentModeKHR by_vsync[VSYNC_COUNT] = {
        VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR};
    const char *e = getenv("SAVAGEWIND_VK_PRESENT");
    if (e && *e)
        return !strcmp(e, "mailbox")     ? VK_PRESENT_MODE_MAILBOX_KHR
             : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
             : !strcmp(e, "relaxed")   ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                       : VK_PRESENT_MODE_FIFO_KHR;
    int v = g_settings.vsync;
    return by_vsync[v >= 0 && v < VSYNC_COUNT ? v : VSYNC_DEFAULT];
}

static const char *present_mode_name(VkPresentModeKHR m) {
    return m == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
         : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo relaxed" : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
         : "other";
}

// The wanted mode, or what comes closest on this surface: Adaptive -> On, Off -> Fast (no waiting either), Fast -> On
// or Immediate (surfaces without mailbox — Intel's have fifo and immediate — must not land on FIFO: it would queue
// every present on the display's refresh and fight framepace's own pacing; 120 on a 160 Hz display then judders,
// one long wait every few seconds). *have: the surface's modes, for the log.
static VkPresentModeKHR present_mode(char *have, size_t cap) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    size_t k = 0;
    have[0] = 0;
    for (uint32_t i = 0; i < n && k < cap; i++)
        k += (size_t)snprintf(have + k, cap - k, "%s%s", i ? ", " : "", present_mode_name(m[i]));
    // (VkPresentModeKHR is an enum whose IMMEDIATE is 0: the terminator cannot be 0)
    static const VkPresentModeKHR NONE = (VkPresentModeKHR)-1;
    static const VkPresentModeKHR pref[VSYNC_COUNT][3] = {
        {VK_PRESENT_MODE_FIFO_KHR, NONE, NONE},                               // on
        {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, NONE},   // fast: nothing queued behind vsync
        {VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR, NONE},   // adaptive -> on
        {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, NONE}};  // off -> fast
    const char *env = getenv("SAVAGEWIND_VK_PRESENT");
    VkPresentModeKHR forced = wanted_present_mode();
    const VkPresentModeKHR one_shot[2] = {forced, NONE};  // the env override is a single mode, FIFO behind it
    int v = g_settings.vsync;
    const VkPresentModeKHR *ladder = (env && *env) ? one_shot : pref[v >= 0 && v < VSYNC_COUNT ? v : VSYNC_DEFAULT];
    for (int j = 0; ladder[j] != NONE; j++)
        for (uint32_t i = 0; i < n; i++)
            if (m[i] == ladder[j]) return ladder[j];
    return VK_PRESENT_MODE_FIFO_KHR;  // the one every surface has
}

// ================================================================== HDR (Video > HDR)
// While Windows HDR is on, the desktop is composed in HDR and an SDR swapchain is shown as SDR content: dim, with dull
// colours, on many displays (issue #95). So while it is on for the window's display (SDL's HDR state of the window) or
// Video > HDR is On, the swapchain is HDR10 (BT.2020 primaries, PQ): everything is still drawn into 8-bit images as
// before (g_vk.sc_images are those then), and vk_frame_end writes the frame into the swapchain image in one pass:
// sRGB-encoded to linear (gamma 2.2, as SDR displays show it; SAVAGEWIND_HDR_SRGB=1: the piecewise sRGB curve), BT.709
// to BT.2020, scaled to SDR white (the brightness Windows gives SDR content on that display, which SDL reports;
// hdr_paper_white in the ini fixes it), PQ-encoded. Off, or where the display can't take it: SDR as before.
static bool g_hdr;                    // the swapchain is HDR10
static bool hdr_failed;               // HDR was wanted when it was made, but the display has no HDR10 format
static float g_hdr_white = 200;       // nits SDR white is shown at
static VkFormat hdr_format;
static VkImage hdr_sc_images[8];      // the swapchain's own images while g_hdr
static VkImageView hdr_sc_views[8];
static VmaAllocation hdr_mem[8];      // g_vk.sc_images' memory while g_hdr
static uint32_t hdr_count;
static VkDescriptorSetLayout hdr_dsl;
static VkPipelineLayout hdr_layout;
static VkPipeline hdr_pipe;
static VkDescriptorPool hdr_pool;
static VkDescriptorSet hdr_sets[8];

// Video > HDR as it stands now: whether the swapchain should be HDR10, and SDR white's nits
static bool hdr_wanted(float *white) {
    SDL_PropertiesID p = SDL_GetWindowProperties(window);
    bool on = SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    float w = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 0) * 80;  // (scRGB: 1.0 is 80 nits)
    if (g_settings.hdr == HDR_ON) on = true;
    if (g_settings.hdr == HDR_OFF) on = false;
    *white = g_settings.hdr_white ? (float)g_settings.hdr_white : w >= 80 ? w : 200;
    return on && g_cs_ext;
}

static const char k_hdr_vs[] =
    "#version 450\n"
    "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char k_hdr_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { float white; float srgb; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec3 c = clamp(texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb, 0.0, 1.0);\n"
    "    vec3 l = pc.srgb > 0.5 ? mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)) : pow(c, vec3(2.2));\n"
    "    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
    "    vec3 y = pow(clamp(to2020 * l * (pc.white / 10000.0), 0.0, 1.0), vec3(0.1593017578125));\n"
    "    o = vec4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375)), 1.0);\n"
    "}\n";

static bool hdr_pipeline(VkFormat fmt) {
    if (hdr_pipe && fmt == hdr_format) return true;
    if (hdr_pipe) vkDestroyPipeline(g_vk.dev, hdr_pipe, NULL), hdr_pipe = VK_NULL_HANDLE;
    if (!hdr_dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
                                              .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &hdr_dsl));
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &hdr_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &hdr_layout));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8,
                                         .poolSizeCount = 1, .pPoolSizes = &ps};
        VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &hdr_pool));
        VkDescriptorSetLayout l8[8] = {hdr_dsl, hdr_dsl, hdr_dsl, hdr_dsl, hdr_dsl, hdr_dsl, hdr_dsl, hdr_dsl};
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = hdr_pool,
                                          .descriptorSetCount = 8, .pSetLayouts = l8};
        VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, hdr_sets));
    }
    VkShaderModule vs = vk_compile_glsl(k_hdr_vs, STAGE_VERTEX, "hdr.vert"), fs = vk_compile_glsl(k_hdr_fs, STAGE_FRAGMENT, "hdr.frag");
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                                .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = 1, .pAttachments = &ba};
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                               .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &fmt};
        VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rc, .stageCount = 2,
                                           .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                           .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                           .pColorBlendState = &cb, .pDynamicState = &ds, .layout = hdr_layout};
        if (vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gi, NULL, &hdr_pipe) != VK_SUCCESS) hdr_pipe = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    hdr_format = fmt;
    if (!hdr_pipe) LOG("[vk] HDR output pass unavailable: SDR\n");
    return hdr_pipe != VK_NULL_HANDLE;
}

// The 8-bit images drawn into while the swapchain is HDR10 (and their views), gone with the swapchain they served
static void hdr_release(void) {
    for (uint32_t i = 0; i < hdr_count; i++) {
        vkDestroyImageView(g_vk.dev, hdr_sc_views[i], NULL);
        vmaDestroyImage(g_vk.vma, g_vk.sc_images[i], hdr_mem[i]);
    }
    hdr_count = 0;
}

static void hdr_make_images(uint32_t n, VkExtent2D ext) {
    for (uint32_t i = 0; i < n; i++) {
        hdr_sc_images[i] = g_vk.sc_images[i];  // (the swapchain's)
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hdr_sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = hdr_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &hdr_sc_views[i]));
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {ext.width, ext.height, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &g_vk.sc_images[i], &hdr_mem[i], NULL));
    }
    hdr_count = n;
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
    VkSurfaceFormatKHR draw = sf;  // what the frame is drawn into (the swapchain's format, or the 8-bit images' in HDR)
    bool hdr = hdr_wanted(&g_hdr_white), hdr_found = false;
    for (uint32_t i = 0; hdr && i < nf && !hdr_found; i++)
        if ((fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && hdr_pipeline(fmts[i].format))
            sf = fmts[i], hdr_found = true;
    hdr_failed = hdr && !hdr_found;
    if (hdr && !hdr_found) LOG("[vk] HDR wanted, but this display offers no HDR10 swapchain format: SDR\n");
    if (hdr_found) draw.format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t count = caps.minImageCount + 2;  // (+1, and one for frame generation's queued pair: present_wait_room)
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    char modes[128];
    VkPresentModeKHR mode = present_mode(modes, sizeof modes);
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = old};
    // (what decides whether the display can follow the game: the window, the mode, the display's refresh rate)
    SDL_WindowFlags wf = SDL_GetWindowFlags(window);
    const char *kind = !(wf & SDL_WINDOW_FULLSCREEN)   ? "window"
                     : SDL_GetWindowFullscreenMode(window) ? "exclusive fullscreen (its own display mode)"
                                                          : "borderless fullscreen (the desktop's mode)";
    LOG("[vk] swapchain %ux%u, %u images, present mode %s (VSync %s; surface has %s), %s, display %.2f Hz\n",
        ext.width, ext.height, count, present_mode_name(mode), present_mode_name(wanted_present_mode()), modes, kind,
        winsize_refresh_rate());
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    hdr_release();
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = draw.format;
    g_vk.sc_extent = ext;
    g_vk.sc_mode = wanted_present_mode();  // (what was asked: a change of Video > VSync makes it again)
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    g_hdr = hdr_found;
    if (g_hdr) hdr_make_images(g_vk.sc_count, ext);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = draw.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
        if (!g_hdr) continue;
        VkDescriptorImageInfo ii = {vk_default_sampler(false), g_vk.sc_views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hdr_sets[i], .dstBinding = 0,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &ii};
        vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    }
    if (g_hdr) LOG("[vk] HDR10 output (%s), SDR white at %.0f nits\n",
                   g_settings.hdr == HDR_ON ? "Video > HDR is On" : "Windows HDR is on for this display", g_hdr_white);
}

// While the swapchain is HDR10: the frame drawn into image i (8-bit, sRGB-encoded) written into the swapchain's image
static void hdr_write(VkCommandBuffer cb, uint32_t i);

// ================================================================== present timing
// SAVAGEWIND_LOG_PRESENT=<frames> (tests): every <frames> presents, a "[present]" line with how evenly the frames went
// to the display: when vkQueuePresentKHR was called and, with VK_KHR_present_wait, when each frame was shown (a
// thread of its own waits for each present id). Per series: the mean time between frames, its standard deviation,
// the 99th percentile of how far one is from the median, and the share 2 ms or more off; and present to shown.
// SAVAGEWIND_PRESENT_CSV=<file> also writes every present there (pt_report).
#define PT_RING 2048
static struct { uint64_t call, shown; } pt[PT_RING];
static pthread_mutex_t pt_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t pt_id;           // the last present's id (1, 2, ...; one sequence across swapchains)
static uint64_t pt_next;         // the next id the waiter waits for
static uint64_t pt_from;         // the first id of the current report
static VkSwapchainKHR pt_sc;     // the swapchain those presents went to (NULL while it is being made again)
static bool pt_waiting;          // the waiter is inside vkWaitForPresentKHR
static int pt_every = -1;

static int pt_cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// "every 16.67 ms (sd 0.21, p99 off 0.80, 0.4% >= 2 ms off)" for the timestamps t[0..n) (0: unknown, left out)
static void pt_series(const uint64_t *t, int n, char *out, size_t cap) {
    static double d[PT_RING], dev[PT_RING];
    int k = 0;
    for (int i = 1; i < n; i++)
        if (t[i] && t[i - 1] && t[i] >= t[i - 1]) d[k++] = (t[i] - t[i - 1]) / 1e6;
    if (k < 2) { snprintf(out, cap, "-"); return; }
    double sum = 0, sq = 0;
    for (int i = 0; i < k; i++) sum += d[i], sq += d[i] * d[i];
    double mean = sum / k, sd = sqrt(fmax(0, sq / k - mean * mean));
    memcpy(dev, d, k * sizeof *d);
    qsort(dev, k, sizeof *dev, pt_cmp);
    double med = dev[k / 2];
    int off = 0;
    for (int i = 0; i < k; i++) dev[i] = fabs(d[i] - med), off += dev[i] >= 2.0;
    qsort(dev, k, sizeof *dev, pt_cmp);
    snprintf(out, cap, "every %.2f ms (sd %.2f, p99 off %.2f, %.1f%% >= 2 ms off)", mean, sd, dev[k * 99 / 100],
             100.0 * off / k);
}

// (pt_m held) the report for presents pt_from .. last
static void pt_report(uint64_t last) {
    static uint64_t call[PT_RING], shown[PT_RING];
    int n = 0;
    double lat = 0, lat2 = 0;
    int nl = 0;
    for (uint64_t id = pt_from; id <= last && n < PT_RING; id++, n++) {
        call[n] = pt[id % PT_RING].call, shown[n] = pt[id % PT_RING].shown;
        if (shown[n] && call[n] && shown[n] >= call[n]) {
            double l = (shown[n] - call[n]) / 1e6;
            lat += l, lat2 += l * l, nl++;
        }
    }
    static FILE *csv;  // SAVAGEWIND_PRESENT_CSV=<file>: every present, "id,presented ns,shown ns,target fps"
    static int csv_open;
    if (!csv_open++ && getenv("SAVAGEWIND_PRESENT_CSV")) csv = fopen(getenv("SAVAGEWIND_PRESENT_CSV"), "w");
    for (int i = 0; csv && i < n; i++)
        fprintf(csv, "%llu,%llu,%llu,%d\n", (unsigned long long)(pt_from + i), (unsigned long long)call[i],
                (unsigned long long)shown[i], framepace_target());
    if (csv) fflush(csv);
    pt_from = last + 1;
    char a[160], b[160] = "-";
    pt_series(call, n, a, sizeof a);
    if (g_vk.present_wait) pt_series(shown, n, b, sizeof b);
    double lm = nl ? lat / nl : 0, lsd = nl ? sqrt(fmax(0, lat2 / nl - lm * lm)) : 0;
    LOG("[present] %s, target %d fps, %d frames: presented %s; shown %s; present to shown %.2f ms (sd %.2f)\n",
        present_mode_name(g_vk.sc_mode), framepace_target(), n, a, b, lm, lsd);
}

static void *pt_waiter(void *arg) {
    (void)arg;
    PFN_vkWaitForPresentKHR wait = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR");
    int timeouts = 0;
    pthread_mutex_lock(&pt_m);
    for (;;) {
        while (!pt_sc || pt_next > pt_id) pthread_cond_wait(&pt_cv, &pt_m);
        uint64_t id = pt_next;
        VkSwapchainKHR sc = pt_sc;
        pt_waiting = true;
        pthread_mutex_unlock(&pt_m);
        VkResult r = wait(g_vk.dev, sc, id, 200000000ull);
        uint64_t now = SDL_GetTicksNS();
        pthread_mutex_lock(&pt_m);
        pt_waiting = false;
        pthread_cond_broadcast(&pt_cv);
        if (sc != pt_sc || id != pt_next) continue;  // (the swapchain was made again meanwhile)
        if (r == VK_SUCCESS) {
            pt[id % PT_RING].shown = now;
            pt_next++, timeouts = 0;
            if (id - pt_from + 1 >= (uint64_t)pt_every) pt_report(id);
        } else if (r == VK_TIMEOUT && ++timeouts < 3) {
        } else {  // (a hidden window is never shown: from here on, the presents alone)
            LOG("[present] waiting for present %llu: %d; reporting the presents alone\n", (unsigned long long)id, r);
            g_vk.present_wait = false;
            pthread_mutex_unlock(&pt_m);
            return NULL;
        }
    }
}

// vkQueuePresentKHR, timed and given a present id for the waiter when SAVAGEWIND_LOG_PRESENT asks
static VkResult queue_present(VkQueue q, const VkPresentInfoKHR *info) {
    if (pt_every < 0) {
        const char *e = getenv("SAVAGEWIND_LOG_PRESENT");
        pt_every = e ? atoi(e) : 0;
        if (pt_every > PT_RING) pt_every = PT_RING;
        if (pt_every > 0) {
            LOG("[present] timing every %d presents; shown times %s\n", pt_every,
                g_vk.present_wait ? "from VK_KHR_present_wait" : "not available");
            if (g_vk.present_wait) { pthread_t t; pthread_create(&t, NULL, pt_waiter, NULL); }
        }
    }
    if (pt_every <= 0) return vkQueuePresentKHR(q, info);
    pthread_mutex_lock(&pt_m);
    uint64_t id = ++pt_id;
    if (!pt_from) pt_from = id;
    pthread_mutex_unlock(&pt_m);
    VkPresentIdKHR pid = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &id};
    VkPresentInfoKHR pi = *info;
    if (g_vk.present_wait) pi.pNext = &pid;
    uint64_t t = SDL_GetTicksNS();
    VkResult r = vkQueuePresentKHR(q, &pi);
    pthread_mutex_lock(&pt_m);
    pt[id % PT_RING].call = t, pt[id % PT_RING].shown = 0;
    pt_sc = info->pSwapchains[0];
    if (g_vk.present_wait) pthread_cond_broadcast(&pt_cv);
    else if (id - pt_from + 1 >= (uint64_t)pt_every) pt_report(id);
    pthread_mutex_unlock(&pt_m);
    return r;
}

// (before the swapchain goes) the waiter lets go of it; what it hadn't seen shown is left out
static void pt_retire(void) {
    if (pt_every <= 0) return;
    pthread_mutex_lock(&pt_m);
    pt_sc = VK_NULL_HANDLE;
    while (pt_waiting) pthread_cond_wait(&pt_cv, &pt_m);
    pt_next = pt_id + 1;
    pthread_mutex_unlock(&pt_m);
}

static void recreate_swapchain(void) {
    vk_present_idle();
    pt_retire();
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ================================================================== present thread
// vkQueuePresentKHR waits until the GPU has finished the frame (NVIDIA on Windows, windowed). On the game's GPU thread
// that wait (13-18 ms on a fifth of the open-world frames: the frame's last submission goes to the GPU just before)
// came on top of translating the next frame, and BOTW's frame timer dropped the game to 20 fps (interval 3) whenever
// three frames in a row ran over 31.7 ms. On the console a present only queues the frame. Here a thread of its own
// presents, on a second queue of the same family (queues are externally synchronized: the GPU thread keeps submitting
// on the first one meanwhile). The presents queue up (frame generation's pair: the game's frame waits there while the
// next frame is drawn); vkQueuePresentKHR and vkAcquireNextImageKHR take sc_lock (the swapchain is externally
// synchronized). The next frame begins once only a game frame after a generated one is still waiting (vk_frame_begin:
// one frame at a time otherwise, as before); a swapchain change and teardown wait for them all.
// SAVAGEWIND_VK_PRESENT_THREAD=0 (or a family with one queue): presents on the calling thread, as before.
static pthread_mutex_t pres_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t sc_lock = PTHREAD_MUTEX_INITIALIZER;
enum { PRES_Q = 4 };
static struct { VkSwapchainKHR sc; VkSemaphore sem; uint32_t img; int kind; uint64_t seq; } presq[PRES_Q];
static int presq_head, presq_n;  // (pres_m) queued or being presented
static _Atomic int pres_stale;   // that present said the swapchain is out of date or suboptimal
// Frame generation (exec.c vk_fg_generate) presents two frames for each the game draws, the generated one, then the
// game's (PRES_GEN, PRES_AFTER_GEN). The display shows a frame when the GPU has drawn it, not when it is handed over:
// the GPU runs a frame or two behind (present to shown ~33 ms at 50 drawn fps), and both frames of a pair are done
// within a millisecond of each other. Spacing the presents by the CPU's clock left them on screen together and then a
// long gap (with VSync's FIFO and with G-SYNC: ghosting while the camera moved). So each is presented once the GPU has
// drawn it, the generated one at once, the game's half the time between generated frames after it, as AMD's frame
// interpolation swapchain does.
enum { PRES_PLAIN, PRES_GEN, PRES_AFTER_GEN };

static void present_wait_turn(int kind, uint64_t seq) {
    static uint64_t last_gen;
    static double gen_iv;  // (between generated frames, averaged: the game's frame time)
    if (kind == PRES_PLAIN) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull);  // (drawn; a second at most)
    uint64_t now = SDL_GetTicksNS();
    if (kind == PRES_GEN) {
        double dt = last_gen ? (double)(now - last_gen) : 0;
        if (dt > 0 && dt < 1e8) gen_iv = gen_iv > 0 ? gen_iv + (dt - gen_iv) * 0.1 : dt;
        last_gen = now;
        return;
    }
    uint64_t at = last_gen + (uint64_t)(gen_iv / 2);
    if (gen_iv > 0 && now < at) SDL_DelayPrecise(at - now);
}

static void *present_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pres_m);
    for (;;) {
        while (!presq_n) pthread_cond_wait(&pres_cv, &pres_m);
        __typeof__(presq[0]) q = presq[presq_head];
        pthread_mutex_unlock(&pres_m);
        present_wait_turn(q.kind, q.seq);
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &q.sem,
                               .swapchainCount = 1, .pSwapchains = &q.sc, .pImageIndices = &q.img};
        pthread_mutex_lock(&sc_lock);
        VkResult r = queue_present(g_vk.present_queue, &pi);
        pthread_mutex_unlock(&sc_lock);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store(&pres_stale, 1);
        else if (r != VK_SUCCESS) { static int n; if (n++ < 5) LOG("[vk] present failed: %d\n", r); }
        pthread_mutex_lock(&pres_m);
        presq_head = (presq_head + 1) % PRES_Q;
        presq_n--;
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
    while (presq_n) pthread_cond_wait(&pres_cv, &pres_m);
    pthread_mutex_unlock(&pres_m);
}

// Before the next window image is drawn: no plain present waiting (one frame at a time, as before), at most two of
// frame generation's (the previous pair's game frame, this pair's generated one)
static void present_wait_room(void) {
    pthread_mutex_lock(&pres_m);
    for (;;) {
        bool plain = false;
        for (int i = 0; i < presq_n; i++) plain |= presq[(presq_head + i) % PRES_Q].kind == PRES_PLAIN;
        if (!plain && presq_n <= 2) break;
        pthread_cond_wait(&pres_cv, &pres_m);
    }
    pthread_mutex_unlock(&pres_m);
}

static VkResult acquire_image(VkSemaphore sem, uint32_t *img) {
    if (!present_threaded()) return vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, img);
    for (;;) {  // (a short timeout at a time: not holding the swapchain while the present thread needs it)
        pthread_mutex_lock(&sc_lock);
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 2000000, sem, VK_NULL_HANDLE, img);
        pthread_mutex_unlock(&sc_lock);
        if (r != VK_TIMEOUT && r != VK_NOT_READY) return r;
    }
}

// Present window image img once sem is signalled (kind, seq: frame generation's pair and the frame's submission, see
// present_wait_turn). Returns false when the swapchain must be made again.
static bool present_image(uint32_t img, VkSemaphore sem, int kind, uint64_t seq) {
    if (!present_threaded()) {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
                               .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
        present_wait_turn(kind, seq);
        VkResult r = queue_present(g_vk.queue, &pi);
        return !(r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR);
    }
    pthread_mutex_lock(&pres_m);
    while (presq_n == PRES_Q) pthread_cond_wait(&pres_cv, &pres_m);
    presq[(presq_head + presq_n) % PRES_Q] = (__typeof__(presq[0])){g_vk.swapchain, sem, img, kind, seq};
    presq_n++;
    pthread_cond_broadcast(&pres_cv);
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

static void hdr_write(VkCommandBuffer cb, uint32_t i) {
    image_barrier(cb, g_vk.sc_images[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(cb, hdr_sc_images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = hdr_sc_views[i],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hdr_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hdr_layout, 0, 1, &hdr_sets[i], 0, NULL);
    static int srgb = -1;
    if (srgb < 0) srgb = getenv("SAVAGEWIND_HDR_SRGB") != NULL;
    float pc[2] = {g_hdr_white, (float)srgb};
    vkCmdPushConstants(cb, hdr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_dyn_forget();
    image_barrier(cb, hdr_sc_images[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
    present_wait_room();
    g_present_split[4] += SDL_GetTicksNS() - tp;
    float white;  // (Windows HDR turned on or off, the window moved to another display, Video > HDR changed)
    bool hdr = hdr_wanted(&white);
    if (g_hdr) g_hdr_white = white;
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        atomic_exchange(&pres_stale, 0) || g_vk.sc_mode != wanted_present_mode() || hdr != (g_hdr || hdr_failed))
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
    VkResult r = acquire_image(acquire_sem[slot], &cur_frame.image);
    g_present_split[0] += SDL_GetTicksNS() - ta;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = acquire_image(acquire_sem[slot], &cur_frame.image);
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
static int frame_kind;  // (frame generation: PRES_GEN / PRES_AFTER_GEN for the next vk_frame_end's present)
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    if (g_hdr) hdr_write(cb, cur_frame.image);
    else image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    uint64_t ts = SDL_GetTicksNS();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    g_present_split[3] += SDL_GetTicksNS() - ts;
    ts = SDL_GetTicksNS();
    if (!present_image(img, done_sem[img], frame_kind, seq)) recreate_swapchain();
    frame_kind = PRES_PLAIN;
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
    if (rt_save_png(path, rgba, (int)w, (int)h)) LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    else LOG("[vk] saving %s failed\n", path);
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

// Frame generation (Video > Frame generation; exec.c vk_fg_generate): wanted in play, not over the menus (the pause
// menu's pages, the map, the title screen, the F1 menu), where the picture mostly stands and the game's own frames
// keep its cursor crisp. SAVAGEWIND_FG=0/1 overrides the setting (tests).
extern volatile int g_fg_want;
extern volatile int g_fg_active;  // (framepace.c: the game draws every other presented frame)
TexBackend *vk_fg_generate(NvnTexture *t, TexBackend *b);
static uint64_t gen_frames;
static char fg_real_shot[256];  // (SAVAGEWIND_FG_SHOT_AT: the game's frame after a generated one saved)

void vk_fg_wait_idle(void) {
    vk_present_idle();
    vkDeviceWaitIdle(g_vk.dev);
}

static bool fg_wanted(void) {
    static int forced = -2;
    if (forced == -2) forced = getenv("SAVAGEWIND_FG") ? atoi(getenv("SAVAGEWIND_FG")) : -1;
    if (!(forced >= 0 ? forced : g_settings.frame_gen)) return false;
    static int log = -1;
    static uint64_t n;
    if (log < 0) log = getenv("SAVAGEWIND_FG_LOG") != NULL;
    if (log && n++ % 300 == 0)
        LOG("[fg] wanted? title %d menu %d paused %d F1 menu %d\n", framepace_title_ui(), framepace_menu_ui(),
            framepace_paused(), menu_is_open());
    return !framepace_title_ui() && !framepace_menu_ui() && !framepace_paused() && !menu_is_open();
}

static void present_locked(NvnTexture *t) {
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    uint64_t t0 = SDL_GetTicksNS();
    vk_end_pass();
    g_present_split[1] += SDL_GetTicksNS() - t0;
    t0 += pso_wait();  // (not the acquire's time)
    // Frame generation: the frame between the last one and this (exec.c vk_fg_generate), presented first
    TexBackend *gen = vk_fg_generate(t, b);
    {   // (measuring) SAVAGEWIND_FG_TEST=1: the frame made, not presented
        static int test = -1;
        if (test < 0) test = getenv("SAVAGEWIND_FG_TEST") ? atoi(getenv("SAVAGEWIND_FG_TEST")) : 0;
        if (test == 1) gen = NULL;
    }
    g_fg_active = gen != NULL;
    static const float black[4] = {0, 0, 0, 1};
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video page "Upscaler"). A window that
        // isn't 16:9 shows more of the world (Video > Fill wide screens: the game's views widened, output_scale.c
        // rt_persp), else the game's 16:9 image fitted into it with bars.
        // Fullscreen on a size the display has no mode for (runtime/window_size.c): that size, scaled to the screen.
        OutScale os;
        int iw, ih;
        if (winsize_virtual(&iw, &ih) && (iw != (int)dw || ih != (int)dh)) {
            output_scale(iw, ih, g_settings.wide_view, &os);
            output_scale_screen(dw, dh, iw, ih, &os);
        } else {
            output_scale(dw, dh, g_settings.wide_view, &os);
        }
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh;
        float crop[4] = {(float)os.crop[0], (float)os.crop[1], (float)os.crop[2], (float)os.crop[3]};  // (uv origin, size)
        renderer_set_scale((float)sx, (float)sy);
        renderer_set_scene_scale((float)os.scene_sx, (float)os.scene_sy);
        renderer_set_shadow_size(shadow_map_size(total_frames + 1));
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        uint64_t td = SDL_GetTicksNS();
        if (gen) {  // the generated frame, as the game's are (crop, anti-aliasing, menu), then this one half a frame later
            vk_present_draw(cb, gen, (float)vx, (float)vy, (float)vw, (float)vh, pv);
            menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
            // SAVAGEWIND_FG_SHOT_AT=<frame>,... (counted as SAVAGEWIND_SHOT_AT): the window at two presents in a row
            // from there, each generated frame and the game's after it -> build/frames/window_N_<0|1>_<gen|real>.png
            // (real 0, gen 1, real 1: a generated frame between the two it was made from)
            static const char *gnext;
            static int ginit, gleft;
            static long gat;
            if (!ginit) { ginit = 1; gnext = getenv("SAVAGEWIND_FG_SHOT_AT"); if (gnext) mkdir("build/frames", 0755); }
            if (!gleft && gnext && *gnext) {
                extern uint64_t framepace_script_frame(uint64_t presented);
                long at = strtol(gnext, NULL, 10);
                if ((long)framepace_script_frame(total_frames + 1) >= at) {
                    gat = at, gleft = 2;
                    while (*gnext && *gnext != ',') gnext++;
                    if (*gnext == ',') gnext++;
                }
            }
            char gpath[256] = "";
            if (gleft) {
                snprintf(gpath, sizeof gpath, "build/frames/window_%05ld_%d_gen.png", gat, 2 - gleft);
                snprintf(fg_real_shot, sizeof fg_real_shot, "build/frames/window_%05ld_%d_real.png", gat, 2 - gleft);
                gleft--;
            }
            frame_kind = PRES_GEN;
            vk_frame_end(gpath[0] ? gpath : NULL);
            frame_kind = PRES_AFTER_GEN;  // (the game's frame, below)
            gen_frames++;
            perfmon_generated_frame();
            if (!vk_frame_begin(black)) { renderer_submit(); goto presented; }
            cb = vk_cmd();
        }
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("SAVAGEWIND_MENU_AT") ? atol(getenv("SAVAGEWIND_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_present_split[2] += SDL_GetTicksNS() - td;
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("SAVAGEWIND_WINDOW_SHOT") ? atol(getenv("SAVAGEWIND_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        // SAVAGEWIND_WINDOW_SHOT_AT=<frame>,... (ascending, counted as SAVAGEWIND_SHOT_AT): the window as presented
        // (bars and menus included) -> build/frames/window_N.png
        static const char *wnext;
        static int winit;
        char wpath[256] = "";
        if (!winit) { winit = 1; wnext = getenv("SAVAGEWIND_WINDOW_SHOT_AT"); if (wnext) mkdir("build/frames", 0755); }
        if (wnext && *wnext && !shot) {
            extern uint64_t framepace_script_frame(uint64_t presented);
            long at = strtol(wnext, NULL, 10);
            if ((long)framepace_script_frame(total_frames + 1) >= at) {
                snprintf(wpath, sizeof wpath, "build/frames/window_%05ld.png", at);
                while (*wnext && *wnext != ',') wnext++;
                if (*wnext == ',') wnext++;
            }
        }
        vk_frame_end(shot ? "build/window.png" : wpath[0] ? wpath : fg_real_shot[0] ? fg_real_shot : NULL);
        fg_real_shot[0] = 0;
        if (shot && getenv("SAVAGEWIND_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
presented:
    g_fg_want = fg_wanted();  // (for the next frame's draws)
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    extern void vk_list_stats_frame(void);
    vk_list_stats_frame();
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
    {   // SAVAGEWIND_EXIT_AT=<frame> (counted as SAVAGEWIND_INPUT): end the run there
        static long exit_at = -1;
        if (exit_at < 0) exit_at = getenv("SAVAGEWIND_EXIT_AT") ? atol(getenv("SAVAGEWIND_EXIT_AT")) : 0;
        if (exit_at && (long)script_frame >= exit_at) { fflush(stderr); _exit(0); }
    }
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("SAVAGEWIND_SLOW_FRAME_MS") ? atol(getenv("SAVAGEWIND_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (SAVAGEWIND_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void test_warp_frame(uint64_t frame);  // runtime/services/test_warp.c (SAVAGEWIND_TEST_TIME)
    test_warp_frame(total_frames);
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
    {   // (testing) SAVAGEWIND_FILTER_AT=<frame>:<anisotropy 0-4>/<sharpening 0-10>,... (ascending): Video >
        // Anisotropic filtering and Sharpening from that frame on (A/B of the same view)
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("SAVAGEWIND_FILTER_AT"); }
        while (next && *next) {
            char *end;
            long at = strtol(next, &end, 10);
            if ((long)total_frames < at) break;
            int a = 0, s = 0;
            if (*end == ':' && sscanf(end + 1, "%d/%d", &a, &s) == 2) {
                g_settings.anisotropy = a;
                g_settings.sharpening = s;
                LOG("[test] frame %llu: anisotropy %d sharpening %d\n", (unsigned long long)total_frames, a, s);
            }
            next = strchr(end, ',');
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
        if (gen_frames && last_fps_time > 0)
            LOG("[fg] %.1f generated frames/s: %.1f presented\n", gen_frames / (now - last_fps_time),
                (frames + gen_frames) / (now - last_fps_time));
        max_gap = 0;
        frames = 0;
        gen_frames = 0;
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
        extern void vk_keylog_flush(void);
        vk_keylog_flush();
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

void vk_wait_ready(void) {  // (a guest thread: the window and device are the main thread's, ui_run)
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
}

void backend_init(void) {
    vk_wait_ready();
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
void backend_guest_memory_reset(void) {}  // (exec.c: the next SetHeapSize maps the heap again, or it is imported on first use)
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
