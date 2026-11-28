// Global memory of the Vulkan command executor: the guest heap's import and copies, the ranges shaders store to,
// the heap copy used for vertex/index fetches, and the ranges copied for the HUD's global memory reads (exec.c).
#include <SDL3/SDL.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../ui_layout.h"
#ifdef _WIN32
#define NOUSER
#include <windows.h>
#endif

#include "../../kernel.h"
#include "../../control.h"
#include "../../prof.h"
#include "../../platform.h"
#include "shaders.h"
#include "vk.h"
#include "exec/internal.h"
#include "exec_feedback.h"
#include "bloom_scale.h"
#include "exec_copy.h"
#include "vram.h"
#include "fsr3.h"
#include "fsr_api.h"
#include "mvec.h"
#include "raytrace.h"
#include "xess.h"
#include "../output_scale.h"
#include "record.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../color_grade.h"
#include "../../colour_vision.h"
#include "../output_scale.h"
#include "framegen.h"
#include "dlss.h"
#include "../output_scale.h"
#include "../../game_sites.h"
#include "../../framepace.h"
#include "../../services/fs_layout.h"
#include "../../perfcount.h"

// (a dispatch's constant buffers, copied: gsnap_cs_prepare) the address the compute shader is given for each, 0: its
// own; and the buffer the copies are in
ExecGsnapCs gsnap_cs;

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): on Windows, sparse-bind committed guest pages
// imported with VK_EXT_external_memory_host; other paths retain the whole-heap import. GPU writes land in guest
// memory. SAVAGEWIND_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
uint64_t gmem_size;
bool gmem_sparse;
#define GMEM_SPARSE_PAGE_SIZE 0x10000ull
#define GMEM_SPARSE_CHUNK_SIZE (1ull << 16)
#define GMEM_SPARSE_CHUNK_PAGES (GMEM_SPARSE_CHUNK_SIZE / GMEM_SPARSE_PAGE_SIZE)
#define GMEM_SPARSE_PAGE_COUNT ((HEAP_MAX_LARGEST + GMEM_SPARSE_PAGE_SIZE - 1) / GMEM_SPARSE_PAGE_SIZE)  // (room for the largest heap)
#define GMEM_SPARSE_MEM_COUNT GMEM_SPARSE_PAGE_COUNT
static uint8_t gmem_sparse_bound_pages[GMEM_SPARSE_PAGE_COUNT];
static uint64_t gmem_sparse_generation;
static VkDeviceMemory gmem_sparse_mem[GMEM_SPARSE_MEM_COUNT];
static VkSparseMemoryBind gmem_sparse_binds[GMEM_SPARSE_PAGE_COUNT];
static uint32_t gmem_sparse_nmem;

#ifdef _WIN32
static bool gmem_sparse_refresh(uint64_t size);
#endif

// (maxwell_gmem_bda) the heap's memory has to be allocated with a device address
static const void *gmem_alloc_chain(VkMemoryAllocateFlagsInfo *f, const void *next) {
    if (!maxwell_gmem_bda) return next;
    *f = (VkMemoryAllocateFlagsInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = next,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    return f;
}

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &memory);
    uint32_t memory_type = (uint32_t)__builtin_ctz(mp.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
#ifdef VK_EXT_memory_priority
    VkMemoryPriorityAllocateInfoEXT priority = {.sType = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,
                                                .priority = 0.1f};
    if (g_vk.memory_priority) imp.pNext = &priority;
#endif
    VkMemoryAllocateFlagsInfo af;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = gmem_alloc_chain(&af, &imp), .allocationSize = size,
                               .memoryTypeIndex = memory_type};
    VkDeviceMemory mem;
    VkResult alloc_result = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (alloc_result != VK_SUCCESS) {
        LOG("[vk] guest heap import allocation failed (%d)\n", alloc_result);
        return false;
    }
    VkMemoryPropertyFlags chosen_flags = memory.memoryTypes[memory_type].propertyFlags;
    uint32_t chosen_heap = memory.memoryTypes[memory_type].heapIndex;
    LOG("[vk] guest heap memory type %u on heap %u (%llu MB, flags %#x)%s\n",
        memory_type, chosen_heap, (unsigned long long)(memory.memoryHeaps[chosen_heap].size >> 20), chosen_flags,
        (chosen_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "" : " (non-device-local preferred)");
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | (maxwell_gmem_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vk_dyn_forget();  // (a new buffer may get the old handle: nothing recorded may be taken as still bound)
        setc_gen++;
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB%s\n", (unsigned long long)(size >> 20),
        g_vk.memory_priority ? " (low memory priority)" : "");
    return true;
}

static bool gmem_sparse_create(uint64_t size) {
#ifdef _WIN32
    if (!plat_guest_sparse_heap_enabled()) return false;
    if (!g_vk.sparse_buffer || !g_vk.sparse_queue || !g_vk.sparse_nonresident_strict || !g_vk.sparse_host_import) {
        LOG("[vk] sparse guest heap incompatible with GPU: \n sparse buffer %s\n sparse queue %s\n strict nonresident access %s\n host import %s\n",
            g_vk.sparse_buffer ? "available" : "unavailable",
            g_vk.sparse_queue ? "available" : "unavailable",
            g_vk.sparse_nonresident_strict ? "available" : "unavailable",
            g_vk.sparse_host_import ? "available" : "unavailable");
        return false;
    }
    VkPhysicalDeviceExternalBufferInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExternalBufferProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(g_vk.phys, &ext_info, &ext_props);
    if (!(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        LOG("[vk] sparse guest heap external host import unsupported for full buffer usage\n");
        return false;
    }
    if (!gmem_sparse_timeline) {
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                          .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                          .initialValue = 0};
        VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                           .pNext = &type};
        VkResult sr = vkCreateSemaphore(g_vk.dev, &semaphore, NULL, &gmem_sparse_timeline);
        if (sr != VK_SUCCESS) {
            LOG("[vk] sparse guest heap timeline creation failed (%d)\n", sr);
            return false;
        }
    }
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .size = HEAP_MAX,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_vk.dev, &bi, NULL, &buf);
    if (r != VK_SUCCESS) {
        LOG("[vk] sparse guest heap buffer creation failed (%d)\n", r);
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    if (req.alignment != 0x10000 || req.size < HEAP_MAX) {
        LOG("[vk] sparse guest heap alignment/size unsupported (%llu/%llu)\n",
            (unsigned long long)req.alignment, (unsigned long long)req.size);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return false;
    }
    gmem_buf = buf;
    gmem_size = size;
    gmem_sparse = true;
    memset(gmem_sparse_bound_pages, 0, sizeof gmem_sparse_bound_pages);
    gmem_sparse_generation = UINT64_MAX;
    LOG("[vk] sparse guest heap buffer created: %llu MB virtual, %llu KiB binding alignment\n",
        (unsigned long long)(HEAP_MAX >> 20), (unsigned long long)(req.alignment >> 10));
    if (!gmem_sparse_refresh(size)) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_size = 0;
        gmem_sparse = false;
        memset(gmem_sparse_bound_pages, 0, sizeof gmem_sparse_bound_pages);
        gmem_sparse_generation = UINT64_MAX;
        return false;
    }
    return true;
#else
    (void)size;
    return false;
#endif
}

#ifdef _WIN32
static bool gmem_sparse_import_range(uint64_t offset, uint64_t bytes, VkDeviceMemory *out_mem) {
    uint64_t address = HEAP_BASE + offset;
    if (!bytes || (offset % GMEM_SPARSE_PAGE_SIZE) || (bytes % GMEM_SPARSE_PAGE_SIZE) ||
        offset > gmem_size || bytes > gmem_size - offset) return false;
    if (!plat_guest_commit_sparse_range(address, bytes)) {
        LOG("[vk] sparse guest heap could not commit GPU backing window %#llx+%#llx\n",
            (unsigned long long)address, (unsigned long long)bytes);
        return false;
    }
    static bool cache_ready;
    static PFN_vkGetMemoryHostPointerPropertiesEXT get_host_props;
    static VkMemoryRequirements req;
    static VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props;
    static VkPhysicalDeviceMemoryProperties memory;
    if (!cache_ready) {
        get_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
        vkGetBufferMemoryRequirements(g_vk.dev, gmem_buf, &req);
        host_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &memory);
        if (!get_host_props || !host_props.minImportedHostPointerAlignment) {
            LOG("[vk] sparse guest heap host-import properties unavailable\n");
            return false;
        }
        cache_ready = true;
    }
    void *host = (void *)(uintptr_t)address;
    VkMemoryHostPointerPropertiesEXT host_memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (((uintptr_t)host % host_props.minImportedHostPointerAlignment) != 0 ||
        get_host_props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &host_memory) != VK_SUCCESS) {
        LOG("[vk] sparse guest heap host pointer query failed at %#llx\n", (unsigned long long)address);
        return false;
    }
    uint32_t type = UINT32_MAX;
    uint32_t compatible = req.memoryTypeBits & host_memory.memoryTypeBits;
    int best_score = -1;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if (!(compatible & (1u << i))) continue;
        VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        int score = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 0 : 4;
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) score += 2;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) score++;
        if (score > best_score) {
            best_score = score;
            type = i;
        }
    }
    if (type == UINT32_MAX) {
        LOG("[vk] sparse guest heap has no compatible memory type at %#llx\n", (unsigned long long)address);
        return false;
    }
    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &import,
                               .allocationSize = bytes,
                               .memoryTypeIndex = type};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        LOG("[vk] sparse guest heap import failed for %llu MiB (%d)\n",
            (unsigned long long)(bytes >> 20), r);
        return false;
    }
    *out_mem = mem;
    return true;
}

static bool gmem_sparse_range_bound(uint64_t addr, uint64_t end) {
    if (!gmem_sparse || addr < HEAP_BASE || end <= addr || end > HEAP_BASE + gmem_size) return false;
    uint64_t first = (addr - HEAP_BASE) / GMEM_SPARSE_CHUNK_SIZE;
    uint64_t last = (end - 1 - HEAP_BASE) / GMEM_SPARSE_CHUNK_SIZE;
    for (uint64_t chunk = first; chunk <= last; chunk++) {
        uint64_t page = chunk * GMEM_SPARSE_CHUNK_PAGES;
        if (page >= GMEM_SPARSE_PAGE_COUNT || !gmem_sparse_bound_pages[page]) return false;
    }
    return true;
}

static bool gmem_sparse_refresh(uint64_t size) {
    uint64_t generation = plat_guest_commit_generation();
    if (generation == gmem_sparse_generation) return true;

    uint32_t first_new_mem = gmem_sparse_nmem;
    uint32_t bind_count = 0;
    uint64_t page_count = size / GMEM_SPARSE_PAGE_SIZE;
    uint64_t chunk_count = (page_count + GMEM_SPARSE_CHUNK_PAGES - 1) / GMEM_SPARSE_CHUNK_PAGES;
    uint64_t run_start = UINT64_MAX;
    for (uint64_t chunk = 0; chunk <= chunk_count; chunk++) {
        bool active = false;
        if (chunk < chunk_count) {
            uint64_t first_page = chunk * GMEM_SPARSE_CHUNK_PAGES;
            uint64_t end_page = first_page + GMEM_SPARSE_CHUNK_PAGES;
            if (end_page > page_count) end_page = page_count;
            if (!gmem_sparse_bound_pages[first_page]) {
                uint64_t first_word = first_page / 64;
                uint64_t end_word = (end_page + 63) / 64;
                for (uint64_t word = first_word; word < end_word && !active; word++) {
                    uint64_t committed = plat_guest_sparse_heap_committed_word(word);
                    uint64_t word_first_page = word * 64;
                    uint64_t lo = first_page > word_first_page ? first_page - word_first_page : 0;
                    uint64_t hi = end_page < word_first_page + 64 ? end_page - word_first_page : 64;
                    uint64_t mask = (hi == 64 ? UINT64_MAX : (1ull << hi) - 1) & ~((1ull << lo) - 1);
                    active = (committed & mask) != 0;
                }
            }
        }
        if (active && run_start == UINT64_MAX) run_start = chunk;
        if (!active && run_start != UINT64_MAX) {
            if (gmem_sparse_nmem >= GMEM_SPARSE_MEM_COUNT) {
                LOG("[vk] sparse guest heap memory tracking overflow\n");
                goto fail;
            }
            VkDeviceMemory mem = VK_NULL_HANDLE;
            uint64_t offset = run_start * GMEM_SPARSE_CHUNK_SIZE;
            uint64_t bytes = (chunk - run_start) * GMEM_SPARSE_CHUNK_SIZE;
            if (offset + bytes > size) bytes = size - offset;
            bool imported = gmem_sparse_import_range(offset, bytes, &mem);
            if (!imported) goto fail;
            gmem_sparse_mem[gmem_sparse_nmem++] = mem;
            gmem_sparse_binds[bind_count++] = (VkSparseMemoryBind){
                .resourceOffset = offset, .size = bytes, .memory = mem, .memoryOffset = 0};
            run_start = UINT64_MAX;
        }
    }
    if (bind_count) {
        // Sparse binds and queue submissions share g_vk.queue, which Vulkan requires to be externally synchronized.
        // Drain prior recorder packets before binding; later submissions wait on this bind's timeline value.
        rec_drain();
        VkSparseBufferMemoryBindInfo buffer_bind = {
            .buffer = gmem_buf, .bindCount = bind_count, .pBinds = gmem_sparse_binds};
        uint64_t wait_value = submit_seq;
        uint64_t signal_value = gmem_sparse_bind_seq + 1;
        VkTimelineSemaphoreSubmitInfo timeline = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = wait_value ? 1u : 0u,
            .pWaitSemaphoreValues = wait_value ? &wait_value : NULL,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &signal_value};
        VkBindSparseInfo bind_info = {
            .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
            .pNext = &timeline,
            .waitSemaphoreCount = wait_value ? 1u : 0u,
            .pWaitSemaphores = wait_value ? &g_vk.timeline : NULL,
            .bufferBindCount = 1,
            .pBufferBinds = &buffer_bind,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &gmem_sparse_timeline};
        pthread_mutex_lock(&g_vk_queue_lock);
        VkResult r = vkQueueBindSparse(g_vk.queue, 1, &bind_info, VK_NULL_HANDLE);
        pthread_mutex_unlock(&g_vk_queue_lock);
        if (r != VK_SUCCESS) {
            LOG("[vk] sparse guest heap batch binding failed (%d)\n", r);
            goto fail;
        }
        gmem_sparse_bind_seq = signal_value;
        for (uint32_t i = 0; i < bind_count; i++) {
            uint64_t first = gmem_sparse_binds[i].resourceOffset / GMEM_SPARSE_PAGE_SIZE;
            uint64_t pages = gmem_sparse_binds[i].size / GMEM_SPARSE_PAGE_SIZE;
            memset(&gmem_sparse_bound_pages[first], 1, (size_t)pages);
        }
    }
    gmem_sparse_generation = generation;
    return true;

