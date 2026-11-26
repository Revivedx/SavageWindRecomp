// Cheats > Collectible hints on the map (settings.h collectibles_map; collectibles.h collectibles_map_view): where the
// game's map draws a world point, read from the map screen as the game draws it (render thread, as
// framepace_map_ui). Game 1.9.0 only: the class and offsets below are that version's (runtime/pc_cursor.c's map).
//
// The map screen is ScreenMgr's screen 0x2a (AppMap; the same object is the HUD's minimap, framepace_map_ui tells the
// full map). It keeps its view in a data object (+0x3628 -> +0x8d8): the zoom at +0x878, the translation (already
// zoomed) at +0x870/+0x874, and the scale from world to map at +0x8b0/+0x8b4, which the game's own conversion
// (0x7000d8c860) applies as map = (x * s0, -(z * s1)). On screen: map * zoom + translation, in the layout's 1280x720
// units (centred, y up). The map's cursor, in the same map units, is at +0x3868/+0x386c (runtime/pc_cursor.c sends the
// pointer there). The byte at +0x38c8 is 1 on the map and 0 in Hero's Path mode (seen in 1.9.0: it changes with X and
// back with B, nothing else in the map's objects does). The hints stay off in Hero's Path mode and while the sensor's
// settings panel, a dialog over the map (0x39) or a stamp's Edit/Delete window (0x3e) is up.
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collectibles.h"
#include "framepace.h"
#include "game_sites.h"
#include "nvn/output_scale.h"
#include "rt.h"
#include "settings.h"

#define MAP_SCREEN 0x2a
#define MAP_CLASS 0x7001ce8298ull  // (AppMap's vtable, 1.9.0)

static bool guest(uint64_t p, uint64_t n) { return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p; }
static uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { return *(volatile float *)(uintptr_t)a; }

static uint64_t screen_at(uint32_t id) {
    uint64_t mgr = g_site.screen_mgr ? rd64(g_site.screen_mgr) : 0;
    if (!guest(mgr, 0x38) || *(volatile uint32_t *)(uintptr_t)(mgr + 0x28) <= id) return 0;
    uint64_t arr = rd64(mgr + 0x30);
    return guest(arr, 8 * (id + 1)) ? rd64(arr + 8 * id) : 0;
}

static bool screen_active(uint32_t id) {
    uint64_t s = screen_at(id);
    return guest(s, 0x100) && *(volatile uint8_t *)(uintptr_t)(s + 0xfe) == 2;
}

// A pane (nn::ui2d::Pane) named `name` under `pane`: its children's list head at +0x20 (each linked 8 bytes in), the
// name at +0xb8 (as pc_cursor.c pane_named)
static uint64_t pane_named(uint64_t pane, const char *name, unsigned depth, unsigned *budget) {
    if (!guest(pane, 0xf8) || depth > 32 || !*budget) return 0;
    --*budget;
    if (!strncmp((const char *)(uintptr_t)(pane + 0xb8), name, 24)) return pane;
    uint64_t head = pane + 0x20, link = rd64(head + 8);
    while (guest(link, 0x10) && link != head && *budget) {
        uint64_t found = pane_named(link - 8, name, depth + 1, budget);
        if (found) return found;
        link = rd64(link + 8);
    }
    return 0;
}

// The sensor's settings panel is up: its content pane (Pa_SensorBox_00 > N_All_00) visible, with some alpha
static bool sensor_panel(uint64_t screen) {
    static uint64_t for_screen, all;  // (found once per screen object)
    if (for_screen != screen || !guest(all, 0xf8) || strncmp((const char *)(uintptr_t)(all + 0xb8), "N_All_00", 9)) {
        uint64_t wrapper = rd64(screen + 0x30), root = guest(wrapper, 0x20) ? rd64(wrapper + 0x18) : 0;
        unsigned budget = 2048;
        all = pane_named(pane_named(root, "Pa_SensorBox_00", 0, &budget), "N_All_00", 0, &budget);
        for_screen = screen;
        if (!all) return false;
    }
    return (*(volatile uint8_t *)(uintptr_t)(all + 0x58) & 1) && *(volatile uint8_t *)(uintptr_t)(all + 0x5a);
}

// A pane visible with at least half its alpha (pc_cursor.c pane_shown), and its box in layout units (left, bottom,
// right, top) from its global matrix and size (pc_cursor.c options_box)
static bool pane_shown(uint64_t pane) {
    return guest(pane, 0xf8) && (*(volatile uint8_t *)(uintptr_t)(pane + 0x58) & 1) &&
           *(volatile uint8_t *)(uintptr_t)(pane + 0x5a) >= 0x80;
}

