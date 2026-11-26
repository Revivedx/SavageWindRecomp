// Savage Wind's UI look (ui_theme.h): SWMM+'s colours, fonts and controls, for Configuration and the in-game menu.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "i18n.h"
#include "option_rows.h"  // (ui_texture_rgba)
#include "ui_theme.h"

extern "C" {
#include "imageload.h"
#include "paths.h"
}

static bool exists(const char *p) {
    if (!p || !*p) return false;
    FILE *f = fopen(p, "rb");  // (a missing file is an assertion in ImGui, not a NULL return)
    if (!f) return false;
    fclose(f);
    return true;
}

static ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

bool ui_theme_fonts(ImFontAtlas *atlas, UiFonts *out, const ImWchar *ranges, float unit) {
    *out = UiFonts();
    // the face: SWMM+'s (Segoe UI on Windows; Noto Sans, which it ships, elsewhere when the system has it), else the
    // Fira Sans Savage Wind ships
    char fira_med[1024] = "", fira_semi[1024] = "", fa[1024] = "";
    rt_resource("FiraSans-Medium.ttf", "third_party/fonts/FiraSans/FiraSans-Medium.ttf", fira_med, sizeof fira_med);
    rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", fira_semi, sizeof fira_semi);
    bool icons = rt_resource("fa-solid-900.ttf", "third_party/fonts/FontAwesome/fa-solid-900.ttf", fa, sizeof fa) &&
                 exists(fa);
    struct Face { const char *regular, *semibold, *bold; };
    const Face faces[] = {
        {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf"},
        {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
         "/usr/share/fonts/noto/NotoSans-Bold.ttf"},
        {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
         "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"},
        {"/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf",
         "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"},
        {fira_med, fira_semi, fira_semi},
    };
    const char *test = getenv("SAVAGEWIND_UI_FONT");  // (test) another face for all three
    const Face *face = nullptr;
    Face test_face = {test, test, test};
    if (exists(test)) face = &test_face;
    for (const Face &f : faces)
        if (!face && exists(f.regular)) face = &f;
    if (!face) return false;
    if (unit < 1) unit = 1;
    if (unit > 3) unit = 3;  // (bigger outputs scale the 3x fonts up: a CJK fallback at 4x would not fit the atlas)
    static const ImWchar icon_ranges[] = {0xe000, 0xf8ff, 0};
    auto add = [&](const char *path, float size) -> ImFont * {
        if (!exists(path)) path = face->regular;
        ImFontConfig c;
        c.OversampleH = 2;
        ImFont *f = atlas->AddFontFromFileTTF(path, size, &c, ranges);
        if (!f) return nullptr;
        if (icons) {  // Font Awesome, merged: each icon as wide as the text is tall (they line up in the sidebar)
            ImFontConfig m;
            m.MergeMode = true;
            m.PixelSnapH = true;
            m.GlyphMinAdvanceX = size * 1.15f;
            m.GlyphOffset = ImVec2(0, size * 0.02f);
            atlas->AddFontFromFileTTF(fa, size * 0.86f, &m, icon_ranges);
        }
        ui_merge_fallback_font(atlas, size, &c);  // (the menu language's glyphs the face lacks)
        return f;
    };
    out->body = add(face->regular, 14 * unit);
    out->semibold = add(face->semibold, 14 * unit);
    out->bold = add(face->bold, 20 * unit);
    if (!out->semibold) out->semibold = out->body;
    if (!out->bold) out->bold = out->semibold;
    out->unit = unit;
    return out->body != nullptr;
}

void ui_theme_style(ImGuiStyle &s, float u) {
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10, s.ChildRounding = 8, s.FrameRounding = 5, s.PopupRounding = 8, s.GrabRounding = 7;
    s.TabRounding = 5, s.ScrollbarRounding = 4;
    s.WindowPadding = ImVec2(18, 18), s.FramePadding = ImVec2(11, 5), s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4), s.CellPadding = ImVec2(8, 6);
    s.WindowBorderSize = 0, s.ChildBorderSize = 1, s.PopupBorderSize = 1, s.FrameBorderSize = 1;
    s.ScrollbarSize = 8, s.GrabMinSize = 14, s.IndentSpacing = 16, s.TabBarBorderSize = 1, s.SeparatorTextBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = col(UI_TEXT);
    c[ImGuiCol_TextDisabled] = col(UI_MUTED);
    c[ImGuiCol_WindowBg] = col(UI_PANEL);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = col(IM_COL32(0x16, 0x18, 0x1B, 255));
    c[ImGuiCol_Border] = col(UI_BORDER);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = col(UI_CONTROL);
    c[ImGuiCol_FrameBgHovered] = col(UI_CONTROL_HOT);
    c[ImGuiCol_FrameBgActive] = col(IM_COL32(0x18, 0x1A, 0x1D, 255));
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col(UI_BASE);
    c[ImGuiCol_MenuBarBg] = col(UI_BASE);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = col(UI_TRACK);
    c[ImGuiCol_ScrollbarGrabHovered] = col(IM_COL32(0x33, 0x37, 0x3E, 255));
    c[ImGuiCol_ScrollbarGrabActive] = col(IM_COL32(0x3C, 0x40, 0x48, 255));
    c[ImGuiCol_CheckMark] = col(UI_TEXT);
    c[ImGuiCol_SliderGrab] = col(UI_TEXT);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Button] = col(UI_CONTROL);
    c[ImGuiCol_ButtonHovered] = col(UI_CONTROL_HOT);
    c[ImGuiCol_ButtonActive] = col(IM_COL32(0x18, 0x1A, 0x1D, 255));
    c[ImGuiCol_Header] = col(UI_HOVER);
    c[ImGuiCol_HeaderHovered] = col(UI_ROW_HOVER);
    c[ImGuiCol_HeaderActive] = col(UI_TRACK);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = col(UI_BORDER);
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = col(UI_CONTROL);
    c[ImGuiCol_TabHovered] = col(UI_CONTROL_HOT);
    c[ImGuiCol_TabSelected] = col(UI_HOVER);
    c[ImGuiCol_TabSelectedOverline] = col(UI_ACCENT_LIGHT);
    c[ImGuiCol_TabDimmed] = col(UI_CONTROL);
    c[ImGuiCol_TabDimmedSelected] = col(UI_HOVER);
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotLines] = col(UI_ACCENT);
    c[ImGuiCol_TableHeaderBg] = col(UI_RAISED);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = col(UI_BORDER);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextLink] = col(UI_ACCENT_LIGHT);
    c[ImGuiCol_TextSelectedBg] = col(IM_COL32(0x00, 0x5A, 0x9E, 255));
    c[ImGuiCol_DragDropTarget] = col(UI_ACCENT_LIGHT);
    c[ImGuiCol_NavCursor] = col(UI_ACCENT_LIGHT);
    c[ImGuiCol_NavWindowingHighlight] = col(UI_ACCENT_LIGHT);
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    s.ScaleAllSizes(u);
}

