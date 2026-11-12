// amiibo without figures: the figures (the player's dumps), what they hold and their game data. See amiibo.h.
//
// A dump's settings and game data are encrypted, as on the figure: with the amiibo keys (key_retail.bin in the amiibo
// folder: "unfixed infos" and "locked secret", 80 bytes each) they are read as amiitool does - the tag's pages put in
// its internal order, a key, IV and HMAC key from each master key and a seed of the tag (HMAC-SHA256 as a DRBG), the
// data deciphered with AES-128-CTR and both HMACs checked. Without the keys a figure is read without them: no owner,
// no game data.
#include "amiibo.h"

#include <dirent.h>
#include <libavutil/aes.h>
#include <libavutil/hmac.h>
#include <libavutil/mem.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"
#include "ui/i18n.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define MAX_FIGURES 8192

// ---- HMAC-SHA256 and AES-128 in CTR mode, from FFmpeg's libavutil (linked anyway, for the movies)
// HMAC-SHA256 of two pieces (a then b) under a 16-byte key; false if libavutil can't allocate
static bool hmac(const uint8_t key[16], const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]) {
    AVHMAC *h = av_hmac_alloc(AV_HMAC_SHA256);
    if (!h) return false;
    av_hmac_init(h, key, 16);
    av_hmac_update(h, a, (unsigned)an);
    if (bn) av_hmac_update(h, b, (unsigned)bn);
    bool ok = av_hmac_final(h, out, 32) == 32;
    av_hmac_free(h);
    return ok;
}
// p (n bytes) XORed with the key stream: the IV is the whole 16-byte counter, counting up big-endian
static bool aes_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *p, size_t n) {
    struct AVAES *aes = av_aes_alloc();
    if (!aes || av_aes_init(aes, key, 128, 0) < 0) { av_free(aes); return false; }
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        av_aes_crypt(aes, ks, ctr, 1, NULL, 0);
        for (size_t i = 0; i < 16 && o + i < n; i++) p[o + i] ^= ks[i];
        for (int i = 15; i >= 0 && !++ctr[i]; i--) {}
    }
    av_free(aes);
    return true;
}

// ---- the figures' data (amiitool's nfc3d)
typedef struct { uint8_t hmac_key[16]; char type[14]; uint8_t rfu, magic_size, magic[16], xor_pad[32]; } MasterKey;
static MasterKey g_keys[2];  // "unfixed infos" (data), "locked secret" (tag)
static bool g_have_keys;

// The tag's pages (0x208 of them) in the internal order the keys and HMACs go by
static void tag_to_internal(const uint8_t *t, uint8_t *i) {
    memcpy(i + 0x000, t + 0x008, 0x008), memcpy(i + 0x008, t + 0x080, 0x020), memcpy(i + 0x028, t + 0x010, 0x024);
    memcpy(i + 0x04c, t + 0x0a0, 0x168), memcpy(i + 0x1b4, t + 0x034, 0x020), memcpy(i + 0x1d4, t + 0x000, 0x008);
    memcpy(i + 0x1dc, t + 0x054, 0x02c);
}
// A key, IV and HMAC key (16 bytes each) from a master key and the tag's seed (false: libavutil failed)
static bool keygen(const MasterKey *m, const uint8_t *in, uint8_t out[48]) {
    uint8_t seed[64], prep[128], *p = prep, blk[32];
    memcpy(seed, in + 0x029, 2), memset(seed + 2, 0, 14), memcpy(seed + 16, in + 0x1d4, 8), memcpy(seed + 24, in + 0x1d4, 8);
    memcpy(seed + 32, in + 0x1e8, 32);
    size_t tl = strnlen(m->type, sizeof m->type);
    memcpy(p, m->type, tl), p += tl, *p++ = 0;
    int lead = 16 - (m->magic_size > 16 ? 16 : m->magic_size);
    memcpy(p, seed, (size_t)lead), p += lead;
    memcpy(p, m->magic, (size_t)(16 - lead)), p += 16 - lead;
    memcpy(p, seed + 16, 16), p += 16;
    for (int i = 0; i < 32; i++) *p++ = seed[32 + i] ^ m->xor_pad[i];
    for (int c = 0, o = 0; o < 48; c++, o += 32) {
        uint8_t ctr[2] = {(uint8_t)(c >> 8), (uint8_t)c};
        if (!hmac(m->hmac_key, ctr, 2, prep, (size_t)(p - prep), blk)) return false;
        memcpy(out + o, blk, o + 32 <= 48 ? 32 : 48 - o);
    }
    return true;
}

