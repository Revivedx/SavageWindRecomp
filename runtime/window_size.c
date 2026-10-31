// Window size (Video > Window size). The list (user, 2026-10-19), by shape: 16:9 640x360 .. 7680x4320, 21:9
// 2560x1080 / 3440x1440 / 3840x1600, 16:10 1280x800 / 1680x1050 / 2560x1600, 4:3 and 5:4 640x480 .. 1600x1200. Offered:
// those that fit on the window's display (its desktop size), and the desktop size itself; fullscreen also offers
// bigger ones the display has a mode for (GPU downsampling such as NVIDIA DSR adds them).
//   In a window: the window's inside is that size (in pixels), centred on its display.
//   Fullscreen: the desktop size is borderless fullscreen (no mode change); another size the display has a mode for
//   switches the display to it (exclusive fullscreen); any other (a 21:9 size on a 16:9 display) stays borderless and
//   the game image is that size, scaled to fit the screen (winsize_virtual; runtime/nvn/vulkan/device.c).
// Sizes are pixels; SDL's window sizes are pixels / the display's pixel density (1 on Windows).
#include "window_size.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "hle.h"
#include "settings.h"

static const WinSize k_sizes[] = {
    {640, 360}, {960, 540}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},  // 16:9
    {5120, 2880}, {7680, 4320},
    {2560, 1080}, {3440, 1440}, {3840, 1600},                                            // 21:9
    {1280, 800}, {1680, 1050}, {2560, 1600},                                             // 16:10
    {640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1600, 1200},                     // 4:3, 5:4
};
#define NSIZES (int)(sizeof k_sizes / sizeof k_sizes[0])

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static WinSize g_desk = {1920, 1080};  // the display's desktop size (a guess until a display is read)
static float g_density = 1;            // its pixel density (window size = pixels / density)
static WinSize g_modes[64];            // its fullscreen modes' sizes
static int g_nmodes;
static SDL_DisplayID g_display;
static WinSize g_virtual;              // fullscreen on a size the display has no mode for (0 x 0: none)
static float g_hz;                     // its refresh rate (0: not known)

static bool same(WinSize a, WinSize b) { return a.w == b.w && a.h == b.h; }

// 0 16:9, 1 21:9, 2 16:10, 3 4:3, 4 5:4, 5 another shape
static int shape(int w, int h) {
    double r = h > 0 ? (double)w / h : 0;
    if (fabs(r - 16.0 / 9.0) < 0.02) return 0;
    if (r >= 2.3 && r <= 2.45) return 1;  // (64:27, 43:18 and 12:5 are all sold as 21:9)
    if (fabs(r - 1.6) < 0.02) return 2;
    if (fabs(r - 4.0 / 3.0) < 0.02) return 3;
    if (fabs(r - 1.25) < 0.02) return 4;
    return 5;
}

const char *winsize_aspect(int w, int h) {
    static const char *const names[] = {"16:9", "21:9", "16:10", "4:3", "5:4"};
    int s = shape(w, h);
    return s < 5 ? names[s] : NULL;
}

static int group(WinSize s) {  // (4:3 and 5:4 together)
    int k = shape(s.w, s.h);
    return k == 4 ? 3 : k == 5 ? 4 : k;
}

static int cmp(const void *a, const void *b) {
    const WinSize *x = a, *y = b;
    int gx = group(*x), gy = group(*y);
    if (gx != gy) return gx - gy;
    long long ax = (long long)x->w * x->h, ay = (long long)y->w * y->h;
    return ax < ay ? -1 : ax > ay ? 1 : x->w - y->w;
}

int winsize_list(WinSize *out, bool fullscreen) {
    pthread_mutex_lock(&g_m);
    WinSize desk = g_desk, modes[64];
    int nm = g_nmodes;
    for (int i = 0; i < nm; i++) modes[i] = g_modes[i];
    pthread_mutex_unlock(&g_m);
    int n = 0;
    bool have_desk = false;
    for (int i = 0; i < NSIZES && n < WINSIZE_MAX - 1; i++) {
        WinSize s = k_sizes[i];
        bool ok = s.w <= desk.w && s.h <= desk.h;
        for (int m = 0; !ok && fullscreen && m < nm; m++) ok = same(s, modes[m]);
        if (!ok) continue;
        have_desk |= same(s, desk);
        out[n++] = s;
    }
    if (!have_desk) out[n++] = desk;
    qsort(out, (size_t)n, sizeof *out, cmp);
    for (int i = 1; i < n; i++)  // (the desktop's own size first)
        if (same(out[i], desk)) {
            for (; i > 0; i--) out[i] = out[i - 1];
            out[0] = desk;
            break;
        }
    return n;
}

