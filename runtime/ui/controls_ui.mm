// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the key bound to
// each. Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "i18n.h"
#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
}

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, "%s  %d##%d_%d", tr(dev == BIND_DEV_PAD ? "Press a button..." : "Press a key..."),
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", tr(dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                    bind_key_label(g_bind_key[slot])), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited savagewind.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
}

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
    int fam = bind_label_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *pad_name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : NULL;
    char line[300];
    if (n > 0) snprintf(line, sizeof line, tr("Controller: %s  (%s button names)"), pad_name ? pad_name : tr("connected"), fam_name[fam % 3]);
    else snprintf(line, sizeof line, tr("No controller connected  (%s button names shown)"), fam_name[fam % 3]);
    SDL_free(ids);
    ImGui::TextUnformatted(line);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A button "
                                 "that's already in use swaps with the old one."));
    ImGui::PopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep savagewind.ini current
    static int seen_serial = -1;
    static char msg[400];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const char *now = rslot < 0 ? "" : tr(rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rslot], fam) : bind_key_label(g_bind_key[rslot]));
        if (kind == 5)
            snprintf(msg, sizeof msg, "%s", tr(rdev == BIND_DEV_PAD ? "Controller bindings reset to the defaults."
                                                                     : "Keyboard bindings reset to the defaults."));
        else if (kind == 1) snprintf(msg, sizeof msg, tr("%s is now %s."), tr(bind_input_name(rslot)), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, tr("%s is now %s; %s moved to %s (swapped)."), tr(bind_input_name(rslot)), now,
                     tr(bind_input_name(rother)), tr(rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother])));
        else if (kind == 3) snprintf(msg, sizeof msg, tr("Cancelled: %s unchanged."), tr(bind_input_name(rslot)));
        else if (kind == 4) snprintf(msg, sizeof msg, tr("Nothing pressed: %s unchanged."), tr(bind_input_name(rslot)));
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", msg);

    if (ImGui::BeginTable("##binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Controller"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(tr("Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT, BIND_WALK, BIND_TILT};
        for (int slot : order) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(bind_input_name(slot)));
            ImGui::TableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
            else ImGui::TextDisabled("%s", tr(slot == BIND_WALK ? "Tilt the stick" : slot == BIND_TILT ? "Motion sensors" :
                                              slot < BIND_RS_UP ? "Left stick" : "Right stick"));
            ImGui::TableNextColumn();
            cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (ImGui::Button(tr("Reset controller to defaults"))) { bind_capture_cancel(); bind_reset(BIND_DEV_PAD); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));
    if (ImGui::Button(tr("Reset keyboard to defaults"))) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY); }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s %s", tr("Bindings apply to the game only. Menus keep the fixed layout (right face button / Enter "
                                 "select, bottom face button / Esc back), and the in-game menu always opens with F1 or Esc, or "
                                 "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                                 "locked out. The game's button icons follow the controller bindings."),
                       tr("In the title, pause and map menus, unbound arrow keys act as the left stick and Enter selects."));
    ImGui::PopTextWrapPos();
}
