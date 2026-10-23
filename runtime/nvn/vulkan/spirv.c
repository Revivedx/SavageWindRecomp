// GLSL -> SPIR-V: shaderc, the vertex-shader post-processing (no_contraction) and the SPIR-V disk cache. What this
// file makes is part of every shader cache's key (vk_spirv_id: a hash of this file, from the Makefile), so it holds
// only that step.
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <zlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"

#ifndef SPIRV_ID
#define SPIRV_ID __DATE__ " " __TIME__  // (built without the Makefile's hash: valid for this build only)
#endif
const char *vk_spirv_id(void) { return SPIRV_ID; }

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<id>/<hash of stage + source>.spz, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one). <id>: this file's (vk_cache_dir_id):
// another build's folder is removed (vk_cache_cleanup). A file: "SPZ1", the SPIR-V's word count, the SPIR-V
// deflated (zlib; a third to a quarter of the size: the precompiled shaders of the whole game take ~0.4 GB instead
// of 1.4).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage;
    for (const char *p = vk_spirv_id(); *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static void spirv_path(char *path, size_t n, uint64_t h) {
    char name[96];
    snprintf(name, sizeof name, "spirv/%s/%016llx.spz", vk_cache_dir_id(vk_spirv_id()), (unsigned long long)h);
    vk_cache_path(path, n, name);
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[512];
    spirv_path(path, sizeof path, h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *z = n > 8 && n < (64l << 20) ? malloc((size_t)n) : NULL;
    if (z && fread(z, 1, (size_t)n, f) != (size_t)n) { free(z); z = NULL; }
    fclose(f);
    uint32_t *d = NULL, words = 0;
    if (z && !memcmp(z, "SPZ1", 4)) {
        memcpy(&words, z + 4, 4);
        uLongf len = (uLongf)words * 4;
        d = words && words < (16u << 20) ? malloc(len) : NULL;
        if (d && (uncompress((Bytef *)d, &len, z + 8, (uLong)(n - 8)) != Z_OK || len != (uLongf)words * 4 || d[0] != 0x07230203)) {
            free(d);
            d = NULL;
        }
    }
    free(z);
    if (d) *nwords = words;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    char path[512], tmp[540], name[96];
    if (!made) {
        made = 1;
        vk_cache_path(path, sizeof path, "spirv");
        mkdir(path, 0755);
        snprintf(name, sizeof name, "spirv/%s", vk_cache_dir_id(vk_spirv_id()));
        vk_cache_path(path, sizeof path, name);
        mkdir(path, 0755);
    }
    spirv_path(path, sizeof path, h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    uLongf zn = compressBound((uLong)(nwords * 4));
    uint8_t *z = malloc(8 + zn);
    if (!z) return;
    memcpy(z, "SPZ1", 4);
    uint32_t w = (uint32_t)nwords;
    memcpy(z + 4, &w, 4);
    bool ok = compress2(z + 8, &zn, (const Bytef *)spv, (uLong)(nwords * 4), 1) == Z_OK;  // (fast: stores happen at draws too)
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    ok = f && fwrite(z, 1, 8 + zn, f) == 8 + zn;
    if (f && fclose(f)) ok = false;
    free(z);
    if (f && (!ok || rename(tmp, path) != 0)) remove(tmp);  // (another thread may have stored it first)
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
// The SPIR-V of src in the disk cache (compiled now if missing): 0 it was there, 1 compiled, -1 failed.
int vk_spirv_ensure(const char *src, int stage) {
    char path[512];
    spirv_path(path, sizeof path, src_hash(src, stage));
    struct stat st;
    if (!getenv("SAVAGEWIND_VK_NO_SPIRV_CACHE") && !stat(path, &st) && st.st_size > 20) return 0;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, "precompile", &n);
    free(spv);
    return spv ? 1 : -1;
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