fail:
    while (gmem_sparse_nmem > first_new_mem) vkFreeMemory(g_vk.dev, gmem_sparse_mem[--gmem_sparse_nmem], NULL);
    return false;
}
#endif

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)
static bool gmem_mapped;     // the heap is GPU memory mapped at its addresses (backend_heap_set): no import

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#if defined(__has_include) && __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else  // (kernel headers older than 5.6: the uapi definitions, unchanged since)
#include <linux/types.h>
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
// On amdgpu the import is a userptr buffer, and the kernel gets every page of every userptr buffer again at each
// submission: on the Steam Deck ~4 ms a submit for the 3 GB heap (0.03 ms without it), half of a 40 ms frame. So
// there the heap is GPU memory from the start: the first SetHeapSize (kernel.c), before the game has touched the
// heap, backs the heap's addresses with a dma-buf the GPU also has as a buffer; the shaders' global memory and the
// vertex reads use it in place, as they did the import. ARM's Mali blob (RK3588) faults on the import instead
// (TRANSLATION_FAULT reading it within seconds), so the backing is chosen with SAVAGEWIND_VK_HEAP:
//   import   the host-pointer import (gmem_import), no backing here (also SAVAGEWIND_VK_HEAP_IMPORT=1)
//   export   Vulkan memory exported as a dma-buf and mapped over the heap (the default on RADV)
//   dmaheap  a dma-buf from the kernel's dma-buf heap (SAVAGEWIND_VK_DMA_HEAP: its name, default "system"), mapped over
//            the heap and imported into Vulkan, as the Mali wrapper ICDs' WSI shares its images
// Unset: export on RADV and Turnip (which has no host-pointer import: without a backing the shaders' global memory read
// zeros), import elsewhere.
enum { HEAP_IMPORT, HEAP_EXPORT, HEAP_DMAHEAP };

static int heap_mode(void) {
    const char *e = getenv("SAVAGEWIND_VK_HEAP");
    if (e && !strcmp(e, "export")) return HEAP_EXPORT;
    if (e && !strcmp(e, "dmaheap")) return HEAP_DMAHEAP;
    if (e && !strcmp(e, "import")) return HEAP_IMPORT;
    if (e && *e) LOG("[vk] SAVAGEWIND_VK_HEAP=%s: not import / export / dmaheap, the default instead\n", e);
    if (getenv("SAVAGEWIND_VK_HEAP_IMPORT")) return HEAP_IMPORT;
    return g_vk.radv || g_vk.turnip ? HEAP_EXPORT : HEAP_IMPORT;
}

static void heap_unmap(uint64_t size) {  // the heap's addresses back to plain memory (nothing has used them yet)
    mmap((void *)(uintptr_t)HEAP_BASE, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

static VkBuffer heap_buffer(uint64_t size) {
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | (maxwell_gmem_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) return VK_NULL_HANDLE;
    return buf;
}

static void heap_done(VkBuffer buf, VkDeviceMemory mem, uint64_t size, const char *how) {
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    gmem_mapped = true;
    LOG("[vk] guest heap is GPU memory (%s): %llu MB\n", how, (unsigned long long)(size >> 20));
}

// export: Vulkan memory of the heap's size, exported as a dma-buf, mapped over the heap. Host-cached first (the game's
// own memory), not device-local where there is a choice (RADV: VRAM); a unified-memory GPU (Mali) has only
// device-local types, so there they are allowed.
static void heap_export(uint64_t size) {
    uint32_t type = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int pass = 0; pass < 4 && type == UINT32_MAX; pass++)  // (cached / any) x (not device-local / unified memory)
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            bool local_ok = pass >= 2 && !g_vk.radv;
            if ((f & want) == want && (local_ok || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) &&
                ((pass & 1) || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) && !(f & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD))
                type = i;
        }
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdKHR");
    if (type == UINT32_MAX || !get_fd) { LOG("[vk] guest heap export: no memory type for it, imported instead\n"); return; }
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateFlagsInfo af;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = gmem_alloc_chain(&af, &ex), .allocationSize = size,
                               .memoryTypeIndex = type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) { LOG("[vk] guest heap export: allocation failed (%d), imported instead\n", r); return; }
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if (get_fd(g_vk.dev, &gi, &fd) != VK_SUCCESS || fd < 0) {
        LOG("[vk] guest heap export: no dma-buf, imported instead\n");
        vkFreeMemory(g_vk.dev, mem, NULL);
        return;
    }
    void *p = mmap((void *)(uintptr_t)HEAP_BASE, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    close(fd);  // (the mapping keeps the dma-buf)
    if (p != (void *)(uintptr_t)HEAP_BASE) {
        LOG("[vk] guest heap export: mapping the dma-buf failed (%s), imported instead\n", strerror(errno));
        heap_unmap(size);
        vkFreeMemory(g_vk.dev, mem, NULL);
        return;
    }
    VkBuffer buf = heap_buffer(size);
    if (!buf || vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS) {
        LOG("[vk] guest heap export: no buffer for it, imported instead\n");
        if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
        heap_unmap(size);
        vkFreeMemory(g_vk.dev, mem, NULL);
        return;
    }
    char how[64];
    snprintf(how, sizeof how, "exported dma-buf mapped at the heap, type %u", type);
    extern uint64_t g_mem_direct_lo, g_mem_direct_hi;  // (control_mem.h: the control peeks copy in place here)
    g_mem_direct_lo = HEAP_BASE, g_mem_direct_hi = HEAP_BASE + size;
    heap_done(buf, mem, size, how);
}

// dmaheap: a dma-buf from /dev/dma_heap/<name>, mapped over the heap, imported into Vulkan (it then owns the fd).
static void heap_dmaheap(uint64_t size) {
    const char *name = getenv("SAVAGEWIND_VK_DMA_HEAP");
    if (!name || !*name) name = "system";
    char path[128];
    snprintf(path, sizeof path, "/dev/dma_heap/%s", name);
    int hfd = open(path, O_RDONLY | O_CLOEXEC);
    if (hfd < 0) { LOG("[vk] guest heap dmaheap: can't open %s (%s), imported instead\n", path, strerror(errno)); return; }
    struct dma_heap_allocation_data d = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d);
    int err = errno;
    close(hfd);
    if (r < 0) { LOG("[vk] guest heap dmaheap: %llu MB from %s failed (%s), imported instead\n", (unsigned long long)(size >> 20), path, strerror(err)); return; }
    int fd = (int)d.fd;
    PFN_vkGetMemoryFdPropertiesKHR fdp = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR mfp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult vr = fdp ? fdp(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &mfp) : VK_ERROR_EXTENSION_NOT_PRESENT;
    VkBuffer buf = heap_buffer(size);
    VkMemoryRequirements req = {0};
    if (buf) vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    uint32_t bits = mfp.memoryTypeBits & req.memoryTypeBits;
    if (vr != VK_SUCCESS || !buf || !bits) {
        LOG("[vk] guest heap dmaheap: the driver won't take the dma-buf (%d, types %#x & %#x), imported instead\n", vr,
            mfp.memoryTypeBits, req.memoryTypeBits);
        if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
        close(fd);
        return;
    }
    void *p = mmap((void *)(uintptr_t)HEAP_BASE, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (p != (void *)(uintptr_t)HEAP_BASE) {
        LOG("[vk] guest heap dmaheap: mapping the dma-buf failed (%s), imported instead\n", strerror(errno));
        heap_unmap(size);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        close(fd);
        return;
    }
    uint32_t type = (uint32_t)__builtin_ctz(bits);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vr = VK_ERROR_UNKNOWN;
    for (int dedicated = 0; dedicated < 2 && vr != VK_SUCCESS; dedicated++) {  // (some drivers want imports dedicated)
        int ifd = dup(fd);  // (a successful import owns its fd; a failed one leaves it with us)
        if (ifd < 0) break;
        VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf};
        VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = dedicated ? &ded : NULL,
                                       .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = ifd};
        VkMemoryAllocateFlagsInfo af;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = gmem_alloc_chain(&af, &imp),
                                   .allocationSize = req.size > size ? req.size : size, .memoryTypeIndex = type};
        vr = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
        if (vr != VK_SUCCESS) close(ifd);
    }
    close(fd);  // (the mapping and the import keep the dma-buf)
    if (vr != VK_SUCCESS || vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS) {
        LOG("[vk] guest heap dmaheap: importing the dma-buf failed (%d), imported instead\n", vr);
        if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        heap_unmap(size);
        return;
    }
    char how[96];
    snprintf(how, sizeof how, "dma-buf from %s mapped at the heap, imported as type %u", path, type);
    extern uint64_t g_mem_direct_lo, g_mem_direct_hi;  // (control_mem.h: the control peeks copy in place here)
    g_mem_direct_lo = HEAP_BASE, g_mem_direct_hi = HEAP_BASE + size;
    heap_done(buf, mem, size, how);
}

void backend_heap_set(uint64_t size) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_GMEM") != NULL;
    if (gmem_mapped && size > gmem_size)
        LOG("[vk] guest heap grew to %llu MB past the %llu MB mapped: the rest is copied, not shared\n",
            (unsigned long long)(size >> 20), (unsigned long long)(gmem_size >> 20));
    if (off || gmem_mapped || gmem_buf || !size) return;
    vk_wait_ready();
    int mode = heap_mode();
    if (mode == HEAP_IMPORT) return;
    if (!g_vk.dmabuf) { LOG("[vk] guest heap: the driver has no dma-buf support, imported instead\n"); return; }
    size = (size + 0x1FFFFF) & ~0x1FFFFFull;  // (2 MB pages)
    if (mode == HEAP_EXPORT) heap_export(size);
    else heap_dmaheap(size);
}
#else
void backend_heap_set(uint64_t size) { (void)size; }
#endif

VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && !gmem_mapped) {
#ifdef _WIN32
        if (plat_guest_sparse_heap_enabled()) {
            if (gmem_sparse) {
                if (want > gmem_size) gmem_size = want;
                if (!gmem_sparse_refresh(gmem_size)) FATAL("sparse guest heap page binding failed\n");
            } else if (!gmem_buf && want && !gmem_sparse_create(want)) {
                bool auto_mode = g_settings.sparse_heap == SPARSE_HEAP_AUTO;
                if (auto_mode) {
                    char reason[512];
                    size_t n = 0;
                    n += (size_t)snprintf(reason + n, sizeof reason - n, "The selected GPU cannot use the sparse guest heap:");
                    if (!g_vk.sparse_buffer && n < sizeof reason)
                        n += (size_t)snprintf(reason + n, sizeof reason - n, "\n- sparse buffer or residency support is unavailable");
                    if (!g_vk.sparse_queue && n < sizeof reason)
                        n += (size_t)snprintf(reason + n, sizeof reason - n, "\n- no selected queue supports sparse binding");
                    if (!g_vk.sparse_nonresident_strict && n < sizeof reason)
                        n += (size_t)snprintf(reason + n, sizeof reason - n, "\n- strict nonresident access is unavailable");
                    if (!g_vk.sparse_host_import && n < sizeof reason)
                        n += (size_t)snprintf(reason + n, sizeof reason - n, "\n- the sparse host-memory import probe failed");
                    if (n < sizeof reason)
                        snprintf(reason + n, sizeof reason - n,
                                 "\n- sparse heap initialization or host-memory binding failed (see the log for details)");
                    LOG("[vk] sparse guest heap initialization failed in auto mode; disabling it, saving sparse_heap=off and restarting after the error dialog closes\n");
                    g_settings.sparse_heap = SPARSE_HEAP_OFF;
                    settings_save();
                    char message[1024];
                    snprintf(message, sizeof message, "%s\n\nAdapter: %s\nSparse heap has been turned off in savagewind.ini. The game will restart when you close this message.",
                             reason, g_vk.props.deviceName);
                    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sparse heap unavailable", message, NULL))
                        LOG("[vk] could not show sparse heap error dialog: %s\n", SDL_GetError());
                    if (setenv("SAVAGEWIND_VK_SPARSE_HEAP", "0", 1) != 0)
                        FATAL("could not disable the sparse heap for the automatic restart\n");
                    plat_exec(NULL);
                    FATAL("could not restart with the sparse heap disabled\n");
                }
                LOG("[vk] sparse guest heap initialization failed with sparse_heap=on; leaving the setting unchanged\n");
                FATAL("sparse guest heap initialization failed (the lines above say why): this GPU or driver can't use "
                      "it. Set sparse_heap = auto in savagewind.ini, or start without SAVAGEWIND_VK_SPARSE_HEAP=1\n");
            }
        } else
#endif
        if (want > gmem_size && !gmem_import(want)) {
            LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
            off = 1;
        }
    }
    if (maxwell_gmem_bda) {  // (GMemInfo: the heap's device address and the size mapped there; zeros without one)
        static VkBuffer addr_of;
        static VkDeviceAddress addr;
        if (gmem_buf && addr_of != gmem_buf) {
            VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = gmem_buf};
            addr = vkGetBufferDeviceAddress(g_vk.dev, &ai);
            addr_of = gmem_buf;
        }
        const uint64_t info[2] = {gmem_buf ? addr : 0, gmem_buf ? gmem_size : 0};
        return ubo_span(copy_dedupe(DEDUPE_GMEM, info, sizeof info, 256), sizeof info);
    }
    if (gmem_buf) {
        // The heap's first GMEM_SPLIT bytes (GMEM_BUFFER_INDEX); the rest is the second binding (gmem_info_hi): one binding
        // of all of it is past some drivers' maxStorageBufferRange (Mali-G610: 2 GB, the heap 3 GB)
        // (maxwell_gmem_split off: one binding of the whole heap, as before)
        uint64_t range = maxwell_gmem_split && gmem_size > GMEM_SPLIT ? GMEM_SPLIT : gmem_size, lim = g_vk.props.limits.maxStorageBufferRange;
        if (maxwell_gmem_split && range > lim) range = lim & ~0xFFFFull;
        return (VkDescriptorBufferInfo){gmem_buf, 0, range};
    }
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// The heap past GMEM_SPLIT (GMEM2_BUFFER_INDEX), or a small zeroed buffer when it is no larger than that
VkDescriptorBufferInfo gmem_info_hi(void) {
    VkDescriptorBufferInfo lo = gmem_info();  // (makes the import first where there is one)
    if (gmem_buf && lo.buffer == gmem_buf && gmem_size > GMEM_SPLIT) {
        uint64_t range = gmem_size - GMEM_SPLIT, lim = g_vk.props.limits.maxStorageBufferRange;
        if (range > lim) range = lim & ~0xFFFFull;
        return (VkDescriptorBufferInfo){gmem_buf, GMEM_SPLIT, range};
    }
    static uint32_t zero[1024];
    VkSpan z = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){z.buf, z.off, sizeof zero};
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; SAVAGEWIND_VK_COPY_VERTICES=1 forces copies.
static bool gsnap_verts(void);
static uint64_t gsnap_verts_max(void);
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("SAVAGEWIND_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
#ifdef _WIN32
    if (plat_guest_sparse_heap_enabled() && g_vk.sparse_buffer && g_vk.sparse_queue &&
        g_vk.sparse_nonresident_strict && g_vk.sparse_host_import && addr < HEAP_BASE + HEAP_MAX &&
        size && addr + size > addr) {
        uint64_t end = addr + size;
        uint64_t heap_size = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
        if (heap_size > HEAP_MAX) heap_size = HEAP_MAX;
        if (end > HEAP_BASE + heap_size) end = HEAP_BASE + heap_size;
        if (addr >= end) return false;
        if (gmem_sparse_range_bound(addr, end)) {
            if (profd) prof_heap_range_cached++;
        } else {
            uint64_t range_check_start = profd ? SDL_GetTicksNS() : 0;
            bool committed = plat_guest_commit_sparse_range(addr, end - addr);
            if (profd) {
                prof_heap_range_checks++;
                prof_heap_range_check_ns += SDL_GetTicksNS() - range_check_start;
            }
            if (!committed) return false;
        }
    }
#endif
    if (gmem_sparse) gmem_info();
    if (!gmem_mapped && addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

// ---- shaders that store to global memory, and the CPU's copies of guest memory
// A shader with STG writes guest memory (the imported heap) on the GPU, while this thread goes on copying guest memory
// for later draws and dispatches (constant buffers; vertices and indices outside the heap). Run on the GPU while the
// rest of its list was translated, such a shader wrote memory being copied, the copy came out half-written, and the
// GPU faulted on a write (~1 world load in 7). So the rest of such a list went to the GPU in one piece at its end: in
// BOTW's open world ~10 compute dispatches a frame store results this way, the first ~100 draws into the frame, and
// the GPU sat idle until the whole frame was translated (~10 ms) and then rendered it (~8 ms), while the game waits
// for that frame's fence early in the next one.
// Now the memory such a shader may write - its storage buffers - goes into a set of ranges (they come back frame after
// frame). A constant buffer in it is read in place from the imported heap, as the console's GPU reads it (after the
// shader: each pass starts with a full barrier), not copied. Any other CPU copy of memory in the set (vertices or
// indices outside the heap, a constant buffer that can't be bound in place) first waits for the GPU to finish what
// was submitted. The list goes on to the GPU in pieces. A storing shader without storage buffers (it writes through
// other pointers), or a full set, holds the rest of its list as before.
// SAVAGEWIND_VK_GMEM_HOLD=1: hold after every storing shader (as before).
#define GW_MAX 4096
static struct { uint64_t lo, hi; } gw[GW_MAX];  // sorted, disjoint, not touching
static int ngw;
static uint64_t gw_lo = ~0ull, gw_hi;  // (bounds of the set: most reads are nowhere near it)

static int gw_find(uint64_t addr) {  // the first range ending after addr
    int a = 0, b = ngw;
    while (a < b) {
        int m = (a + b) / 2;
        if (gw[m].hi <= addr) a = m + 1; else b = m;
    }
    return a;
}

// The set changes rarely (it lasts the boot) while the same buffers are looked up every frame: answers are kept by
// range, until the set changes (gw_gen). The search was 3% of the render thread's time (4 cores, Rito).
// SAVAGEWIND_VK_NO_GW_MEMO=1: search every time.
uint32_t gw_gen = 1;
static bool gw_overlaps_exact(uint64_t lo, uint64_t hi);

// A bit a 1 MB block of the set (cpuopts bit 128), so that the many lookups nowhere near it end on one load. The
// bounds above take the reads outside [gw_lo, gw_hi), but the set's ranges lie all over the heap (the storage buffers
// of the compute passes) and so did the bounds: in a profile capture gw_overlaps, gw_ubo_copied and the memo
// of gw_check were 0.32 ms a frame of the queue thread, most of it on reads that answered "no". The bitmap is hashed
// by block number (4096 bits: the 4 GB the heap holds, one bit each), so a block that shares a bit with one in the set
// is asked of the exact search; no bit ever clears before the set does. A range wider than 8 blocks is always asked.
#define GW_BLOCK_SHIFT 20
#define GW_BLOCK_BITS 4096
static uint64_t gw_blocks[GW_BLOCK_BITS / 64];
static inline bool gw_maybe(uint64_t lo, uint64_t hi) {  // false: no range of the set overlaps [lo, hi)
    if (!ngw || hi <= gw_lo || lo >= gw_hi) return false;
    if (hi <= lo) return true;  // (empty or wrapped: the exact search answers)
    uint64_t b0 = lo >> GW_BLOCK_SHIFT, b1 = (hi - 1) >> GW_BLOCK_SHIFT;
    if (b1 - b0 >= 8) return true;
    for (uint64_t b = b0; b <= b1; b++)
        if (gw_blocks[(b & (GW_BLOCK_BITS - 1)) >> 6] >> (b & 63) & 1) return true;
    return false;
}
static void gw_blocks_add(uint64_t lo, uint64_t hi) {  // the blocks of a range just put in the set
    uint64_t b0 = lo >> GW_BLOCK_SHIFT, b1 = hi > lo ? (hi - 1) >> GW_BLOCK_SHIFT : b0;
    if (b1 - b0 >= GW_BLOCK_BITS) { memset(gw_blocks, 0xff, sizeof gw_blocks); return; }
    for (uint64_t b = b0; b <= b1; b++) gw_blocks[(b & (GW_BLOCK_BITS - 1)) >> 6] |= 1ull << (b & 63);
}
static inline bool gw_overlaps(uint64_t lo, uint64_t hi) {
    if ((vk_cpu_opts() & 128) && !gw_maybe(lo, hi)) return false;
    return gw_overlaps_exact(lo, hi);
}
static bool gw_overlaps_exact(uint64_t lo, uint64_t hi) {
    if (!ngw || hi <= gw_lo || lo >= gw_hi) return false;
    static struct { uint64_t lo, hi; uint32_t gen; bool r; } memo[1024];
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_VK_NO_GW_MEMO") == NULL;
    uint32_t k = (uint32_t)(((lo >> 6) ^ (hi >> 4)) * 0x9E3779B1u) >> 22;
    if (on && memo[k].gen == gw_gen && memo[k].lo == lo && memo[k].hi == hi) return memo[k].r;
    int i = gw_find(lo);
    bool r = i < ngw && gw[i].lo < hi;
    memo[k].lo = lo, memo[k].hi = hi, memo[k].gen = gw_gen, memo[k].r = r;
    return r;
}

static bool gw_add(uint64_t lo, uint64_t hi) {  // false: no room
    int i = gw_find(lo);
    if (i < ngw && gw[i].lo <= lo && hi <= gw[i].hi) return true;  // (already in)
    gw_gen++;
    if (i > 0 && gw[i - 1].hi == lo) i--;  // (touching on the left: merge)
    int j = i;
    while (j < ngw && gw[j].lo <= hi) j++;  // [i, j): overlapping or touching
    if (j == i && ngw == GW_MAX) return false;
    if (j > i) {
        if (gw[i].lo < lo) lo = gw[i].lo;
        if (gw[j - 1].hi > hi) hi = gw[j - 1].hi;
        memmove(&gw[i + 1], &gw[j], (ngw - j) * sizeof gw[0]);
        ngw -= j - i - 1;
    } else {
        memmove(&gw[i + 1], &gw[i], (ngw - i) * sizeof gw[0]);
        ngw++;
    }
    gw[i].lo = lo;
    gw[i].hi = hi;
    if (lo < gw_lo) gw_lo = lo;
    if (hi > gw_hi) gw_hi = hi;
    gw_blocks_add(lo, hi);
    return true;
}

static void gsnap_gpu_note(int stage);
void gw_note(int stage) {  // a shader of this stage that stores to global memory was recorded
    if (gsnap_mode()) gsnap_gpu_note(stage);  // (the GPU data list is read only by the copies: gsnap on)
    barr.vtx_hazard = true;  // (the next pass's vertex stage may read it)
    static int hold = -1;
    if (hold < 0) hold = getenv("SAVAGEWIND_VK_GMEM_HOLD") != NULL;
    if (barr.chunk_hold) return;  // (the rest of the list goes in one piece anyway)
    bool ok = !hold;
    int n = 0;
    for (int i = 0; i < 16 && ok; i++) {
        Range r = st.sbuf[stage][i];
        if (!r.addr || !r.size) continue;
        ok = gw_add(r.addr, r.addr + r.size);
        n++;
    }
    if (!ok || !n) { barr.chunk_hold = true; hc_all_stale = true; stat_gw_holds++; }
}

// A constant buffer bound in place (in the set, in the heap, aligned for a uniform buffer descriptor), or false.
bool gw_ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!gw_overlaps(addr, addr + size) || addr % g_vk.props.limits.minUniformBufferOffsetAlignment) return false;
    VkBuffer b;
    VkDeviceSize off;
    if (!guest_direct(addr, size, &b, &off)) return false;
    *out = (VkDescriptorBufferInfo){b, off, size};
    return true;
}

// Constant buffers read by the GPU in place, from the imported heap, instead of a copy per command list (guest_copy:
// about a tenth of the render thread at Rito). On an integrated GPU that memory is the GPU's own, so the reads cost
// what the copy's would, and the copy goes. A discrete GPU reads them across PCIe, a little GPU time (RTX 3060 at the
// castle, 1280x720: 4.5 -> 4.95 ms) for render thread time (7.6 -> 7.2 ms): where the CPU has 4 cores or fewer to run
// the game on, the render thread is the limit and it pays off (4 cores: Rito 84 -> 95 fps, the castle 129 -> 141).
// Read through the heap copy in video memory instead (guest_fetch) was slower on both sides. The game writes the next frame's constants while the GPU draws this one,
// as on the Switch, whose GPU also reads them in place: it keeps them apart and waits on its fences before it
// reuses them, and those fences come back once our GPU is done. What stays a copy: ranges UpdateUniformBuffer writes
// inside a list (uubt: draws before the update must not see it).
// Not with the GPU overlap (gpu_overlap, the default): its fence waits come one frame late, so the game rewrites
// constants the GPU has yet to read, which it can only do while they are copied. In place under it, the world turned
// magenta or white and textures flickered (#332: Vega 7 RADV with the recorder off every time, an RTX 3060 1 boot in 3).
// The overlap is worth more than the in-place reads (4 cores, 720p: in place without it 112 / 90 / 110 fps at the
// plateau / Rito / the castle, copies with it 125 / 96 / 141).
// SAVAGEWIND_VK_UBO_DIRECT=0/1: off / on whatever the GPU (on a discrete GPU the reads cross PCIe; with the overlap,
// wrong constants now and then).
// SAVAGEWIND_VK_UBO_CHECK=1: each direct range hashed when bound and again once its submission is done, mismatches
// counted (the CPU wrote it while the GPU could still read it; a write after the GPU finished counts too).
static int ubo_direct_on = -1, ubo_check;
#define UBOC_N 65536
static struct { uint64_t addr, size, hash, seq; } uboc[UBOC_N];
static uint64_t uboc_w, uboc_r, uboc_bad, uboc_n, uboc_lost, uboc_sub_bad, uboc_sub_n, uboc_s;
void ubo_check_submit(uint64_t seq) {  // (before submission seq goes to the GPU) its ranges hashed again: a change here is certain
    if (!ubo_check) return;
    if (uboc_s < uboc_r) uboc_s = uboc_r;
    for (; uboc_s < uboc_w && uboc[uboc_s % UBOC_N].seq <= seq; uboc_s++) {
        __typeof__(uboc[0]) *e = &uboc[uboc_s % UBOC_N];
        uboc_sub_n++;
        if (hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)e->addr, e->size) != e->hash) uboc_sub_bad++;
    }
}
static void ubo_check_add(uint64_t addr, uint64_t size) {
    if (uboc_w - uboc_r >= UBOC_N) { uboc_lost++; return; }
    __typeof__(uboc[0]) *e = &uboc[uboc_w++ % UBOC_N];
    e->addr = addr, e->size = size, e->seq = vk_pending_seq();
    e->hash = hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)addr, size);
}
void ubo_check_done(bool report) {  // (the stats line) the ranges whose submissions are done, hashed again
    if (!ubo_check) return;
    uint64_t done = vk_completed_seq();
    for (; uboc_r < uboc_w && uboc[uboc_r % UBOC_N].seq <= done; uboc_r++) {
        __typeof__(uboc[0]) *e = &uboc[uboc_r % UBOC_N];
        uboc_n++;
        if (hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)e->addr, e->size) != e->hash) uboc_bad++;
    }
    if (report) LOG("[vk] constant buffers in place: %llu checked, %llu changed before their submission was seen done, %llu not checked; at submission %llu checked, %llu changed\n",
                    (unsigned long long)uboc_n, (unsigned long long)uboc_bad, (unsigned long long)uboc_lost,
                    (unsigned long long)uboc_sub_n, (unsigned long long)uboc_sub_bad);
}
bool ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (ubo_direct_on < 0) {
        const char *e = getenv("SAVAGEWIND_VK_UBO_DIRECT");
        bool igpu = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        ubo_direct_on = e ? atoi(e) != 0 : (igpu || plat_cpu_count() <= 4) && !gpu_overlap();
        ubo_check = getenv("SAVAGEWIND_VK_UBO_CHECK") != NULL;
        LOG("[vk] constant buffers: %s%s\n", ubo_direct_on ? "read in place" : "copied per list (SAVAGEWIND_VK_UBO_DIRECT=1: in place)",
            ubo_direct_on && ubo_check ? ", checked" : "");
    }
    if (!ubo_direct_on || addr % g_vk.props.limits.minUniformBufferOffsetAlignment) return false;
    for (int i = 0; i < nuub; i++)
        if (addr < uubt[i].hi && uubt[i].lo < addr + size) return false;
    VkBuffer b;
    VkDeviceSize off;
    if (!guest_direct(addr, size, &b, &off)) return false;
    *out = (VkDescriptorBufferInfo){b, off, size};
    stat_ubo_direct++;
    if (ubo_check) ubo_check_add(addr, size);
    return true;
}

