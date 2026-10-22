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
#define SPIRV_POST_VERSION 3  // (no_contraction)
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
    // ---- temporal effects (exec.c "temporal": scene_resolve)
    // Camera motion vectors: each pixel's position, rebuilt from its depth, taken to the previous frame by m = previous
    // view-projection x inverse of this one (the game's matrices, GL style). a.x = 1: depth stored as (z_ndc + 1) / 2;
    // a.y = the sign of clip y at the bottom of the image (+1: row 0 is clip y = -1); a.zw = this frame's jitter in clip
    // units (the vectors carry none). Out (RGBA16F): xy = velocity = this frame's uv - the previous frame's, in uv units;
    // z = the pixel's distance in the previous frame / its distance now; w = 1. Where the G-buffer's velocity target
    // has an object's own (obj, w = 1: ShaderKey.velocity, in clip units / 2 along the game's y) that is taken instead.
    if (!strcmp(name, "mv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D obj;\n"
               "layout(push_constant) uniform P { mat4 m; vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(dep, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz);\n"
               "  vec2 ju = vec2(pc.a.z, pc.a.w * pc.a.y) * 0.5;\n"
               "  vec4 ov = texelFetch(obj, min(p, textureSize(obj, 0) - 1), 0);\n"
               "  if (ov.w > 0.5 && !any(isnan(ov)) && !any(isinf(ov))) { o = vec4(ov.x, ov.y * pc.a.y, ov.z, 1.0); return; }\n"
               "  float d = texelFetch(dep, p, 0).r;\n"
               "  vec2 u0 = uv - ju;\n"
               "  vec2 n = u0 * 2.0 - 1.0;\n"
               "  n.y *= pc.a.y;\n"
               "  vec4 q = pc.m * vec4(n, pc.a.x > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec2 pn = q.xy / q.w;\n"
               "  pn.y *= pc.a.y;\n"
               "  o = vec4(u0 - (pn * 0.5 + 0.5), q.w, 1.0);\n"
               "}\n";
    }
    // Motion vectors shown (b.xy = 1 / the output's size, b.z = the scale: velocity in uv x b.z around mid-grey; blue:
    // an object's own vector) - the F12 debug view and SAVAGEWIND_MV_DUMP
    if (!strcmp(name, "mv_show.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec4 m = texture(t, gl_FragCoord.xy * pc.b.xy);\n"
               "  vec2 v = m.rg * pc.b.z;\n"
               "  if (any(isnan(m.rg)) || any(isinf(m.rg))) { o = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"  // (magenta: not a number)
               "  o = vec4(clamp(0.5 + v, 0.0, 1.0), 0.5, 1.0);\n"
               "}\n";
    }
    // TAA (exec.c taa_run), on the game's finished scene: t = this frame (jittered), h = the history (rgb, a = its
    // distance), mv = the motion vectors (xy velocity, z previous / current distance), dep = depth. a = the projection
    // (P00, P11, A, B), b = (reset, history weight, depth mode, 0). Blended in a tone-mapped space (c / (1 + max c)); the
    // velocity of the nearest of the 3x3 (edges keep the foreground's); the history by Catmull-Rom, clipped to the
    // 3x3's YCoCg mean +- 1.25 sigma (variance clipping), dropped where its distance is not the one expected
    // (uncovered). Out: rgb, a = this distance. (LGPE Recomp's.)
    if (!strcmp(name, "taa.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
               "layout(set = 0, binding = 3) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }\n"
               "vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }\n"
               "vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
               "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
               "vec3 cat(vec2 uv, vec2 sz) {\n"  // Catmull-Rom, 5 bilinear taps (the corners dropped)
               "  vec2 sp = uv * sz, tp = floor(sp - 0.5) + 0.5, f = sp - tp;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (tp - 1.0) / sz, t3 = (tp + 2.0) / sz, t12 = (tp + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y) + texture(h, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y)\n"
               "         + texture(h, t12).rgb * (w12.x * w12.y) + texture(h, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y)\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return max(r / ws, vec3(0.0));\n"
               "}\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  vec3 c = max(texelFetch(t, p, 0).rgb, vec3(0.0));\n"
               "  vec2 dsc = vec2(dsz) / vec2(sz);\n"
               "  float l = lin(texelFetch(dep, min(ivec2(vec2(p) * dsc), dsz - 1), 0).r);\n"
               "  if (pc.b.x > 0.5) { o = vec4(c, l); return; }\n"
               "  vec3 m1 = vec3(0.0), m2 = vec3(0.0);\n"
               "  float dn = 1.0;\n"
               "  ivec2 np = p;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);\n"
               "      vec3 s = ycc(tm(max(texelFetch(t, q, 0).rgb, vec3(0.0))));\n"
               "      m1 += s; m2 += s * s;\n"
               "      float d = texelFetch(dep, min(ivec2(vec2(q) * dsc), dsz - 1), 0).r;\n"
               "      if (d < dn) { dn = d; np = q; }\n"
               "    }\n"
               "  m1 /= 9.0;\n"
               "  vec3 sg = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));\n"
               "  vec4 v = texelFetch(mv, min(ivec2(vec2(np) * vec2(textureSize(mv, 0)) / vec2(sz)), textureSize(mv, 0) - 1), 0);\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz), pu = uv - v.xy;\n"
               "  if (any(lessThan(pu, vec2(0.0))) || any(greaterThan(pu, vec2(1.0)))) { o = vec4(c, l); return; }\n"
               "  ivec2 hs = textureSize(h, 0);\n"
               "  float lh = texelFetch(h, clamp(ivec2(pu * vec2(hs)), ivec2(0), hs - 1), 0).a, le = v.z * l;\n"
               "  float keep = clamp(1.0 - (abs(lh - le) / max(le * 0.02, 2.0) - 1.0), 0.0, 1.0);\n"
               "  vec3 hy = ycc(tm(cat(pu, vec2(hs))));\n"
               "  vec3 lo = m1 - 1.25 * sg, hi = m1 + 1.25 * sg, ce = 0.5 * (hi + lo), ex = 0.5 * (hi - lo) + 1e-4;\n"
               "  vec3 dv = (hy - ce) / ex;\n"
               "  float mx = max(abs(dv.x), max(abs(dv.y), abs(dv.z)));\n"
               "  if (mx > 1.0) hy = ce + (hy - ce) / mx;\n"
               "  vec3 cy = ycc(tm(c));\n"
               "  float w = pc.b.y * keep;\n"
               "  o = vec4(itm(rgb(mix(cy, hy, w))), l);\n"
               "}\n";
    }
    // TAA's result into the scene's format: t0 = the history just written (rgb), t1 = the scene (its alpha)
    if (!strcmp(name, "taa_out.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  o = vec4(texelFetch(t0, p, 0).rgb, texelFetch(t1, p, 0).a);\n"
               "}\n";
    }
    // ---- frame generation (exec.c "frame generation"): N's and N-1's images, vectors and distances.
    // The scene's vectors and distances at the render size: xy = the motion vector (this uv - the previous one), z = the
    // distance (metres: a = A, B of the projection, depth mode; GL style, distance = B / (z_ndc + A)), w = the previous
    // / current distance. Not a number: still, unknown distance (far).
    if (!strcmp(name, "fx_cap.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  float d = texelFetch(dep, p, 0).r;\n"
               "  float l = pc.a.y / ((pc.a.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.x);\n"
               "  if (isnan(l) || isinf(l) || l <= 0.0) l = 60000.0;\n"
               "  vec4 m = texelFetch(mv, min(p, textureSize(mv, 0) - 1), 0);\n"
               "  if (any(isnan(m)) || any(isinf(m))) m = vec4(0.0, 0.0, 1.0, 1.0);\n"
               "  o = vec4(m.xy, min(l, 60000.0), m.z);\n"
               "}\n";
    }
    // A level of the luma pyramid: a.x = 1, from the image (a 4x4 box: four bilinear taps; its luma); else from the
    // level before (a 2x2 box: one tap)
    if (!strcmp(name, "fx_luma.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 sz = vec2(textureSize(t, 0)), p = floor(gl_FragCoord.xy);\n"
               "  float y;\n"
               "  if (pc.a.x > 0.5) {\n"
               "    vec2 c = (p * 4.0 + 2.0) / sz, e = 1.0 / sz;\n"
               "    vec3 s = texture(t, c - e).rgb + texture(t, c + vec2(e.x, -e.y)).rgb + texture(t, c + vec2(-e.x, e.y)).rgb\n"
               "           + texture(t, c + e).rgb;\n"
               "    y = dot(s * 0.25, vec3(0.299, 0.587, 0.114));\n"
               "  } else y = texture(t, (p * 2.0 + 1.0) / sz).r;\n"
               "  o = vec4(y);\n"
               "}\n";
    }
    // The optical flow at one level of the pyramid (exec.c fx_begin): for each texel of N (ln), the vector v (uv: this
    // uv - the previous one, like the engine's) whose 5x5 block of N-1 (lp) at uv - v is the closest (sum of absolute
    // differences). Candidates: none, the engine's vector there (g: a little favoured - exact where it applies), the
    // coarser level's vectors (fc) here and around (a.x = 0) or a search of +-4 texels (a.x = 1: the coarsest), then a
    // search of +-1 texel around the best, and with a.y = 1 (the finest) of half and quarter texels.
    // Out: xy = the vector, z = its mean difference.
    if (!strcmp(name, "fx_flow.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ln;\n"
               "layout(set = 0, binding = 1) uniform sampler2D lp;\n"
               "layout(set = 0, binding = 2) uniform sampler2D fc;\n"
               "layout(set = 0, binding = 3) uniform sampler2D g;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float blk[25];\n"
               "vec2 px, uv;\n"
               "float cost(vec2 v) {\n"
               "  float s = 0.0;\n"
               "  for (int i = 0; i < 25; i++) s += abs(blk[i] - texture(lp, uv - v + vec2(i % 5 - 2, i / 5 - 2) * px).r);\n"
               "  return s;\n"
               "}\n"
               "vec2 best; float bc;\n"
               "void try_(vec2 v, float bias) { float c = cost(v) * bias; if (c < bc) { bc = c; best = v; } }\n"
               "void main() {\n"
               "  px = 1.0 / vec2(textureSize(ln, 0));\n"
               "  uv = (floor(gl_FragCoord.xy) + 0.5) * px;\n"
               "  for (int i = 0; i < 25; i++) blk[i] = texture(ln, uv + vec2(i % 5 - 2, i / 5 - 2) * px).r;\n"
               "  best = vec2(0.0); bc = cost(best);\n"
               "  vec2 e = texture(g, uv).xy;\n"
               "  if (!any(isnan(e)) && !any(isinf(e))) try_(e, 0.9);\n"
               "  if (pc.a.x < 0.5) {\n"
               "    ivec2 cs = textureSize(fc, 0), cq = ivec2(uv * vec2(cs));\n"
               "    for (int i = 0; i < 5; i++) {\n"
               "      ivec2 d = i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(1, 0) : i == 3 ? ivec2(0, -1) : i == 4 ? ivec2(0, 1) : ivec2(0);\n"
               "      try_(texelFetch(fc, clamp(cq + d, ivec2(0), cs - 1), 0).xy, 1.0);\n"
               "    }\n"
               "  }\n"
               "  int r = pc.a.x > 0.5 ? 4 : 1;\n"
               "  vec2 c0 = best;\n"
               "  for (int y = -r; y <= r; y++)\n"
               "    for (int x = -r; x <= r; x++) if (x != 0 || y != 0) try_(c0 + vec2(x, y) * px, 1.0);\n"
               "  if (pc.a.y > 0.5)\n"
               "    for (int k = 0; k < 2; k++) {\n"
               "      vec2 c1 = best;\n"
               "      float st = k == 0 ? 0.5 : 0.25;\n"
               "      for (int y = -1; y <= 1; y++)\n"
               "        for (int x = -1; x <= 1; x++) if (x != 0 || y != 0) try_(c1 + vec2(x, y) * px * st, 1.0);\n"
               "    }\n"
               "  o = vec4(best, bc / 25.0, 1.0);\n"
               "}\n";
    }
    // Each pixel's vector (render size): the engine's (g) or the optical flow's (f), whichever takes N-1 (cp) to N (cn)
    // better over a 3x3 two pixels apart (a.xy = 1 / the image's size). The engine's unless the flow's difference is
    // clearly smaller: they are exact for what they describe, the flow is a block's. Out: xy = the vector, z = the
    // distance in N, w = the previous / current distance (the engine's), -1 (the flow's: none known) or -2 (neither
    // fits: blended without the distances' tests).
    if (!strcmp(name, "fx_pick.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D cn;\n"
               "layout(set = 0, binding = 1) uniform sampler2D cp;\n"
               "layout(set = 0, binding = 2) uniform sampler2D g;\n"
               "layout(set = 0, binding = 3) uniform sampler2D f;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float err(vec2 uv, vec2 v) {\n"
               "  float s = 0.0;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec2 q = uv + vec2(x, y) * pc.a.xy * 2.0;\n"
               "      vec3 d = abs(texture(cn, q).rgb - texture(cp, q - v).rgb);\n"
               "      s += d.r + d.g + d.b;\n"
               "    }\n"
               "  return s / 27.0;\n"
               "}\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(textureSize(g, 0));\n"
               "  vec4 e = texelFetch(g, p, 0);\n"
               "  vec2 fl = texture(f, uv).xy;\n"
               "  float ee = err(uv, e.xy), ef = err(uv, fl);\n"
               "  if (ef < ee * 0.6 - 0.004) o = vec4(fl, e.z, -1.0);\n"
               "  else o = vec4(e.xy, e.z, ee > 0.08 && ef > 0.08 ? -2.0 : e.w);\n"
               "}\n";
    }
    // The chosen vectors (vf) carried to the in-between image's time: each texel to where it is at t (a.x = 1 - t back
    // from N along its vector), onto the 2x2 texels around; a.y = 0: the atomic minimum of its distance there (dep:
    // the bits of a positive float order as the float), 1: its vector where its distance won (vec; z = 0: none)
    if (!strcmp(name, "fx_splat.comp")) {
        *stage = STAGE_COMPUTE;
        return "#version 450\n"
               "layout(local_size_x = 8, local_size_y = 8) in;\n"
               "layout(set = 0, binding = 0) uniform sampler2D vf;\n"
               "layout(set = 0, binding = 1, r32ui) uniform uimage2D dep;\n"
               "layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D vec;\n"
               "layout(push_constant) uniform P { vec4 a; } pc;\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(vf, 0), p = ivec2(gl_GlobalInvocationID.xy);\n"
               "  if (any(greaterThanEqual(p, sz))) return;\n"
               "  vec4 v = texelFetch(vf, p, 0);\n"
               "  if (any(isnan(v)) || any(isinf(v)) || any(greaterThan(abs(v.xy), vec2(1.0)))) return;\n"
               "  v.z = max(v.z, 1e-3);\n"
               "  vec2 x = vec2(p) + 0.5 - pc.a.x * v.xy * vec2(sz);\n"
               "  uint key = floatBitsToUint(v.z);\n"
               "  ivec2 b0 = ivec2(floor(x - 0.5));\n"
               "  for (int k = 0; k < 4; k++) {\n"
               "    ivec2 q = b0 + ivec2(k & 1, k >> 1);\n"
               "    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, sz))) continue;\n"
               "    if (pc.a.y < 0.5) imageAtomicMin(dep, q, key);\n"
               "    else if (imageLoad(dep, q).r == key) imageStore(vec, q, v);\n"
               "  }\n"
               "}\n";
    }
    // The image presented (fin) with the HUD mask in its alpha: 1 where the composite's image now (cur: the HUD drawn
    // over it) differs from its copy as composited (cmp) by more than a.x .. a.y around the pixel (a cross: the HUD's
    // soft edges)
    if (!strcmp(name, "fx_final.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D fin;\n"
               "layout(set = 0, binding = 1) uniform sampler2D cur;\n"
               "layout(set = 0, binding = 2) uniform sampler2D cmp;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), sz = textureSize(cur, 0);\n"
               "  float h = 0.0;\n"
               "  for (int i = 0; i < 5; i++) {\n"
               "    ivec2 d = i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(1, 0) : i == 3 ? ivec2(0, -1) : i == 4 ? ivec2(0, 1) : ivec2(0);\n"
               "    ivec2 q = clamp(p + d, ivec2(0), sz - 1);\n"
               "    vec3 df = abs(texelFetch(cur, q, 0).rgb - texelFetch(cmp, q, 0).rgb);\n"
               "    h = max(h, max(df.r, max(df.g, df.b)));\n"
               "  }\n"
               "  o = vec4(texelFetch(fin, p, 0).rgb, smoothstep(pc.a.x, pc.a.y, h));\n"
               "}\n";
    }
    // An in-between image at t (a.x): each pixel along the vector carried to t (sv; where none landed, the farthest
    // around - a background uncovered - or N's own there, vf), from N (cn) at uv + (1 - t) v and N-1 (cp) at uv - t v:
    // the one that sees the surface where only one does (N: no nearer surface at that point, vf's distance; N-1: its
    // distance there, gp, the one the engine's vector expects; neither under its HUD, the images' alpha), blended by t
    // otherwise. Where N has its HUD, N as it is.
    // b.xy = 1 / the image's size; b.z (SAVAGEWIND_FX_SHOW): 1 the vectors (red: the flow's), 2 the HUD mask and which
    // frames see the surface (green N, blue N-1), 3 the plain blend of N-1 and N.
    if (!strcmp(name, "fx_warp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D cn;\n"
               "layout(set = 0, binding = 1) uniform sampler2D cp;\n"
               "layout(set = 0, binding = 2) uniform sampler2D sv;\n"
               "layout(set = 0, binding = 3) uniform sampler2D gp;\n"
               "layout(set = 0, binding = 4) uniform sampler2D vf;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec4 at(sampler2D s, vec2 uv) { ivec2 z = textureSize(s, 0); return texelFetch(s, clamp(ivec2(uv * vec2(z)), ivec2(0), z - 1), 0); }\n"
               "bool inside(vec2 u) { return all(greaterThanEqual(u, vec2(0.0))) && all(lessThanEqual(u, vec2(1.0))); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 uv = (vec2(p) + 0.5) * pc.b.xy;\n"
               "  float t = pc.a.x;\n"
               "  ivec2 ss = textureSize(sv, 0), sq = clamp(ivec2(uv * vec2(ss)), ivec2(0), ss - 1);\n"
               "  vec4 s = texelFetch(sv, sq, 0);\n"
               "  bool hole = s.z <= 0.0;\n"
               "  if (hole) {\n"
               "    vec4 bst = vec4(0.0);\n"
               "    for (int y = -2; y <= 2; y++)\n"
               "      for (int x = -2; x <= 2; x++) {\n"
               "        vec4 c = texelFetch(sv, clamp(sq + ivec2(x, y) * 2, ivec2(0), ss - 1), 0);\n"
               "        if (c.z > bst.z) bst = c;\n"
               "      }\n"
               "    s = bst.z > 0.0 ? bst : at(vf, uv);\n"
               "  }\n"
               "  vec2 un = uv + (1.0 - t) * s.xy, up = uv - t * s.xy;\n"
               "  vec4 cN = texture(cn, un), cP = texture(cp, up);\n"
               "  bool okN = inside(un) && cN.a < 0.5, okP = inside(up) && cP.a < 0.5;\n"
               "  if (s.w > -1.5) {\n"
               "    okN = okN && at(vf, un).z > s.z * 0.95 - 0.1;\n"
               "    if (s.w > 0.0) okP = okP && at(gp, up).z > s.z * s.w * 0.95 - 0.1;\n"
               "  }\n"
               "  vec3 c = okN == okP ? mix(cP.rgb, cN.rgb, t) : okN ? cN.rgb : cP.rgb;\n"
               "  vec4 f = texelFetch(cn, p, 0);\n"
               "  float m = f.a;\n"
               "  if (pc.b.z > 0.5 && pc.b.z < 1.5) c = vec3(s.w < -0.5 && s.w > -1.5 ? 1.0 : 0.0, clamp(0.5 + s.xy * 16.0, 0.0, 1.0)) * (hole ? 0.5 : 1.0);\n"
               "  if (pc.b.z > 1.5 && pc.b.z < 2.5) c = vec3(m, okN ? 0.5 : 0.0, okP ? 0.5 : 0.0), m = 0.0;\n"
               "  if (pc.b.z > 2.5) c = mix(texelFetch(cp, p, 0).rgb, f.rgb, t);\n"
               "  o = vec4(mix(c, f.rgb, m), 1.0);\n"
               "}\n";
    }
    // The scene depth as R32F (FSR 3.1 reads a colour image)
    if (!strcmp(name, "depth_r32.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texelFetch(dep, ivec2(gl_FragCoord.xy), 0).r); }\n";
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
    static struct { char name[32]; VkShaderModule m; } cache[96];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 96) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
