// Music volume (the music_volume setting): the game's music gets its own volume, sound effects and voices keep theirs
// (issue #1083, as the Silence of the Wild mod did for Cemu by editing the game's own volume table).
//
// The game plays music as DSP-ADPCM voices of the nn::audio renderer, one per channel: the BGM streams
// (Sound/Resource/Stream/BGM_*.bfstm, Bgm*.bfstm) and the music banks (Sound/Resource/*Bgm*.bars: the field's phrases,
// the towns', Kass's songs). Each channel carries its own 16 ADPCM coefficients, computed per sound, so they tell a music
// voice from every other sound (runtime/shrine_sensor.c tells the sensor's voices apart the same way):
//   - when the game first reads a music file (runtime/services/fs.c), its channels' coefficient tables are noted here;
//   - a voice acquired with one of them (nn::audio::AcquireVoiceSlot, runtime/services/audio.c) is a music voice until
//     it is released, and the volume the game sets on it (nn::audio::SetVoiceVolume) is multiplied by the setting's
//     gain. A changed setting is applied at the game's next renderer update (RequestUpdateAudioRenderer).
// Only the volume value changes: the samples, the mix and the sound effects stay as the game made them, on the sdk's own
// renderer and on our audren:u mixer alike. Cutscene streams (Event*, Demo*) mix music with voices and stay as they are.
#include "music_volume.h"
#include "hle.h"
#include "settings.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t rd32i(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------ music files
bool music_path(const char *path) {
    if (!path) return false;
    static const char res[] = "Sound/Resource/";
    if (strncmp(path, res, sizeof res - 1)) return false;
    const char *name = path + sizeof res - 1;
    size_t n = strlen(name);
    if (!strncmp(name, "Stream/", 7)) {
        const char *s = name + 7;
        size_t sn = strlen(s);
        return sn > 6 && !strcasecmp(s + sn - 6, ".bfstm") && (!strncmp(s, "BGM_", 4) || !strncmp(s, "Bgm", 3));
    }
    if (strchr(name, '/') || n < 8 || strcasecmp(name + n - 5, ".bars")) return false;
    for (const char *p = name; p + 3 <= name + n - 5; p++)
        if (!strncmp(p, "Bgm", 3)) return true;
    return false;
}

// Coefficient tables, by a 64-bit hash of their 32 bytes: open addressing, a slot taken once and never freed.
#define COEF_CAP 32768
static uint64_t g_coef[COEF_CAP];
static int g_ncoef;
static int g_dirty;   // a voice acquired or a table noted since: music_voice_changed looks again

static uint64_t coef_hash(const uint8_t *k) {
    uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a
    for (int i = 0; i < 32; i++) h = (h ^ k[i]) * 0x100000001b3ull;
    return h | 1;                         // 0 marks a free slot
}

static int all_zero(const uint8_t *k) {
    for (int i = 0; i < 32; i++)
        if (k[i]) return 0;
    return 1;
}

static int coef_add(const uint8_t *k) {   // g_lock held; 1 when new
    if (all_zero(k)) return 0;            // (a table that names no sound)
    uint64_t h = coef_hash(k);
    for (uint32_t i = 0, at = (uint32_t)h; i < 64; i++, at++) {
        uint64_t *s = &g_coef[at % COEF_CAP];
        if (*s == h) return 0;
        if (!*s) return *s = h, g_ncoef++, __atomic_store_n(&g_dirty, 1, __ATOMIC_RELAXED), 1;
    }
    return 0;                             // (a full probe run: the table counts as a sound effect)
}

