// Benchmark page in the F1 menu and live top-right progress HUD (runtime/benchmark.h, runtime/ui/menu.mm).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"
#include "ui_theme.h"
#include "benchmark_ui.h"

extern "C" {
#include <SDL3/SDL.h>
#include "../benchmark.h"
#include "../control.h"
#include "../framepace.h"
}

namespace {

void kpi_card(const char *id, const char *label, const char *value, ImVec4 val_col, float w, float u) {
    ImGui::BeginChild(id, ImVec2(w, 64 * u), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("%s", label);
    ImGui::SetWindowFontScale(1.22f);
    ImGui::TextColored(val_col, "%s", value);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
}

void draw_frametime_graph(const BenchRouteStats *r, float scale) {
    if (!r || !r->valid || r->spark_n <= 0) return;
    float w = ImGui::GetContentRegionAvail().x;
    if (w < 120 * scale) w = 120 * scale;
    float h = 130 * scale;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::Dummy(ImVec2(w, h));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 20, 18, 220), 6.0f * scale);
    dl->AddRect(p0, p1, IM_COL32(70, 90, 82, 180), 6.0f * scale);

    float max_ms = (float)r->max_ms;
    if (max_ms < 40.0f) max_ms = 40.0f;
    if (max_ms > 100.0f) max_ms = 100.0f;

    struct RefLine { float ms; const char *lbl; ImU32 col; };
    const RefLine refs[] = {
        {33.333f, "30 FPS (33.3 ms)", IM_COL32(220, 90, 80, 140)},
        {16.667f, "60 FPS (16.7 ms)", IM_COL32(230, 195, 75, 140)},
        {8.333f,  "120 FPS (8.3 ms)", IM_COL32(115, 205, 105, 140)},
    };
    for (const RefLine &rf : refs) {
        if (rf.ms >= max_ms) continue;
        float y = p1.y - (rf.ms / max_ms) * (h - 8 * scale) - 4 * scale;
        dl->AddLine(ImVec2(p0.x + 2, y), ImVec2(p1.x - 2, y), rf.col, 1.0f);
        dl->AddText(ImVec2(p0.x + 6 * scale, y - ImGui::GetFontSize() * 0.95f), rf.col, rf.lbl);
    }

    int n = r->spark_n;
    float dx = n > 1 ? (w - 8 * scale) / (float)(n - 1) : 0.0f;
    for (int i = 1; i < n; i++) {
        float v0 = r->spark_ms[i - 1], v1 = r->spark_ms[i];
        if (v0 > max_ms) v0 = max_ms;
        if (v1 > max_ms) v1 = max_ms;
        ImVec2 a(p0.x + 4 * scale + (float)(i - 1) * dx, p1.y - (v0 / max_ms) * (h - 8 * scale) - 4 * scale);
        ImVec2 b(p0.x + 4 * scale + (float)i * dx, p1.y - (v1 / max_ms) * (h - 8 * scale) - 4 * scale);
        ImU32 lc = v1 > 33.333f ? IM_COL32(235, 95, 85, 255) : v1 > 16.667f ? IM_COL32(235, 200, 85, 255) : IM_COL32(125, 215, 115, 255);
        dl->AddLine(a, b, lc, 1.8f * scale);
    }
}

}  // namespace

