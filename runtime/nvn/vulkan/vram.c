// Video memory: VMA's blocks made ahead of need on a thread of their own, and what the device-local heap has left.
//
// VMA suballocates images and buffers from blocks of device memory (32-256 MB a block, one vkAllocateMemory each).
// When no block of a memory type has room, the allocation that needs it makes a new one on the thread that asked -
// the render / translation threads - and that can take milliseconds (~0.1-0.6 ms on an RTX 3060 with room to
// spare, reportedly ~8-19 ms on other setups: a hitch whenever the game's video memory grows). And VMA gives its one
// empty block of a type back (vkFreeMemory) as soon as anything else of that type is freed, so a type whose use
// goes up and down makes blocks over and over. Now a helper thread keeps room ahead: woken whenever a thread of the
// game makes a block, and every HR_PERIOD_MS besides, it asks VMA, per memory type in use, for half of the largest
// block seen there without making a block (NEVER_ALLOCATE): given, there is room, and it is given back. Refused, it
// asks again letting VMA make a block - on the helper - and keeps that allocation (the anchor: a block holding it is
// never empty, so VMA keeps it) until the next one is made, then frees the old one (its half: room for the game).
// (Approach from another recompiler's VMA setup.) Allocations VMA gives memory of their own (dedicated: render
// targets, the heap copy, images over half a block) are not helped. Under pressure - the heap's usage plus a block
// over HR_PRESSURE of its budget (VK_EXT_memory_budget where the driver has it: what the OS grants this process;
// else VMA's estimate, 80% of the heap) - no spare is made and the anchor is given back.
// Off by default (on an RTX 3060 a block takes 0.1-0.6 ms, so there was nothing to gain, and the spare holds video
// memory): SAVAGEWIND_VRAM_HEADROOM=1 turns the helper on. SAVAGEWIND_VRAM_LOG=1: every vkAllocateMemory over 1 ms and
// a count every 30 s. Every block made is logged, with the thread that waited for it.
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vram.h"

#define HR_PERIOD_MS 250
#define HR_PRESSURE 0.90

static bool hr_on, hr_log, budget_ext;
static pthread_t hr_thread;
static _Atomic bool hr_thread_up;
static pthread_mutex_t hr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hr_cond = PTHREAD_COND_INITIALIZER;
static bool hr_wake;
static _Atomic uint64_t hr_block[VK_MAX_MEMORY_TYPES];  // largest block seen per memory type (0: none yet)
static VmaAllocation hr_anchor[VK_MAX_MEMORY_TYPES];   // (helper) half a block holding the newest spare block
static uint32_t hr_drops;                              // (helper) anchors given back under pressure
static VkPhysicalDeviceMemoryProperties hr_mp;
_Atomic uint64_t g_vram_alloc_ns;  // vkAllocateMemory time on the game's threads (exec.c's hitch lines)
static _Atomic uint32_t st_game_blocks, st_helper_blocks, st_dedicated;
static _Atomic uint64_t st_game_ns, st_helper_ns, st_dedicated_ns;

static bool on_helper(void) { return atomic_load(&hr_thread_up) && pthread_equal(pthread_self(), hr_thread); }

static bool dedicated_info(const VkMemoryAllocateInfo *ai) {
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) return true;
    return false;
}

// (VMA's vkAllocateMemory) timed; a block made on a thread of the game wakes the helper
static VKAPI_ATTR VkResult VKAPI_CALL vram_allocate(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *cb,
                                                    VkDeviceMemory *mem) {
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkAllocateMemory(dev, ai, cb, mem);
    uint64_t dt = SDL_GetTicksNS() - t0;
    bool helper = on_helper(), ded = dedicated_info(ai);
    uint32_t type = ai->memoryTypeIndex;
    uint64_t size = ai->allocationSize;
    bool block = r == VK_SUCCESS && !ded && size >= (16ull << 20) && !(size & (size - 1));  // (VMA's blocks: powers of 2)
    if (!helper) atomic_fetch_add(&g_vram_alloc_ns, dt);
    if (ded) atomic_fetch_add(&st_dedicated, 1), atomic_fetch_add(&st_dedicated_ns, dt);
    if (block) {
        if (helper) atomic_fetch_add(&st_helper_blocks, 1), atomic_fetch_add(&st_helper_ns, dt);
        else atomic_fetch_add(&st_game_blocks, 1), atomic_fetch_add(&st_game_ns, dt);
        uint64_t old = atomic_load(&hr_block[type]);
        while (size > old && !atomic_compare_exchange_weak(&hr_block[type], &old, size)) {}
        LOG("[vram] t=%.2f new block: %llu MB of memory type %u (heap %u) in %.1f ms on %s\n", t0 / 1e9,
            (unsigned long long)(size >> 20), type, hr_mp.memoryTypes[type].heapIndex, dt / 1e6,
            helper ? "the helper (ahead of need)" : "a game thread");
        if (!helper && hr_on) {
            pthread_mutex_lock(&hr_lock);
            hr_wake = true;
            pthread_cond_signal(&hr_cond);
            pthread_mutex_unlock(&hr_lock);
        }
    } else if (hr_log && dt > 1000000)
        LOG("[vram] t=%.2f vkAllocateMemory %.1f MB type %u%s: %.1f ms on %s%s\n", t0 / 1e9, size / 1048576.0, type,
            ded ? " (dedicated)" : "", dt / 1e6, helper ? "the helper" : "a game thread", r == VK_SUCCESS ? "" : ", failed");
    return r;
}

