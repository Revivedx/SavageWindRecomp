// Exercise real game-font decoding without booting the guest. Supply a RomFS
// directory from the user's dump; no font fixtures are stored in the repository.
#include <assert.h>
#include <string.h>
#include "../runtime/ui/quick_hint_font.mm"
extern "C" { const char *g_romfs_dir; }
const char *tr(const char *s) {
    return !strcmp(s, "Drop") ? "Выбросить" : !strcmp(s, "Sort") ? "Сортировать" : "Назад";
}
int main(int argc, char **argv) {
    assert(argc == 2);
    ImGui::CreateContext();
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    atlas->AddFontDefault();
    ImFont *text, *buttons;
    g_romfs_dir = "/nonexistent";
    quick_hint_fonts_load(atlas, &text, &buttons);
    assert(!text && !buttons); // missing optional resources preserve fallback
    g_romfs_dir = argv[1];
    quick_hint_fonts_load(atlas, &text, &buttons);
    assert(text && buttons);
    assert(text->FindGlyphNoFallback(0x412)); // Cyrillic В from the game
    assert(text->FindGlyphNoFallback(0x44b)); // ы
    assert(text->FindGlyphNoFallback(0x421)); // С appears only in the translated Sort label
    assert(buttons->FindGlyphNoFallback(0xe041));
    assert(text->CalcTextSizeA(32, FLT_MAX, 0, "Выбросить").x > 100);
    const ImFontGlyph *a = text->FindGlyphNoFallback('A');
    assert(a && a->Visible && a->AdvanceX > 15);
    unsigned char *rgba; int w, h;
    atlas->GetTexDataAsRGBA32(&rgba, &w, &h);
    assert(w > 0 && h > 0);
    // Native letters and icon masks must contain coverage, not blank rectangles.
    const ImFontGlyph *masks[] = {a, buttons->FindGlyphNoFallback(0xe041)};
    for (const ImFontGlyph *g : masks) {
        int lit = 0;
        for (int y = (int)(g->V0 * h); y < (int)(g->V1 * h); y++)
            for (int x = (int)(g->U0 * w); x < (int)(g->U1 * w); x++)
                lit += rgba[(y * w + x) * 4 + 3] > 128;
        assert(lit > 30);
    }
    // Invalid font headers must fail without registering custom cells.
    unsigned char damaged[128] = {};
    std::vector<Cell> cells;
    assert(!append(atlas, damaged, sizeof damaged, {65}, cells));
    assert(!append(atlas, damaged, 4, {65}, cells));
    assert(cells.empty());
    ImGui::DestroyContext();
    puts("ok: original bitmap text, Cyrillic, button artwork, atlas upload, missing/malformed fallback");
}
