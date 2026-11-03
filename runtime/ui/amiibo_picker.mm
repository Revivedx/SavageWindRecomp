// The guest thread owns live settings and scan configuration; the render thread edits a snapshot.
#include "amiibo_picker.h"
#include "i18n.h"
#include "osk.h"
#include "../wolf_link_dump.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
extern "C" {
#include "../settings.h"
void input_reset(void);
}

namespace {
std::atomic<bool> active{false};
std::mutex lock;
AmiiboPickerResult draft, finished;
bool pending, appearing;
bool focus_results_requested;
bool scan_picker;
uint64_t scan_generation;
int current, preview, tab, series, frames;
int recent[AMIIBO_RECENT_MAX], nrecent;
char query[96];
ImFont *picker_font, *picker_bold, *picker_edge;
AmiiboPickerControls controls;
bool use_own_wolf;
WolfLinkStatus wolf_status;

static void game_cell(ImDrawList *draw, ImVec2 start, ImVec2 end, bool selected, float unit) {
    if (!selected) { draw->AddRectFilled(start, end, IM_COL32(28, 30, 29, 215), 3 * unit); return; }
    ImU32 left = IM_COL32(118, 117, 104, 240), right = IM_COL32(72, 71, 62, 240);
    draw->AddRectFilledMultiColor(start, end, left, right, right, left);
}
static void game_box(ImDrawList *draw, ImVec2 start, ImVec2 end, float unit) {
    draw->AddRectFilled(start, end, IM_COL32(9, 11, 10, 240), 2 * unit);
    draw->AddRect(start, end, IM_COL32(62, 64, 58, 255), 2 * unit, 0, 1.2f * unit);
}
static void game_cursor(ImDrawList *draw, ImVec2 start, ImVec2 end, float unit) {
    for (int layer = 3; layer >= 1; layer--) {
        float glow = layer * 1.6f * unit;
        draw->AddRect(ImVec2(start.x - glow, start.y - glow), ImVec2(end.x + glow, end.y + glow),
                      IM_COL32(255, 248, 205, 30 * (4 - layer)), 3 * unit + glow, 0, 1.6f * unit);
    }
    ImU32 color = IM_COL32(255, 252, 228, 255);
    float corner = 7 * unit;
    draw->AddRect(start, end, color, 2 * unit, 0, 1.6f * unit);
    draw->AddTriangleFilled(start, ImVec2(start.x + corner, start.y), ImVec2(start.x, start.y + corner), color);
    draw->AddTriangleFilled(ImVec2(end.x, start.y), ImVec2(end.x, start.y + corner), ImVec2(end.x - corner, start.y), color);
    draw->AddTriangleFilled(end, ImVec2(end.x - corner, end.y), ImVec2(end.x, end.y - corner), color);
    draw->AddTriangleFilled(ImVec2(start.x, end.y), ImVec2(start.x, end.y - corner), ImVec2(start.x + corner, end.y), color);
}

static void game_text(ImDrawList *draw, ImVec2 position, float size, ImU32 color, const char *text, const ImVec4 *clip = nullptr, bool bold = false, float wrap = 0) {
    int first_vertex = draw->VtxBuffer.Size;
    ImFont *font = bold && picker_bold ? picker_bold : ImGui::GetFont();
    if (font == picker_font && picker_edge)
        draw->AddText(picker_edge, size, position, IM_COL32(16, 16, 12, 160), text, nullptr, wrap, clip);
    draw->AddText(font, size, position, color, text, nullptr, wrap, clip);
    float baseline = position.y + size * 0.78f;
    for (int index = first_vertex; index < draw->VtxBuffer.Size; index++)
        draw->VtxBuffer[index].pos.x += (baseline - draw->VtxBuffer[index].pos.y) * 0.2f;
}

static void favorite_star(ImDrawList *draw, ImVec2 center, float radius, bool favorite, bool hovered) {
    ImVec2 points[10];
    for (int point = 0; point < 10; point++) {
        float angle = point * 3.14159265f / 5 - 3.14159265f * .5f;
        float length = point % 2 ? radius * .45f : radius;
        points[point] = ImVec2(center.x + cosf(angle) * length, center.y + sinf(angle) * length);
    }
    ImU32 color = favorite ? IM_COL32(236, 194, 84, 255) : hovered ? IM_COL32(236, 232, 208, 255) : IM_COL32(122, 126, 119, 255);
    if (favorite)
        for (int point = 0; point < 10; point++) draw->AddTriangleFilled(center, points[point], points[(point + 1) % 10], color);
    draw->AddPolyline(points, 10, color, ImDrawFlags_Closed, std::max(1.f, radius * .12f));
}

static float key_badge(ImDrawList *draw, ImVec2 start, const std::string &label, float unit, float size = 16) {
    float width = ImGui::GetFont()->CalcTextSizeA(size * unit, FLT_MAX, 0, label.c_str()).x + 16 * unit;
    if (draw) {
        draw->AddRectFilled(start, ImVec2(start.x + width, start.y + 26 * unit), IM_COL32(236, 232, 208, 24), 4 * unit);
        draw->AddRect(start, ImVec2(start.x + width, start.y + 26 * unit), IM_COL32(236, 232, 208, 70), 4 * unit);
        draw->AddText(ImGui::GetFont(), size * unit, ImVec2(start.x + 8 * unit, start.y + (26 - size) * unit * .5f),
                      IM_COL32(236, 232, 208, 255), label.c_str());
    }
    return width;
}

void close_picker(int figure) {
    draft.selected = figure;
    finished = draft;
    pending = true;
    if (!scan_picker) active = false;
    osk_cancel();
}
}

