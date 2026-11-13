// Unit test for shrine sensor volume / mute (runtime/shrine_sensor.c).
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "shrine_sensor.h"

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int16_t clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

// Standard simple DSP-ADPCM coefficients (predictor 0 = simple 1st-order LP)
static const int16_t test_coefs[16] = {
    1600, 0,  1024, 512, 2048, -512, 512, 0,
    0,    0,  800,  200, 1200, -200, 400, 100,
};

static void decode_adpcm(const uint8_t *raw, uint32_t nsamp, const int16_t coefs[16],
                         int16_t h0, int16_t h1, int16_t *out) {
    for (uint32_t s = 0; s < nsamp; s++) {
        uint32_t fr = s / 14, k = s % 14;
        uint8_t ps = raw[fr * 8];
        int scale = ps & 15, ci = (ps >> 4) & 7;
        uint8_t b = raw[fr * 8 + 1 + k / 2];
        int nib = (k & 1) ? ((int8_t)(b << 4) >> 4) : ((int8_t)b >> 4);
        int64_t pred = (int64_t)coefs[ci * 2] * h0 + (int64_t)coefs[ci * 2 + 1] * h1;
        int16_t v = clamp16((((int64_t)nib * ((int64_t)2048 << scale)) + pred + 1024) >> 11);
        h1 = h0;
        h0 = v;
        out[s] = v;
    }
}

