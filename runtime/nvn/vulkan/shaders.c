// Translated NVN programs (with their cache), shader modules, descriptor layouts from the SPIR-V itself, built-in
// shaders. GLSL -> SPIR-V is in spirv.c.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "../shader_archive.h"
#include "shaders.h"

// What the translated shaders depend on besides their binaries: the translator's sources (maxwell_translator_id)
// and the GLSL -> SPIR-V step's (vk_spirv_id). Caches built from translated shaders (cache/xlate, the pipeline cache,
// the pipeline list) are valid for one value of this only; both ids are hashes of those sources (Makefile), so
// builds that do not change them keep the caches.
const char *vk_shader_version(void) {
    static char v[64];
    if (!v[0]) snprintf(v, sizeof v, "%s/%s", maxwell_translator_id(), vk_spirv_id());
    return v;
}

// The shader and pipeline caches live in cache/ under the data root (the working directory), or in the folder
// SAVAGEWIND_CACHE_DIR names (tests that must not touch the player's caches).
const char *vk_cache_dir(void) {
    static const char *dir;
    if (!dir) {
        const char *e = getenv("SAVAGEWIND_CACHE_DIR");
        dir = e && *e ? e : "cache";
        mkdir(dir, 0755);
    }
    return dir;
}

void vk_cache_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", vk_cache_dir(), name);
}

// ---- translated shaders (Maxwell -> GLSL): 1-4 ms each in the open world, on the GPU thread while compiles are
// synchronous, and programs share their vertex and fragment shaders. Kept in memory by stage, binary and key, and on
// disk as cache/xlate/<hash>.bin, so a shader seen in an earlier session costs a file read. The hash covers the bytes
// the translator reads, the key and the translator's build (a new build of translate.c starts over).
// The hash is also the translation's id (vk_translate_id): the pipeline list (exec.c) names shaders by it.
typedef struct { char magic[4]; uint32_t info_size, msl_len, bin_size; ShaderKey key; int32_t stage; } XlateHeader;
#define XLATE_TAB 16384
static struct { uint64_t h; ShaderInfo *si; } xlate_tab[XLATE_TAB];
static struct { const ShaderInfo *si; uint64_t h; } xlate_ids[XLATE_TAB];  // the other way round
static pthread_mutex_t xlate_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t hash64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

// want: the header the file must have (all but msl_len), or NULL to take the one in the file (by id: the hash
// covers it) when it is sane and of that stage.
// A file: the header, the ShaderInfo, the GLSL's deflated size and the GLSL deflated (zlib: a sixth of the size). In
// cache/xlate/<id>/ (the translator's id, vk_cache_dir_id): another build's folder is removed (vk_cache_cleanup).
static void xlate_path(char *path, size_t n, uint64_t h) {
    char name[96];
    snprintf(name, sizeof name, "xlate/%s/%016llx.xlz", vk_cache_dir_id(maxwell_translator_id()), (unsigned long long)h);
    vk_cache_path(path, n, name);
}

static ShaderInfo *xlate_load(uint64_t h, const XlateHeader *want, int stage) {
    char path[512];
    xlate_path(path, sizeof path, h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    XlateHeader hd;
    ShaderInfo *si = NULL;
    bool ok = fread(&hd, sizeof hd, 1, f) == 1 && hd.msl_len < (64u << 20);
    if (ok && want)
        ok = !memcmp(&hd, want, offsetof(XlateHeader, msl_len)) && hd.bin_size == want->bin_size &&
             !memcmp(&hd.key, &want->key, sizeof hd.key) && hd.stage == want->stage;
    else if (ok)
        ok = !memcmp(hd.magic, "SWX2", 4) && hd.info_size == sizeof(ShaderInfo) && hd.stage == stage && hd.msl_len;
    uint32_t zn = 0;
    if (ok) {
        si = malloc(sizeof *si);
        char *msl = malloc(hd.msl_len + 1);
        uint8_t *z = NULL;
        bool got = si && msl && fread(si, sizeof *si, 1, f) == 1 && fread(&zn, 4, 1, f) == 1 && zn < (64u << 20) &&
                   (z = malloc(zn ? zn : 1)) && fread(z, 1, zn, f) == zn && si->stage == hd.stage;
        uLongf len = hd.msl_len;
        if (got && uncompress((Bytef *)msl, &len, z, zn) == Z_OK && len == hd.msl_len) {
            msl[hd.msl_len] = 0;
            si->msl = msl;
        } else {
            free(si);
            free(msl);
            si = NULL;
        }
        free(z);
    }
    fclose(f);
    return si;
}

// Into the tables (xlate_lock held): the first translation stored under h wins, so that every caller gets the
// same ShaderInfo for the same id. Returns that one (si freed when another was there first).
static ShaderInfo *xlate_keep(uint64_t h, ShaderInfo *si) {
    uint32_t slot = (uint32_t)h & (XLATE_TAB - 1), probes = 0;
    while (xlate_tab[slot].h && xlate_tab[slot].h != h && probes++ < XLATE_TAB) slot = (slot + 1) & (XLATE_TAB - 1);
    if (xlate_tab[slot].h == h) {
        if (xlate_tab[slot].si != si) maxwell_free(si);
        return xlate_tab[slot].si;
    }
    if (xlate_tab[slot].h) return si;  // (full: not kept)
    xlate_tab[slot].h = h;
    xlate_tab[slot].si = si;
    uint32_t r = (uint32_t)(((uintptr_t)si >> 4) * 0x9E3779B1u) & (XLATE_TAB - 1);
    for (probes = 0; xlate_ids[r].si && probes < XLATE_TAB; probes++) r = (r + 1) & (XLATE_TAB - 1);
    if (!xlate_ids[r].si) { xlate_ids[r].si = si; xlate_ids[r].h = h; }
    return si;
}

uint64_t vk_translate_id(const ShaderInfo *si) {
    uint64_t h = 0;
    pthread_mutex_lock(&xlate_lock);
    uint32_t r = (uint32_t)(((uintptr_t)si >> 4) * 0x9E3779B1u) & (XLATE_TAB - 1);
    for (uint32_t probes = 0; xlate_ids[r].si && probes < XLATE_TAB; probes++, r = (r + 1) & (XLATE_TAB - 1))
        if (xlate_ids[r].si == si) { h = xlate_ids[r].h; break; }
    pthread_mutex_unlock(&xlate_lock);
    return h;
}

ShaderInfo *vk_translate_load_id(uint64_t id, int stage) {
    if (!id) return NULL;
    pthread_mutex_lock(&xlate_lock);
    uint32_t slot = (uint32_t)id & (XLATE_TAB - 1), probes = 0;
    while (xlate_tab[slot].h && xlate_tab[slot].h != id && probes++ < XLATE_TAB) slot = (slot + 1) & (XLATE_TAB - 1);
    ShaderInfo *si = xlate_tab[slot].h == id ? xlate_tab[slot].si : NULL;
    pthread_mutex_unlock(&xlate_lock);
    if (si) return si->stage == stage ? si : NULL;
    si = xlate_load(id, NULL, stage);
    if (!si) return NULL;
    pthread_mutex_lock(&xlate_lock);
    si = xlate_keep(id, si);
    pthread_mutex_unlock(&xlate_lock);
    return si;
}

static void xlate_store(uint64_t h, const XlateHeader *hd, const ShaderInfo *si) {
    static int made;
    char path[512], tmp[540], name[96];
    if (!made) {
        made = 1;
        vk_cache_path(path, sizeof path, "xlate");
        mkdir(path, 0755);
        snprintf(name, sizeof name, "xlate/%s", vk_cache_dir_id(maxwell_translator_id()));
        vk_cache_path(path, sizeof path, name);
        mkdir(path, 0755);
    }
    xlate_path(path, sizeof path, h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)si);
    uLongf zn = compressBound(hd->msl_len);
    uint8_t *z = malloc(zn);
    if (!z) return;
    bool ok = compress2(z, &zn, (const Bytef *)si->msl, hd->msl_len, 1) == Z_OK;
    ShaderInfo copy = *si;
    copy.msl = NULL;
    uint32_t zn32 = (uint32_t)zn;
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    ok = f && fwrite(hd, sizeof *hd, 1, f) == 1 && fwrite(&copy, sizeof copy, 1, f) == 1 && fwrite(&zn32, 4, 1, f) == 1 &&
         fwrite(z, 1, zn, f) == zn;
    if (f && fclose(f)) ok = false;
    free(z);
    if (f && (!ok || rename(tmp, path) != 0)) remove(tmp);  // (another thread may have stored it first)
}

