// The runtime's own image decoding and PNG writing (runtime/imageload.h). One C implementation for both users: the
// start screen's picture (runtime/ui/splash.mm) and the window icon (runtime/paths.c), plus the screenshots and
// texture dumps of the Vulkan backend (runtime/nvn/vulkan/device.c, exec.c). JPEG and PNG only, no HDR/linear
// conversions - the same stb_image options the start screen compiled in itself before.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdlib.h>
#include <string.h>

#include "imageload.h"

bool rt_image_info(const char *path, int *w, int *h) {
    int n;
    return stbi_info(path, w, h, &n) != 0;
}

unsigned char *rt_image_rgba(const char *path, int *w, int *h) { return stbi_load(path, w, h, NULL, 4); }

const char *rt_image_error(void) { return stbi_failure_reason(); }

void rt_image_free(void *px) { stbi_image_free(px); }

bool rt_save_png(const char *path, const unsigned char *rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    size_t n = (size_t)w * (size_t)h * 4;
    unsigned char *px = malloc(n);
    if (!px) return false;
    memcpy(px, rgba, n);
    for (size_t i = 3; i < n; i += 4) px[i] = 255;
    bool ok = stbi_write_png(path, w, h, 4, px, w * 4) != 0;
    free(px);
    return ok;
}