float winsize_refresh_rate(void) {
    pthread_mutex_lock(&g_m);
    float hz = g_hz;
    pthread_mutex_unlock(&g_m);
    return hz;
}

WinSize winsize_desktop(void) {
    pthread_mutex_lock(&g_m);
    WinSize d = g_desk;
    pthread_mutex_unlock(&g_m);
    return d;
}

static WinSize resolve(WinSize want, bool fullscreen) {
    WinSize list[WINSIZE_MAX];
    int n = winsize_list(list, fullscreen);
    if (want.w <= 0 || want.h <= 0) {
        if (fullscreen) return winsize_desktop();
        want = (WinSize){1280, 720};
    }
    for (int i = 0; i < n; i++)
        if (same(list[i], want)) return list[i];
    // not offered here (another display, or set for fullscreen only): the biggest of that shape that fits, else the
    // smallest 16:9 size, else the desktop
    int best = -1;
    for (int i = 0; i < n; i++)
        if (group(list[i]) == group(want) && list[i].w <= want.w && list[i].h <= want.h &&
            (best < 0 || list[i].w * list[i].h > list[best].w * list[best].h))
            best = i;
    if (best < 0 && !fullscreen)
        for (int i = 0; i < n; i++)
            if (group(list[i]) == 0 && (best < 0 || list[i].w * list[i].h < list[best].w * list[best].h)) best = i;
    return best >= 0 ? list[best] : winsize_desktop();
}

WinSize winsize_effective(bool fullscreen) {
    return resolve((WinSize){g_settings.window_w, g_settings.window_h}, fullscreen);
}

WinSize winsize_default(bool fullscreen) { return resolve((WinSize){0, 0}, fullscreen); }

void winsize_name(WinSize s, const char *native, char *buf, int n) {
    const char *a = winsize_aspect(s.w, s.h);
    WinSize d = winsize_desktop();
    if (!native || !same(s, d)) native = NULL;
    if (a && native) snprintf(buf, (size_t)n, "%d x %d  (%s, %s)", s.w, s.h, a, native);
    else if (a || native) snprintf(buf, (size_t)n, "%d x %d  (%s)", s.w, s.h, a ? a : native);
    else snprintf(buf, (size_t)n, "%d x %d", s.w, s.h);
}

bool winsize_virtual(int *w, int *h) {
    pthread_mutex_lock(&g_m);
    WinSize v = g_virtual;
    pthread_mutex_unlock(&g_m);
    if (v.w <= 0) return false;
    *w = v.w, *h = v.h;
    return true;
}

