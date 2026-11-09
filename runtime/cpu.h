// Guest CPU state and helpers used by recompiled code.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((aligned(16))) Cpu {
    uint64_t x[32];          // x0-x30 (x[31] unused; sp is separate)
    uint64_t sp;             // 256
    uint64_t pc;             // 264: target for indirect dispatch
    uint64_t nzcv;           // 272: flags in bits 31..28
    uint64_t tpidr;          // 280
    uint64_t tpidrro;        // 288
    uint64_t excl_addr;      // 296
    uint64_t excl_val;       // 304
    void *thread;            // 312: runtime thread object
    uint64_t fpcr;           // 320: guest FPCR/FPSR (portable FP/SIMD lowering only; the asm path uses the host's)
    uint64_t fpsr;           // 328
    uint8_t _pad[512 - 336];
    __uint128_t v[32];       // 512
} Cpu;

_Static_assert(offsetof(Cpu, sp) == 256, "sp");
_Static_assert(offsetof(Cpu, pc) == 264, "pc");
_Static_assert(offsetof(Cpu, nzcv) == 272, "nzcv");
_Static_assert(offsetof(Cpu, fpcr) == 320, "fpcr");
_Static_assert(offsetof(Cpu, fpsr) == 328, "fpsr");
_Static_assert(offsetof(Cpu, v) == 512, "v");

typedef void (*GuestFn)(Cpu *);

// ---- runtime services (runtime/*.c)
void rt_dispatch(Cpu *c);                 // call function at c->pc
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) __attribute__((noreturn));
void rt_udf(Cpu *c, uint64_t pc) __attribute__((noreturn));
uint64_t rt_ticks(void);

#if defined(__aarch64__)
static inline uint64_t rt_get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline uint64_t rt_get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void rt_set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline void rt_set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }
#else
// Host FP state is not the guest's here: code lifted with --simd=c keeps FPCR/FPSR in Cpu (c->fpcr, zeroed
// with the Cpu), so the host-side reset in thread start-up has nothing to do.
static inline uint64_t rt_get_fpcr(void) { return 0; }
static inline uint64_t rt_get_fpsr(void) { return 0; }
static inline void rt_set_fpcr(uint64_t v) { (void)v; }
static inline void rt_set_fpsr(uint64_t v) { (void)v; }
#endif

// ---- memory: guest addresses are host addresses
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint8_t __attribute__((may_alias)) u8a;

// Function-entry profiler (runtime/fnprof.c, SAVAGEWIND_FNPROF): one predictable branch per call when off.
extern volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
#define FN_ENTER(a) do { if (__builtin_expect(g_fnprof, 0)) fnprof_hit(a, c->x[30], c->x[0]); } while (0)

#define LD8(a)  (*(const u8a *)(uintptr_t)(a))
#define LD16(a) (*(const u16u *)(uintptr_t)(a))
#define LD32(a) (*(const u32u *)(uintptr_t)(a))
#define LD64(a) (*(const u64u *)(uintptr_t)(a))
#define ST8(a, v)  (*(u8a *)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(u16u *)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(u32u *)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(u64u *)(uintptr_t)(a) = (uint64_t)(v))

// ---- exclusive monitor (emulated with compare-and-swap)
#define EXCL(N, T)                                                                         \
    static inline uint64_t ldx##N(Cpu *c, uint64_t a) {                                    \
        T v = __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                        \
        c->excl_addr = a;                                                                  \
        c->excl_val = v;                                                                   \
        return v;                                                                          \
    }                                                                                      \
    static inline uint32_t stx##N(Cpu *c, uint64_t a, uint64_t v) {                        \
        if (c->excl_addr != a) return 1;                                                   \
        c->excl_addr = ~0ull;                                                              \
        T expected = (T)c->excl_val;                                                       \
        return !__atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, (T)v, 0,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);           \
    }                                                                                      \
    static inline uint64_t lda##N(uint64_t a) {                                            \
        return __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                       \
    }                                                                                      \
    static inline void stl##N(uint64_t a, uint64_t v) {                                    \
        __atomic_store_n((T *)(uintptr_t)a, (T)v, __ATOMIC_RELEASE);                       \
    }
