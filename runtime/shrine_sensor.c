// Shrine sensor volume / mute (runtime/shrine_sensor.c).
// Scales or silences the waveform of Sys_Item_SheikSensor inside Sound/Resource/M_UI.bars.
// BOTW keeps M_UI.bars in Pack/Bootup.pack and as a loose file in RomFS, storing waves as BFWAV (FWAV) DSP-ADPCM;
// patching the waveform keeps the exact file sizes and works identically with the SDK AudioRenderer and audren:u.
#include "shrine_sensor.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t rd32i(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static inline int16_t rd16i(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }

static inline int16_t clamp16(int64_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

static void adpcm_decode_ch(const uint8_t *raw, uint32_t nsamp, const int16_t coefs[16],
                            int16_t h0, int16_t h1, int16_t *pcm) {
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
        pcm[s] = v;
    }
}

static void adpcm_encode_ch(const int16_t *pcm, uint32_t nsamp, const int16_t coefs[16], uint8_t *raw) {
    uint32_t nframes = (nsamp + 13) / 14;
    int16_t h0 = 0, h1 = 0;
    for (uint32_t fr = 0; fr < nframes; fr++) {
        uint32_t base = fr * 14;
        uint32_t len = nsamp - base < 14 ? nsamp - base : 14;
        int64_t best_err = INT64_MAX;
        uint8_t best_frame[8] = {0};
        int16_t best_h0 = 0, best_h1 = 0;

        for (int ci = 0; ci < 8 && best_err > 0; ci++) {
            int64_t c0 = coefs[ci * 2], c1 = coefs[ci * 2 + 1];
            for (int scale = 0; scale <= 12 && best_err > 0; scale++) {
                int64_t step = (int64_t)2048 << scale;
                int16_t th0 = h0, th1 = h1;
                int64_t err = 0;
                uint8_t nibs[14] = {0};
                for (uint32_t k = 0; k < len; k++) {
                    int16_t tgt = pcm[base + k];
                    int64_t pred = c0 * th0 + c1 * th1;
                    int64_t diff = ((int64_t)tgt << 11) - pred;
                    int64_t nib = diff >= 0 ? (diff + step / 2) / step : -((-diff + step / 2) / step);
                    if (nib > 7) nib = 7;
                    if (nib < -8) nib = -8;
                    int16_t dec = clamp16((nib * step + pred + 1024) >> 11);
                    int64_t d = (int64_t)tgt - dec;
                    err += d * d;
                    if (err >= best_err) break;
                    th1 = th0;
                    th0 = dec;
                    nibs[k] = (uint8_t)(nib & 0xF);
                }
                if (err < best_err) {
                    best_err = err;
                    best_h0 = th0;
                    best_h1 = th1;
                    best_frame[0] = (uint8_t)((ci << 4) | scale);
                    for (int k = 0; k < 7; k++) {
                        best_frame[1 + k] = (uint8_t)((nibs[2 * k] << 4) | nibs[2 * k + 1]);
                    }
                }
            }
        }
        memcpy(raw + fr * 8, best_frame, 8);
        h0 = best_h0;
        h1 = best_h1;
    }
}