static bool pane_box(uint64_t pane, float box[4]) {
    float w = rdf(pane + 0x50), h = rdf(pane + 0x54), x = rdf(pane + 0x7c), y = rdf(pane + 0x8c);
    float hw = (fabsf(rdf(pane + 0x70)) * w + fabsf(rdf(pane + 0x74)) * h) * .5f;
    float hh = (fabsf(rdf(pane + 0x80)) * w + fabsf(rdf(pane + 0x84)) * h) * .5f;
    box[0] = x - hw, box[1] = y - hh, box[2] = x + hw, box[3] = y + hh;
    return isfinite(x) && isfinite(y) && isfinite(hw) && isfinite(hh) && hw > 0 && hh > 0;
}

// The map's prompt bar (Move, Sheikah Sensor, Hero's Path Mode...): the slate frame AppHome_00 (screen 0x2c) draws
// each prompt as a picture, Pa_Guide_0N > N_InOut_00 > P_Text_00 (pc_cursor.c map_guides), 200 units wide with the
// label and its button right-aligned in it. The label is the text box Pa_Guide_0N > N_CaptureText_00 > T_Guide_00: its
// UTF-16 text at +0xe0, its length at +0x11a (seen in 1.9.0). The box of the leftmost picture shown, moved down with
// the bar in a taller window, and its label in UTF-8; false: none.
static bool prompt_bar(float box[4], char *text, size_t size) {
    static uint64_t for_home, pictures[6], labels[6];  // (found once per frame object, again while none is shown)
    uint64_t home = screen_at(0x2c), name = guest(home, 0x100) ? rd64(home + 0xd0) : 0;
    if (!guest(home, 0x100) || *(volatile uint8_t *)(uintptr_t)(home + 0xfe) != 2 || !guest(name, 16) ||
        strncmp((const char *)(uintptr_t)name, "AppHome_00", 11))
        return false;
    if (for_home != home) {
        uint64_t wrapper = rd64(home + 0x30), root = guest(wrapper, 0x20) ? rd64(wrapper + 0x18) : 0;
        for (int i = 0; i < 6; i++) {
            char slot[16];
            snprintf(slot, sizeof slot, "Pa_Guide_%02d", i);
            unsigned budget = 512;
            uint64_t s = pane_named(root, slot, 0, &budget);
            budget = 16;
            uint64_t group = pane_named(s, "N_InOut_00", 0, &budget);
            budget = 16;
            pictures[i] = pane_named(group, "P_Text_00", 0, &budget);
            budget = 16;
            uint64_t capture = pane_named(s, "N_CaptureText_00", 0, &budget);
            budget = 16;
            labels[i] = pane_named(capture, "T_Guide_00", 0, &budget);
        }
        for_home = home;
    }
    int left = -1;
    for (int i = 0; i < 6; i++) {
        float b[4];
        uint64_t p = pictures[i];
        if (!pane_shown(p) || strncmp((const char *)(uintptr_t)(p + 0xb8), "P_Text_00", 10) || !pane_box(p, b)) continue;
        if (left < 0 || b[0] < box[0]) memcpy(box, b, sizeof b), left = i;
    }
    if (left < 0) {
        for_home = 0;
        return false;
    }
    box[1] -= output_map_extra_y(), box[3] -= output_map_extra_y();
    size_t n = 0;
    uint64_t label = labels[left];
    bool named = guest(label, 0x120) && !strncmp((const char *)(uintptr_t)(label + 0xb8), "T_Guide_00", 11);
    uint64_t str = named ? rd64(label + 0xe0) : 0;
    unsigned len = named ? *(volatile uint16_t *)(uintptr_t)(label + 0x11a) : 0;
    if (named && len <= 64 && guest(str, 2 * len))
        for (unsigned i = 0; i < len; i++) {  // (UTF-16 to UTF-8; the bar's labels are in the basic plane)
            unsigned c = ((const volatile uint16_t *)(uintptr_t)str)[i];
            if (c < 0x80 && n + 1 < size) text[n++] = (char)c;
            else if (c < 0x800 && n + 2 < size) text[n++] = (char)(0xc0 | c >> 6), text[n++] = (char)(0x80 | (c & 0x3f));
            else if (c >= 0x800 && n + 3 < size)
                text[n++] = (char)(0xe0 | c >> 12), text[n++] = (char)(0x80 | (c >> 6 & 0x3f)), text[n++] = (char)(0x80 | (c & 0x3f));
        }
    text[n] = 0;
    return true;
}

