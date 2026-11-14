// Synthetic presentation checks; no game files or graphics device required. make test-output-scale
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/nvn/output_scale.c"
Settings g_settings = {.wide_view = true, .fov = 100, .render_scale = RSCALE_100};
static bool title, loading, map;
bool framepace_map_ui(void) { return map; }
bool framepace_title_ui(void) { return title; }
bool framepace_loading_ui(void) { return loading; }
static bool fsr3_avail;
bool framepace_in_event(void) { return false; }
bool fsr3_available(void) { return fsr3_avail; }
bool fsr4_available(void) { return fsr3_avail; }
static uint64_t shown_frame;
uint64_t plat_now_ns(void) { return 1000000000; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t framepace_shown_frame(void) { return shown_frame; }
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

    // Upscaler modes (Native AA 1.0x .. Ultra Performance 3.0x), FSR 4 / FSR 3 availability, and spatial FSR 1 fallback.
    assert(fsr3_upscaler_enabled());
    near(upscale_mode_factor(UPQ_NATIVE), 1.0);
    near(upscale_mode_factor(UPQ_ULTRA), 1.3);
    near(upscale_mode_factor(UPQ_QUALITY), 1.5);
    near(upscale_mode_factor(UPQ_BALANCED), 1.7);
    near(upscale_mode_factor(UPQ_PERFORMANCE), 2.0);
    near(upscale_mode_factor(UPQ_ULTRA_PERFORMANCE), 3.0);
    assert(upscaler_available(UPSCALE_OFF) && upscaler_available(UPSCALE_FSR1));
    assert(!upscaler_available(UPSCALE_FSR3) && !upscaler_available(UPSCALE_FSR4));
    fsr3_avail = true;
    assert(upscaler_available(UPSCALE_FSR3) && upscaler_available(UPSCALE_FSR4));
    g_settings.upscaler = UPSCALE_FSR4;
    g_settings.upscale_quality = UPQ_NATIVE;
    assert(rt_fsr3());
    near(upscale_factor(), 1.0);
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 1080); near(900 * o.scene_sy, 1080);
    g_settings.upscaler = UPSCALE_FSR1;
    assert(!rt_fsr3());
    near(upscale_factor(), 1.3);
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 1080); near(900 * o.scene_sy, 1080.0 / 1.3);
    fsr3_avail = false;
    g_settings.upscaler = UPSCALE_OFF;
    g_settings.upscale_quality = UPQ_QUALITY;

    // Render scale (50% .. 200%) scaling 3D dimensions
    g_settings.render_scale = RSCALE_50;
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 540); near(900 * o.scene_sy, 540);

    g_settings.render_scale = RSCALE_100;
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 1080); near(900 * o.scene_sy, 1080);

    g_settings.render_scale = RSCALE_150;
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 1620); near(900 * o.scene_sy, 1620);

    g_settings.render_scale = RSCALE_200;
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 2160); near(900 * o.scene_sy, 2160);
    g_settings.render_scale = RSCALE_100;

    // SHADOW_POTATO returns 448
    g_settings.shadow_res = SHADOW_POTATO;
    assert(shadow_map_size(0) == 448);
    g_settings.shadow_res = SHADOW_CONSOLE;
    assert(shadow_map_size(0) == 0);

    // UPSCALE_FSR3 + UPQ_NATIVE
    fsr3_avail = true;
    g_settings.upscaler = UPSCALE_FSR3;
    g_settings.upscale_quality = UPQ_NATIVE;
    assert(rt_fsr3());
    near(upscale_factor(), 1.0);
    output_scale(1920, 1080, true, &o);
    near(1080 * o.sy, 1080); near(900 * o.scene_sy, 1080);
    uint32_t rw = 0, rh = 0;
    rt_render_size(1920, 1080, &rw, &rh);
    assert(rw == 1920 && rh == 1080);

    // Hybrid (fast) mode with FSR 4 Quality seeds jitter_size in output_scale and applies sub-pixel jitter
    rt_jitter_set_size(0, 0);
    g_settings.mvec_mode = MVEC_MODE_HYBRID;
    g_settings.upscaler = UPSCALE_FSR4;
    g_settings.upscale_quality = UPQ_QUALITY;
    output_scale(1920, 1080, true, &o);
    assert(__atomic_load_n(&jitter_size[0], __ATOMIC_RELAXED) == 1280);
    assert(__atomic_load_n(&jitter_size[1], __ATOMIC_RELAXED) == 720);
    uint8_t proj[0x100] = {0};
    float aspect = 16.0f / 9.0f, nf[2] = {0.1f, 25000.0f};
    memcpy(proj + 0xb0, &aspect, 4);
    memcpy(proj + 0x98, nf, 8);
    shown_frame = 0;
    uint32_t ox0 = 0, oy0 = 0;
    rt_persp_offset((uint64_t)(uintptr_t)proj, &ox0, &oy0);
    float fx0 = 0, fy0 = 0;
    memcpy(&fx0, &ox0, 4); memcpy(&fy0, &oy0, 4);
    assert(fabsf(fy0 - (-1.0f / 6.0f) / 720.0f) < 1e-7f);
    shown_frame = 1;
    uint32_t ox1 = 0, oy1 = 0;
    rt_persp_offset((uint64_t)(uintptr_t)proj, &ox1, &oy1);
    float fx1 = 0, fy1 = 0;
    memcpy(&fx1, &ox1, 4); memcpy(&fy1, &oy1, 4);
    assert(fabsf(fx1 - (-0.25f) / 1280.0f) < 1e-7f);
    assert(fabsf(fy1 - (2.0f / 3.0f - 0.5f) / 720.0f) < 1e-7f);

    fsr3_avail = false;
    g_settings.upscaler = UPSCALE_OFF;

    puts("PASS: map overscan clipped at 16:10 and 4:3; Native AA, FSR 4/FSR 3 availability, Hybrid jitter seeding and FSR 1 fallback verified");
}