static size_t build_fwav_bars(uint8_t *buf, size_t cap, uint8_t enc, uint32_t nsamp) {
    memset(buf, 0, cap);

    uint32_t amta1_off = 0x40;
    uint32_t amta2_off = 0xC0;
    uint32_t fwav_off = 0x140;

    // BARS Header
    memcpy(buf, "BARS", 4);
    wr32(buf + 4, (uint32_t)cap);
    memcpy(buf + 8, "\xFF\xFE\x01\x01", 4);
    wr32(buf + 12, 2);
    wr32(buf + 16, 0x11111111);
    wr32(buf + 20, 0x22222222);
    wr32(buf + 24, amta1_off);
    wr32(buf + 28, 0xFFFFFFFFu);
    wr32(buf + 32, amta2_off);
    wr32(buf + 36, fwav_off);

    // AMTA 1 (Sys_Dummy)
    uint8_t *a1 = buf + amta1_off;
    memcpy(a1, "AMTA\xFF\xFE\x00\x04", 8);
    wr32(a1 + 8, 0x70);
    wr32(a1 + 0xC, 0x20);
    wr32(a1 + 0x18, 0x50);
    memcpy(a1 + 0x20, "DATA", 4);
    wr32(a1 + 0x24, 0x28);
    memcpy(a1 + 0x50, "STRG", 4);
    wr32(a1 + 0x54, 0x10);
    strcpy((char *)a1 + 0x58, "Sys_Dummy");

    // AMTA 2 (Sys_Item_SheikSensor)
    uint8_t *a2 = buf + amta2_off;
    memcpy(a2, "AMTA\xFF\xFE\x00\x04", 8);
    wr32(a2 + 8, 0x78);
    wr32(a2 + 0xC, 0x20);
    wr32(a2 + 0x18, 0x50);
    memcpy(a2 + 0x20, "DATA", 4);
    wr32(a2 + 0x24, 0x28);
    memcpy(a2 + 0x50, "STRG", 4);
    wr32(a2 + 0x54, 0x20);
    strcpy((char *)a2 + 0x58, "Sys_Item_SheikSensor");

    // FWAV Header at fwav_off
    // INFO @ +0x40 (size 0x100), DATA @ +0x140
    uint32_t info_rel = 0x40, info_sz = 0x100, data_rel = 0x140;
    uint32_t ch_bytes = enc == 2 ? ((nsamp + 13) / 14) * 8 : enc == 1 ? nsamp * 2 : nsamp;
    uint32_t data_sz = 8 + 0x38 + ch_bytes * 2 + 0x40;
    uint8_t *fw = buf + fwav_off;
    memcpy(fw, "FWAV\xFF\xFE\x40\x00\x00\x02\x01\x00", 12);
    wr32(fw + 0x0C, data_rel + data_sz);
    wr16(fw + 0x10, 2);
    // section 0: INFO (0x7000)
    wr16(fw + 0x14, 0x7000);
    wr32(fw + 0x18, info_rel);
    wr32(fw + 0x1C, info_sz);
    // section 1: DATA (0x7001)
    wr16(fw + 0x20, 0x7001);
    wr32(fw + 0x24, data_rel);
    wr32(fw + 0x28, data_sz);

    uint8_t *info = fw + info_rel;
    memcpy(info, "INFO", 4);
    wr32(info + 4, info_sz);
    info[8] = enc;
    info[9] = 0;  // loop = 0
    wr32(info + 0xC, 48000);
    wr32(info + 0x14, nsamp);

    // Channel table at info + 0x1C (2 channels)
    uint8_t *tab = info + 0x1C;
    wr32(tab, 2);
    uint32_t ci0_rel = 0x18, ci1_rel = 0x30;
    wr16(tab + 4, 0x7100);
    wr32(tab + 8, ci0_rel);
    wr16(tab + 12, 0x7100);
    wr32(tab + 16, ci1_rel);

    uint32_t s0_rel = 0x38, s1_rel = 0x38 + ch_bytes + 0x20;
    uint32_t adpcm0_rel = 0x40, adpcm1_rel = 0x60;
    uint8_t *ci0 = tab + ci0_rel, *ci1 = tab + ci1_rel;
    wr16(ci0, 0x1F00); wr32(ci0 + 4, s0_rel);
    wr16(ci0 + 8, 0x0300); wr32(ci0 + 12, adpcm0_rel);
    wr16(ci1, 0x1F00); wr32(ci1 + 4, s1_rel);
    wr16(ci1 + 8, 0x0300); wr32(ci1 + 12, adpcm1_rel);

    memcpy(ci0 + adpcm0_rel, test_coefs, sizeof test_coefs);
    memcpy(ci1 + adpcm1_rel, test_coefs, sizeof test_coefs);

    uint8_t *wdata = fw + data_rel;
    memcpy(wdata, "DATA", 4);
    wr32(wdata + 4, data_sz);

    uint8_t *p0 = wdata + 8 + s0_rel;
    uint8_t *p1 = wdata + 8 + s1_rel;
    if (enc == 2) {
        uint32_t nfr = (nsamp + 13) / 14;
        for (uint32_t fr = 0; fr < nfr; fr++) {
            p0[fr * 8] = 0x08;  // ci=0, scale=8 (moderate loud signal)
            p1[fr * 8] = 0x08;
            for (int k = 0; k < 7; k++) {
                p0[fr * 8 + 1 + k] = (k & 1) ? 0x2E : 0x4C;
                p1[fr * 8 + 1 + k] = (k & 1) ? 0x3D : 0x5B;
            }
        }
        wr16(ci0 + adpcm0_rel + 32, p0[0]);
        wr16(ci1 + adpcm1_rel + 32, p1[0]);
    } else if (enc == 1) {
        for (uint32_t s = 0; s < nsamp; s++) {
            int16_t v = (int16_t)(10000 * sin(s * 0.2));
            memcpy(p0 + s * 2, &v, 2);
            memcpy(p1 + s * 2, &v, 2);
        }
    }
    wr32(buf + 4, fwav_off + data_rel + data_sz);
    return fwav_off + data_rel + data_sz;
}

