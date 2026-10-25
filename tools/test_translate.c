// Fuzz test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
//  1. MSL must not change (built with MSL_REF only): every program is translated by the current translate.c
//     (key.glsl = 0) and by a reference MSL-only translator (an older translate.c compiled with old_* symbol names,
//     see tools/test_translate.sh); the MSL text and all ShaderInfo fields must be identical.
//  2. GLSL must compile: the same program with key.glsl = 1 is compiled in-process with shaderc (Vulkan 1.3) and
//     the SPIR-V validated with SPIRV-Tools.
//
// Programs are synthesized from the opcode patterns translate.c recognizes (every OPM(value, mask) and the
// half-precision `(op & mask) == value` tests, read from the source file): the unmasked bits are random, with
// register / predicate / cbuf / texture / attribute / branch fields biased towards meaningful values.
//
// usage: test_translate <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#ifdef WITH_GLSL
#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>
#endif

#ifdef MSL_REF
// the reference translator (same ShaderKey prefix and ShaderInfo layout)
ShaderInfo *old_maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void old_maxwell_free(ShaderInfo *s);
extern uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;
#endif

static uint64_t rng_state, g_seed;
static uint64_t rnd64(void) {  // splitmix64
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t n) { return (uint32_t)(rnd64() % n); }

typedef struct { uint64_t val, mask; } Pat;
static Pat pats[512];
static int npats;

static void load_patterns(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    fread(s, 1, n, f);
    s[n] = 0;
    fclose(f);
    for (char *p = s; (p = strstr(p, "OPM(0x")); p += 4) {
        unsigned long long v, m;
        if (sscanf(p, "OPM(0x%llxull, 0x%llxull)", &v, &m) == 2 && npats < 512) {
            bool dup = false;
            for (int i = 0; i < npats; i++) dup |= pats[i].val == v && pats[i].mask == m;
            if (!dup) pats[npats++] = (Pat){v, m};
        }
    }
    for (char *p = s; (p = strstr(p, "(op & 0x")); p += 4) {  // emit_half: 16-bit opcode tests
        unsigned m, v;
        if (sscanf(p, "(op & 0x%x) == 0x%x", &m, &v) == 2 && npats < 512) pats[npats++] = (Pat){(uint64_t)v << 48, (uint64_t)m << 48};
    }
    free(s);
    if (npats < 50) { fprintf(stderr, "only %d patterns found in %s\n", npats, path); exit(1); }
}

