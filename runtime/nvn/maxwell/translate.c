// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in shader.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks.
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tools/test_translate.c checks it).
#if defined(_WIN32)
#include <malloc.h>  // alloca
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane;
    bool uses_rscale;  // resolution-scale table (MSL: fragment / compute only)
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs the SPH interpolates linearly in screen space
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register ("multiply" mode)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG present: the gmem buffer is not readonly
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    bool uses_shared;        // LDS / STS: compute shared memory, smem_words words (a power of two)
    uint32_t smem_words;
    // (vertex) how the input attributes' values are used: the attribute each register holds (-1: none; a MOV
    // carries it along), and those registers the instruction being translated reads (attr_uses)
    int8_t attr_of[256];
    struct { uint8_t r; int8_t a; } reads[24];
    int nreads;
    uint32_t attr_float_use, attr_int_use;
    // (GLSL) structured control flow (structure_flow): if / else and do / while for the pc state machine. By
    // instruction index: the braces to write before it, and whether it is dead code (after an unconditional jump or
    // EXIT)
    bool structured;
    Str *flow_before;
    uint8_t *flow_dead;
} Ctx;

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    if (c->attr_of[r] >= 0 && c->nreads < 24) { c->reads[c->nreads].r = (uint8_t)r; c->reads[c->nreads++].a = c->attr_of[r]; }
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    c->attr_of[r] = -1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// Whether p is "(" + a uint operand + ")": a register (rN), a constant buffer word (cbN[i][j]) or a literal ending in u.
static bool uint_operand(const char *p) {
    if (*p++ != '(') return false;
    const char *q = p;
    if (*q == 'r' && q[1] >= '0' && q[1] <= '9') {
        for (q++; *q >= '0' && *q <= '9'; q++) {}
    } else if (!strncmp(q, "cb", 2) && q[2] >= '0' && q[2] <= '9') {
        for (q += 2; *q >= '0' && *q <= '9'; q++) {}
        for (int k = 0; k < 2; k++) {
            if (*q++ != '[') return false;
            if (*q < '0' || *q > '9') return false;
            while (*q >= '0' && *q <= '9') q++;
            if (*q++ != ']') return false;
        }
    } else if (*q >= '0' && *q <= '9') {
        while (id_char(*q)) q++;
        if (q[-1] != 'u') return false;
    } else {
        return false;
    }
    return *q == ')';
}

// How the instruction just emitted (text: its code) used the attribute values it read: as a float when each read is
// as_type<float>(rN), else as an integer (an address, bit operations, a conversion). Assignments to rN don't count.
static void attr_uses(Ctx *c, const char *text) {
    for (int k = 0; k < c->nreads; k++) {
        char name[8];
        int n = snprintf(name, sizeof name, "r%d", c->reads[k].r);
        bool as_float = false, as_int = false;
        for (const char *t = text; (t = strstr(t, name)); t += n) {
            if ((t > text && id_char(t[-1])) || id_char(t[n])) continue;
            if (!strncmp(t + n, " = ", 3)) continue;
            if (t - text >= 15 && !strncmp(t - 15, "as_type<float>(", 15) && t[n] == ')') as_float = true;
            else as_int = true;
        }
        if (as_float) c->attr_float_use |= 1u << c->reads[k].a;
        if (as_int) c->attr_int_use |= 1u << c->reads[k].a;
    }
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

// (GLSL: the built-in; the overloaded as_u helper would be a function call per instruction for drivers to inline)
static const char *fresult(Ctx *c, const char *e, bool sat, char *buf) {
    const char *cast = c->glsl ? "floatBitsToUint" : "as_type<uint>";
    if (sat) sprintf(buf, "%s(saturate(%s))", cast, e);
    else sprintf(buf, "%s(%s)", cast, e);
    return buf;
}
#define FRES(e, sat) fresult(c, (e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    default:
        if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
        else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

// A condition code test (flow control bits 0-4, CSETP bits 8-12) on the flags kept: zero (ccz), sign (ccs) and carry
// (cc bit 0); overflow isn't tracked (0). NULL for the tests this can't express (NUM, NaN, the CSM ones).
static const char *cc_test(int test) {
    switch (test) {
    case 0: return "false";                       // F
    case 15: return "true";                       // T
    case 2: case 10: return "ccz";                // EQ, EQU
    case 5: case 13: return "!ccz";               // NE, NEU
    case 1: case 9: return "ccs";                 // LT, LTU
    case 6: case 14: return "!ccs";               // GE, GEU
    case 4: case 12: return "(!ccz && !ccs)";     // GT, GTU
    case 3: case 11: return "(ccz || ccs)";       // LE, LEU
    case 16: return "true";                       // OFF
    case 17: return "((cc & 1u) == 0u)";          // LO
    case 18: case 31: return "(!ccs && !ccz)";    // SFF, RGT
    case 19: return "(ccz || (cc & 1u) == 0u)";   // LS
    case 20: return "((cc & 1u) != 0u && !ccz)";  // HI
    case 21: case 30: return "(ccs || ccz)";      // SFT, RLE
    case 22: return "((cc & 1u) != 0u)";          // HS
    case 23: return "false";                      // OFT
    default: return NULL;
    }
}

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind >= 3) sprintf(e, "as_type<uint>(float(in.a%d[%d]))", a, comp);  // integer formats read as float
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp < 2) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr.
                // "multiply" yields attr/w times a register: usually w (then attr), but BOTW's soft particles
                // pass w / clip.w there to get the projected screen position of their depth lookup (taken
                // as plain attr, each particle showed the whole depth buffer: tiled "fog" in the Shrine of
                // Resurrection).
                // An input the SPH interpolates linearly in screen space (declared noperspective) has no 1/w in
                // it: "pass" yields attr itself, "multiply" attr times the register. (BOTW's HUD vertex colors:
                // under the minimap's perspective camera, w ~ 989, attr/w left them black and transparent.)
                bool linear = (c->noperspective_mask >> a) & 1;
                if (c->ipa_mode == 0 && linear) sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ && linear) {
                    char rb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, rb));
                }
                else if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char rb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp,
                            reg(c, c->ipa_mul, rb));
                }
                else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
    if (c->stage == STAGE_VERTEX && addr >= 0x80 && addr < 0x280 && dst != RZ) c->attr_of[dst] = (int8_t)((addr - 0x80) >> 4);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = R((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d);
            break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg forms)
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18) {
        bool setp = (op & 0xfff8) == 0x5d20;
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        const char *y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        int cop = (int)bits(w, 35, 4), bop = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            bool hand = bits(w, 49, 1);
            if (hand) sp(&c->body, "      l = l && h; h = !l;\n");
            if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
            if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
        } else {
            bool bf = bits(w, 49, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    sp(&c->body, "    { float4 t = %s;\n", call);
    (void)slot;
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp); }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static _Thread_local char bufs[4][1024];  // (per thread: shaders translate on several at once, precompile.c)
    static _Thread_local int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX && !c->glsl) { snprintf(o, 1024, "%s", inner); return o; }  // (MSL: no table there)
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// Texel offsets are in guest texels: a scaled allocation (render-target scaling) has 1 / scale as many per guest
// texel, so an offset moves the coordinates by offset * scale / allocated size (rscale[1 + slot]).
static const char *off_scale(Ctx *c, int slot, int comps) {
    static _Thread_local char bufs[4][48];
    static _Thread_local int bi;
    char *o = bufs[bi++ & 3];
    c->uses_rscale = true;
    snprintf(o, 48, " * rscale[%d].%s", slot + 1, comps == 1 ? "x" : "xy");
    return o;
}

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x)%s / float(tsize%d(%s).x))", co[0], off, off_scale(c, slot, 1), slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s)%s / vec2(tsize%d(%s).xy))", co[0], co[1], off, off_scale(c, slot, 2), slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s)%s / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, off_scale(c, slot, 2), slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A)); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s))", slot, slot, F(A), F(Bv)); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(0))", slot, slot, F(A), F(Bv)); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(%s)))", slot, slot, F(A), F(A1), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(0)))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s)", slot, slot, F(A1), F(Bv), ALAYER(A, slot)); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s, level(0))", slot, slot, F(A1), F(Bv), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, %s, level(0)))", slot, slot, F(A1), F(Bv), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// Whether bits 0-7 of w name a register the instruction writes. Not for the compares that set predicates there
// (FSETP P0, PT reads as R7), stores (the data register), R2P and flow control.
static bool writes_gpr(uint64_t w) {
    static const uint16_t none[][2] = {  // top 16 bits: value, mask
        {0x5bb0, 0xfff0}, {0x4bb0, 0xfff0}, {0x36b0, 0xfef0},  // FSETP
        {0x5b60, 0xfff0}, {0x4b60, 0xfff0}, {0x3660, 0xfef0},  // ISETP
        {0x5d20, 0xfff8}, {0x5090, 0xfff8}, {0x50a0, 0xfff8},  // HSETP2, PSETP, CSETP
        {0x38f0, 0xfef8}, {0x5cf0, 0xfff8}, {0x4cf0, 0xfff8},  // R2P
        {0xeff0, 0xfff8}, {0xeed8, 0xfff8}, {0xef50, 0xfff8}, {0xef58, 0xfff8},  // ST attribute, STG, STL, STS
        {0xe300, 0xfff0}, {0xe330, 0xfff0}, {0xe240, 0xfff0}, {0xe250, 0xfff0},  // EXIT, KIL, BRA, BRX
        {0xe290, 0xfff0}, {0xe2a0, 0xfff0}, {0xe2b0, 0xfff0}, {0xe340, 0xfff0}, {0xe350, 0xfff0},  // SSY PBK PCNT BRK CONT
        {0xf0f8, 0xfff8}, {0xf0f0, 0xfff8}, {0x50b0, 0xfff8}, {0xef98, 0xfff8}, {0xf0a8, 0xfff8},  // SYNC DEPBAR NOP MEMBAR BAR
    };
    uint16_t op = (uint16_t)(w >> 48);
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++)
        if ((op & none[i][1]) == none[i][0]) return false;
    return true;
}