static void keys_read(const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s" SEP "key_retail.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    g_have_keys = fread(g_keys, 1, sizeof g_keys, f) == sizeof g_keys && g_keys[0].magic_size <= 16 && g_keys[1].magic_size <= 16;
    fclose(f);
    LOG("[amiibo] amiibo keys: %s\n", g_have_keys ? "read" : "not valid");
}

bool amiibo_read_data(const Amiibo *a, AmiiboData *d) {
    memset(d, 0, sizeof *d);
    if (!g_have_keys || !a->path) return false;
    uint8_t tag[0x21c] = {0}, in[0x208], plain[0x208], dk[48], tk[48], h[32];
    FILE *f = fopen(a->path, "rb");
    if (!f) return false;
    size_t n = fread(tag, 1, sizeof tag, f);
    fclose(f);
    if (n < 0x208) return false;
    tag_to_internal(tag, in);
    memcpy(plain, in, sizeof plain);
    if (!keygen(&g_keys[0], in, dk) || !keygen(&g_keys[1], in, tk) || !aes_ctr(dk, dk + 16, plain + 0x02c, 0x188) ||
        !hmac(tk + 32, plain + 0x1d4, 0x34, NULL, 0, h)) {
        LOG("[amiibo] %s: libavutil failed\n", a->name);
        return false;
    }
    bool ok = !memcmp(h, in + 0x1b4, 32);
    memcpy(plain + 0x1b4, h, 32);
    ok = ok && hmac(dk + 32, plain + 0x029, 0x1df, NULL, 0, h) && !memcmp(h, in + 0x008, 32);
    if (!ok) { LOG("[amiibo] %s: the keys don't fit its data\n", a->name); return false; }
    d->flags = plain[0x2c];
    for (int i = 0; i < 10; i++) d->nickname[i] = (uint16_t)(plain[0x38 + 2 * i] << 8 | plain[0x39 + 2 * i]);
    d->app_id = (uint32_t)plain[0xb6] << 24 | plain[0xb7] << 16 | plain[0xb8] << 8 | plain[0xb9];
    memcpy(d->area, plain + 0xdc, sizeof d->area);
    return true;
}

static Amiibo *g_figs;
static int g_nfigs;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

// One dump: an NTAG215 image (540 bytes; 532 and 572 too), its serial number in pages 0-1 and the amiibo ID, not
// encrypted, at 0x54
static void read_dump(const char *path, const char *name, const char *series) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t d[0x5c];
    size_t n = fread(d, 1, sizeof d, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (n < sizeof d || (size != 532 && size != 540 && size != 572) || d[0] != 0x04 || d[0x5b] != 0x02) return;
    uint8_t uid[7] = {d[0], d[1], d[2], d[4], d[5], d[6], d[7]};
    for (int i = 0; i < g_nfigs; i++)
        if (!memcmp(g_figs[i].id, d + 0x54, 8) && !memcmp(g_figs[i].uid, uid, 7)) return;  // (the same tag twice)
    if (g_nfigs >= MAX_FIGURES) return;
    Amiibo *a = &g_figs[g_nfigs++];
    char nm[256];
    snprintf(nm, sizeof nm, "%s", name);
    char *dot = strrchr(nm, '.');
    if (dot) *dot = 0;
    a->name = copy(nm), a->series = copy(series), a->path = copy(path);
    memcpy(a->id, d + 0x54, 8), memcpy(a->uid, uid, 7);
}

// The dumps in dir and its folders (series: the folder under the amiibo folder, "" for its own files)
static void scan(const char *dir, const char *series, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s" SEP "%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan(p, depth ? series : e->d_name, depth + 1);
            continue;
        }
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".bin")) read_dump(p, e->d_name, series);
    }
    closedir(d);
}


