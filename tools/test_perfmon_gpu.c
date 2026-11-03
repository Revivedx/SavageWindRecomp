// clang -O1 -ffunction-sections -fdata-sections tools/test_perfmon_gpu.c -Wl,--gc-sections -lpthread -ldl -lm -o /tmp/test_perfmon_gpu
// Usage: /tmp/test_perfmon_gpu <fake sysfs root>
#include <assert.h>
#include "../runtime/perfmon.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    char dev[512];
    // Two AMD cards: unknown identity must not guess card0.
    assert(!amdgpu_find(dev, sizeof dev, argv[1], -1, -1));
    assert(!dev[0]);
    // The selected render node resolves to card1, independent of card/render enumeration order.
    assert(amdgpu_find(dev, sizeof dev, argv[1], 226, 128));
    char p[640];
    snprintf(p, sizeof p, "%s/mem_info_vram_total", dev);
    assert(read_float_file(p) == 17179869184.0f);
    assert(amdgpu_find(dev, sizeof dev, argv[1], 226, 129));
    snprintf(p, sizeof p, "%s/mem_info_vram_total", dev);
    assert(read_float_file(p) == 536870912.0f);
    // An inaccessible or non-amdgpu selected node must never fall back to another GPU.
    assert(!amdgpu_find(dev, sizeof dev, argv[1], 226, 130));
    // A single AMD device remains usable on drivers without the DRM Vulkan extension; card indices may exceed 7.
    char single[512];
    snprintf(single, sizeof single, "%s/single", argv[1]);
    assert(amdgpu_find(dev, sizeof dev, single, -1, -1));
    assert(strstr(dev, "/card12/device"));
    assert(!amdgpu_find(dev, sizeof dev, "/nonexistent-perfmon-sysfs", -1, -1));
    // The overlay may start first; late renderer selection must be published safely.
    perfmon_set_gpu_hint("AMD Radeon RX 6800");
    assert(g_gpu_drm_major == -1 && g_gpu_drm_minor == -1);
    perfmon_set_gpu_drm(226, 128);
    assert(g_gpu_drm_major == 226 && g_gpu_drm_minor == 128);
    char hint[96];
    gpu_hint(hint);
    assert(!strcmp(hint, "AMD Radeon RX 6800"));
    puts("ok: renderer node selection, reversed enumeration, ambiguous/unique devices, missing node and late identity");
}
