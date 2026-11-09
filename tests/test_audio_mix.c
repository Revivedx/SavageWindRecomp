// Unit tests for 5.1 surround pass-through, ITU stereo downmix, and 3D binaural headphone spatialization.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/audio_mix.h"

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int nearf(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static int test_channel_counts(void) {
    CHECK(audio_device_channels(AUDIO_OUT_STEREO) == 2u, "stereo device channels");
    CHECK(audio_device_channels(AUDIO_OUT_SURROUND_51) == 6u, "5.1 device channels");
    CHECK(audio_device_channels(AUDIO_OUT_HEADPHONES_3D) == 6u, "3D headphones device channels");

    CHECK(audio_output_channels(AUDIO_OUT_STEREO) == 2u, "stereo stream channels");
    CHECK(audio_output_channels(AUDIO_OUT_SURROUND_51) == 6u, "5.1 stream channels");
    CHECK(audio_output_channels(AUDIO_OUT_HEADPHONES_3D) == 2u, "3D headphones stream channels");
    return 0;
}

static int test_stereo_downmix(void) {
    float out[2] = {0};
    float mono[6] = {0.4f, 0, 0, 0, 0, 0};
    audio_mix_sample(NULL, mono, 1, AUDIO_OUT_STEREO, out);
    CHECK(nearf(out[0], 0.4f, 1e-6f) && nearf(out[1], 0.4f, 1e-6f), "mono -> stereo");

    float in6[6] = {0.2f, -0.1f, 0.5f, 0.4f, 0.3f, -0.2f};
    audio_mix_sample(NULL, in6, 6, AUDIO_OUT_STEREO, out);
    float exp_l = 0.2f + 0.707f * 0.5f + 0.251f * 0.4f + 0.707f * 0.3f;
    float exp_r = -0.1f + 0.707f * 0.5f + 0.251f * 0.4f + 0.707f * (-0.2f);
    CHECK(nearf(out[0], exp_l, 1e-5f) && nearf(out[1], exp_r, 1e-5f),
          "ITU 5.1 -> stereo got (%.5f, %.5f) expected (%.5f, %.5f)", out[0], out[1], exp_l, exp_r);
    return 0;
}

static int test_surround_51_passthrough(void) {
    float out[6] = {0};
    float in6[6] = {0.15f, -0.25f, 0.35f, 0.45f, -0.55f, 0.65f};
    audio_mix_sample(NULL, in6, 6, AUDIO_OUT_SURROUND_51, out);
    for (int k = 0; k < 6; k++)
        CHECK(nearf(out[k], in6[k], 1e-6f), "5.1 ch %d got %.5f expected %.5f", k, out[k], in6[k]);

    // 2-channel source into a 5.1 stream populates FL/FR and zeroes FC, LFE, BL, BR.
    float in2[6] = {0.5f, -0.5f, 0.9f, 0.9f, 0.9f, 0.9f};
    audio_mix_sample(NULL, in2, 2, AUDIO_OUT_SURROUND_51, out);
    CHECK(nearf(out[0], 0.5f, 1e-6f) && nearf(out[1], -0.5f, 1e-6f), "2ch in 5.1 FL/FR");
    for (int k = 2; k < 6; k++) CHECK(out[k] == 0.0f, "2ch in 5.1 ch %d should be 0", k);
    return 0;
}

static int test_binaural_3d_itd_and_symmetry(void) {
    Audio3DState st_l, st_r;
    audio_3d_reset(&st_l);
    audio_3d_reset(&st_r);

    // Front Left vs Front Right impulse: near ear at sample 0, far ear at sample 12.
    for (int s = 0; s < 48; s++) {
        float in_fl[6] = {s == 0 ? 1.0f : 0.0f, 0, 0, 0, 0, 0};
        float in_fr[6] = {0, s == 0 ? 1.0f : 0.0f, 0, 0, 0, 0};
        float out_fl[2] = {0}, out_fr[2] = {0};
        audio_mix_sample(&st_l, in_fl, 6, AUDIO_OUT_HEADPHONES_3D, out_fl);
        audio_mix_sample(&st_r, in_fr, 6, AUDIO_OUT_HEADPHONES_3D, out_fr);
        CHECK(nearf(out_fl[0], out_fr[1], 1e-6f) && nearf(out_fl[1], out_fr[0], 1e-6f),
              "front symmetry at sample %d", s);
        if (s == 0) {
            CHECK(out_fl[0] > 0.8f && out_fl[1] == 0.0f, "FL sample 0: direct to L only");
        } else if (s < 12) {
            CHECK(out_fl[0] == 0.0f && out_fl[1] == 0.0f, "FL before ITD at sample %d", s);
        } else if (s == 12) {
            CHECK(out_fl[0] == 0.0f && out_fl[1] > 0.04f, "FL contralateral ITD arrival at sample 12");
        }
    }

    // Back Left vs Back Right impulse: near ear at sample 8, far ear at sample 34.
    audio_3d_reset(&st_l);
    audio_3d_reset(&st_r);
    for (int s = 0; s < 48; s++) {
        float in_bl[6] = {0, 0, 0, 0, s == 0 ? 1.0f : 0.0f, 0};
        float in_br[6] = {0, 0, 0, 0, 0, s == 0 ? 1.0f : 0.0f};
        float out_bl[2] = {0}, out_br[2] = {0};
        audio_mix_sample(&st_l, in_bl, 6, AUDIO_OUT_HEADPHONES_3D, out_bl);
        audio_mix_sample(&st_r, in_br, 6, AUDIO_OUT_HEADPHONES_3D, out_br);
        CHECK(nearf(out_bl[0], out_br[1], 1e-6f) && nearf(out_bl[1], out_br[0], 1e-6f),
              "rear symmetry at sample %d", s);
        if (s < 8) {
            CHECK(out_bl[0] == 0.0f && out_bl[1] == 0.0f, "BL before rear depth offset at sample %d", s);
        } else if (s == 8) {
            CHECK(out_bl[0] > 0.5f && out_bl[1] == 0.0f, "BL ipsilateral arrival at sample 8");
        } else if (s < 34) {
            CHECK(out_bl[1] == 0.0f, "BL contralateral silent before sample 34 (at %d)", s);
        } else if (s == 34) {
            CHECK(out_bl[1] > 0.03f, "BL contralateral ITD arrival at sample 34");
        }
    }
    return 0;
}

static int test_binaural_center_lfe_and_headroom(void) {
    Audio3DState st;
    audio_3d_reset(&st);

    // Center channel is perfectly balanced (L == R) with early reflection at sample 2.
    float out0[2] = {0}, out2[2] = {0};
    float fc_imp[6] = {0, 0, 0.5f, 0, 0, 0};
    float zero[6] = {0};
    audio_mix_sample(&st, fc_imp, 6, AUDIO_OUT_HEADPHONES_3D, out0);
    CHECK(nearf(out0[0], out0[1], 1e-6f) && nearf(out0[0], 0.34f, 1e-5f), "FC direct");
    audio_mix_sample(&st, zero, 6, AUDIO_OUT_HEADPHONES_3D, out2);
    audio_mix_sample(&st, zero, 6, AUDIO_OUT_HEADPHONES_3D, out2);
    CHECK(nearf(out2[0], out2[1], 1e-6f) && nearf(out2[0], 0.03f, 1e-5f), "FC 2-sample reflection");

    // LFE low-pass accumulates sub-bass over steady input while attenuating Nyquist alternation.
    audio_3d_reset(&st);
    float lfe_dc = 0.0f, lfe_alt = 0.0f;
    for (int s = 0; s < 240; s++) {
        float in_dc[6] = {0, 0, 0, 0.8f, 0, 0}, out[2];
        audio_mix_sample(&st, in_dc, 6, AUDIO_OUT_HEADPHONES_3D, out);
        lfe_dc = fabsf(out[0]);
    }
    audio_3d_reset(&st);
    for (int s = 0; s < 240; s++) {
        float in_nyq[6] = {0, 0, 0, (s & 1) ? -0.8f : 0.8f, 0, 0}, out[2];
        audio_mix_sample(&st, in_nyq, 6, AUDIO_OUT_HEADPHONES_3D, out);
        lfe_alt = fabsf(out[0]);
    }
    CHECK(lfe_dc > 3.0f * lfe_alt, "LFE low-pass should favor bass (dc=%.4f vs nyq=%.4f)", lfe_dc, lfe_alt);

    // Full-scale simultaneous input on all 6 channels stays strictly bounded inside (-1, 1).
    audio_3d_reset(&st);
    float full[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    for (int s = 0; s < 128; s++) {
        float out[2] = {0};
        audio_mix_sample(&st, full, 6, AUDIO_OUT_HEADPHONES_3D, out);
        CHECK(out[0] > 0.85f && out[0] < 1.0f && nearf(out[0], out[1], 1e-6f),
              "soft limiter out of bounds at %d: %.6f", s, out[0]);
    }
    CHECK(audio_soft_clip(NAN) == 0.0f, "NaN guard");
    return 0;
}

int main(void) {
    if (test_channel_counts()) return 1;
    if (test_stereo_downmix()) return 1;
    if (test_surround_51_passthrough()) return 1;
    if (test_binaural_3d_itd_and_symmetry()) return 1;
    if (test_binaural_center_lfe_and_headroom()) return 1;
    printf("test_audio_mix: ok\n");
    return 0;
}