float ui_unit(float w, float h, int text_pct) {
    float u = fminf(w / 1180.0f, h / 740.0f) * 1.25f;
    if (text_pct >= 50 && text_pct <= 200) u *= text_pct / 100.0f;
    return u < 0.6f ? 0.6f : u;
}

// ---- the wordmark pictures: one texture per picture and ImGui context (each context has its own renderer)
struct ImageSlot {
    ImGuiContext *ctx = nullptr;
    std::string file;
    UiImage img;
};
static ImageSlot g_images[8];

UiImage ui_image(const char *file) {
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    for (ImageSlot &s : g_images)
        if (s.ctx == ctx && s.file == file) return s.img;
    ImageSlot *slot = nullptr;
    for (ImageSlot &s : g_images)
        if (!s.ctx && !slot) slot = &s;
    if (!slot) return UiImage();
    slot->ctx = ctx, slot->file = file;
    char path[1024], dev[256];
    snprintf(dev, sizeof dev, "docs/media/%s", file);  // (i18n-skip: a path)
    int w, h;
    if (rt_resource(file, dev, path, sizeof path) && exists(path)) {
        if (unsigned char *px = rt_image_rgba(path, &w, &h)) {
            slot->img.tex = (ImTextureID)ui_texture_rgba(px, w, h);
            if (slot->img.tex) slot->img.w = w, slot->img.h = h;
            rt_image_free(px);
        }
    }
    if (!slot->img.tex) fprintf(stderr, "[ui] no %s: drawn in text instead\n", file);
    return slot->img;
}

UiImage ui_wordmark(void) { return ui_image("wordmark.png"); }

void ui_wordmark_free(void) {
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    for (ImageSlot &s : g_images) {
        if (s.ctx != ctx) continue;
        if (s.img.tex) ui_texture_free((unsigned long long)s.img.tex);
        s = ImageSlot();
    }
}

