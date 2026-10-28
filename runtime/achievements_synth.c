// The default sound of an unlocked achievement: a short original "ya-ha-ha!" made of sine harmonics, in the spirit of a
// Korok's laugh. No audio file is involved (the game's own sounds belong to Nintendo and aren't shipped with this
// project): to hear another sound, put a WAV named achievement.wav in the data root (runtime/achievements_sound.c).
#include <math.h>
#include <stdint.h>

#include "achievements.h"

typedef struct { float f0, f1, len, breath, gain; } Syl;  // pitch glide (Hz), vowel length (s), breath before it (s), loudness

size_t ach_synth_jingle(int16_t *out, size_t max, int rate) {
    static const Syl syl[] = {
        {700, 930, 0.13f, 0.000f, 0.80f},    // ya
        {880, 780, 0.09f, 0.025f, 0.65f},    // ha
        {960, 840, 0.11f, 0.020f, 0.70f},    // ha
        {1060, 900, 0.20f, 0.020f, 0.85f},   // ha!
    };
    const double TAU = 6.283185307179586;
    size_t pos = 0;
    uint32_t rng = 0x1234567u;
    double phase = 0;
    for (size_t k = 0; k < sizeof syl / sizeof syl[0]; k++) {
        const Syl *s = &syl[k];
        size_t nb = (size_t)(s->breath * (float)rate), nv = (size_t)(s->len * (float)rate), gap = (size_t)(0.018 * rate);
        if (pos + nb + nv + gap > max) break;
        for (size_t i = 0; i < nb; i++) {  // the "h": a soft burst of noise
            rng = rng * 1664525u + 1013904223u;
            double n = ((double)(rng >> 8) / 8388608.0 - 1.0) * 0.18 * s->gain * (1.0 - fabs(2.0 * (double)i / (double)nb - 1.0));
            out[pos++] = (int16_t)(n * 32000.0);
        }
        for (size_t i = 0; i < nv; i++) {  // the vowel: a gliding tone with two overtones and a little vibrato
            double t = (double)i / (double)nv, secs = (double)i / rate;
            double f = (double)s->f0 + ((double)s->f1 - (double)s->f0) * t;
            phase += TAU * f * (1.0 + 0.015 * sin(TAU * 6.5 * secs)) / rate;
            double v = (sin(phase) + 0.45 * sin(2 * phase) + 0.2 * sin(3 * phase)) / 1.65;
            double attack = (double)i / (0.012 * rate);
            double env = (attack < 1.0 ? attack : 1.0) * (t < 0.55 ? 1.0 : 1.0 - (t - 0.55) / 0.45);
            out[pos++] = (int16_t)(v * env * s->gain * 0.55 * 32767.0);
        }
        for (size_t i = 0; i < gap; i++) out[pos++] = 0;
    }
    return pos;
}