EXCL(8, uint8_t)
EXCL(16, uint16_t)
EXCL(32, uint32_t)
EXCL(64, uint64_t)

// ---- flags (packed NZCV in bits 31..28)
#define FLAG_N(c) (((c)->nzcv >> 31) & 1)
#define FLAG_Z(c) (((c)->nzcv >> 30) & 1)
#define FLAG_C(c) (((c)->nzcv >> 29) & 1)
#define FLAG_V(c) (((c)->nzcv >> 28) & 1)
#define COND_EQ(c) (FLAG_Z(c))
#define COND_NE(c) (!FLAG_Z(c))
#define COND_CS(c) (FLAG_C(c))
#define COND_CC(c) (!FLAG_C(c))
#define COND_MI(c) (FLAG_N(c))
#define COND_PL(c) (!FLAG_N(c))
#define COND_VS(c) (FLAG_V(c))
#define COND_VC(c) (!FLAG_V(c))
#define COND_HI(c) (FLAG_C(c) && !FLAG_Z(c))
#define COND_LS(c) (!(FLAG_C(c) && !FLAG_Z(c)))
#define COND_GE(c) (FLAG_N(c) == FLAG_V(c))
#define COND_LT(c) (FLAG_N(c) != FLAG_V(c))
#define COND_GT(c) (!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c))
#define COND_LE(c) (!(!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c)))

#define MKFLAGS(n, z, cy, v) \
    (((uint64_t)(n) << 31) | ((uint64_t)(z) << 30) | ((uint64_t)(cy) << 29) | ((uint64_t)(v) << 28))

static inline uint64_t adds64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint64_t subs64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}
static inline uint32_t subs32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 31);
    return r;
}
static inline uint64_t logic64(Cpu *c, uint64_t r) { c->nzcv = MKFLAGS(r >> 63, r == 0, 0, 0); return r; }
static inline uint32_t logic32(Cpu *c, uint32_t r) { c->nzcv = MKFLAGS(r >> 31, r == 0, 0, 0); return r; }

#define ADC_IMPL(N, S)                                                                     \
    static inline uint##N##_t adc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a + b + (uint##N##_t)FLAG_C(c);                                             \
    }                                                                                      \
    static inline uint##N##_t sbc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a - b - 1 + (uint##N##_t)FLAG_C(c);                                         \
    }                                                                                      \
    static inline uint##N##_t adcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        uint##N##_t cin = FLAG_C(c), r = a + b + cin;                                      \
        int carry = cin ? r <= a : r < a;                                                  \
        c->nzcv = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));    \
        return r;                                                                          \
    }                                                                                      \
    static inline uint##N##_t sbcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        return adcs##N(c, a, (uint##N##_t)~b);                                             \
    }
ADC_IMPL(64, )
ADC_IMPL(32, )

// ---- function-local flags (tools/flags.py, gen.py --locals)
// From TotK Recomp's runtime/cpu.h (https://github.com/valentderah/totk-pc-recomp, commit f469c5a), Copyright (c) 2026
// Valiantsin Dzerakh, MIT License: third_party/totk-pc-recomp/LICENSE.
// Functions generated with --locals keep NZCV in a local Flags F, and write c->nzcv only where they leave by a tail
// exit or before a patch site. A definition of a known kind stores its operands (a - b, a + b, or a logic result in
// a); fc_<kind>(&F, CC_xx) computes a condition from them, so a compare and its branch become one host compare. F.nz
// holds the packed form for definitions only known packed (CCMP, FCMP, MSR, ADCS), for a call's flags and for
// readers that several kinds of definition reach. Every helper is always inlined with a constant condition, so F
// stays in registers and only the condition's own arithmetic remains.
typedef struct { uint64_t a, b, nz; } Flags;
enum { CC_EQ, CC_NE, CC_CS, CC_CC, CC_MI, CC_PL, CC_VS, CC_VC, CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE };
#define CPU_FL static inline __attribute__((always_inline))