extern "C" bool amiibo_picker_active(void) { return active.load(); }
void amiibo_picker_set_controls(const AmiiboPickerControls &value) { controls = value; }
static void open_picker(bool scanning, uint64_t generation) {
    std::lock_guard<std::mutex> guard(lock);
    current = preview = amiibo_valid_figure(g_settings.amiibo_figure);
    draft.selected = -1;
    std::copy(g_settings.amiibo_favorites, g_settings.amiibo_favorites + AMIIBO_COUNT, draft.favorites);
    nrecent = std::min(g_settings.amiibo_recent_count, AMIIBO_RECENT_MAX);
    std::copy(g_settings.amiibo_recent, g_settings.amiibo_recent + nrecent, recent);
    tab = series = frames = 0;
    query[0] = 0;
    pending = false;
    scan_picker = scanning;
    scan_generation = generation;
    use_own_wolf = g_settings.amiibo_own_wolf;
    WolfLinkDump imported;
    wolf_status = use_own_wolf ? wolf_link_dump_load(&imported) : WOLF_DUMP_OK;
    appearing = true;
    focus_results_requested = false;
    input_reset();
    active = true;
}
extern "C" void amiibo_picker_open(void) { open_picker(false, 0); }
extern "C" void amiibo_picker_open_scan(uint64_t generation) {
    open_picker(true, generation);
}
extern "C" void amiibo_picker_cancel_scan(void) {
    std::lock_guard<std::mutex> guard(lock);
    if (!scan_picker) return;
    active = false;
    pending = false;
    scan_picker = false;
    osk_cancel();
}
extern "C" bool amiibo_picker_take(AmiiboPickerResult *result) {
    std::lock_guard<std::mutex> guard(lock);
    if (!pending || scan_picker) return false;
    *result = finished;
    pending = false;
    return true;
}
extern "C" void amiibo_picker_reset(void) {
    std::lock_guard<std::mutex> guard(lock);
    active = false;
    pending = false;
    appearing = false;
    scan_picker = false;
}

void amiibo_picker_set_fonts(ImFont *light, ImFont *bold, ImFont *edge) {
    picker_font = light;
    picker_bold = bold;
    picker_edge = edge;
}

