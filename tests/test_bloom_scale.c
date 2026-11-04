#include <assert.h>
#include <stdio.h>
#include "../runtime/nvn/vulkan/bloom_scale.h"

int main(void) {
    const uint64_t reduce = UINT64_C(0x11eb927369affaf0);
    const uint64_t blur = UINT64_C(0x76d0ca9268564299);
    const uint64_t combine = UINT64_C(0xc602d85d67ced2bf);
    // Actual mip chain and odd scratch targets observed in the 4K trace.
    assert(bloom_native_target(reduce, 1, 0x3f, 320, 180, 1));
    assert(bloom_native_target(blur, 1, 0x3f, 160, 90, 1));
    assert(bloom_native_target(blur, 1, 0x3f, 80, 45, 1));
    assert(bloom_native_target(blur, 1, 0x3f, 40, 22, 1));
    assert(bloom_native_target(blur, 1, 0x3f, 20, 11, 1));
    assert(bloom_native_target(combine, 1, 0x3f, 320, 180, 1));
    // A matching size alone must never classify scene, depth or UI work as bloom.
    assert(!bloom_native_target(0, 1, 0x3f, 320, 180, 1));
    assert(!bloom_native_target(blur, 1, 0x34, 320, 180, 1));
    assert(!bloom_native_target(blur, 4, 0x3f, 320, 180, 1));
    assert(!bloom_native_target(blur, 1, 0x3f, 1600, 900, 1));
    assert(!bloom_native_target(blur, 1, 0x3f, 320, 180, 4));
    assert(!bloom_native_target(blur, 1, 0x3f, 0, 0, 1));
    puts("Bloom target guards: PASS (gameplay visual validation pending)");
}
