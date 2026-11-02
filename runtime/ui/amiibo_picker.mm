// The guest thread owns live settings and scan configuration; the render thread edits a snapshot.
#include "amiibo_picker.h"
#include "i18n.h"
#include "osk.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <mutex>
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
int current, preview, tab, series, frames;
int recent[AMIIBO_RECENT_MAX], nrecent;
char query[96];

void close_picker(int figure) {
    draft.selected = figure;
    finished = draft;
    pending = true;
    active = false;
    osk_cancel();
}
}

extern "C" bool amiibo_picker_active(void) { return active.load(); }
extern "C" void amiibo_picker_open(void) {
    std::lock_guard<std::mutex> guard(lock);
    current = preview = amiibo_valid_figure(g_settings.amiibo_figure);
    draft.selected = -1;
    std::copy(g_settings.amiibo_favorites, g_settings.amiibo_favorites + AMIIBO_COUNT, draft.favorites);
    nrecent = std::min(g_settings.amiibo_recent_count, AMIIBO_RECENT_MAX);
    std::copy(g_settings.amiibo_recent, g_settings.amiibo_recent + nrecent, recent);
    tab = series = frames = 0;
    query[0] = 0;
    pending = false;
    appearing = true;
    input_reset();
    active = true;
}
extern "C" bool amiibo_picker_take(AmiiboPickerResult *result) {
    std::lock_guard<std::mutex> guard(lock);
    if (!pending) return false;
    *result = finished;
    pending = false;
    return true;
}
extern "C" void amiibo_picker_reset(void) {
    std::lock_guard<std::mutex> guard(lock);
    active = false;
    pending = false;
    appearing = false;
}

void amiibo_picker_draw(float scale) {
    std::lock_guard<std::mutex> guard(lock);
    if (!active) return;
    ImGuiIO &io = ImGui::GetIO();
    bool was_typing = ui_text_busy();
    ImVec2 size(std::min(980.f * scale, io.DisplaySize.x * .94f), io.DisplaySize.y * .88f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::Begin(tr("Choose amiibo"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::TextWrapped("%s: %s", tr("Selected figure"), virtual_amiibos[current].name);
    ImGui::SetNextItemWidth(-1);
    ui_input_text("##amiibo-search", query, sizeof query, 0, tr("Search character, game or series..."), tr("Search amiibo"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##amiibo-series", tr(amiibo_series_name(series)))) {
        for (int i = 0; i < AMIIBO_SERIES_COUNT; i++)
            if (ImGui::Selectable(tr(amiibo_series_name(i)), series == i)) series = i;
        ImGui::EndCombo();
    }
    const char *tabs[] = {"All", "Favorites", "Recent"};
    if (!ui_text_busy() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) tab = (tab + 2) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) tab = (tab + 1) % 3;
    }
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool selected_tab = tab == i;
        if (selected_tab) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(tr(tabs[i]))) tab = i;
        if (selected_tab) ImGui::PopStyleColor();
    }
    std::vector<int> results;
    for (int slot = 0, n = tab == 2 ? nrecent : AMIIBO_COUNT; slot < n; slot++) {
        int i = tab == 2 ? recent[slot] : slot;
        if (i < 0 || i >= AMIIBO_COUNT) continue;
        if (tab == 1 && !draft.favorites[i]) continue;
        if (amiibo_matches(i, query, series)) results.push_back(i);
    }
    ImGui::Text("%d %s", (int)results.size(), tr("figures"));
    float footer = ImGui::GetTextLineHeightWithSpacing() * 3 + ImGui::GetFrameHeightWithSpacing() + 18 * scale;
    ImGui::BeginChild("##amiibo-results", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (results.empty()) ImGui::TextWrapped("%s", tr("No figures match. Try another search or filter."));
    for (int i : results) {
        ImGui::PushID(i);
        if (ImGui::SmallButton(draft.favorites[i] ? "[-]" : "[+]")) draft.favorites[i] = !draft.favorites[i];
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(draft.favorites[i] ? "Remove favorite" : "Add favorite"));
        ImGui::SameLine();
        if (appearing && i == current) ImGui::SetKeyboardFocusHere();
        if (ImGui::Selectable(virtual_amiibos[i].name, current == i) && frames > 2) close_picker(i);
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) preview = i;
        if (appearing && i == current) {
            ImGui::SetItemDefaultFocus();
            ImGui::SetScrollHereY(.5f);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextWrapped("%s", virtual_amiibos[preview].name);
    ImGui::TextDisabled("%s", tr(amiibo_series_name(amiibo_series(preview))));
    ImGui::TextDisabled("%s", tr("[+] Favorite   L/R: tabs   Confirm: select   Back: cancel"));
    if (ImGui::Button(tr("Cancel"))) close_picker(-1);
    bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (frames > 2 && !popup && !was_typing && !ui_text_busy() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) close_picker(-1);
    ImGui::End();
    appearing = false;
    frames++;
}
