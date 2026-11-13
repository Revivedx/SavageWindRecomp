#include "quick_hint_font.h"
#include "imgui_internal.h"
#include "i18n.h"
#include <vector>
#include <algorithm>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "../rt.h"
#include "../services/archive.h"
#include "../nvn/tegra.h"
#include "../nvn/glyphs.h"
}

namespace {
struct Bitmap {
    std::vector<unsigned char> data;
    bool has(size_t o, size_t n) const { return o <= data.size() && n <= data.size() - o; }
    unsigned u16(size_t o) const { return data[o] | (unsigned)data[o + 1] << 8; }
    unsigned u32(size_t o) const { return u16(o) | u16(o + 2) << 16; }
    unsigned glyph(unsigned ch) const {
        size_t p = u32(0x30);
        for (int hop = 0; p && hop < 128; hop++) {
            if (p < 8 || !has(p - 8, 26) || memcmp(data.data() + p - 8, "CMAP", 4)) break;
            unsigned lo = u32(p), hi = u32(p + 4), method = u16(p + 8);
            if (lo > hi || hi > 0xffff) break;
            if (ch >= lo && ch <= hi) {
                if (method == 0) return u16(p + 16) + ch - lo;
                if (method == 1 && has(p + 16, (size_t)(hi - lo + 1) * 2)) return u16(p + 16 + (ch - lo) * 2);
                if (method == 2) {
                    unsigned count = u16(p + 16);
                    if (!has(p + 18, count * 4)) break;
                    for (unsigned i = 0; i < count; i++)
                        if (u16(p + 18 + i * 4) == ch) return u16(p + 20 + i * 4);
                }
                return 0xffff;
            }
            p = u32(p + 12);
        }
        return 0xffff;
    }
    bool width(unsigned gi, int &left, unsigned &advance) const {
        size_t p = u32(0x2c);
        for (int hop = 0; p && hop < 128; hop++) {
            if (p < 8 || !has(p - 8, 16) || memcmp(data.data() + p - 8, "CWDH", 4)) break;
            unsigned lo = u16(p), hi = u16(p + 2);
            size_t at = p + 8 + (size_t)(gi >= lo ? gi - lo : 0) * 3;
            if (gi >= lo && gi <= hi && has(at, 3)) {
                left = (signed char)data[at]; advance = data[at + 2]; return true;
            }
            p = u32(p + 4);
        }
        return false;
    }
};
struct Cell { int rect; std::vector<unsigned char> pixels; };

// BC4 coverage at a single texel in a linear sheet.
unsigned char coverage(const unsigned char *sheet, unsigned w, unsigned x, unsigned y) {
    const unsigned char *b = sheet + ((size_t)(y / 4) * (w / 4) + x / 4) * 8;
    unsigned char pal[8] = {b[0], b[1]};
    if (b[0] > b[1]) for (unsigned i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * b[0] + i * b[1]) / 7;
    else {
        for (unsigned i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * b[0] + i * b[1]) / 5;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (i * 8);
    return pal[(bits >> (3 * ((y % 4) * 4 + x % 4))) & 7];
}

ImFont *append(ImFontAtlas *atlas, const unsigned char *src, size_t size,
               const std::vector<unsigned> &chars, std::vector<Cell> &cells, bool buttons = false) {
    Bitmap f; f.data.assign(src, src + size);
    if (!f.has(0, 0x60) || memcmp(src, "FFNT\xff\xfe", 6) || f.u32(8) != 0x04010000 || f.u32(0xc) != size) return nullptr;
    size_t t = f.u32(0x28);
    if (t < 8 || !f.has(t - 8, 32) || memcmp(src + t - 8, "TGLP", 4)) return nullptr;
    unsigned cw = src[t], ch = src[t + 1], sheets = src[t + 2];
    unsigned cols = f.u16(t + 12), rows = f.u16(t + 14), w = f.u16(t + 16), h = f.u16(t + 18);
    size_t bntx = f.u32(t + 20);
    if (!cw || !ch || !cols || !rows || !sheets || sheets > 16 || !w || !h || w > 2048 || h > 2048 ||
        w % 4 || h % 4 || cols * (cw + 1) > w || rows * (ch + 1) > h ||
        !f.has(bntx, 0x20) || memcmp(src + bntx, "BNTX", 4)) return nullptr;
    // Switch 4.1 font sheets have a single BNTX texture, with a mip pointer
    // relative to the BNTX base. Validate its format and bounds before decoding.
    size_t brti = 0;
    for (size_t p = bntx; f.has(p, 0x78) && p < bntx + 0x1000; p += 4)
        if (!memcmp(src + p, "BRTI", 4)) { brti = p; break; }
    if (!brti || f.u32(brti + 0x1c) != 0x1d01 || f.u32(brti + 0x24) != w || f.u32(brti + 0x28) != h) return nullptr;
    size_t ptr = bntx + f.u32(brti + 0x70);
    if (!f.has(ptr, 8)) return nullptr;
    size_t start = bntx + f.u32(ptr), sheet_size = (size_t)w * h / 2;
    if (!f.has(start, sheet_size * sheets)) return nullptr;
    std::vector<unsigned char> linear(sheet_size * sheets);
    for (unsigned i = 0; i < sheets; i++)
        tegra_deswizzle(src + start + i * sheet_size, linear.data() + i * sheet_size,
                       w / 4, h / 4, 1, 8, tegra_default_block_height_log2(h / 4), 0);
    // Use exactly the game's icon policy, including raw Switch bindings,
    // keyboard/mouse and controller families. Modified sheets are kept intact
    // by glyphs_apply's content check, just as in the guest renderer.
    if (buttons && sheets == 1)
        glyphs_apply((uintptr_t)(src + start), (uintptr_t)src, (uintptr_t)src + size,
                     0x49, w, h, linear.data(), w / 4, h / 4);
    ImFontConfig cfg;
    cfg.SizePixels = ch;
    snprintf(cfg.Name, sizeof cfg.Name, "quick_selector_bitmap");
    ImFont *font = atlas->AddFontDefault(&cfg);
    for (unsigned code : chars) {
        unsigned gi = f.glyph(code), sheet = gi / (cols * rows), local = gi % (cols * rows), advance;
        int left;
        if (gi == 0xffff || sheet >= sheets || !f.width(gi, left, advance)) continue;
        unsigned x0 = local % cols * (cw + 1), y0 = local / cols * (ch + 1);
        Cell cell;
        cell.rect = atlas->AddCustomRectFontGlyph(font, (ImWchar)code, cw, ch, advance, ImVec2((float)left, 0));
        cell.pixels.resize(cw * ch);
        for (unsigned y = 0; y < ch; y++) for (unsigned x = 0; x < cw; x++)
            cell.pixels[y * cw + x] = coverage(linear.data() + sheet * sheet_size, w, x0 + x, h - 1 - y0 - y);
        cells.push_back(std::move(cell));
    }
    return font;
}
} // namespace

void quick_hint_fonts_load(ImFontAtlas *atlas, ImFont **text, ImFont **buttons) {
    *text = *buttons = nullptr;
    if (!g_romfs_dir) return;
    char path[1200];
    const char *overlay = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    FILE *file = nullptr;
    if (overlay && *overlay) {
        snprintf(path, sizeof path, "%s/Font/Font_EU.sbfarc", overlay);
        file = fopen(path, "rb");
    }
    if (!file) {
        snprintf(path, sizeof path, "%s/Font/Font_EU.sbfarc", g_romfs_dir);
        file = fopen(path, "rb");
    }
    if (!file) return;
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    if (length <= 0 || length > 16 * 1024 * 1024) { fclose(file); return; }
    std::vector<unsigned char> packed(length);
    bool read = fread(packed.data(), 1, length, file) == (size_t)length; fclose(file);
    if (!read) return;
    size_t size;
    unsigned char *archive = yaz0_unpack(packed.data(), packed.size(), &size);
    if (!archive) return;
    std::vector<unsigned> chars;
    for (unsigned c = 32; c < 127; c++) chars.push_back(c);
    for (const char *label : {tr("Sort"), tr("Drop"), tr("Back")}) {
        while (*label) {
            unsigned code; int n = ImTextCharFromUtf8(&code, label, nullptr);
            if (!n) break;
            label += n;
            if (code >= 127 && code <= 0xffff && std::find(chars.begin(), chars.end(), code) == chars.end()) chars.push_back(code);
        }
    }
    std::vector<unsigned> icons;
    for (unsigned c = 0xe040; c <= 0xe043; c++) icons.push_back(c);
    std::vector<Cell> cells;
    for (int i = 0; i < 2; i++) {
        uint32_t base, lo, hi;
        if (sarc_find(archive, size, i ? "External_00.bffnt" : "Normal_00.bffnt", &base, &lo, &hi) >= 0)
            *(i ? buttons : text) = append(atlas, archive + base + lo, hi - lo, i ? icons : chars, cells, i != 0);
    }
    free(archive);
    if (cells.empty()) return;
    unsigned char *pixels; int w, h;
    atlas->GetTexDataAsAlpha8(&pixels, &w, &h);
    for (const Cell &cell : cells) {
        const ImFontAtlasCustomRect *r = atlas->GetCustomRectByIndex(cell.rect);
        for (unsigned y = 0; y < r->Height; y++)
            memcpy(pixels + (r->Y + y) * w + r->X, cell.pixels.data() + y * r->Width, r->Width);
    }
    LOG("[quick-hint] loaded game bitmap fonts (%zu glyphs)\n", cells.size());
}
