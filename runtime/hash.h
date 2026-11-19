// A fast 64-bit hash of bytes (not a cryptographic one): the caches' keys and checks
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// h: where it starts (a seed, or the hash of what came before: hashes chain)
static inline uint64_t hash64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ (h >> 32);
}