CPU_FL int fl_cond(int cc, int n, int z, int cy, int v) {
    switch (cc) {
    case CC_EQ: return z;
    case CC_NE: return !z;
    case CC_CS: return cy;
    case CC_CC: return !cy;
    case CC_MI: return n;
    case CC_PL: return !n;
    case CC_VS: return v;
    case CC_VC: return !v;
    case CC_HI: return cy && !z;
    case CC_LS: return !cy || z;
    case CC_GE: return n == v;
    case CC_LT: return n != v;
    case CC_GT: return !z && n == v;
    default: return z || n != v;
    }
}

// Subtract (SUBS, CMP): conditions as host compares of the operands.
#define FL_SUB(N, U, S)                                                                    \
    CPU_FL U fsub##N(Flags *f, U a, U b) { f->a = a; f->b = b; return (U)(a - b); }            \
    CPU_FL int fc_sub##N(const Flags *f, int cc) {                                             \
        U a = (U)f->a, b = (U)f->b, r = (U)(a - b);                                        \
        switch (cc) {                                                                      \
        case CC_EQ: return a == b;                                                         \
        case CC_NE: return a != b;                                                         \
        case CC_CS: return a >= b;                                                         \
        case CC_CC: return a < b;                                                          \
        case CC_MI: return (S)r < 0;                                                       \
        case CC_PL: return (S)r >= 0;                                                      \
        case CC_VS: return (S)((a ^ b) & (a ^ r)) < 0;                                     \
        case CC_VC: return (S)((a ^ b) & (a ^ r)) >= 0;                                    \
        case CC_HI: return a > b;                                                          \
        case CC_LS: return a <= b;                                                         \
        case CC_GE: return (S)a >= (S)b;                                                   \
        case CC_LT: return (S)a < (S)b;                                                    \
        case CC_GT: return (S)a > (S)b;                                                    \
        default: return (S)a <= (S)b;                                                      \
        }                                                                                  \
    }                                                                                      \
    CPU_FL uint64_t nz_sub##N(U a, U b) {                                                      \
        U r = (U)(a - b);                                                                  \
        return MKFLAGS((S)r < 0, r == 0, a >= b, (S)((a ^ b) & (a ^ r)) < 0);              \
    }                                                                                      \
    CPU_FL uint64_t fnz_sub##N(const Flags *f) { return nz_sub##N((U)f->a, (U)f->b); }
FL_SUB(64, uint64_t, int64_t)
FL_SUB(32, uint32_t, int32_t)

// Add (ADDS, CMN) and logic results (ANDS, TST, BICS): conditions from N, Z, C, V.
#define FL_ADD(N, U, S)                                                                    \
    CPU_FL U fadd##N(Flags *f, U a, U b) { f->a = a; f->b = b; return (U)(a + b); }            \
    CPU_FL uint64_t nz_add##N(U a, U b) {                                                      \
        U r = (U)(a + b);                                                                  \
        return MKFLAGS((S)r < 0, r == 0, r < a, (S)((a ^ r) & (b ^ r)) < 0);               \
    }                                                                                      \
    CPU_FL int fc_add##N(const Flags *f, int cc) {                                             \
        U a = (U)f->a, b = (U)f->b, r = (U)(a + b);                                        \
        return fl_cond(cc, (S)r < 0, r == 0, r < a, (S)((a ^ r) & (b ^ r)) < 0);           \
    }                                                                                      \
    CPU_FL uint64_t fnz_add##N(const Flags *f) { return nz_add##N((U)f->a, (U)f->b); }         \
    CPU_FL U flogic##N(Flags *f, U r) { f->a = r; return r; }                                  \
    CPU_FL int fc_logic##N(const Flags *f, int cc) {                                           \
        U r = (U)f->a;                                                                     \
        return fl_cond(cc, (S)r < 0, r == 0, 0, 0);                                        \
    }                                                                                      \
    CPU_FL uint64_t fnz_logic##N(const Flags *f) { return MKFLAGS((S)f->a < 0, (U)f->a == 0, 0, 0); }
FL_ADD(64, uint64_t, int64_t)
FL_ADD(32, uint32_t, int32_t)

// Packed flags.
CPU_FL int fc_nz(const Flags *f, int cc) {
    return fl_cond(cc, f->nz >> 31 & 1, f->nz >> 30 & 1, f->nz >> 29 & 1, f->nz >> 28 & 1);
}
CPU_FL uint64_t fnz_nz(const Flags *f) { return f->nz; }