// BFSTM, or the BFSTP prefetch a bank keeps of a stream's start (little endian): the INFO block's channel table; each
// channel's DSP-ADPCM info starts with its coefficients. The prefetch's are the stream's, and the game may start a
// stream's voices from its prefetch before it reads the stream.
static int note_bfstm(const uint8_t *d, size_t n) {
    if (n < 0x40 || (memcmp(d, "FSTM", 4) && memcmp(d, "FSTP", 4)) || rd16(d + 4) != 0xFEFF) return 0;
    uint16_t nsec = rd16(d + 0x10);
    uint32_t info = 0;
    for (uint16_t s = 0; s < nsec && 0x14 + (size_t)(s + 1) * 12 <= n; s++)
        if (rd16(d + 0x14 + s * 12) == 0x4000) info = rd32(d + 0x14 + s * 12 + 4);
    if (!info || (uint64_t)info + 0x20 > n || memcmp(d + info, "INFO", 4)) return 0;
    const uint8_t *body = d + info + 8;
    size_t blen = n - info - 8;
    if (rd16(body) != 0x4100 || rd16(body + 16) != 0x0101) return 0;
    int32_t si = rd32i(body + 4);   // the stream info, whose first byte is the encoding
    if (si <= 0 || (size_t)si >= blen || body[si] != 2) return 0;   // DSP-ADPCM only
    int32_t tab = rd32i(body + 20);   // the channel info table (references relative to it)
    if (tab <= 0 || (size_t)tab + 4 > blen) return 0;
    uint32_t nch = rd32(body + tab);
    int found = 0;
    for (uint32_t c = 0; c < nch && c < 64 && (size_t)tab + 4 + (c + 1) * 8 <= blen; c++) {
        int32_t ci = rd32i(body + tab + 4 + c * 8 + 4);
        if (ci <= 0 || (size_t)tab + ci + 8 > blen) continue;
        const uint8_t *chan = body + tab + ci;
        int32_t a = rd32i(chan + 4);    // its DSP-ADPCM info, relative to the channel info
        if (rd16(chan) != 0x0300 || a <= 0 || (size_t)tab + ci + a + 32 > blen) continue;
        found += coef_add(chan + a);
    }
    return found;
}

// FWAV inside a BARS bank: the INFO section's channel table, as runtime/shrine_sensor.c reads the sensor's.
static int note_fwav(const uint8_t *w, size_t n) {
    if (n < 0x20 || memcmp(w, "FWAV\xFF\xFE", 6)) return 0;
    uint16_t nsec = rd16(w + 0x10);
    uint32_t io = 0, isz = 0;
    for (uint16_t s = 0; s < nsec && 0x14 + (size_t)(s + 1) * 12 <= n; s++)
        if (rd16(w + 0x14 + s * 12) == 0x7000) io = rd32(w + 0x14 + s * 12 + 4), isz = rd32(w + 0x14 + s * 12 + 8);
    if (!io || isz < 0x20 || (uint64_t)io + isz > n) return 0;
    const uint8_t *info = w + io;
    if (memcmp(info, "INFO", 4) || info[8] != 2) return 0;   // DSP-ADPCM only
    uint32_t nch = rd32(info + 0x1C);
    int found = 0;
    for (uint32_t c = 0; c < nch && c < 16 && 0x20 + (uint64_t)(c + 1) * 8 <= isz; c++) {
        int32_t rel = rd32i(info + 0x1C + 4 + c * 8 + 4);
        if (rel < 0 || 0x1C + (size_t)rel + 16 > isz) continue;
        const uint8_t *ci = info + 0x1C + rel;
        int32_t a = rd32i(ci + 12);
        if (a <= 0 || 0x1C + (size_t)rel + (size_t)a + 32 > isz) continue;
        found += coef_add(ci + a);
    }
    return found;
}

static int note_bars(const uint8_t *d, size_t n) {
    if (n < 16 || memcmp(d, "BARS", 4) || memcmp(d + 8, "\xFF\xFE", 2)) return 0;
    uint32_t cnt = rd32(d + 12);
    if (16 + (uint64_t)cnt * 12 > n) return 0;
    int found = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t wave = rd32(d + 20 + cnt * 4 + i * 8);
        if (wave == 0xFFFFFFFFu || wave >= n) continue;
        found += !memcmp(d + wave, "FSTP", 4) ? note_bfstm(d + wave, n - wave) : note_fwav(d + wave, n - wave);
    }
    return found;
}

int music_note_file(const char *path, const uint8_t *data, size_t size) {
    if (!path || !data) return 0;
    size_t pl = strlen(path);
    pthread_mutex_lock(&g_lock);
    int found = pl > 6 && !strcasecmp(path + pl - 6, ".bfstm") ? note_bfstm(data, size) : note_bars(data, size);
    int total = g_ncoef;
    pthread_mutex_unlock(&g_lock);
    static int logged;
    if (found && logged++ < 4) LOG("[music] %s: %d channel(s) noted as music (%d in all)\n", path, found, total);
    return found;
}

float music_gain(int volume_pct) {
    if (volume_pct >= 100) return 1.0f;
    if (volume_pct <= 0) return 0.0f;
    return powf(volume_pct / 100.0f, 1.5f);   // (perceptual, as the sensor's: 50 % sounds about half as loud)
}

