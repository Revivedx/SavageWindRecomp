// Achievement pictures: achievements/<id>.png (or .jpg) in the data root, loaded the first time one is needed. Same texture
// paths as the start screen's picture (splash.mm): Metal on macOS, Vulkan elsewhere.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "ach_icon.h"
#include "imageload.h"

extern "C" {
#include "paths.h"
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
void vk_imgui_texture_free(uint64_t tex);
#endif
}

namespace {
std::unordered_map<std::string, ImTextureID> g_icons;  // (0 = looked for, not there)
#ifdef __APPLE__
std::vector<id<MTLTexture>> g_mtl;  // (ImGui only holds the pointer)
#endif

ImTextureID load(const char *id) {
    static const char *const ext[] = {"png", "jpg", "jpeg"};
    for (const char *e : ext) {
        char path[1200];
        snprintf(path, sizeof path, "%s/achievements/%s.%s", rt_data_root(), id, e);
        int w, h;
        if (!rt_image_info(path, &w, &h)) continue;  // (no such file)
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            fprintf(stderr, "[achievements] %s: %dx%d, icons are up to 1024x1024\n", path, w, h);
            return 0;
        }
        unsigned char *px = rt_image_rgba(path, &w, &h);
        if (!px) {
            fprintf(stderr, "[achievements] %s: %s\n", path, rt_image_error());
            return 0;
        }
        ImTextureID tex = 0;
#ifdef __APPLE__
        id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:w height:h mipmapped:NO];
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        if (t) {
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * 4];
            g_mtl.push_back(t);
            tex = (ImTextureID)(intptr_t)(__bridge void *)t;
        }
#else
        tex = (ImTextureID)vk_imgui_texture_rgba(px, w, h);
#endif
        rt_image_free(px);
        return tex;
    }
    return 0;
}
}  // namespace

ImTextureID ach_icon(const char *id) {
    if (!id || !*id) return 0;
    auto it = g_icons.find(id);
    if (it != g_icons.end()) return it->second;
    ImTextureID tex = load(id);
    g_icons[id] = tex;
    return tex;
}

void ach_icons_free(void) {
#ifdef __APPLE__
    g_mtl.clear();
#else
    for (auto &kv : g_icons)
        if (kv.second) vk_imgui_texture_free((uint64_t)kv.second);
#endif
    g_icons.clear();
}