static bool gw_ubo_copied(int stage, uint32_t cbuf_mask) {  // a constant buffer the stage reads would be copied from the set
    for (uint32_t m = cbuf_mask >> 3; m; m &= m - 1) {  // (cbuf k is bound slot k - 3)
        int i = __builtin_ctz(m);
        if (i >= 18) break;
        Range r = st.ubuf[stage][i];
        uint64_t size = r.size ? (r.size > 0x10000 ? 0x10000 : r.size) : 0x10000;
        VkDescriptorBufferInfo d;
        if (r.addr && gw_overlaps(r.addr, r.addr + size) && !gw_ubo_direct(r.addr, size, &d)) return true;
    }
    return false;
}

// Whether any constant buffer a stage reads is in or near the set (cpuopts bit 128): the cheap form of gw_ubo_copied's
// loop, bitmap only. False: none overlaps, so none is copied from the set (the answer needs no memo).
static inline bool gw_ubo_near(int stage, uint32_t cbuf_mask) {
    for (uint32_t m = cbuf_mask >> 3; m; m &= m - 1) {
        int i = __builtin_ctz(m);
        if (i >= 18) break;
        Range r = st.ubuf[stage][i];
        uint64_t size = r.size ? (r.size > 0x10000 ? 0x10000 : r.size) : 0x10000;
        if (r.addr && gw_maybe(r.addr, r.addr + size)) return true;
    }
    return false;
}

// Before a draw's or dispatch's copies (and before its pass begins): copies of memory in the set wait for the GPU.
// cbufs[stage]: the constant buffers each stage reads (ShaderInfo.cbuf_mask; 0: none).
void gw_check(const uint32_t cbufs[6], bool vertices, uint64_t iaddr, uint64_t isize) {
    if (!ngw) return;
    static uint64_t chk_vbuf_gen, chk_vbuf_gw, chk_ubuf_gen[6], chk_ubuf_gw[6], chk_ubuf_gc[6];
    static uint32_t chk_cbuf_mask[6];
    uint64_t gc = __atomic_load_n(&gcopy_gen, __ATOMIC_RELAXED);
    int opts = vk_cpu_opts();
    bool wait = false;
    for (int s = 0; s < 6 && !wait; s++) {
        uint32_t m = cbufs[s];
        if (!m) continue;
        if ((opts & 128) && !gw_ubo_near(s, m)) continue;
        if (chk_ubuf_gen[s] == ubuf_gen[s] && chk_ubuf_gw[s] == gw_gen && chk_ubuf_gc[s] == gc && (chk_cbuf_mask[s] & m) == m)
            continue;
        wait = gw_ubo_copied(s, m);
        if (!wait) {
            if (chk_ubuf_gen[s] != ubuf_gen[s] || chk_ubuf_gw[s] != gw_gen || chk_ubuf_gc[s] != gc) {
                chk_ubuf_gen[s] = ubuf_gen[s]; chk_ubuf_gw[s] = gw_gen; chk_ubuf_gc[s] = gc; chk_cbuf_mask[s] = m;
            } else {
                chk_cbuf_mask[s] |= m;
            }
        }
    }
    VkBuffer b;
    VkDeviceSize off;
    // (the overlap first: it is rarely there, and guest_direct is the dearer of the two)
    if (vertices && !wait && (chk_vbuf_gen != vbuf_gen || chk_vbuf_gw != gw_gen)) {
        for (int i = 0; i < 16 && !wait; i++) {
            Range v = st.vbuf[i];
            uint64_t n = v.size ? v.size : 256;
            if (v.addr && gw_overlaps(v.addr, v.addr + n)) wait = !guest_direct(v.addr, n, &b, &off);
        }
        if (!wait) { chk_vbuf_gen = vbuf_gen; chk_vbuf_gw = gw_gen; }
    }
    if (iaddr && !wait && gw_overlaps(iaddr, iaddr + isize)) wait = !guest_direct(iaddr, isize, &b, &off);
    if (!wait) return;
    stat_gw_waits++;
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_wait_seq(submit_seq);
    guest_copy_flush();  // (the list's shared copies may hold what was there before)
}

// ---- vertices and indices from a copy of the heap in video memory (discrete GPUs, Windows)
// The imported heap is system memory on a discrete GPU, so every draw fetched its vertices and indices across PCIe.
// Now they come from a device-local copy of the heap holding the geometry pages: the 4K pages vertex and index
// fetches read (hc_geo). Before each list, the ones the CPU wrote since (Windows' write watch, plat_written_pages)
// and the ones fetched for the first time are copied over from the import, on the GPU, ahead of the list's draws.
// A fetch from pages the copy doesn't hold as they are now reads the import in place, as before, and its pages join
// the next list's copy. What shaders store to (the gw set) is always read in place; CPU writes during a list
// (UpdateUniformBuffer, CopyBufferToBuffer, readbacks) take their pages out of the copy until the next one.
// Write-watch queries cover only spans around the geometry pages held. One that resets (GetWriteWatch with
// WRITE_WATCH_FLAG_RESET) costs ~10-15 us a call and ~1 us a MB it covers; one that only looks, ~0.6 us a call. So
// the spans hug the geometry (up to 128 of them: ~140 MB for BOTW's ~50 MB), each is looked at first, and only the
// ones written since (~8 a list) are queried and reset: 0.33 ms a list instead of 0.54 (6 spans over ~450 MB).
// SAVAGEWIND_VK_HEAP_COPY_SPANS=<n>: at most n spans; SAVAGEWIND_VK_HEAP_COPY_PEEK=0: every span queried and reset.
// Two shapes. One buffer the heap's size, made at once: only with SAVAGEWIND_VK_HEAP_COPY=1 now. It was the shape of
// cards with room for the whole heap and 4 GB besides it (8 GB and up), but the 3120 MB of video memory are also
// 3120 MB of the process's commit charge (NVIDIA's Windows driver backs video memory with committed, untouched system
// memory; AMD's too), which a PC with 8 GB of RAM cannot cover: private bytes 8348 MB against 5868 MB in chunks at the
// Great Plateau (RTX 3060, 1080p), with the same frame rate in 4 views at 1080p and 1440p (within the 3% between runs).
// The default on every discrete card: the heap in chunks of HC_CHUNK_MB, each a buffer of its own
// made where geometry is first fetched - on a thread of their own (hc_maker: a block of video memory can take
// milliseconds), live from the list after - while the video memory in use (VK_EXT_memory_budget: the process's;
// else VMA's count) stays within the plan: the smaller of the budget and 90% of the video memory, less HC_RESERVE_MB
// for the game's own growth. A fetch inside one live chunk binds that chunk's buffer at its offset in it; one across a
// chunk's edge, or in a chunk not made (over the plan), reads the import in place as before. Every HC_LOOK lists:
// over the plan (the game's own memory grew), the least fetched live chunk goes (destroyed once the GPU is past it);
// within it, chunks refused before may be made; and a chunk the GPU read in place much more than the least fetched
// live one takes its place. BOTW's geometry (25-56 MB of the 3120 MB heap) sat in 17-20 chunks of 32 MB (~0.6 GB) in
// every place tried, with ~1 fetch a list across a chunk's edge.
// Off on integrated GPUs (the import is already their memory), outside Windows, and with SAVAGEWIND_VK_HEAP_COPY=0
// (=1: one buffer whatever the room, =2: chunks whatever the room). SAVAGEWIND_VK_HEAP_COPY_BUDGET_MB=<n>: plan for a
// card of n MB of video memory (the shape, the chunks' plan and size: a smaller card simulated). _CHUNK_MB=<n>: the
// chunk size (a power of 2, 4-1024). SAVAGEWIND_VK_HEAP_COPY_LOG=<lists>: what was copied, every that many lists.
// (Approach from Demetter's fork, savagewind-native-28oct; reimplemented; chunks ours.)
#define HC_PAGE 4096ull
#define HC_SPANS_MAX 256
#define HC_MAXCH 1024
#define HC_CHUNK_MB 32
// Where the plan less HC_RESERVE_MB is lower, the plan is the point textures.c starts letting images go
// (VRAM_TRIM_TARGET of the budget) less one VMA block (vram_block_size): the copy takes what the textures leave and
// keeps a block's room for the game's own growth under that point. An MX350 (1669 MB budget, blocks of 64 MB): 1157 MB
// was about what the game's own blocks hold once loaded - after a load peak (a shrine entered from the open world) they
// stayed over it, every chunk went and none fitted again, the geometry read in place (scene pass 2.7 -> 12 ms, 57 -> 33
// fps) - and the plan is 1355 MB now. Budgets over ~1.7 GB with 256 MB blocks keep the reserve's plan.
// Cards of VRAM_SMALL_HEAP or less: chunks of HC_CHUNK_SMALL_MB. BOTW's geometry is scattered over the heap: in 32 MB
// chunks it took 17-20 of them (~600 MB), in 8 MB chunks 33-44 (~260-350 MB).
#define HC_CHUNK_SMALL_MB 8
#define HC_RESERVE_MB 512
#define HC_LOOK 120
enum { HC_NONE, HC_WANT, HC_MAKING, HC_MADE, HC_LIVE, HC_DYING, HC_OVER };  // a chunk's state
typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    _Atomic int state;       // HC_*: HC_MAKING -> HC_MADE / HC_OVER by hc_maker, every other change by this thread
    uint64_t fetches, missed;  // fetches from it / in place for want of it, since the last look
    uint64_t dead_seq;         // (HC_DYING) destroyed once this submission completes
} HcChunk;
static int hc_state = -1;  // -1: not decided yet, 0: off, 1: one buffer, 2: chunks
static HcChunk hc_ch[HC_MAXCH];
static int hc_nch;              // chunks the heap spans (one buffer: 1); 0: no copy
static unsigned hc_shift;       // a heap offset's chunk: offset >> hc_shift
static uint64_t hc_vram;        // video memory planned for (the device's, or SAVAGEWIND_VK_HEAP_COPY_BUDGET_MB)
static uint64_t hc_size;          // bytes of the heap it mirrors (gmem_size when made)
static uint64_t *hc_geo, *hc_ok;  // a bit per page: geometry / held by the copy as it is now
static size_t hc_words;
static uint64_t *hc_pages;  // (written pages, plat_written_pages)
static size_t hc_pages_max;
static struct { uint64_t lo, hi; } hc_span[HC_SPANS_MAX];  // heap offsets the write-watch queries cover
static int hc_nspans, hc_spans_cap = 128;  // (SAVAGEWIND_VK_HEAP_COPY_SPANS)
static bool hc_geo_new;  // pages became geometry (or their chunk live) since the spans were made
static bool hc_want_any, hc_dying_any;
static uint64_t hc_lists;  // (chunks: lists since made, for HC_LOOK)
static uint64_t hc_stat_lists, hc_stat_bytes, hc_stat_runs, hc_stat_inplace, hc_stat_mirror, hc_stat_query_ns, hc_stat_written;
static uint64_t hc_stat_edge, hc_stat_nochunk, hc_made_total, hc_dropped_total, hc_stat_resets;
static pthread_mutex_t hc_make_lock = PTHREAD_MUTEX_INITIALIZER;  // (hc_maker making a chunk; hc_free waits for it)
static pthread_cond_t hc_make_cond = PTHREAD_COND_INITIALIZER;
static bool hc_make_req, hc_maker_up;
static uint64_t hc_gen;  // (under hc_make_lock) bumped by hc_free: a chunk made for the copy before is destroyed

static uint64_t hc_base(int c) { return (uint64_t)c << hc_shift; }
static uint64_t hc_chunk_size(int c) { uint64_t b = hc_base(c); return hc_nch == 1 ? hc_size : (hc_size - b < (1ull << hc_shift) ? hc_size - b : 1ull << hc_shift); }
static bool hc_live(int c) { return atomic_load_explicit(&hc_ch[c].state, memory_order_relaxed) == HC_LIVE; }

// Whether any page of [lo, hi) holds geometry the copy mirrors: its write-watch flags are the copy's to reset, not a
// texture's (textures.c)
bool vk_heap_geometry_any(uint64_t lo, uint64_t hi) {
    if (hc_state <= 0 || !hc_geo || hi <= HEAP_BASE || lo >= hi) return false;
    uint64_t p0 = lo > HEAP_BASE ? (lo - HEAP_BASE) / HC_PAGE : 0, p1 = (hi - HEAP_BASE + HC_PAGE - 1) / HC_PAGE;
    if (p1 > (uint64_t)hc_words * 64) p1 = (uint64_t)hc_words * 64;
    for (uint64_t p = p0; p < p1;) {
        uint64_t w = p >> 6, take = 64 - (p & 63);
        if (take > p1 - p) take = p1 - p;
        uint64_t m = (take == 64 ? ~0ull : (1ull << take) - 1) << (p & 63);
        if (hc_geo[w] & m) return true;
        p += take;
    }
    return false;
}

// What the plan lets video memory in use come to (chunks), and what is in use now
static uint64_t hc_limit(uint64_t *usage) {
    uint64_t budget;
    vram_device_local(usage, &budget, NULL);
    uint64_t plan = (budget < hc_vram / 10 * 9 ? budget : hc_vram / 10 * 9), reserve = (uint64_t)HC_RESERVE_MB << 20;
    uint64_t lim = plan > reserve ? plan - reserve : 0, block = vram_block_size();
    uint64_t trim = (uint64_t)((double)budget * VRAM_TRIM_TARGET), at_trim = trim > block ? trim - block : 0;
    if (at_trim > plan) at_trim = plan;
    return lim > at_trim ? lim : at_trim;  // (see HC_CHUNK_SMALL_MB)
}