static int by_series_name(const void *x, const void *y) {
    const Amiibo *a = x, *b = y;
    int s = strcmp(a->series, b->series);
    return s ? s : strcmp(a->name, b->name);
}

static void list_read(void) {
    g_figs = calloc(MAX_FIGURES, sizeof *g_figs);
    if (!g_figs) return;
    const char *dir = amiibo_folder();
    mkdir(dir, 0755);  // (made for the player to drop dumps into)
    keys_read(dir);
    scan(dir, "", 0);
    qsort(g_figs, (size_t)g_nfigs, sizeof *g_figs, by_series_name);
    for (int i = 0; i < g_nfigs; i++) {
        ui_register_glyphs(g_figs[i].name);
        ui_register_glyphs(g_figs[i].series);
    }
    LOG("[amiibo] %d figures in %s\n", g_nfigs, dir);
}

int amiibo_count(void) { pthread_once(&g_once, list_read); return g_nfigs; }
const Amiibo *amiibo_at(int i) { return i >= 0 && i < amiibo_count() ? &g_figs[i] : NULL; }

const char *amiibo_folder(void) {
    static char dir[1100];
    if (!dir[0]) snprintf(dir, sizeof dir, "%s" SEP "amiibo", rt_data_root());
    return dir;
}

bool amiibo_has_keys(void) { pthread_once(&g_once, list_read); return g_have_keys; }

bool amiibo_key(int i, char *out, size_t n) {
    const Amiibo *a = amiibo_at(i);
    if (!a) return false;
    snprintf(out, n, "%s%s%s", a->series[0] ? a->series : "", a->series[0] ? "/" : "", a->name);
    return true;
}

int amiibo_on_reader(void) {
    if (!g_settings.amiibo_reader[0]) return -1;
    char k[256];
    for (int i = 0, n = amiibo_count(); i < n; i++)
        if (amiibo_key(i, k, sizeof k) && !strcmp(k, g_settings.amiibo_reader)) return i;
    return -1;
}

// ---- game data: <data>/amiibo_data/<amiibo ID>-<serial>.bin, the access ID (4 bytes, little-endian) then the data
static void area_path(const Amiibo *a, char *p, size_t n) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s" SEP "amiibo_data", rt_data_root());
    mkdir(dir, 0755);
    int k = snprintf(p, n, "%s" SEP, dir);
    for (int i = 0; i < 8 && k < (int)n; i++) k += snprintf(p + k, n - k, "%02x", a->id[i]);
    k += snprintf(p + k, n - k, "-");
    for (int i = 0; i < 7 && k < (int)n; i++) k += snprintf(p + k, n - k, "%02x", a->uid[i]);
    snprintf(p + k, n - k, ".bin");
}

int amiibo_area_load(const Amiibo *a, uint32_t *access_id, uint8_t *buf, int cap) {
    char p[1300];
    area_path(a, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) {  // (none written here yet: the figure's own, from its dump)
        AmiiboData d;
        if (!amiibo_read_data(a, &d) || !(d.flags & AMIIBO_HAS_AREA)) return -1;
        int k = cap < (int)sizeof d.area ? cap : (int)sizeof d.area;
        memcpy(buf, d.area, (size_t)k);
        *access_id = d.app_id;
        return k;
    }
    uint8_t h[4];
    int n = fread(h, 1, 4, f) == 4 ? (int)fread(buf, 1, (size_t)cap, f) : -1;
    fclose(f);
    if (n >= 0) *access_id = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
    return n;
}

bool amiibo_area_save(const Amiibo *a, uint32_t access_id, const uint8_t *buf, int size) {
    char p[1300];
    area_path(a, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    uint8_t h[4] = {(uint8_t)access_id, (uint8_t)(access_id >> 8), (uint8_t)(access_id >> 16), (uint8_t)(access_id >> 24)};
    bool ok = fwrite(h, 1, 4, f) == 4 && fwrite(buf, 1, (size_t)size, f) == (size_t)size;
    ok = fclose(f) == 0 && ok;
    LOG("[amiibo] %s's game data saved (%d bytes)%s\n", a->name, size, ok ? "" : ": failed");
    return ok;
}
