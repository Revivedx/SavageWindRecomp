// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <stddef.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
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

// What the translated shaders depend on besides their binaries: the translator's build and the SPIR-V
// post-processing below. Caches built from translated shaders (cache/xlate, the pipeline cache, the pipeline list)
// are valid for one value of this only.
#define SPIRV_POST_VERSION 4  // (no_contraction, strip_names)
const char *vk_shader_version(void) {
    static char v[64];
    if (!v[0]) snprintf(v, sizeof v, "%s/spv%d", maxwell_translator_id(), SPIRV_POST_VERSION);
    return v;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + SPIRV_POST_VERSION;
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[512], name[64];
    snprintf(name, sizeof name, "spirv/%016llx.spv", (unsigned long long)h);
    vk_cache_path(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    char path[512], tmp[540], name[64];
    if (!made) { made = 1; vk_cache_path(path, sizeof path, "spirv"); mkdir(path, 0755); }
    snprintf(name, sizeof name, "spirv/%016llx.spv", (unsigned long long)h);
    vk_cache_path(path, sizeof path, name);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// ---- translated shaders (Maxwell -> GLSL): 1-4 ms each in the open world, on the GPU thread while compiles are
// synchronous, and programs share their vertex and fragment shaders. Kept in memory by stage, binary and key, and on
// disk as cache/xlate/<hash>.bin, so a shader seen in an earlier session costs a file read. The hash covers the bytes
// the translator reads, the key and the translator's id (maxwell_translator_id: a change to translate.c starts over).
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
static ShaderInfo *xlate_load(uint64_t h, const XlateHeader *want, int stage) {
    char path[512], name[64];
    snprintf(name, sizeof name, "xlate/%016llx.bin", (unsigned long long)h);
    vk_cache_path(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    XlateHeader hd;
    ShaderInfo *si = NULL;
    bool ok = fread(&hd, sizeof hd, 1, f) == 1 && hd.msl_len < (64u << 20);
    if (ok && want)
        ok = !memcmp(&hd, want, offsetof(XlateHeader, msl_len)) && hd.bin_size == want->bin_size &&
             !memcmp(&hd.key, &want->key, sizeof hd.key) && hd.stage == want->stage;
    else if (ok)
        ok = !memcmp(hd.magic, "SWX1", 4) && hd.info_size == sizeof(ShaderInfo) && hd.stage == stage && hd.msl_len;
    if (ok) {
        si = malloc(sizeof *si);
        char *msl = malloc(hd.msl_len + 1);
        if (si && msl && fread(si, sizeof *si, 1, f) == 1 && fread(msl, 1, hd.msl_len, f) == hd.msl_len && si->stage == hd.stage) {
            msl[hd.msl_len] = 0;
            si->msl = msl;
        } else {
            free(si);
            free(msl);
            si = NULL;
        }
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
    char path[512], tmp[540], name[64];
    if (!made) { made = 1; vk_cache_path(path, sizeof path, "xlate"); mkdir(path, 0755); }
    snprintf(name, sizeof name, "xlate/%016llx.bin", (unsigned long long)h);
    vk_cache_path(path, sizeof path, name);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)si);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    ShaderInfo copy = *si;
    copy.msl = NULL;
    bool ok = fwrite(hd, sizeof *hd, 1, f) == 1 && fwrite(&copy, sizeof copy, 1, f) == 1 &&
              fwrite(si->msl, 1, hd->msl_len, f) == hd->msl_len;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

ShaderInfo *vk_translate_cached(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    static int off = -1;  // SAVAGEWIND_NO_XLATE_CACHE=1: translate every time
    if (off < 0) off = getenv("SAVAGEWIND_NO_XLATE_CACHE") != NULL;
    if (off || maxwell_translation_uncacheable()) return maxwell_translate(code, max_size, stage, key);
    XlateHeader hd;
    memset(&hd, 0, sizeof hd);
    memcpy(hd.magic, "SWX1", 4);
    hd.info_size = sizeof(ShaderInfo);
    hd.bin_size = (uint32_t)maxwell_binary_size(code, max_size, stage);
    hd.key = *key;
    hd.stage = stage;
    const char *id = maxwell_translator_id();
    uint64_t h = hash64(0xcbf29ce484222325ull, id, strlen(id));
    h = hash64(h, &hd, sizeof hd);
    h = hash64(h, code, hd.bin_size);
    if (!h) h = 1;
    pthread_mutex_lock(&xlate_lock);
    uint32_t slot = (uint32_t)h & (XLATE_TAB - 1), probes = 0;
    while (xlate_tab[slot].h && xlate_tab[slot].h != h && probes++ < XLATE_TAB) slot = (slot + 1) & (XLATE_TAB - 1);
    ShaderInfo *si = xlate_tab[slot].h == h ? xlate_tab[slot].si : NULL;
    pthread_mutex_unlock(&xlate_lock);
    if (si) return si;
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

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// Debug names (OpSource, OpSourceExtension, OpName, OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed):
// glslang writes one for every register variable, ~16 % of a translated shader's SPIR-V, read by no driver.
static void strip_names(uint32_t *spv, size_t *nwords) {
    size_t o = 5;
    for (size_t i = 5; i < *nwords;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > *nwords) return;  // (malformed: left as it is from here)
        bool name = op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op == 8 || op == 317 || op == 330;
        if (!name) { memmove(spv + o, spv + i, wc * 4); o += wc; }
        i += wc;
    }
    *nwords = o;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. SAVAGEWIND_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("SAVAGEWIND_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, getenv("SAVAGEWIND_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        strip_names(out, nwords);
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
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
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
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
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), texelFetch(t, ip, 0).a);\n"
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