static bool hc_on(void) {
    if (hc_state >= 0) return hc_state;
    const char *e = getenv("SAVAGEWIND_VK_HEAP_COPY");
    int force = e ? atoi(e) : -1;
    uint64_t probe;
    bool ww = plat_written_pages(HEAP_BASE, HC_PAGE, &probe, 2) >= 0;
    vk_tex_range_written(HEAP_BASE, HC_PAGE);  // (a texture there lost its flag to the probe)
    uint64_t vram;
    vram_device_local(NULL, NULL, &vram);
    const char *bm = getenv("SAVAGEWIND_VK_HEAP_COPY_BUDGET_MB");
    hc_vram = bm && atoll(bm) > 0 ? (uint64_t)atoll(bm) << 20 : vram;
    bool discrete = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    uint64_t heap = kernel_heap_size();
    hc_state = force == 0 || gmem_mapped || !ww ? 0 : force == 1 ? 1 : force == 2 ? 2 : !discrete ? 0 : 2;
    const char *cm = getenv("SAVAGEWIND_VK_HEAP_COPY_CHUNK_MB");
    unsigned def = hc_vram <= VRAM_SMALL_HEAP ? HC_CHUNK_SMALL_MB : HC_CHUNK_MB;
    unsigned mb = cm ? (unsigned)atoi(cm) : def;
    if (mb < 4 || mb > 1024 || (mb & (mb - 1))) mb = def;
    hc_shift = 20 + (unsigned)__builtin_ctz(mb);
    uint64_t usage, lim = hc_limit(&usage);
    LOG("[vk] heap copy in video memory: %s (%s GPU, %llu MB of video memory%s, heap %llu MB, write watch %s)\n",
        hc_state == 1 ? "on, one buffer" : hc_state == 2 ? "on, in chunks" : "off", discrete ? "discrete" : "not a discrete",
        (unsigned long long)(hc_vram >> 20), bm ? " (SAVAGEWIND_VK_HEAP_COPY_BUDGET_MB)" : "", (unsigned long long)(heap >> 20),
        ww ? "yes" : "no");
    if (hc_state == 2)
        LOG("[vk] heap copy: chunks of %u MB while video memory in use stays within %llu MB (%llu MB in use now, budget %s)\n",
            mb, (unsigned long long)(lim >> 20), (unsigned long long)(usage >> 20),
            vram_budget_ext() ? "VK_EXT_memory_budget" : "VMA's estimate");
    return hc_state;
}

// (thread) makes the chunks asked for (HC_MAKING): a buffer of video memory each, outside the lock
static void *hc_maker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&hc_make_lock);
    for (;;) {
        while (!hc_make_req) pthread_cond_wait(&hc_make_cond, &hc_make_lock);
        hc_make_req = false;
        for (int c = 0; c < hc_nch; c++) {
            if (atomic_load(&hc_ch[c].state) != HC_MAKING) continue;
            uint64_t gen = hc_gen;
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hc_chunk_size(c),
                                     .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                          .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
            VkBuffer b;
            VmaAllocation m;
            pthread_mutex_unlock(&hc_make_lock);
            bool ok = vmaCreateBuffer(g_vk.vma, &bi, &ai, &b, &m, NULL) == VK_SUCCESS;
            pthread_mutex_lock(&hc_make_lock);
            if (gen != hc_gen) {  // (hc_free meanwhile: for a copy gone)
                if (ok) vmaDestroyBuffer(g_vk.vma, b, m);
                break;
            }
            if (ok) hc_ch[c].buf = b, hc_ch[c].mem = m;
            atomic_store(&hc_ch[c].state, ok ? HC_MADE : HC_OVER);
        }
    }
    return NULL;
}

static void hc_chunk_destroy(int c) {
    vmaDestroyBuffer(g_vk.vma, hc_ch[c].buf, hc_ch[c].mem);
    hc_ch[c].buf = VK_NULL_HANDLE;
    hc_ch[c].mem = NULL;
}

void hc_free(void) {
    pthread_mutex_lock(&hc_make_lock);  // (hc_maker between chunks)
    bool any = false;
    for (int c = 0; c < hc_nch; c++) any |= hc_ch[c].buf != VK_NULL_HANDLE;
    if (any) {
        vk_wait_seq(submit_seq);
        vk_dyn_forget();  // (a new buffer may get the old handle: nothing recorded may be taken as still bound)
        setc_gen++;
        for (int c = 0; c < hc_nch; c++)
            if (hc_ch[c].buf) hc_chunk_destroy(c);
    }
    memset(hc_ch, 0, sizeof hc_ch);
    hc_nch = 0;
    hc_gen++;
    pthread_mutex_unlock(&hc_make_lock);
    hc_size = 0;
    free(hc_geo), free(hc_ok), free(hc_pages);
    hc_geo = hc_ok = hc_pages = NULL;
    hc_nspans = 0;
    hc_want_any = hc_dying_any = false;
}

static bool hc_ready(void) {  // the copy, made for the heap as imported now
    if (!gmem_buf || !hc_on()) return false;
    if (hc_size == gmem_size) return hc_nch > 0;
    hc_free();
    hc_size = gmem_size;
    while (hc_state == 2 && (hc_size + (1ull << hc_shift) - 1) >> hc_shift > HC_MAXCH) hc_shift++;
    if (hc_state == 1) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = gmem_size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &hc_ch[0].buf, &hc_ch[0].mem, NULL) != VK_SUCCESS) {
            LOG("[vk] heap copy: no room for %llu MB in video memory, in chunks\n", (unsigned long long)(gmem_size >> 20));
            hc_ch[0].buf = VK_NULL_HANDLE;
            hc_state = 2;
            hc_size = 0;
            return hc_ready();
        }
        hc_shift = 63;
        hc_nch = 1;
        atomic_store(&hc_ch[0].state, HC_LIVE);
        LOG("[vk] heap copy: %llu MB in video memory\n", (unsigned long long)(gmem_size >> 20));
    } else {
        pthread_mutex_lock(&hc_make_lock);
        hc_nch = (int)((hc_size + (1ull << hc_shift) - 1) >> hc_shift);
        pthread_mutex_unlock(&hc_make_lock);
        if (!hc_maker_up) {
            pthread_t t;
            hc_maker_up = pthread_create(&t, NULL, hc_maker, NULL) == 0;
            if (hc_maker_up) pthread_detach(t);
            else { hc_nch = 0; hc_state = 0; return false; }
        }
        hc_lists = 0;
    }
    hc_words = (size_t)((gmem_size / HC_PAGE + 63) / 64);
    hc_geo = calloc(hc_words, 8), hc_ok = calloc(hc_words, 8);
    hc_pages_max = 65536;
    hc_pages = malloc(hc_pages_max * sizeof *hc_pages);
    return true;
}

// [lo, hi) of the heap (offsets) no longer as the copy holds it
static void hc_stale(uint64_t lo, uint64_t hi) {
    if (!hc_ok) return;
    if (hi > hc_size) hi = hc_size;
    for (uint64_t p = lo / HC_PAGE; p < (hi + HC_PAGE - 1) / HC_PAGE; p++) hc_ok[p >> 6] &= ~(1ull << (p & 63));
}

// The CPU wrote guest memory during a list (the write watch tells the next list)
void hc_cpu_wrote(uint64_t addr, uint64_t size) {
    if (hc_ok && addr >= HEAP_BASE && addr < HEAP_BASE + hc_size) hc_stale(addr - HEAP_BASE, addr - HEAP_BASE + size);
}

// (chunks: a word of hc_geo / hc_ok, 64 pages, lies in one chunk)
static int hc_word_chunk(size_t w) { return (int)(((uint64_t)w * 64 * HC_PAGE) >> hc_shift); }

// The spans the queries cover: runs of geometry words (64 pages, 256 KB) of live chunks joined across gaps, with the
// smallest gap that leaves at most hc_spans_cap of them
static void hc_spans_make(void) {
    static bool env;
    if (!env) {
        env = true;
        const char *e = getenv("SAVAGEWIND_VK_HEAP_COPY_SPANS");
        if (e && atoi(e) > 0) hc_spans_cap = atoi(e) < HC_SPANS_MAX ? atoi(e) : HC_SPANS_MAX;
    }
    for (size_t gap = 1;; gap *= 2) {
        int n = 0;
        size_t w = 0;
        bool over = false;
        while (w < hc_words) {
            while (w < hc_words && !(hc_geo[w] && hc_live(hc_word_chunk(w)))) w++;
            if (w == hc_words) break;
            size_t lo = w, last = w;
            for (; w < hc_words && w - last <= gap; w++)
                if (hc_geo[w] && hc_live(hc_word_chunk(w))) last = w;
            if (n == hc_spans_cap) { over = true; break; }
            hc_span[n].lo = (uint64_t)lo * 64 * HC_PAGE;
            hc_span[n].hi = (uint64_t)(last + 1) * 64 * HC_PAGE;
            if (hc_span[n].hi > hc_size) hc_span[n].hi = hc_size;
            n++;
            w = last + 1;
        }
        if (!over) { hc_nspans = n; break; }
    }
    hc_geo_new = false;
}

// A live chunk goes: out of the copy now, its buffer destroyed once the GPU is past what was recorded with it
static void hc_chunk_drop(int c) {
    atomic_store(&hc_ch[c].state, HC_DYING);
    hc_ch[c].dead_seq = vk_pending_seq();
    hc_stale(hc_base(c), hc_base(c) + hc_chunk_size(c));
    hc_dying_any = true;
    hc_geo_new = true;  // (spans without it)
    hc_dropped_total++;
}

// (chunks, as a list begins) made chunks go live, dead ones are destroyed, wanted ones are asked for within the plan,
// and every HC_LOOK lists the plan is looked at again
static void hc_chunks_step(void) {
    int live = 0, making = 0;
    bool req = false;
    uint64_t done = hc_dying_any ? vk_completed_seq() : 0;
    bool dying = false;
    for (int c = 0; c < hc_nch; c++) {
        int s = atomic_load(&hc_ch[c].state);
        if (s == HC_MADE) {  // (its pages aren't in hc_ok: this list copies them)
            atomic_store(&hc_ch[c].state, s = HC_LIVE);
            hc_geo_new = true;
            hc_made_total++;
        } else if (s == HC_DYING) {
            if (done >= hc_ch[c].dead_seq) {
                vk_dyn_forget();  // (as hc_free)
                setc_gen++;
                hc_chunk_destroy(c);
                atomic_store(&hc_ch[c].state, s = HC_NONE);
            } else dying = true;
        }
        live += s == HC_LIVE;
        making += s == HC_MAKING;
    }
    hc_dying_any = dying;
    uint64_t chunk = 1ull << hc_shift;
    bool look = ++hc_lists % HC_LOOK == 0;
    if (hc_want_any || look) {
        uint64_t usage, lim = hc_limit(&usage);
        usage += (uint64_t)making * chunk;
        if (look) {
            int worst = -1, best = -1;  // (the least fetched live chunk, the most missed one not made)
            for (int c = 0; c < hc_nch; c++) {
                int s = atomic_load(&hc_ch[c].state);
                if (s == HC_LIVE && (worst < 0 || hc_ch[c].fetches < hc_ch[worst].fetches)) worst = c;
                if ((s == HC_OVER || s == HC_WANT) && (best < 0 || hc_ch[c].missed > hc_ch[best].missed)) best = c;
            }
            if (worst >= 0 && usage > lim) hc_chunk_drop(worst), live--;  // (over the plan: one a look)
            else if (worst >= 0 && best >= 0 && hc_ch[best].missed > 64 && hc_ch[best].missed > 4 * hc_ch[worst].fetches + 64 &&
                     usage + chunk > lim)
                hc_chunk_drop(worst), live--;  // (the room for a busier one, made from a later look)
            for (int c = 0; c < hc_nch; c++) {
                if (atomic_load(&hc_ch[c].state) == HC_OVER) atomic_store(&hc_ch[c].state, hc_ch[c].missed ? HC_WANT : HC_NONE);
                hc_ch[c].fetches = hc_ch[c].missed = 0;
            }
            hc_want_any = true;
        }
        if (hc_want_any) {
            hc_want_any = false;
            for (;;) {  // (the most missed first)
                int best = -1;
                for (int c = 0; c < hc_nch; c++)
                    if (atomic_load(&hc_ch[c].state) == HC_WANT && (best < 0 || hc_ch[c].missed > hc_ch[best].missed)) best = c;
                if (best < 0) break;
                if (usage + chunk > lim || hc_dying_any) { atomic_store(&hc_ch[best].state, HC_OVER); continue; }
                atomic_store(&hc_ch[best].state, HC_MAKING);
                usage += chunk;
                req = true;
            }
        }
    }
    if (req) {
        pthread_mutex_lock(&hc_make_lock);
        hc_make_req = true;
        pthread_cond_signal(&hc_make_cond);
        pthread_mutex_unlock(&hc_make_lock);
    }
}

static void hc_copy_flush(VkCommandBuffer *cb, VkBuffer dst, VkBufferCopy *rg, int *nrg) {
    if (!*nrg) return;
    if (!*cb) {
        *cb = vk_upload_cmd();
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,  // (earlier lists' fetches of these pages)
                               .srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT};
        rec_CmdPipelineBarrier2(*cb, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                                         .pMemoryBarriers = &mb});
    }
    rec_CmdCopyBuffer(*cb, gmem_buf, dst, (uint32_t)*nrg, rg);
    for (int i = 0; i < *nrg; i++) hc_stat_bytes += rg[i].size;
    hc_stat_runs += (uint64_t)*nrg;
    *nrg = 0;
}