// A bindless handle in register r, read by the instruction at pc. When the nearest earlier write of r is
// MOV r, c[2][off] or LDC r, c[2][off] (the bound texture handles, where the bound forms' TidB points as well), the
// lookup is the bound one with TidB off / 4: returns that, or -1. A MOV r, Rs follows Rs back from there. BOTW 1.8.2
// loads the handle right before (MOV R11, c[2][0x20]; TMML.B ... R11). The scan is linear: a write of r on another
// path in between is not seen.
static int bindless_index(Ctx *c, uint32_t pc, int r) {
    if (r == RZ) return -1;
    const uint64_t *code = (const uint64_t *)c->code;
    for (int64_t i = (int64_t)(pc / 8) - 1; i >= 0 && (int64_t)(pc / 8) - i < 512; i--) {
        if (i % 4 == 0) continue;  // scheduling words
        uint64_t w = code[i];
        if ((int)bits(w, 0, 8) != r || !writes_gpr(w)) continue;
        bool always = bits(w, 16, 3) == PT && !bits(w, 19, 1);
        if (always && (w & 0xfff8000000000000ull) == 0x4c98000000000000ull && bits(w, 34, 5) == 2)  // MOV c[2][]
            return (int)bits(w, 20, 14);
        if (always && (w & 0xfff8000000000000ull) == 0xef90000000000000ull && bits(w, 8, 8) == RZ &&  // LDC c[2][]
            bits(w, 36, 5) == 2 && bits(w, 48, 3) == 4)
            return (int)(bits(w, 20, 16) / 4);
        if (always && (w & 0xfff8000000000000ull) == 0x5c98000000000000ull && bits(w, 39, 4) == 0xf)  // MOV r, Rs
            return bindless_index(c, (uint32_t)(i * 8), (int)bits(w, 20, 8));
        return -1;  // written by something else
    }
    return -1;
}

static int dim_type(int dim) {  // TexDim (bits 28-30: array bit, then 1d / 2d / 3d / cube)
    int target = dim >> 1;
    bool array = dim & 1;
    return target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
         : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
         : target == 2 ? TEXTYPE_3D
                       : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
}

