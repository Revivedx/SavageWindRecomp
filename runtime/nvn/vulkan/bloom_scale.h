#ifndef SW_BLOOM_SCALE_H
#define SW_BLOOM_SCALE_H
#include <stdbool.h>
#include <stdint.h>

// BOTW 1.9's bloom extraction, threshold, mip reduction, separable blur and
// accumulation. Keep their small working images on the console's texel grid:
// the blur's baked tap offsets rely on bilinear filtering across adjacent texels.
static inline bool bloom_native_target(uint64_t fs, uint32_t target, uint32_t format,
                                       uint32_t width, uint32_t height, uint32_t samples) {
    if (target != 1 || format != 0x3f || samples > 1 || !width || !height ||
        width > 640 || height > 360) return false;
    switch (fs) {
    case UINT64_C(0x134b2ad2880b2dc0):
    case UINT64_C(0xadf7acdfaeb7f391):
    case UINT64_C(0x11eb927369affaf0):
    case UINT64_C(0x76d0ca9268564299):
    case UINT64_C(0x17bf3bff14a507ff):
    case UINT64_C(0xc602d85d67ced2bf): return true;
    default: return false;
    }
}
#endif
