// On-screen keyboard (see osk.h). A controller has no keys to type with, and ImGui only starts text entry on a
// gamepad's Y (A on a field did nothing): A on any of our text fields opens this keyboard instead.
//
// One keyboard at a time (only one ImGui UI runs: start screen or in-game overlay, one thread each). It is a modal
// popup at the root of the UI, so the menu behind sees "a popup is open" and B / Esc / L / R don't act on it.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "osk.h"

static struct {
    int open;                        // a field's keyboard is up
    ImGuiID field;                   // the field it types for
    char *buf;                       // ... and its buffer (static in every caller: written on OK)
    size_t size;
    ImGuiInputTextFlags flags;
    char text[128], title[96];
    bool appearing, nav_kbd_off;
    ImGuiID done_field;              // answered: reported by that field's next ui_input_text
    int done, done_frame;            // 1 OK, -1 cancelled; when (a field gone by then never hears of it)
} s;
static std::atomic<bool> s_cancel_req{false};
static bool s_cancelled_now;

bool osk_is_open(void) { return s.open != 0; }
void osk_cancel(void) { s_cancel_req = true; }

bool ui_text_busy(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    return s.open || (g && (g->IO.WantTextInput || g->WantTextInputNextFrame == 1));
}

bool ui_input_text_cancelled(void) { return s_cancelled_now; }

// (test) SAVAGEWIND_FIELD_FOCUS=<label>:<n>[:edit]: the n-th time field <label> is drawn it gets the nav cursor (a
// controller's A - SAVAGEWIND_HOST_INPUT sdlpad:east - then opens the keyboard); :edit starts typing in it instead.
static int test_focus(const char *label) {
    static const char *spec = getenv("SAVAGEWIND_FIELD_FOCUS");
    static int count;
    if (!spec) return 0;
    const char *c = strchr(spec, ':');
    if (!c || strncmp(spec, label, (size_t)(c - spec)) || label[c - spec]) return 0;
    if (++count != atoi(c + 1)) return 0;
    return strstr(c + 1, ":edit") ? 2 : 1;
}

bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags, const char *hint, const char *title, bool open_keyboard) {
    ImGuiContext &g = *GImGui;
    int test = test_focus(label);
    if (test == 2) ImGui::SetKeyboardFocusHere();
    bool r = hint ? ImGui::InputTextWithHint(label, hint, buf, size, flags) : ImGui::InputText(label, buf, size, flags);
    ImGuiID id = ImGui::GetItemID();
    if (test == 1) { ImGui::FocusItem(); ImGui::SetNavCursorVisible(true); }
    // (and in view for the screenshots for a few frames: the focus' own scroll misses on a NavFlattened page)
    static ImGuiID in_view_id;
    static int in_view;
    if (test) in_view_id = id, in_view = 10;
    if (in_view_id == id && in_view > 0 && in_view--) ImGui::SetScrollHereY(0.5f);
    if (test) fprintf(stderr, "[osk] test: %s field %s\n", test == 2 ? "typing in" : "cursor on", label);
    static std::string last;  // (test runs: what the field holds, as it changes)
    const char *tf = getenv("SAVAGEWIND_FIELD_FOCUS");
    if (tf && !strncmp(tf, label, strlen(label)) && tf[strlen(label)] == ':' && last != buf) { last = buf; fprintf(stderr, "[osk] test: %s = \"%s\"%s\n", label, buf, g.ActiveId == id ? " (typing)" : ""); }
    s_cancelled_now = false;
    if (s.done && s.done_field == id && ImGui::GetFrameCount() - s.done_frame <= 1) {
        if (s.done > 0) r = true;
        else s_cancelled_now = true;
        s.done = 0, s.done_field = 0;
    }
    // activated from the controller: A (ImGui's "activate", which leaves a text field alone), Y (its "input"), or a
    // focus request (SetKeyboardFocusHere) while the controller drives the menu. A mouse click types directly.
    bool by_nav = open_keyboard || g.NavActivateId == id || (ImGui::IsItemActivated() && g.ActiveIdSource != ImGuiInputSource_Mouse);
    if (by_nav && (open_keyboard || g.NavInputSource == ImGuiInputSource_Gamepad) && !s.open && !(flags & ImGuiInputTextFlags_ReadOnly)) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        s.open = 1, s.appearing = true, s.done = 0;
        s.field = id, s.buf = buf, s.size = size, s.flags = flags;
        snprintf(s.text, sizeof s.text, "%s", buf);
        snprintf(s.title, sizeof s.title, "%s", tr(title ? title : hint ? hint : "Enter text"));
        r = false;
    }
    return r;
}