void bench_ui_tab(bool *want_close) {
    static int mode = BENCH_MODE_FULL;
    static int single_route = 0;
    static int dur_idx = 1;     // 10 s
    static int settle_idx = 1;  // 5 s
    static bool uncap = true;
    static int selected_row = -1;  // -1 = overall
    static char notice[280];

    float scale = ImGui::GetFontSize() / 34.0f;
    if (scale < 0.45f) scale = 0.45f;
    BenchSnapshot snap;
    bench_snapshot(&snap);

    opt_begin("##benchmark", scale);

    float pos[3];
    bool in_world = control_tp_link_pos(pos) && !framepace_title_ui() && !framepace_loading_ui();
    bool in_pause = in_world && framepace_menu_ui();
    bool can_start = in_world && !in_pause;
    if (!in_world && !snap.has_results) {
        ImGui::TextDisabled("%s", tr("Load a save and enter the world to run the benchmark."));
        opt_end();
        return;
    }

    opt_section(tr("Suite setup"));
    static const char *const suite_names[] = {
        "Full suite (7 routes)", "Quick suite (3 routes)", "Single route", "Current spot",
    };
    opt_choice(tr("Suite"), &mode, suite_names, 4, BENCH_MODE_FULL,
               tr("Full suite runs all 7 canonical views across Hyrule. Quick suite measures Plateau, Kakariko and Korok Forest. Current spot samples where Link is standing now."));

    if (mode == BENCH_MODE_SINGLE) {
        const char *rnames[BENCH_ROUTE_COUNT];
        for (int i = 0; i < BENCH_ROUTE_COUNT; i++) {
            const BenchRouteInfo *ri = bench_route_info(i);
            rnames[i] = ri ? tr(ri->name) : "";
        }
        opt_choice(tr("Route"), &single_route, rnames, BENCH_ROUTE_COUNT, 0,
                   tr("Which fixed camera route to measure."));
    }

    static const char *const dur_names[] = {"5 s", "10 s", "15 s", "20 s"};
    static const int dur_vals[] = {5, 10, 15, 20};
    opt_choice(tr("Sample duration"), &dur_idx, dur_names, 4, 1,
               tr("How long each route samples frame times after warming up."));

    static const char *const settle_names[] = {"3 s", "5 s", "8 s"};
    static const int settle_vals[] = {3, 5, 8};
    opt_choice(tr("Warm-up"), &settle_idx, settle_names, 3, 1,
               tr("Settle window after teleporting and aiming before frame capture begins."));

    opt_toggle(tr("Uncap FPS and VSync"), &uncap, true,
               tr("Temporarily turns off Frame rate limit, VSync and Frame generation during measurement, then restores your settings afterwards."));

    bool active = snap.phase >= BENCH_TELEPORTING && snap.phase <= BENCH_RESTORING;
    if (in_pause && !active) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f), "%s", tr("Close the pause menu to run the benchmark."));
    }
    if (!can_start) opt_disable_next();
    if (opt_action(tr("Start benchmark"),
                   active ? tr("Running...") : (can_start ? tr("Start") : (in_pause ? tr("In play only") : tr("In world only"))),
                   tr("Runs the selected benchmark suite and returns Link cleanly to his starting position with full results."), true)) {
        if (!active && can_start) {
            BenchConfig cfg = {mode, single_route, dur_vals[dur_idx], settle_vals[settle_idx], uncap};
            if (bench_start(&cfg)) {
                notice[0] = 0;
                selected_row = -1;
                if (want_close) *want_close = true;
            }
        }
    }

    if (snap.phase == BENCH_CANCELLED && !snap.has_results) {
        ImGui::TextDisabled("%s", tr("Benchmark cancelled."));
    }

    if (snap.has_results) {
        opt_section(tr("Results"));
        if (snap.phase == BENCH_CANCELLED)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f), "%s", tr("Partial results (cancelled early):"));

        const BenchRouteStats *kpi = snap.overall.valid ? &snap.overall : &snap.results[0];
        if (kpi && kpi->valid) {
            float avail = ImGui::GetContentRegionAvail().x;
            float gap = ImGui::GetStyle().ItemSpacing.x;
            float cw = (avail - gap * 4.0f) / 5.0f;
            if (cw < 80 * scale) cw = 80 * scale;
            char v_avg[24], v_1p[24], v_01p[24], v_hit[24], v_vram[24];
            snprintf(v_avg, sizeof v_avg, "%.1f", kpi->avg_fps);
            snprintf(v_1p, sizeof v_1p, "%.1f", kpi->low_1pct_fps);
            snprintf(v_01p, sizeof v_01p, "%.1f", kpi->low_01pct_fps);
            snprintf(v_hit, sizeof v_hit, "%u", kpi->hitches);
            if (kpi->vram_peak_mb > 0) snprintf(v_vram, sizeof v_vram, "%.0f MB", kpi->vram_peak_mb);
            else snprintf(v_vram, sizeof v_vram, "-");

            kpi_card("##k1", tr("Avg FPS"), v_avg, ImVec4(0.55f, 0.88f, 0.38f, 1.0f), cw, scale);
            ImGui::SameLine();
            kpi_card("##k2", tr("1% Low"), v_1p, ImVec4(0.92f, 0.80f, 0.32f, 1.0f), cw, scale);
            ImGui::SameLine();
            kpi_card("##k3", tr("0.1% Low"), v_01p, ImVec4(0.90f, 0.56f, 0.32f, 1.0f), cw, scale);
            ImGui::SameLine();
            kpi_card("##k4", tr("Hitches"), v_hit, kpi->hitches ? ImVec4(0.92f, 0.38f, 0.35f, 1.0f) : ImVec4(0.55f, 0.88f, 0.38f, 1.0f), cw, scale);
            ImGui::SameLine();
            kpi_card("##k5", tr("Peak VRAM"), v_vram, ImVec4(0.38f, 0.72f, 0.92f, 1.0f), cw, scale);
        }

        if (snap.gpu_name[0] || snap.width > 0) {
            ImGui::TextDisabled("%s | %dx%d | %s", snap.gpu_name[0] ? snap.gpu_name : "GPU", snap.width, snap.height, snap.settings_summary);
        }

        ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##bench_tbl", 8, tf)) {
            ImGui::TableSetupColumn(tr("Route"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn(tr("Avg FPS"));
            ImGui::TableSetupColumn(tr("1% Low"));
            ImGui::TableSetupColumn(tr("0.1% Low"));
            ImGui::TableSetupColumn(tr("Avg ms"));
            ImGui::TableSetupColumn(tr("p99 ms"));
            ImGui::TableSetupColumn(tr("Max ms"));
            ImGui::TableSetupColumn(tr("Hitches"));
            ImGui::TableHeadersRow();

            for (int i = 0; i < snap.nresults && i < BENCH_MAX_RESULTS; i++) {
                const BenchRouteStats *r = &snap.results[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(r->name[0] ? r->name : r->id, selected_row == i, ImGuiSelectableFlags_SpanAllColumns)) selected_row = i;
                ImGui::PopID();
                if (!r->valid) {
                    ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.92f, 0.38f, 0.35f, 1.0f), "%s", r->fail_reason[0] ? r->fail_reason : tr("Failed"));
                    for (int c = 0; c < 6; c++) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                    continue;
                }
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->avg_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->low_1pct_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->low_01pct_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->avg_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->p99_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->max_ms);
                ImGui::TableNextColumn(); ImGui::Text("%u", r->hitches);
            }
            if (snap.overall.valid && snap.nresults > 1) {
                const BenchRouteStats *r = &snap.overall;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(-1);
                if (ImGui::Selectable(tr("Overall"), selected_row == -1, ImGuiSelectableFlags_SpanAllColumns)) selected_row = -1;
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->avg_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->low_1pct_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->low_01pct_fps);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->avg_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", r->p99_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", r->max_ms);
                ImGui::TableNextColumn(); ImGui::Text("%u", r->hitches);
            }
            ImGui::EndTable();
        }

        const BenchRouteStats *graph_r = (selected_row >= 0 && selected_row < snap.nresults)
                                       ? &snap.results[selected_row]
                                       : (snap.overall.valid ? &snap.overall : &snap.results[0]);
        if (graph_r && graph_r->valid) {
            ImGui::TextDisabled("%s: %s (%.1f - %.1f ms, %s %.2f ms)",
                                tr("Frame time"), graph_r->name[0] ? graph_r->name : tr("Overall"),
                                graph_r->min_ms, graph_r->max_ms, tr("median"), graph_r->median_ms);
            draw_frametime_graph(graph_r, scale);
        }

        if (opt_action(tr("Copy Markdown report"), tr("Copy"),
                       tr("Copies a formatted Markdown summary table of all benchmark routes and system info to the clipboard."), true)) {
            char md[4096];
            bench_format_markdown(&snap, md, sizeof md);
            SDL_SetClipboardText(md);
            snprintf(notice, sizeof notice, "%s", tr("Copied Markdown benchmark report to clipboard."));
        }
        if (opt_action(tr("Save CSV report"), tr("Save"),
                       tr("Saves detailed per-route metrics to a timestamped CSV file inside the benchmarks folder."), true)) {
            char path[600];
            if (bench_save_csv(&snap, path, sizeof path)) {
                snprintf(notice, sizeof notice, "%s: %s", tr("Saved CSV"), path);
            } else {
                snprintf(notice, sizeof notice, "%s", tr("Could not save CSV file."));
            }
        }
        if (notice[0]) ImGui::TextDisabled("%s", notice);
    }

    opt_end();
}

