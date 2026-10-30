// Achievements in the overlay: toasts and the menu page (runtime/ui/achievements_ui.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <float.h>
#include <math.h>

#include <algorithm>
#include <deque>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"  // (ImTextCharFromUtf8)
#include "../achievements.h"
#include "achievements_ui.h"
#include "ach_icon.h"

namespace {
// A panel slides down from the top edge of the screen (centered), holds, and slides back up the same way.
const double TOAST_SECONDS = 5.5, SLIDE_IN = 0.45, SLIDE_OUT = 0.45, SHINE_AT = 0.35, SHINE_LEN = 0.9;
struct Toast { int index; double start; };
std::deque<Toast> shown;  // render thread only
const ImVec4 GOLD(0.93f, 0.76f, 0.33f, 1), GREY(0.62f, 0.62f, 0.60f, 1);
ImFont *g_title_font, *g_body_font;  // (ach_ui_fonts)

void pull_new(double now) {
    int i;
    bool any = false, last = false;
    // SAVAGEWIND_ACH_DEMO=<seconds>[:<id>,<id>...]: then the toast of those achievements (the first one's without
    // ids), to see them
    static double demo = -1;
    static std::deque<int> demo_ids;
    if (demo < 0) {
        const char *v = getenv("SAVAGEWIND_ACH_DEMO"), *ids = v ? strchr(v, ':') : NULL;
        demo = v ? atof(v) : 0;
        for (const char *p = ids; p && *p;) {
            const char *q = strchr(++p, ',');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            for (int k = 0; k < g_ach_count; k++)
                if (strlen(g_ach_defs[k].id) == n && !strncmp(g_ach_defs[k].id, p, n)) demo_ids.push_back(k);
            p = q;
        }
        if (demo > 0 && demo_ids.empty()) demo_ids.push_back(0);
    }
    bool demo_due = demo > 0 && now >= demo;
    while (shown.size() < 3 && ((demo_due && !demo_ids.empty()) || ach_toast_take(&i))) {  // (at most 3 at once)
        if (demo_due && !demo_ids.empty()) i = demo_ids.front(), demo_ids.pop_front(), fprintf(stderr, "[achievements] demo toast\n");
        shown.push_back({i, now});
        any = true;
        last |= i >= 0 && i < g_ach_count && g_ach_defs[i].kind == ACH_META;
    }
    if (last) ach_sound_play_final();  // (once for the batch: the last achievement's own, else the game's "Yahaha!")
    else if (any) ach_sound_play();
}
float ease_out(float t) { t = 1 - t; return 1 - t * t * t; }
float ease_in(float t) { return t * t * t; }
ImU32 col(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

// The header in capitals (ASCII and the Latin-1 letters: "Logro obtenido" -> "LOGRO OBTENIDO"), its letters spread
std::string upper(const char *s) {
    std::string u(s);
    for (size_t k = 0; k < u.size(); k++) {
        unsigned char c = u[k];
        if (c >= 'a' && c <= 'z') u[k] = (char)(c - 32);
        else if (c == 0xc3 && k + 1 < u.size() && (unsigned char)u[k + 1] >= 0xa0 && (unsigned char)u[k + 1] != 0xb7 &&
                 (unsigned char)u[k + 1] <= 0xbe) u[k + 1] = (char)((unsigned char)u[k + 1] - 0x20), k++;
    }
    return u;
}
float spaced_width(ImFont *f, float size, const char *s, float sp) {
    float w = 0;
    int n = 0;
    for (const char *p = s; *p; n++) {
        unsigned c;
        const char *q = p;
        p += ImTextCharFromUtf8(&c, p, NULL);
        w += f->CalcTextSizeA(size, FLT_MAX, 0, q, p).x;
    }
    return n ? w + sp * (n - 1) : 0;
}
void spaced_text(ImDrawList *dl, ImFont *f, float size, ImVec2 at, ImU32 c, const char *s, float sp) {
    for (const char *p = s; *p;) {
        unsigned ch;
        const char *q = p;
        p += ImTextCharFromUtf8(&ch, p, NULL);
        dl->AddText(f, size, at, c, q, p);
        at.x += f->CalcTextSizeA(size, FLT_MAX, 0, q, p).x + sp;
    }
}
}  // namespace

void ach_ui_fonts(ImFont *title, ImFont *body) { g_title_font = title, g_body_font = body; }

bool ach_ui_active(void) { return !shown.empty() || ach_toast_pending() > 0; }

void ach_ui_toasts(float display_w, float display_h, float scale) {
    (void)display_h;
    const float SIZE = 0.92f;  // (the whole panel: its measures and its text)
    scale *= SIZE;
    double now = ImGui::GetTime();
    while (!shown.empty() && now - shown.front().start > TOAST_SECONDS) shown.pop_front();
    pull_new(now);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont(), *tf = g_title_font ? g_title_font : font, *bf = g_body_font ? g_body_font : font;
    const float base = ImGui::GetFontSize() * SIZE;
    const float s_head = base * 0.60f, s_name = base * 1.30f, s_desc = base * 0.74f, spread = 2.4f * scale;
    float y = 16 * scale;  // (resting top of the first panel)
    for (size_t k = 0; k < shown.size(); k++) {
        AchInfo in;
        ach_info(shown[k].index, &in);
        if (!in.def) continue;
        std::string head = upper(tr("Achievement Obtained"));
        const char *name = tr(in.def->name), *desc = tr(in.def->desc);
        float hw = spaced_width(bf, s_head, head.c_str(), spread), hh = bf->CalcTextSizeA(s_head, FLT_MAX, 0, "A").y;
        ImVec2 sn = tf->CalcTextSizeA(s_name, FLT_MAX, 0, name), sd = bf->CalcTextSizeA(s_desc, FLT_MAX, 0, desc);
        ImTextureID pic = ach_icon(in.def->id);  // (the player's picture for it, if there is one)
        float pad = 20 * scale, icon = 66 * scale, gap = 20 * scale, rounding = 10 * scale, bar = 3 * scale;
        float text_w = std::max(hw, std::max(sn.x, sd.x));
        float w = std::max(pad + icon + gap + text_w + pad * 1.5f, 440 * scale);
        float h = std::max(pad + hh + 5 * scale + sn.y + 2 * scale + sd.y + pad + bar, icon + 2 * pad + bar);

        // the slide: down into place when it appears, the same movement backwards when it leaves; it fades with it
        double age = now - shown[k].start, left = TOAST_SECONDS - age;
        float slide = age < SLIDE_IN ? ease_out((float)(age / SLIDE_IN)) : left < SLIDE_OUT ? ease_out((float)(left / SLIDE_OUT)) : 1.0f;
        float a = std::min(1.0f, slide * 1.4f);
        float rest = y;                                    // where this panel stops
        float y0 = rest - (h + rest) * (1.0f - slide);     // above the screen at first (slide 0), then at `rest`
        float x0 = (display_w - w) * 0.5f, x1 = x0 + w, y1 = y0 + h;

        // a soft shadow under it
        for (int i = 1; i <= 6; i++) {
            float e = i * 2.5f * scale;
            dl->AddRectFilled(ImVec2(x0 - e, y0 - e + 4 * scale), ImVec2(x1 + e, y1 + e + 4 * scale), col(0, 0, 0, 0.07f * a), rounding + e);
        }
        // the panel: deep teal-black, lighter at the top, a gold edge on the left, a thin gold outline and an inner line
        ImU32 gold = col(0.95f, 0.78f, 0.36f, a);
        float th = std::max(1.0f, 1.3f * scale);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(0.03f, 0.06f, 0.07f, 0.94f * a), rounding);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h * 0.5f), col(0.30f, 0.55f, 0.58f, 0.09f * a), rounding, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + 5 * scale, y1), gold, rounding, ImDrawFlags_RoundCornersLeft);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col(0.95f, 0.78f, 0.36f, 0.65f * a), rounding, 0, th);
        float in4 = 5 * scale;
        dl->AddRect(ImVec2(x0 + in4 + 3 * scale, y0 + in4), ImVec2(x1 - in4, y1 - in4), col(0.95f, 0.78f, 0.36f, 0.14f * a), rounding * 0.6f, 0, th);

        // the emblem: a glowing gold ring, the player's picture in it or a triangle of three triangles
        float cx = x0 + pad + 6 * scale + icon * 0.5f, cy = y0 + (h - bar) * 0.5f, r = icon * 0.5f;
        float pulse = 0.5f + 0.5f * sinf((float)age * 3.0f);
        for (int i = 1; i <= 5; i++) dl->AddCircleFilled(ImVec2(cx, cy), r + i * 2.6f * scale, col(0.95f, 0.78f, 0.36f, (0.035f + 0.02f * pulse) * a), 48);
        dl->AddCircleFilled(ImVec2(cx, cy), r, col(0.05f, 0.09f, 0.10f, a), 48);
        if (pic) {
            float ir = r - 4 * scale;
            dl->AddImageRounded(pic, ImVec2(cx - ir, cy - ir), ImVec2(cx + ir, cy + ir), ImVec2(0, 0), ImVec2(1, 1), col(1, 1, 1, a), ir);
        } else {
            float t = r * 0.42f, hgt = t * 0.866f;  // (each small triangle: side t)
            auto tri = [&](float bx, float by) {  // (bx, by: its bottom left corner)
                dl->AddTriangleFilled(ImVec2(bx, by), ImVec2(bx + t, by), ImVec2(bx + t * 0.5f, by - hgt), gold);
            };
            float bx = cx - t, by = cy + hgt;
            tri(bx, by), tri(bx + t, by), tri(bx + t * 0.5f, by - hgt);
        }
        dl->AddCircle(ImVec2(cx, cy), r, gold, 48, 2 * th);

        // the text: the header in spread gold capitals, the name in the serif, the description below
        float tx = cx + r + gap, ty = y0 + pad;
        spaced_text(dl, bf, s_head, ImVec2(tx, ty), gold, head.c_str(), spread);
        float ny = ty + hh + 5 * scale;
        dl->AddText(tf, s_name, ImVec2(tx + 2 * scale, ny + 2 * scale), col(0, 0, 0, 0.7f * a), name);  // (shadow)
        dl->AddText(tf, s_name, ImVec2(tx, ny), col(1, 0.98f, 0.93f, a), name);
        dl->AddText(bf, s_desc, ImVec2(tx, ny + sn.y + 2 * scale), col(0.74f, 0.79f, 0.77f, a), desc);

        // a light that sweeps across it once it is in, and the time it has left as a gold line along the bottom
        dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
        float sp = (float)((age - SHINE_AT) / SHINE_LEN);
        if (sp > 0 && sp < 1) {
            float sx = x0 - h + (w + 2 * h) * ease_out(sp), bw = 46 * scale;
            dl->AddQuadFilled(ImVec2(sx, y0), ImVec2(sx + bw, y0), ImVec2(sx + bw - h * 0.6f, y1), ImVec2(sx - h * 0.6f, y1),
                              col(1, 0.95f, 0.8f, 0.10f * a * sinf(sp * 3.14159f)));
        }
        float life = std::clamp((float)(left / TOAST_SECONDS), 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(x0 + rounding, y1 - bar - 2 * scale), ImVec2(x0 + rounding + (w - 2 * rounding) * life, y1 - 2 * scale),
                          col(0.95f, 0.78f, 0.36f, 0.55f * a), bar);
        dl->PopClipRect();
        y = rest + h + 12 * scale;  // (the next one rests below this one, whatever this one is doing)
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
