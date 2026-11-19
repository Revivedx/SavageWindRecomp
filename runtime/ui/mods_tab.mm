// The Mods tab (Settings, menu.mm tab 11): lists installed mod folders, toggles enabled state, reorders priority,
// and reports overlapping romfs files between enabled mods.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "i18n.h"
#include "imgui.h"
#include "mods_tab.h"
#include "option_rows.h"

extern "C" {
#include "../mods.h"
}
void reveal_folder(const std::string &dir);  // launcher.mm: Explorer / Finder / the file manager

namespace {

ModList g_list = {};
bool g_has_list = false;
std::string g_selected_id;
bool g_dirty = true;
int g_last_frame = -100;
bool g_atexit_registered = false;

std::string fmt(const char *p, ...) {
    va_list ap;
    va_start(ap, p);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, p, ap);
    va_end(ap);
    if (n <= 0) {
        va_end(ap2);
        return "";
    }
    std::string out((size_t)n, '\0');
    vsnprintf(&out[0], (size_t)n + 1, p, ap2);
    va_end(ap2);
    return out;
}

void free_cached_list(void) {
    mods_list_free(&g_list);
    g_has_list = false;
}

int find_selected_index(void) {
    if (!g_has_list || g_list.count <= 0 || g_selected_id.empty()) return -1;
    for (int i = 0; i < g_list.count; i++) {
        if (strcmp(g_list.items[i].id, g_selected_id.c_str()) == 0) return i;
    }
    return -1;
}

void register_mod_glyphs(const ModEntry &m) {
    ui_register_glyphs(m.id);
    ui_register_glyphs(m.name);
    if (m.version[0]) ui_register_glyphs(m.version);
    if (m.author[0]) ui_register_glyphs(m.author);
    if (m.description[0]) ui_register_glyphs(m.description);
    if (m.rel_dir[0]) ui_register_glyphs(m.rel_dir);
    for (int s = 0; s < m.sample_count; s++) {
        if (m.samples[s][0]) ui_register_glyphs(m.samples[s]);
    }
}

void rescan_mods(void) {
    if (!g_atexit_registered) {
        atexit(free_cached_list);
        g_atexit_registered = true;
    }
    mods_list_free(&g_list);
    mods_scan(NULL, &g_list, true);
    g_has_list = true;
    g_dirty = false;

    for (int i = 0; i < g_list.count; i++) register_mod_glyphs(g_list.items[i]);

    char env_dir[1024];
    if (mods_env_override(env_dir, sizeof env_dir)) ui_register_glyphs(env_dir);

    if (g_list.count <= 0) {
        g_selected_id.clear();
    } else if (find_selected_index() < 0) {
        g_selected_id = g_list.items[0].id;
    }
}