static uint64_t setf(uint64_t w, int lo, int n, uint64_t v) {
    uint64_t m = ((1ull << n) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}
static uint64_t getf(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }
static bool is(uint64_t w, uint64_t v, uint64_t m) { return (w & m) == v; }

static uint32_t rreg(void) { uint32_t x = rnd(100); return x < 8 ? 255 : x < 12 ? 250 + rnd(5) : rnd(24); }

static const uint32_t attr_addrs[] = {0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0,
                                      0xb0, 0xc0, 0x100, 0x270, 0x27c, 0x2f8, 0x2fc, 0x3fc, 0x6c, 0x2e0, 0x2e4, 0x40};

// one random instruction at byte offset pc of a program of `total` bytes
static uint64_t gen_insn(uint32_t pc, uint32_t total) {
    if (rnd(400) == 0) {  // unknown instruction (but no cbuf index >= 17: the translators index arrays of 18)
        uint64_t w = rnd64();
        w = setf(w, 34, 5, rnd(17));
        return setf(w, 36, 5, rnd(17)) & ~(1ull << 38);
    }
    const Pat *p = &pats[rnd(npats)];
    uint64_t w = rnd64();
    if (rnd(100) < 90) w = setf(w, 0, 8, rreg());
    if (rnd(100) < 90) w = setf(w, 8, 8, rreg());
    if (rnd(100) < 70) w = setf(w, 20, 8, rreg());
    if (rnd(100) < 80) w = setf(w, 39, 8, rreg());
    w = setf(w, 16, 4, rnd(100) < 70 ? 7 : rnd(16));  // predicate guard (mostly PT)
    w = (w & ~p->mask) | p->val;
    // cbuf index fields: hardware has 18 buffers; 17 clashes with the gmem binding (both translators reject it)
    if (is(w, 0xef90000000000000ull, 0xfff8000000000000ull)) w = setf(w, 36, 5, rnd(17));  // LDC
    else if (getf(w, 34, 5) >= 17) w = setf(w, 34, 5, rnd(17));
    // texture index: few textures so that slots get reused
    if (is(w, 0xd000000000000000ull, 0xf400000000000000ull) || is(w, 0xc038000000000000ull, 0xfc38000000000000ull) ||
        is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) || is(w, 0xdeb8000000000000ull, 0xfff8000000000000ull))
        w = setf(w, 36, 13, rnd(100) < 90 ? rnd(4) : rnd(8192));
    if (is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) w = setf(w, 22, 6, 1);  // TXQ dimension
    // attribute addresses (LD / ST a[], IPA)
    uint32_t aa = attr_addrs[rnd(sizeof attr_addrs / sizeof attr_addrs[0])] + (rnd(100) < 30 ? 4 * rnd(4) : 0);
    if (is(w, 0xefd8000000000000ull, 0xfff8000000000000ull) || is(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {
        w = setf(w, 20, 10, aa);
        if (rnd(100) < 85) w = setf(w, 8, 8, 255);  // not indexed
    }
    if (is(w, 0xe000000000000000ull, 0xff00000000000000ull)) {
        w = setf(w, 28, 10, aa);
        if (rnd(100) < 90) w = setf(w, 38, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    if (is(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        static const uint32_t srs[] = {0x00, 0x12, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x50};
        w = setf(w, 20, 8, srs[rnd(sizeof srs / sizeof srs[0])]);
    }
    if (is(w, 0x5080000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) {  // MUFU: valid functions
        static const uint32_t fns[] = {0, 1, 2, 3, 4, 5, 8};
        w = setf(w, 20, 4, fns[rnd(7)]);
    }
    if (is(w, 0x5bc0000000000000ull, 0xffe0000000000000ull) || is(w, 0x4bc0000000000000ull, 0xffe0000000000000ull) ||
        is(w, 0x36c0000000000000ull, 0xfee0000000000000ull) || is(w, 0x53c0000000000000ull, 0xffe0000000000000ull))
        if (rnd(100) < 90) w = setf(w, 48, 4, 0);  // PRMT default mode
    // P2R / R2P .CC and BFE .BREV are unsupported: mostly off
    static const uint64_t bit40_ops[] = {0x38e8, 0x5ce8, 0x4ce8, 0x38f0, 0x5cf0, 0x4cf0, 0x5c00, 0x4c00, 0x3800};
    for (int i = 0; i < 9; i++)
        if ((w >> 48 & (bit40_ops[i] & 0xff00 ? (bit40_ops[i] >> 8 == 0x38 ? 0xfef8 : 0xfff8) : 0)) == bit40_ops[i] && rnd(100) < 90)
            w = setf(w, 40, 1, 0);
    if (is(w, 0x5098000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) return gen_insn(pc, total);  // CSET fails
    // branch targets inside the program
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull ||
        top12 == 0xe2b0000000000000ull || top12 == 0xe250000000000000ull) {
        if (rnd(100) < 90) {
            int64_t target = (int64_t)rnd(total / 8) * 8;
            int64_t off = target - (int64_t)pc - 8;
            if (off == -8) off = 8;  // not a self-branch (that ends the program)
            w = setf(w, 20, 24, (uint64_t)off & 0xFFFFFF);
        }
        w = setf(w, 5, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    return w;
}

static size_t gen_program(uint8_t *buf, size_t cap, int stage) {
    memset(buf, 0, cap);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (stage == STAGE_FRAGMENT) {
        uint32_t omap = rnd(100) < 40 ? 0xF : rnd(100) < 50 ? (uint32_t)rnd64() : (uint32_t)rnd64() & 0xFF;
        memcpy(buf + 0x30 + 0x48, &omap, 4);
        uint32_t dw = rnd(4) == 0 ? 2 : 0;
        memcpy(buf + 0x30 + 0x4C, &dw, 4);
    }
    int groups = 1 + rnd(rnd(100) < 70 ? 3 : 10);
    uint32_t total = (uint32_t)(groups + 1) * 32;
    uint64_t *code = (uint64_t *)(buf + hdr);
    int i = 0;
    for (int g = 0; g < groups; g++) {
        code[i++] = rnd64();  // scheduling word
        for (int k = 0; k < 3; k++, i++) code[i] = gen_insn((uint32_t)i * 8, total);
    }
    code[i++] = 0x001f8000fc0007e0ull;
    code[i++] = 0xe30000000007000full;  // EXIT
    code[i++] = 0xe2400fffff87000full;  // BRA self (end of program)
    code[i++] = 0xe2400fffff87000full;
    return hdr + (size_t)i * 8 + 64;
}

static void gen_key(ShaderKey *k, int stage) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 32; i++) k->attr_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    for (int i = 0; i < 8; i++) k->rt_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    k->depth_minus_one = rnd(2);
    k->flip_y = rnd(2);
    if (stage == STAGE_FRAGMENT && rnd(2)) k->vs_out_mask = (uint32_t)rnd64();
}

#ifdef MSL_REF
static bool same_info(const ShaderInfo *a, const ShaderInfo *b) {
    if ((a->msl == NULL) != (b->msl == NULL)) return false;
    if (a->msl && strcmp(a->msl, b->msl)) return false;
    ShaderInfo x = *a, y = *b;
    x.msl = y.msl = NULL;
    return !memcmp(&x, &y, sizeof x);
}
#endif

static void dump(const char *dir, const char *tag, long n, const char *src, const char *err, const uint8_t *prog, size_t len) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%llu_%ld.txt", dir, tag, (unsigned long long)g_seed, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "// %s\n", err ? err : "");
    if (prog) {
        fprintf(f, "// program:");
        for (size_t i = 0; i < len; i += 8) fprintf(f, " %016llx", (unsigned long long)*(const uint64_t *)(prog + i));
        fprintf(f, "\n");
    }
    fputs(src ? src : "(null)", f);
    fclose(f);
}

// Directed checks of cbuf_reach (how far register-indexed constant reads reach): a vertex program that reads
// c[3][(R20 << 4) + 0x160] in a loop over R20 = first .. 1 (count down), with R20 from first_insn.
static uint64_t mov32i(int d, uint32_t v) { return 0x0100000000000000ull | (uint64_t)v << 20 | 7ull << 16 | (uint64_t)d; }
static uint64_t mov(int d, int src) { return 0x5c98078000070000ull | (uint64_t)src << 20 | (uint64_t)d; }
static bool reach_case(const char *name, uint64_t first_insn, uint32_t want_dyn, uint32_t want_size) {
    static uint8_t prog[0x80 + 16 * 8 + 64];
    memset(prog, 0, sizeof prog);
    uint64_t *code = (uint64_t *)(prog + 0x80);
    code[0] = code[4] = code[8] = 0x001f8000fc0007e0ull;  // scheduling words
    code[1] = first_insn;
    code[2] = 0x3818000000000000ull | 4ull << 39 | 0x160ull << 20 | 20ull << 8 | 7ull << 16 | 0;  // ISCADD R0, R20, 0x160, 4
    code[3] = 0xef90000000000000ull | 3ull << 36 | 4ull << 48 | 0ull << 8 | 7ull << 16 | 1;       // LDC R1, c[3][R0]
    code[5] = 0x1c00000000000000ull | 0xffffffffull << 20 | 20ull << 8 | 7ull << 16 | 20;          // IADD32I R20, R20, -1
    code[6] = 0x3660000000000000ull | 4ull << 49 | 1ull << 48 | 7ull << 39 | 20ull << 8 | 7ull << 16 | 0 << 3 | 7;  // ISETP.GT P0, R20, 0
    code[7] = 0xe240000000000000ull | (uint64_t)(-48 & 0xffffff) << 20 | 0ull << 16 | 0xf;          // @P0 BRA (to code[2])
    code[9] = mov(2, 1);  // (the read kept)
    code[10] = 0xe30000000007000full;  // EXIT
    code[11] = 0xe2400fffff87000full;  // BRA self
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.glsl = 1;
    ShaderInfo *si = maxwell_translate(prog, sizeof prog, STAGE_VERTEX, &key);
    bool ok = si->msl && (si->cbuf_mask & 8) && (si->cbuf_dynamic & 8) == want_dyn && si->cbuf_size[3] == want_size;
    if (!ok) printf("cbuf reach %s: FAILED (%s; dynamic %#x, size %u; want %#x, %u)\n", name, si->msl ? "translated" : si->error,
                    si->cbuf_dynamic, si->cbuf_size[3], want_dyn, want_size);
    maxwell_free(si);
    return ok;
}

static bool reach_tests(void) {
    bool ok = reach_case("constant start", mov32i(20, 2), 0, 512);  // reads 0x170 and 0x180: 25 vec4s
    ok &= reach_case("unknown start", mov(20, 21), 8, 0x10000);
    ok &= reach_case("past 64 KB", mov32i(20, 0x1000), 8, 0x10000);  // (the clamp reads the last vec4)
    printf("cbuf reach: %s\n", ok ? "3 checks passed" : "FAILED");
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    const char *mode = argc > 4 ? argv[4] : "both";
    const char *dir = argc > 5 ? argv[5] : NULL;
    bool do_msl = strcmp(mode, "glsl") != 0, do_glsl = strcmp(mode, "msl") != 0;
    maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = 0x10000000ull;
#ifdef MSL_REF
    old_maxwell_gmem_base = maxwell_gmem_base;
    old_maxwell_gmem_size = maxwell_gmem_size;
#else
    if (do_msl) { fprintf(stderr, "built without MSL_REF: glsl mode only\n"); do_msl = false; }
#endif
#ifdef WITH_GLSL
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_source_language(opt, shaderc_source_language_glsl);
    spv_context spv = spvContextCreate(SPV_ENV_VULKAN_1_3);
#else
    if (do_glsl) { fprintf(stderr, "built without WITH_GLSL: msl mode only\n"); do_glsl = false; }
#endif
    bool reach_ok = reach_tests();
    static uint8_t prog[0x80 + 64 * 32 + 128];
    long msl_ok = 0, msl_fail = 0, msl_diff = 0, gl_ok = 0, gl_tfail = 0, gl_tfail_only = 0, gl_cfail = 0, gl_vfail = 0;
    long by_stage[3] = {0};
    char reasons[64][96];
    long reason_n[64];
    int nreasons = 0;
    for (long n = 0; n < count; n++) {
        int stage = rnd(3) == 0 ? STAGE_VERTEX : rnd(2) ? STAGE_FRAGMENT : STAGE_COMPUTE;
        size_t len = gen_program(prog, sizeof prog, stage);
        ShaderKey key;
        gen_key(&key, stage);
        bool msl_good = true;
        if (do_msl) {
            ShaderInfo *a = maxwell_translate(prog, len, stage, &key);
#ifdef MSL_REF
            ShaderInfo *b = old_maxwell_translate(prog, len, stage, &key);
            if (!same_info(a, b)) {
                if (msl_diff < 20) dump(dir, "msl_diff", n, a->msl, b->msl, prog, len);
                msl_diff++;
            }
            old_maxwell_free(b);
#endif
            if (a->msl) msl_ok++; else msl_fail++;
            msl_good = a->msl != NULL;
            maxwell_free(a);
        }
#ifdef WITH_GLSL
        if (do_glsl) {
            key.glsl = 1;
            ShaderInfo *g = maxwell_translate(prog, len, stage, &key);
            if (!g->msl) {
                gl_tfail++;
                if (msl_good && do_msl) gl_tfail_only++;
                {  // tally the reasons (GLSL-only: the MSL translation succeeded)
                    char r[96];
                    snprintf(r, sizeof r, "%s%.24s", msl_good && do_msl ? "GLSL-only " : "", g->error);
                    for (char *q = r; *q; q++) if (*q >= '0' && *q <= '9') *q = '#';
                    int i = 0;
                    while (i < nreasons && strcmp(reasons[i], r)) i++;
                    if (i == nreasons && nreasons < 64) { strcpy(reasons[nreasons], r); reason_n[nreasons++] = 0; }
                    if (i < 64) reason_n[i]++;
                }
            } else {
                shaderc_shader_kind kind = stage == STAGE_VERTEX ? shaderc_vertex_shader
                                         : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
                shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), kind, "fuzz", "main", opt);
                if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                    if (gl_cfail < 50) dump(dir, "glsl_compile", n, g->msl, shaderc_result_get_error_message(res), prog, len);
                    gl_cfail++;
                } else {
                    spv_diagnostic diag = NULL;
                    spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                    spv_validator_options vo = spvValidatorOptionsCreate();
                    spv_result_t vr = spvValidateWithOptions(spv, vo, &bin, &diag);
                    spvValidatorOptionsDestroy(vo);
                    if (vr != SPV_SUCCESS) {
                        if (gl_vfail < 50) dump(dir, "glsl_validate", n, g->msl, diag ? diag->error : "?", prog, len);
                        gl_vfail++;
                    } else {
                        gl_ok++;
                        by_stage[stage == STAGE_VERTEX ? 0 : stage == STAGE_FRAGMENT ? 1 : 2]++;
                        if (dir && gl_ok <= (getenv("TT_DUMP_OK") ? atol(getenv("TT_DUMP_OK")) : 40)) dump(dir, "glsl_ok", n, g->msl, NULL, prog, len);
                    }
                    spvDiagnosticDestroy(diag);
                }
                shaderc_result_release(res);
            }
            maxwell_free(g);
        }
#endif
    }
    if (do_msl) printf("MSL: %ld programs, %ld translated, %ld failed in both, %ld DIFFERENCES\n", count, msl_ok, msl_fail, msl_diff);
    if (do_glsl) {
        printf("GLSL: %ld compiled+validated (vs %ld / fs %ld / cs %ld), %ld compile failures, %ld validation failures, "
               "%ld translation failures (%ld where MSL translated)\n",
               gl_ok, by_stage[0], by_stage[1], by_stage[2], gl_cfail, gl_vfail, gl_tfail, gl_tfail_only);
        for (int i = 0; i < nreasons; i++) printf("  translation failure: %-36s %ld\n", reasons[i], reason_n[i]);
    }
    return (msl_diff || gl_cfail || gl_vfail || !reach_ok) ? 1 : 0;
}