void amiibo_picker_draw(float) {
    std::unique_lock<std::mutex> guard(lock);
    if (!active) return;
    ImGuiIO &io = ImGui::GetIO();
    bool was_typing = ui_text_busy();
    float unit = std::min(io.DisplaySize.x / 1280.f, io.DisplaySize.y / 720.f);
    ImVec2 origin((io.DisplaySize.x - 1280 * unit) * .5f, (io.DisplaySize.y - 720 * unit) * .5f);
    auto at = [&](float horizontal, float vertical) {
        return ImVec2(origin.x + horizontal * unit, origin.y + vertical * unit);
    };
    const ImU32 cream = IM_COL32(236, 232, 208, 255), muted = IM_COL32(155, 160, 149, 255);
    const ImU32 gold = IM_COL32(236, 194, 84, 255);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * unit);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * unit, 8 * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * unit, 6 * unit));
    ImGui::PushStyleColor(ImGuiCol_Text, cream);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(9, 11, 10, 240));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 41, 36, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(85, 87, 73, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(105, 105, 85, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(118, 117, 104, 240));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(118, 117, 104, 240));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(140, 138, 122, 255));
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::Begin((std::string(tr("Choose amiibo")) + "###amiibo-picker").c_str(), nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(22 * unit / ImGui::GetFontSize());
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, IM_COL32(12, 19, 21, 237), IM_COL32(12, 19, 21, 237),
                                  IM_COL32(7, 11, 10, 242), IM_COL32(7, 11, 10, 242));
    const char *title = tr("Choose amiibo");
    ImFont *title_font = picker_bold ? picker_bold : ImGui::GetFont();
    float title_width = title_font->CalcTextSizeA(38 * unit, FLT_MAX, 0, title).x;
    float title_size = std::min(38 * unit, 700 * unit * 38 * unit / std::max(title_width, 1.f));
    title_width = title_font->CalcTextSizeA(title_size, FLT_MAX, 0, title).x;
    game_text(draw, at(640 - title_width / unit * .5f, 58), title_size, cream, title, nullptr, true);
    float title_margin = title_width / unit * .5f + 32;
    draw->AddLine(at(140, 80), at(640 - title_margin, 80), IM_COL32(236, 194, 84, 65), unit);
    draw->AddLine(at(640 + title_margin, 80), at(1140, 80), IM_COL32(236, 194, 84, 65), unit);

    bool shortcut = !was_typing && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    bool keyboard_search = shortcut && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false);
    bool search_pressed = shortcut && ImGui::IsKeyPressed(ImGuiKey_F13, false);
    bool cleared = shortcut && (ImGui::IsKeyPressed(ImGuiKey_F14, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false));
    if (cleared) query[0] = 0;
    game_box(draw, at(140, 126), at(780, 170), unit);
    ImGui::SetCursorScreenPos(at(144, 128));
    ImGui::SetNextItemWidth(580 * unit);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    if (keyboard_search) {
        ImGuiID search_id = ImGui::GetID("##amiibo-search");
        GImGui->NavInputSource = ImGuiInputSource_Keyboard;
        ImGui::SetFocusID(search_id, ImGui::GetCurrentWindow());
        ImGui::ActivateItemByID(search_id);
        GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
    }
    bool search_confirmed = ui_input_text("##amiibo-search", query, sizeof query, ImGuiInputTextFlags_EnterReturnsTrue,
                                          tr("Search character, game or series..."), tr("Search amiibo"), search_pressed);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(at(738, 130));
    ImGui::BeginDisabled(!query[0]);
    if (ImGui::Button("x###amiibo-clear", ImVec2(36 * unit, 36 * unit))) { query[0] = 0; cleared = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Clear search"));
    ImGui::EndDisabled();
    if (shortcut && !search_pressed) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
            series = (series + AMIIBO_SERIES_COUNT - 1) % AMIIBO_SERIES_COUNT;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_RightBracket))
            series = (series + 1) % AMIIBO_SERIES_COUNT;
    }
    ImGui::SetCursorScreenPos(at(804, 128));
    ImGui::SetNextItemWidth(336 * unit);
    if (ImGui::BeginCombo("##amiibo-series", tr(amiibo_series_name(series)))) {
        for (int group = 0; group < AMIIBO_SERIES_COUNT; group++)
            if (ImGui::Selectable(tr(amiibo_series_name(group)), series == group)) series = group;
            else if (series == group) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
    int previous_tab = tab;
    if (!ui_text_busy() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2) || ImGui::IsKeyPressed(ImGuiKey_Q)) tab = (tab + 2) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2) || ImGui::IsKeyPressed(ImGuiKey_E)) tab = (tab + 1) % 3;
    }
    key_badge(draw, at(140, 194), controls.tabs, unit, 14);
    const char *tabs[] = {"All", "Favorites", "Recent"};
    for (int index = 0; index < 3; index++) {
        ImGui::SetCursorScreenPos(at(232 + index * 184, 188));
        ImGui::PushStyleColor(ImGuiCol_Button, index == tab ? IM_COL32(236, 194, 84, 30) : IM_COL32(0, 0, 0, 0));
        ImGui::PushID(index);
        std::string label = std::string(tr(tabs[index])) + "###amiibo-tab";
        if (ImGui::Button(label.c_str(), ImVec2(176 * unit, 38 * unit))) tab = index;
        ImGui::PopID();
        ImGui::PopStyleColor();
        if (index == tab) draw->AddRectFilled(at(244 + index * 184, 224), at(396 + index * 184, 226), gold);
    }
    std::vector<int> results;
    for (int slot = 0, count = tab == 2 ? nrecent : AMIIBO_COUNT; slot < count; slot++) {
        int figure = tab == 2 ? recent[slot] : slot;
        if (figure < 0 || figure >= AMIIBO_COUNT) continue;
        if (tab == 1 && !draft.favorites[figure]) continue;
        if (amiibo_matches(figure, query, series)) results.push_back(figure);
    }
    auto usable = [&](int figure) {
        return !use_own_wolf || wolf_status == WOLF_DUMP_OK || strcmp(virtual_amiibos[figure].id, "01030000024f0902");
    };
    bool preview_hidden = !results.empty() && std::find(results.begin(), results.end(), preview) == results.end();
    if (preview_hidden) preview = results.front();
    bool focus_result = appearing || previous_tab != tab || cleared || search_confirmed || focus_results_requested ||
                        (preview_hidden && !was_typing && !ui_text_busy() && !ImGui::IsAnyItemActive());
    focus_results_requested = false;
    bool shortcuts = !was_typing && !ui_text_busy() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    bool favorite_pressed = shortcuts && (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) ||
                                          (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)));
    ImGui::SetCursorScreenPos(at(804, 198));
    ImGui::TextDisabled(tr("Figures: %d"), (int)results.size());
    draw->AddLine(at(140, 236), at(1140, 236), IM_COL32(236, 232, 208, 35), unit);
    ImGui::SetCursorScreenPos(at(134, 244));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * unit, 6 * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * unit, 6 * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5 * unit);
    ImGui::BeginChild("##amiibo-results", ImVec2(652 * unit, 356 * unit), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoBackground);
    if (results.empty()) {
        ImGui::SetCursorPos(ImVec2(24 * unit, 64 * unit));
        ImGui::PushTextWrapPos(604 * unit);
        ImGui::TextWrapped("%s", tr("No figures match. Try another search or filter."));
        ImGui::PopTextWrapPos();
        ImGui::SetCursorPos(ImVec2(24 * unit, 144 * unit));
        if (ImGui::Button(tr("Reset filters"))) { query[0] = 0; series = tab = 0; focus_results_requested = true; }
    }
    struct DrawRow { int figure; ImVec2 start, end; bool favorite_hovered; };
    std::vector<DrawRow> rows;
    int focused_figure = -1, hovered_figure = -1, clicked_figure = -1;
    ImGuiListClipper clipper;
    clipper.Begin((int)results.size(), 52 * unit);
    if (focus_result && !results.empty())
        clipper.IncludeItemByIndex((int)(std::find(results.begin(), results.end(), preview) - results.begin()));
    while (clipper.Step())
        for (int position = clipper.DisplayStart; position < clipper.DisplayEnd; position++) {
            int figure = results[position];
            ImGui::PushID(figure);
            ImVec2 start = ImGui::GetCursorScreenPos();
            float width = ImGui::GetContentRegionAvail().x;
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            bool favorite_clicked = ImGui::InvisibleButton("##favorite", ImVec2(40 * unit, 46 * unit));
            ImGui::PopItemFlag();
            bool favorite_hovered = ImGui::IsItemHovered();
            if (favorite_clicked) { draft.favorites[figure] = !draft.favorites[figure]; clicked_figure = figure; }
            if (favorite_hovered) ImGui::SetTooltip("%s", tr(draft.favorites[figure] ? "Remove favorite" : "Add favorite"));
            ImGui::SameLine(0, 0);
            if (focus_result && figure == preview) ImGui::SetKeyboardFocusHere();
            ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
            bool selected = ImGui::Selectable("##figure", false, ImGuiSelectableFlags_AllowDoubleClick,
                                               ImVec2(width - 40 * unit, 46 * unit));
            ImGui::PopStyleColor(4);
            if (favorite_clicked) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(false);
            }
            if (selected) clicked_figure = figure;
            bool mouse_selection = selected && (ImGui::IsMouseClicked(0) || ImGui::IsMouseReleased(0));
            if (selected && frames > 2 && !favorite_pressed && usable(figure) && (!mouse_selection || ImGui::IsMouseDoubleClicked(0)))
                close_picker(figure);
            bool hovered = ImGui::IsItemHovered();
            if (hovered && figure == current && io.MousePos.x > start.x + width - 24 * unit)
                ImGui::SetTooltip("%s", tr("Selected figure"));
            if (hovered) hovered_figure = figure;
            if (ImGui::IsItemFocused()) focused_figure = figure;
            if (focus_result && figure == preview) {
                ImGui::FocusWindow(ImGui::GetCurrentWindow());
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                ImGui::SetItemDefaultFocus();
                ImGui::SetScrollHereY(.5f);
            }
            if (ImGui::IsItemVisible())
                rows.push_back({figure, start, ImVec2(start.x + width, start.y + 46 * unit), favorite_hovered});
            ImGui::PopID();
        }
    if (clicked_figure >= 0) preview = clicked_figure;
    else if ((io.MouseDelta.x || io.MouseDelta.y) && hovered_figure >= 0) preview = hovered_figure;
    else if (focused_figure >= 0) preview = focused_figure;
    if (favorite_pressed && !results.empty()) draft.favorites[preview] = !draft.favorites[preview];
    ImDrawList *list_draw = ImGui::GetWindowDrawList();
    for (const DrawRow &row : rows) {
        bool selected = row.figure == preview;
        game_cell(list_draw, row.start, row.end, selected, unit);
        favorite_star(list_draw, ImVec2(row.start.x + 22 * unit, row.start.y + 23 * unit), 10 * unit,
                      draft.favorites[row.figure], row.favorite_hovered);
        ImVec4 clip(row.start.x + 46 * unit, row.start.y, row.end.x - 20 * unit, row.end.y);
        game_text(list_draw, ImVec2(row.start.x + 50 * unit, row.start.y + 10 * unit), 24 * unit,
                  cream, virtual_amiibos[row.figure].name, &clip);
        if (row.figure == current)
            list_draw->AddCircleFilled(ImVec2(row.end.x - 10 * unit, row.start.y + 23 * unit), 2.5f * unit, gold);
        if (selected) game_cursor(list_draw, row.start, row.end, unit);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);

    game_box(draw, at(804, 250), at(1140, 600), unit);
    ImGui::SetCursorScreenPos(at(824, 270));
    ImGui::BeginChild("##amiibo-details", ImVec2(296 * unit, 310 * unit), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImDrawList *details = ImGui::GetWindowDrawList();
    ImVec2 detail_origin = ImGui::GetCursorScreenPos();
    ImVec4 detail_clip(detail_origin.x, detail_origin.y, detail_origin.x + 296 * unit, detail_origin.y + 310 * unit);
    game_text(details, detail_origin, 17 * unit, muted, tr("Preview"), &detail_clip);
    if (!results.empty() && !usable(preview)) {
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 36 * unit), 25 * unit, cream,
                  virtual_amiibos[preview].name, &detail_clip, true, 286 * unit);
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 128 * unit), 21 * unit, gold,
                  tr(wolf_link_dump_status_text(wolf_status)), &detail_clip, false, 286 * unit);
    } else if (!results.empty()) {
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 36 * unit), 28 * unit, cream,
                  virtual_amiibos[preview].name, &detail_clip, true, 286 * unit);
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 142 * unit), 17 * unit, muted, tr("Series"), &detail_clip);
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 168 * unit), 22 * unit, cream,
                  tr(amiibo_series_name(amiibo_series(preview))), &detail_clip, false, 286 * unit);
        if (!strcmp(virtual_amiibos[preview].id, "01030000024f0902"))
            game_text(details, ImVec2(detail_origin.x, detail_origin.y + 214 * unit), 19 * unit, gold,
                      tr(use_own_wolf ? "Own amiibo" : "Synthetic (20 hearts)"), &detail_clip);
        game_text(details, ImVec2(detail_origin.x, detail_origin.y + 242 * unit), 17 * unit, muted, tr("amiibo ID"), &detail_clip);
        details->AddText(ImGui::GetFont(), 21 * unit, ImVec2(detail_origin.x, detail_origin.y + 269 * unit),
                         cream, virtual_amiibos[preview].id);
    } else {
        ImVec2 center(detail_origin.x + 138 * unit, detail_origin.y + 140 * unit);
        details->AddCircle(center, 24 * unit, IM_COL32(155, 160, 149, 100), 32, 2 * unit);
        details->AddLine(ImVec2(center.x + 17 * unit, center.y + 17 * unit),
                          ImVec2(center.x + 38 * unit, center.y + 38 * unit), IM_COL32(155, 160, 149, 100), 3 * unit);
    }
    ImGui::EndChild();

    struct Hint { const char *label; const std::string *key; };
    std::vector<Hint> hints = {{tr("Search amiibo"), &controls.search}, {tr("Clear search"), &controls.clear},
                               {tr("Series"), &controls.series}};
    if (!results.empty()) hints.push_back({tr(draft.favorites[preview] ? "Remove favorite" : "Add favorite"), &controls.favorite});
    float total = 0;
    for (const Hint &hint : hints)
        total += key_badge(nullptr, ImVec2(), *hint.key, unit) + 8 * unit +
                 ImGui::GetFont()->CalcTextSizeA(17 * unit, FLT_MAX, 0, hint.label).x + 28 * unit;
    float hint_unit = unit * std::min(1.f, 1000 * unit / std::max(total, 1.f));
    ImVec2 hint_position = at(140, 614);
    for (const Hint &hint : hints) {
        hint_position.x += key_badge(draw, hint_position, *hint.key, hint_unit) + 8 * hint_unit;
        draw->AddText(ImGui::GetFont(), 17 * hint_unit, ImVec2(hint_position.x, hint_position.y + 4 * hint_unit), muted, hint.label);
        hint_position.x += ImGui::GetFont()->CalcTextSizeA(17 * hint_unit, FLT_MAX, 0, hint.label).x + 28 * hint_unit;
    }
    draw->AddLine(at(140, 650), at(1140, 650), IM_COL32(236, 232, 208, 35), unit);
    ImGui::SetCursorScreenPos(at(140, 664));
    std::string cancel_label = std::string(tr("Cancel")) + "###amiibo-cancel";
    if (ImGui::Button(cancel_label.c_str(), ImVec2(170 * unit, 40 * unit))) close_picker(-1);
    key_badge(draw, at(326, 671), controls.cancel, unit);
    ImGui::SetCursorScreenPos(at(820, 664));
    ImGui::BeginDisabled(results.empty() || !usable(preview));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(236, 194, 84, 225));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, gold);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 219, 130, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(25, 29, 24, 255));
    std::string confirm_label = std::string(tr("Choose amiibo")) + "###amiibo-confirm";
    if (ImGui::Button(confirm_label.c_str(), ImVec2(320 * unit, 40 * unit))) close_picker(preview);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (!results.empty()) {
        float badge_width = key_badge(nullptr, ImVec2(), controls.confirm, unit);
        key_badge(draw, at(804 - badge_width / unit, 671), controls.confirm, unit);
    }
    bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (frames > 2 && !popup && !was_typing && !ui_text_busy() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) close_picker(-1);
    ImGui::SetWindowFontScale(1);
    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(5);
    appearing = false;
    frames++;
    if (scan_picker && pending) {
        AmiiboPickerResult result = finished;
        uint64_t generation = scan_generation;
        pending = false;
        guard.unlock();
        bool accepted = nfp_scan_complete(generation, &result);
        guard.lock();
        if (scan_picker && scan_generation == generation) {
            if (accepted) { active = false; scan_picker = false; }
            else {
                WolfLinkDump imported;
                wolf_status = wolf_link_dump_load(&imported);
                active = true;
            }
        }
    }
}
