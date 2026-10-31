// Video memory (vram.c): VMA's blocks made ahead of need on a helper thread, and the device-local heap's budget
#pragma once
#include <stdatomic.h>
#include "vk.h"

// device.c, before / after vmaCreateAllocator (memory_budget: VK_EXT_memory_budget enabled on the device)
void vram_allocator_setup(VmaAllocatorCreateInfo *ai, VmaVulkanFunctions *fn, bool memory_budget);
void vram_start(void);
// The largest device-local heap's index: VMA's usage and budget of it, its size (any of them may be NULL)
uint32_t vram_device_local(uint64_t *usage, uint64_t *budget, uint64_t *size);
bool vram_budget_ext(void);
extern _Atomic uint64_t g_vram_alloc_ns;  // vkAllocateMemory time on the game's threads so far
