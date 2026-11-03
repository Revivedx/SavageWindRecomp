#include "game_font.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

#include "imgui.h"

extern "C" {
#include "../language.h"
#include "../nvn/tegra.h"
#include "../rt.h"
#include "../services/archive.h"
uint8_t *fs_game_file(const char *path, size_t *out_n);
}

struct GameFont {
    struct Width { int8_t left; uint8_t glyph, advance; };
    int cell_w = 0, cell_h = 0, cols = 0, rows = 0, sheet_w = 0, ascent = 0, line = 0;
    bool edged = false;
    std::vector<std::vector<uint8_t>> sheets;
    std::vector<Width> widths;
    std::vector<uint16_t> index;
};

namespace {

struct Rect { GameFont *source_font; int id, glyph, layer; };
struct Added { ImFont *font; int count, ascent; };
std::vector<std::unique_ptr<GameFont>> g_fonts;
std::vector<Rect> g_rects;
std::vector<Added> g_added;
uint8_t *g_arc;
size_t g_arc_n;

uint16_t rd16(const uint8_t *offset) { return (uint16_t)(offset[0] | offset[1] << 8); }
uint32_t rd32(const uint8_t *offset) { return offset[0] | offset[1] << 8 | offset[2] << 16 | (uint32_t)offset[3] << 24; }

void bc4_decode(const uint8_t *lin, uint32_t wb, uint32_t hb, uint8_t *img) {
    uint32_t glyph_width = wb * 4;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *block = lin + ((uint64_t)by * wb + bx) * 8;
            uint8_t r0 = block[0], r1 = block[1], pal[8] = {r0, r1};
            if (r0 > r1) for (int index = 1; index < 7; index++) pal[index + 1] = (uint8_t)(((7 - index) * r0 + index * r1) / 7);
            else { for (int index = 1; index < 5; index++) pal[index + 1] = (uint8_t)(((5 - index) * r0 + index * r1) / 5); pal[6] = 0, pal[7] = 255; }
            uint64_t bits = 0;
            for (int index = 0; index < 6; index++) bits |= (uint64_t)block[2 + index] << (8 * index);
            for (int index = 0; index < 16; index++) img[(uint64_t)(by * 4 + index / 4) * glyph_width + bx * 4 + index % 4] = pal[(bits >> (3 * index)) & 7];
        }
}