static void *hr_main(void *arg) {
    (void)arg;
    uint64_t last_stats = SDL_GetTicksNS();
    for (;;) {
        pthread_mutex_lock(&hr_lock);
        if (!hr_wake) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += HR_PERIOD_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&hr_cond, &hr_lock, &ts);
        }
        hr_wake = false;
        pthread_mutex_unlock(&hr_lock);
        VmaBudget bud[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(g_vk.vma, bud);
        for (uint32_t t = 0; t < hr_mp.memoryTypeCount && hr_on; t++) {
            uint64_t blk = atomic_load(&hr_block[t]);
            if (!blk) continue;
            uint32_t h = hr_mp.memoryTypes[t].heapIndex;
            if ((double)(bud[h].usage + blk) > HR_PRESSURE * (double)bud[h].budget) {  // pressure: no spare
                if (hr_anchor[t]) vmaFreeMemory(g_vk.vma, hr_anchor[t]), hr_anchor[t] = NULL, hr_drops++;
                continue;
            }
            VkMemoryRequirements req = {.size = blk / 2, .alignment = 256, .memoryTypeBits = 1u << t};
            VmaAllocationCreateInfo probe = {.flags = VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT, .memoryTypeBits = 1u << t};
            VmaAllocationCreateInfo make = {.memoryTypeBits = 1u << t};
            VmaAllocation a;
            if (vmaAllocateMemory(g_vk.vma, &req, &probe, &a, NULL) == VK_SUCCESS) { vmaFreeMemory(g_vk.vma, a); continue; }
            if (vmaAllocateMemory(g_vk.vma, &req, &make, &a, NULL) != VK_SUCCESS) continue;
            if (hr_anchor[t]) vmaFreeMemory(g_vk.vma, hr_anchor[t]);  // (its half: room for the game in the old block)
            hr_anchor[t] = a;
        }
        if (hr_log && SDL_GetTicksNS() - last_stats > 30000000000ull) {
            last_stats = SDL_GetTicksNS();
            LOG("[vram] blocks made on game threads %u (%.1f ms), on the helper %u (%.1f ms); dedicated %u (%.1f ms); "
                "anchors given back under pressure %u\n", atomic_load(&st_game_blocks), atomic_load(&st_game_ns) / 1e6,
                atomic_load(&st_helper_blocks), atomic_load(&st_helper_ns) / 1e6, atomic_load(&st_dedicated),
                atomic_load(&st_dedicated_ns) / 1e6, hr_drops);
        }
    }
    return NULL;
}

void vram_allocator_setup(VmaAllocatorCreateInfo *ai, VmaVulkanFunctions *fn, bool memory_budget) {
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &hr_mp);
    budget_ext = memory_budget;
    memset(fn, 0, sizeof *fn);
    fn->vkAllocateMemory = vram_allocate;
    ai->pVulkanFunctions = fn;
    if (memory_budget) ai->flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
}

void vram_start(void) {
    const char *e = getenv("SAVAGEWIND_VRAM_HEADROOM");
    hr_log = getenv("SAVAGEWIND_VRAM_LOG") && atoi(getenv("SAVAGEWIND_VRAM_LOG"));
    hr_on = e && atoi(e) != 0;  // (off by default: no gain measured where blocks are quick, and it holds video memory)
    uint64_t usage, budget, size;
    vram_device_local(&usage, &budget, &size);
    LOG("[vram] blocks ahead of need: %s; video memory %llu MB, budget %llu MB (%s), in use %llu MB\n",
        hr_on ? "on" : "off (SAVAGEWIND_VRAM_HEADROOM=1: on)", (unsigned long long)(size >> 20), (unsigned long long)(budget >> 20),
        budget_ext ? "VK_EXT_memory_budget" : "VMA's estimate", (unsigned long long)(usage >> 20));
    if (!hr_on && !hr_log) return;  // (with the log: the helper only counts)
    if (pthread_create(&hr_thread, NULL, hr_main, NULL) == 0) atomic_store(&hr_thread_up, true);
    else hr_on = false;
}

// The largest device-local heap: VMA's usage and budget of it (the whole process's with VK_EXT_memory_budget), its size
uint32_t vram_device_local(uint64_t *usage, uint64_t *budget, uint64_t *size) {
    uint32_t best = 0;
    uint64_t sz = 0;
    for (uint32_t i = 0; i < hr_mp.memoryHeapCount; i++)
        if ((hr_mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && hr_mp.memoryHeaps[i].size > sz)
            sz = hr_mp.memoryHeaps[i].size, best = i;
    VmaBudget bud[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, bud);
    if (usage) *usage = bud[best].usage;
    if (budget) *budget = bud[best].budget;
    if (size) *size = sz;
    return best;
}

bool vram_budget_ext(void) { return budget_ext; }
