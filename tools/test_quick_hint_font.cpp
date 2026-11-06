// Exercise real game-font decoding without booting the guest. Supply a RomFS
// directory from the user's dump; no font fixtures are stored in the repository.
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include "../runtime/ui/quick_hint_font.mm"
extern "C" {
#include "../runtime/settings.h"
#include "../runtime/input_bind.h"
const char *g_romfs_dir;
Settings g_settings;
static int device = DEVICE_PAD, family;
int input_last_device(void) { return device; }
int input_pad_family(void) { return family; }
bool pc_cursor_preview_active(void) { return false; }
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t cap) {
    (void)name;
    snprintf(out, cap, "%s", dev_rel);
    FILE *file = fopen(out, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}
}
const char *tr(const char *s) {
    return !strcmp(s, "Sort") ? "Сортировать" : s;
}

static std::vector<unsigned char> button_mask(int kind) {
    ImGui::CreateContext();
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    atlas->AddFontDefault();
    g_settings.button_icons = kind;
    ImFont *text, *buttons;
    quick_hint_fonts_load(atlas, &text, &buttons);
    assert(buttons);
    unsigned char *pixels; int w, h;
    atlas->GetTexDataAsAlpha8(&pixels, &w, &h);
    const ImFontGlyph *g = buttons->FindGlyphNoFallback(0xe042);
    assert(g && g->Visible);
    std::vector<unsigned char> result;
    for (int y = (int)(g->V0 * h); y < (int)(g->V1 * h); y++)
        for (int x = (int)(g->U0 * w); x < (int)(g->U1 * w); x++) result.push_back(pixels[y * w + x]);
    ImGui::DestroyContext();
    return result;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    unsetenv("SAVAGEWIND_ROMFS_OVERLAY");
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);
    g_settings.button_icons = ICONS_NINTENDO;
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
    assert(text->FindGlyphNoFallback(0x440)); // Cyrillic р from the translated Sort label
    assert(text->FindGlyphNoFallback(0x421)); // С appears only in the translated Sort label
    assert(buttons->FindGlyphNoFallback(0xe041));
    assert(text->CalcTextSizeA(32, FLT_MAX, 0, "Сортировать").x > 100);
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

    auto native = button_mask(ICONS_NINTENDO);
    auto xbox = button_mask(ICONS_XBOX);
    auto ps = button_mask(ICONS_PLAYSTATION);
    auto keyboard = button_mask(ICONS_KEYBOARD);
    assert(native != xbox && xbox != ps && ps != keyboard);
    g_bind_pad[BIND_X] = SDL_GAMEPAD_BUTTON_SOUTH;
    auto ps_remapped = button_mask(ICONS_PLAYSTATION);
    assert(ps_remapped != ps);
    g_bind_key[BIND_X] = SDL_SCANCODE_Q;
    auto key_remapped = button_mask(ICONS_KEYBOARD);
    assert(key_remapped != keyboard);
    g_bind_key[BIND_X] = BIND_MOUSE + SDL_BUTTON_RIGHT;
    auto mouse = button_mask(ICONS_KEYBOARD);
    assert(mouse != key_remapped);
    device = DEVICE_PAD; family = 2;
    assert(button_mask(ICONS_AUTO) == ps_remapped);
    device = DEVICE_KEYBOARD;
    assert(button_mask(ICONS_AUTO) == mouse);

    // A synthetic replacement sheet must retain its own art, like a mod.
    std::vector<unsigned char> modified(0x20000, 0), linear(0x20000, 0x5a), before = linear;
    uint64_t source = (uintptr_t)modified.data();
    assert(!glyphs_apply(source, source, source + modified.size(), 0x49, 256, 1024,
                         linear.data(), 64, 256));
    assert(linear == before);

    // A replacement archive takes precedence over the base game's font. Use
    // deliberately invalid synthetic bytes, never a bundled game fixture.
    char overlay[] = "/tmp/quick-sort-font-XXXXXX";
    assert(mkdtemp(overlay));
    std::string directory = std::string(overlay) + "/Font";
    std::string file = directory + "/Font_EU.sbfarc";
    assert(!mkdir(directory.c_str(), 0700));
    FILE *replacement = fopen(file.c_str(), "wb");
    assert(replacement && fwrite("synthetic", 1, 9, replacement) == 9);
    fclose(replacement);
    setenv("SAVAGEWIND_ROMFS_OVERLAY", overlay, 1);
    ImGui::CreateContext();
    atlas = ImGui::GetIO().Fonts;
    atlas->AddFontDefault();
    quick_hint_fonts_load(atlas, &text, &buttons);
    assert(!text && !buttons);
    ImGui::DestroyContext();
    unsetenv("SAVAGEWIND_ROMFS_OVERLAY");
    assert(!unlink(file.c_str()) && !rmdir(directory.c_str()) && !rmdir(overlay));
    puts("ok: bitmap text, Cyrillic, four icon families, pad/key/mouse remaps, Auto, modified sheets and overlay precedence");
}
