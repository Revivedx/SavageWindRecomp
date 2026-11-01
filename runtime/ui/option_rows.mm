// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

#include "i18n.h"
#include "option_rows.h"
#include "osk.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)
struct DescImage { unsigned long long tex = 0; int w = 0, h = 0; };
static DescImage g_img_hover, g_img_focus, g_img_shown;  // (the pictures of those rows: opt_desc_image)
static std::vector<unsigned long long> g_img_live;       // the pictures offered this frame

static const ImU32 kGold = IM_COL32(236, 194, 84, 255);
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static bool g_value_dim;  // the next row's value is shown greyed (a value that can't be picked yet)
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    static std::string page;  // (another page: its first row's description, not the last page's)
    if (page != id) page = id, g_desc_shown.clear(), g_img_shown = DescImage();
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    g_img_hover = g_img_focus = DescImage(), g_img_live.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.48f, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * scale));  // (rows close)
    g_desc_first.clear();
}

// (labels, values, descriptions and titles are English: shown in the menu language, tr(); IDs stay English)
void opt_section(const char *title) {
    std::string t = tr_upper(tr(title));
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.52f, 1), "%s", t.c_str());
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool upper = true,
               bool arrows = false) {
    ImGui::PushID(label);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool dim = off || g_value_dim;
    g_value_dim = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (desc) desc = tr(desc);
    if (foc) g_desc_focus = desc ? desc : "";
    if (hov) g_desc_hover = desc ? desc : "";
    if (g_desc_first.empty() && desc) g_desc_first = desc;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 9 * g_scale, pad = 16 * g_scale, fs = ImGui::GetFontSize();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), hov || foc ? (off ? IM_COL32(236, 194, 84, 120) : kGold)
                                                        : IM_COL32(255, 255, 255, off ? 18 : 38), r, 0,
                hov || foc ? 2.0f : 1.0f);
    std::string t = upper ? tr_upper(tr(label)) : std::string(tr(label));
    float ty = p.y + (h - fs) * 0.5f;
    ImU32 label_col = off ? IM_COL32(110, 110, 102, 255) : IM_COL32(240, 236, 224, 255);  // (greyed out: clearly dim)
    dl->AddText(ImVec2(p.x + pad, ty), label_col, t.c_str());
    float right = p.x + w - pad;  // (the value's right edge)
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        ImU32 ac = off ? IM_COL32(110, 110, 100, 255) : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x, hit = gap;
            if (mx >= lx - hit && mx <= lx + aw + hit) step = -1;
            else if (mx >= rx - hit && mx <= rx + aw + hit) step = 1;
        }
        right = rx - gap;
    }
    if (!value.empty()) {
        float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;
        dl->AddText(ImVec2(vx, ty), dim ? IM_COL32(110, 110, 102, 255) : changed ? IM_COL32(255, 255, 255, 255)
                                                                                 : IM_COL32(205, 205, 195, 255), value.c_str());
    }
    ImGui::PopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (!row(label, tr(*v ? "On" : "Off"), *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, tr(names[cur]), cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_list(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool *picked) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, tr(names[cur]), cur != def, desc, true, true);
    *picked = false;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::PushID(label);
    static bool test_open;  // (test: SAVAGEWIND_OPT_LIST=<label> opens that list once)
    const char *t = getenv("SAVAGEWIND_OPT_LIST");
    if (step == 2 || (t && !test_open && !strcmp(t, label) && (test_open = true))) ImGui::OpenPopup("##list");
    int nv = step == 1 || step == -1 ? (cur + step + n) % n : cur;
    // the list below the row (above it when there's no room), the current value selected and in view
    float rows_h = ImGui::GetFrameHeightWithSpacing() * 12;
    bool up = b.y + rows_h > ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y;
    ImGui::SetNextWindowPos(up ? ImVec2(a.x, a.y) : ImVec2(a.x, b.y), ImGuiCond_Appearing, ImVec2(0, up ? 1.0f : 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(b.x - a.x, 0), ImVec2(b.x - a.x, rows_h));
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    bg.w = 1;  // (opaque: the rows below don't show through)
    ImGui::PushStyleColor(ImGuiCol_PopupBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kGold));
    bool open = ImGui::BeginPopup("##list");
    ImGui::PopStyleColor(2);
    if (open) {
        bool appearing = ImGui::IsWindowAppearing();
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tr(names[i]), i == cur)) nv = i, *picked = true;
            if (i == cur && appearing) ImGui::SetItemDefaultFocus(), ImGui::SetScrollHereY(0.5f);
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_choice_avail(const char *label, int *v, int *shown, const char *const *names, const bool *avail, int n, int def,
                      const char *desc) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    if (*shown < 0 || *shown >= n || (avail[*shown] && *shown != cur)) *shown = cur;  // (changed elsewhere)
    int s = *shown;
    g_value_dim = !avail[s];
    int step = row(label, tr(names[s]), s != def, desc, true, true);
    if (!step) return false;
    s = (s + (step == 2 ? 1 : step) + n) % n;
    *shown = s;
    if (!avail[s] || s == cur) return false;
    *v = s;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, tr(fmt), *v);
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", tr(lo_name));
    else snprintf(buf, sizeof buf, tr(fmt), *v);
    int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

void opt_desc_image(unsigned long long tex, int w, int h) {
    g_img_live.push_back(tex);
    if (ImGui::IsItemHovered()) g_img_hover = {tex, w, h};
    if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible) g_img_focus = {tex, w, h};
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = off ? IM_COL32(150, 150, 140, 255) : selected ? kGold : IM_COL32(205, 205, 195, 255);
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.0f);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, col);
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id) {
        if (row(label, buf[0] ? buf : (hint ? tr(hint) : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? tr(desc) : "";
    if (done || ui_input_text_cancelled() || (focus == 0 && !ImGui::IsItemActive() && !osk_is_open())) editing = 0;
    ImGui::PopID();
    return done;
}

void opt_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("SAVAGEWIND_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (!g_desc_hover.empty()) g_desc_shown = g_desc_hover, g_img_shown = g_img_hover;
    else if (!g_desc_focus.empty()) g_desc_shown = g_desc_focus, g_img_shown = g_img_focus;
    else if (g_desc_shown.empty()) g_desc_shown = g_desc_first, g_img_shown = DescImage();
    bool live = false;  // (a picture whose row is gone may have been freed)
    for (unsigned long long t : g_img_live) live |= t == g_img_shown.tex;
    if (!live) g_img_shown = DescImage();
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 24 * g_scale;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        IM_COL32(236, 194, 84, 110), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (g_img_shown.tex && g_img_shown.w > 0) {  // the picture of the selected row, above its text
        ImVec2 at = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = w * g_img_shown.h / g_img_shown.w;
        ImGui::Dummy(ImVec2(w, h + 8 * g_scale));
        ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)g_img_shown.tex, at, ImVec2(at.x + w, at.y + h), ImVec2(0, 0),
                                                    ImVec2(1, 1), IM_COL32_WHITE, 8 * g_scale);
    }
    ImGui::SetWindowFontScale(1.3f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
    ImGui::PopID();
}
