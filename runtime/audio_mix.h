// Multichannel 5.1 surround pass-through, stereo downmix, and 3D binaural headphone spatializer.
// Channel order (Nintendo Switch DeviceSink / audout and SDL3 5.1): 0=FL, 1=FR, 2=FC, 3=LFE, 4=BL, 5=BR.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "settings.h"

#define AUDIO_MIX_DELAY_LEN 64u  // power-of-two per-channel delay line (~1.33 ms at 48 kHz)

typedef struct {
    float fl_d[AUDIO_MIX_DELAY_LEN], fr_d[AUDIO_MIX_DELAY_LEN];
    float bl_d[AUDIO_MIX_DELAY_LEN], br_d[AUDIO_MIX_DELAY_LEN];
    float fc_d[4];
    uint32_t pos;
    float fl_x_lp, fr_x_lp;  // contralateral head-shadow low-pass (~2.2 kHz) for front +/-30 deg
    float bl_i_lp, br_i_lp;  // ipsilateral pinna high-shelf (~6.5 kHz) for rear +/-110 deg
    float bl_x_lp, br_x_lp;  // contralateral head-shadow low-pass (~1.3 kHz) for rear +/-110 deg
    float lfe_lp;            // subwoofer low-pass (~120 Hz)
} Audio3DState;

static inline void audio_3d_reset(Audio3DState *st) {
    if (st) memset(st, 0, sizeof *st);
}

// Active channel count reported to the game through IAudioDevice::GetActiveChannelCount(Auto).
// Both Surround 5.1 and 3D Headphones ask the game for its full 6-channel (FL, FR, FC, LFE, BL, BR) mix.
static inline uint32_t audio_device_channels(int mode) {
    return (mode == AUDIO_OUT_SURROUND_51 || mode == AUDIO_OUT_HEADPHONES_3D) ? 6u : 2u;
}

// Host SDL_AudioStream channel count: 6 discrete channels for 5.1 speaker output, 2 for Stereo and 3D Headphones.
static inline uint32_t audio_output_channels(int mode) {
    return mode == AUDIO_OUT_SURROUND_51 ? 6u : 2u;
}

static inline float audio_clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

// Smooth C1-continuous soft-knee limiter above 0.85 for binaural summing headroom.
static inline float audio_soft_clip(float x) {
    if (!(x >= -16.0f && x <= 16.0f)) return 0.0f;
    float a = fabsf(x);
    if (a <= 0.85f) return x;
    float d = a - 0.85f;
    float s = 0.85f + 0.15f * (d / (0.15f + d));
    return x < 0.0f ? -s : s;
}

// Mixes one frame of up to 6 normalized input channels (in[0..5] in [-1, 1]: FL, FR, FC, LFE, BL, BR) into
// out[0..audio_output_channels(mode)-1].
static inline void audio_mix_sample(Audio3DState *st, const float in[6], uint32_t inputs, int mode, float *out) {
    if (mode == AUDIO_OUT_SURROUND_51) {
        float fl = inputs >= 1 ? in[0] : 0.0f;
        float fr = inputs == 1 ? in[0] : (inputs >= 2 ? in[1] : 0.0f);
        out[0] = audio_clampf(fl, -1.0f, 1.0f);
        out[1] = audio_clampf(fr, -1.0f, 1.0f);
        for (uint32_t k = 2; k < 6; k++)
            out[k] = inputs > k ? audio_clampf(in[k], -1.0f, 1.0f) : 0.0f;
        return;
    }
    if (mode == AUDIO_OUT_HEADPHONES_3D && st) {
        uint32_t p = st->pos & (AUDIO_MIX_DELAY_LEN - 1);
        float fl = inputs >= 1 ? in[0] : 0.0f;
        float fr = inputs == 1 ? in[0] : (inputs >= 2 ? in[1] : 0.0f);
        float fc = inputs >= 6 ? in[2] : 0.0f;
        float lfe = inputs >= 6 ? in[3] : 0.0f;
        float bl = inputs >= 6 ? in[4] : 0.0f;
        float br = inputs >= 6 ? in[5] : 0.0f;

        st->fl_d[p] = fl;
        st->fr_d[p] = fr;
        st->bl_d[p] = bl;
        st->br_d[p] = br;
        st->fc_d[p & 3] = fc;

        // Front speakers (+/-30 deg): 0-sample near ear, 12-sample (~0.25 ms @ 48 kHz) ITD + 2.2 kHz head-shadow LP to far ear
        float fl_far = st->fl_d[(p - 12) & (AUDIO_MIX_DELAY_LEN - 1)];
        float fr_far = st->fr_d[(p - 12) & (AUDIO_MIX_DELAY_LEN - 1)];
        st->fl_x_lp += 0.25f * (fl_far - st->fl_x_lp);
        st->fr_x_lp += 0.25f * (fr_far - st->fr_x_lp);

        // Rear surround speakers (+/-110 deg): 8-sample depth offset + pinna high-shelf to near ear;
        // 34-sample (~0.71 ms @ 48 kHz) ITD + 1.3 kHz head-shadow LP to far ear
        float bl_near = st->bl_d[(p - 8) & (AUDIO_MIX_DELAY_LEN - 1)];
        float br_near = st->br_d[(p - 8) & (AUDIO_MIX_DELAY_LEN - 1)];
        st->bl_i_lp += 0.58f * (bl_near - st->bl_i_lp);
        st->br_i_lp += 0.58f * (br_near - st->br_i_lp);
        float bl_ipsi = 0.70f * st->bl_i_lp + 0.30f * bl_near;
        float br_ipsi = 0.70f * st->br_i_lp + 0.30f * br_near;

        float bl_far = st->bl_d[(p - 34) & (AUDIO_MIX_DELAY_LEN - 1)];
        float br_far = st->br_d[(p - 34) & (AUDIO_MIX_DELAY_LEN - 1)];
        st->bl_x_lp += 0.16f * (bl_far - st->bl_x_lp);
        st->br_x_lp += 0.16f * (br_far - st->br_x_lp);

        // Front Center (0 deg): anchored in front with a 2-sample micro-early reflection
        float fc_early = st->fc_d[(p - 2) & 3];
        float center = 0.68f * fc + 0.06f * fc_early;

        // LFE subwoofer: ~120 Hz low-pass + low-mid punch
        st->lfe_lp += 0.016f * (lfe - st->lfe_lp);
        float sub = 0.32f * st->lfe_lp + 0.08f * lfe;

        st->pos = p + 1;

        float L = 0.88f * fl + 0.22f * st->fr_x_lp + center + sub + 0.76f * bl_ipsi + 0.28f * st->br_x_lp;
        float R = 0.88f * fr + 0.22f * st->fl_x_lp + center + sub + 0.76f * br_ipsi + 0.28f * st->bl_x_lp;
        out[0] = audio_soft_clip(L);
        out[1] = audio_soft_clip(R);
        return;
    }
    float L = inputs >= 1 ? in[0] : 0.0f;
    float R = inputs == 1 ? in[0] : (inputs >= 2 ? in[1] : 0.0f);
    if (inputs >= 6) {
        L += 0.707f * in[2] + 0.251f * in[3] + 0.707f * in[4];
        R += 0.707f * in[2] + 0.251f * in[3] + 0.707f * in[5];
    }
    out[0] = audio_clampf(L, -1.0f, 1.0f);
    out[1] = audio_clampf(R, -1.0f, 1.0f);
}