bool parse(const uint8_t *font_bytes, size_t byte_count, GameFont &font) {
    if (byte_count < 0x40 || memcmp(font_bytes, "FFNT", 4) || rd16(font_bytes + 4) != 0xFEFF) return false;
    size_t header_size = rd16(font_bytes + 6);
    if (header_size > byte_count - 0x20) return false;
    const uint8_t *fi = font_bytes + header_size;
    if (fi + 0x20 > font_bytes + byte_count || memcmp(fi, "FINF", 4)) return false;
    font.edged = fi[8] == 2, font.ascent = fi[11], font.line = rd16(fi + 12);
    uint32_t glyph = rd32(fi + 20), width = rd32(fi + 24), map = rd32(fi + 28);

    if (glyph < 8 || (uint64_t)glyph + 0x18 > byte_count || memcmp(font_bytes + glyph - 8, "TGLP", 4)) return false;
    const uint8_t *texture_header = font_bytes + glyph;
    font.cell_w = texture_header[0], font.cell_h = texture_header[1];
    int sheets = texture_header[2], format = rd16(texture_header + 10), sheet_h = rd16(texture_header + 18);
    font.cols = rd16(texture_header + 12), font.rows = rd16(texture_header + 14), font.sheet_w = rd16(texture_header + 16);
    bool bc4 = format == 12;
    if ((format != 12 && format != 8) || !sheets || !font.cell_w || !font.cell_h || !font.cols || !font.rows ||
        !font.sheet_w || !sheet_h || font.sheet_w > 4096 || sheet_h > 4096 || sheets > 64 ||
        font.sheet_w % 4 || sheet_h % 4 || !font.line) return false;
    uint32_t wb = bc4 ? font.sheet_w / 4 : font.sheet_w, hb = bc4 ? sheet_h / 4 : sheet_h, bpb = bc4 ? 8 : 1, bytes = wb * hb * bpb;
    if ((font.cell_w + 1) * font.cols > font.sheet_w || (font.cell_h + 1) * font.rows > sheet_h + 1) return false;
    uint64_t end = (uint64_t)glyph - 8 + rd32(font_bytes + glyph - 4);
    if (end > byte_count || end < (uint64_t)sheets * bytes + glyph) return false;
    uint64_t data = (end - (uint64_t)sheets * bytes) & ~0xfffull;
    if (data < (uint64_t)glyph + 0x18 || data > byte_count || (uint64_t)sheets * bytes > byte_count - data) return false;
    int bh = 4;
    while (bh > 0 && (hb + 7) / 8 <= (1u << (bh - 1))) bh--;
    std::vector<uint8_t> lin(bytes), img((size_t)font.sheet_w * sheet_h);
    for (int sheet_index = 0; sheet_index < sheets; sheet_index++) {
        tegra_deswizzle(font_bytes + data + (uint64_t)sheet_index * bytes, lin.data(), wb, hb, 1, bpb, bh, 0);
        if (bc4) bc4_decode(lin.data(), wb, hb, img.data());
        else memcpy(img.data(), lin.data(), img.size());
        std::vector<uint8_t> top(img.size());
        for (int row = 0; row < sheet_h; row++)
            memcpy(&top[(size_t)row * font.sheet_w], &img[(size_t)(sheet_h - 1 - row) * font.sheet_w], font.sheet_w);
        font.sheets.push_back(std::move(top));
    }

    for (uint32_t offset = width; offset >= 8 && (uint64_t)offset + 8 <= byte_count;) {
        uint32_t first = rd16(font_bytes + offset), last = rd16(font_bytes + offset + 2), next = rd32(font_bytes + offset + 4);
        if (last < first || offset + 8 + (uint64_t)(last - first + 1) * 3 > byte_count) return false;
        if (font.widths.size() <= last) font.widths.resize(last + 1);
        for (uint32_t index = first; index <= last; index++) {
            const uint8_t *glyph_width = font_bytes + offset + 8 + (index - first) * 3;
            if (glyph_width[1] > font.cell_w) return false;
            font.widths[index] = {(int8_t)glyph_width[0], glyph_width[1], glyph_width[2]};
        }
        if (!next) break;
        if (next <= offset || next >= byte_count) return false;
        offset = next;
    }

    font.index.assign(0x10000, 0xffff);
    for (uint32_t offset = map; offset >= 8 && (uint64_t)offset + 18 <= byte_count;) {
        uint32_t lo = rd32(font_bytes + offset), hi = rd32(font_bytes + offset + 4), how = rd16(font_bytes + offset + 8), next = rd32(font_bytes + offset + 12);
        if (hi < lo || hi >= 0x10000 || how > 2) return false;
        const uint8_t *map_data = font_bytes + offset + 16;
        if (how == 0 && hi >= lo) {
            for (uint32_t character = lo; character <= hi && character < 0x10000; character++) font.index[character] = (uint16_t)(character - lo + rd16(map_data));
        } else if (how == 1 && hi >= lo && map_data + (uint64_t)(hi - lo + 1) * 2 <= font_bytes + byte_count) {
            for (uint32_t character = lo; character <= hi && character < 0x10000; character++) font.index[character] = rd16(map_data + (character - lo) * 2);
        } else if (how == 2) {
            uint32_t count = rd16(map_data);
            if ((uint64_t)offset + 20 + (uint64_t)count * 8 > byte_count) return false;
            for (uint32_t index = 0; index < count && map_data + 4 + (uint64_t)index * 8 + 8 <= font_bytes + byte_count; index++) {
                uint32_t character = rd32(map_data + 4 + index * 8);
                if (character < 0x10000) font.index[character] = rd16(map_data + 8 + index * 8);
            }
        }
        if (!next) break;
        if (next <= offset || next >= byte_count) return false;
        offset = next;
    }
    return true;
}

}

