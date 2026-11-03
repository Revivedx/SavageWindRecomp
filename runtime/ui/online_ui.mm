// Online play's screens (runtime/online.h): the Online tab of the in-game menu (create a session, join one, who is
// there, chat) and the overlay drawn over the game while in a session - the other players where they are in the world
// (a figure in their colour, their name and how far), an arrow at the screen's edge for those out of view, the
// session's players, join / leave notices and the chat's last lines.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../framepace.h"
}
#include "../game_sites.h"
#include "../online.h"

static const ImU32 k_colors[ONLINE_MAX] = {
    IM_COL32(236, 194, 84, 255),  IM_COL32(90, 170, 255, 255),  IM_COL32(255, 110, 110, 255), IM_COL32(120, 220, 120, 255),
    IM_COL32(220, 130, 255, 255), IM_COL32(255, 160, 70, 255),  IM_COL32(90, 230, 220, 255),  IM_COL32(240, 240, 240, 255),
};

static const char *status_text(void) {
    static char out[200];
    char arg[96];
    const char *f = online_status(arg, sizeof arg);
    snprintf(out, sizeof out, tr(f), arg);
    return out;
}

static ImU32 with_alpha(ImU32 c, float a) {
    return (c & 0x00ffffff) | ((ImU32)(a * 255) << 24);
}

// ------------------------------------------------------------------ the Online tab
extern "C" void online_tab_draw(float scale) {
    static char port[8], chat[ONLINE_CHAT_MAX];
    if (!opt_text_editing("Port")) snprintf(port, sizeof port, "%d", g_online.port);
    int st = online_state();
    bool in = st == ONLINE_CONNECTED || st == ONLINE_CONNECTING;

    opt_begin("##online", scale);
    opt_section("Online");
    char status[200];
    snprintf(status, sizeof status, "%s", status_text());
    opt_action("Status", status, "What the online connection is doing now.");

    opt_section("Player");
    if (in) opt_disable_next();
    if (opt_text("Name", g_online.name, sizeof g_online.name, "Link", "Your name, shown over your Link to the others."))
        online_settings_save();

    if (!in) {
        opt_section("Create a session");
        if (opt_text("Port", port, sizeof port, "52520",
                     "The TCP port the session listens on (52520 by default). Players outside your network need it "
                     "forwarded on your router, or a virtual network such as Tailscale, ZeroTier or Radmin VPN.")) {
            int p = atoi(port);
            if (p > 0 && p < 65536) g_online.port = p;
            online_settings_save();
        }
        if (opt_text("Password", g_online.password, sizeof g_online.password, tr("none"),
                     "Players need it to join. Empty: anyone who knows the address can."))
            online_settings_save();
        char addrs[256];
        online_local_addresses(addrs, sizeof addrs);
        char desc[512];
        snprintf(desc, sizeof desc, tr("Hosts a session on this PC and joins it. Others join with one of this PC's "
                                       "addresses: %s"), addrs[0] ? addrs : "-");
        if (opt_button("Create session", desc)) online_host();

        opt_section("Join a session");
        if (opt_text("Host address", g_online.host, sizeof g_online.host, "192.168.0.10",
                     "The address of the PC that created the session (an IP or a name). The port and the password "
                     "are the ones above."))
            online_settings_save();
        if (opt_button("Join session", "Connects to the session at the host address.")) online_join();
    } else {
        opt_section("Session");
        OnlinePlayer pl[ONLINE_MAX];
        online_players(pl);
        int me = online_local_id();
        for (int i = 0; i < ONLINE_MAX; i++) {
            if (!pl[i].used) continue;
            char label[64], value[64];
            snprintf(label, sizeof label, "%s%s", pl[i].name, i == me ? tr(" (you)") : "");
            if (pl[i].flags & ONLINE_F_LOADING) snprintf(value, sizeof value, "%s", tr("loading"));
            else snprintf(value, sizeof value, "%s%u ms", pl[i].flags & ONLINE_F_PAUSED ? tr("paused, ") : "", pl[i].ping_ms);
            opt_action(label, value, "A player in this session, and their connection's round trip.", false);
        }
        if (st == ONLINE_CONNECTED) {
            if (opt_text("Chat", chat, sizeof chat, tr("type a message"), "A message to everyone in the session.")) {
                online_chat_send(chat);
                chat[0] = 0;
            }
        }
        if (opt_button(online_is_host() ? "End session" : "Leave session",
                       online_is_host() ? "Ends the session for everyone." : "Leaves the session."))
            online_leave();
    }

    opt_section("On screen");
    if (opt_toggle("Show players", &g_online.show_markers, true, "The other players' figures where they are in the world."))
        online_settings_save();
    if (opt_toggle("Show names", &g_online.show_names, true, "Names and distances over the other players."))
        online_settings_save();
    opt_end();
}