static bool patch_fwav(uint8_t *fwav, size_t wsize, int volume_pct) {
    if (wsize < 0x2C || memcmp(fwav, "FWAV\xFF\xFE", 6)) return false;
    uint16_t nsec = rd16(fwav + 0x10);
    if ((uint64_t)0x14 + (uint64_t)nsec * 12 > wsize) return false;

    uint32_t info_off = 0, info_sz = 0, data_off = 0, data_sz = 0;
    for (uint16_t s = 0; s < nsec; s++) {
        const uint8_t *ref = fwav + 0x14 + s * 12;
        uint16_t type = rd16(ref);
        uint32_t off = rd32(ref + 4), sz = rd32(ref + 8);
        if ((uint64_t)off + sz > wsize) return false;
        if (type == 0x7000) { info_off = off; info_sz = sz; }
        else if (type == 0x7001) { data_off = off; data_sz = sz; }
    }
    if (!info_off || !data_off || info_sz < 0x20 || data_sz < 8) return false;

    uint8_t *info = fwav + info_off;
    uint8_t *wdata = fwav + data_off;
    if (memcmp(info, "INFO", 4) || memcmp(wdata, "DATA", 4)) return false;

    uint8_t enc = info[8];
    uint32_t nsamp = rd32(info + 0x14);
    if (nsamp == 0 || nsamp > (64u << 20)) return false;

    size_t tab_off = 0x1C;
    if (tab_off + 4 > info_sz) return false;
    uint32_t nch = rd32(info + tab_off);
    if (nch == 0 || nch > 16 || tab_off + 4 + (uint64_t)nch * 8 > info_sz) return false;

    if (volume_pct >= 100) return true;

    float factor = volume_pct <= 0 ? 0.0f : (float)volume_pct / 100.0f;
    float gain = volume_pct <= 0 ? 0.0f : powf(factor, 1.5f);

    for (uint32_t ch = 0; ch < nch; ch++) {
        int32_t ch_rel = rd32i(info + tab_off + 4 + ch * 8 + 4);
        if (ch_rel < 0 || tab_off + (size_t)ch_rel + 20 > info_sz) return false;
        uint8_t *ci = info + tab_off + ch_rel;

        int32_t soff_rel = rd32i(ci + 4);
        int32_t aoff_rel = rd32i(ci + 12);
        if (soff_rel < 0 || (uint64_t)soff_rel + 8 > data_sz) return false;

        size_t sample_pos = 8 + (size_t)soff_rel;
        uint8_t *sample_ptr = wdata + sample_pos;
        size_t avail_bytes = data_sz - sample_pos;

        if (enc == 2) {  // DSP-ADPCM
            size_t nframes = ((size_t)nsamp + 13) / 14;
            size_t nbytes = nframes * 8;
            if (nbytes > avail_bytes) return false;
            if (aoff_rel <= 0 || tab_off + (size_t)ch_rel + (size_t)aoff_rel + 44 > info_sz) return false;
            uint8_t *adpcm = ci + aoff_rel;

            if (volume_pct <= 0) {
                memset(sample_ptr, 0, nbytes);
                memset(adpcm + 32, 0, 12);  // start & loop context (pred_scale, h0, h1)
            } else {
                int16_t coefs[16];
                for (int i = 0; i < 16; i++) coefs[i] = rd16i(adpcm + i * 2);
                int16_t h0 = rd16i(adpcm + 34), h1 = rd16i(adpcm + 36);
                int16_t *pcm = malloc((size_t)nsamp * sizeof(int16_t));
                if (!pcm) return false;
                adpcm_decode_ch(sample_ptr, nsamp, coefs, h0, h1, pcm);
                for (uint32_t s = 0; s < nsamp; s++) {
                    pcm[s] = clamp16(lroundf((float)pcm[s] * gain));
                }
                adpcm_encode_ch(pcm, nsamp, coefs, sample_ptr);
                free(pcm);
                uint16_t pred0 = sample_ptr[0];
                memcpy(adpcm + 32, &pred0, 2);
                memset(adpcm + 34, 0, 10);  // h0=0, h1=0, clear loop context
            }
        } else if (enc == 1) {  // PCM16
            size_t nbytes = (size_t)nsamp * 2;
            if (nbytes > avail_bytes) return false;
            if (volume_pct <= 0) {
                memset(sample_ptr, 0, nbytes);
            } else {
                for (uint32_t s = 0; s < nsamp; s++) {
                    int16_t v = rd16i(sample_ptr + s * 2);
                    v = clamp16(lroundf((float)v * gain));
                    memcpy(sample_ptr + s * 2, &v, 2);
                }
            }
        } else if (enc == 0) {  // PCM8
            if ((size_t)nsamp > avail_bytes) return false;
            if (volume_pct <= 0) {
                memset(sample_ptr, 0, nsamp);
            } else {
                for (uint32_t s = 0; s < nsamp; s++) {
                    int8_t v = (int8_t)sample_ptr[s];
                    int32_t sv = (int32_t)lroundf((float)v * gain);
                    sample_ptr[s] = (uint8_t)(sv > 127 ? 127 : sv < -128 ? -128 : sv);
                }
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool patch_bwav(uint8_t *bars, size_t size, uint32_t wave, int volume_pct) {
    if ((uint64_t)wave + 0x10 > size || memcmp(bars + wave, "BWAV", 4)) return false;
    if (volume_pct >= 100) return true;
    uint16_t num_ch = rd16(bars + wave + 0xE);
    size_t ch_base = (size_t)wave + 0x10;
    if (ch_base + 0x4C > size || rd32(bars + ch_base + 4) < 8000) ch_base = (size_t)wave + 0x40;
    for (uint16_t ch = 0; ch < num_ch && ch_base + (size_t)(ch + 1) * 0x4C <= size; ch++) {
        uint32_t data_start = rd32(bars + ch_base + ch * 0x4C + 0x30);
        uint32_t num_samples = rd32(bars + ch_base + ch * 0x4C + 0xC);
        uint64_t pos = (uint64_t)wave + data_start;
        if (volume_pct <= 0 && data_start && pos < size) {
            size_t bytes = (num_samples + 1) / 2 + 16;
            if (pos + bytes > size) bytes = (size_t)(size - pos);
            memset(bars + pos, 0, bytes);
        }
    }
    return true;
}

bool shrine_sensor_patch_bars(uint8_t *bars, size_t size, int volume_pct) {
    if (!bars || size < 16 || memcmp(bars, "BARS", 4) || memcmp(bars + 8, "\xFF\xFE", 2)) return false;
    uint32_t cnt = rd32(bars + 12);
    if (16 + (uint64_t)cnt * 12 > size) return false;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t at = rd32(bars + 16 + cnt * 4 + i * 8);
        uint32_t wave = rd32(bars + 20 + cnt * 4 + i * 8);
        if ((uint64_t)at + 0x1C > size || memcmp(bars + at, "AMTA\xFF\xFE", 6)) continue;

        uint32_t strg_off = rd32(bars + at + 0x18);
        if ((uint64_t)at + strg_off + 8 > size) continue;
        if (memcmp(bars + at + strg_off, "STRG", 4)) continue;

        size_t name_off = (size_t)at + strg_off + 8;
        const void *nul = memchr(bars + name_off, '\0', size - name_off);
        if (!nul) continue;
        if (strcmp((const char *)bars + name_off, "Sys_Item_SheikSensor")) continue;

        if (wave == 0xFFFFFFFFu || wave >= size) return false;
        if (size - wave >= 6 && !memcmp(bars + wave, "FWAV\xFF\xFE", 6)) {
            return patch_fwav(bars + wave, size - wave, volume_pct);
        }
        if (size - wave >= 4 && !memcmp(bars + wave, "BWAV", 4)) {
            return patch_bwav(bars, size, wave, volume_pct);
        }
        return false;
    }
    return false;
}

// ------------------------------------------------------------------ live volume
// The game plays the sensor as DSP-ADPCM voices, one per channel, each with its channel's 16 ADPCM coefficients.
// Those are computed per sound, so they tell the sensor's voices from every other sound's. At start the sensor's
// channels are noted from M_UI.bars (coefficients, samples, start context). Then:
// - the sdk's own renderer (runtime/services/audio.c, the default): each frame, before its command list runs, a data
//   source command whose coefficients are the sensor's has its samples brought to the setting's volume where they
//   lie in the game's memory (shrine_sensor_render): re-encoded from the original once per change, written only over
//   the original or an earlier volume's samples;
// - our audren:u mixer: its voices are scaled by shrine_sensor_gain at mixing.
#define SENSOR_CHANS 8
typedef struct {
    int16_t coefs[16];
    uint32_t nsamp;
    size_t nbytes;
    int16_t h0, h1;
    uint8_t *orig, *cur, *prev;  // the samples as shipped, at the volume now set, at the one before
    int cur_vol;
} SensorChan;
static SensorChan g_ch[SENSOR_CHANS];
static int g_nch;

static void note_chan(const uint8_t *coef_bytes, const uint8_t *samples, uint32_t nsamp, size_t nbytes) {
    int16_t k[16];
    for (int i = 0; i < 16; i++) k[i] = rd16i(coef_bytes + i * 2);
    for (int i = 0; i < g_nch; i++)
        if (!memcmp(g_ch[i].coefs, k, sizeof k)) return;
    if (g_nch >= SENSOR_CHANS) return;
    SensorChan *c = &g_ch[g_nch];
    uint8_t *orig = malloc(nbytes), *cur = malloc(nbytes), *prev = malloc(nbytes);
    if (!orig || !cur || !prev) { free(orig), free(cur), free(prev); return; }
    memcpy(c->coefs, k, sizeof k);
    c->nsamp = nsamp, c->nbytes = nbytes;
    c->h0 = rd16i(coef_bytes + 34), c->h1 = rd16i(coef_bytes + 36);
    memcpy(orig, samples, nbytes), memcpy(cur, samples, nbytes), memcpy(prev, samples, nbytes);
    c->orig = orig, c->cur = cur, c->prev = prev, c->cur_vol = 100;
    g_nch++;
}

// Sys_Item_SheikSensor's FWAV in a BARS file, or NULL
static const uint8_t *sensor_fwav(const uint8_t *bars, size_t size, size_t *wsize) {
    if (!bars || size < 16 || memcmp(bars, "BARS", 4) || memcmp(bars + 8, "\xFF\xFE", 2)) return NULL;
    uint32_t cnt = rd32(bars + 12);
    if (16 + (uint64_t)cnt * 12 > size) return NULL;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t at = rd32(bars + 16 + cnt * 4 + i * 8), wave = rd32(bars + 20 + cnt * 4 + i * 8);
        if ((uint64_t)at + 0x1C > size || memcmp(bars + at, "AMTA\xFF\xFE", 6)) continue;
        uint32_t strg = rd32(bars + at + 0x18);
        if ((uint64_t)at + strg + 8 > size || memcmp(bars + at + strg, "STRG", 4)) continue;
        size_t name = (size_t)at + strg + 8;
        if (!memchr(bars + name, '\0', size - name) || strcmp((const char *)bars + name, "Sys_Item_SheikSensor")) continue;
        if (wave >= size || size - wave < 0x2C || memcmp(bars + wave, "FWAV\xFF\xFE", 6)) return NULL;
        *wsize = size - wave;
        return bars + wave;
    }
    return NULL;
}

int shrine_sensor_note_bars(const uint8_t *bars, size_t size) {
    size_t wsize = 0;
    const uint8_t *fwav = sensor_fwav(bars, size, &wsize);
    if (!fwav) return 0;
    uint16_t nsec = rd16(fwav + 0x10);
    if ((uint64_t)0x14 + (uint64_t)nsec * 12 > wsize) return 0;
    uint32_t info_off = 0, info_sz = 0, data_off = 0, data_sz = 0;
    for (uint16_t s = 0; s < nsec; s++) {
        const uint8_t *ref = fwav + 0x14 + s * 12;
        uint32_t off = rd32(ref + 4), sz = rd32(ref + 8);
        if ((uint64_t)off + sz > wsize) return 0;
        if (rd16(ref) == 0x7000) info_off = off, info_sz = sz;
        else if (rd16(ref) == 0x7001) data_off = off, data_sz = sz;
    }
    if (!info_off || info_sz < 0x20 || !data_off || data_sz < 8) return 0;
    const uint8_t *info = fwav + info_off, *wdata = fwav + data_off;
    if (memcmp(info, "INFO", 4) || memcmp(wdata, "DATA", 4) || info[8] != 2) return 0;  // (DSP-ADPCM only)
    uint32_t nsamp = rd32(info + 0x14), nch = rd32(info + 0x1C);
    size_t nbytes = ((size_t)nsamp + 13) / 14 * 8;
    if (!nsamp || nsamp > (64u << 20) || nch == 0 || nch > 16 || 0x20 + (uint64_t)nch * 8 > info_sz) return 0;
    int found = 0;
    for (uint32_t ch = 0; ch < nch; ch++) {
        int32_t ch_rel = rd32i(info + 0x1C + 4 + ch * 8 + 4);
        if (ch_rel < 0 || 0x1C + (size_t)ch_rel + 20 > info_sz) continue;
        const uint8_t *ci = info + 0x1C + ch_rel;
        int32_t s_rel = rd32i(ci + 4), a_rel = rd32i(ci + 12);
        if (a_rel <= 0 || 0x1C + (size_t)ch_rel + (size_t)a_rel + 44 > info_sz) continue;
        if (s_rel < 0 || 8 + (uint64_t)s_rel + nbytes > data_sz) continue;
        note_chan(ci + a_rel, wdata + 8 + s_rel, nsamp, nbytes);
        found++;
    }
    return found;
}

static SensorChan *chan_of(const int16_t coefs[16]) {
    for (int i = 0; i < g_nch; i++)
        if (!memcmp(g_ch[i].coefs, coefs, sizeof g_ch[i].coefs)) return &g_ch[i];
    return NULL;
}

bool shrine_sensor_voice(const int16_t coefs[16]) { return chan_of(coefs) != NULL; }

float shrine_sensor_gain(int volume_pct) {
    if (volume_pct >= 100) return 1.0f;
    if (volume_pct <= 0) return 0.0f;
    return powf(volume_pct / 100.0f, 1.5f);  // (perceptual: 50 % sounds about half as loud)
}

// c->cur at volume (c->prev keeps the samples it replaces)
static bool encode_at(SensorChan *c, int volume) {
    int16_t *pcm = NULL;
    if (volume > 0 && volume < 100 && !(pcm = malloc((size_t)c->nsamp * sizeof *pcm))) return false;
    uint8_t *t = c->prev;
    c->prev = c->cur, c->cur = t;
    if (volume >= 100) memcpy(c->cur, c->orig, c->nbytes);
    else if (volume <= 0) memset(c->cur, 0, c->nbytes);
    else {
        float g = shrine_sensor_gain(volume);
        adpcm_decode_ch(c->orig, c->nsamp, c->coefs, c->h0, c->h1, pcm);
        for (uint32_t s = 0; s < c->nsamp; s++) pcm[s] = clamp16(lroundf(pcm[s] * g));
        adpcm_encode_ch(pcm, c->nsamp, c->coefs, c->cur);
        free(pcm);
    }
    c->cur_vol = volume;
    return true;
}

bool shrine_sensor_render(const int16_t coefs[16], uint8_t *samples, size_t size, int volume) {
    SensorChan *c = chan_of(coefs);
    if (!c || size < c->nbytes) return false;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume != c->cur_vol && !encode_at(c, volume)) return false;
    if (!memcmp(samples, c->cur, c->nbytes)) return false;
    // (only over the sensor's own samples: as shipped, or as an earlier volume left them)
    if (memcmp(samples, c->orig, c->nbytes) && memcmp(samples, c->prev, c->nbytes)) return false;
    memcpy(samples, c->cur, c->nbytes);
    return true;
}
