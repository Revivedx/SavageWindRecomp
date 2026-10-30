// Achievement pictures: achievements/<id>.png (or .jpg) in the data root, else the game's own icon for it from the
// player's RomFS (g_ach_game_icons), loaded the first time one is needed. Same texture paths as the start screen's
// picture (splash.mm): Metal on macOS, Vulkan elsewhere.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "ach_icon.h"
#include "imageload.h"

extern "C" {
#include "paths.h"
#include "nvn/tegra.h"
#include "achievements.h"
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

// The texture of RGBA pixels (w x h), 0 if it couldn't be made
ImTextureID texture(const unsigned char *px, int w, int h) {
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
    return tex;
}

// A picture file (PNG / JPEG) that is there: 0 if it can't be read
ImTextureID load_file(const char *path, int w, int h) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        fprintf(stderr, "[achievements] %s: %dx%d, icons are up to 1024x1024\n", path, w, h);
        return 0;
    }
    unsigned char *px = rt_image_rgba(path, &w, &h);
    if (!px) {
        fprintf(stderr, "[achievements] %s: %s\n", path, rt_image_error());
        return 0;
    }
    ImTextureID tex = texture(px, w, h);
    rt_image_free(px);
    return tex;
}

std::vector<uint8_t> read_all(const char *path) {
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (64 << 20)) {
        d.resize((size_t)n);
        if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    }
    fclose(f);
    return d;
}

std::vector<uint8_t> yaz0(const std::vector<uint8_t> &s) {
    std::vector<uint8_t> o;
    if (s.size() < 16 || memcmp(s.data(), "Yaz0", 4)) return o;
    size_t n = (size_t)s[4] << 24 | s[5] << 16 | s[6] << 8 | s[7], i = 16;
    o.reserve(n);
    while (o.size() < n && i < s.size()) {
        uint8_t flags = s[i++];
        for (int b = 0; b < 8 && o.size() < n; b++) {
            if (flags & (0x80 >> b)) {
                if (i >= s.size()) return {};
                o.push_back(s[i++]);
                continue;
            }
            if (i + 1 >= s.size()) return {};
            size_t dist = ((s[i] & 15) << 8 | s[i + 1]) + 1, len = s[i] >> 4;
            i += 2;
            if (!len) {
                if (i >= s.size()) return {};
                len = s[i++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > o.size()) return {};
            for (size_t k = 0; k < len; k++) o.push_back(o[o.size() - dist]);
        }
    }
    return o.size() == n ? o : std::vector<uint8_t>();
}

template <class T> T rd(const std::vector<uint8_t> &d, uint64_t at) {
    T v = 0;
    if (at + sizeof v <= d.size()) memcpy(&v, d.data() + at, sizeof v);
    return v;
}

// An inventory icon: Yaz0 over a BFRES holding one BNTX texture, RGBA8 (sRGB) in the console's block-linear layout
ImTextureID load_stock_icon(const char *path) {
    std::vector<uint8_t> u = yaz0(read_all(path));
    auto find = [&](const char *magic, size_t from) {
        for (size_t k = from; k + 4 <= u.size(); k += 4)
            if (!memcmp(u.data() + k, magic, 4)) return k;
        return (size_t)-1;
    };
    static const char k_bntx[4] = {'B', 'N', 'T', 'X'}, k_brti[4] = {'B', 'R', 'T', 'I'};  // (the texture file, its texture)
    size_t bn = find(k_bntx, 0), ti = bn == (size_t)-1 ? bn : find(k_brti, bn);
    if (ti == (size_t)-1) {
        if (!u.empty()) fprintf(stderr, "[achievements] %s: no texture in it\n", path);
        return 0;
    }
    uint32_t fmt = rd<uint32_t>(u, ti + 0x1c), w = rd<uint32_t>(u, ti + 0x24), h = rd<uint32_t>(u, ti + 0x28);
    uint32_t bhl = rd<uint32_t>(u, ti + 0x34), size = rd<uint32_t>(u, ti + 0x50);
    uint64_t data = bn + rd<uint64_t>(u, bn + rd<uint64_t>(u, ti + 0x70));
    if ((fmt >> 8) != 0x0b || !w || !h || w > 1024 || h > 1024 || bhl > 5 || data + size > u.size()) {
        fprintf(stderr, "[achievements] %s: not an icon this reads (format %#x, %ux%u)\n", path, fmt, w, h);
        return 0;
    }
    std::vector<uint8_t> px((size_t)w * h * 4);
    tegra_deswizzle(u.data() + data, px.data(), w, h, 1, 4, bhl, 0);
    return texture(px.data(), (int)w, (int)h);
}

ImTextureID load(const char *id) {
    // the player's own picture first: achievements/<id>.png (or .jpg) in the data root
    static const char *const ext[] = {"png", "jpg", "jpeg"};
    char path[1200];
    int w, h;
    for (const char *e : ext) {
        snprintf(path, sizeof path, "%s/achievements/%s.%s", rt_data_root(), id, e);
        if (rt_image_info(path, &w, &h)) return load_file(path, w, h);
    }
    // else the game's
    for (int k = 0; k < g_ach_game_icon_count; k++) {
        const AchGameIcon &g = g_ach_game_icons[k];
        if (strcmp(g.id, id)) continue;
        if (g.icon[0] == '@') {
            snprintf(path, sizeof path, "%s/extracted/romfs/UI/PictureBook/%s.jpg", rt_data_root(), g.icon + 1);
            return rt_image_info(path, &w, &h) ? load_file(path, w, h) : 0;
        }
        snprintf(path, sizeof path, "%s/extracted/romfs/UI/StockItem/%s.sbitemico", rt_data_root(), g.icon);
        return load_stock_icon(path);
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