void winsize_refresh(void *win) {
    SDL_DisplayID d = win ? SDL_GetDisplayForWindow((SDL_Window *)win) : 0;
    if (!d) d = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = d ? SDL_GetDesktopDisplayMode(d) : NULL;
    if (!dm) return;
    float density = dm->pixel_density > 0 ? dm->pixel_density : 1;
    WinSize desk = {(int)lroundf(dm->w * density), (int)lroundf(dm->h * density)};
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(d);  // (exclusive fullscreen: the mode in use)
    float hz = cur && cur->refresh_rate > 0 ? cur->refresh_rate : dm->refresh_rate;
    pthread_mutex_lock(&g_m);
    g_hz = hz;
    bool known = d == g_display && same(desk, g_desk);
    pthread_mutex_unlock(&g_m);
    if (known) return;
    WinSize modes[64];
    int nm = 0, count = 0;
    SDL_DisplayMode **ms = SDL_GetFullscreenDisplayModes(d, &count);
    for (int i = 0; ms && i < count && nm < 64; i++) {
        WinSize s = {(int)lroundf(ms[i]->w * ms[i]->pixel_density), (int)lroundf(ms[i]->h * ms[i]->pixel_density)};
        bool dup = false;
        for (int k = 0; k < nm && !dup; k++) dup = same(modes[k], s);
        if (!dup) modes[nm++] = s;
    }
    SDL_free(ms);
    pthread_mutex_lock(&g_m);
    g_display = d, g_desk = desk, g_density = density, g_nmodes = nm;
    for (int i = 0; i < nm; i++) g_modes[i] = modes[i];
    pthread_mutex_unlock(&g_m);
    WinSize list[WINSIZE_MAX];
    char buf[512];
    int len = 0, n = winsize_list(list, true);
    for (int i = 0; i < n && len < (int)sizeof buf - 16; i++) len += snprintf(buf + len, sizeof buf - len, " %dx%d", list[i].w, list[i].h);
    LOG("[window] display %u: desktop %dx%d (density %.2f, %.2f Hz), %d fullscreen sizes; offered:%s\n", (unsigned)d,
        desk.w, desk.h, density, hz, nm, buf);
}

// The display's mode of that size with the highest refresh rate (NULL: none)
static bool find_mode(SDL_DisplayID d, WinSize s, SDL_DisplayMode *out) {
    int count = 0;
    SDL_DisplayMode **ms = SDL_GetFullscreenDisplayModes(d, &count);
    bool found = false;
    for (int i = 0; ms && i < count; i++) {
        WinSize m = {(int)lroundf(ms[i]->w * ms[i]->pixel_density), (int)lroundf(ms[i]->h * ms[i]->pixel_density)};
        if (same(m, s) && (!found || ms[i]->refresh_rate > out->refresh_rate)) *out = *ms[i], found = true;
    }
    SDL_free(ms);
    return found;
}

void winsize_apply(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    if (!w) return;
    static bool first = true;
    // test windows (SAVAGEWIND_WINDOW / SAVAGEWIND_HIDDEN) keep their size at start and are never made fullscreen
    bool test = getenv("SAVAGEWIND_WINDOW") || getenv("SAVAGEWIND_HIDDEN");
    bool keep_size = first && getenv("SAVAGEWIND_WINDOW");
    first = false;
    winsize_refresh(w);
    bool fs = g_settings.fullscreen && !test;
    WinSize s = winsize_effective(fs), desk = winsize_desktop(), virt = {0, 0};
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    if (fs) {
        SDL_DisplayMode mode;
        bool exclusive = !same(s, desk) && find_mode(d, s, &mode);
        if (!same(s, desk) && !exclusive) virt = s;
        SDL_SetWindowFullscreenMode(w, exclusive ? &mode : NULL);  // (NULL: borderless, the desktop's size)
        SDL_SetWindowFullscreen(w, true);
        LOG("[window] fullscreen %dx%d (%s)\n", s.w, s.h,
            exclusive ? "display mode" : virt.w ? "borderless, image scaled to the screen" : "borderless");
    } else {
        SDL_SetWindowFullscreen(w, false);
        if (!keep_size) {
            if (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(w);
            pthread_mutex_lock(&g_m);
            float density = g_density;
            pthread_mutex_unlock(&g_m);
            SDL_SetWindowSize(w, (int)lroundf(s.w / density), (int)lroundf(s.h / density));
            // centred in the usable area (not under the taskbar); the title bar stays on screen when it doesn't fit
            SDL_Rect u;
            if (d && SDL_GetDisplayUsableBounds(d, &u)) {
                int top = 0, x = u.x + (u.w - (int)lroundf(s.w / density)) / 2,
                    y = u.y + (u.h - (int)lroundf(s.h / density)) / 2;
                SDL_GetWindowBordersSize(w, &top, NULL, NULL, NULL);
                if (y < u.y + top) y = u.y + top;
                if (x < u.x) x = u.x;
                SDL_SetWindowPosition(w, x, y);
            }
            LOG("[window] window %dx%d\n", s.w, s.h);
        }
    }
    pthread_mutex_lock(&g_m);
    g_virtual = virt;
    pthread_mutex_unlock(&g_m);
}