static size_t build_bwav_bars(uint8_t *buf, size_t cap) {
    memset(buf, 0, cap);
    uint32_t amta_off = 0x40, bwav_off = 0x100, samp_rel = 0x70;
    memcpy(buf, "BARS", 4);
    wr32(buf + 4, 0x200);
    memcpy(buf + 8, "\xFF\xFE\x01\x00", 4);
    wr32(buf + 12, 1);
    wr32(buf + 16, 0x22222222);
    wr32(buf + 20, amta_off);
    wr32(buf + 24, bwav_off);

    uint8_t *a = buf + amta_off;
    memcpy(a, "AMTA\xFF\xFE\x01\x00", 8);
    wr32(a + 8, 0x78);
    wr32(a + 0xC, 0x20);
    wr32(a + 0x18, 0x50);
    memcpy(a + 0x20, "DATA", 4);
    wr32(a + 0x24, 0x28);
    memcpy(a + 0x50, "STRG", 4);
    wr32(a + 0x54, 0x20);
    strcpy((char *)a + 0x58, "Sys_Item_SheikSensor");

    uint8_t *bw = buf + bwav_off;
    memcpy(bw, "BWAV\xFF\xFE\x00\x01", 8);
    wr16(bw + 0xE, 1);
    wr32(bw + 0x10 + 4, 48000);
    wr32(bw + 0x10 + 0xC, 32);
    wr32(bw + 0x10 + 0x30, samp_rel);
    memset(bw + samp_rel, 0xAB, 32);
    return 0x200;
}

static int peak_pcm16(const int16_t *p, uint32_t n) {
    int pk = 0;
    for (uint32_t i = 0; i < n; i++) {
        int a = p[i] < 0 ? -p[i] : p[i];
        if (a > pk) pk = a;
    }
    return pk;
}

