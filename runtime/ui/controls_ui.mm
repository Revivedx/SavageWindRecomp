// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm). First a table of the game's own actions (Jump, Attack...), each with a
// controller button and a key of its own: two actions can share one, and an action keeps the button of its Switch
// input until it is given a binding. Below it the inputs no action covers (sticks, camera, motion, menus), and, folded,
// the Switch buttons the actions follow. Select a binding, then press the new button / key (Esc or 5 s without a
// press cancels; a Switch input's button already in use swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "i18n.h"
#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
}
extern "C" bool control_enabled(void);  // runtime/control.c: is the test endpoint on (SAVAGEWIND_CONTROL)?

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, "%s  %d##%d_%d", tr(dev == BIND_DEV_PAD ? "Press a button..." : "Press a key..."),
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", tr(dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                    bind_key_label(g_bind_key[slot])), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited savagewind.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int pushed = 0;
    bool dup = false;
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        dup |= i != slot && (dev == BIND_DEV_KEY ? bind_key_same_context(i, slot) : bind_pad_slot(i)) && t[i] == t[slot] &&
               t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
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

// ---- what a binding says: a Switch input, or an action (slot BIND_ACTION + id)
static void action_name(int id, char *out, size_t n) {
    if (act_name(id)) snprintf(out, n, "%s", tr(act_name(id)));
    else snprintf(out, n, tr("Action %d"), id);
}
static void slot_name(int slot, char *out, size_t n) {
    if (bind_slot_is_action(slot)) action_name(slot - BIND_ACTION, out, n);
    else snprintf(out, n, "%s", tr(bind_input_name(slot)));
}
static const char *host_name(int dev, int host, int fam) {
    return tr(dev == BIND_DEV_PAD ? bind_pad_label(host, fam) : bind_key_label(host));
}
// what the slot is bound to now
static const char *slot_binding(int dev, int slot, int fam) {
    if (!bind_slot_is_action(slot)) return host_name(dev, dev == BIND_DEV_PAD ? g_bind_pad[slot] : g_bind_key[slot], fam);
    const int h = dev == BIND_DEV_PAD ? g_act_pad[slot - BIND_ACTION] : g_act_key[slot - BIND_ACTION];
    return h == ACT_NONE ? tr("Unbound") : h == ACT_FOLLOW ? tr("Default") : host_name(dev, h, fam);
}

// An action's cell: its own binding in gold, or (grey) the button of the Switch input the game reads for it, which
// is what it follows until it is given one. Right-click: back to that, or nothing.
static void action_cell(int dev, int id, int fam, int cap_dev, int cap_slot, int ms_left) {
    const int slot = BIND_ACTION + id;
    const int host = dev == BIND_DEV_PAD ? g_act_pad[id] : g_act_key[id];
    const bool waiting = cap_dev == dev && cap_slot == slot;
    char lbl[96];
    if (waiting) snprintf(lbl, sizeof lbl, "%s  %d##a%d_%d", tr(dev == BIND_DEV_PAD ? "Press a button..." : "Press a key..."),
                          (ms_left + 999) / 1000, dev, id);
    else if (host == ACT_FOLLOW) {
        const int b = act_switch_button(id);  // (-1 until the game has run: the start screen)
        snprintf(lbl, sizeof lbl, "%s##a%d_%d", b < 0 ? tr("Default") :
                 host_name(dev, dev == BIND_DEV_PAD ? g_bind_pad[b] : g_bind_key[b], fam), dev, id);
    } else snprintf(lbl, sizeof lbl, "%s##a%d_%d", slot_binding(dev, slot, fam), dev, id);
    int pushed = 0;
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    if (host != ACT_FOLLOW) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
    else if (!waiting) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(tr("Use default"), NULL, false, host != ACT_FOLLOW)) { act_set(dev, id, ACT_FOLLOW); settings_save(); }
        if (ImGui::MenuItem(tr("Unbind"), NULL, false, host != ACT_NONE)) { act_set(dev, id, ACT_NONE); settings_save(); }
        ImGui::EndPopup();
    }
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
}