// ------------------------------------------------------------------ music voices
// Every ADPCM voice the game acquires is kept with its coefficient table's hash, music or not: a stream's voices can be
// acquired from its prefetch before the stream file is read, so a table noted later still turns its voices into music.
#define VOICE_CAP 1024
typedef struct {
    uint64_t voice, hash;
    float asked;     // the volume the game set (1 until it sets one: nn::audio's start value)
    float applied;   // the gain the voice's volume carries now (1: as the game set it)
    uint8_t music;
} Voice;
static Voice g_voice[VOICE_CAP];
static int g_nvoice;
static float g_seen_gain = 1.0f;   // the gain music_voice_changed last applied

static int voice_index(uint64_t voice) {   // g_lock held
    for (int i = 0; i < g_nvoice; i++)
        if (g_voice[i].voice == voice) return i;
    return -1;
}

static bool hash_known(uint64_t h) {   // g_lock held
    for (uint32_t i = 0, at = (uint32_t)h; i < 64; i++, at++) {
        uint64_t s = g_coef[at % COEF_CAP];
        if (s == h) return true;
        if (!s) return false;
    }
    return false;
}

void music_voice_set(uint64_t voice, const int16_t *coefs) {
    pthread_mutex_lock(&g_lock);
    int i = voice_index(voice);
    if (!coefs || all_zero((const uint8_t *)coefs)) {   // not ADPCM: a slot reused by another kind of voice
        if (i >= 0) g_voice[i] = g_voice[--g_nvoice];
    } else {
        if (i < 0 && g_nvoice < VOICE_CAP) i = g_nvoice++;
        if (i >= 0) {
            uint64_t h = coef_hash((const uint8_t *)coefs);
            g_voice[i] = (Voice){voice, h, 1.0f, 1.0f, (uint8_t)hash_known(h)};
            __atomic_store_n(&g_dirty, 1, __ATOMIC_RELAXED);   // its start volume may need the gain
            static int logged;
            if (g_voice[i].music && logged++ < 16)
                LOG("[music] voice %llx acquired as music (%d ADPCM voices tracked)\n", (unsigned long long)voice, g_nvoice);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void music_voice_drop(uint64_t voice) {
    pthread_mutex_lock(&g_lock);
    int i = voice_index(voice);
    if (i >= 0) g_voice[i] = g_voice[--g_nvoice];
    pthread_mutex_unlock(&g_lock);
}

int music_voice_volume(uint64_t voice, float asked, float *out) {
    pthread_mutex_lock(&g_lock);
    int i = voice_index(voice), music = 0;
    if (i >= 0) {
        Voice *v = &g_voice[i];
        if (!v->music) v->music = (uint8_t)hash_known(v->hash);
        v->asked = asked;   // (set as asked when it is not music: the game's own volume)
        if ((music = v->music)) v->applied = music_gain(g_settings.music_volume), *out = asked * v->applied;
    }
    pthread_mutex_unlock(&g_lock);
    return music;
}

int music_voice_changed(uint64_t *voice, float *vol, int max) {
    float g = music_gain(g_settings.music_volume);
    // (read without the lock: a change is seen at a later update at worst)
    if (g == g_seen_gain && !__atomic_load_n(&g_dirty, __ATOMIC_RELAXED)) return 0;
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_dirty, 0, __ATOMIC_RELAXED);
    int n = 0;
    for (int i = 0; i < g_nvoice; i++) {
        Voice *v = &g_voice[i];
        if (!v->music) v->music = (uint8_t)hash_known(v->hash);
        if (!v->music || v->applied == g) continue;
        if (n == max) {   // the rest at the next update
            __atomic_store_n(&g_dirty, 1, __ATOMIC_RELAXED);
            break;
        }
        voice[n] = v->voice, vol[n++] = v->asked * g, v->applied = g;
    }
    int changed = g != g_seen_gain;
    g_seen_gain = g;
    pthread_mutex_unlock(&g_lock);
    if (changed) LOG("[music] volume %d%%: %d music voice(s) set again\n", g_settings.music_volume, n);
    return n;
}

// Voices are guest pointers: forgotten at a reboot (the noted coefficient tables describe files and stay).
RT_ON_RESET(music_voices_reset) {
    pthread_mutex_lock(&g_lock);
    g_nvoice = 0;
    g_seen_gain = 1.0f, g_dirty = 0;
    pthread_mutex_unlock(&g_lock);
}