int main(void) {
    printf("[test_shrine_sensor] starting tests...\n");

    assert(!shrine_sensor_patch_bars(NULL, 0, 50));
    uint8_t junk[32] = {0};
    assert(!shrine_sensor_patch_bars(junk, sizeof junk, 50));

    // 1. FWAV DSP-ADPCM (matches BOTW M_UI.bars format)
    enum { NSAMP = 280 };
    uint8_t bars[4096];
    size_t len = build_fwav_bars(bars, sizeof bars, 2, NSAMP);
    uint8_t orig_bars[4096];
    memcpy(orig_bars, bars, len);

    const uint8_t *fw = bars + 0x140;
    const uint8_t *s0 = fw + 0x140 + 8 + 0x38;
    int16_t pcm[NSAMP];
    decode_adpcm(s0, NSAMP, test_coefs, 0, 0, pcm);
    int orig_pk = peak_pcm16(pcm, NSAMP);
    assert(orig_pk > 1000);

    // Live volume: the sensor's coefficients are noted (both channels share them here: one signature), other
    // coefficients are not the sensor's, and the gain follows the setting
    static const int16_t other[16] = {1, 2, 3};
    assert(!shrine_sensor_voice(test_coefs));
    assert(shrine_sensor_note_bars(bars, len) == 2);
    assert(shrine_sensor_voice(test_coefs) && !shrine_sensor_voice(other));
    assert(shrine_sensor_note_bars(junk, sizeof junk) == 0);
    assert(shrine_sensor_gain(100) == 1.0f && shrine_sensor_gain(0) == 0.0f);
    assert(fabsf(shrine_sensor_gain(50) - 0.35355f) < 1e-4f);
    printf("  [pass] live volume: sensor voices told apart by their ADPCM coefficients\n");

    // Live volume in the game's memory: the sensor's samples are rewritten at each change, back to the original at
    // 100 %, and nothing else is written over
    {
        enum { NB = (NSAMP + 13) / 14 * 8 };
        uint8_t mem[NB], other_mem[NB];
        memcpy(mem, s0, NB);
        assert(!shrine_sensor_render(test_coefs, mem, NB, 100));  // (as shipped already)
        assert(shrine_sensor_render(test_coefs, mem, NB, 50));
        int16_t live[NSAMP];
        decode_adpcm(mem, NSAMP, test_coefs, 0, 0, live);
        int lpk = peak_pcm16(live, NSAMP);
        assert(lpk < orig_pk * 0.45 && lpk > orig_pk * 0.25);
        assert(!shrine_sensor_render(test_coefs, mem, NB, 50));  // (no change: not written again)
        assert(shrine_sensor_render(test_coefs, mem, NB, 0));
        for (int b = 0; b < NB; b++) assert(mem[b] == 0);
        assert(shrine_sensor_render(test_coefs, mem, NB, 100));
        assert(!memcmp(mem, s0, NB));
        memset(other_mem, 0x5a, NB);  // (someone else's samples now)
        assert(!shrine_sensor_render(test_coefs, other_mem, NB, 30));
        for (int b = 0; b < NB; b++) assert(other_mem[b] == 0x5a);
        assert(!shrine_sensor_render(test_coefs, mem, NB - 1, 30));  // (too small)
        printf("  [pass] live volume: samples rewritten in place at each change, back to the original at 100%%\n");
    }

    // 100%: untouched
    assert(shrine_sensor_patch_bars(bars, len, 100));
    assert(memcmp(bars, orig_bars, len) == 0);
    printf("  [pass] FWAV ADPCM 100%% leaves data untouched (peak=%d)\n", orig_pk);

    // 50%: powf(0.5, 1.5) ~= 0.35355
    memcpy(bars, orig_bars, len);
    assert(shrine_sensor_patch_bars(bars, len, 50));
    decode_adpcm(s0, NSAMP, test_coefs, 0, 0, pcm);
    int pk50 = peak_pcm16(pcm, NSAMP);
    double want50 = orig_pk * pow(0.5, 1.5);
    assert(fabs(pk50 - want50) < want50 * 0.05 + 4);
    printf("  [pass] FWAV ADPCM 50%% scales peak %d -> %d (expected ~%.0f)\n", orig_pk, pk50, want50);

    // 10%: powf(0.1, 1.5) ~= 0.03162
    memcpy(bars, orig_bars, len);
    assert(shrine_sensor_patch_bars(bars, len, 10));
    decode_adpcm(s0, NSAMP, test_coefs, 0, 0, pcm);
    int pk10 = peak_pcm16(pcm, NSAMP);
    double want10 = orig_pk * pow(0.1, 1.5);
    assert(fabs(pk10 - want10) < want10 * 0.1 + 4);
    printf("  [pass] FWAV ADPCM 10%% scales peak %d -> %d (expected ~%.0f)\n", orig_pk, pk10, want10);

    // 0%: muted to digital zero
    memcpy(bars, orig_bars, len);
    assert(shrine_sensor_patch_bars(bars, len, 0));
    decode_adpcm(s0, NSAMP, test_coefs, 0, 0, pcm);
    assert(peak_pcm16(pcm, NSAMP) == 0);
    assert(rd16(fw + 0x40 + 0x1C + 0x18 + 0x40 + 32) == 0);
    printf("  [pass] FWAV ADPCM 0%% mutes cleanly to digital zero\n");

    // 2. FWAV PCM16
    len = build_fwav_bars(bars, sizeof bars, 1, 128);
    assert(shrine_sensor_patch_bars(bars, len, 50));
    assert(shrine_sensor_patch_bars(bars, len, 0));
    for (uint32_t i = 0; i < 128 * 2; i++) assert(bars[0x140 + 0x140 + 8 + 0x38 + i] == 0);
    printf("  [pass] FWAV PCM16 50%% and 0%% mute pass\n");

    // 3. BWAV mute respects wave + data_start
    uint8_t bbars[512];
    size_t blen = build_bwav_bars(bbars, sizeof bbars);
    assert(bbars[0x170] == 0xAB);
    assert(shrine_sensor_patch_bars(bbars, blen, 0));
    for (int i = 0; i < 32; i++) assert(bbars[0x170 + i] == 0);
    printf("  [pass] BWAV 0%% zeroes wave + data_start\n");

    printf("[test_shrine_sensor] all tests passed successfully!\n");
    return 0;
}