// A cache folder's name for an id (TRANSLATOR_ID / SPIRV_ID are 16 hex digits; built without the Makefile, a date)
const char *vk_cache_dir_id(const char *id) {
    static __thread char out[2][20];
    static __thread int k;
    char *o = out[k++ & 1];
    bool hex = strlen(id) == 16;
    for (const char *p = id; *p && hex; p++) hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
    if (hex) snprintf(o, 20, "%s", id);
    else snprintf(o, 20, "%016llx", (unsigned long long)hash64(0xcbf29ce484222325ull, id, strlen(id)));
    return o;
}

// The translations and SPIR-V of other builds (other ids) and of the versions before the folders by id: removed, so
// that updates do not pile them up (the whole game's precompiled shaders are ~0.4 GB). Runs on a thread of its own.
static void *cleanup_thread(void *arg) {
    (void)arg;
    const char *keep[2] = {vk_cache_dir_id(maxwell_translator_id()), vk_cache_dir_id(vk_spirv_id())};
    static const char *const sub[2] = {"xlate", "spirv"};
    int removed = 0;
    for (int i = 0; i < 2; i++) {
        char dir[512];
        vk_cache_path(dir, sizeof dir, sub[i]);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, keep[i])) continue;
            char path[800];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(path, &st)) continue;
            if (S_ISDIR(st.st_mode) ? plat_remove_tree(path) : !remove(path)) removed++;
        }
        closedir(d);
    }
    if (removed) LOG("[shader] removed %d shader cache entries of other builds\n", removed);
    return NULL;
}

void vk_cache_cleanup(void) {
    if (getenv("SAVAGEWIND_CACHE_KEEP")) return;  // (a cache folder shared by builds of different translators)
    pthread_t t;
    if (!pthread_create(&t, NULL, cleanup_thread, NULL)) pthread_detach(t);
}

// ---- shader keys. A translation depends on its key only through what the shader uses: the vertex attributes a vertex
// shader reads (their kinds: float, int, uint), the inputs a fragment shader reads (which of them the vertex shader
// provides), the color targets it writes (float, int, uint). vk_key_normalize clears the rest, from the Shader
// Program Header, so that one translation serves draws that differ in state the shader ignores, and so that the key
// a shader gets can be told ahead of time (precompile.c).
uint32_t vk_sph_inputs(const uint8_t *code, int stage) {
    const uint8_t *sph = code + 0x30;
    uint32_t m = 0;
    for (int i = 0; i < 32; i++) {
        // ImapGenericVector at 0x18: 4 bits (x, y, z, w) per attribute in vertex shaders, 2 bits per component in
        // fragment shaders
        bool used = stage == STAGE_VERTEX ? (sph[0x18 + i / 2] >> ((i & 1) * 4)) & 0xF : sph[0x18 + i] != 0;
        if (used) m |= 1u << i;
    }
    return m;
}

void vk_key_normalize(const uint8_t *code, int stage, ShaderKey *k) {
    if (stage == STAGE_VERTEX) {
        uint32_t in = vk_sph_inputs(code, stage);
        for (int i = 0; i < 32; i++)
            if (!((in >> i) & 1)) k->attr_kind[i] = 0;
        memset(k->rt_kind, 0, sizeof k->rt_kind);
        k->vs_out_mask = 0;
        k->shared_size = 0;
        k->mvec_rt = 0;
    } else if (stage == STAGE_FRAGMENT) {
        uint32_t omap;
        memcpy(&omap, code + 0x30 + 0x48, 4);  // (4 bits per color target)
        for (int rt = 0; rt < 8; rt++)
            if (!((omap >> (rt * 4)) & 0xF)) k->rt_kind[rt] = 0;
        k->vs_out_mask &= vk_sph_inputs(code, stage);
        memset(k->attr_kind, 0, sizeof k->attr_kind);
        k->flip_y = k->depth_minus_one = false;
        k->shared_size = 0;
        k->mvec = 0;
    } else {
        uint32_t shared = k->shared_size;
        bool glsl = k->glsl;
        memset(k, 0, sizeof *k);
        k->shared_size = shared;
        k->glsl = glsl;
    }
}

// ---- shader key log: cache/vk_shader_keys.list, every (shader binary, key) translated, the binary by
// shader_code_hash (runtime/nvn/shader_archive.c). It does not depend on the translator: the precompilation
// (precompile.c) reads it to know which keys this player's game uses, also after a translator update. One record per
// pair (dropped repeats and damaged records are rewritten away when it is read); new ones are appended by
// vk_keylog_flush (device.c's saver thread, exit).
typedef struct { char magic[8]; uint32_t version, rec_size, key_size, reserved; } KeyLogHeader;
typedef struct { uint64_t code; uint32_t stage, bin_size; ShaderKey key; uint32_t check, reserved; } KeyLogRec;
static pthread_mutex_t keylog_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *keylog_known;
static size_t keylog_cap, keylog_n;
static KeyLogRec *keylog_all, *keylog_pend;
static int keylog_nall, keylog_capall, keylog_npend, keylog_cappend;
static bool keylog_loaded;

static void keylog_header(KeyLogHeader *h) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "SWKEYS01", 8);
    h->version = 1;
    h->rec_size = sizeof(KeyLogRec);
    h->key_size = sizeof(ShaderKey);
}

static uint64_t keylog_hash(const KeyLogRec *r) {
    uint64_t h = hash64(0x9E3779B97F4A7C15ull, &r->code, 16);
    h = hash64(h, &r->key, sizeof r->key);
    return h ? h : 1;
}

static bool keylog_add_known(uint64_t h) {  // (keylog_lock held) false: there already
    if ((keylog_n + 1) * 2 > keylog_cap) {
        size_t cap = keylog_cap ? keylog_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < keylog_cap; i++) {
            if (!keylog_known[i]) continue;
            size_t j = keylog_known[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = keylog_known[i];
        }
        free(keylog_known);
        keylog_known = t;
        keylog_cap = cap;
    }
    size_t j = h & (keylog_cap - 1);
    for (; keylog_known[j]; j = (j + 1) & (keylog_cap - 1))
        if (keylog_known[j] == h) return false;
    keylog_known[j] = h;
    keylog_n++;
    return true;
}

static void keylog_push(KeyLogRec **a, int *n, int *cap, const KeyLogRec *r) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        *a = realloc(*a, sizeof **a * (size_t)*cap);
    }
    (*a)[(*n)++] = *r;
}

static void keylog_load(void) {  // (keylog_lock held)
    keylog_loaded = true;
    KeyLogHeader want, h;
    keylog_header(&want);
    char path[512];
    vk_cache_path(path, sizeof path, "vk_shader_keys.list");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    int bad = 0, dup = 0;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(&h, &want, sizeof h);
    KeyLogRec r;
    while (ok && fread(&r, sizeof r, 1, f) == 1) {
        uint64_t rh = keylog_hash(&r);
        if (r.check != (uint32_t)rh) bad++;
        else if (!keylog_add_known(rh)) dup++;
        else keylog_push(&keylog_all, &keylog_nall, &keylog_capall, &r);
    }
    fclose(f);
    if (ok && !bad && !dup) return;
    char tmp[540];  // (another format, damaged or repeated records: written again, the good records only)
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE *o = fopen(tmp, "wb");
    bool w = o && fwrite(&want, sizeof want, 1, o) == 1 &&
             fwrite(keylog_all, sizeof *keylog_all, (size_t)keylog_nall, o) == (size_t)keylog_nall;
    if (o && fclose(o)) w = false;
    if (!w || !vk_replace_file(tmp, path)) remove(tmp);
    LOG("[shader] key log: %d records%s (%d damaged, %d repeated dropped)\n", keylog_nall, ok ? "" : ", older format: started over",
        bad, dup);
}

