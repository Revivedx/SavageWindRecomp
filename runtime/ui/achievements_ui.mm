// Achievements in the overlay: toasts and the menu page (runtime/ui/achievements_ui.h).
#include <stdio.h>
#include <time.h>

#include <float.h>

#include <algorithm>
#include <deque>

#include "i18n.h"
#include "imgui.h"
#include "../achievements.h"
#include "achievements_ui.h"
#include "ach_icon.h"

namespace {
// A panel slides down from the top edge of the screen (centered), holds, and slides back up the same way.
const double TOAST_SECONDS = 5.5, SLIDE_IN = 0.45, SLIDE_OUT = 0.45;
struct Toast { int index; double start; };
std::deque<Toast> shown;  // render thread only
const ImVec4 GOLD(0.93f, 0.76f, 0.33f, 1), GREY(0.62f, 0.62f, 0.60f, 1);

void pull_new(double now) {
    int i;
    bool any = false;
    while (shown.size() < 3 && ach_toast_take(&i)) shown.push_back({i, now}), any = true;  // (at most 3 at once; the rest wait)
    if (any) ach_sound_play();  // (once for the batch: the game's "Yahaha!")
}
float ease_out(float t) { t = 1 - t; return 1 - t * t * t; }
float ease_in(float t) { return t * t * t; }
ImU32 col(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
}  // namespace

bool ach_ui_active(void) { return !shown.empty() || ach_toast_pending() > 0; }

void ach_ui_toasts(float display_w, float display_h, float scale) {
    (void)display_h;
    double now = ImGui::GetTime();
    while (!shown.empty() && now - shown.front().start > TOAST_SECONDS) shown.pop_front();
    pull_new(now);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    const float s_head = base * 0.78f, s_name = base * 1.45f, s_desc = base * 0.80f;
    float y = 14 * scale;  // (resting top of the first panel)
    for (size_t k = 0; k < shown.size(); k++) {
        AchInfo in;
        ach_info(shown[k].index, &in);
        if (!in.def) continue;
        const char *head = tr("Achievement Obtained"), *name = tr(in.def->name), *desc = tr(in.def->desc);
        ImVec2 sh = font->CalcTextSizeA(s_head, FLT_MAX, 0, head), sn = font->CalcTextSizeA(s_name, FLT_MAX, 0, name),
               sd = font->CalcTextSizeA(s_desc, FLT_MAX, 0, desc);
        ImTextureID pic = ach_icon(in.def->id);  // (the player's picture for it, if there is one)
        float pad = 18 * scale, icon = (pic ? 46 : 34) * scale, gap = 14 * scale, rounding = 12 * scale;
        float text_w = std::max(sh.x, std::max(sn.x, sd.x));
        float w = pad + icon + gap + text_w + pad * 1.6f;
        float h = pad + sh.y + 4 * scale + sn.y + 3 * scale + sd.y + pad;

        // the slide: down into place when it appears, the same movement backwards when it leaves
        double age = now - shown[k].start, left = TOAST_SECONDS - age;
        float slide = age < SLIDE_IN ? ease_out((float)(age / SLIDE_IN)) : left < SLIDE_OUT ? ease_out((float)(left / SLIDE_OUT)) : 1.0f;
        float rest = y;                                    // where this panel stops
        float y0 = rest - (h + rest) * (1.0f - slide);     // above the screen at first (slide 0), then at `rest`
        float x0 = (display_w - w) * 0.5f, x1 = x0 + w, y1 = y0 + h;

        // the panel: dark, rounded, with a thin gold outline
        ImU32 dark = col(0.04f, 0.04f, 0.05f, 0.90f), gold = col(0.93f, 0.76f, 0.33f, 1.0f);
        float th = std::max(1.0f, 1.5f * scale);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), dark, rounding);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col(0.93f, 0.76f, 0.33f, 0.85f), rounding, 0, th);
        if (pic) {  // the picture, in its own rounded square
            float ix = x0 + pad, iy = (y0 + y1) * 0.5f - icon * 0.5f;
            dl->AddImageRounded(pic, ImVec2(ix, iy), ImVec2(ix + icon, iy + icon), ImVec2(0, 0), ImVec2(1, 1), col(1, 1, 1, 1), 6 * scale);
        } else {
        // a diamond as the emblem
        float cx = x0 + pad + icon * 0.5f, cy = (y0 + y1) * 0.5f, r = icon * 0.5f;
        dl->AddQuadFilled(ImVec2(cx, cy - r), ImVec2(cx + r, cy), ImVec2(cx, cy + r), ImVec2(cx - r, cy), col(0.93f, 0.76f, 0.33f, 0.18f));
        dl->AddQuad(ImVec2(cx, cy - r), ImVec2(cx + r, cy), ImVec2(cx, cy + r), ImVec2(cx - r, cy), gold, th);
        float r2 = r * 0.45f;
        dl->AddQuadFilled(ImVec2(cx, cy - r2), ImVec2(cx + r2, cy), ImVec2(cx, cy + r2), ImVec2(cx - r2, cy), gold);
        }
        // the text
        float tx = x0 + pad + icon + gap, ty = y0 + pad;
        dl->AddText(font, s_head, ImVec2(tx, ty), gold, head);
        dl->AddText(font, s_name, ImVec2(tx + 1, ty + sh.y + 4 * scale + 1), col(0, 0, 0, 0.6f), name);  // (shadow)
        dl->AddText(font, s_name, ImVec2(tx, ty + sh.y + 4 * scale), col(1, 1, 1, 1), name);
        dl->AddText(font, s_desc, ImVec2(tx, ty + sh.y + 4 * scale + sn.y + 3 * scale), ImGui::ColorConvertFloat4ToU32(GREY), desc);
        y = rest + h + 10 * scale;  // (the next one rests below this one, whatever this one is doing)
    }
}

void ach_ui_tab(void) {
    int total = g_ach_count;
    ImGui::TextColored(GOLD, "%s  %d / %d", tr("Achievements unlocked"), ach_unlocked_total(), total);
    ImGui::TextColored(GREY, "%s", tr("They unlock when the game saves. Each game has its own and they're only announced once: a new game starts from zero."));
    ImGui::Spacing();
    for (int i = 0; i < total; i++) {
        AchInfo in;
        ach_info(i, &in);
        if (!in.def) continue;
        ImGui::PushID(i);
        ImGui::Separator();
        ImTextureID pic = ach_icon(in.def->id);
        float side = ImGui::GetFontSize() * 3.2f;
        if (pic) {  // the picture on the left (dimmed while locked), the text in a group on its right
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(side, side));
            ImGui::GetWindowDrawList()->AddImageRounded(pic, p, ImVec2(p.x + side, p.y + side), ImVec2(0, 0), ImVec2(1, 1),
                                                        in.unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 255), 6.0f);
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        ImGui::TextColored(in.unlocked ? GOLD : GREY, "%s%s", in.unlocked ? "[x] " : "[ ] ", tr(in.def->name));
        ImGui::TextColored(GREY, "    %s", tr(in.def->desc));
        if (in.unlocked && in.at > 0) {
            char when[64] = "";
            time_t t = in.at;
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
            ImGui::TextColored(GREY, "    %s %s", tr("Unlocked"), when);
        } else if (!in.unlocked && in.target > 1) {
            char buf[32];
            snprintf(buf, sizeof buf, "%u / %u", in.cur, in.target);
            ImGui::ProgressBar(in.target ? (float)in.cur / (float)in.target : 0.0f, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), buf);
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
}