GameFont *game_font_load(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "Font/Font_%.2s.sbfarc", language_text_folder());
    if (!g_arc) {
        size_t zn = 0;
        uint8_t *compressed = fs_game_file(path, &zn);
        g_arc = compressed ? yaz0_unpack(compressed, zn, &g_arc_n) : NULL;
        free(compressed);
    }
    uint32_t off, sheet_index, end;
    auto font = std::make_unique<GameFont>();
    bool ok = g_arc && sarc_find(g_arc, g_arc_n, name, &off, &sheet_index, &end) >= 0 && off + (uint64_t)end <= g_arc_n && end > sheet_index &&
              parse(g_arc + off + sheet_index, end - sheet_index, *font);
    LOG("[ui] game font %s in %s: %s\n", name, path, ok ? "read" : "not read");
    if (!ok) return NULL;
    g_fonts.push_back(std::move(font));
    return g_fonts.back().get();
}

float game_font_line(const GameFont *source_font) { return source_font ? (float)source_font->line : 0; }
bool game_font_edged(const GameFont *source_font) { return source_font && source_font->edged; }

void game_font_add(GameFont *source_font, ImFontAtlas *atlas, ImFont *font, const ImWchar *ranges, int layer) {
    if (!source_font || !font) return;
    int per_sheet = source_font->cols * source_font->rows, count = 0;
    for (const ImWchar *range = ranges; range && range[0]; range += 2)
        for (unsigned character = range[0]; character <= range[1]; character++) {
            int index = source_font->index[character];
            if (index == 0xffff || index >= (int)source_font->widths.size() || index / per_sheet >= (int)source_font->sheets.size()) continue;
            const GameFont::Width &glyph_width = source_font->widths[index];
            int id = atlas->AddCustomRectFontGlyph(font, (ImWchar)character, glyph_width.glyph ? glyph_width.glyph : 1, source_font->cell_h, glyph_width.advance,
                                                   ImVec2(glyph_width.left, 0));
            g_rects.push_back({source_font, id, glyph_width.glyph ? index : -1, layer});
            count++;
        }
    g_added.push_back({font, count, source_font->ascent});
}

void game_font_finish(ImFontAtlas *atlas) {
    if (atlas->TexPixelsAlpha8) {
        for (const Rect &rc : g_rects) {
            const ImFontAtlasCustomRect *range = atlas->GetCustomRectByIndex(rc.id);
            const GameFont *source_font = rc.source_font;
            int per_sheet = source_font->cols * source_font->rows, cell = rc.glyph % per_sheet;
            int x0 = cell % source_font->cols * (source_font->cell_w + 1), y0 = cell / source_font->cols * (source_font->cell_h + 1);
            for (int row = 0; row < range->Height; row++) {
                uint8_t *dst = atlas->TexPixelsAlpha8 + (size_t)(range->Y + row) * atlas->TexWidth + range->X;
                if (rc.glyph < 0) { memset(dst, 0, range->Width); continue; }
                const uint8_t *src = &source_font->sheets[rc.glyph / per_sheet][(size_t)(y0 + row) * source_font->sheet_w + x0];
                for (int column = 0; column < range->Width; column++) {
                    int coverage = src[column];
                    if (source_font->edged) coverage = rc.layer == GAME_FONT_EDGE ? (coverage < 128 ? coverage * 2 : 255) : (coverage > 128 ? (coverage - 128) * 255 / 127 : 0);
                    dst[column] = (uint8_t)coverage;
                }
            }
        }

        for (const Added &added_font : g_added) {
            float dy = added_font.font->Ascent - added_font.ascent;
            for (int glyph_index = added_font.font->Glyphs.Size - added_font.count; glyph_index < added_font.font->Glyphs.Size; glyph_index++)
                if (glyph_index >= 0) added_font.font->Glyphs[glyph_index].Y0 += dy, added_font.font->Glyphs[glyph_index].Y1 += dy;
        }
    }
    g_rects.clear(), g_added.clear(), g_fonts.clear();
    free(g_arc);
    g_arc = NULL, g_arc_n = 0;
}
