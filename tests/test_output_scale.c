// Synthetic presentation checks; no game files or graphics device required. make test-output-scale
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/output_scale.c"
Settings g_settings = {.wide_view = true, .fov = 100, .render_scale = RSCALE_100};
volatile int g_movie_playing;
static bool title, loading, map, pause;
bool framepace_menu_ui(void) { return pause; }
bool framepace_map_ui(void) { return map; }
bool framepace_title_ui(void) { return title; }
bool framepace_loading_ui(void) { return loading; }
bool framepace_in_event(void) { return false; }
bool fsr3_available(void) { return false; }
bool fsr4_available(void) { return false; }
static bool dlss_on;  // (the Render scale cases below)
bool vk_dlss_available(void) { return dlss_on; }
bool xess_available(void) { return false; }
bool xess_usable(void) { return false; }
bool vk_dlssg_available(void) { return false; }
int vk_dlssg_max_mult(void) { return 2; }
uint64_t plat_now_ns(void) { return 1000000000; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t framepace_shown_frame(void) { return 0; }
float winsize_refresh_rate(void) { return 60; }
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

    pause = true;
    g_settings.hud_edges = true;
#ifndef SW_METAL
    assert(fabsf(output_pause_extra_y() - 40) < .001f);
    assert(fabsf(output_ui_extra_y() - 40) < .001f);
    g_settings.hud_edges = false;
    near(output_pause_extra_y(), 0);
    g_settings.hud_edges = true;
    map = true;
    near(output_pause_extra_y(), 0);
    map = false;
#endif
    pause = false;
    near(output_pause_extra_y(), 0);
    output_scale(1280, 800, false, &o);
    near(o.shape, 1); near(o.vh, 720); near(o.vy, 40);
    title = true;
    output_scale(1280, 800, true, &o);
#ifdef SW_METAL
    near(o.shape, 1); near(o.vh, 720); near(o.vy, 40);
#else
    near(o.shape, .9); near(o.vh, 800); near(o.vy, 0);
#endif
    output_scale(2560, 1080, true, &o);
    near(o.shape, 1); near(o.vw, 1920); near(o.vx, 320);
    title = false; loading = true;
    output_scale(1280, 800, true, &o);
    near(o.shape, .9); near(o.vh, 800); near(o.vy, 0);
    near(1920 * o.sx, 1280); near(1080 * o.sy, 800);
    output_scale(1280, 800, false, &o);
    near(o.shape, 1); near(o.vh, 720);
    output_scale(2560, 1080, true, &o);
    near(o.shape, 1); near(o.vw, 1920); near(o.vx, 320);
    loading = false;
    output_scale(1920, 1200, true, &o);
    near(o.shape, .9); near(o.vh, 1200);

    // A full-height map reveals more terrain within the original pixel scale.
    map = true;
    output_scale(1280, 800, true, &o);
#ifdef SW_METAL
    near(o.shape, .9); near(o.vh, 720); near(o.vy, 40);
    near(output_map_extra_y(), 0);
    output_scale(1024, 768, true, &o);
    near(o.vh, 576); near(o.vy, 96); near(output_map_extra_y(), 0);
#else
    near(o.shape, .9); near(o.vw, 1280); near(o.vh, 800); near(o.vy, 0);
    near(o.crop[1], 0); near(o.crop[3], 1);
    near(output_map_extra_y(), 40);
    near(1920 * o.sx, 1280); near(1080 * o.sy, 800);
    output_scale(1024, 768, true, &o);
    near(o.vh, 768); near(o.vy, 0); near(o.crop[3], 1);
    near(output_map_extra_y(), 120);
#endif
    output_scale(1280, 1024, true, &o);
    near(o.vh, 720); near(o.vy, 152);
    near(output_map_extra_y(), 0);

    output_scale(1280, 800, false, &o);
    near(o.vh, 720); near(o.crop[3], 1); near(output_map_extra_y(), 0);
    output_scale(1920, 1080, true, &o);
    near(o.vh, 1080); near(o.crop[3], 1);
    output_scale(2560, 1080, true, &o);
    near(o.vw, 2560); near(o.crop[2], 1); near(o.crop[3], 1);
    map = false;
    output_scale(1280, 800, true, &o);
    near(o.vh, 800); near(o.crop[3], 1);

    // Near-16:9 integer resolutions (1366x768) snap 1:1 instead of rendering 1365px wide (#770).
    for (int wide = 0; wide <= 1; wide++) {
        output_scale(1366, 768, wide != 0, &o);
        near(1920 * o.sx, 1366); near(1080 * o.sy, 768);
        near(1600 * o.scene_sx, 1366); near(900 * o.scene_sy, 768);
        near(o.vw, 1366); near(o.vh, 768); near(o.vx, 0); near(o.vy, 0);
    }

#ifndef SW_METAL
    // Render scale only scales the 3D scene; UI/HUD targets stay at output resolution (#746).
    g_settings.render_scale = RSCALE_75;
    output_scale(1920, 1080, true, &o);
    near(1920 * o.sx, 1920); near(1080 * o.sy, 1080);
    near(1600 * o.scene_sx, 1440); near(900 * o.scene_sy, 810);

    g_settings.render_scale = RSCALE_200;
    output_scale(1920, 1080, true, &o);
    near(1920 * o.sx, 1920); near(1080 * o.sy, 1080);
    near(1600 * o.scene_sx, 3840); near(900 * o.scene_sy, 2160);
    g_settings.render_scale = RSCALE_100;
#endif
    // Pre-rendered 16:9 movies keep their aspect ratio instead of stretching or squishing (#838).
    g_movie_playing = 1;
    output_scale(2560, 1080, true, &o);
    near(o.shape, 1); near(o.vw, 1920); near(o.vh, 1080); near(o.vx, 320); near(o.vy, 0);
    output_scale(1280, 800, true, &o);
    near(o.shape, 1); near(o.vw, 1280); near(o.vh, 720); near(o.vx, 0); near(o.vy, 40);
    g_movie_playing = 0;


    // Dynamic resolution never draws the scene under a third of the window's lines: at 100% down to 3.0x, at 75% to
    // 2.0x (2.4 would be under), at 50% (the Potato preset) to 1.5x, its fixed size before
    near(k_dyn_res_factors[dyn_res_max_step()], 3.0);
    g_settings.render_scale = RSCALE_75;
    near(k_dyn_res_factors[dyn_res_max_step()], 2.0);
    g_settings.render_scale = RSCALE_50;
    near(k_dyn_res_factors[dyn_res_max_step()], 1.5);
    g_settings.render_scale = RSCALE_100;

#ifndef SW_METAL
    // Render scale with a temporal upscaler (DLSS here): up to 100%, in Native (DLAA) 100%. A stored value it doesn't
    // offer runs as 100%; render_scale_fit, from the menus, sets it to 100% and back when it is offered again.
    dlss_on = true;
    g_settings.upscaler = UPSCALE_DLSS, g_settings.upscale_quality = UPQ_QUALITY;
    g_settings.render_scale = RSCALE_75;
    assert(render_scale_offered(RSCALE_75) && render_scale_now() == RSCALE_75);
    g_settings.render_scale = RSCALE_150;
    assert(!render_scale_offered(RSCALE_150) && render_scale_now() == RSCALE_100);
    assert(g_settings.render_scale == RSCALE_150);  // (stored as it was until a menu fits it)
    render_scale_fit();
    assert(g_settings.render_scale == RSCALE_100);
    g_settings.upscaler = UPSCALE_OFF;  // (stepping on to no upscaler: the 150% comes back)
    render_scale_fit();
    assert(g_settings.render_scale == RSCALE_150);
    g_settings.upscaler = UPSCALE_DLSS, g_settings.upscale_quality = UPQ_NATIVE;
    g_settings.render_scale = RSCALE_75;
    assert(!render_scale_offered(RSCALE_75) && render_scale_now() == RSCALE_100);
    render_scale_fit();
    assert(g_settings.render_scale == RSCALE_100);
    g_settings.upscale_quality = UPQ_DYNAMIC;  // (on to Dynamic: 75% is offered again and comes back)
    render_scale_fit();
    assert(g_settings.render_scale == RSCALE_75);
    g_settings.render_scale = RSCALE_50;
    assert(render_scale_offered(RSCALE_50) && render_scale_now() == RSCALE_50);
    g_settings.upscale_quality = UPQ_NATIVE;  // (a value the player set: fitted to 100%, kept once set in its row)
    render_scale_fit();
    render_scale_chosen();
    g_settings.upscale_quality = UPQ_DYNAMIC;
    render_scale_fit();
    assert(g_settings.render_scale == RSCALE_100);
    dlss_on = false, g_settings.upscaler = UPSCALE_OFF, g_settings.upscale_quality = UPQ_QUALITY;
    g_settings.render_scale = RSCALE_100;
#endif
    puts("PASS: taller map viewport, 16:9 fallback, disabled fill, title, loading, movies, ultrawide, dynamic floor and "
         "render scale with DLSS");
}