static void keylog_add(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    KeyLogRec r;
    memset(&r, 0, sizeof r);
    r.code = shader_code_hash(code, max_size, stage, &r.bin_size);
    r.stage = (uint32_t)stage;
    r.key = *key;
    uint64_t h = keylog_hash(&r);
    r.check = (uint32_t)h;
    pthread_mutex_lock(&keylog_lock);
    if (!keylog_loaded) keylog_load();
    if (keylog_add_known(h)) {
        keylog_push(&keylog_all, &keylog_nall, &keylog_capall, &r);
        keylog_push(&keylog_pend, &keylog_npend, &keylog_cappend, &r);
    }
    pthread_mutex_unlock(&keylog_lock);
}

void vk_keylog_flush(void) {
    pthread_mutex_lock(&keylog_lock);
    if (keylog_npend) {
        char path[512];
        vk_cache_path(path, sizeof path, "vk_shader_keys.list");
        FILE *f = fopen(path, "ab");
        if (f) {
            fseek(f, 0, SEEK_END);
            bool ok = true;
            if (ftell(f) == 0) {
                KeyLogHeader h;
                keylog_header(&h);
                ok = fwrite(&h, sizeof h, 1, f) == 1;
            }
            ok = ok && fwrite(keylog_pend, sizeof *keylog_pend, (size_t)keylog_npend, f) == (size_t)keylog_npend;
            if (fclose(f)) ok = false;
            if (ok) keylog_npend = 0;
        }
    }
    pthread_mutex_unlock(&keylog_lock);
}

int vk_keylog_read(VkKeyLogEntry **out) {
    pthread_mutex_lock(&keylog_lock);
    if (!keylog_loaded) keylog_load();
    int n = keylog_nall;
    *out = malloc(sizeof **out * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++)
        (*out)[i] = (VkKeyLogEntry){keylog_all[i].code, keylog_all[i].stage, keylog_all[i].bin_size, keylog_all[i].key};
    pthread_mutex_unlock(&keylog_lock);
    return n;
}

// The translation's id (its cache/xlate name) and the header its file has
static uint64_t xlate_id(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key, XlateHeader *hd) {
    memset(hd, 0, sizeof *hd);
    memcpy(hd->magic, "SWX2", 4);
    hd->info_size = sizeof(ShaderInfo);
    hd->bin_size = (uint32_t)maxwell_binary_size(code, max_size, stage);
    hd->key = *key;
    hd->stage = stage;
    const char *id = maxwell_translator_id();
    uint64_t h = hash64(0xcbf29ce484222325ull, id, strlen(id));
    h = hash64(h, hd, sizeof *hd);
    h = hash64(h, code, hd->bin_size);
    return h ? h : 1;
}

// A translation from cache/xlate only, not kept in memory (maxwell_free it): NULL when it isn't there.
ShaderInfo *vk_xlate_read(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    XlateHeader hd;
    uint64_t h = xlate_id(code, max_size, stage, key, &hd);
    return xlate_load(h, &hd, stage);
}

// Shader precompilation (precompile.c): the translation and SPIR-V of a shader binary with a key, into the caches
// only (nothing kept in memory). 1 made (translated or compiled), 0 both were cached, -1 failed.
int vk_precompile_one(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key, char *why, size_t nwhy) {
    XlateHeader hd;
    uint64_t h = xlate_id(code, max_size, stage, key, &hd);
    keylog_add(code, max_size, stage, key);
    ShaderInfo *si = xlate_load(h, &hd, stage);
    bool made = false;
    if (!si) {
        si = maxwell_translate(code, max_size, stage, key);
        if (!si->msl) {
            snprintf(why, nwhy, "translation failed: %s", si->error);
            maxwell_free(si);
            return -1;
        }
        hd.msl_len = (uint32_t)strlen(si->msl);
        xlate_store(h, &hd, si);
        made = true;
    }
    int r = vk_spirv_ensure(si->msl, stage);
    maxwell_free(si);
    if (r < 0) snprintf(why, nwhy, "GLSL to SPIR-V failed");
    return r < 0 ? -1 : made || r > 0;
}

static bool xlate_off(void) {
    static int off = -1;  // SAVAGEWIND_NO_XLATE_CACHE=1: translate every time
    if (off < 0) off = getenv("SAVAGEWIND_NO_XLATE_CACHE") != NULL;
    return off || maxwell_translation_uncacheable();
}

ShaderInfo *vk_translate_peek(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    if (xlate_off()) return NULL;
    XlateHeader hd;
    uint64_t h = xlate_id(code, max_size, stage, key, &hd);
    pthread_mutex_lock(&xlate_lock);
    uint32_t slot = (uint32_t)h & (XLATE_TAB - 1), probes = 0;
    while (xlate_tab[slot].h && xlate_tab[slot].h != h && probes++ < XLATE_TAB) slot = (slot + 1) & (XLATE_TAB - 1);
    ShaderInfo *si = xlate_tab[slot].h == h ? xlate_tab[slot].si : NULL;
    pthread_mutex_unlock(&xlate_lock);
    return si;
}

ShaderInfo *vk_translate_cached(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    if (xlate_off()) return maxwell_translate(code, max_size, stage, key);
    XlateHeader hd;
    uint64_t h = xlate_id(code, max_size, stage, key, &hd);
    pthread_mutex_lock(&xlate_lock);
    uint32_t slot = (uint32_t)h & (XLATE_TAB - 1), probes = 0;
    while (xlate_tab[slot].h && xlate_tab[slot].h != h && probes++ < XLATE_TAB) slot = (slot + 1) & (XLATE_TAB - 1);
    ShaderInfo *si = xlate_tab[slot].h == h ? xlate_tab[slot].si : NULL;
    pthread_mutex_unlock(&xlate_lock);
    if (si) return si;
    keylog_add(code, max_size, stage, key);
    si = xlate_load(h, &hd, stage);
    if (si && getenv("SAVAGEWIND_XLATE_VERIFY")) {  // (debugging) the cached translation is the one the translator gives
        ShaderInfo *f = maxwell_translate(code, max_size, stage, key);
        ShaderInfo a = *si, b = *f;
        a.msl = b.msl = NULL;
        bool same = f->msl && !strcmp(f->msl, si->msl) && !memcmp(&a, &b, sizeof a);
        static int nlog;
        if (!same && nlog++ < 20) {
            size_t i = 0;
            while (f->msl && si->msl[i] && si->msl[i] == f->msl[i]) i++;
            LOG("[xlate] cached translation differs (stage %d, %zu / %zu bytes, first at %zu, struct %s)\n", stage, strlen(si->msl),
                f->msl ? strlen(f->msl) : 0, i, memcmp(&a, &b, sizeof a) ? "differs" : "same");
        }
        maxwell_free(f);
    }
    if (!si) {
        si = maxwell_translate(code, max_size, stage, key);
        if (!si->msl) return si;
        hd.msl_len = (uint32_t)strlen(si->msl);
        xlate_store(h, &hd, si);
    }
    pthread_mutex_lock(&xlate_lock);
    si = xlate_keep(h, si);
    pthread_mutex_unlock(&xlate_lock);
    return si;
}

