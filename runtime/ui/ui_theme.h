// Savage Wind's UI look, shared by Savage Wind Configuration (runtime/ui/launcher.mm) and the in-game menu (F1,
// runtime/ui/menu.mm): the same look as SWMM+, the mod manager, so both programs read as one. Its colours, fonts and
// sizes come from SWMM+'s theme (crates/widgets/src/visuals.rs there):
//
// - a dark palette: the frame (header, sidebar, status bar) on UI_BASE, pages on UI_PANEL, cards on UI_RAISED, the
//   selected sidebar item on UI_HOVER with a UI_ACCENT_LIGHT bar, borders UI_BORDER;
// - one accent, UI_ACCENT (primary buttons, a selected choice, progress), with green / yellow / red for states;
// - Segoe UI on Windows (the system's own, not shipped), else Noto Sans when the system has it, else the shipped Fira
//   Sans; Font Awesome's solid icons (shipped, OFL: third_party/fonts/FontAwesome) merged into each font, as in SWMM+;
// - controls rounded 5, cards 8, windows 10; 14 px text, 12 px small, 20 px bold headings, all times the layout unit
//   (ui_unit), so the same layout fills a window, a TV or a Steam Deck.
//
// The wordmark (docs/media/wordmark.png, shipped as wordmark.png) is the name in SWMM+'s title lettering, rendered to
// a picture: the font it was drawn with isn't shipped.
#pragma once
#include "imgui.h"

// colours (RGBA)
#define UI_BASE         IM_COL32(0x0D, 0x0E, 0x10, 255)
#define UI_PANEL        IM_COL32(0x13, 0x14, 0x17, 255)
#define UI_RAISED       IM_COL32(0x19, 0x1B, 0x1E, 255)
#define UI_HOVER        IM_COL32(0x22, 0x25, 0x29, 255)
#define UI_NAV_HOVER    IM_COL32(0x1F, 0x22, 0x27, 255)  // (a sidebar item under the mouse: clearly lit)
#define UI_ROW_HOVER    IM_COL32(0x2C, 0x30, 0x37, 255)  // (a settings row or list item under the mouse / cursor)
#define UI_BORDER       IM_COL32(0x25, 0x28, 0x2D, 255)
#define UI_CONTROL      IM_COL32(0x1E, 0x21, 0x25, 255)
#define UI_CONTROL_HOT  IM_COL32(0x32, 0x36, 0x3D, 255)
#define UI_TRACK        IM_COL32(0x2A, 0x2D, 0x33, 255)
#define UI_FIELD        IM_COL32(0x0A, 0x0B, 0x0D, 255)
#define UI_TEXT         IM_COL32(0xE7, 0xE8, 0xEA, 255)
#define UI_TEXT_DIM     IM_COL32(0xA9, 0xAD, 0xB4, 255)
#define UI_MUTED        IM_COL32(0x86, 0x8B, 0x93, 255)
#define UI_FAINT        IM_COL32(0x4A, 0x4F, 0x57, 255)
#define UI_ACCENT       IM_COL32(0x00, 0x78, 0xD4, 255)
#define UI_ACCENT_HOT   IM_COL32(0x1A, 0x86, 0xDD, 255)
#define UI_ACCENT_LIGHT IM_COL32(0x60, 0xCD, 0xFF, 255)
#define UI_GREEN        IM_COL32(0x6C, 0xCB, 0x5F, 255)
#define UI_BLUE         IM_COL32(0x38, 0xB6, 0xF1, 255)
#define UI_YELLOW       IM_COL32(0xFC, 0xE1, 0x00, 255)
#define UI_RED          IM_COL32(0xFF, 0x99, 0xA4, 255)

// Font Awesome 6 solid icons (UTF-8), the ones the menus use
#define ICON_HOUSE        "\xef\x80\x95"  // f015
#define ICON_FOLDER       "\xef\x81\xbb"  // f07b
#define ICON_FOLDER_OPEN  "\xef\x81\xbc"  // f07c
#define ICON_FLOPPY       "\xef\x83\x87"  // f0c7
#define ICON_PUZZLE       "\xef\x84\xae"  // f12e
#define ICON_GAMEPAD      "\xef\x84\x9b"  // f11b
#define ICON_DESKTOP      "\xef\x8e\x90"  // f390
#define ICON_VOLUME       "\xef\x80\xa8"  // f028
#define ICON_GLOBE        "\xef\x82\xac"  // f0ac
#define ICON_STAR         "\xef\x80\x85"  // f005
#define ICON_TROPHY       "\xef\x82\x91"  // f091
#define ICON_QUESTION     "\xef\x81\x99"  // f059
#define ICON_GEAR         "\xef\x80\x93"  // f013
#define ICON_PLAY         "\xef\x81\x8b"  // f04b
#define ICON_CHECK        "\xef\x80\x8c"  // f00c
#define ICON_WARNING      "\xef\x81\xb1"  // f071
#define ICON_UPDATE       "\xef\x82\xaa"  // f0aa
#define ICON_DOWNLOAD     "\xef\x80\x99"  // f019
#define ICON_EXIT         "\xef\x8b\xb5"  // f2f5
#define ICON_XMARK        "\xef\x80\x8d"  // f00d
#define ICON_MAGIC        "\xee\x8b\x8a"  // e2ca
#define ICON_LOCATION     "\xef\x8f\x85"  // f3c5
#define ICON_CUBE         "\xef\x86\xb2"  // f1b2
#define ICON_INFO         "\xef\x81\x9a"  // f05a
#define ICON_SLIDERS      "\xef\x87\x9e"  // f1de
#define ICON_KEYBOARD     "\xef\x84\x9c"  // f11c
#define ICON_CIRCLE_CHECK "\xef\x81\x98"  // f058
#define ICON_EXTERNAL     "\xef\x82\x8e"  // f08e
#define ICON_CHEVRON_DOWN "\xef\x81\xb8"  // f078
#define ICON_CHEVRON_LEFT "\xef\x81\x93"  // f053
#define ICON_CHEVRON_RIGHT "\xef\x81\x94"  // f054
#define ICON_TRASH        "\xef\x87\xb8"  // f1f8
#define ICON_ZIP          "\xef\x87\x86"  // f1c6
#define ICON_FONT         "\xef\x80\xb1"  // f031
#define ICON_HARD_DRIVE   "\xef\x82\xa0"  // f0a0
#define ICON_ROTATE       "\xef\x8b\xb1"  // f2f1