// A list begins: the written geometry pages leave the copy, and every geometry page of a live chunk it doesn't hold
// is copied over (runs of pages, in the list's upload commands: before its draws)
void hc_list_begin(void) {
    if (!hc_ready()) return;
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t b0 = hc_stat_bytes, r0 = hc_stat_runs, w0 = hc_stat_written;  // (the slow-list line below)
    if (hc_state == 2) hc_chunks_step();
    bool remade = hc_geo_new, all = hc_all_stale;
    int full = 0;
    if (hc_geo_new) hc_spans_make();
    if (hc_all_stale) memset(hc_ok, 0, hc_words * 8), hc_all_stale = false;
    static int peek = -1;
    if (peek < 0) peek = !getenv("SAVAGEWIND_VK_HEAP_COPY_PEEK") || atoi(getenv("SAVAGEWIND_VK_HEAP_COPY_PEEK"));
    for (int i = 0; i < hc_nspans; i++) {
        // (a write after the look is in the next list's query, as one after the query is)
        if (peek && plat_any_written(HEAP_BASE + hc_span[i].lo, hc_span[i].hi - hc_span[i].lo) == 0) continue;
        hc_stat_resets++;
        long n = plat_written_pages(HEAP_BASE + hc_span[i].lo, hc_span[i].hi - hc_span[i].lo, hc_pages, hc_pages_max);
        // (the flags it reset are textures' too: they read them from vk_tex_pages_written, textures.c)
        if (n < 0) { hc_stale(hc_span[i].lo, hc_span[i].hi); full++; vk_tex_range_written(HEAP_BASE + hc_span[i].lo, hc_span[i].hi - hc_span[i].lo); continue; }
        hc_stat_written += (uint64_t)n;
        vk_tex_pages_written(hc_pages, n);
        for (long k = 0; k < n; k++) {
            uint64_t p = (hc_pages[k] - HEAP_BASE) / HC_PAGE;
            hc_ok[p >> 6] &= ~(1ull << (p & 63));
        }
    }
    uint64_t tq = SDL_GetTicksNS() - t0;
    hc_stat_query_ns += tq;
    VkBufferCopy rg[256];
    int nrg = 0, rc = -1;  // (rc: the chunk rg's runs go to)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (size_t w = 0; w < hc_words; w++) {
        uint64_t need = hc_geo[w] & ~hc_ok[w];
        if (!need) continue;
        int c = hc_nch == 1 ? 0 : hc_word_chunk(w);
        if (!hc_live(c)) continue;  // (not held: fetched in place)
        if (c != rc) hc_copy_flush(&cb, rc >= 0 ? hc_ch[rc].buf : VK_NULL_HANDLE, rg, &nrg), rc = c;
        uint64_t base = hc_base(c);
        hc_ok[w] |= hc_geo[w];
        for (; need; need &= need - 1) {
            uint64_t off = ((uint64_t)w * 64 + (uint64_t)__builtin_ctzll(need)) * HC_PAGE;
            if (nrg && rg[nrg - 1].srcOffset + rg[nrg - 1].size == off) { rg[nrg - 1].size += HC_PAGE; continue; }
            if (nrg == 256) hc_copy_flush(&cb, hc_ch[c].buf, rg, &nrg);
            rg[nrg++] = (VkBufferCopy){off, off - base, HC_PAGE};
        }
    }
    if (rc >= 0) hc_copy_flush(&cb, hc_ch[rc].buf, rg, &nrg);
    if (cb) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                               .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT};
        rec_CmdPipelineBarrier2(cb, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                                        .pMemoryBarriers = &mb});
    }
    uint64_t tt = SDL_GetTicksNS() - t0;
    if (tt > 5000000) {  // a list the copy held up: what it did
        uint64_t spanned = 0;
        for (int i = 0; i < hc_nspans; i++) spanned += hc_span[i].hi - hc_span[i].lo;
        LOG("[vk] heap copy, slow list: %.1f ms (query %.1f ms over %d spans, %.0f MB; %llu pages written, %d spans over the "
            "page list), %.1f MB copied in %llu runs%s%s\n", tt / 1e6, tq / 1e6, hc_nspans, spanned / 1048576.0,
            (unsigned long long)(hc_stat_written - w0), full, (hc_stat_bytes - b0) / 1048576.0,
            (unsigned long long)(hc_stat_runs - r0), remade ? ", spans remade" : "", all ? ", all stale" : "");
    }
    static int every = -1;
    if (every < 0) every = getenv("SAVAGEWIND_VK_HEAP_COPY_LOG") ? atoi(getenv("SAVAGEWIND_VK_HEAP_COPY_LOG")) : 0;
    if (every > 0 && ++hc_stat_lists >= (uint64_t)every) {
        size_t geo = 0, geo_held = 0;
        for (size_t w = 0; w < hc_words; w++) {
            geo += (size_t)__builtin_popcountll(hc_geo[w]);
            if (hc_live(hc_nch == 1 ? 0 : hc_word_chunk(w))) geo_held += (size_t)__builtin_popcountll(hc_geo[w]);
        }
        double f = (double)hc_stat_lists;
        uint64_t spanned = 0;
        for (int i = 0; i < hc_nspans; i++) spanned += hc_span[i].hi - hc_span[i].lo;
        LOG("[vk] heap copy, a list: %.1f KB copied in %.1f runs, %.0f pages written, query %.3f ms over %d spans (%.0f MB, %.1f reset); "
            "fetches %.0f from the copy, %.0f in place (%.0f across a chunk's edge, %.0f in chunks not held); geometry %.1f MB "
            "(%.1f MB in held chunks)\n", hc_stat_bytes / f / 1024, hc_stat_runs / f, hc_stat_written / f,
            hc_stat_query_ns / f / 1e6, hc_nspans, spanned / 1048576.0, hc_stat_resets / f, hc_stat_mirror / f, hc_stat_inplace / f, hc_stat_edge / f,
            hc_stat_nochunk / f, geo * HC_PAGE / 1048576.0, geo_held * HC_PAGE / 1048576.0);
        if (hc_state == 2) {
            int n[HC_OVER + 1] = {0};
            for (int c = 0; c < hc_nch; c++) n[atomic_load(&hc_ch[c].state)]++;
            uint64_t usage, lim = hc_limit(&usage);
            LOG("[vk] heap copy, chunks of %u MB: %d live (%d made, %d dropped so far), %d being made, %d over the plan, "
                "%d going; video memory in use %llu MB of %llu MB\n", 1u << (hc_shift - 20), n[HC_LIVE],
                (int)hc_made_total, (int)hc_dropped_total, n[HC_MAKING] + n[HC_MADE], n[HC_OVER] + n[HC_WANT], n[HC_DYING],
                (unsigned long long)(usage >> 20), (unsigned long long)(lim >> 20));
        }
        hc_stat_lists = hc_stat_bytes = hc_stat_runs = hc_stat_inplace = hc_stat_mirror = hc_stat_query_ns = hc_stat_written = 0;
        hc_stat_edge = hc_stat_nochunk = hc_stat_resets = 0;
    }
}

// (GPU overlap) Vertex and index data the CPU rewrites every frame, copied at translation (#620, #629, #683, #717).
// The overlap tells the game a frame is done one frame early (renderer_wait_seq), and BOTW writes some of its vertex
// data into a ring it reuses after that wait: rain and torch particles, read in place, came out stretched across the
// screen for a frame where the GPU was a frame behind (RTX 5060 at 160 fps, the Deck with frame generation). Copying
// every small fetch cost 151 -> 136 fps in heavy rain (RTX 3060, 1080p); turning the overlap off, 150 -> 95. The data
// that raced, in the Lanayru rain, was 129 B - 2 KB fetches all within 2 MB of the heap; the rest of the in-place
// fetches the CPU writes did not change while read. So the regions are learned: some small fetches are hashed when
// recorded and again right after the real wait on their submission; a change (the CPU wrote it while the GPU could
// still read it) marks its region, and from then on every fetch in a marked region is copied. Never GPU data (gw).
// The samples go to the waiting thread through a ring the render thread never waits on (one in 4 under a lock cost
// 4% of the frame rate). In the rain the ring was learned in its first second, and nothing the CPU wrote changed while
// read after that; 150.9 -> 149.5 fps (4 runs each, interleaved).
// A region is marked only once it changed while read in VDYN_HITS different frames, each within a second of the last,
// and not on a loading screen: a fast travel reuses freed memory for new meshes once, and its load marked ~30 regions in
// one go; every fetch in them then went through the copy for the rest of the session (upload 200 -> 1200 MB per 2 s,
// 60 -> 54 fps on a GTX 1070, #815). The ring that races is rewritten every frame, and is still marked in a few frames.
// In a marked region only fetches up to VDYN_COPY_MAX are copied (the ring's are 129 B - 2 KB), not the meshes around it.
// Vertex data GPU-written too (gw: storing shaders write it as well) is learned and copied the same way: BOTW's rain
// particles are such a ring, both the CPU and its shaders write it. It was left out (read in place: a copy made at
// translation misses what a shader writes before the draw), and on a GPU a frame behind the CPU the rain came out as
// streaks across the screen (#956: an RTX 2060 at 120 fps; its 6 GB heap copy in chunks). Only a region seen changed
// while read in VDYN_HITS frames is copied, as for the rest (the rain's ring: one region); every small GPU-written fetch
// is sampled, not one in 32, so that ring is learned in the rain's first frames.
// SAVAGEWIND_VK_VDYN=0: off; =2: every fetch up to 4 KB copied (no learning, the cost above).
#define VDYN_N 4096
#define VDYN_SAMPLE_MAX 4096
#define VDYN_SHIFT 20  // (a region: 1 MB; 64 KB ones took 10 marks to cover the ring instead of 2)
#define VDYN_SMASK 31  // (one small fetch in 32 sampled)
#define VDYN_HITS 3
#define VDYN_COPY_MAX (64u << 10)
static int vdyn_mode = -1;
static _Atomic uint8_t vdyn[HEAP_MAX_LARGEST >> VDYN_SHIFT];  // (a region: 1 once marked, see above)
static uint8_t vdyn_hits[HEAP_MAX_LARGEST >> VDYN_SHIFT];       // (vdyn_lock: frames it changed in, in a row)
static uint64_t vdyn_last[HEAP_MAX_LARGEST >> VDYN_SHIFT];      // (vdyn_lock: the frame of the last change)
static struct { uint64_t addr, size, hash, seq; } vdyn_chk[VDYN_N];
static _Atomic uint64_t vdyn_w, vdyn_r;  // (written by the render thread / read by a waiting thread)
static uint64_t vdyn_tick;
static _Atomic int vdyn_marked;
static pthread_mutex_t vdyn_lock = PTHREAD_MUTEX_INITIALIZER;  // (the readers)

static int vdyn_on(void) {
    if (vdyn_mode < 0) {
        const char *e = getenv("SAVAGEWIND_VK_VDYN");
        vdyn_mode = !gpu_overlap() ? 0 : e ? atoi(e) : 1;
        LOG("[vk] vertex data the CPU rewrites while it is read: %s\n",
            vdyn_mode == 2 ? "every fetch up to 4 KB copied" : vdyn_mode ? "its regions learned and copied" : "read in place");
    }
    return vdyn_mode;
}

static bool vdyn_hit(uint64_t addr, uint64_t size) {
    if (!atomic_load_explicit(&vdyn_marked, memory_order_relaxed) || addr < HEAP_BASE || addr + size > HEAP_BASE + HEAP_MAX) return false;
    for (uint64_t r = (addr - HEAP_BASE) >> VDYN_SHIFT, e = (addr + size - 1 - HEAP_BASE) >> VDYN_SHIFT; r <= e; r++)
        if (atomic_load_explicit(&vdyn[r], memory_order_relaxed)) return true;
    return false;
}

// (render thread) a fetch read in place: copied instead (false) if its region was marked; some small ones sampled
static bool vdyn_fetch(uint64_t addr, uint64_t size) {
    if (!size || !vdyn_on()) return true;
    bool gw = gw_overlaps(addr, addr + size);
    if (vdyn_mode == 2 && size <= VDYN_SAMPLE_MAX && !gw) return false;
    if (size <= VDYN_COPY_MAX && vdyn_hit(addr, size)) return false;
    if (size > VDYN_SAMPLE_MAX || addr < HEAP_BASE || addr + size > HEAP_BASE + HEAP_MAX || (!gw && (++vdyn_tick & VDYN_SMASK)))
        return true;
    uint64_t w = atomic_load_explicit(&vdyn_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&vdyn_r, memory_order_acquire) >= VDYN_N) return true;  // (full: no sample)
    __typeof__(vdyn_chk[0]) *c = &vdyn_chk[w % VDYN_N];
    c->addr = addr, c->size = size, c->seq = vk_pending_seq();
    c->hash = hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)addr, size & ~3ull);
    atomic_store_explicit(&vdyn_w, w + 1, memory_order_release);
    return true;
}

extern bool framepace_loading_ui(void);
// (renderer_wait_seq, right after a real wait on submission seq) the samples recorded up to it, hashed again
void vdyn_check(uint64_t seq) {
    if (vdyn_mode != 1 || pthread_mutex_trylock(&vdyn_lock)) return;
    uint64_t r = atomic_load_explicit(&vdyn_r, memory_order_relaxed), w = atomic_load_explicit(&vdyn_w, memory_order_acquire);
    for (; r < w && vdyn_chk[r % VDYN_N].seq <= seq; r++) {
        __typeof__(vdyn_chk[0]) *c = &vdyn_chk[r % VDYN_N];
        if (hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)c->addr, c->size & ~3ull) == c->hash) continue;
        uint64_t reg = (c->addr - HEAP_BASE) >> VDYN_SHIFT, frame = renderer_present_count();
        if (framepace_loading_ui() || frame == vdyn_last[reg]) continue;
        vdyn_hits[reg] = frame - vdyn_last[reg] <= 60 && vdyn_hits[reg] < 255 ? vdyn_hits[reg] + 1 : 1;
        vdyn_last[reg] = frame;
        if (vdyn_hits[reg] < VDYN_HITS) continue;
        if (atomic_exchange_explicit(&vdyn[reg], 1, memory_order_relaxed)) continue;
        int n = atomic_fetch_add_explicit(&vdyn_marked, 1, memory_order_relaxed);
        if (n < 32) LOG("[vk] vertex data rewritten while read at %llx (%llu bytes): the %u KB around it copied from now on\n",
                        (unsigned long long)c->addr, (unsigned long long)c->size, 1u << (VDYN_SHIFT - 10));
    }
    atomic_store_explicit(&vdyn_r, r, memory_order_release);
    pthread_mutex_unlock(&vdyn_lock);
}

// (testing) SAVAGEWIND_VK_RACE_PROBE=1: what the GPU reads of guest memory in place (or through the heap copy, which
// the GPU fills when it gets to the list) changing between translation and the real wait on its submission, by kind
// and size, whatever vdyn does: one fetch in 8 of every size up to 256 KB hashed when recorded and again after the
// wait. A "[race]" line every 600 checks: samples, changes, and the biggest changed fetches.
enum { RP_VTX_S, RP_VTX_L, RP_IDX_S, RP_IDX_L, RP_GW_VTX, RP_GW_IDX, RP_KINDS };
static const char *const rp_kind_name[RP_KINDS] = {"vertex<=4K", "vertex>4K", "index<=4K", "index>4K", "gpu-written vertex",
                                                   "gpu-written index"};
