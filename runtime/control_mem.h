// (test and development only) Guest memory helpers of the control endpoint for other control commands.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Guest memory the OS copy cannot read or write but that is always mapped read-write while the game runs: on Linux, the
// guest heap as a dma-buf mapped over it (vulkan/exec.c heap_export, heap_dmaheap; RADV's default, the Steam Deck's):
// process_vm_readv / writev fail there (EFAULT: no page structs behind a dma-buf mapping), so peeks and pokes inside
// this range copy in place. 0, 0: none.
extern uint64_t g_mem_direct_lo, g_mem_direct_hi;
bool control_mem_peek(uint64_t addr, void *buf, size_t n);        // read n bytes (committed guest memory only)
bool control_mem_poke(uint64_t addr, const void *buf, size_t n);  // write n <= 64 bytes (ordinary writable pages only)
// Where three floats in a row are within tol of v[0..2] (writable memory), found on a background thread (about 0.3 s,
// never blocks the caller). _start returns false if one is already running. _poll: 0 nothing started, 1 still running,
// 2 done (writes up to max addresses to out and the count to *n; the result is handed over once).
#define CONTROL_MEM_VEC_MAX 256
bool control_mem_find_vec3_start(const float v[3], float tol);
int control_mem_find_vec3_poll(uint64_t *out, size_t max, size_t *n);