// The table of actions: every one the game asks about (the ones no code in the game reaches are left out)
static void action_table(const char *id, int fam, int cd, int cs, int ms) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(tr("Controller"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn(tr("Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();
    for (int a = 0; a < ACT_COUNT; a++) {
        if (act_hidden(a)) continue;
        char name[64];
        action_name(a, name, sizeof name);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        ImGui::TableNextColumn();
        action_cell(BIND_DEV_PAD, a, fam, cd, cs, ms);
        ImGui::TableNextColumn();
        action_cell(BIND_DEV_KEY, a, fam, cd, cs, ms);
    }
    ImGui::EndTable();
}

enum { FIXED_ESC = -1, FIXED_F2 = -2, FIXED_F1 = -3 };  // (rows of switch_table that are not a slot)

// Rows of Switch inputs: the controller button and the key bound to each
static void switch_table(const char *id, const int *order, int rows, int fam, int cd, int cs, int ms) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(tr("Controller"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn(tr("Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();
    for (int row = 0; row < rows; row++) {
        const int slot = order[row];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (slot == FIXED_ESC || slot == FIXED_F2 || slot == FIXED_F1) {  // hard-wired: shown, never changed
            ImGui::TextUnformatted(tr(slot == FIXED_ESC ? "Pause menu / back" : slot == FIXED_F2 ? "Free mouse cursor" :
                                                                                                    "Savage Wind menu"));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", slot == FIXED_F1 ? tr(bind_pad_label(5 /* guide */, fam)) : "-");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", tr(bind_key_label(slot == FIXED_ESC ? SDL_SCANCODE_ESCAPE :
                                                        slot == FIXED_F2 ? SDL_SCANCODE_F2 : SDL_SCANCODE_F1)));
            continue;
        }
        ImGui::TextUnformatted(tr(bind_input_name(slot)));
        ImGui::TableNextColumn();
        if (bind_pad_slot(slot)) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
        else ImGui::TextDisabled("%s", tr(slot == BIND_WALK ? "Tilt the stick" : slot == BIND_TILT ? "Motion sensors" :
                                          bind_key_gyro(slot) ? "Motion sensors" : slot < BIND_RS_UP ? "Left stick" : "Right stick"));
        ImGui::TableNextColumn();
        cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
    }
    ImGui::EndTable();
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
        const char *now = rslot < 0 ? "" : slot_binding(rdev, rslot, fam);
        char name[64] = "", other[64] = "";
        if (rslot >= 0) slot_name(rslot, name, sizeof name);
        if (rother >= 0) slot_name(rother, other, sizeof other);
        if (kind == 5)
            snprintf(msg, sizeof msg, "%s", tr(rdev == BIND_DEV_PAD ? "Controller bindings reset to the defaults."
                                                                     : "Keyboard bindings reset to the defaults."));
        else if (kind == 1) snprintf(msg, sizeof msg, tr("%s is now %s."), name, now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, tr("%s is now %s; %s moved to %s (swapped)."), name, now, other,
                     slot_binding(rdev, rother, fam));
        else if (kind == 3) snprintf(msg, sizeof msg, tr("Cancelled: %s unchanged."), name);
        else if (kind == 4) snprintf(msg, sizeof msg, tr("Nothing pressed: %s unchanged."), name);
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", msg);

    // the page: the game's actions first (they follow the Switch inputs below until given a binding of their own)
    action_table("##actions", fam, cd, cs, ms);
    bool all_default = true;
    for (int a = 0; a < ACT_COUNT; a++) all_default &= act_is_default(a);
    ImGui::BeginDisabled(all_default);
    if (ImGui::Button(tr("Reset actions to defaults"))) { bind_capture_cancel(); act_reset(); settings_save(); }
    ImGui::EndDisabled();

    // what no action covers: the game's menus and quick menus, then movement, camera and motion. Raw Switch buttons are
    // never listed by their name, only by what they do.
    ImGui::SeparatorText(tr("Menus"));
    static const int menus[] = {BIND_PLUS, BIND_MINUS, BIND_UP, BIND_LEFT, BIND_RIGHT, BIND_A, BIND_B, BIND_X, BIND_Y,
                                BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_RSTICK, FIXED_ESC, FIXED_F2, FIXED_F1};
    switch_table("##binds_menus", menus, (int)(sizeof menus / sizeof *menus), fam, cd, cs, ms);
    ImGui::SeparatorText(tr("Movement, camera and motion"));
    static const int other[] = {BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                BIND_RS_LEFT, BIND_RS_RIGHT, BIND_WALK, BIND_TILT, BIND_GYRO_UP, BIND_GYRO_DOWN,
                                BIND_GYRO_LEFT, BIND_GYRO_RIGHT, BIND_GYRO_TURN_LEFT, BIND_GYRO_TURN_RIGHT,
                                BIND_GYRO_CENTER, BIND_FLY};
    const int other_rows = (int)(sizeof other / sizeof *other) - (control_enabled() ? 0 : 1);  // (the fly row: test builds only)
    switch_table("##binds_other", other, other_rows, fam, cd, cs, ms);

    ImGui::Spacing();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (ImGui::Button(tr("Reset controller to defaults"))) { bind_capture_cancel(); bind_reset(BIND_DEV_PAD); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));
    if (ImGui::Button(tr("Reset keyboard to defaults"))) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY); }
    ImGui::EndDisabled();

    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Bindings apply to the game only. Esc and F1 always open the in-game menu, and so do the "
                                 "controller's Back + Start held together or its Home / Guide button, so this page can't "
                                 "be locked out. The game's button icons follow the controller bindings."));
    ImGui::PopTextWrapPos();
}