#define RP_N 16384
#define RP_MAX (256u << 10)
static int rp_on = -1;
static struct { uint64_t addr, size, hash, seq; int kind; } rp_chk[RP_N];
static _Atomic uint64_t rp_sub_cpu[RP_KINDS];  // (of those: the CPU wrote the pages since translation - write watch)
static _Atomic uint64_t rp_w, rp_r, rp_s;  // (written by the render thread; read after the wait / in the recorder's turn)
static uint64_t rp_tick, rp_samples[RP_KINDS], rp_changed[RP_KINDS], rp_checks;
static _Atomic uint64_t rp_sub_samples[RP_KINDS], rp_sub_changed[RP_KINDS];  // (changed before the GPU had the submission)
static uint64_t rp_big_addr, rp_big_size;
static pthread_mutex_t rp_lock = PTHREAD_MUTEX_INITIALIZER;
void race_probe_note(uint64_t addr, uint64_t size, bool index) {
    if (rp_on < 0) {
        rp_on = getenv("SAVAGEWIND_VK_RACE_PROBE") && atoi(getenv("SAVAGEWIND_VK_RACE_PROBE"));
        if (rp_on) LOG("[race] probe on: in-place vertex / index fetches hashed at translation and after their wait\n");
    }
    if (!rp_on || !size || size > RP_MAX || addr < HEAP_BASE || addr + size > HEAP_BASE + HEAP_MAX || (++rp_tick & 7)) return;
    bool gw = gw_overlaps(addr, addr + size);  // (GPU-written: may change by the GPU's own writes too)
    uint64_t w = atomic_load_explicit(&rp_w, memory_order_relaxed), r = atomic_load_explicit(&rp_r, memory_order_acquire),
             rs = atomic_load_explicit(&rp_s, memory_order_acquire);
    if (w - (r < rs ? r : rs) >= RP_N) return;
    __typeof__(rp_chk[0]) *c = &rp_chk[w % RP_N];
    c->addr = addr, c->size = size, c->seq = vk_pending_seq();
    c->kind = gw ? (index ? RP_GW_IDX : RP_GW_VTX) : (index ? RP_IDX_S : RP_VTX_S) + (size > VDYN_SAMPLE_MAX);
    if (gw) {  // (the CPU's writes from here on, apart from the GPU's: write watch, reset now; the GPU's don't count there)
        uint64_t pages[64];
        plat_written_pages(addr & ~0xFFFull, ((addr + size + 0xFFF) & ~0xFFFull) - (addr & ~0xFFFull), pages, 64);
    }
    c->hash = hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)addr, size & ~3ull);
    atomic_store_explicit(&rp_w, w + 1, memory_order_release);
}
void race_probe_check(uint64_t seq) {
    if (rp_on <= 0 || pthread_mutex_trylock(&rp_lock)) return;
    uint64_t r = atomic_load_explicit(&rp_r, memory_order_relaxed), w = atomic_load_explicit(&rp_w, memory_order_acquire);
    for (; r < w && rp_chk[r % RP_N].seq <= seq; r++) {
        __typeof__(rp_chk[0]) *c = &rp_chk[r % RP_N];
        rp_samples[c->kind]++;
        if (hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)c->addr, c->size & ~3ull) == c->hash) continue;
        rp_changed[c->kind]++;
        if (c->size > rp_big_size) rp_big_size = c->size, rp_big_addr = c->addr;
    }
    atomic_store_explicit(&rp_r, r, memory_order_release);
    if (++rp_checks % 600 == 0) {
        char line[512];
        int n = 0;
        for (int k = 0; k < RP_KINDS; k++)
            n += snprintf(line + n, sizeof line - n, " %s %llu/%llu", rp_kind_name[k], (unsigned long long)rp_changed[k],
                          (unsigned long long)rp_samples[k]);
        LOG("[race] changed while read (of sampled):%s; biggest %llu bytes at %llx\n", line, (unsigned long long)rp_big_size,
            (unsigned long long)rp_big_addr);
        n = 0;
        for (int k = 0; k < RP_KINDS; k++)
            n += snprintf(line + n, sizeof line - n, " %s %llu/%llu (CPU %llu)", rp_kind_name[k],
                          (unsigned long long)atomic_exchange(&rp_sub_changed[k], 0), (unsigned long long)atomic_exchange(&rp_sub_samples[k], 0),
                          (unsigned long long)atomic_exchange(&rp_sub_cpu[k], 0));
        LOG("[race] changed before the GPU had the submission:%s\n", line);
        memset(rp_samples, 0, sizeof rp_samples);
        memset(rp_changed, 0, sizeof rp_changed);
        rp_big_size = rp_big_addr = 0;
    }
    pthread_mutex_unlock(&rp_lock);
}

// (recorder's turn, right before submission seq goes to the driver) the samples recorded into it, hashed again: a change
// here was made after translation and before the GPU could read it
static void race_probe_submit_run(void *arg) {
    uint64_t seq = (uint64_t)(uintptr_t)arg;
    uint64_t s = atomic_load_explicit(&rp_s, memory_order_relaxed), w = atomic_load_explicit(&rp_w, memory_order_acquire);
    for (; s < w && rp_chk[s % RP_N].seq <= seq; s++) {
        __typeof__(rp_chk[0]) *c = &rp_chk[s % RP_N];
        atomic_fetch_add_explicit(&rp_sub_samples[c->kind], 1, memory_order_relaxed);
        if (hash_words(0x9E3779B97F4A7C15ull, (const void *)(uintptr_t)c->addr, c->size & ~3ull) != c->hash) {
            atomic_fetch_add_explicit(&rp_sub_changed[c->kind], 1, memory_order_relaxed);
            if (plat_any_written(c->addr, c->size) == 1) atomic_fetch_add_explicit(&rp_sub_cpu[c->kind], 1, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&rp_s, s, memory_order_release);
}
void race_probe_submit(uint64_t seq) {
    if (rp_on > 0) rec_custom(race_probe_submit_run, (void *)(uintptr_t)seq);
    else if (rp_on == 0) atomic_store_explicit(&rp_s, atomic_load_explicit(&rp_w, memory_order_relaxed), memory_order_relaxed);
}

void vdyn_reset(void) {  // (vk_exec_reset, the GPU idle: another boot, other buffers)
    pthread_mutex_lock(&vdyn_lock);
    for (size_t i = 0; i < sizeof vdyn; i++) atomic_store_explicit(&vdyn[i], 0, memory_order_relaxed);
    memset(vdyn_hits, 0, sizeof vdyn_hits);
    memset(vdyn_last, 0, sizeof vdyn_last);
    atomic_store(&vdyn_marked, 0);
    atomic_store(&vdyn_r, atomic_load(&vdyn_w));
    pthread_mutex_unlock(&vdyn_lock);
}

// A vertex or index fetch: from the copy if it holds [addr, addr + size) as it is now - within one live chunk - else
// in place from the import (guest_direct), its pages then geometry for the next list's copy (and its chunk wanted).
// While a storing shader whose stores aren't known holds the list (barr.chunk_hold), in place.
bool guest_fetch(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (gsnap_verts() && size && size <= gsnap_verts_max() && !gw_overlaps(addr, addr + size)) return false;  // (copied)
    if (!vdyn_fetch(addr, size)) return false;  // (copied)
    if (!guest_direct(addr, size, buf, off)) return false;
    if (!hc_nch) return true;
    if (barr.chunk_hold || !size || addr + size > HEAP_BASE + hc_size || gw_overlaps(addr, addr + size)) {
        hc_stat_inplace++;
        return true;
    }
    uint64_t o = addr - HEAP_BASE, p0 = o / HC_PAGE, p1 = (o + size - 1) / HC_PAGE;
    bool all = true;
    for (uint64_t w = p0 >> 6; w <= p1 >> 6; w++) {
        uint64_t m = ~0ull;
        if (w == p0 >> 6) m &= ~0ull << (p0 & 63);
        if (w == p1 >> 6) m &= ~0ull >> (63 - (p1 & 63));
        if ((hc_ok[w] & m) != m) all = false;
        if ((hc_geo[w] & m) != m) hc_geo[w] |= m, hc_geo_new = true;
    }
    int c = (int)(o >> hc_shift), c1 = (int)((o + size - 1) >> hc_shift);
    HcChunk *ch = &hc_ch[c];
    if (c != c1) {  // (both wanted: their other fetches)
        for (int k = c; k <= c1; k++)
            if (atomic_load_explicit(&hc_ch[k].state, memory_order_relaxed) == HC_NONE)
                atomic_store_explicit(&hc_ch[k].state, HC_WANT, memory_order_relaxed), hc_want_any = true;
        hc_stat_edge++, hc_stat_inplace++;
        return true;
    }
    int s = atomic_load_explicit(&ch->state, memory_order_relaxed);
    if (s != HC_LIVE) {
        if (s == HC_NONE) atomic_store_explicit(&ch->state, HC_WANT, memory_order_relaxed), hc_want_any = true;
        ch->missed++;
        hc_stat_nochunk++, hc_stat_inplace++;
        return true;
    }
    if (!all) { hc_stat_inplace++; return true; }
    hc_stat_mirror++;
    ch->fetches++;
    *buf = ch->buf;
    *off = o - hc_base(c);
    return true;
}

// In-place reboot (exec.c vk_exec_reset): the heap import, its copy, and the storing-shader / guest-write state.
void vk_mem_reset(void) {
    vdyn_reset();
    ngw = 0;
    gw_gen++;
    nuub = 0;
    guest_copy_flush();
    gw_lo = ~0ull;
    gw_hi = 0;
    memset(gw_blocks, 0, sizeof gw_blocks);
    gcopy_gen++;
    hc_free();  // (before the import goes: the copy mirrors it)
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        if (gmem_mem) vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < gmem_sparse_nmem; i++) vkFreeMemory(g_vk.dev, gmem_sparse_mem[i], NULL);
    gmem_sparse_nmem = 0;
    gmem_size = gmem_tried = 0;
    gmem_mapped = false;  // (still mapped until main.c resets guest memory: the dma-buf lives on until then)
    gmem_sparse = false;
    memset(gmem_sparse_bound_pages, 0, sizeof gmem_sparse_bound_pages);
    gmem_sparse_generation = UINT64_MAX;
}

// ---- (GPU overlap) the global memory the HUD's shaders read, copied at translation
// With the GPU overlap the game's wait on its frame fence returns a frame early, so it may write frame N+2's data
// while the GPU still runs frame N. Constant buffers are copied at translation, but what shaders read through global
// memory (NVN's storage buffers: BOTW's text vertex shader fetches its glyphs that way) was read in place, when the GPU
// got to it: at each change of a button prompt's text, a GPU still on the frame before (Mali-G610) read glyph data the
// game had already rewritten - doubled or shifted letters (X Y B -> T X Y) and the prompt layout's hidden Wii U panes
// ("Tap here", "Wii U GamePad") for a frame or two. In passes into the final image (the HUD and menus), the storage
// buffers a read-only stage has bound (and the bytes after them, gsnap_info) are copied with its constants, and its
// shader reads those ranges from the copy (translate.c GLD, GSNAP_BUFFER_INDEX; the rest in place). Never ranges a
// storing shader has bound (GPU data: gsnap_gpu_note).
// Not only Mali: a desktop GPU that falls behind (1440p at 90-120 fps, NVIDIA) read the speech and item text the same
// way, a frame or two of misplaced letters as the text came up (#263). With the overlap, a dialogue line at the
// Shrine of Resurrection's pedestal on an RTX 5060: 4 of its bound ranges and 98 times the bytes after them rewritten
// before the GPU was done with them, none with the overlap off; with the copies, under 1 ms of CPU in 2 s in the game.
// Every pass, not only the final image (#452): the HUD's button prompt shadows are drawn once into a texture of their
// own from the prompt's text, and at 120 fps that pass read glyph positions the game had already rewritten - a shadow
// out of place for as long as the prompt kept its text. In Kakariko uncapped (RTX 3060) 175.1 -> 173.4 fps, ~1.7 ms of
// CPU in 2 s: every global memory read the game makes is the HUD's.
// Only with the overlap on, on every GPU. SAVAGEWIND_VK_GSNAP=0: off, =1: final image only; =all (default): every pass
// (the vertex data still only the HUD's); SAVAGEWIND_VK_GSNAP_MAX=<KB>: the largest storage buffer copied (1024);
// SAVAGEWIND_DEBUG_GSNAP=1: counts every 2 s.
// GPU data (ranges a storing shader had bound): kept as sorted, merged intervals. When full, the two closest are
// joined (the gap between them counts as GPU data too: fewer copies, never a stale one).
#define GSNAP_GPU_MAX 512
static struct { uint64_t lo, hi; } gsnap_gpu[GSNAP_GPU_MAX];
static int ngsnap_gpu;
static uint64_t gsnap_gpu_stores;  // (debug: storing stages seen)

int gsnap_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SAVAGEWIND_VK_GSNAP");
        m = !gpu_overlap() ? 0 : !e || !strcmp(e, "all") ? 2 : atoi(e) ? 1 : 0;
        LOG("[vk] global memory the HUD reads: %s\n",
            m == 2 ? "copied at translation (every pass, vertex data the HUD's)" : m ? "copied at translation (final image)" : "in place");
    }
    return m;
}

static int gsnap_gpu_find(uint64_t lo) {  // first interval whose hi > lo
    int a = 0, b = ngsnap_gpu;
    while (a < b) { int c = (a + b) / 2; if (gsnap_gpu[c].hi <= lo) a = c + 1; else b = c; }
    return a;
}