void bench_ui_overlay(float display_w, float display_h, float scale) {
    (void)display_h;
    BenchSnapshot snap;
    bench_snapshot(&snap);
    if (snap.phase < BENCH_TELEPORTING || snap.phase > BENCH_RESTORING) return;

    ImGui::SetNextWindowPos(ImVec2(display_w - 16 * scale, 16 * scale), 0, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 18 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 8 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.08f, 0.07f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.55f, 0.88f, 0.38f, 1.0f));
    struct PopGuard { ~PopGuard() { ImGui::PopStyleColor(2); ImGui::PopStyleVar(3); } } pop_guard;

    ImGui::Begin("##bench_hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.76f);

    if (snap.phase == BENCH_RESTORING) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "%s", tr("BENCHMARK"));
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.88f, 0.38f, 1.0f), "%s  %d / %d",
                           tr("BENCHMARK"), snap.route_idx + 1, snap.route_total > 0 ? snap.route_total : 1);
    }
    ImGui::SameLine();
    ImGui::Text("- %s", snap.current_route_name[0] ? snap.current_route_name : "");

    const char *phase_lbl = tr("Preparing...");
    switch (snap.phase) {
    case BENCH_TELEPORTING: phase_lbl = tr("Teleporting..."); break;
    case BENCH_AIMING:      phase_lbl = tr("Aiming camera..."); break;
    case BENCH_SETTLING:    phase_lbl = tr("Warming up..."); break;
    case BENCH_MEASURING:   phase_lbl = tr("Measuring..."); break;
    case BENCH_RESTORING:   phase_lbl = tr("Restoring position..."); break;
    default: break;
    }

    float frac = 0.0f;
    if (snap.phase == BENCH_SETTLING || snap.phase == BENCH_MEASURING || snap.phase == BENCH_AIMING) {
        if (snap.phase_target_s > 0.0) frac = (float)(snap.phase_elapsed_s / snap.phase_target_s);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        ImGui::Text("%s  %.1f / %.0f s", phase_lbl, snap.phase_elapsed_s, snap.phase_target_s);
    } else {
        ImGui::Text("%s  (%.1f s)", phase_lbl, snap.phase_elapsed_s);
        frac = (float)fmod(snap.phase_elapsed_s * 0.5, 1.0);
    }
    ImGui::ProgressBar(frac, ImVec2(290 * scale, 10 * scale), "");

    ImGui::Text("%.0f FPS   %.1f ms", snap.live_fps, snap.live_ms);
    if (snap.live_spark_n > 1) {
        float top = 40.0f;
        for (int i = 0; i < snap.live_spark_n; i++)
            if (snap.live_spark[i] > top) top = snap.live_spark[i];
        if (top > 100.0f) top = 100.0f;
        ImGui::PlotLines("##bhud_plot", snap.live_spark, snap.live_spark_n, 0, NULL, 0.0f, top, ImVec2(290 * scale, 36 * scale));
    }
    ImGui::TextDisabled("%s", tr("Press Esc to cancel"));
    ImGui::End();
}
