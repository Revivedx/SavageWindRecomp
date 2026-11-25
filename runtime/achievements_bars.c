// One sound out of the game's own sound banks (.bars), for the achievement notice (achievements_sound.c): read from the
// player's RomFS when it's needed, never shipped.
//
// A .bars is a list of (AMTA, audio) pairs: the AMTA's STRG section holds the sound's name, the audio is an FWAV whose
// INFO section gives the encoding, rate and channels and whose DATA section holds the samples. BOTW's are DSP-ADPCM
// (encoding 2): frames of 8 bytes, a header byte (coefficient pair index, scale) and 14 4-bit samples. Each block says its
// own byte order (a BOM after its magic): the voice banks' AMTAs are big-endian, the rest little-endian.
#include <stdlib.h>
#include <string.h>

#include "achievements.h"

static uint32_t rd32(const uint8_t *p, bool be) {
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p, bool be) { return (uint16_t)(be ? p[0] << 8 | p[1] : p[0] | p[1] << 8); }
static bool big(const uint8_t *bom) { return bom[0] == 0xFE && bom[1] == 0xFF; }
static bool fits(size_t n, size_t off, size_t len) { return off <= n && len <= n - off; }

static int16_t *fwav_decode(const uint8_t *b, size_t n, size_t o, int *rate, size_t *count) {
    if (!fits(n, o, 0x14) || memcmp(b + o, "FWAV", 4)) return NULL;
    bool be = big(b + o + 4);
    size_t info = 0, data = 0;
    for (unsigned i = 0, nsec = rd16(b + o + 0x10, be); i < nsec && i < 8; i++) {
        size_t r = o + 0x14 + i * 12;
        if (!fits(n, r, 12)) return NULL;
        if (rd16(b + r, be) == 0x7000) info = o + rd32(b + r + 4, be);
        if (rd16(b + r, be) == 0x7001) data = o + rd32(b + r + 4, be);
    }
    if (!info || !data || !fits(n, info, 0x28) || !fits(n, data, 8)) return NULL;
    if (b[info + 8] != 2 || !rd32(b + info + 0x1C, be)) return NULL;  // DSP-ADPCM, at least one channel
    uint32_t hz = rd32(b + info + 0xC, be), samples = rd32(b + info + 0x14, be);
    // the first channel only (the notice is mono)
    size_t ci = info + 0x1C + rd32(b + info + 0x20 + 4, be);
    if (!fits(n, ci, 0x10)) return NULL;
    size_t at = data + 8 + rd32(b + ci + 4, be), coef_at = ci + rd32(b + ci + 0xC, be);
    if (!fits(n, coef_at, 32) || !hz || !samples || samples > 30u * hz) return NULL;
    int16_t coef[16];
    for (int i = 0; i < 16; i++) coef[i] = (int16_t)rd16(b + coef_at + i * 2, be);
    size_t frames = (samples + 13) / 14;
    if (!fits(n, at, frames * 8)) return NULL;
    int16_t *out = malloc(samples * sizeof *out);
    if (!out) return NULL;
    int h1 = 0, h2 = 0;
    for (size_t k = 0, f = 0; f < frames; f++) {
        const uint8_t *fr = b + at + f * 8;
        int scale = 1 << (fr[0] & 0xF), c1 = coef[(fr[0] >> 4 & 7) * 2], c2 = coef[(fr[0] >> 4 & 7) * 2 + 1];
        for (int j = 0; j < 14 && k < samples; j++, k++) {
            int nib = j & 1 ? fr[1 + j / 2] & 0xF : fr[1 + j / 2] >> 4;
            if (nib >= 8) nib -= 16;
            int s = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2;
            s >>= 11;
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            out[k] = (int16_t)s;
            h2 = h1, h1 = s;
        }
    }
    *rate = (int)hz, *count = samples;
    return out;
}

int16_t *ach_bars_sound(const uint8_t *b, size_t n, const char *name, int *rate, size_t *count) {
    if (!b || !fits(n, 0, 0x10) || memcmp(b, "BARS", 4)) return NULL;
    bool be = big(b + 8);
    uint32_t entries = rd32(b + 0xC, be);
    size_t pairs = 0x10 + (size_t)entries * 4;
    if (entries > 4096 || !fits(n, pairs, (size_t)entries * 8)) return NULL;
    size_t want = strlen(name);
    for (uint32_t i = 0; i < entries; i++) {
        size_t amta = rd32(b + pairs + i * 8, be), audio = rd32(b + pairs + i * 8 + 4, be);
        if (!fits(n, amta, 0x1C) || memcmp(b + amta, "AMTA", 4)) continue;
        bool abe = big(b + amta + 4);
        size_t strg = amta + rd32(b + amta + 0x18, abe);
        if (!fits(n, strg, 8) || memcmp(b + strg, "STRG", 4)) continue;
        size_t len = rd32(b + strg + 4, abe);
        if (!fits(n, strg + 8, len) || len < want || memcmp(b + strg + 8, name, want) || (len > want && b[strg + 8 + want]))
            continue;
        return fwav_decode(b, n, audio, rate, count);
    }
    return NULL;
}
