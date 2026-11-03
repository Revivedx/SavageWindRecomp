#include <cassert>
#include <string>
#include "../runtime/ui/game_font.mm"

extern "C" const char *language_text_folder(void) {
    const char *folder = getenv("SAVAGEWIND_TEST_FONT_FOLDER");
    return folder ? folder : "USen";
}
extern "C" uint8_t *fs_game_file(const char *path, size_t *size) {
    const char *root = getenv("SAVAGEWIND_TEST_FONT_ROOT");
    *size = 0;
    if (!root) return nullptr;
    std::string filename = std::string(root) + "/" + path;
    FILE *file = fopen(filename.c_str(), "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    assert(length > 0);
    uint8_t *data = (uint8_t *)malloc(length);
    assert(data && fread(data, 1, length, file) == (size_t)length);
    fclose(file);
    *size = length;
    return data;
}

static void store16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
    data[offset] = value;
    data[offset + 1] = value >> 8;
}
static void store32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    store16(data, offset, value);
    store16(data, offset + 2, value >> 16);
}

int main(void) {
    std::vector<uint8_t> data(0x1200);
    memcpy(data.data(), "FFNT", 4);
    store16(data, 4, 0xfeff);
    store16(data, 6, 0x20);
    memcpy(data.data() + 0x20, "FINF", 4);
    data[0x2b] = 3;
    store16(data, 0x2c, 4);
    store32(data, 0x34, 0x48);
    store32(data, 0x38, 0x88);
    store32(data, 0x3c, 0xa8);
    memcpy(data.data() + 0x40, "TGLP", 4);
    store32(data, 0x44, 0x11c0);
    data[0x48] = data[0x49] = 3;
    data[0x4a] = 1;
    store16(data, 0x52, 8);
    store16(data, 0x54, 1);
    store16(data, 0x56, 1);
    store16(data, 0x58, 64);
    store16(data, 0x5a, 8);
    data[0x91] = 3;
    data[0x92] = 4;
    store32(data, 0xa8, 'A');
    store32(data, 0xac, 'A');
    memset(data.data() + 0x1000, 255, 512);
    GameFont parsed;
    assert(parse(data.data(), data.size(), parsed));
    assert(parsed.index['A'] == 0 && parsed.widths[0].advance == 4);
    assert(parsed.sheets.size() == 1 && parsed.sheets[0][0] == 255);
    for (size_t length = 0; length < 0x100; length++) {
        GameFont truncated;
        assert(!parse(data.data(), length, truncated));
    }
    auto invalid = data;
    store32(invalid, 0x34, 0xfffffff0);
    GameFont overflow;
    assert(!parse(invalid.data(), invalid.size(), overflow));
    invalid = data;
    store16(invalid, 0x54, 0);
    GameFont zero_columns;
    assert(!parse(invalid.data(), invalid.size(), zero_columns));
    invalid = data;
    store32(invalid, 0x8c, 0x88);
    GameFont cycle;
    assert(!parse(invalid.data(), invalid.size(), cycle));
    ImGui::CreateContext();
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFont *font = atlas->AddFontDefault();
    const ImWchar ranges[] = {'A', 'A', 0};
    game_font_add(&parsed, atlas, font, ranges, GAME_FONT_FILL);
    atlas->Build();
    game_font_finish(atlas);
    assert(font->FindGlyph('A')->AdvanceX == 4 && g_rects.empty() && g_fonts.empty());
    atlas->Clear();
    if (getenv("SAVAGEWIND_TEST_FONT_ROOT")) {
        GameFont *light = game_font_load("NormalS_00.bffnt");
        GameFont *bold = game_font_load("Normal_00.bffnt");
        if (getenv("SAVAGEWIND_TEST_FONT_ABSENT")) assert(!light && !bold);
        else assert(light && bold && game_font_line(light) > 0 && game_font_edged(light));
        font = atlas->AddFontDefault();
        game_font_add(light, atlas, font, atlas->GetGlyphRangesDefault(), GAME_FONT_FILL);
        atlas->Build();
        game_font_finish(atlas);
        assert(g_rects.empty() && g_fonts.empty());
    } else assert(!game_font_load("Normal_00.bffnt"));
    game_font_finish(atlas);
    ImGui::DestroyContext();
    puts("PASS: game font parsing, malformed input, atlas glyphs and missing-font fallback");
}