// ------------------------------------------------------------------ the overlay
extern "C" bool online_overlay_wanted(void) { return online_state() != ONLINE_OFF; }

static bool project(const double *vp, float ysign, const float *p, ImVec2 size, ImVec2 *out, float *w) {
    double c[4];
    for (int r = 0; r < 4; r++) c[r] = vp[r * 4] * p[0] + vp[r * 4 + 1] * p[1] + vp[r * 4 + 2] * p[2] + vp[r * 4 + 3];
    *w = (float)c[3];
    if (c[3] <= 0.05) return false;
    float x = (float)((c[0] / c[3] + 1) / 2), y = (float)((c[1] / c[3] * ysign + 1) / 2);
    *out = ImVec2(x * size.x, y * size.y);
    return true;
}

// a player's figure: a head, a body and a pointer at the feet for the heading; h: Link's height on screen in pixels
static void figure(ImDrawList *dl, ImVec2 feet, ImVec2 head, ImVec2 dir, ImU32 col, float h) {
    float r = h * 0.11f, th = fmaxf(h * 0.13f, 2.0f);
    ImVec2 neck(head.x + (feet.x - head.x) * 0.22f, head.y + (feet.y - head.y) * 0.22f);
    ImVec2 hip(head.x + (feet.x - head.x) * 0.62f, head.y + (feet.y - head.y) * 0.62f);
    ImU32 dark = IM_COL32(0, 0, 0, 150);
    dl->AddEllipseFilled(feet, ImVec2(h * 0.22f, h * 0.07f), IM_COL32(0, 0, 0, 90));
    dl->AddLine(neck, hip, dark, th + 3);
    dl->AddLine(hip, ImVec2(feet.x - h * 0.08f, feet.y), dark, th * 0.8f + 3);
    dl->AddLine(hip, ImVec2(feet.x + h * 0.08f, feet.y), dark, th * 0.8f + 3);
    dl->AddLine(neck, hip, col, th);
    dl->AddLine(hip, ImVec2(feet.x - h * 0.08f, feet.y), col, th * 0.8f);
    dl->AddLine(hip, ImVec2(feet.x + h * 0.08f, feet.y), col, th * 0.8f);
    ImVec2 sh(neck.x, neck.y + h * 0.05f);
    dl->AddLine(sh, ImVec2(sh.x - h * 0.16f, hip.y), col, th * 0.7f);
    dl->AddLine(sh, ImVec2(sh.x + h * 0.16f, hip.y), col, th * 0.7f);
    dl->AddCircleFilled(ImVec2(head.x, head.y + r), r + 1.5f, dark);
    dl->AddCircleFilled(ImVec2(head.x, head.y + r), r, col);
    float dx = dir.x - feet.x, dy = dir.y - feet.y, l = sqrtf(dx * dx + dy * dy);
    if (l > 2) {
        dx /= l, dy /= l;
        float s = fmaxf(h * 0.12f, 5.0f);
        ImVec2 tip(feet.x + dx * s * 2.2f, feet.y + dy * s * 2.2f);
        dl->AddTriangleFilled(tip, ImVec2(feet.x + dx * s - dy * s * 0.7f, feet.y + dy * s + dx * s * 0.7f),
                              ImVec2(feet.x + dx * s + dy * s * 0.7f, feet.y + dy * s - dx * s * 0.7f), col);
    }
}

