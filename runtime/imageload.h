// Image files the runtime reads and writes itself (runtime/imageload.c): JPEG / PNG through stb_image and
// stb_image_write (third_party/stb, public domain). SDL's own SDL_LoadPNG / SDL_SavePNG appeared in SDL 3.4, and the
// distributions the app is built on ship 3.2 - Debian 13, Ubuntu 25.04 - whose headers have neither, so the build
// cannot call them.
#ifndef IMAGELOAD_H
#define IMAGELOAD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool rt_image_info(const char *path, int *w, int *h);             // header only, no decode (a size check)
unsigned char *rt_image_rgba(const char *path, int *w, int *h);   // malloc'd w*h*4 RGBA; free with rt_image_free
const char *rt_image_error(void);                                 // why the last rt_image_* call failed

void rt_image_free(void *px);

// 8-bit PNG, alpha forced opaque (render targets often keep the G-buffer's alpha, which viewers turn into black).
bool rt_save_png(const char *path, const unsigned char *rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