bool collectibles_map_view(CollectMapView *v) {
    if (!g_settings.collectibles_map || strcmp(g_game_version, "1.9.0") || !framepace_map_ui()) return false;
    uint64_t screen = screen_at(MAP_SCREEN);
    if (!guest(screen, 0x3630) || rd64(screen) != MAP_CLASS) return false;
    if (screen_active(0x39) || screen_active(0x3e) || sensor_panel(screen)) return false;
    uint64_t map = rd64(screen + 0x3628), d = guest(map, 0x8e0) ? rd64(map + 0x8d8) : 0;
    if (!guest(d, 0x38d0) || !*(volatile uint8_t *)(uintptr_t)(d + 0x38c8)) return false;
    v->translate[0] = rdf(d + 0x870), v->translate[1] = rdf(d + 0x874), v->zoom = rdf(d + 0x878);
    v->scale[0] = rdf(d + 0x8b0), v->scale[1] = rdf(d + 0x8b4);
    v->half[0] = 632, v->half[1] = 280 + output_map_extra_y();
    if (!(isfinite(v->translate[0]) && isfinite(v->translate[1]) && isfinite(v->zoom) && v->zoom > 0 &&
          isfinite(v->scale[0]) && v->scale[0] > 0 && isfinite(v->scale[1]) && v->scale[1] > 0))
        return false;
    v->cursor[0] = rdf(d + 0x3868) / v->scale[0], v->cursor[1] = -rdf(d + 0x386c) / v->scale[1];
    if (!isfinite(v->cursor[0]) || !isfinite(v->cursor[1])) v->cursor[0] = v->cursor[1] = 1e9f;  // (no region there)
    if (!prompt_bar(v->bar, v->bar_text, sizeof v->bar_text)) v->bar[0] = 1, v->bar[2] = 0, v->bar_text[0] = 0;
    return true;
}

// ---- The ground under the hints (collectibles.h collectibles_ground): the presenter's small copies of the game image
#define GROUND_EVERY_S 0.2  // (a copy at most this often while asked: panning brings new ground in view)
static CollectMapView ground_asked, ground_view;
static double ground_asked_at = -1, ground_taken_at = -10, ground_view_at = -10;
static uint8_t *ground_px;
static int ground_w, ground_h, ground_cap;
static unsigned ground_gen;

static float ground_zoom_was;  // (the zoom asked for the frame before)

static double ground_now(void) { return SDL_GetTicksNS() / 1e9; }

// The map zooming: its zoom changed from one ask to the next. A copy is paired with the view asked a frame before it,
// and a zoom step moves a circle near the canvas's edge by more than its radius in a frame (a pan, far less).
static bool ground_zooming(float zoom, float was) { return fabsf(zoom - was) > 1e-4f * was; }

void collectibles_ground_ask(const CollectMapView *v) {
    ground_zoom_was = ground_asked.zoom;
    ground_asked = *v, ground_asked_at = ground_now();
}

bool collectibles_ground_wanted(void) {
    double now = ground_now();
    if (now - ground_asked_at > 0.1 || now - ground_taken_at < GROUND_EVERY_S) return false;
    if (ground_zooming(ground_asked.zoom, ground_zoom_was)) return false;
    ground_taken_at = now;
    ground_view = ground_asked;  // (the view this copy shows; it arrives a frame or two later)
    ground_view_at = now;
    return true;
}

void collectibles_ground_put(const uint8_t *rgba, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (ground_zooming(ground_asked.zoom, ground_view.zoom)) return;  // (a zoom began while it was on its way)
    int need = w * h * 4;
    if (need > ground_cap) {
        uint8_t *p = realloc(ground_px, (size_t)need);
        if (!p) return;
        ground_px = p, ground_cap = need;
    }
    memcpy(ground_px, rgba, (size_t)need);
    ground_w = w, ground_h = h;
    ground_gen++;
}

bool collectibles_ground(CollectGround *out) {
    if (!ground_px || !ground_gen || ground_now() - ground_view_at > 1.0) return false;
    out->rgba = ground_px, out->w = ground_w, out->h = ground_h, out->gen = ground_gen, out->view = ground_view;
    return true;
}
