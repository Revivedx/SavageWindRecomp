// Video memory (vram.c): VMA's blocks made ahead of need on a helper thread, and the device-local heap's budget
#pragma once
#include <stdatomic.h>
#include "vk.h"

// device.c, before / after vmaCreateAllocator (memory_budget: VK_EXT_memory_budget enabled on the device)
void vram_allocator_setup(VmaAllocatorCreateInfo *ai, VmaVulkanFunctions *fn, bool memory_budget);
void vram_start(void);
// The largest device-local heap's index: VMA's usage and budget of it, its size (any of them may be NULL)
uint32_t vram_device_local(uint64_t *usage, uint64_t *budget, uint64_t *size);
void vram_device_local_allocated(uint64_t *allocated, uint64_t *budget);  // its allocations' bytes (blocks' room left out)
uint64_t vram_budget_cap(uint64_t budget);  // SAVAGEWIND_VRAM_BUDGET_MB: no bigger than that
bool vram_budget_ext(void);
extern _Atomic uint64_t g_vram_alloc_ns;  // vkAllocateMemory time on the game's threads so far
// The share of the budget over which textures.c lets images go (vram_trim). The helper's spare blocks stop there
// (vram.c HR_PRESSURE), and the heap copy plans up to it less one block (exec.c hc_limit).
#define VRAM_TRIM_TARGET 0.85
// Cards with a device-local heap of this or less: finer VMA blocks (vram.c) and heap copy chunks (exec.c)
#define VRAM_SMALL_HEAP (2048ull << 20)
uint64_t vram_block_size(void);  // VMA's block size in the largest device-local heap