// The fonts a context uses (ui_theme_fonts): body text (and its icons), its semibold, the bold headings, built for
// one layout unit (14 px text at 14 * unit px): draw them with ImGui's font scale = ui_unit / fonts.unit. Build them
// again when the unit changes a lot (another output size), so the text stays sharp.
struct UiFonts {
    ImFont *body = nullptr, *semibold = nullptr, *bold = nullptr;
    float unit = 2;
};
// Loads them into the atlas (after io.Fonts->Clear()); ranges: the text's glyph ranges (the menu language's); the
// menu language's fallback glyphs (runtime/ui/i18n.mm) and the icons are merged into each. False: none could be
// loaded (the caller keeps its own).
bool ui_theme_fonts(ImFontAtlas *atlas, UiFonts *out, const ImWchar *ranges, float unit);
// The unit to build fonts for at layout unit u (steps of 0.5, 1..3: a resize doesn't rebuild them every frame)
inline float ui_font_unit(float u) {
    float f = (float)(int)(u * 2 + 0.5f) * 0.5f;
    return f < 1 ? 1 : f > 3 ? 3 : f;
}

// The style for a context, its sizes times u (the layout unit: 1 = the design's 1180 x 740 window)
void ui_theme_style(ImGuiStyle &s, float u);

// The layout unit for an output this size: the design's proportions, readable on a TV and a Steam Deck (a 1080 p
// output: 1.8), times the player's menu text size (Options: percent, 100 = as designed)
float ui_unit(float w, float h, int text_pct);

// The wordmark: loaded once per ImGui renderer (it is a texture), freed with the context's renderer
struct UiImage {
    ImTextureID tex = 0;
    int w = 0, h = 0;
};
UiImage ui_wordmark(void);
UiImage ui_image(const char *file);  // (another of these pictures: docs/media/<file>, shipped beside the program)
void ui_wordmark_free(void);         // (all of this context's pictures)
// A subtitle beside the wordmark drawn at wordmark_at, wordmark_h tall: the picture file (the same lettering, e.g.
// "Configuration": docs/media/wordmark_config.png) on the wordmark's baseline in col; without it, text in the body font
void ui_draw_subtitle(ImDrawList *dl, ImVec2 wordmark_at, float wordmark_h, const char *file, const char *text, ImU32 col);
// The wordmark h tall at p (shadow: a soft shadow under it, over a picture)
void ui_draw_wordmark(ImDrawList *dl, ImVec2 p, float h, bool shadow);

// ---- widgets (all sized in layout units u)
// A button: kind 0 secondary (dark), 1 primary (accent), 2 danger (red outline); icon (or NULL) before the label.
// size.x 0: fits the label. True when pressed.
enum { UI_BTN_SECONDARY, UI_BTN_PRIMARY, UI_BTN_DANGER };
bool ui_button(const char *label, int kind, float u, const char *icon = nullptr, ImVec2 size = ImVec2(0, 0),
               bool enabled = true);
// A small rounded label: tone 0 grey, 1 green, 2 blue, 3 yellow, 4 red
enum { UI_TONE_GREY, UI_TONE_GREEN, UI_TONE_BLUE, UI_TONE_YELLOW, UI_TONE_RED };
void ui_pill(const char *text, int tone, float u);
// A card: a raised, bordered box (a child window that grows to its content) with padding
void ui_card_begin(const char *id, float u, float width = 0);
void ui_card_end(void);
// A page's title and its one-line explanation
void ui_page_title(const char *title, const char *subtitle, float u, const UiFonts &f);
// A sidebar item: an icon and a label, the selected one lit with the accent bar. True when chosen.
bool ui_nav_item(const char *icon, const char *label, bool selected, bool enabled, float u, float width);
// A sidebar group label (SETTINGS)
void ui_nav_group(const char *label, float u);
// A status line icon in a colour, then text (the status bar, a card's row)
void ui_icon_text(const char *icon, ImU32 icon_col, const char *text, ImU32 text_col);
// A thin progress bar f (0..1), w x 6u
void ui_progress(float f, float w, float u);

// ---- the sidebar (Configuration and the in-game menu)
// A page: group 0 at the top, 1 under the SETTINGS label, 2 at the bottom; on false: shown greyed, can't be picked
struct UiNavPage {
    int id;
    const char *name;  // English (shown translated)
    const char *icon;
    int group;
    bool on;
};
// The sidebar in a child window of that size (UI_BASE behind it). Returns the page clicked, or the one the controller's
// cursor moved onto (the page follows the cursor, as on a console); else cur. focus_page (or -999): put the
// controller's cursor on that page's item this frame. *focused: the cursor is on the sidebar; *activated: an item was
// clicked or pressed (A / Enter) this frame - the caller moves the cursor into its page.
int ui_sidebar(const UiNavPage *pages, int n, int cur, float u, ImVec2 size, bool *focused, int focus_page = -999,
               bool *activated = nullptr);
// The next / previous page that can be picked (L / R, Q / E), wrapping
int ui_sidebar_step(const UiNavPage *pages, int n, int cur, int dir);