static void gsnap_gpu_add(uint64_t lo, uint64_t hi) {
    int i = gsnap_gpu_find(lo);
    if (i < ngsnap_gpu && gsnap_gpu[i].lo <= lo && hi <= gsnap_gpu[i].hi) return;  // (already in)
    if (i > 0 && gsnap_gpu[i - 1].hi == lo) i--;  // (touching on the left)
    int j = i;
    while (j < ngsnap_gpu && gsnap_gpu[j].lo <= hi) j++;  // [i, j): overlapping or touching
    if (j > i) {
        if (gsnap_gpu[i].lo < lo) lo = gsnap_gpu[i].lo;
        if (gsnap_gpu[j - 1].hi > hi) hi = gsnap_gpu[j - 1].hi;
        memmove(&gsnap_gpu[i + 1], &gsnap_gpu[j], (size_t)(ngsnap_gpu - j) * sizeof gsnap_gpu[0]);
        ngsnap_gpu -= j - i - 1;
        gsnap_gpu[i].lo = lo, gsnap_gpu[i].hi = hi;
        return;
    }
    if (ngsnap_gpu == GSNAP_GPU_MAX) {  // full: join every neighbour closer than the median gap (about half), once
        static uint64_t gaps[GSNAP_GPU_MAX];
        int ng = ngsnap_gpu - 1;
        for (int k = 0; k < ng; k++) gaps[k] = gsnap_gpu[k + 1].lo - gsnap_gpu[k].hi;
        int a = 0, b = ng - 1, want = ng / 2;  // (quickselect: the median gap)
        while (a < b) {
            uint64_t piv = gaps[(a + b) / 2];
            int x = a, y = b;
            while (x <= y) {
                while (gaps[x] < piv) x++;
                while (gaps[y] > piv) y--;
                if (x <= y) { uint64_t tmp = gaps[x]; gaps[x] = gaps[y]; gaps[y] = tmp; x++; y--; }
            }
            if (want <= y) b = y; else if (want >= x) a = x; else break;
        }
        uint64_t lim = gaps[want];
        int o = 0;
        for (int k = 1; k < ngsnap_gpu; k++) {
            if (gsnap_gpu[k].lo - gsnap_gpu[o].hi <= lim) gsnap_gpu[o].hi = gsnap_gpu[k].hi;
            else gsnap_gpu[++o] = gsnap_gpu[k];
        }
        ngsnap_gpu = o + 1;
        gsnap_gpu_add(lo, hi);
        return;
    }
    memmove(&gsnap_gpu[i + 1], &gsnap_gpu[i], (size_t)(ngsnap_gpu - i) * sizeof gsnap_gpu[0]);
    gsnap_gpu[i].lo = lo, gsnap_gpu[i].hi = hi;
    ngsnap_gpu++;
}

static void gsnap_gpu_note(int stage) {
    gsnap_gpu_stores++;
    for (int i = 0; i < 16; i++) {
        Range r = st.sbuf[stage][i];
        if (r.addr && r.size) gsnap_gpu_add(r.addr, r.addr + r.size);
    }
}

static bool gsnap_gpu_hit(Range r) {
    int i = gsnap_gpu_find(r.addr);
    return i < ngsnap_gpu && gsnap_gpu[i].lo < r.addr + r.size;
}

// The HUD's vertex and index data copied too (guest_fetch), in passes into the final image: fetches of up to
// SAVAGEWIND_VK_GSNAP_VERTS_MAX KB (64) that no storing shader wrote. Every fetch there copied, with constant buffers
// and fetches in the set of GPU writes waiting for the GPU, cost 30 -> 17 FPS on Mali-G610 and 176 -> 94 fps in
// Kakariko uncapped (RTX 3060); the small ones alone 174.2 -> 176.0 (noise). SAVAGEWIND_VK_GSNAP_VERTS=0: off.
static bool gsnap_verts(void) {
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_VK_GSNAP_VERTS") || atoi(getenv("SAVAGEWIND_VK_GSNAP_VERTS"));
    return on && gsnap_mode() && ps.out;
}
static uint64_t gsnap_verts_max(void) {
    static int64_t kb = -1;
    if (kb < 0) { const char *e = getenv("SAVAGEWIND_VK_GSNAP_VERTS_MAX"); kb = e ? atoll(e) : 64; }
    return (uint64_t)kb << 10;
}

static bool gsnap_linear_hit(Range r) {  // (render targets also written into the heap on the GPU: GPU data)
    if (!__atomic_load_n(&nlinear_mem, __ATOMIC_RELAXED)) return false;
    bool hit = false;
    pthread_mutex_lock(&readback_lock);
    for (int i = 0; i < nlinear_mem && !hit; i++)
        hit = r.addr < linear_mem[i].addr + linear_mem[i].size && linear_mem[i].addr < r.addr + r.size;
    pthread_mutex_unlock(&readback_lock);
    return hit;
}

// A compute shader has no constant buffer bindings of its own: the driver's block (cbuf_info, k == 0) gives it their
// addresses and it reads them through global memory, so they were read in place, when the GPU got to the dispatch,
// while every other stage's are copied at translation. With the GPU overlap that can be a frame after the game was
// told the frame is done, and it has reused the memory by then. In the Lost Woods at 240 fps (RTX 5070 Ti, the GPU the
// limit: ~190 fps at 2560x1440, DLSS ultra quality, render scale 125%) the mist's two particle shaders then ran on
// whatever the game had since put where two of their constant buffers were, and marked every particle of the emitter
// dead: the mist flickered, redrawn from nothing in another shape 4 to 6 times a second in play (with the upscaler off
// at 200%, standing still: 24 to 36
// emitters wiped in 35 s, each time with the frame before's dispatch not done when this one was translated).
// So the constant buffers a compute shader reads (the addresses it takes from the driver's block: rows 0x21 + i of
// its cbuf 0, ShaderInfo.cbuf_rows) are copied with the dispatch, one after the other into the buffer bound at
// GSNAP_BUFFER_INDEX, and the driver's block gives it addresses past the heap's window instead, HEAP_BASE +
// maxwell_gmem_size + the copy's place: its global memory reads of those come from that buffer (translate.c GLD),
// at one comparison a read. Not ranges a storing shader had bound: GPU data, which keeps its own address. When none
// is copied, the binding is one empty buffer per submission, as gsnap_info's.
// In play there, the copies on and off every 10 s in one run: 194.7 fps with them and 193.3 without (median frame
// 5.07 and 5.11 ms), 186 KB in ~195 copies a frame, 0.018 ms of the render thread. No emitter wiped with them.
// (Tried first: the shader walking a table of the copied ranges at every read, as the HUD's shaders do. With every
// bound buffer copied, 190 -> 170 fps, 2.2 MB in 250 copies a frame and a fifth more GPU time in the dispatches; with
// only the buffers read and the table skipped for reads outside the span they lie in, 187.6 -> 183.6.)
// Only with the overlap on (maxwell_gmem_snap). SAVAGEWIND_VK_GSNAP_CS=0: off (read in place, as before).
void gsnap_cs_prepare(const ShaderInfo *si) {  // (dispatch_compute, before the stage's set is made)
    static int on = -1;
    if (on < 0) on = !getenv("SAVAGEWIND_VK_GSNAP_CS") || atoi(getenv("SAVAGEWIND_VK_GSNAP_CS"));
    memset(&gsnap_cs, 0, sizeof gsnap_cs);
    if (!si->uses_gmem || !maxwell_gmem_snap) return;  // (its translation has no such buffer)
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size(), total = 0, at[16];
    Range r[16] = {{0}};
    uint32_t read = !on ? 0 : si->cbuf_rows_all & 1 ? 0xffff : (uint32_t)(si->cbuf_rows[0][0] >> 0x21) & 0xffff;
    for (int i = 0; i < 16 && read >> i; i++) {
        Range q = st.ubuf[5][i];
        if (!(read >> i & 1) || !q.addr || !q.size || (q.addr & 3) || q.addr < lo || q.addr >= hi) continue;
        if (q.size > 0x10000) q.size = 0x10000;  // (a constant buffer's most: 16 of them fill GSNAP_CS_SPAN)
        if (q.addr + q.size > hi) q.size = hi - q.addr;
        q.size &= ~3ull;
        if (!q.size || gsnap_gpu_hit(q) || gsnap_linear_hit(q)) continue;
        r[i] = q, at[i] = total;
        total += (q.size + 15) & ~15ull;
    }
    if (!total) {
        static uint64_t eseq = ~0ull;
        static VkDescriptorBufferInfo ebi;
        uint64_t sq = vk_pending_seq();
        if (eseq != sq) {
            VkSpan e = vk_transient(16, 256);
            memset(e.ptr, 0, 16);
            ebi = (VkDescriptorBufferInfo){e.buf, e.off, 16};
            eseq = sq;
        }
        gsnap_cs.bi = ebi;
        return;
    }
    VkSpan span = vk_transient(total, 256);
    for (int i = 0; i < 16; i++) {
        if (!r[i].size) continue;
        uint64_t padded = (r[i].size + 15) & ~15ull;
        memcpy((uint8_t *)span.ptr + at[i], (const void *)(uintptr_t)r[i].addr, r[i].size);
        memset((uint8_t *)span.ptr + at[i] + r[i].size, 0, padded - r[i].size);
        gsnap_cs.addr[i] = HEAP_BASE + maxwell_gmem_size + at[i];
    }
    gsnap_cs.bi = (VkDescriptorBufferInfo){span.buf, span.off, total};
}

// The ranges a read-only stage's global memory reads are copied from (translate.c GLD: up to GSNAP_N): its storage
// buffers, each with SAVAGEWIND_VK_GSNAP_EXT bytes after it. BOTW's HUD text shader reads its glyphs past the size its storage
// buffer is bound with: copied at the bound size (~500 bytes) the prompt still broke, with the bytes after it it
// didn't. (Where that reaches GPU data, the bound size only.) In every pass; SAVAGEWIND_VK_GSNAP=1: passes into the
// final image only. SAVAGEWIND_VK_GSNAP_EXT=<KB>: the bytes after (16).
#define GSNAP_N 4
VkDescriptorBufferInfo gsnap_info(int stage) {
    if (stage == 5) return gsnap_cs.bi;  // (a compute shader's constant buffers: gsnap_cs_prepare)
    static struct { uint64_t seq; int n; Range r[GSNAP_N]; VkDescriptorBufferInfo bi; } cache[32];
    static int cpos;
    static uint64_t maxsz, ext, t_log;
    static uint64_t n_draw, n_other, n_copy, n_bytes, n_reuse, n_skip, n_short, n_ns;
    static int dbg = -1;
    if (!maxsz) {
        const char *e = getenv("SAVAGEWIND_VK_GSNAP_MAX"), *x = getenv("SAVAGEWIND_VK_GSNAP_EXT");
        maxsz = (e && atoi(e) > 0 ? (uint64_t)atoi(e) : 1024) << 10;
        ext = (x && atoi(x) >= 0 ? (uint64_t)atoi(x) : 16) << 10;
        dbg = getenv("SAVAGEWIND_DEBUG_GSNAP") != NULL;
    }
    int m = gsnap_mode();
    bool here = m && (m == 2 || ps.out) && (stage == 0 || stage == 1) && gmem_size;
    if (!here) {  // (no copies here: one empty table per submission, the shader looks at none of it)
        if (m && (stage == 0 || stage == 1)) n_other++;
        static uint64_t eseq = ~0ull;
        static VkDescriptorBufferInfo ebi;
        uint64_t sq = vk_pending_seq();
        if (eseq != sq) {
            VkSpan e = vk_transient(GSNAP_N * 16 + 16, 256);
            memset(e.ptr, 0, GSNAP_N * 16 + 16);
            ebi = (VkDescriptorBufferInfo){e.buf, e.off, GSNAP_N * 16 + 16};
            eseq = sq;
        }
        return ebi;
    }
    uint64_t t_in = dbg ? SDL_GetTicksNS() : 0;
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + gmem_size;
    Range r[GSNAP_N];
    int n = 0;
    n_draw++;
    for (int i = 0; i < 16 && n < GSNAP_N; i++) {
        Range q = st.sbuf[stage][i];
        if (!q.addr || !q.size) continue;
        if (q.size > maxsz || (q.addr & 3) || q.addr < lo || q.addr + q.size > hi || gsnap_gpu_hit(q) || gsnap_linear_hit(q)) {
            n_skip++;
            continue;
        }
        Range x = {q.addr, q.size + ext};
        if (x.addr + x.size > hi) x.size = hi - x.addr;
        if (ext && (gsnap_gpu_hit(x) || gsnap_linear_hit(x))) { x = q; n_short++; }
        x.size = (x.size + 3) & ~3ull;
        if (x.addr + x.size > hi) x.size = (hi - x.addr) & ~3ull;
        r[n++] = x;
    }
    uint64_t seq = vk_pending_seq();
    for (int c = 0; c < 32; c++)  // (the same ranges again in this submission: the same copy)
        if (cache[c].seq == seq && cache[c].n == n && !memcmp(cache[c].r, r, sizeof r[0] * (size_t)n)) {
            n_reuse++;
            if (dbg) n_ns += SDL_GetTicksNS() - t_in;
            return cache[c].bi;
        }
    uint64_t hdr = GSNAP_N * 16, total = hdr;
    for (int k = 0; k < n; k++) total += (r[k].size + 15) & ~15ull;
    if (!n) total += 16;
    VkSpan span = vk_transient(total, 256);
    uint32_t *h = (uint32_t *)span.ptr;
    memset(h, 0, hdr);
    uint64_t at = hdr;
    for (int k = 0; k < n; k++) {
        uint64_t padded = (r[k].size + 15) & ~15ull;
        h[k * 4 + 0] = (uint32_t)((r[k].addr - HEAP_BASE) >> 2);
        h[k * 4 + 1] = (uint32_t)(r[k].size >> 2);
        h[k * 4 + 2] = (uint32_t)((at - hdr) >> 2);
        memcpy((uint8_t *)span.ptr + at, (const void *)(uintptr_t)r[k].addr, r[k].size);
        memset((uint8_t *)span.ptr + at + r[k].size, 0, padded - r[k].size);
        at += padded;
        n_copy++;
        n_bytes += r[k].size;
    }
    h[3] = (uint32_t)n;  // (gs_r[0].w: the shader looks at this many)
    if (!n) memset((uint8_t *)span.ptr + hdr, 0, 16);
    VkDescriptorBufferInfo bi = {span.buf, span.off, total};
    cache[cpos].seq = seq;
    cache[cpos].n = n;
    memcpy(cache[cpos].r, r, sizeof r[0] * (size_t)n);
    cache[cpos].bi = bi;
    cpos = (cpos + 1) & 31;
    if (dbg) {
        uint64_t now = SDL_GetTicksNS();
        n_ns += now - t_in;
        if (!t_log) t_log = now;
        if (now - t_log > 2000000000ull) {
            LOG("[gsnap] 2 s: %llu HUD draws, %llu elsewhere; %llu ranges copied (%.2f MB), %llu reused, %llu skipped, %llu not "
                "extended (GPU data after); %llu storing shaders, GPU data in %d ranges; %.2f ms CPU\n",
                (unsigned long long)n_draw, (unsigned long long)n_other, (unsigned long long)n_copy, n_bytes / 1048576.0,
                (unsigned long long)n_reuse, (unsigned long long)n_skip, (unsigned long long)n_short,
                (unsigned long long)gsnap_gpu_stores, ngsnap_gpu, n_ns / 1e6);
            gsnap_gpu_stores = 0;
            n_draw = n_other = n_copy = n_bytes = n_reuse = n_skip = n_short = n_ns = 0;
            t_log = now;
        }
    }
    return bi;
}
