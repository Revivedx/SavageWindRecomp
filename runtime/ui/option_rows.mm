// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
// In SWMM+'s look (ui_theme.h): the rows in one card with lines between them, each label as written, its value in a
// control on the right (a box with arrows, a drop-down box, a switch); the description in a card of its own.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

#include "i18n.h"
#include "option_rows.h"
#include "osk.h"
#include "ui_theme.h"

static float g_scale = 1;
static float U(float v) { return v * g_scale * 2; }  // (design units: the callers' scale is 1 at 1080 p, u is 2 there)
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)
struct DescImage { unsigned long long tex = 0; int w = 0, h = 0; };
static DescImage g_img_hover, g_img_focus, g_img_shown;  // (the pictures of those rows: opt_desc_image)
static std::vector<unsigned long long> g_img_live;       // the pictures offered this frame

static const ImU32 kGold = UI_ACCENT_LIGHT;  // (the selection's colour)
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static bool g_value_dim;  // the next row's value is shown greyed (a value that can't be picked yet)
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
static bool g_restore_req, g_take_restore;  // (opt_restore_focus: restore the last focused row on this page)
static ImGuiID g_restore_id;
static std::string g_cur_page;
static std::unordered_map<std::string, ImGuiID> g_saved_focus;
void opt_focus_first(bool on) { g_focus_req = on; }
void opt_restore_focus(bool on) { g_restore_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    static std::string page;  // (another page: its first row's description, not the last page's)
    if (page != id) page = id, g_desc_shown.clear(), g_img_shown = DescImage();
    g_cur_page = id;
    g_take_restore = false;
    g_restore_id = 0;
    if (g_restore_req) {
        g_restore_req = false;
        auto it = g_saved_focus.find(g_cur_page);
        if (it != g_saved_focus.end() && it->second != 0) {
            g_take_restore = true;
            g_restore_id = it->second;
            g_focus_req = false;
            ImGui::NavMoveRequestCancel();
        }
    }
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    g_img_hover = g_img_focus = DescImage(), g_img_live.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    // the rows: one card (raised, bordered), rows edge to edge inside it with lines between them
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(UI_RAISED));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(UI_BORDER));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, U(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(6), U(6)));
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.56f, avail.y), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));  // (rows touch: lines part them)
    g_desc_first.clear();
}

