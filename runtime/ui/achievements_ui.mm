// Achievements in the overlay: toasts and the menu page (runtime/ui/achievements_ui.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <float.h>
#include <math.h>

#include <algorithm>
#include <deque>
#include <vector>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"  // (ImTextCharFromUtf8)
#include "../achievements.h"
#include "achievements_ui.h"
#include "ach_icon.h"
#include "quick_hint_font.h"
extern "C" {
#include "../rt.h"  // (g_romfs_dir)
}

namespace {
// A panel in the game's own look (its item-get box and its buttons) fades in near the top of the screen (centered),
// holds, and fades out.
const double TOAST_SECONDS = 5.5, FADE_IN = 0.30, FADE_OUT = 0.40;
struct Toast { int index; double start; };
std::deque<Toast> shown;  // render thread only
const ImVec4 GOLD(0.93f, 0.76f, 0.33f, 1), GREY(0.62f, 0.62f, 0.60f, 1);
ImFont *g_title_font, *g_body_font;  // (ach_ui_fonts)
ImFont *g_game_title, *g_game_body;  // (ach_ui_game_fonts: the game's own, when the romfs has them)

// The game's font when it has every character of s (a language whose letters it lacks: the overlay's font)
ImFont *pick(ImFont *game, ImFont *other, const char *s) {
    if (!game) return other;
    for (unsigned c; *s;) {
        int n = ImTextCharFromUtf8(&c, s, nullptr);
        if (!n) break;
        s += n;
        if (c >= 32 && !game->FindGlyphNoFallback((ImWchar)c)) return other;
    }
    return game;
}

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

// Text slanted as the game's titles are (about 12 degrees: each vertex moved right by its height over the baseline)
void slanted_text(ImDrawList *dl, ImFont *f, float size, ImVec2 at, ImU32 c, const char *s) {
    int first = dl->VtxBuffer.Size;
    dl->AddText(f, size, at, c, s);
    float base = at.y + size * 0.8f;
    for (int i = first; i < dl->VtxBuffer.Size; i++) dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * 0.21f;
}
ImU32 col(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

}  // namespace

void ach_ui_fonts(ImFont *title, ImFont *body) { g_title_font = title, g_body_font = body; }

bool ach_ui_game_fonts(ImFontAtlas *atlas) {
    g_game_title = g_game_body = nullptr;
    if (!g_romfs_dir) return false;
    // the characters the toasts write: ASCII, the Latin-1 letters, and whatever the texts in this language add
    std::vector<unsigned> chars;
    for (unsigned c = 32; c < 127; c++) chars.push_back(c);
    for (unsigned c = 0xa1; c <= 0xff; c++) chars.push_back(c);
    auto add = [&](const char *s) {
        for (unsigned c; s && *s;) {
            int n = ImTextCharFromUtf8(&c, s, nullptr);
            if (!n) break;
            s += n;
            if (c >= 0x100 && c <= 0xffff && std::find(chars.begin(), chars.end(), c) == chars.end()) chars.push_back(c);
        }
    };
    add(tr("Achievement Obtained"));
    for (int i = 0; i < g_ach_count; i++) add(tr(g_ach_defs[i].name)), add(tr(g_ach_defs[i].desc));
    // Normal_00: the game's text; its titles (item names, the option rows) are the same font, slanted (slanted_text)
    static const char *const fonts[] = {"Normal_00.bffnt"};
    ImFont *got[1];
    game_fonts_add(atlas, fonts, 1, chars.data(), (int)chars.size(), got);
    g_game_title = g_game_body = got[0];
    return true;
}

bool ach_ui_active(void) {
    static int demo = -1;  // (SAVAGEWIND_ACH_DEMO: its toasts have to be drawn too, before they are taken; 10 minutes)
    static time_t demo_until;
    if (demo < 0) demo = getenv("SAVAGEWIND_ACH_DEMO") != NULL, demo_until = time(NULL) + 600;
    return !shown.empty() || ach_toast_pending() > 0 || (demo && time(NULL) < demo_until);
}

void ach_ui_toasts(float display_w, float display_h, float scale) {
    (void)display_h;
    double now = ImGui::GetTime();
    while (!shown.empty() && now - shown.front().start > TOAST_SECONDS) shown.pop_front();
    pull_new(now);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont(), *tf = g_title_font ? g_title_font : font, *bf = g_body_font ? g_body_font : font;
    const float base = ImGui::GetFontSize();
    const float s_head = base * 0.62f, s_name = base * 1.12f, s_desc = base * 0.70f;
    float y = 64 * scale;  // (resting top of the first panel: below the screen's edge, as the game's own boxes)
    for (size_t k = 0; k < shown.size(); k++) {
        AchInfo in;
        ach_info(shown[k].index, &in);
        if (!in.def) continue;
        const char *head = tr("Achievement Obtained"), *name = tr(in.def->name), *desc = tr(in.def->desc);
        ImFont *hf = pick(g_game_body, bf, head), *nf = pick(g_game_title, tf, name), *df = pick(g_game_body, bf, desc);
        ImVec2 sh = hf->CalcTextSizeA(s_head, FLT_MAX, 0, head), sn = nf->CalcTextSizeA(s_name, FLT_MAX, 0, name),
               sd = df->CalcTextSizeA(s_desc, FLT_MAX, 0, desc);
        ImTextureID pic = ach_icon(in.def->id);  // (the player's picture for it, if there is one)
        float pad = 16 * scale, icon = 76 * scale, gap = 18 * scale;
        float text_w = std::max(sh.x, std::max(sn.x, sd.x));
        float w = std::max(pad + icon + gap + text_w + pad * 1.6f, 480 * scale);
        float h = std::max(pad + sh.y + 2 * scale + sn.y + 4 * scale + sd.y + pad, icon + 2 * pad);

        // in: a fade, coming down a little; out: a fade
        double age = now - shown[k].start, left = TOAST_SECONDS - age;
        float a = age < FADE_IN ? ease_out((float)(age / FADE_IN)) : left < FADE_OUT ? (float)(left / FADE_OUT) : 1.0f;
        a = std::clamp(a, 0.0f, 1.0f);
        float drop = age < FADE_IN ? (1 - ease_out((float)(age / FADE_IN))) * 10 * scale : 0;
        float x0 = floorf((display_w - w) * 0.5f), x1 = x0 + w, y0 = floorf(y - drop), y1 = y0 + h;

        // the panel: translucent black with square corners, a thin pale frame and a fainter one just inside it (the
        // game's buttons), and short bright marks on its corners (its item box)
        float th = std::max(1.0f, 1.2f * scale), r = 2 * scale;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(0, 0, 0, 0.72f * a), r);
        dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y0 + h * 0.5f), col(1, 1, 1, 0.035f * a),
                                    col(1, 1, 1, 0.035f * a), col(1, 1, 1, 0), col(1, 1, 1, 0));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col(0.86f, 0.85f, 0.78f, 0.55f * a), r, 0, th);
        float in4 = 4 * scale;
        dl->AddRect(ImVec2(x0 + in4, y0 + in4), ImVec2(x1 - in4, y1 - in4), col(0.86f, 0.85f, 0.78f, 0.16f * a), 0, 0, th);
        ImU32 mark = col(0.97f, 0.96f, 0.90f, 0.85f * a);
        float m = 9 * scale, mt = std::max(1.0f, 1.6f * scale);
        const ImVec2 corner[4] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
        for (int c = 0; c < 4; c++) {
            float dx = c & 1 ? -m : m, dy = c & 2 ? -m : m;
            ImVec2 p = corner[c];
            dl->AddLine(p, ImVec2(p.x + dx, p.y), mark, mt);
            dl->AddLine(p, ImVec2(p.x, p.y + dy), mark, mt);
        }

        // the picture on the left, on a soft gold glow (as the game shows the item it hands over)
        float cx = x0 + pad + icon * 0.5f, cy = y0 + h * 0.5f, ir = icon * 0.5f;
        float pulse = 0.5f + 0.5f * sinf((float)age * 2.4f);
        for (int i = 6; i >= 1; i--)
            dl->AddCircleFilled(ImVec2(cx, cy), ir * (0.35f + 0.13f * i), col(1.0f, 0.86f, 0.45f, (0.045f + 0.012f * pulse) * a), 40);
        if (pic) {
            float pr = ir * 0.86f;
            dl->AddImage(pic, ImVec2(cx - pr, cy - pr), ImVec2(cx + pr, cy + pr), ImVec2(0, 0), ImVec2(1, 1), col(1, 1, 1, a));
        } else {  // (no picture: three triangles in gold)
            ImU32 gold = col(0.98f, 0.84f, 0.45f, a);
            float t = ir * 0.62f, hgt = t * 0.866f, bx = cx - t, by = cy + hgt;
            auto tri = [&](float x, float y2) { dl->AddTriangleFilled(ImVec2(x, y2), ImVec2(x + t, y2), ImVec2(x + t * 0.5f, y2 - hgt), gold); };
            tri(bx, by), tri(bx + t, by), tri(bx + t * 0.5f, by - hgt);
        }

        // the text: a small header, the name large in the game's slanted title face, the description in its body text
        float tx = x0 + pad + icon + gap, ty = y0 + pad;
        dl->AddText(hf, s_head, ImVec2(tx, ty), col(0.93f, 0.85f, 0.55f, a), head);
        float ny = ty + sh.y + 2 * scale;
        slanted_text(dl, nf, s_name, ImVec2(tx + 1.5f * scale, ny + 1.5f * scale), col(0, 0, 0, 0.8f * a), name);  // (shadow)
        slanted_text(dl, nf, s_name, ImVec2(tx, ny), col(1, 1, 1, a), name);
        dl->AddText(df, s_desc, ImVec2(tx, ny + sn.y + 4 * scale), col(0.90f, 0.90f, 0.86f, a), desc);
        y = y0 + drop + h + 10 * scale;  // (the next one rests below this one)
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
