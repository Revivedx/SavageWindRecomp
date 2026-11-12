// Launch options on the start screen: a list of flags (empty at first), a name and a value field with Enter / Add, and
// suggestions from the table of flags the program reads (runtime/launch_flags_table.c) as the name is typed. Any other
// name can be typed too. Every change is saved at once; PLAY restarts the game once if the saved flags differ from the
// ones this run started with (runtime/launch_flags_boot.c).
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "launch_flags.h"
#include "launch_flags_boot.h"
#include "i18n.h"
#include "launch_flags_ui.h"
#include "osk.h"

namespace {

LfList g_list;
bool g_loaded;
char g_name[LF_NAME_MAX], g_value[LF_VALUE_MAX];
char g_msg[200];            // the last problem, in red (empty: none)
bool g_msg_error;
char g_row_value[LF_MAX_FLAGS][LF_VALUE_MAX];
bool g_row_active[LF_MAX_FLAGS];

const ImVec4 kGold(0.93f, 0.76f, 0.33f, 1), kWarn(1, 0.6f, 0.35f, 1), kBad(1, 0.5f, 0.4f, 1);

void say(const char *text, bool error) {
    snprintf(g_msg, sizeof g_msg, "%s", text);
    g_msg_error = error;
}

void save(void) {
    char path[1100];
    if (!launch_flags_path(path, sizeof path) || !lf_save(&g_list, path))
        say(tr("Could not save the launch options (is the data folder writable?)."), true);
}

void add(void) {
    LfError e = lf_set(&g_list, g_name, g_value);
    if (e != LF_OK) {
        say(tr(lf_error_text(e)), true);
        return;
    }
    g_msg[0] = g_name[0] = g_value[0] = 0;
    save();
}

void remove_at(int i) {
    lf_remove(&g_list, i);
    memset(g_row_active, 0, sizeof g_row_active);
    g_msg[0] = 0;
    save();
}

}  // namespace

void launch_flags_ui(float sc) {
    if (!g_loaded) {
        char path[1100];
        g_loaded = true;
        if (launch_flags_path(path, sizeof path)) {
            lf_load(&g_list, path);
        }
    }
    ImGui::PushID("##launchflags");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Extra settings for testing and fixing problems. Only add one if you were asked to. "
                                 "They are kept until you remove them, and take effect when you press Play."));
    ImGui::PopTextWrapPos();
    if (launch_flags_disabled())
        ImGui::TextColored(kWarn, "%s", tr("Launch options are switched off for this start (--no-launch-flags)."));
    ImGui::Spacing();

    // the list
    if (g_list.n == 0) ImGui::TextDisabled("%s", tr("No launch options set."));
    float vw = 260 * sc, bw = 150 * sc;
    for (int i = 0; i < g_list.n; i++) {
        LfFlag &f = g_list.flag[i];
        ImGui::PushID(i);
        const LfInfo *info = lf_find(f.name);
        ImGui::TextColored(info && info->group == LF_GROUP_DEV ? kWarn : kGold, "%s", f.name);
        if (!g_row_active[i]) snprintf(g_row_value[i], sizeof g_row_value[i], "%s", f.value);
        ImGui::SetNextItemWidth(vw);
        if (ui_input_text("##value", g_row_value[i], sizeof g_row_value[i], ImGuiInputTextFlags_EnterReturnsTrue, NULL,
                          f.name)) {
            LfError e = lf_check(f.name, g_row_value[i]);
            if (e != LF_OK) {
                say(tr(lf_error_text(e)), true);
            } else {
                // (an empty value is 1, as when adding)
                snprintf(f.value, sizeof f.value, "%s", g_row_value[i][0] ? g_row_value[i] : "1");
                g_msg[0] = 0;
                save();
            }
        }
        g_row_active[i] = ImGui::IsItemActive();
        ImGui::SameLine();
        bool gone = ImGui::Button(tr("Remove"), ImVec2(bw, 0));
        if (info && info->desc[0]) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", tr(info->desc));
            ImGui::PopTextWrapPos();
        } else if (!info) {
            ImGui::TextDisabled("%s", tr("Custom flag"));
        }
        ImGui::PopID();
        if (gone) {
            remove_at(i);
            break;
        }
    }
    ImGui::Spacing();

    // the new one
    bool enter = false;
    ImGui::SetNextItemWidth(420 * sc);
    enter |= ui_input_text("##newname", g_name, sizeof g_name, ImGuiInputTextFlags_EnterReturnsTrue,
                           tr("Name, e.g. FSR3"), tr("Name of the flag"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(vw);
    enter |= ui_input_text("##newvalue", g_value, sizeof g_value, ImGuiInputTextFlags_EnterReturnsTrue,
                           tr("Value (empty: 1)"), tr("Value"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Add"), ImVec2(bw, 0)) || (enter && g_name[0])) add();

    // what the typed name is, and a warning for the ones that are for developers
    const LfInfo *typed = g_name[0] ? lf_find(g_name) : NULL;
    if (typed) {
        ImGui::PushTextWrapPos(0);
        if (typed->desc[0]) ImGui::TextDisabled("%s", tr(typed->desc));
        if (typed->group == LF_GROUP_DEV)
            ImGui::TextColored(kWarn, "%s", tr("For developers: this can slow the game down or write large files."));
        if (typed->kind == LF_KIND_SWITCH)
            ImGui::TextDisabled("%s", tr("This one is on when it is in the list. To turn it off, remove it."));
        ImGui::PopTextWrapPos();
    }
    if (g_msg[0]) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(g_msg_error ? kBad : kGold, "%s", g_msg);
        ImGui::PopTextWrapPos();
    }

    // suggestions: what matches the typed name (what testers are most often asked for, when nothing is typed)
    const LfInfo *sug[8];
    int n = lf_suggest(g_name, sug, 8);
    if (n > 0 && !(typed && n == 1 && typed == sug[0])) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr(g_name[0] ? "Suggestions" : "Often asked for"));
        for (int i = 0; i < n; i++) {
            ImGui::PushID(sug[i]->name);
            bool dev = sug[i]->group == LF_GROUP_DEV;
            if (dev) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
            if (ImGui::Selectable(sug[i]->name + 11, false)) {  // (without SAVAGEWIND_, as people type it)
                snprintf(g_name, sizeof g_name, "%s", sug[i]->name);
                snprintf(g_value, sizeof g_value, "%s", sug[i]->example);
                g_msg[0] = 0;
            }
            if (dev) ImGui::PopStyleColor();
            if (sug[i]->desc[0]) {
                ImGui::SameLine(0, 14 * sc);
                ImGui::TextDisabled("%s", tr(sug[i]->desc));
            }
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}