// (labels, values, descriptions and titles are English: shown in the menu language, tr(); IDs stay English)
// A section: its name small, in capitals and muted, as the sidebar's groups
void opt_section(const char *title) {
    std::string t = tr_upper(tr(title));
    bool first = ImGui::GetCursorPosY() <= ImGui::GetStyle().WindowPadding.y + 1;
    ImGui::Dummy(ImVec2(0, first ? U(4) : U(14)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize() * 11 / 14;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fs, ImVec2(p.x + U(12), p.y), UI_MUTED, t.c_str());
    ImGui::Dummy(ImVec2(0, fs + U(6)));
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// arrows: ◀ value ▶, left / right step through the values; drop: the value in a box with ▼, a list opens on a click
// (left / right do nothing)
// A chevron, drawn (the same on every font): dir -1 left, 1 right, 2 down; a the half height
static void chevron(ImDrawList *dl, float x, float cy, int dir, float a, ImU32 c) {
    ImVec2 pts[3];
    if (dir == 2) pts[0] = ImVec2(x - a, cy - a * 0.5f), pts[1] = ImVec2(x, cy + a * 0.5f), pts[2] = ImVec2(x + a, cy - a * 0.5f);
    else if (dir == -2) pts[0] = ImVec2(x - a, cy + a * 0.5f), pts[1] = ImVec2(x, cy - a * 0.5f), pts[2] = ImVec2(x + a, cy + a * 0.5f);
    else pts[0] = ImVec2(x - dir * a * 0.3f, cy - a), pts[1] = ImVec2(x + dir * a * 0.3f, cy), pts[2] = ImVec2(x - dir * a * 0.3f, cy + a);
    dl->AddPolyline(pts, 3, c, 0, fmaxf(1.2f, U(1.6f)));
}

// toggle: -1 not a switch, else the switch drawn on (1) or off (0) in place of the value
// order: -1 none, else up (bit 0) and down (bit 1) arrows at the right end; a move lands in g_order_move (-1 / +1)
static int g_order_move;
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool upper = true,
               bool arrows = false, bool drop = false, int toggle = -1, int order = -1) {
    (void)upper;  // (labels are shown as written, SWMM+'s way)
    ImGui::PushID(label);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * U(14));  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 2.6f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    ImGuiID item_id = ImGui::GetItemID();
    if (g_take_restore && item_id == g_restore_id) {  // (opt_restore_focus)
        g_take_restore = false;
        g_restore_id = 0;
        ImGui::SetFocusID(item_id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
    } else if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        ImGui::SetFocusID(item_id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool dim = off || g_value_dim;
    g_value_dim = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    if (foc && !g_cur_page.empty()) g_saved_focus[g_cur_page] = item_id;
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
    float r = U(5), pad = U(12), fs = ImGui::GetFontSize();
    ImVec2 q(p.x + w, p.y + h);
    if (hov || foc) dl->AddRectFilled(p, q, UI_ROW_HOVER, r);  // (clearly lit: the mouse or the cursor is on it)
    else dl->AddLine(ImVec2(p.x + pad, q.y - 0.5f), ImVec2(q.x - pad, q.y - 0.5f), UI_BORDER, 1.0f);  // (the line under it)
    if (foc) dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.28f), ImVec2(p.x + U(3), q.y - h * 0.28f), kGold, U(2));
    std::string t = tr(label);
    float ty = p.y + (h - fs) * 0.5f, cy = p.y + h * 0.5f;
    dl->AddText(ImVec2(p.x + pad + U(4), ty), off ? UI_FAINT : UI_TEXT, t.c_str());  // (greyed out: clearly dim)
    float right = q.x - pad;  // (the value's right edge)
    ImU32 vcol = dim ? UI_FAINT : changed ? UI_TEXT : UI_TEXT_DIM;
    float bh = fminf(h - U(12), U(30));  // (a control box's height)
    if (order >= 0) {  // [▲][▼] at the right end: higher or lower in the order
        float bw = U(26), gap = U(4), a = fs * 0.24f;
        for (int k = 1; k >= 0; k--) {  // (down at the end, up before it)
            bool can = order >> k & 1;
            float bx1 = right, bx0 = right - bw;
            ImVec2 b0(bx0, cy - bh * 0.5f), b1(bx1, cy + bh * 0.5f);
            bool in = ImGui::IsMouseHoveringRect(b0, b1);
            dl->AddRectFilled(b0, b1, can && in ? UI_CONTROL_HOT : UI_CONTROL, r);
            dl->AddRect(b0, b1, UI_BORDER, r, 0, 1.0f);
            chevron(dl, (bx0 + bx1) * 0.5f, cy, k ? 2 : -2, a, !can || off ? UI_FAINT : in ? UI_TEXT : UI_MUTED);
            if (clicked && in) {  // (a click on an arrow moves the row; it doesn't flip the switch)
                step = 0;
                if (can && !off) g_order_move = k ? 1 : -1;
            }
            right = bx0 - gap;
        }
        right -= U(8);
        if (foc && !off) {  // LB / RB on a controller, Page Up / Page Down on a keyboard
            if ((order & 1) && (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_PageUp))) g_order_move = -1;
            if ((order & 2) && (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_PageDown))) g_order_move = 1;
            for (ImGuiKey k : {ImGuiKey_GamepadL1, ImGuiKey_GamepadR1, ImGuiKey_PageUp, ImGuiKey_PageDown})
                ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
    }
    if (toggle >= 0) {  // a switch, as SWMM+'s: the accent with the knob right when on, grey with it left when off
        float tw = U(38), th = U(20), x0 = right - tw;
        ImU32 track = toggle ? (off ? IM_COL32(0x1C, 0x3A, 0x55, 255) : UI_ACCENT) : UI_TRACK;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(right, cy + th * 0.5f), track, th * 0.5f);
        float kr = th * 0.5f - U(3), kx = toggle ? right - th * 0.5f : x0 + th * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, cy), kr, toggle ? (off ? UI_TEXT_DIM : IM_COL32_WHITE) : (off ? UI_FAINT : UI_TEXT_DIM));
        right = x0 - U(10);
        if (!value.empty()) {  // (the switch's own word beside it: On / Off, translated)
            float vw = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(right - vw, ty), vcol, value.c_str());
        }
    } else if (arrows && !value.empty()) {  // [ ‹ value › ] in a control box; a click on an end steps
        float vw = ImGui::CalcTextSize(value.c_str()).x, a = fs * 0.28f, side = U(26);
        float bw = fmaxf(vw + 2 * side + U(8), U(170)), bx0 = right - bw;
        ImU32 ac = off ? UI_FAINT : hov || foc ? UI_TEXT : UI_MUTED;
        dl->AddRectFilled(ImVec2(bx0, cy - bh * 0.5f), ImVec2(right, cy + bh * 0.5f), hov || foc ? UI_CONTROL_HOT : UI_CONTROL, r);
        dl->AddRect(ImVec2(bx0, cy - bh * 0.5f), ImVec2(right, cy + bh * 0.5f), foc ? kGold : UI_BORDER, r, 0, 1.0f);
        chevron(dl, bx0 + side * 0.55f, cy, -1, a, ac);
        chevron(dl, right - side * 0.55f, cy, 1, a, ac);
        dl->AddText(ImVec2(bx0 + (bw - vw) * 0.5f, ty), vcol, value.c_str());
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x;
            if (mx >= bx0 && mx <= bx0 + side) step = -1;
            else if (mx >= right - side && mx <= right) step = 1;
        }
        right = bx0;
    } else if (drop && !value.empty()) {  // [ value  ˅ ] in a control box; a click opens the list
        float vw = ImGui::CalcTextSize(value.c_str()).x, a = fs * 0.28f;
        float bw = fmaxf(vw + U(44), U(190)), bx0 = right - bw;
        ImU32 ac = off ? UI_FAINT : hov || foc ? UI_TEXT : UI_MUTED;
        dl->AddRectFilled(ImVec2(bx0, cy - bh * 0.5f), ImVec2(right, cy + bh * 0.5f), hov || foc ? UI_CONTROL_HOT : UI_CONTROL, r);
        dl->AddRect(ImVec2(bx0, cy - bh * 0.5f), ImVec2(right, cy + bh * 0.5f), foc ? kGold : UI_BORDER, r, 0, 1.0f);
        dl->AddText(ImVec2(bx0 + U(12), ty), vcol, value.c_str());
        chevron(dl, right - U(16), cy, 2, a, ac);
        right = bx0;
        if (step == 1 || step == -1) step = 0;
    } else if (!value.empty()) {
        float vw = ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(right - vw, ty), vcol, value.c_str());
    }
    ImGui::PopID();
    return step;
}