int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first) {
    static bool upper = true;
    if (appearing) upper = upper_first;
    ImGuiIO &io = ImGui::GetIO();
    // B deletes: owning it keeps ImGui's nav cancel (which drops the cursor off the keys) from seeing it
    ImGuiID own = ImGui::GetID("##oskkeys");
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, own);
    if (max_chars <= 0 || max_chars > (int)cap - 1) max_chars = (int)cap - 1;
    int len = (int)strlen(text), r = 0;
    auto add = [&](char ch) { if (len < max_chars) text[len++] = ch, text[len] = 0; };
    auto del = [&]() { if (len > 0) text[--len] = 0; };

    // typed on a keyboard while it's up: straight into the text
    for (ImWchar c : io.InputQueueCharacters) if (c >= 0x20 && c < 0x7f) add((char)c);
    io.InputQueueCharacters.resize(0);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) del();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) r = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) r = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) r = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, ImGuiInputFlags_Repeat, own)) del();

    ImGui::PushFont(big);
    ImGui::Text("%s_", text);
    ImGui::PopFont();
    static const char *rows_u[] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "0123456789"};
    static const char *rows_l[] = {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "0123456789"};
    const char **rows = upper ? rows_u : rows_l;
    float k = 70 * scale;
    for (int row = 0; row < 4; row++) {
        for (int i = 0; rows[row][i]; i++) {
            if (i) ImGui::SameLine();
            char lbl[8] = {rows[row][i], 0};
            if (appearing && row == 0 && i == 0) {  // (cursor shown: else the first A only reveals it)
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            if (ImGui::Button(lbl, ImVec2(k, k))) add(rows[row][i]);
        }
    }
    if (ImGui::Button(upper ? "abc" : "ABC", ImVec2(k * 2, k))) upper = !upper;
    ImGui::SameLine();
    if (ImGui::Button(tr("Space"), ImVec2(k * 3, k)) && len > 0) add(' ');
    ImGui::SameLine();
    if (ImGui::Button(tr("Delete"), ImVec2(k * 2, k))) del();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(k * 2, k))) r = -1;
    ImGui::SameLine();
    if (ImGui::Button(tr("OK"), ImVec2(k * 2, k))) r = 1;
    ImGui::TextDisabled(tr("A  type      B  delete      Start / +  OK      (%d / %d)"), len, max_chars);
    return r;
}

// close it (and give the keyboard back to ImGui's navigation)
static void finish(int result) {
    if (result > 0 && s.buf && s.size) {
        if (s.flags & ImGuiInputTextFlags_CharsUppercase)
            for (char *p = s.text; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(s.buf, s.size, "%s", s.text);
    }
    s.done = result, s.done_field = s.field, s.done_frame = ImGui::GetFrameCount();
    s.open = 0, s.buf = NULL;
    if (s.nav_kbd_off) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s.nav_kbd_off = false;
}

void osk_draw(ImFont *big, float scale) {
    bool cancel = s_cancel_req.exchange(false);
    if (!s.open) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGuiContext &g = *GImGui;
    if (!ImGui::IsPopupOpen("##osk")) {
        ImGui::OpenPopup("##osk");
        s.appearing = true;
        // keys typed while it's up are text: Space / Enter / arrows must not also press the focused key
        if (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) { io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard; s.nav_kbd_off = true; }
    }
    g.WantTextInputNextFrame = 1;  // (typing stays on: the backends keep text input enabled; ui_text_busy)
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##osk", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        finish(-1);
        return;
    }
    ImGui::PushFont(big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", s.title);
    ImGui::PopFont();
    int max = (int)(s.size < sizeof s.text ? s.size : sizeof s.text) - 1;
    int r = cancel ? -1 : osk_keys(s.text, sizeof s.text, max, s.appearing, scale, big);
    s.appearing = false;
    if (r) {
        ImGui::CloseCurrentPopup();
        finish(r);
    }
    ImGui::EndPopup();
}