void ui_draw_subtitle(ImDrawList *dl, ImVec2 wordmark_at, float wordmark_h, const char *file, const char *text, ImU32 col) {
    UiImage wm = ui_wordmark(), sub = file && *file ? ui_image(file) : UiImage();
    float x = wordmark_at.x + (wm.tex ? wordmark_h * wm.w / wm.h : 120 * wordmark_h / 20) + wordmark_h * 0.45f;
    if (!wm.tex || !sub.tex) {  // (no pictures: the text in the body font, centred on the wordmark)
        float fs = ImGui::GetFontSize() * 13 / 14;
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x, wordmark_at.y + (wordmark_h - fs) * 0.5f), col, text);
        return;
    }
    // the same lettering at 0.7 of the wordmark's size, centred on its capitals (both pictures are cropped at the
    // capitals' top; the wordmark's foot is its W's tail, ~2 % below the baseline). (On the baseline it sat low: user)
    float k = wordmark_h / wm.h, h = sub.h * k * 0.7f, mid = wordmark_at.y + wordmark_h * 0.975f * 0.5f;
    float w = h * sub.w / sub.h;
    dl->AddImage(sub.tex, ImVec2(x, mid - h * 0.5f), ImVec2(x + w, mid + h * 0.5f), ImVec2(0, 0), ImVec2(1, 1), col);
}

void ui_draw_wordmark(ImDrawList *dl, ImVec2 p, float h, bool shadow) {
    UiImage wm = ui_wordmark();
    if (!wm.tex) {  // (no picture: the name in the bold font)
        ImFont *f = ImGui::GetFont();
        if (shadow) dl->AddText(f, h, ImVec2(p.x + h * 0.04f, p.y + h * 0.06f), IM_COL32(0, 10, 20, 150), "Savage Wind");
        dl->AddText(f, h, p, UI_TEXT, "Savage Wind");
        return;
    }
    float w = h * wm.w / wm.h;
    if (shadow)  // a soft shadow: the picture in black, a few times, a little apart
        for (int i = 0; i < 6; i++) {
            float a = (float)i / 6 * 6.2831853f, r = h * 0.05f;
            ImVec2 o(p.x + cosf(a) * r + h * 0.03f, p.y + sinf(a) * r + h * 0.07f);
            dl->AddImage(wm.tex, o, ImVec2(o.x + w, o.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 8, 18, 46));
        }
    dl->AddImage(wm.tex, p, ImVec2(p.x + w, p.y + h));
}

// ---- widgets
bool ui_button(const char *label, int kind, float u, const char *icon, ImVec2 size, bool enabled) {
    ImGuiStyle &st = ImGui::GetStyle();
    std::string text = icon ? std::string(icon) + "  " + label : std::string(label);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, true);
    float h = size.y > 0 ? size.y : 30 * u, w = size.x > 0 ? size.x : ts.x + 2 * (kind == UI_BTN_PRIMARY ? 16 : 12) * u;
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##btn", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImU32 bg, border, fg;
    if (kind == UI_BTN_PRIMARY) bg = hov ? UI_ACCENT_HOT : UI_ACCENT, border = 0, fg = IM_COL32_WHITE;
    else if (kind == UI_BTN_DANGER)
        bg = hov ? IM_COL32(0x2A, 0x1A, 0x1D, 255) : 0, border = IM_COL32(0x5C, 0x2C, 0x33, 255), fg = UI_RED;
    else bg = held ? IM_COL32(0x18, 0x1A, 0x1D, 255) : hov ? UI_CONTROL_HOT : UI_CONTROL, border = UI_BORDER, fg = UI_TEXT;
    if (!enabled) fg = UI_MUTED, bg = kind == UI_BTN_PRIMARY ? IM_COL32(0x1C, 0x3A, 0x55, 255) : bg;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = st.FrameRounding;
    if (bg) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    if (border) dl->AddRect(p, ImVec2(p.x + w, p.y + h), border, r, 0, 1.0f);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fg, text.c_str());
    ImGui::EndDisabled();
    ImGui::PopID();
    return pressed && enabled;
}

void ui_pill(const char *text, int tone, float u) {
    static const ImU32 bg[] = {IM_COL32(0x23, 0x26, 0x2B, 255), IM_COL32(0x1B, 0x2E, 0x1D, 255),
                               IM_COL32(0x0F, 0x2A, 0x3D, 255), IM_COL32(0x2B, 0x26, 0x10, 255),
                               IM_COL32(0x33, 0x1B, 0x1F, 255)};
    static const ImU32 fg[] = {UI_TEXT_DIM, UI_GREEN, UI_ACCENT_LIGHT, IM_COL32(0xF3, 0xDD, 0x6B, 255), UI_RED};
    if (tone < 0 || tone > 4) tone = 0;
    float fs = ImGui::GetFontSize() * 12 / 14;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, text);
    float px = 9 * u, py = 2 * u, h = ts.y + 2 * py;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float line = ImGui::GetTextLineHeight();
    p.y += (line - h) * 0.5f;  // (centred on the line it sits on)
    ImGui::Dummy(ImVec2(ts.x + 2 * px, line));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + 2 * px, p.y + h), bg[tone], h * 0.5f);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + px, p.y + py), fg[tone], text);
}