static bool toggle_row(const char *label, bool *v, const char *value, bool changed, const char *desc, bool upper,
                       bool switch_shape) {
    if (!row(label, value, changed, desc, upper, false, false, switch_shape ? (*v ? 1 : 0) : -1))
        return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    return toggle_row(label, v, "", *v != def, desc, true, true);
}

bool opt_toggle_item(const char *label, bool *v, const char *value_override, const char *desc) {
    return toggle_row(label, v, value_override ? value_override : "", *v, desc, false, !value_override);
}

int opt_order_item(const char *label, bool *v, const char *value_override, bool can_up, bool can_down, const char *desc) {
    g_order_move = 0;
    int step = row(label, value_override ? value_override : "", *v, desc, false, false, false,
                   value_override ? -1 : (*v ? 1 : 0), (can_up ? 1 : 0) | (can_down ? 2 : 0));
    if (g_order_move) return g_order_move < 0 ? OPT_ORDER_UP : OPT_ORDER_DOWN;
    if (!step) return 0;
    *v = !*v;
    return OPT_ORDER_TOGGLED;
}

static bool choice_row(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap,
                       bool upper, bool translate_names) {
    if (n <= 0) return false;
    int cur = *v >= 0 && *v < n ? *v : 0;
    const char *raw = names[cur] ? names[cur] : "";
    int step = row(label, translate_names ? tr(raw) : raw, cur != def, desc, upper, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    return choice_row(label, v, names, n, def, desc, wrap, true, true);
}

bool opt_choice_item(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap,
                     bool upper) {
    return choice_row(label, v, names, n, def, desc, wrap, upper, false);
}

static bool list_row(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool *picked,
                     bool drop) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, tr(names[cur]), cur != def, desc, true, !drop, drop);
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

bool opt_list(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool *picked) {
    return list_row(label, v, names, n, def, desc, picked, false);
}

bool opt_dropdown(const char *label, int *v, const char *const *names, int n, int def, const char *desc) {
    bool picked;
    return list_row(label, v, names, n, def, desc, &picked, true);
}

bool opt_choice_avail(const char *label, int *v, int *shown, const char *const *names, const bool *avail, int n, int def,
                      const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    if (*shown < 0 || *shown >= n || (avail[*shown] && *shown != cur)) *shown = cur;  // (changed elsewhere)
    int s = *shown;
    g_value_dim = !avail[s];
    int step = row(label, tr(names[s]), s != def, desc, true, true);
    if (!step) return false;
    s += step == 2 ? 1 : step;
    s = wrap ? (s + n) % n : s < 0 ? 0 : s >= n ? n - 1 : s;
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
    float r = ImGui::GetFontSize() * 0.36f, cx = b.x - U(12) - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (selected) {  // (SWMM+'s radio: the accent ring with a white dot)
        dl->AddCircleFilled(ImVec2(cx, cy), r, off ? IM_COL32(0x1C, 0x3A, 0x55, 255) : UI_ACCENT);
        dl->AddCircleFilled(ImVec2(cx, cy), r * 0.42f, off ? UI_TEXT_DIM : IM_COL32_WHITE);
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), r, UI_CONTROL);
        dl->AddCircle(ImVec2(cx, cy), r, off ? UI_FAINT : UI_MUTED, 0, fmaxf(1.0f, U(1.2f)));
    }
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
    float gap = U(14);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    // the description: a card of its own beside the rows, its text in the muted body colour
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(UI_RAISED));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(UI_BORDER));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(UI_TEXT_DIM));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, U(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(16), U(14)));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(UI_TEXT_DIM));
    if (g_img_shown.tex && g_img_shown.w > 0) {  // the picture of the selected row, above its text
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImVec2 room = ImGui::GetContentRegionAvail();
        float w = room.x, h = w * g_img_shown.h / g_img_shown.w, most = room.y * 0.45f;  // (the text below has to fit)
        if (h > most) w = w * most / h, h = most, at.x += (room.x - w) * 0.5f;
        ImGui::Dummy(ImVec2(room.x, h + 8 * g_scale));
        ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)g_img_shown.tex, at, ImVec2(at.x + w, at.y + h), ImVec2(0, 0),
                                                    ImVec2(1, 1), IM_COL32_WHITE, 8 * g_scale);
    }
    ImGui::SetWindowFontScale(1.1f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopID();
}
