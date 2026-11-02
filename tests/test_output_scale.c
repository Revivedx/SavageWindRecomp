// Synthetic presentation checks; no game files or graphics device required. make test-output-scale
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/output_scale.c"
Settings g_settings = {.wide_view = true, .fov = 100};
static bool title, loading, map;
bool framepace_map_ui(void) { return map; }
bool framepace_title_ui(void) { return title; }
bool framepace_loading_ui(void) { return loading; }
bool framepace_in_event(void) { return false; }
bool fsr3_available(void) { return false; }
uint64_t plat_now_ns(void) { return 1000000000; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t framepace_shown_frame(void) { return 0; }
static void near(double actual, double expected) {
    assert(fabs(actual - expected) < 1e-6);
}

int main(void) {
    OutScale o;
    output_scale(1280, 800, true, &o);
    near(o.shape, .9);
    near(o.vw, 1280); near(o.vh, 800);
    near(o.vx, 0); near(o.vy, 0);
    near(1920 * o.sx, 1280); near(1080 * o.sy, 800);

    output_scale(1280, 800, false, &o);
    near(o.shape, 1); near(o.vh, 720); near(o.vy, 40);
    title = true;
    output_scale(1280, 800, true, &o);
    near(o.shape, 1); near(o.vh, 720);
    title = false; loading = true;
    output_scale(1280, 800, true, &o);
    near(o.shape, 1); near(o.vh, 720);
    loading = false;
    output_scale(1920, 1200, true, &o);
    near(o.shape, .9); near(o.vh, 1200);

    // Clip the map's overscan without changing its targets, projection or pointer mapping.
    map = true;
    output_scale(1280, 800, true, &o);
    near(o.shape, .9); near(o.vw, 1280); near(o.vh, 720); near(o.vy, 40);
    near(o.crop[1], .05); near(o.crop[3], .9);
    near(1920 * o.sx, 1280); near(1080 * o.sy, 800);
    // One source pixel still maps to one window pixel: cropping must not stretch the map.
    near(1080 * o.sy * o.crop[3], o.vh);
    output_scale(1024, 768, true, &o);
    near(o.vh, 576); near(o.vy, 96); near(o.crop[3], .75);
    near(1080 * o.sy * o.crop[3], o.vh);

    output_scale(1280, 800, false, &o);
    near(o.vh, 720); near(o.crop[3], 1);
    output_scale(1920, 1080, true, &o);
    near(o.vh, 1080); near(o.crop[3], 1);
    output_scale(2560, 1080, true, &o);
    near(o.vw, 2560); near(o.crop[2], 1); near(o.crop[3], 1);
    map = false;
    output_scale(1280, 800, true, &o);
    near(o.vh, 800); near(o.crop[3], 1);
    puts("PASS: map overscan clipped at 16:10 and 4:3; gameplay, disabled fill, title, loading, 16:9 and ultrawide unchanged");
}