// (testing) SAVAGEWIND_VK_SPIRV_SALT=<n>: every module's SPIR-V generator word made n: the same shaders, binaries the
// driver has never seen - its own shader cache misses them all, as on a first start or in a place never visited.
VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    static long salt = -1;
    if (salt < 0) salt = getenv("SAVAGEWIND_VK_SPIRV_SALT") ? atol(getenv("SAVAGEWIND_VK_SPIRV_SALT")) : 0;
    uint32_t *copy = NULL;
    if (salt && nwords > 5 && (copy = malloc(nwords * 4))) {
        memcpy(copy, spv, nwords * 4);
        copy[2] = (uint32_t)salt;
        spv = copy;
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    bool ok = vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS;
    free(copy);
    return ok ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
// Temporal anti-aliasing (Video > Anti-aliasing TAA, exec.c mvec_taa): AMD FSR 3.1's upscaler's reconstruction at the
// scene's own size, as compute passes (8x8 groups; bindings 0-7 sampled, linear / clamp, 8-15 storage images). Push
// constants: size (w, h, 1 / w, 1 / h); jit (this frame's jitter J, then the previous frame's, in pixels, y down: pixel
// p's sample is at p + 0.5 - J); cam (A, B: view depth Z = B / (A + ndc), ndc = cam.z * depth + cam.w); st (x: 1 when
// last frame's images hold history, y: the jitter's phase count, zw: the pass's own size where it isn't the scene's).
// Motion vectors here are UV offsets to where the pixel was (prepare turns mvec_fill's NDC into them).
//   - taa_prepare: per pixel, the depth and vector of the nearest surface in its 3x3 (.xy; .z its view depth, negative
//     when it has no vector; .w the farthest's), its luma, and the nearest depth scattered (atomic min of the float's
//     bits) to the 2x2 it lands on in last frame's image: what was in front where.
//   - taa_shade_src / taa_shade_down / taa_shade: shading change. This frame's 5 lumas around the pixel against last
//     frame's around where it was, both sorted: the smallest relative difference between the two sets (0 when any two
//     match), so jitter and sub-pixel motion, which reorder the values, don't count; averaged to half, quarter and
//     eighth size, |mean difference x mean sign| (a change in one direction: lighting) at the strongest of the three.
//   - taa_reactivity: disocclusion (no tap of last frame's nearest depths is this surface or behind it, by more than a
//     distance-scaled separation), motion divergence (what is now where the pixel was moves differently), the reactive
//     mask (3x3 max of the vectors' .z: blended effects) with shading change, and the accumulation: last frame's at
//     where the pixel was, 0 on a shading change, -1/3 on a disocclusion (two frames of the new image alone), 1/3 more
//     a frame. Thin features: a luma ridge in the 3x3 that fills no 2x2 quadrant around the pixel (branches, wires).
//   - taa_instability: the last 4 lumas along the motion; a pixel nearer an older one than the last (it oscillates, as
//     sub-pixel leaves do with the jitter) keeps its history from the clamp.
//   - taa_resolve: the history (Lanczos 2, deringed to its 4 nearest texels) clipped to an ellipsoid in YCoCg around
//     the 3x3's Gaussian-weighted mean, its radii 1-3 standard deviations (tight with motion, near geometry, little
//     accumulation, reactive or changed shading; luma 1.7x) unless unstable or locked; the new sample a Lanczos 2 of the
//     3x3 at their jittered distances, deringed to the 3x3, whose weight (0.74 / 16 for a typical frame's) against the
//     accumulation sets the blend: about 1/23 on a converged still pixel. The thin-feature lock lives in the history's
//     .a: it needs two frames of detection to count, fades over the jitter sequence and dies with disocclusion.
//     st.z (SAVAGEWIND_TAA_VIEW): the output shows a mask in magenta over grey: 1 disocclusion, 2 accumulation, 3 shading
//     change, 4 luma instability, 5 lock, 6 motion divergence, 7 no motion vector.
static const char taa_common[] =
        "#version 450\n"
        "layout(local_size_x = 8, local_size_y = 8) in;\n"
        "layout(push_constant) uniform P { vec4 size; vec4 jit; vec4 cam; vec4 st; } pc;\n"
        "const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
        "const float FP16_MIN = 6.10e-05;\n"
        "vec3 ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
        "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
        "float v4k(vec2 mv) { return length(mv * vec2(3840.0, 2160.0)); }\n"
        "bool inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }\n"
        "float min_over_max(float a, float b, float z) { float m = max(a, b); return m != 0.0 ? min(a, b) / m : z; }\n"
        "ivec2 pixel() { return ivec2(gl_GlobalInvocationID.xy); }\n"
        "ivec2 rsize() { return ivec2(pc.size.xy); }\n"
        "ivec2 clampp(ivec2 q) { return clamp(q, ivec2(0), rsize() - 1); }\n"
        "bool onscreen(ivec2 q) { return all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, rsize())); }\n"
        "float view_z(float d) { return pc.cam.y / (pc.cam.x + pc.cam.z * d + pc.cam.w); }\n";
static const char taa_prepare[] =
        "layout(set = 0, binding = 0) uniform sampler2D color_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D vec_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D depth_t;\n"
        "layout(set = 0, binding = 8, rgba32f) uniform writeonly image2D dil_i;\n"
        "layout(set = 0, binding = 9, r16f) uniform writeonly image2D luma_i;\n"
        "layout(set = 0, binding = 10, r32ui) uniform uimage2D recon_i;\n"
        "void main() {\n"
        "  ivec2 p = pixel();\n"
        "  if (!onscreen(p)) return;\n"
        "  float dn = texelFetch(depth_t, p, 0).r, df = dn;\n"
        "  ivec2 pn = p;\n"
        "  for (int y = -1; y <= 1; y++)\n"
        "    for (int x = -1; x <= 1; x++) {\n"
        "      ivec2 q = clampp(p + ivec2(x, y));\n"
        "      float d = texelFetch(depth_t, q, 0).r;\n"
        "      if (d < dn) { dn = d; pn = q; }\n"
        "      df = max(df, d);\n"
        "    }\n"
        "  vec4 v = texelFetch(vec_t, pn, 0);\n"
        "  bool has = v.a >= 0.25;\n"
        "  vec2 mv = has ? vec2(-0.5 * v.x, 0.5 * v.y) : vec2(0.0);\n"
        "  float zn = view_z(dn);\n"
        "  imageStore(dil_i, p, vec4(mv, has ? zn : -zn, view_z(df)));\n"
        "  imageStore(luma_i, p, vec4(dot(max(texelFetch(color_t, p, 0).rgb, vec3(0.0)), LUMA)));\n"
        "  if (!has) return;\n"
        "  vec2 m = v4k(mv) > mix(0.25, 0.75, clamp(zn / 100.0, 0.0, 1.0)) ? mv : vec2(0.0);\n"
        "  vec2 fp = ((vec2(p) + 0.5) * pc.size.zw + m) * pc.size.xy - 0.5;\n"
        "  ivec2 b = ivec2(floor(fp));\n"
        "  vec2 f = fp - vec2(b);\n"
        "  float w[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);\n"
        "  uint zb = floatBitsToUint(zn);\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    ivec2 q = b + ivec2(i & 1, i >> 1);\n"
        "    if (w[i] > FP16_MIN * 10.0 && onscreen(q)) imageAtomicMin(recon_i, q, zb);\n"
        "  }\n"
        "}\n";
static const char taa_shade_src[] =
        "layout(set = 0, binding = 0) uniform sampler2D dil_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D luma_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D luma_prev_t;\n"
        "layout(set = 0, binding = 8, rg16f) uniform writeonly image2D out_i;\n"
        "const ivec2 PLUS[5] = ivec2[5](ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));\n"
        "void sort5(inout float s[5]) {\n"
        "  const ivec2 N[9] = ivec2[9](ivec2(0, 3), ivec2(1, 4), ivec2(0, 2), ivec2(1, 3), ivec2(0, 1), ivec2(2, 4), ivec2(1, 2),\n"
        "                              ivec2(3, 4), ivec2(2, 3));\n"
        "  for (int k = 0; k < 9; k++) {\n"
        "    float a = s[N[k].x], b = s[N[k].y];\n"
        "    s[N[k].x] = min(a, b);\n"
        "    s[N[k].y] = max(a, b);\n"
        "  }\n"
        "}\n"
        "float set_difference(float a[5], float b[5]) {\n"
        "  sort5(a);\n"
        "  sort5(b);\n"
        "  if (min(a[4], b[4]) <= 1.175494351e-38) return 0.0;\n"
        "  float best = 65504.0;\n"
        "  int i = 0, j = 0;\n"
        "  for (int k = 0; k < 9 && i < 5 && j < 5; k++) {\n"
        "    float d = a[i] - b[j];\n"
        "    if (abs(d) <= FP16_MIN) return 0.0;\n"
        "    d = sign(d) * (1.0 - min_over_max(a[i], b[j], 0.0));\n"
        "    if (abs(d) < abs(best)) best = d;\n"
        "    if (a[i] < b[j]) i++; else j++;\n"
        "  }\n"
        "  return best;\n"
        "}\n"
        "float difference(ivec2 q) {\n"
        "  vec4 d = texelFetch(dil_t, q, 0);\n"
        "  vec2 uv = (vec2(q) + 0.5) * pc.size.zw, ruv = uv + pc.jit.zw * pc.size.zw + d.xy;\n"
        "  if (pc.st.x < 0.5 || d.z < 0.0 || !inside(ruv)) return 0.0;\n"
        "  ivec2 bc = ivec2(floor((uv + pc.jit.xy * pc.size.zw) * pc.size.xy)), bp = ivec2(floor(ruv * pc.size.xy));\n"
        "  float a[5], b[5];\n"
        "  for (int i = 0; i < 5; i++) {\n"
        "    a[i] = max(texelFetch(luma_t, clampp(bc + PLUS[i]), 0).r, FP16_MIN);\n"
        "    b[i] = max(texelFetch(luma_prev_t, clampp(bp + PLUS[i]), 0).r, FP16_MIN);\n"
        "  }\n"
        "  return set_difference(a, b);\n"
        "}\n"
        "void main() {\n"
        "  ivec2 p = pixel(), o = ivec2(pc.st.zw);\n"
        "  if (any(greaterThanEqual(p, o))) return;\n"
        "  vec2 sum = vec2(0.0);\n"
        "  float n = 0.0;\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    ivec2 q = 2 * p + ivec2(i & 1, i >> 1);\n"
        "    if (!onscreen(q)) continue;\n"
        "    float d = difference(q);\n"
        "    sum += vec2(d, d != 0.0 ? sign(d) : 0.0);\n"
        "    n += 1.0;\n"
        "  }\n"
        "  imageStore(out_i, p, vec4(sum / max(n, 1.0), 0.0, 0.0));\n"
        "}\n";
static const char taa_shade_down[] =
        "layout(set = 0, binding = 0) uniform sampler2D in_t;\n"
        "layout(set = 0, binding = 8, rg16f) uniform writeonly image2D out_i;\n"
        "void main() {\n"
        "  ivec2 p = pixel(), o = ivec2(pc.st.zw), s = textureSize(in_t, 0);\n"
        "  if (any(greaterThanEqual(p, o))) return;\n"
        "  vec2 sum = vec2(0.0);\n"
        "  float n = 0.0;\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    ivec2 q = 2 * p + ivec2(i & 1, i >> 1);\n"
        "    if (any(greaterThanEqual(q, s))) continue;\n"
        "    sum += texelFetch(in_t, q, 0).rg;\n"
        "    n += 1.0;\n"
        "  }\n"
        "  imageStore(out_i, p, vec4(sum / max(n, 1.0), 0.0, 0.0));\n"
        "}\n";
static const char taa_shade[] =
        "layout(set = 0, binding = 0) uniform sampler2D mip0_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D mip1_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D mip2_t;\n"
        "layout(set = 0, binding = 8, r8) uniform writeonly image2D out_i;\n"
        "void main() {\n"
        "  ivec2 p = pixel(), o = ivec2(pc.st.zw);\n"
        "  if (any(greaterThanEqual(p, o))) return;\n"
        "  vec2 uv = (vec2(p) + 0.5) / vec2(o) + pc.jit.xy * pc.size.zw;\n"
        "  vec2 m0 = texture(mip0_t, uv).rg, m1 = texture(mip1_t, uv).rg, m2 = texture(mip2_t, uv).rg;\n"
        "  float s = max(abs(m0.x * m0.y), max(abs(m1.x * m1.y), abs(m2.x * m2.y)));\n"
        "  imageStore(out_i, p, vec4(clamp(s, 0.0, 1.0)));\n"
        "}\n";
static const char taa_reactivity[] =
        "layout(set = 0, binding = 0) uniform sampler2D dil_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D vec_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D acc_prev_t;\n"
        "layout(set = 0, binding = 3) uniform sampler2D shade_t;\n"
        "layout(set = 0, binding = 4) uniform sampler2D luma_t;\n"
        "layout(set = 0, binding = 8, r32ui) uniform readonly uimage2D recon_i;\n"
        "layout(set = 0, binding = 9, rgba8) uniform writeonly image2D react_i;\n"
        "layout(set = 0, binding = 10, r8) uniform writeonly image2D acc_i;\n"
        "layout(set = 0, binding = 11, r8) uniform writeonly image2D lock_i;\n"
        "float disocclusion(vec2 uv, vec2 mv, float zc) {\n"
        "  if (v4k(mv) <= mix(0.25, 0.75, clamp(zc / 100.0, 0.0, 1.0))) mv = vec2(0.0);\n"
        "  vec2 fp = (uv + mv) * pc.size.xy - 0.5;\n"
        "  ivec2 b = ivec2(floor(fp));\n"
        "  vec2 f = fp - vec2(b);\n"
        "  float w[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);\n"
        "  float sum = 0.0, wsum = 0.0, sep = 1.37e-05 * length(pc.size.xy * 0.5);\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    ivec2 q = b + ivec2(i & 1, i >> 1);\n"
        "    if (!onscreen(q) || w[i] <= FP16_MIN * 10.0) continue;\n"
        "    float zp = uintBitsToFloat(imageLoad(recon_i, q).r), dz = zc - zp;\n"
        "    if (!(dz > 1.175494351e-38)) return 0.0;\n"
        "    sum += clamp(sep * max(zc, zp) / dz, 0.0, 1.0) * w[i];\n"
        "    wsum += w[i];\n"
        "  }\n"
        "  return wsum > 0.0 ? clamp(1.0 - sum / wsum, 0.0, 1.0) : 0.0;\n"
        "}\n"
        "float motion_divergence(vec2 uv, vec2 mv, float zc) {\n"
        "  vec4 r = texelFetch(dil_t, clampp(ivec2((uv + mv) * pc.size.xy)), 0);\n"
        "  float v = v4k(mv);\n"
        "  if (v <= 0.0) return 0.0;\n"
        "  return (1.0 - clamp(v4k(r.xy) / v, 0.0, 1.0)) * min_over_max(abs(r.z), zc, 0.0) * clamp(v / 10.0, 0.0, 1.0);\n"
        "}\n"
        "float thin_feature(ivec2 p) {\n"
        "  const ivec2 O[9] = ivec2[9](ivec2(0, 0), ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 0), ivec2(1, 0), ivec2(-1, 1),\n"
        "                              ivec2(0, 1), ivec2(1, 1));\n"
        "  float s[9], lo = 3.402823466e+38, hi = 1.175494351e-38;\n"
        "  for (int i = 0; i < 9; i++) {\n"
        "    s[i] = texelFetch(luma_t, clampp(p + O[i]), 0).r;\n"
        "    lo = min(lo, s[i]);\n"
        "    hi = max(hi, s[i]);\n"
        "  }\n"
        "  uint similar = 1u;\n"
        "  float dlo = 3.402823466e+38, dhi = 0.0;\n"
        "  for (int i = 1; i < 9; i++) {\n"
        "    if (abs(s[i] - s[0]) / (hi - lo) < 0.9) similar |= 1u << i;\n"
        "    else { dlo = min(dlo, s[i]); dhi = max(dhi, s[i]); }\n"
        "  }\n"
        "  if (!(s[0] > dhi || s[0] < dlo)) return 0.0;\n"
        "  const uint Q[4] = uint[4](0x17u, 0x2Du, 0xD1u, 0x1A1u);  // (the 2x2 quadrants around the centre: a solid area)\n"
        "  for (int i = 0; i < 4; i++) if ((similar & Q[i]) == Q[i]) return 0.0;\n"
        "  return 1.0 - lo / hi;\n"
        "}\n"
        "void main() {\n"
        "  ivec2 p = pixel();\n"
        "  if (!onscreen(p)) return;\n"
        "  vec2 uv = (vec2(p) + 0.5) * pc.size.zw;\n"
        "  vec4 d = texelFetch(dil_t, p, 0);\n"
        "  bool has = d.z > 0.0;\n"
        "  float zc = abs(d.z);\n"
        "  float disocc = has ? disocclusion(uv, d.xy, zc) : 1.0;\n"
        "  float reactive = 0.0;\n"
        "  for (int y = -1; y <= 1; y++)\n"
        "    for (int x = -1; x <= 1; x++) reactive = max(reactive, texelFetch(vec_t, clampp(p + ivec2(x, y)), 0).z);\n"
        "  float shading = max(clamp(reactive, 0.0, 1.0), clamp(texture(shade_t, uv - pc.jit.xy * pc.size.zw).r, 0.0, 1.0));\n"
        "  float divergence = has ? motion_divergence(uv, d.xy, zc) : 0.0;\n"
        "  vec2 ruv = uv + d.xy;\n"
        "  float acc = pc.st.x > 0.5 && has && inside(ruv) ? clamp(texture(acc_prev_t, ruv).r, 0.0, 1.0) : 0.0;\n"
        "  acc = mix(acc, 0.0, shading);\n"
        "  acc = mix(acc, min(-1.0 / 3.0, acc), disocc);\n"
        "  acc *= float(round(acc * 100.0) > 1.0);\n"
        "  imageStore(acc_i, p, vec4(clamp(acc + 1.0 / 3.0, 0.0, 1.0)));\n"
        "  imageStore(react_i, p, vec4(divergence, disocc, shading, max(acc, 0.0)));\n"
        "  float lock = thin_feature(p);\n"
        "  imageStore(lock_i, p, vec4(lock > 0.01 ? lock : 0.0));\n"
        "}\n";
static const char taa_instability[] =
        "layout(set = 0, binding = 0) uniform sampler2D dil_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D react_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D luma_t;\n"
        "layout(set = 0, binding = 3) uniform sampler2D hist_t;\n"
        "layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D hist_i;\n"
        "layout(set = 0, binding = 9, r8) uniform writeonly image2D instab_i;\n"
        "void main() {\n"
        "  ivec2 p = pixel();\n"
        "  if (!onscreen(p)) return;\n"
        "  vec4 d = texelFetch(dil_t, p, 0);\n"
        "  vec2 uv = (vec2(p) + 0.5) * pc.size.zw, cuv = uv + pc.jit.xy * pc.size.zw, ruv = uv + pc.jit.zw * pc.size.zw + d.xy;\n"
        "  vec4 h = vec4(0.0);\n"
        "  float instab = 0.0;\n"
        "  if (pc.st.x > 0.5 && d.z > 0.0 && inside(ruv)) {\n"
        "    vec4 m = texture(react_t, cuv);\n"
        "    if (m.a > 0.9) {\n"
        "      float l = texture(luma_t, cuv).r;\n"
        "      h = texture(hist_t, ruv);\n"
        "      float s0 = min_over_max(l, h.x, 1.0);\n"
        "      if (s0 < 1.0) {\n"
        "        float best = s0;\n"
        "        for (int i = 1; i < 4; i++)\n"
        "          if (sign(l - h.x) == sign(l - h[i])) best = max(best, min_over_max(l, h[i], 0.0));\n"
        "        instab = float(best > s0);\n"
        "      }\n"
        "      h = vec4(l, h.xyz);\n"
        "      instab *= float(h.w != 0.0);\n"
        "      instab *= (1.0 - clamp(v4k(d.xy) / 20.0, 0.0, 1.0)) * (1.0 - m.g) * (1.0 - m.r) * (1.0 - m.b);\n"
        "    }\n"
        "  }\n"
        "  imageStore(hist_i, p, h);\n"
        "  imageStore(instab_i, p, vec4(instab));\n"
        "}\n";
static const char taa_resolve[] =
        "layout(set = 0, binding = 0) uniform sampler2D color_t;\n"
        "layout(set = 0, binding = 1) uniform sampler2D dil_t;\n"
        "layout(set = 0, binding = 2) uniform sampler2D react_t;\n"
        "layout(set = 0, binding = 3) uniform sampler2D instab_t;\n"
        "layout(set = 0, binding = 4) uniform sampler2D hist_t;\n"
        "layout(set = 0, binding = 5) uniform sampler2D lock_t;\n"
        "layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D hist_i;\n"
        "layout(set = 0, binding = 9, rgba16f) uniform writeonly image2D out_i;\n"
        "float lanczos2(float x) {\n"
        "  const float PI = 3.14159265;\n"
        "  x = abs(x);\n"
        "  if (x < 1e-5) return 1.0;\n"
        "  if (x >= 2.0) return 0.0;\n"
        "  return 2.0 * sin(PI * x) * sin(0.5 * PI * x) / (PI * PI * x * x);\n"
        "}\n"
        "float lanczos2_sq(float x2) {\n"
        "  x2 = min(x2, 4.0);\n"
        "  float a = 0.4 * x2 - 1.0, b = 0.25 * x2 - 1.0;\n"
        "  return (25.0 / 16.0 * a * a - 9.0 / 16.0) * (b * b);\n"
        "}\n"
        "vec4 history(vec2 uv) {\n"
        "  vec2 fp = uv * pc.size.xy - 0.5;\n"
        "  ivec2 b = ivec2(floor(fp));\n"
        "  vec2 f = fp - vec2(b);\n"
        "  vec4 wx, wy;\n"
        "  for (int i = 0; i < 4; i++) { wx[i] = lanczos2(float(i - 1) - f.x); wy[i] = lanczos2(float(i - 1) - f.y); }\n"
        "  wx /= dot(wx, vec4(1.0));\n"
        "  wy /= dot(wy, vec4(1.0));\n"
        "  vec4 sum = vec4(0.0), lo = vec4(65504.0), hi = vec4(-65504.0);\n"
        "  for (int j = 0; j < 4; j++)\n"
        "    for (int i = 0; i < 4; i++) {\n"
        "      vec4 s = texelFetch(hist_t, clampp(b + ivec2(i - 1, j - 1)), 0);\n"
        "      sum += s * (wx[i] * wy[j]);\n"
        "      if (i == 1 || i == 2) if (j == 1 || j == 2) { lo = min(lo, s); hi = max(hi, s); }\n"
        "    }\n"
        "  return clamp(sum, lo, hi);\n"
        "}\n"
        "void main() {\n"
        "  ivec2 p = pixel();\n"
        "  if (!onscreen(p)) return;\n"
        "  vec2 uv = (vec2(p) + 0.5) * pc.size.zw, J = pc.jit.xy, cuv = uv + J * pc.size.zw;\n"
        "  vec4 d = texelFetch(dil_t, p, 0);\n"
        "  vec2 mv = d.xy, ruv = uv + mv;\n"
        "  float vel = v4k(mv);\n"
        "  bool fresh = pc.st.x < 0.5 || d.z < 0.0 || !inside(ruv);\n"
        "  vec4 m = texture(react_t, cuv);\n"
        "  float reactive = clamp(m.r, 0.0, 1.0), disocc = clamp(m.g, 0.0, 1.0), shading = clamp(m.b, 0.0, 1.0);\n"
        "  float acc = clamp(m.a, 0.0, 1.0);\n"
        "  acc *= float(round(acc * 100.0) > 1.0);\n"
        "  float instab = texture(instab_t, uv).r;\n"
        "  vec4 c4 = texelFetch(color_t, p, 0);\n"
        "\n"
        "  vec3 hist = vec3(0.0);\n"
        "  float lock = 0.0;\n"
        "  if (!fresh) {\n"
        "    vec4 h = history(ruv);\n"
        "    hist = ycocg(h.rgb);\n"
        "    lock = h.a;\n"
        "  }\n"
        "  float fade = max(shading, max(reactive, disocc));\n"
        "  lock = max(0.0, lock - fade * 2.0);\n"
        "  float lock_now = clamp(clamp(lock - 1.0, 0.0, 1.0) * (2.0 - 1.0), 0.0, 1.0);\n"
        "  lock = clamp(lock + texelFetch(lock_t, p, 0).r * (1.0 - reactive), 0.0, 2.0);\n"
        "  lock = max(0.0, lock - 0.1 / pc.st.y * (1.0 - fade));\n"
        "  lock *= float(inside(uv - mv));\n"
        "\n"
        "  float hw = min(acc, mix(acc, 0.15, clamp(vel / 0.5, 0.0, 1.0)));\n"
        "\n"
        "  vec3 up = vec3(0.0), mu = vec3(0.0), m2 = vec3(0.0), lo = vec3(65504.0), hi = vec3(-65504.0);\n"
        "  float uw = 0.0, bw = 0.0;\n"
        "  for (int y = -1; y <= 1; y++)\n"
        "    for (int x = -1; x <= 1; x++) {\n"
        "      ivec2 q = p + ivec2(x, y);\n"
        "      if (!onscreen(q)) continue;\n"
        "      vec3 s = ycocg(max(texelFetch(color_t, q, 0).rgb, vec3(0.0)));\n"
        "      vec2 o = vec2(x, y) - J;\n"
        "      float o2 = dot(o, o), w = lanczos2_sq(o2), b = exp(-2.3 * o2);\n"
        "      up += s * w; uw += w;\n"
        "      mu += s * b; m2 += s * s * b; bw += b;\n"
        "      lo = min(lo, s); hi = max(hi, s);\n"
        "    }\n"
        "  mu /= bw;\n"
        "  vec3 sigma = sqrt(abs(m2 / bw - mu * mu));\n"
        "  uw *= float(uw > FP16_MIN);\n"
        "  if (uw > FP16_MIN) {\n"
        "    up = clamp(up / uw, lo, hi);\n"
        "    uw *= 0.74 / 16.0;\n"
        "  }\n"
        "  if (acc == 0.0) { up = mu; uw = 1.0; hw = 0.0; }\n"
        "\n"
        "  float t = max(max(clamp(vel / 20.0, 0.0, 1.0), clamp(0.75 - d.w / 20.0, 0.0, 1.0)),\n"
        "                max(max(1.0 - acc, sqrt(reactive)), shading));\n"
        "  vec3 box = sigma * vec3(1.7, 1.0, 1.0) * mix(3.0, 1.0, t);\n"
        "  vec3 th = (hist - mu) / max(box, vec3(1.193e-7));\n"
        "  if (length(th) > 1.0) {\n"
        "    vec3 clipped = normalize(th) * box + mu;\n"
        "    hist = mix(clipped, hist, clamp(max(instab, lock_now) * acc * (1.0 - disocc), 0.0, 1.0));\n"
        "  }\n"
        "\n"
        "  hw *= float(hw > FP16_MIN);\n"
        "  hw = max(FP16_MIN, hw + uw);\n"
        "  vec3 col = max(rgb(mix(hist, up, clamp(uw / hw, 0.0, 1.0))), vec3(0.0));\n"
        "  imageStore(hist_i, p, vec4(col, lock));\n"
        "  int view = int(pc.st.z);\n"
        "  if (view != 0) {\n"
        "    vec3 grey = vec3(dot(col, LUMA) * 0.5);\n"
        "    float v = view == 1 ? disocc : view == 2 ? acc : view == 3 ? shading : view == 4 ? instab : view == 5 ? lock_now\n"
        "            : view == 6 ? reactive : float(d.z < 0.0);\n"
        "    col = mix(grey, vec3(1.0, 0.1, 0.6), clamp(v, 0.0, 1.0));\n"
        "  }\n"
        "  imageStore(out_i, p, vec4(col, c4.a));\n"
        "}\n";

static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, uv).rgb, 1.0); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size (alpha: bilinear, for the 3D scene's composite, exec.c scene_upscale). 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing). b.z = 1: the input is linear (BOTW's
    // 3D scene, exec.c scene_post): taken to gamma 2.0 first, as FSR 1 wants perceptual input, and left there for
    // RCAS. In linear light the small differences in shadows and leaves counted for little next to the lit parts', so
    // the edge directions and the clamp to the nearest 4 made flat blotches there once the composite's gamma brought
    // them out: rocks and trees looked posterized, the more the smaller the scene (#141).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) {\n"
               "  vec3 c = texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb;\n"
               "  return pc.b.z > 0.5 ? sqrt(max(c, vec3(0.0))) : c;\n"
               "}\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  float alpha = texture(t, (pp + 0.5) / pc.b.xy).a;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), alpha);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    // b.z = 1: the input is EASU's gamma 2.0 of a linear image, the output linear again.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  vec3 r = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
               "  o = vec4(pc.b.z > 0.5 ? r * r : r, texelFetch(t, ip, 0).a);\n"
               "}\n";
    }
    // AMD FidelityFX CAS (MIT), contrast-adaptive sharpening without scaling (ffx_cas.h CasFilter, its green-weight
    // path): a 3x3 neighbourhood's soft minimum and maximum set how far each pixel may be pushed away from its cross
    // neighbours, so edges sharpen without halos and flat or noisy areas stay. a.x = sharpness 0..1 (peak -1/8 ..
    // -1/5), b.xy = image size. Alpha passes through (exec.c scene_post).
    // Motion vectors over the scene (SAVAGEWIND_MVEC_VIEW, exec.c mvec_scene_view): t0 the scene, t1 the motion vectors
    // (exec.c mvec_fill: NDC distance from the previous frame in .xy; .a 1 for a draw's own vector, 0.5 for the camera's,
    // 0 for none). The scene's grey mixed with a hue for the direction, as strong as the length against a.x pixels;
    // camera vectors hatched; no vector: the scene darkened and blue; the reactive mask (.z, 4x: effects are faint)
    // magenta over them. a.x < 0:
    // raw (red = .a, green / blue = |x| / |y|
    // times -a.x). b.xy: the motion vectors' size, b.zw: the output's.
    if (!strcmp(name, "mvec_view.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy / pc.b.zw;\n"
               "  vec4 s = texture(t0, uv);\n"
               "  float l = dot(s.rgb, vec3(0.299, 0.587, 0.114));\n"
               "  vec4 m = texelFetch(t1, ivec2(uv * pc.b.xy), 0);\n"
               "  if (pc.a.x < 0.0) { o = vec4(m.a, clamp(abs(m.xy) * -pc.a.x, 0.0, 1.0), s.a); return; }\n"
               "  if (m.a < 0.25) { o = vec4(l * 0.3, l * 0.3, l * 0.3 + 0.2, s.a); return; }\n"
               "  vec2 px = m.xy * 0.5 * pc.b.xy;\n"
               "  float k = clamp(length(px) / max(pc.a.x, 0.001), 0.0, 1.0);\n"
               "  vec3 hue = 0.5 + 0.5 * cos(atan(px.y, px.x) + vec3(0.0, 2.094, 4.189));\n"
               "  vec3 c = mix(vec3(l * 0.6), hue, k);\n"
               "  if (m.a < 0.75 && ((int(gl_FragCoord.x + gl_FragCoord.y) >> 3) & 1) != 0) c = mix(c, vec3(l * 0.6), 0.65);\n"
               "  c = mix(c, vec3(1.0, 0.25, 1.0), clamp(m.z * 4.0, 0.0, 1.0) * 0.8);\n"
               "  o = vec4(c, s.a);\n"
               "}\n";
    }
    // Motion vectors for the pixels no draw gave one (exec.c mvec_fill): t0 the object vectors (begin_pass's motion
    // target, alpha 1 where written), t1 the scene depth. Those pixels get the camera's motion: this frame's NDC from
    // their position and depth, taken by m (the previous frame's view-projection * the inverse of this frame's) to the
    // previous frame's; alpha 0.5. Vectors are clip-space NDC differences, as the object vectors (mvec_rt): the
    // viewports have a negative height (exec.c set_viewport: y up), so a pixel's clip y is minus its framebuffer NDC y,
    // and the game's NDC y is that times a.x (-1 when the vertex shaders flip y, ShaderKey.flip_y). a.y: 1 when the
    // game's depth range is [-1, 1] (depth = (z + 1) / 2). a.z: 1 = camera vectors everywhere, 2 = camera minus object
    // vector where there is one (SAVAGEWIND_MVEC_CAMONLY). b.xy: this frame's jitter minus the previous frame's in the
    // game's NDC (mvec_reprojection), taken out of every vector: what remains is the motion alone.
    if (!strcmp(name, "mvec_fill.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(push_constant) uniform P { layout(row_major) mat4 m; vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec4 v = texelFetch(t0, p, 0);\n"
               "  vec2 jit = vec2(pc.b.x, pc.b.y * pc.a.x);\n"
               "  if (v.a > 0.5 && pc.a.z == 0.0) { o = vec4(v.xy - jit, v.zw); return; }\n"
               "  if (v.a < 0.5 && pc.a.z == 2.0) { o = vec4(0.0); return; }\n"
               "  vec2 fb = gl_FragCoord.xy / vec2(textureSize(t0, 0)) * 2.0 - 1.0;\n"
               "  vec2 clip = vec2(fb.x, -fb.y);\n"
               "  float d = texelFetch(t1, p, 0).r;\n"
               "  vec4 q = pc.m * vec4(clip.x, clip.y * pc.a.x, pc.a.y != 0.0 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec2 prev = vec2(q.x, q.y * pc.a.x) / q.w;\n"
               "  o = pc.a.z == 2.0 ? vec4(clip - prev - v.xy, 0.0, 1.0) : vec4(clip - prev - jit, v.z, 0.5);\n"
               "}\n";
    }
    // FSR 3's inputs (exec.c fsr3_scene), at the render size: the scene depth as R32F (t0, a depth view), and the
    // reactive mask from the motion vectors' .z (t0) as R8
    if (!strcmp(name, "fsr3_depth.frag") || !strcmp(name, "fsr3_reactive.frag")) {
        *stage = STAGE_FRAGMENT;
        return name[5] == 'd' ? "#version 450\n"
                                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                                "layout(location = 0) out vec4 o;\n"
                                "void main() { o = vec4(texelFetch(t0, ivec2(gl_FragCoord.xy), 0).r); }\n"
                              : "#version 450\n"
                                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                                "layout(location = 0) out vec4 o;\n"
                                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                                "void main() { o = vec4(clamp(texelFetch(t0, ivec2(gl_FragCoord.xy), 0).z * pc.a.x, 0.0, 1.0)); }\n";
    }
    if (!strncmp(name, "taa_", 4)) {
        static const char *const names[] = {"taa_prepare.comp", "taa_shade_src.comp", "taa_shade_down.comp", "taa_shade.comp",
                                            "taa_reactivity.comp", "taa_instability.comp", "taa_resolve.comp"};
        static const char *const bodies[] = {taa_prepare, taa_shade_src, taa_shade_down, taa_shade, taa_reactivity,
                                             taa_instability, taa_resolve};
        static char *src[7];
        for (int i = 0; i < 7; i++) {
            if (strcmp(name, names[i])) continue;
            if (!src[i]) {
                size_t a = strlen(taa_common), b = strlen(bodies[i]);
                src[i] = malloc(a + b + 1);
                memcpy(src[i], taa_common, a);
                memcpy(src[i] + a, bodies[i], b + 1);
            }
            *stage = STAGE_COMPUTE;
            return src[i];
        }
        return NULL;
    }
    if (!strcmp(name, "cas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //  a b c\n"
               "  //  d e f\n"
               "  //  g h i\n"
               "  vec3 a = tap(ip + ivec2(-1, -1)), b = tap(ip + ivec2(0, -1)), c = tap(ip + ivec2(1, -1));\n"
               "  vec3 d = tap(ip + ivec2(-1, 0)), f = tap(ip + ivec2(1, 0));\n"
               "  vec3 g = tap(ip + ivec2(-1, 1)), h = tap(ip + ivec2(0, 1)), i = tap(ip + ivec2(1, 1));\n"
               "  vec4 e4 = texelFetch(t, ip, 0);\n"
               "  vec3 e = e4.rgb;\n"
               "  vec3 mn = min(min(min(d, e), min(f, b)), h);\n"
               "  vec3 mn2 = min(min(min(mn, a), min(c, g)), i);\n"
               "  mn += mn2;\n"
               "  vec3 mx = max(max(max(d, e), max(f, b)), h);\n"
               "  vec3 mx2 = max(max(max(mx, a), max(c, g)), i);\n"
               "  mx += mx2;\n"
               "  vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 65536.0)), 0.0, 1.0);\n"
               "  amp = sqrt(amp);\n"
               "  float peak = -1.0 / mix(8.0, 5.0, clamp(pc.a.x, 0.0, 1.0));\n"
               "  float w = amp.g * peak;\n"
               "  vec3 r = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);\n"
               "  o = vec4(clamp(r, 0.0, 1.0), e4.a);\n"
               "}\n";
    }
    // Colour grading of the 3D scene (Video > Colour; exec.c scene_post, runtime/color_grade.h). The scene holds
    // linear light (BOTW's 10-bit composite input; the game sRGB-encodes the final image when it presents it). In
    // linear light: g.rgb = exposure x white balance, a.x = saturation (Rec. 709 luma kept). Then, sRGB-encoded as the
    // picture is seen: contrast, an S curve through 0, 0.5 and 1 with slope g.w in its middle, and (grade_lut.frag) the
    // LUT, t1, sampled trilinearly at texel centres (a.z = its size) after DOMAIN_MIN / MAX (dmin, dscale), mixed in
    // by a.y. Back to linear for the composite. Alpha passes through.
    if (!strcmp(name, "grade.frag") || !strcmp(name, "grade_lut.frag")) {
        *stage = STAGE_FRAGMENT;
#define GRADE_BODY                                                                                              \
    "layout(set = 0, binding = 0) uniform sampler2D t;\n"                                                       \
    "layout(push_constant) uniform P { vec4 g; vec4 a; vec4 dmin; vec4 dscale; } pc;\n"                        \
    "layout(location = 0) out vec4 o;\n"                                                                        \
    "vec3 enc(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }\n" \
    "vec3 dec(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"     \
    "void main() {\n"                                                                                           \
    "  vec4 s = texelFetch(t, ivec2(gl_FragCoord.xy), 0);\n"                                                    \
    "  vec3 c = s.rgb * pc.g.rgb;\n"                                                                            \
    "  float y = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"                                                       \
    "  c = max(mix(vec3(y), c, pc.a.x), 0.0);\n"                                                                \
    "  vec3 x = enc(min(c, 1.0));\n"                                                                            \
    "  vec3 lo = 0.5 * pow(2.0 * x, vec3(pc.g.w)), hi = 1.0 - 0.5 * pow(max(2.0 - 2.0 * x, 0.0), vec3(pc.g.w));\n" \
    "  x = mix(lo, hi, step(0.5, x));\n"                                                                        \
    "#ifdef LUT\n"                                                                                              \
    "  vec3 u = clamp((x - pc.dmin.xyz) * pc.dscale.xyz, 0.0, 1.0);\n"                                          \
    "  vec3 l = texture(t1, u * ((pc.a.z - 1.0) / pc.a.z) + 0.5 / pc.a.z).rgb;\n"                               \
    "  x = mix(x, clamp(l, 0.0, 1.0), pc.a.y);\n"                                                               \
    "#endif\n"                                                                                                  \
    "  o = vec4(dec(x), s.a);\n"                                                                                \
    "}\n"
        if (name[5] == '_')
            return "#version 450\n#define LUT 1\nlayout(set = 0, binding = 1) uniform sampler3D t1;\n" GRADE_BODY;
        return "#version 450\n" GRADE_BODY;
#undef GRADE_BODY
    }
    // Frame generation's camera motion vectors (exec.c vk_fg_generate): the 3D scene's depth at each pixel back to
    // NDC, through m (the previous frame's view-projection x this one's inverse) to where that point was, as a uv
    // offset (previous - this). b = depth size, the rows' direction in NDC (+1: row 0 at the bottom), 1 when the depth
    // is OpenGL's -1..1 (NVN's default depth mode). Link's box (uv min, max) and depth range: his own motion there (lk.zw).
    if (!strcmp(name, "fg_mv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D d;\n"
               "layout(push_constant) uniform P { mat4 m; vec4 b; vec4 box; vec4 lk; } pc;\n"
               "layout(location = 0) out vec2 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy / pc.b.xy;\n"
               "  float z = texelFetch(d, ivec2(gl_FragCoord.xy), 0).r;\n"
               "  vec4 c = pc.m * vec4(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * pc.b.z, pc.b.w > 0.5 ? z * 2.0 - 1.0 : z, 1.0);\n"
               "  vec2 p = c.xy / max(abs(c.w), 1e-6) * sign(c.w);\n"
               "  o = vec2(p.x, p.y * pc.b.z) * 0.5 + 0.5 - uv;\n"
               "  bool inbox = all(greaterThanEqual(uv, pc.box.xy)) && all(lessThanEqual(uv, pc.box.zw));\n"
               "  if (inbox && z >= pc.lk.x && z <= pc.lk.y) o = pc.lk.zw;\n"
               "  if (pc.b.w > 1.5) o = vec2(inbox ? 1.0 : 0.0, inbox && z >= pc.lk.x && z <= pc.lk.y ? 1.0 : 0.0);  // (debug)\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