void ui_card_begin(const char *id, float u, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(UI_RAISED));
    ImGui::PushStyleColor(ImGuiCol_Border, col(UI_BORDER));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * u);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * u, 14 * u));
    ImGui::BeginChild(id, ImVec2(width, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void ui_card_end(void) { ImGui::EndChild(); }

void ui_page_title(const char *title, const char *subtitle, float u, const UiFonts &f) {
    if (f.bold) ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(title);
    if (f.bold) ImGui::PopFont();
    if (subtitle && *subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(UI_MUTED));
        ImGui::SetWindowFontScale(13.0f / 14);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6 * u));
}

bool ui_nav_item(const char *icon, const char *label, bool selected, bool enabled, float u, float width) {
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 36 * u;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the item's own highlight shows the selection)
    bool pressed = ImGui::InvisibleButton("##nav", ImVec2(width, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 5 * u;
    if (selected) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), UI_HOVER, r);
    else if (hov || foc) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), UI_NAV_HOVER, r);
    if (foc) dl->AddRect(p, ImVec2(p.x + width, p.y + h), UI_ACCENT_LIGHT, r, 0, fmaxf(1.0f, 1.5f * u));
    if (selected)
        dl->AddRectFilled(ImVec2(p.x, p.y + 10 * u), ImVec2(p.x + 3 * u, p.y + h - 10 * u), UI_ACCENT_LIGHT, 2 * u);
    ImU32 fg = !enabled ? UI_FAINT : selected ? UI_TEXT : hov || foc ? UI_TEXT : UI_TEXT_DIM;
    float fs = ImGui::GetFontSize(), ty = p.y + (h - fs) * 0.5f;
    if (icon) dl->AddText(ImVec2(p.x + 12 * u, ty), selected && enabled ? UI_ACCENT_LIGHT : fg, icon);
    dl->AddText(ImVec2(p.x + 12 * u + (icon ? fs * 1.15f + 10 * u : 0), ty), fg, label);
    ImGui::EndDisabled();
    ImGui::PopID();
    return pressed && enabled;
}

void ui_nav_group(const char *label, float u) {
    ImGui::Dummy(ImVec2(0, 8 * u));
    std::string t = tr_upper(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize() * 11 / 14;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fs, ImVec2(p.x + 12 * u, p.y), UI_MUTED, t.c_str());
    ImGui::Dummy(ImVec2(0, fs + 6 * u));
}

void ui_icon_text(const char *icon, ImU32 icon_col, const char *text, ImU32 text_col) {
    if (icon) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(icon_col));
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col(text_col));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

int ui_sidebar(const UiNavPage *pages, int n, int cur, float u, ImVec2 size, bool *focused, int focus_page,
               bool *activated) {
    if (activated) *activated = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(UI_BASE));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * u, 8 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild("##sidebar", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * u));
    float w = ImGui::GetContentRegionAvail().x;
    int pick = cur, bottom = 0;
    bool any_focus = false, settings_label = false;
    auto item = [&](const UiNavPage &p) {
        if (p.group == 1 && !settings_label) ui_nav_group(tr("Settings"), u), settings_label = true;
        if (ui_nav_item(p.icon, tr(p.name), p.id == cur, p.on, u, w)) {
            pick = p.id;
            if (activated) *activated = true;
        }
        if (focus_page == p.id) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        if (ImGui::IsItemFocused() && GImGui->NavCursorVisible) {
            any_focus = true;
            if (p.on && p.id != cur && pick == cur) pick = p.id;  // (the page follows the controller's cursor)
        }
    };
    for (int i = 0; i < n; i++)
        if (pages[i].group == 2) bottom++;
        else item(pages[i]);
    if (bottom) {  // the bottom group sits at the foot of the sidebar
        float y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - bottom * (36 * u + 2 * u);
        if (y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(y);
        for (int i = 0; i < n; i++)
            if (pages[i].group == 2) item(pages[i]);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    if (focused) *focused = any_focus;
    return pick;
}

int ui_sidebar_step(const UiNavPage *pages, int n, int cur, int dir) {
    int at = 0;
    for (int i = 0; i < n; i++)
        if (pages[i].id == cur) at = i;
    for (int k = 1; k <= n; k++) {
        int i = ((at + dir * k) % n + n) % n;
        if (pages[i].on) return pages[i].id;
    }
    return cur;
}

void ui_progress(float f, float w, float u) {
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 6 * u, line = ImGui::GetTextLineHeight();
    p.y += (line - h) * 0.5f;
    ImGui::Dummy(ImVec2(w, line));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), UI_TRACK, h * 0.5f);
    if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + h), UI_ACCENT, h * 0.5f);
}
