// BC5 (NVN format 0x4C) view format: SAVAGEWIND_BC5_UNORM picks BC5_UNORM, else BC5_SNORM. The environment is read
// once (the first fmt_of call), so changing it later in the process must not change the format.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

VkFormat vk_format(uint32_t f);
bool vk_format_alpha_one(uint32_t f);

static void set_unorm(int on) {
#ifdef _WIN32
    _putenv_s("SAVAGEWIND_BC5_UNORM", on ? "1" : "");
#else
    if (on) setenv("SAVAGEWIND_BC5_UNORM", "1", 1);
    else unsetenv("SAVAGEWIND_BC5_UNORM");
#endif
}

static double now_ns(void) { return (double)clock() * (1e9 / CLOCKS_PER_SEC); }

int main(int argc, char **argv) {
    bool want_unorm = getenv("SAVAGEWIND_BC5_UNORM") != NULL;
    VkFormat want = want_unorm ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK;

    assert(vk_format(0x4C) == want);
    assert(!vk_format_alpha_one(0x4C));

    set_unorm(!want_unorm);
    assert((getenv("SAVAGEWIND_BC5_UNORM") != NULL) != want_unorm);
    assert(vk_format(0x4C) == want);

    assert(vk_format(0x4A) == VK_FORMAT_BC4_SNORM_BLOCK);
    assert(vk_format(0x4B) == VK_FORMAT_BC5_UNORM_BLOCK);
    assert(vk_format(0x4D) == VK_FORMAT_BC7_UNORM_BLOCK);
    assert(vk_format(0x4E) == VK_FORMAT_BC7_SRGB_BLOCK);

    puts("PASS: 0x4C honours SAVAGEWIND_BC5_UNORM and reads it only once");

    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        const int n = 1000000;
        const char *volatile key = "SAVAGEWIND_BC5_UNORM";  // volatile: not hoisted out of the loop
        volatile uintptr_t sink = 0;
        double t0 = now_ns();
        for (int i = 0; i < n; i++) sink ^= (uintptr_t)vk_format(0x4C);
        double cached = (now_ns() - t0) / n;
        t0 = now_ns();
        for (int i = 0; i < n; i++) sink ^= (uintptr_t)getenv(key);
        double raw = (now_ns() - t0) / n;
        printf("vk_format(0x4C): %.2f ns/call; getenv: %.2f ns/call (%.0fx)\n", cached, raw, raw / cached);
        (void)sink;
    }
    return 0;
}