// GLSL gather on slot `slot`: component `comp` (or the comparison with dref) of the four texels of the bilinear
// footprint at co (2 coordinates, 3 for cubes), with layer (uint expression) or NULL. off: NULL, one ivec2
// expression (AOFFI), or four (PTP: texel i comes from the footprint at co + off[i], as textureGatherOffsets).
// Offsets move the coordinates by offset / size of level 0, like glsl_sample's register offsets (no
// shaderImageGatherExtended needed).
static void glsl_gather(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref, int comp,
                        const char *const *off, int noff) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    if (type != TEXTYPE_2D && type != TEXTYPE_2D_ARRAY && !cube) {
        fail(c, "gather on texture type %d", type);
        strcpy(out, "vec4(0.0f)");
        return;
    }
    if (cube && noff) noff = 0;  // (cube lookups take no offset)
    char P[4][700], call[4][1000];
    int n = noff == 4 ? 4 : 1;
    for (int i = 0; i < n; i++) {
        char xy[500];
        if (cube) snprintf(xy, sizeof xy, "vec3(%s, %s, %s)", co[0], co[1], co[2]);
        else if (noff) snprintf(xy, sizeof xy, "(vec2(%s, %s) + vec2(%s)%s / vec2(tsize%d(0).xy))", co[0], co[1], off[i], off_scale(c, slot, 2), slot);
        else snprintf(xy, sizeof xy, "vec2(%s, %s)", co[0], co[1]);
        if (layer) snprintf(P[i], sizeof P[i], "vec%d(%s, float(%s))", cube ? 4 : 3, xy, layer);
        else snprintf(P[i], sizeof P[i], "%s", xy);
        if (t->shadow) snprintf(call[i], sizeof call[i], "textureGather(t%d, %s, %s)", slot, P[i], dref);
        else snprintf(call[i], sizeof call[i], "textureGather(t%d, %s, %d)", slot, P[i], comp);
    }
    if (n == 1) sprintf(out, "%s", call[0]);
    else sprintf(out, "vec4(%s.x, %s.y, %s.z, %s.w)", call[0], call[1], call[2], call[3]);
}

// TLD4 / TLD4.B: from Ra the array index (if any) then the coordinates; from Rb the bindless handle, the offsets
// (AOFFI: one register, 6-bit x and y at bits 0 and 8; PTP: two registers with four x / y pairs, a byte each) and
// the depth reference (register layout as Ryujinx, MIT, reads it).
static void emit_tld4(Ctx *c, uint64_t w, uint32_t pc, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    int type = dim_type((int)bits(w, 28, 3));
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1);
    int comp = bindless ? (int)bits(w, 38, 2) : (int)bits(w, 56, 2);
    int toff = bindless ? (int)bits(w, 36, 2) : (int)bits(w, 54, 2);  // 0 none, 1 AOFFI, 2 PTP
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    if (d == RZ) return;
    if (!c->glsl) { fail(c, "tld4 (msl)"); return; }
    int index = bindless ? bindless_index(c, pc, b) : (int)bits(w, 36, 13);
    if (index < 0 || index >= 8192) { fail(c, "bindless tld4 (handle not from c[2])"); return; }
    if (bindless) b = b == RZ ? RZ : b + 1;
    int slot = tex_slot(c, (uint32_t)index, type, dc, array);
    int ncoord = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY ? 3 : 2;
    char ra[8][16], lay[96], cs[3][40], ofs[4][96], drv[48], call[4096];
    int ri = 0;
    const char *layp = NULL;
    if (array) { snprintf(lay, sizeof lay, "min(%s, uint(tsize%d(0).z) - 1u)", reg(c, a, ra[ri++]), slot); layp = lay; if (a != RZ) a++; }
    const char *co[3];
    for (int i = 0; i < ncoord; i++) {
        snprintf(cs[i], sizeof cs[i], "as_type<float>(%s)", reg(c, a, ra[ri++]));
        co[i] = cs[i];
        if (a != RZ) a++;
    }
    const char *offp[4];
    int noff = 0;
    if (toff == 1 || toff == 2) {
        char pk[2][16];
        reg(c, b, pk[0]);
        if (b != RZ) b++;
        if (toff == 2) { reg(c, b, pk[1]); if (b != RZ) b++; }
        noff = toff == 2 ? 4 : 1;
        for (int i = 0; i < noff; i++) {
            const char *p = pk[(i * 2) >> 2];  // pairs 0, 1 in the first register, 2, 3 in the second
            int sx = ((i * 2) & 3) * 8, sy = ((i * 2 + 1) & 3) * 8;
            snprintf(ofs[i], sizeof ofs[i], "ivec2(int(%s << %du) >> 26, int(%s << %du) >> 26)", p, 26 - sx, p, 26 - sy);
            offp[i] = ofs[i];
        }
    }
    const char *drp = NULL;
    if (dc) { char rb_[16]; snprintf(drv, sizeof drv, "as_type<float>(%s)", reg(c, b, rb_)); drp = drv; }
    glsl_gather(c, call, slot, co, layp, drp, comp, offp, noff);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TLD4S: a 2D gather. Coordinates in Ra, Rb (or Ra, Ra+1 when there is an offset or a depth reference, which then
// come from Rb, Rb+1); results like TEXS: two components to d0 / d0+1, or four with d1 / d1+1 as well.
static void emit_tld4s(Ctx *c, uint64_t w, bool f16) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    bool dc = bits(w, 50, 1), aoffi = bits(w, 51, 1);
    int comp = (int)bits(w, 52, 2);
    if (d0 == RZ && d1 == RZ) return;
    if (!c->glsl) { fail(c, "tld4s (msl)"); return; }
    int slot = tex_slot(c, index, TEXTYPE_2D, dc, false);
    char r0[16], r1[16], cs[2][40], ofs[96], drv[48], call[4096];
    if (dc || aoffi) {
        snprintf(cs[0], sizeof cs[0], "as_type<float>(%s)", reg(c, a, r0));
        snprintf(cs[1], sizeof cs[1], "as_type<float>(%s)", reg(c, a == RZ ? RZ : a + 1, r1));
    } else {
        snprintf(cs[0], sizeof cs[0], "as_type<float>(%s)", reg(c, a, r0));
        snprintf(cs[1], sizeof cs[1], "as_type<float>(%s)", reg(c, b, r1));
    }
    const char *co[2] = {cs[0], cs[1]}, *offp[1] = {ofs};
    int rb = b;
    if (aoffi) {
        char p[16];
        reg(c, rb, p);
        snprintf(ofs, sizeof ofs, "ivec2(int(%s << 26u) >> 26, int(%s << 18u) >> 26)", p, p);
        if (rb != RZ) rb++;
    }
    const char *drp = NULL;
    if (dc) { char q[16]; snprintf(drv, sizeof drv, "as_type<float>(%s)", reg(c, rb, q)); drp = drv; }
    glsl_gather(c, call, slot, co, NULL, drp, comp, offp, aoffi ? 1 : 0);
    write_tex_result(c, slot, call, d1 == RZ ? 0x3 : 0xf, d0, d1, true, f16);
}