static void label(ImDrawList *dl, ImFont *font, float size, ImVec2 at, ImU32 col, const char *text) {
    ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0, text);
    ImVec2 p(at.x - ts.x / 2, at.y - ts.y);
    dl->AddRectFilled(ImVec2(p.x - size * 0.3f, p.y - size * 0.1f), ImVec2(p.x + ts.x + size * 0.3f, p.y + ts.y + size * 0.1f),
                      IM_COL32(0, 0, 0, 140), size * 0.25f);
    dl->AddText(font, size, p, col, text);
}

extern "C" void online_overlay_draw(float scale, ImFont *font) {
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float fs = 34 * scale;
    int st = online_state();
    bool in_play = !framepace_title_ui() && !framepace_menu_ui() && !framepace_paused();

    // join / leave notices: top centre, a few seconds each
    {
        char n[6][ONLINE_NAME_MAX];
        const char *fmt[6];
        uint64_t age[6];
        int k = online_notices(n, fmt, age, 6);
        float y = io.DisplaySize.y * 0.16f;
        for (int i = 0; i < k; i++) {
            if (age[i] > 5000) continue;
            float a = age[i] > 4000 ? 1 - (age[i] - 4000) / 1000.0f : 1;
            char t[128];
            snprintf(t, sizeof t, tr(fmt[i]), n[i]);
            label(dl, font, fs, ImVec2(io.DisplaySize.x / 2, y), with_alpha(IM_COL32(236, 194, 84, 255), a), t);
            y += fs * 1.5f;
        }
    }
    if (st != ONLINE_CONNECTED) {  // connecting / failed: one line, bottom left
        if (st == ONLINE_FAILED || st == ONLINE_CONNECTING) {
            char t[200];
            snprintf(t, sizeof t, "%s: %s", tr("Online"), status_text());
            dl->AddText(font, fs * 0.85f, ImVec2(20 * scale, io.DisplaySize.y - 60 * scale), IM_COL32(255, 220, 160, 220), t);
        }
        return;
    }

    OnlinePlayer pl[ONLINE_MAX];
    online_players(pl);
    int me = online_local_id();

    // the players: left, under the hearts
    {
        float x = 24 * scale, y = io.DisplaySize.y * 0.30f;
        int n = 0;
        for (int i = 0; i < ONLINE_MAX; i++) n += pl[i].used;
        char t[96];
        snprintf(t, sizeof t, tr("Online: %d players"), n);
        dl->AddText(font, fs * 0.8f, ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 180), t);
        dl->AddText(font, fs * 0.8f, ImVec2(x, y), IM_COL32(236, 194, 84, 230), t);
        y += fs * 1.0f;
        for (int i = 0; i < ONLINE_MAX; i++) {
            if (!pl[i].used) continue;
            dl->AddCircleFilled(ImVec2(x + fs * 0.25f, y + fs * 0.38f), fs * 0.2f, k_colors[i]);
            snprintf(t, sizeof t, "%s%s", pl[i].name, pl[i].flags & ONLINE_F_PAUSED ? tr(" (paused)") : "");
            dl->AddText(font, fs * 0.75f, ImVec2(x + fs * 0.6f + 1, y + 1), IM_COL32(0, 0, 0, 180), t);
            dl->AddText(font, fs * 0.75f, ImVec2(x + fs * 0.6f, y), i == me ? IM_COL32(255, 255, 255, 240) : IM_COL32(220, 216, 205, 220), t);
            y += fs * 0.9f;
        }
    }

    // the chat's last lines: bottom left, fading 12 s after the newest
    {
        char lines[8][ONLINE_CHAT_MAX + ONLINE_NAME_MAX + 4];
        uint64_t newest;
        int k = online_chat_lines(lines, 6, &newest);
        if (k && newest < 12000) {
            float a = newest > 10000 ? 1 - (newest - 10000) / 2000.0f : 1;
            float y = io.DisplaySize.y - 110 * scale - k * fs;
            for (int i = 0; i < k; i++, y += fs) {
                dl->AddText(font, fs * 0.8f, ImVec2(25 * scale, y + 1), with_alpha(IM_COL32(0, 0, 0, 255), 0.7f * a), lines[i]);
                dl->AddText(font, fs * 0.8f, ImVec2(24 * scale, y), with_alpha(IM_COL32(255, 255, 255, 255), a), lines[i]);
            }
        }
    }

    // the others in the world
    double vp[16];
    float ysign;
    uint64_t age;
    if (!g_online.show_markers || !in_play || !online_camera_get(vp, &ysign, &age) || age > 500000000ull) return;
    float link[3] = {0, 0, 0};
    uint64_t pinfo = g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (pinfo) memcpy(link, (const void *)(uintptr_t)(pinfo + 0x88), sizeof link);
    ImVec2 size = io.DisplaySize, centre(size.x / 2, size.y / 2);
    for (int i = 0; i < ONLINE_MAX; i++) {
        const OnlinePlayer &p = pl[i];
        if (!p.used || i == me || (p.flags & ONLINE_F_LOADING)) continue;
        float dx = p.pos[0] - link[0], dy = p.pos[1] - link[1], dz = p.pos[2] - link[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > 3000) continue;  // (another map: a shrine, a Divine Beast)
        float head[3] = {p.pos[0], p.pos[1] + 1.75f, p.pos[2]};
        float dir[3] = {p.pos[0] + sinf(p.yaw) * 0.6f, p.pos[1], p.pos[2] + cosf(p.yaw) * 0.6f};
        ImVec2 sf, sh, sd;
        float wf, wh, wd;
        bool on_f = project(vp, ysign, p.pos, size, &sf, &wf), on_h = project(vp, ysign, head, size, &sh, &wh);
        bool visible = on_f && on_h && sf.x > -50 && sf.x < size.x + 50 && sh.y < size.y + 50 && sf.y > -50;
        char t[96];
        if (dist < 1000) snprintf(t, sizeof t, "%s  %.0f m", p.name, dist);
        else snprintf(t, sizeof t, "%s  %.1f km", p.name, dist / 1000);
        if (visible) {
            float h = fabsf(sf.y - sh.y);
            if (h > 2 && !online_has_actor(i)) {
                if (!project(vp, ysign, dir, size, &sd, &wd)) sd = sf;
                // (close to the camera a figure would cover the screen: it fades out from 35% of the height)
                float a = p.flags & ONLINE_F_PAUSED ? 0.45f : 0.9f, big = (h - size.y * 0.35f) / (size.y * 0.3f);
                if (big > 0) a *= fmaxf(0.0f, 1 - big);
                if (a > 0.02f) figure(dl, sf, sh, sd, with_alpha(k_colors[i], a), h);
            }
            if (g_online.show_names) {
                float ls = fs * fminf(fmaxf(h / 120, 0.7f), 1.1f);
                label(dl, font, ls, ImVec2(sh.x, sh.y - 6 * scale), k_colors[i], t);
            }
        } else {  // out of view: an arrow at the screen's edge, towards them
            ImVec2 d(sf.x - centre.x, sf.y - centre.y);
            if (!on_f) d = ImVec2(-d.x, -d.y);  // (behind the camera: the projection comes out mirrored)
            float l = sqrtf(d.x * d.x + d.y * d.y);
            if (l < 1) continue;
            d.x /= l, d.y /= l;
            float mx = size.x / 2 - 60 * scale, my = size.y / 2 - 60 * scale;
            float k = fminf(fabsf(d.x) > 1e-4f ? mx / fabsf(d.x) : 1e9f, fabsf(d.y) > 1e-4f ? my / fabsf(d.y) : 1e9f);
            ImVec2 at(centre.x + d.x * k, centre.y + d.y * k);
            float s = 16 * scale;
            dl->AddTriangleFilled(ImVec2(at.x + d.x * s * 1.6f, at.y + d.y * s * 1.6f),
                                  ImVec2(at.x - d.y * s, at.y + d.x * s), ImVec2(at.x + d.y * s, at.y - d.x * s), k_colors[i]);
            if (g_online.show_names) label(dl, font, fs * 0.75f, ImVec2(at.x - d.x * s * 1.5f, at.y - d.y * s * 1.5f), k_colors[i], t);
        }
    }
}