std::string format_bytes(uint64_t bytes) {
    if (bytes < 1024ULL) return fmt(tr("%llu B"), (unsigned long long)bytes);
    if (bytes < 1024ULL * 1024ULL) return fmt(tr("%.1f KB"), (double)bytes / 1024.0);
    if (bytes < 1024ULL * 1024ULL * 1024ULL) return fmt(tr("%.1f MB"), (double)bytes / (1024.0 * 1024.0));
    return fmt(tr("%.2f GB"), (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

// Truncate only if needed so long mod names don't collide with row values or arrows; pops whole UTF-8 codepoints.
std::string ellipsize_to_width(const std::string &text, float max_w) {
    if (max_w <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= max_w) return text;
    std::string out = text;
    while (!out.empty()) {
        while (!out.empty()) {
            unsigned char c = (unsigned char)out.back();
            out.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x <= max_w) return out + "...";
    }
    return "...";
}

std::string format_mod_desc(const ModEntry &m, int idx, const ModList &list, bool in_game, bool pending_restart) {
    std::string d = m.name[0] ? m.name : m.id;
    if (m.name[0] && strcmp(m.name, m.id) != 0) d += fmt(" (%s)", m.id);

    if (m.enabled) {
        d += "\n" + fmt(tr("Status: Enabled (priority #%d of %d)"), idx + 1, list.count);
    } else {
        d += std::string("\n") + tr("Status: Disabled");
    }

    if (m.version[0]) d += fmt("\n%s: %s", tr("Version"), m.version);
    if (m.author[0]) d += fmt("\n%s: %s", tr("Author"), m.author);

    d += "\n" + fmt(tr("Files: %d (%s)"), m.file_count, format_bytes(m.total_bytes).c_str());
    d += fmt("\n%s: %s", tr("Folder"), m.rel_dir[0] ? m.rel_dir : m.id);

    if (m.enabled) {
        int total_c = m.wins_conflicts + m.loses_conflicts;
        if (total_c == 0) {
            d += std::string("\n") + tr("Conflicts: None with other enabled mods");
        } else {
            if (m.wins_conflicts > 0 && m.loses_conflicts > 0) {
                d += "\n" + fmt(tr("Conflicts: %d (%d overrides lower priority, %d overridden by higher priority)"),
                                total_c, m.wins_conflicts, m.loses_conflicts);
            } else if (m.wins_conflicts > 0) {
                d += "\n" + fmt(tr("Conflicts: Overrides %d file(s) from lower-priority mods"), m.wins_conflicts);
            } else {
                d += "\n" + fmt(tr("Conflicts: %d file(s) overridden by higher-priority mods"), m.loses_conflicts);
            }
            for (int s = 0; s < m.sample_count; s++) {
                if (m.samples[s][0]) d += fmt("\n- %s", m.samples[s]);
            }
            if (total_c > m.sample_count) {
                d += "\n" + fmt(tr("... and %d more"), total_c - m.sample_count);
            }
        }
    }

    if (m.description[0]) {
        d += "\n\n";
        d += m.description;
    }

    d += "\n\n";
    if (!in_game) {
        d += tr("Starts game with these mods when you press Play.");
    } else if (pending_restart) {
        d += tr("Pending change: Choose Restart now above (or close and relaunch) to apply.");
    } else {
        d += tr("Mod changes take effect when the game restarts.");
    }
    return d;
}

}  // namespace

void mods_tab_invalidate(void) { g_dirty = true; }

void mods_tab_draw(bool in_game, bool *want_restart) {
    if (want_restart) *want_restart = false;

    int frame = ImGui::GetFrameCount();
    if (g_dirty || !g_has_list || frame != g_last_frame + 1) rescan_mods();
    g_last_frame = frame;

    opt_begin("##mods", ImGui::GetFontSize() / 34);

    bool pending_restart = in_game && mods_session_pending_restart(NULL);
    if (pending_restart) {
        opt_section("Pending changes");
        if (opt_button("Restart now",
                       "Mod changes apply the next time the game starts. Restarts now with your enabled mods. "
                       "Progress since your last save is lost.")) {
            if (want_restart) *want_restart = true;
        }
    }

    char env_dir[1024];
    if (mods_env_override(env_dir, sizeof env_dir)) {
        opt_section("Launch flag overlay");
        opt_disable_next();
        std::string env_desc = fmt(
            tr("Override folder: %s\n\nThis folder loads at top priority ahead of enabled mods. Edit or remove it "
               "in Prelaunch > Launch options (or your environment)."),
            env_dir);
        opt_action("SAVAGEWIND_ROMFS_OVERLAY", "Active", env_desc.c_str(), false);
    }

    opt_section("Mods");
    // (what Savage Wind takes on for mods and what it doesn't: always the first row)
    opt_disable_next();
    opt_action("About mods", "",
               "Mods are made by other people, not by Savage Wind. We don't check, test or support them, and we aren't "
               "responsible for what they do, including if they break the game or your saves. Back up your saves "
               "before trying one, and turn mods off before reporting a bug: a report of a bug a mod causes is closed right "
               "away. Savage Wind releases never include mods; "
               "in the future we may suggest some we've found to play well with Savage Wind.",
               true);
    if (g_list.count == 0) {
        opt_disable_next();
        opt_action("No mods installed", "",
                   "Put mod folders in the mods folder next to Savage Wind.\n\nSupported layouts:\n"
                   "- mods/<name>/romfs/Actor/... (or Pack, System, etc.)\n- mods/<name>/Actor/... directly\n\n"
                   "Use Open mods folder below, then Refresh mod list.",
                   true);
    } else {
        std::string pending_toggle_id;
        bool pending_toggle_val = false;
        bool has_pending_toggle = false;

        float fs = ImGui::GetFontSize();
        float pad = 16.0f * (fs / 34.0f);
        float avail_w = ImGui::GetContentRegionAvail().x;

        for (int i = 0; i < g_list.count; i++) {
            const ModEntry &m = g_list.items[i];
            bool en = m.enabled;
            int conflicts = m.wins_conflicts + m.loses_conflicts;
            std::string val_str;
            const char *val_override = NULL;
            if (en && conflicts > 0) {
                val_str = fmt("%s (%d!)", tr("On"), conflicts);
                val_override = val_str.c_str();
            }
            const char *shown_val = val_override ? val_override : tr(en ? "On" : "Off");
            float max_label_w = avail_w - 2.0f * pad - ImGui::CalcTextSize(shown_val).x - fs * 0.8f;
            std::string row_label = ellipsize_to_width(fmt("#%d  %s", i + 1, m.name), max_label_w);
            std::string desc = format_mod_desc(m, i, g_list, in_game, pending_restart);

            ImGui::PushID(m.id);
            if (opt_toggle_item(row_label.c_str(), &en, val_override, desc.c_str())) {
                pending_toggle_id = m.id;
                pending_toggle_val = en;
                has_pending_toggle = true;
            }
            ImGui::PopID();
        }

        if (has_pending_toggle) {
            if (mods_set_enabled(NULL, pending_toggle_id.c_str(), pending_toggle_val)) {
                g_selected_id = pending_toggle_id;
                rescan_mods();
                pending_restart = in_game && mods_session_pending_restart(NULL);
            }
        }
    }

    if (g_list.count >= 2) {
        opt_section("Load order");
        int sel_idx = find_selected_index();
        if (sel_idx < 0) sel_idx = 0;
        const ModEntry &sel = g_list.items[sel_idx];
        std::string sel_desc = format_mod_desc(sel, sel_idx, g_list, in_game, pending_restart);

        float fs = ImGui::GetFontSize();
        float pad = 16.0f * (fs / 34.0f);
        float aw = fs * 0.42f, gap = fs * 0.55f;
        std::string sel_lbl = tr_upper(tr("Selected mod"));
        float max_choice_w = ImGui::GetContentRegionAvail().x - 2.0f * pad - ImGui::CalcTextSize(sel_lbl.c_str()).x -
                             2.0f * (aw + gap) - fs * 0.8f;

        std::vector<std::string> choice_strs((size_t)g_list.count);
        std::vector<const char *> choice_ptrs((size_t)g_list.count);
        for (int i = 0; i < g_list.count; i++) {
            choice_strs[i] = ellipsize_to_width(fmt("#%d  %s", i + 1, g_list.items[i].name), max_choice_w);
            choice_ptrs[i] = choice_strs[i].c_str();
        }

        int chosen = sel_idx;
        if (opt_choice_item("Selected mod", &chosen, choice_ptrs.data(), g_list.count, -1, sel_desc.c_str(), true, true)) {
            if (chosen >= 0 && chosen < g_list.count) {
                g_selected_id = g_list.items[chosen].id;
                sel_idx = chosen;
            }
        }

        // Re-read sel in case Selected mod cycler changed sel_idx this frame.
        const ModEntry &cur_sel = g_list.items[sel_idx];
        std::string cur_desc = format_mod_desc(cur_sel, sel_idx, g_list, in_game, pending_restart);

        bool can_up = sel_idx > 0;
        if (!can_up) opt_disable_next();
        if (opt_button("Move higher priority", cur_desc.c_str()) && can_up) {
            std::string move_id = cur_sel.id;
            if (mods_move(NULL, move_id.c_str(), -1)) {
                g_selected_id = move_id;
                rescan_mods();
                pending_restart = in_game && mods_session_pending_restart(NULL);
                sel_idx = find_selected_index();
                if (sel_idx < 0) sel_idx = 0;
            }
        }

        const ModEntry &cur_sel2 = g_list.items[sel_idx];
        std::string cur_desc2 = format_mod_desc(cur_sel2, sel_idx, g_list, in_game, pending_restart);
        bool can_down = sel_idx < g_list.count - 1;
        if (!can_down) opt_disable_next();
        if (opt_button("Move lower priority", cur_desc2.c_str()) && can_down) {
            std::string move_id = cur_sel2.id;
            if (mods_move(NULL, move_id.c_str(), +1)) {
                g_selected_id = move_id;
                rescan_mods();
            }
        }
    }

    opt_section("Actions");
    if (opt_button("Open mods folder",
                   "Opens the mods folder in your file manager. Put mod folders inside it, then choose Refresh mod list.")) {
        SDL_CreateDirectory("mods");
        reveal_folder("mods");
    }

    std::string refresh_desc =
        g_list.count == 0
            ? std::string(tr("Scans the mods folder again for added, removed, or edited mods."))
            : fmt(tr("Enabled: %d of %d, %d %s\n\nScans the mods folder again for added, removed, or edited mods."),
                  g_list.enabled_count, g_list.count, g_list.total_conflicts,
                  tr(g_list.total_conflicts == 1 ? "conflict" : "conflicts"));
    if (opt_button("Refresh mod list", refresh_desc.c_str())) rescan_mods();

    if (g_list.count > 0) {
        bool all_on = g_list.enabled_count >= g_list.count;
        if (all_on) opt_disable_next();
        if (opt_button("Enable all mods", "Enables every installed mod in the current load order.") && !all_on) {
            if (mods_set_all_enabled(NULL, true)) rescan_mods();
        }

        bool all_off = g_list.enabled_count == 0;
        if (all_off) opt_disable_next();
        if (opt_button("Disable all mods",
                       "Disables all installed mods without deleting any mod folders or changing load order.") &&
            !all_off) {
            if (mods_set_all_enabled(NULL, false)) rescan_mods();
        }
    }

    opt_end();
}