// TMML / TMML.B: the level of detail a lookup at the coordinates would use, 8.8 fixed point: component 0 the
// computed LOD, 1 the level accessed (textureQueryLod's y and x); 2 and 3 read 0. Only fragment shaders have
// derivatives: other stages get 0.
static void emit_tmml(Ctx *c, uint64_t w, uint32_t pc, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    int dim = (int)bits(w, 28, 3), type = dim_type(dim);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    if (d == RZ) return;
    if (!c->glsl) { fail(c, "tmml (msl)"); return; }
    int index = bindless ? bindless_index(c, pc, b) : (int)bits(w, 36, 13);
    if (index < 0 || index >= 8192) { fail(c, "bindless tmml (handle not from c[2])"); return; }
    bool fresh = !c->tex_slot[index];
    int slot = tex_slot(c, (uint32_t)index, -1, false, false);  // a sample decides the type; this only reads the LOD
    if (fresh) { c->info->tex[slot].type = (uint8_t)type; c->info->tex[slot].is_array = dim & 1; }
    int n = c->info->tex[slot].type == TEXTYPE_1D || c->info->tex[slot].type == TEXTYPE_1D_ARRAY ? 1
          : c->info->tex[slot].type == TEXTYPE_2D || c->info->tex[slot].type == TEXTYPE_2D_ARRAY ? 2 : 3;
    if (dim & 1 && a != RZ) a++;  // (the array index is not part of an LOD query)
    char rr[3][16], P[200];
    if (n == 1) snprintf(P, sizeof P, "as_type<float>(%s)", reg(c, a, rr[0]));
    else if (n == 2) snprintf(P, sizeof P, "vec2(as_type<float>(%s), as_type<float>(%s))", reg(c, a, rr[0]),
                              reg(c, a == RZ ? RZ : a + 1, rr[1]));
    else snprintf(P, sizeof P, "vec3(as_type<float>(%s), as_type<float>(%s), as_type<float>(%s))", reg(c, a, rr[0]),
                  reg(c, a == RZ ? RZ : a + 1, rr[1]), reg(c, a == RZ ? RZ : a + 2, rr[2]));
    if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    { vec2 lq = textureQueryLod(t%d, %s);\n", slot, P);
    else sp(&c->body, "    { vec2 lq = vec2(0.0f);\n");
    sp(&c->body, "      uvec4 q = uvec4(uint(int(lq.y * 256.0f)), uint(int(lq.x * 256.0f)), 0u, 0u);\n");
    int k = 0;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i)) {
            int r = d + k++;
            if (r >= RZ) break;
            c->reg_used[r] = 1;
            sp(&c->body, "      r%d = q[%d];\n", r, i);
        }
    sp(&c->body, "    }\n");
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20. TEX.B takes its
// handle from gpr20 first (see bindless_index).
static void emit_tex(Ctx *c, uint64_t w, uint32_t pc, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    int bi = bindless ? bindless_index(c, pc, b) : 0;
    uint32_t index = bindless ? (uint32_t)bi : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    if (bindless && (bi < 0 || bi >= 8192)) { fail(c, "bindless tex (handle not from c[2])"); return; }
    if (bindless) b = b == RZ ? RZ : b + 1;
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        if (array) { sprintf(lay, "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) { sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot); c->reg_used[a] = 1; ar = a + 1; }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) { c->reg_used[pr] = 1; sprintf(dref, "as_type<float>(r%d)", pr); pr++; }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char call[1024];
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s(%s)%s%s, %s%s%s))", slot, slot, vec, coords, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s(%s)%s%s%s%s)", slot, slot, vec, coords, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ instruction translation
static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    // flow control (BRA, BRX, EXIT, KIL, BRK, CONT, SYNC) also tests the condition code (bits 0-4, T = always): BOTW's
    // sky scattering shader branches on FSET.CC's result (BRA NEU), the ground / sky split of its table lookup
    const char *flow_cc = NULL;
    if (top12 == 0xe240000000000000ull || top12 == 0xe250000000000000ull || top12 == 0xe300000000000000ull ||
        top12 == 0xe330000000000000ull || top12 == 0xe340000000000000ull || top12 == 0xe350000000000000ull ||
        (w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) {
        int test = (int)bits(w, 0, 5);
        if (test != 15) {
            flow_cc = cc_test(test);
            if (!flow_cc) { fail(c, "flow test %d at %#x", test, pc); return; }
            c->uses_cc = true;
            guarded = true;
        }
    }
    if (flow_cc) sp(&c->body, "  if (%s && %s) {\n", P(pr, prn), flow_cc);
    else if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->structured ? "    exited = true;\n" : c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("SAVAGEWIND_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        if (!c->structured) sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {  // SSY
        c->uses_branches = true;
        if (!c->structured) sp(&c->body, "    ssy[ssy_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {  // PBK
        c->uses_branches = true;
        sp(&c->body, "    pbk[pbk_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {  // PCNT
        c->uses_branches = true;
        sp(&c->body, "    pcnt[pcnt_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {  // SYNC
        if (!c->structured) sp(&c->body, "    pc = ssy[--ssy_n]; continue;\n");
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {  // BRK
        sp(&c->body, "    pc = pbk[--pbk_n]; continue;\n");
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // CONT
        sp(&c->body, "    pc = pcnt[pcnt_n - 1]; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);  // unsigned (Ryujinx InstLdc.CbufOffset)
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        for (int i = 0; i < n; i++) {
            char cb[256];
            cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (!dyn && (off + 4 * i) & 0) {}
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        emit_tex(c, w, pc, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, pc, true);
    } else if (OPM(0xc800000000000000ull, 0xfc00000000000000ull)) {  // TLD4
        emit_tld4(c, w, pc, false);
    } else if (OPM(0xdec0000000000000ull, 0xffc0010000000000ull)) {  // TLD4.B
        emit_tld4(c, w, pc, true);
    } else if (OPM(0xdf00000000000000ull, 0xffc0000000000000ull)) {  // TLD4S
        emit_tld4s(c, w, false);
    } else if (OPM(0xdf80000000000000ull, 0xffc0000000000000ull)) {  // TLD4S .F16
        emit_tld4s(c, w, true);
    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {  // TMML
        emit_tmml(c, w, pc, false);
    } else if (OPM(0xdf60000000000000ull, 0xfff8000000000000ull)) {  // TMML.B
        emit_tmml(c, w, pc, true);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            c->uses_rscale = true;
            sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) / rscale[%d].x + 0.5f)), "
               "uint(floor(float(tsize%d(int(lv)).y) / rscale[%d].y + 0.5f)), 1u, uint(textureQueryLevels(t%d)));\n",
               R(a), slot, slot + 1, slot, slot + 1, slot);
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        if (bits(w, 47, 1)) {  // .CC (before the write: the sources may include the destination): Z, S of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = %s; cc = 0u; }\n", u, bits(w, 52, 1) ? "false" : "ccv != 0u");
        }
        set_reg(c, d, u);
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        int8_t a = b_form(w) == B_REG && bits(w, 20, 8) != RZ ? c->attr_of[bits(w, 20, 8)] : -1;
        set_reg(c, d, B(false));
        if (a >= 0 && d != RZ) { c->attr_of[d] = a; c->nreads = 0; }  // (a copy: neither a float nor an integer use)
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        switch (mode) {
        case 4: sprintf(t, "rint(%s)", x); break;
        case 5: sprintf(t, "floor(%s)", x); break;
        case 6: sprintf(t, "ceil(%s)", x); break;
        case 7: sprintf(t, "trunc(%s)", x); break;
        default: sprintf(t, "%s", x);
        }
        if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
               bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (bits(w, 40, 1)) fail(c, "bfe brev");
        sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);  // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
        case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
        case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
        case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
        case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
        default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        case 0x00: set_reg(c, d, "0u"); break;
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        const char *sh = ((w >> 60) == 3) ? B(false) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        if (right) sprintf(t, "uint((((ulong(%s) << 32) | ulong(%s)) >> (%s & 31u)))", hi, R(a), sh);
        else sprintf(t, "uint((((ulong(%s) << 32) | ulong(%s)) << (%s & 31u)) >> 32)", hi, R(a), sh);
        set_reg(c, d, t);
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr,
           (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        if (c->glsl) {  // gmem_.w[]: 32-bit words; sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(gmem_.w[uint(ga >> 2)] >> (uint(ga & 3ul) * 8u)) & 0xFFu",
                "uint(sx8(gmem_.w[uint(ga >> 2)] >> (uint(ga & 3ul) * 8u)))",
                "(gmem_.w[uint(ga >> 2)] >> (uint(ga & 2ul) * 8u)) & 0xFFFFu",
                "uint(sx16(gmem_.w[uint(ga >> 2)] >> (uint(ga & 2ul) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = gmem_.w[uint(ga >> 2) + %du]; else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = uint(ga & %s) * 8u; atomicAnd(gmem_.w[wi], ~(0x%xu << sh)); "
                       "atomicOr(gmem_.w[wi], (%s & 0x%xu) << sh); }\n", size < 2 ? "3ul" : "2ul", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) gmem_.w[uint(ga >> 2) + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1); }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef48000000000000ull, 0xfff8000000000000ull) || OPM(0xef58000000000000ull, 0xfff8000000000000ull)) {  // LDS / STS
        // compute shared memory: a word array (byte / short accesses within a word, like LDL / STL). The address is
        // Ra + a signed 24-bit offset; it wraps at the array's size.
        if (c->stage != STAGE_COMPUTE || !c->glsl) { fail(c, "lds/sts outside a vulkan compute shader"); return; }
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        uint32_t m = c->smem_words - 1;
        c->uses_shared = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%uu", (uint32_t)off);
        else sprintf(addr, "(%s + %uu)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint sa = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = smem[(sa >> 2) & %uu]; uint sh = (sa & 3u) * 8u; r%d = %s;\n", m, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = smem[((sa >> 2) + %du) & %uu];\n", d + k, k, m); }
            }
        } else {
            if (size < 4) {
                uint32_t bm = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (sa & 3u) * 8u; uint i = (sa >> 2) & %uu; smem[i] = (smem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   m, bm, R(d), bm);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      smem[((sa >> 2) + %du) & %uu] = %s;\n", k, m, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));  // no SIMD-group functions in vertex functions: lane-local value
        } else if (c->glsl) {  // a quad xor (lane mask < 4) is the subgroup xor within the quad
            static const char *const fn[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { fail(c, "r2p cc"); }
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
           (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP
        int test = (int)bits(w, 8, 5);
        const char *cond = cc_test(test);
        if (!cond) { cond = "false"; fail(c, "csetp test %d", test); }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET
        fail(c, "cset");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) {
    return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull);
}

// ------------------------------------------------------------------ GLSL output

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"}, {"front_facing", "gl_FrontFacing"},
        {"point_coord", "gl_PointCoord"}, {"lane", "gl_SubgroupInvocationID"}, {"vid", "uint(gl_VertexIndex)"},
        {"iid", "uint(gl_InstanceIndex)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            // a register, a constant buffer word or an unsigned literal is a uint: the built-in, not the overloaded
            // helper (a function call per use, which every driver then inlines: thousands in a big shader)
            if (tn == 5 && !strncmp(p + 1, "float", 5) && uint_operand(gt + 1)) sp(o, "uintBitsToFloat");
            else if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        sp(o, "%s", rep);
    }
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    // MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n"
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n"
    // half -> float by bits, not unpackHalf2x16: NVIDIA's driver recognises unpacked halves and runs some of the
    // fp32 math after them in native fp16, differently per shader, so a depth pre-pass and its colour pass (EQUAL
    // test) placed wind-animated foliage apart: black slashes / flicker (tools: notes/PLAN.md 2026-10-08)
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n"
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint lo = packHalf2x16(vec2(r.x, 0.0f)), hi = packHalf2x16(vec2(r.y, 0.0f)); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

// The prelude, a line each: directives (kept) and helpers by name (kept when called).
enum { PRELUDE_MAX = 64 };
typedef struct {
    const char *line[PRELUDE_MAX];
    size_t len[PRELUDE_MAX];
    char name[PRELUDE_MAX][24];  // "" for directives
    bool keep[PRELUDE_MAX];
    int n;
} Prelude;

// Marks the helpers text calls: identifiers followed by '(' that name one (every overload of that name).
static void prelude_mark_calls(Prelude *pr, const char *text, const char *end) {
    for (const char *t = text; t < end;) {
        if (!id_start(*t) || (t > text && id_char(t[-1]))) { t++; continue; }
        const char *u = t;
        while (u < end && id_char(*u)) u++;
        if (u < end && *u == '(')
            for (int i = 0; i < pr->n; i++)
                if (!pr->keep[i] && pr->name[i][0] && (size_t)(u - t) == strlen(pr->name[i]) && !memcmp(t, pr->name[i], u - t))
                    for (int j = i; j < pr->n; j++) if (!strcmp(pr->name[j], pr->name[i])) pr->keep[j] = true;
        t = u;
    }
}

// The prelude's directives and the helpers the rest of the shader calls, directly or through another helper:
// glslang parses every function it is given, ~2 ms a shader for all of them (a quarter of an average shader's
// GLSL -> SPIR-V time).
static char *glsl_with_prelude(const char *rest) {
    Prelude pr = {0};
    for (const char *p = glsl_prelude; *p && pr.n < PRELUDE_MAX; pr.n++) {
        int i = pr.n;
        const char *e = strchr(p, '\n');
        pr.line[i] = p;
        pr.len[i] = e ? (size_t)(e - p + 1) : strlen(p);
        p += pr.len[i];
        const char *par = pr.line[i][0] == '#' ? NULL : memchr(pr.line[i], '(', pr.len[i]);
        if (!par) { pr.keep[i] = true; continue; }  // (directives, and the #define everything uses)
        const char *q = par;
        while (q > pr.line[i] && id_char(q[-1])) q--;
        snprintf(pr.name[i], sizeof pr.name[i], "%.*s", (int)(par - q), q);
    }
    prelude_mark_calls(&pr, rest, rest + strlen(rest));
    for (bool more = true; more;) {  // helpers that call helpers
        bool before[PRELUDE_MAX];
        memcpy(before, pr.keep, sizeof before);
        for (int i = 0; i < pr.n; i++)
            if (pr.keep[i] && pr.name[i][0]) prelude_mark_calls(&pr, pr.line[i], pr.line[i] + pr.len[i]);
        more = memcmp(before, pr.keep, sizeof before) != 0;
    }
    Str s = {0};
    for (int i = 0; i < pr.n; i++) if (pr.keep[i]) sp(&s, "%.*s", (int)pr.len[i], pr.line[i]);
    sp(&s, "%s", rest);
    return s.buf;
}

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][2] = {
        {"sampler1D", "sampler1DShadow"}, {"sampler2D", "sampler2DShadow"}, {"sampler3D", "sampler3D"},
        {"samplerCube", "samplerCubeShadow"}, {"sampler2DArray", "sampler2DArrayShadow"},
        {"samplerCubeArray", "samplerCubeArrayShadow"}, {"sampler1DArray", "sampler1DArrayShadow"}};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    return names[ty][t->shadow ? 1 : 0];
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};  // (the prelude goes in front at the end: glsl_with_prelude)
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    info->writes_gmem = info->uses_gmem && c->uses_gmem_store;
    if (info->uses_gmem)
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem { uint w[]; } gmem_;\n", set, GMEM_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
    if (c->uses_rscale) {  // (vertex shaders too: texel fetches from scaled targets, e.g. depth under the sun's flare)
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[65]; };\n", set);
        info->uses_rscale = true;
    }
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[2]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
    }
    if (stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 || kind == 3 ? "ivec4" : kind == 2 || kind == 4 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black)
        sp(&s, "invariant gl_Position;\nprecise gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
        if (c->uses_shared) sp(&s, "shared uint smem[%u];\n", c->smem_words);
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
    }
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (c->structured) {
        sp(&s, "  bool exited = false; uint loops = 0u;\n");
        sp(&s, "%s", body.buf ? body.buf : "");
    } else if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0u; uint pbk[16]; uint pbk_n = 0u; uint pcnt[16]; uint pcnt_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = glsl_with_prelude(s.buf);
    free(s.buf);
    info->attr_mask = c->in_attr_mask;
    info->attr_int_use = c->attr_int_use & ~c->attr_float_use;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

// The end of a shader's code (in instruction words): the last EXIT followed by self-branch padding, or n words.
static size_t code_end(const uint8_t *code, size_t n) {
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;  // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    return end;
}

size_t maxwell_binary_size(const uint8_t *blob, size_t max_size, int stage) {
    size_t head = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (max_size <= head) return max_size;
    return head + code_end(blob + head, (max_size - head) / 8) * 8;
}

// The cache of translations (runtime/nvn/vulkan/shaders.c) starts over when this id changes: the Makefile's hash of
// this file and shader.h, or, built without it, the time of the build.
#ifndef XLATE_ID
#define XLATE_ID __DATE__ " " __TIME__
#endif
const char *maxwell_translator_id(void) { return XLATE_ID; }

// SPH ImapGenericVector (0x18): a vertex shader's 4 bits per input attribute, a fragment shader's byte per varying
// (2 bits per component, 0 unused).
uint32_t maxwell_input_mask(const uint8_t *blob, int stage) {
    const uint8_t *sph = blob + 0x30;
    uint32_t m = 0;
    for (int i = 0; i < 32 && stage == STAGE_VERTEX; i++)
        if ((sph[0x18 + i / 2] >> ((i & 1) * 4)) & 0xF) m |= 1u << i;
    for (int i = 0; i < 32 && stage == STAGE_FRAGMENT; i++)
        if (sph[0x18 + i]) m |= 1u << i;
    return m;
}

uint32_t maxwell_output_targets(const uint8_t *blob) { return *(const uint32_t *)(blob + 0x30 + 0x48); }

// Whether a translation depends on more than the binary and the key (debugging switches): not to be cached.
bool maxwell_translation_uncacheable(void) {
    return maxwell_dbg_code || maxwell_dbg_vs || getenv("SAVAGEWIND_SAVE_MSL") || getenv("SAVAGEWIND_NO_DISCARD") ||
           getenv("SAVAGEWIND_XLATE_NO_STRUCTURE");
}

// ---- structured control flow (GLSL). Shaders whose branches nest become if / else and do / while in address order
// instead of the pc state machine:
//   @P BRA L; <a>; L:                  ->  if (!P) { <a> }
//   @P BRA L; <a>; BRA J; L: <b>; J:   ->  if (!P) { <a> } else { <b> }     (a SYNC to the join: the same)
//   L: <a>; @P BRA L                   ->  do { <a> } while (P)
//   @P EXIT; <rest>                    ->  if (P) exited = true; if (!exited) { <rest> }
// A jump to where the enclosing range goes when it ends (the join after an else, say) ends the range. Anything else
// (PBK / BRK / PCNT / CONT loops, BRX, breaking out of a do / while) keeps the state machine.
// SAVAGEWIND_XLATE_NO_STRUCTURE=1: always the state machine.
enum { FLOW_NONE, FLOW_JUMP, FLOW_EXIT, FLOW_SSY, FLOW_OTHER };  // (OTHER: loops through the PBK / PCNT stacks, BRX)

typedef struct {
    Ctx *c;
    const uint64_t *code;
    size_t end;
    uint32_t *target;    // jumps: target instruction index (SYNC: its join)
    uint32_t *loop_end;  // a do / while's first instruction: the index of the jump back to it
    bool ok;
} Flow;

// Where control goes when a range ends: its end, and the indexes jumps can name for it.
typedef struct { size_t idx[8]; int n; } Ends;

static int flow_kind(uint64_t w) {
    if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull || (w & 0xfff8000000000000ull) == 0xf0f8000000000000ull)
        return FLOW_JUMP;  // BRA, SYNC
    if ((w & 0xfff0000000000000ull) == 0xe300000000000000ull) return FLOW_EXIT;
    if ((w & 0xfff0000000000020ull) == 0xe290000000000000ull) return FLOW_SSY;
    if ((w & 0xfff0000000000020ull) == 0xe2a0000000000000ull || (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull ||
        (w & 0xfff0000000000000ull) == 0xe340000000000000ull || (w & 0xfff0000000000000ull) == 0xe350000000000000ull ||
        (w & 0xfff0000000000000ull) == 0xe250000000000000ull)
        return FLOW_OTHER;  // PBK PCNT BRK CONT BRX
    return FLOW_NONE;
}

// The condition under which a flow instruction acts (its predicate and condition-code test, as emit_insn guards
// it), or NULL: always.
static const char *flow_cond(Ctx *c, uint64_t w, char *buf, size_t n) {
    int pr = (int)bits(w, 16, 3), test = (int)bits(w, 0, 5);
    bool prn = bits(w, 19, 1);
    const char *cc = test != 15 ? cc_test(test) : NULL;
    if (cc) c->uses_cc = true;
    if (pr == PT && !prn && !cc) return NULL;
    char pb[16];
    snprintf(buf, n, "%s && %s", pred(c, pr, prn, pb), cc ? cc : "true");
    return buf;
}

static size_t next_insn(size_t i) { return (i + 1) % 4 == 0 ? i + 2 : i + 1; }
static size_t prev_insn(size_t i) { return (i - 1) % 4 == 0 ? i - 2 : i - 1; }

static void flow_dead_range(Flow *f, size_t from, size_t to) {
    for (size_t k = from; k < to && k < f->end; k++) f->c->flow_dead[k] = 1;
}

// The ends of a range [.., hi) inside one that ends at parent_hi with parent's ends: hi itself, also what the
// instruction at hi jumps to (unconditionally) when given (alias, 0: none), and the parent's when they end together.
static Ends ends_of(size_t hi, size_t alias, size_t parent_hi, const Ends *parent) {
    Ends e = {{hi}, 1};
    if (alias) e.idx[e.n++] = alias;
    if (hi == parent_hi || (alias && alias == parent_hi))
        for (int k = 0; k < parent->n && e.n < 8; k++) e.idx[e.n++] = parent->idx[k];
    return e;
}

static bool is_end(const Ends *e, size_t t) {
    for (int k = 0; k < e->n; k++) if (e->idx[k] == t) return true;
    return false;
}

static bool structure(Flow *f, size_t lo, size_t hi, const Ends *ends, int depth);

// What follows [from, hi) once a construct may have EXITed: only if (!exited).
static void structure_rest(Flow *f, size_t from, size_t hi, const Ends *ends, int depth) {
    sp(&f->c->flow_before[from], "  if (!exited) {\n");
    Ends e = ends_of(hi, 0, hi, ends);
    structure(f, from, hi, &e, depth + 1);
    sp(&f->c->flow_before[hi], "  }\n");
}

// Instructions [lo, hi): control enters at lo and leaves at hi (a jump to one of ends: the same). Returns whether
// it may EXIT.
static bool structure(Flow *f, size_t lo, size_t hi, const Ends *ends, int depth) {
    Str *before = f->c->flow_before;
    if (depth > 48) { f->ok = false; return false; }
    for (size_t i = lo; i < hi && f->ok;) {
        if (i % 4 == 0) { i++; continue; }
        size_t back = f->loop_end[i];
        if (back && i != lo && back < hi) {  // a do / while: [i, back], back jumps to i while its condition holds
            uint64_t bw = f->code[back];
            char bc[96];
            const char *cond = flow_cond(f->c, bw, bc, sizeof bc);
            if (!cond) { f->ok = false; return false; }  // (an endless loop left by breaks)
            sp(&before[i], "  do {\n");
            Ends e = {{back}, 1};
            bool exits = structure(f, i, back, &e, depth + 1);
            size_t after = next_insn(back);
            sp(&before[after], "  } while ((%s) && !exited && ++loops < 100000u);\n", cond);
            if (exits && after < hi) { structure_rest(f, after, hi, ends, depth); return true; }
            i = after;
            continue;
        }
        uint64_t w = f->code[i];
        int kind = flow_kind(w);
        if (kind == FLOW_OTHER || ((kind == FLOW_JUMP || kind == FLOW_EXIT) && bits(w, 0, 5) != 15 && !cc_test((int)bits(w, 0, 5)))) {
            f->ok = false;  // (loops through the PBK / PCNT stacks, BRX, a condition-code test the translator doesn't know)
            return false;
        }
        char cb[96];
        const char *cond = kind == FLOW_JUMP || kind == FLOW_EXIT ? flow_cond(f->c, w, cb, sizeof cb) : NULL;
        size_t n = next_insn(i);
        if (kind == FLOW_EXIT) {
            if (!cond) { flow_dead_range(f, n, hi); return true; }
            if (n < hi) structure_rest(f, n, hi, ends, depth);
            return true;
        }
        if (kind != FLOW_JUMP) { i = n; continue; }
        size_t t = f->target[i];
        if (t > hi && is_end(ends, t)) t = hi;
        if (t <= i || t > hi) { f->ok = false; return false; }  // (backward: only a do / while's own jump)
        if (!cond) {  // what lies between is dead
            flow_dead_range(f, n, t);
            i = t;
            continue;
        }
        if (t == n) { i = n; continue; }  // (a branch to the next instruction)
        // if / else: the code before the target ends with an unconditional jump further on
        size_t last = prev_insn(t), after = 0;
        uint64_t lw = f->code[last];
        char lb[96];
        if (last >= n && flow_kind(lw) == FLOW_JUMP && !flow_cond(f->c, lw, lb, sizeof lb) && !f->loop_end[t]) {
            size_t j = f->target[last];
            if (j > t && j <= hi) after = j;
            else if (j > hi && is_end(ends, j)) after = hi;
        }
        sp(&before[n], "  if (!(%s)) {\n", cond);
        bool exits;
        if (after) {
            Ends te = ends_of(last, f->target[last], hi, ends);
            exits = structure(f, n, last, &te, depth + 1);
            f->c->flow_dead[last] = 1;
            sp(&before[t], "  } else {\n");
            Ends ee = ends_of(after, 0, hi, ends);
            exits |= structure(f, t, after, &ee, depth + 1);
        } else {
            after = t;
            Ends te = ends_of(t, 0, hi, ends);
            exits = structure(f, n, t, &te, depth + 1);
        }
        sp(&before[after], "  }\n");
        if (exits && after < hi) { structure_rest(f, after, hi, ends, depth); return true; }
        i = after;
    }
    return false;
}

static void structure_flow(Ctx *c, const uint64_t *code, size_t end) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_XLATE_NO_STRUCTURE") != NULL;
    if (off) return;
    Flow f = {c, code, end, calloc(end + 1, sizeof(uint32_t)), calloc(end + 1, sizeof(uint32_t)), true};
    uint32_t ssy[16];
    int nssy = 0;
    for (size_t i = 1; i < end && f.ok; i = next_insn(i)) {  // targets; a SYNC goes to its innermost SSY's join
        uint64_t w = code[i];
        while (nssy && ssy[nssy - 1] <= i) nssy--;
        int64_t t = norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24)) / 8;
        if ((w & 0xfff0000000000020ull) == 0xe290000000000000ull) {  // SSY
            if (nssy == 16 || t <= (int64_t)i || t > (int64_t)end || (nssy && (uint32_t)t > ssy[nssy - 1])) f.ok = false;
            else ssy[nssy++] = (uint32_t)t;
        } else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) {  // SYNC
            if (!nssy) f.ok = false;
            else f.target[i] = ssy[nssy - 1];
        } else if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull) {  // BRA
            if (t < 1 || t > (int64_t)end) f.ok = false;
            else f.target[i] = (uint32_t)t;
            if (f.ok && t <= (int64_t)i) {  // a jump back: a do / while from t (one per first instruction)
                if (f.loop_end[t] || t % 4 == 0) f.ok = false;
                else f.loop_end[t] = (uint32_t)i;
            }
        }
    }
    c->flow_before = calloc(end + 1, sizeof(Str));
    c->flow_dead = calloc(end + 1, 1);
    Ends top = {{end}, 1};
    if (f.ok) structure(&f, 1, end, &top, 0);
    if (f.ok && f.loop_end[1]) f.ok = false;  // (a loop around everything: from the first instruction)
    free(f.target);
    free(f.loop_end);
    if (f.ok) { c->structured = true; return; }
    for (size_t i = 0; i <= end; i++) free(c->flow_before[i].buf);
    free(c->flow_before);
    free(c->flow_dead);
    c->flow_before = NULL;
    c->flow_dead = NULL;
}

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    memset(c->attr_of, 0xFF, sizeof c->attr_of);
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    // shared memory: the size the control section gives (1 KB if none), as a power of two up to 32 KB
    uint32_t sw = key && key->shared_size ? (key->shared_size + 3) / 4 : 256;
    for (c->smem_words = 1; c->smem_words < sw && c->smem_words < 8192; c->smem_words *= 2) {}
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = blob + 0x80;
    // compute programs have neither the 0x30-byte NVN header nor an SPH: code starts at the blob
    if (stage == STAGE_COMPUTE) code = blob;
    c->code = code;
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
        // ImapGenericVector[32] (SPH 0x18): 2 bits per component of each generic input, 1 constant, 2 perspective,
        // 3 screen-linear. An input with a screen-linear component is noperspective (in practice all its used
        // components agree).
        for (int i = 0; i < 32; i++)
            for (int k = 0; k < 4; k++)
                if (((sph[0x18 + i] >> (k * 2)) & 3) == 3) c->noperspective_mask |= 1u << i;
    }
    size_t end = code_end(code, (max_size - (code - blob)) / 8);
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: any instruction may be a target
            any_branch = true;
            memset(label, 1, end);
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    if (c->glsl && any_branch) structure_flow(c, (const uint64_t *)code, end);
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("SAVAGEWIND_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    for (size_t i = 0; i < end && !c->fail; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (c->structured) {
            if (c->flow_before[i].buf) sp(&c->body, "%s", c->flow_before[i].buf);
            int kind = flow_kind(((const uint64_t *)code)[i]);
            if (c->flow_dead[i] || kind == FLOW_JUMP || kind == FLOW_SSY) continue;  // (jumps: in the braces)
        } else if (any_branch && label[i]) {
            sp(&c->body, "  case %uu:\n", pc);
        }
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        c->nreads = 0;
        size_t before = c->body.len;
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        if (c->nreads && c->body.buf) attr_uses(c, c->body.buf + before);
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    if (c->structured) {
        if (c->flow_before[end].buf) sp(&c->body, "%s", c->flow_before[end].buf);
        for (size_t i = 0; i <= end; i++) free(c->flow_before[i].buf);
        free(c->flow_before);
        free(c->flow_dead);
    }
    free(label);
    if (c->fail) { free(c->body.buf); free(c); return info; }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); uint lo = uint(as_type<ushort>(half(r.x))), hi = uint(as_type<ushort>(half(r.y))); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 || kind == 3 ? "int" : kind == 2 || kind == 4 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    info->writes_gmem = info->uses_gmem && c->uses_gmem_store;
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color target 0 get a
    // per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched).
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        if (t->shadow) sp(&args, ", depth%s<float> t%d [[texture(%d)]], sampler s%d [[sampler(%d)]]", tn, i, i, i, i);
        else sp(&args, ", texture%s<float> t%d [[texture(%d)]], sampler s%d [[sampler(%d)]]", tn, i, i, i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0; uint pbk[16]; uint pbk_n = 0; uint pcnt[16]; uint pcnt_n = 0;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->attr_int_use = c->attr_int_use & ~c->attr_float_use;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}