// ADC, SBC, ADCS, SBCS with the carry in from F.
#define FL_ADC(N, U)                                                                       \
    CPU_FL U adc##N##f(int cin, U a, U b) { return (U)(a + b + (U)cin); }                      \
    CPU_FL U sbc##N##f(int cin, U a, U b) { return (U)(a - b - 1 + (U)cin); }                  \
    CPU_FL U adcs##N##f(Flags *f, int cin, U a, U b) {                                         \
        U r = (U)(a + b + (U)cin);                                                         \
        int carry = cin ? r <= a : r < a;                                                  \
        f->nz = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));      \
        return r;                                                                          \
    }                                                                                      \
    CPU_FL U sbcs##N##f(Flags *f, int cin, U a, U b) { return adcs##N##f(f, cin, a, (U)~b); }
FL_ADC(64, uint64_t)
FL_ADC(32, uint32_t)
#undef CPU_FL

// ---- bit ops
#define ROR64(v, r) ((uint64_t)(v) >> ((r) & 63) | (uint64_t)(v) << ((64 - ((r) & 63)) & 63))
#define ROR32(v, r) ((uint32_t)(v) >> ((r) & 31) | (uint32_t)(v) << ((32 - ((r) & 31)) & 31))
static inline uint64_t clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }
static inline uint64_t rbit64(uint64_t v) { return __builtin_bitreverse64(v); }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull);
}
static inline uint64_t rev32_64(uint64_t v) {
    return ((uint64_t)__builtin_bswap32((uint32_t)v)) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32);
}
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if ((int64_t)a == INT64_MIN && (int64_t)b == -1) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

// Portable FP/SIMD helpers for code lifted with --simd=c (default off AArch64; gen.py includes simd.h itself
// in that mode, so AArch64 hosts can build it too).
#if !defined(__cplusplus) && !defined(__aarch64__)
#include "simd.h"
#endif

// Scalar single FSUB for a patch (tools/patches.py _surf_boost_remove), whichever way the SIMD was lifted: through
// the FP helpers of simd.h on x86-64, natively on AArch64 (there the host's FSUB is the guest's, and simd.h is only
// included by --simd c, so fp_op_s does not exist in the default asm mode, #413).
static inline void rt_fsub_s(Cpu *c, int d, int n, int m) {
#if !defined(__cplusplus) && !defined(__aarch64__)
    fp_op_s(c, FOP_SUB, d, n, m);
#else
    uint32_t au = (uint32_t)c->v[n], bu = (uint32_t)c->v[m], ru;
    float a, b, r;
    __builtin_memcpy(&a, &au, 4);
    __builtin_memcpy(&b, &bu, 4);
    r = a - b;
    __builtin_memcpy(&ru, &r, 4);
    c->v[d] = ru;
#endif
}

// Scalar single float from a vector register for a patch (tools/patches.py _grass_blade_lateral), whichever
// way the SIMD was lifted.
static inline float rt_vf32(const Cpu *c, int n) {
    uint32_t u = (uint32_t)c->v[n];
    float f;
    __builtin_memcpy(&f, &u, 4);
    return f;
}

// Scalar single FCMP for a patch (tools/patches.py _grass_blade_lateral), returning NZCV flags in bits 31..28,
// whichever way the SIMD was lifted: through the FP helpers of simd.h on x86-64, natively on AArch64 (where
// simd.h is only included by --simd c, so cmp_s does not exist in the default asm mode, #612).
static inline uint64_t rt_fcmp_s(Cpu *c, float a, float b) {
#if !defined(__cplusplus) && !defined(__aarch64__)
    return cmp_s(c, a, b);
#elif defined(__aarch64__)
    uint64_t nzcv;
    __asm__("fcmp %s1, %s2\n\tmrs %0, nzcv" : "=r"(nzcv) : "w"(a), "w"(b));
    return nzcv;
#else
    return a == b ? 0x60000000ull : a < b ? 0x80000000ull : a > b ? 0x20000000ull : 0x30000000ull;
#endif
}
