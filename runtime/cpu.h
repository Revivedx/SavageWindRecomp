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
// The *_F forms and the *_f helpers work on any flags value: recompiled functions keep the flags in a local when
// nothing can see them there (gen.py local_flags), and the compiler then folds a compare and its branch together.
#define FLAGS_N(f) (((f) >> 31) & 1)
#define FLAGS_Z(f) (((f) >> 30) & 1)
#define FLAGS_C(f) (((f) >> 29) & 1)
#define FLAGS_V(f) (((f) >> 28) & 1)
#define COND_EQ_F(f) (FLAGS_Z(f))
#define COND_NE_F(f) (!FLAGS_Z(f))
#define COND_CS_F(f) (FLAGS_C(f))
#define COND_CC_F(f) (!FLAGS_C(f))
#define COND_MI_F(f) (FLAGS_N(f))
#define COND_PL_F(f) (!FLAGS_N(f))
#define COND_VS_F(f) (FLAGS_V(f))
#define COND_VC_F(f) (!FLAGS_V(f))
#define COND_HI_F(f) (FLAGS_C(f) && !FLAGS_Z(f))
#define COND_LS_F(f) (!(FLAGS_C(f) && !FLAGS_Z(f)))
#define COND_GE_F(f) (FLAGS_N(f) == FLAGS_V(f))
#define COND_LT_F(f) (FLAGS_N(f) != FLAGS_V(f))
#define COND_GT_F(f) (!FLAGS_Z(f) && FLAGS_N(f) == FLAGS_V(f))
#define COND_LE_F(f) (!(!FLAGS_Z(f) && FLAGS_N(f) == FLAGS_V(f)))

#define FLAG_N(c) FLAGS_N((c)->nzcv)
#define FLAG_Z(c) FLAGS_Z((c)->nzcv)
#define FLAG_C(c) FLAGS_C((c)->nzcv)
#define FLAG_V(c) FLAGS_V((c)->nzcv)
#define COND_EQ(c) COND_EQ_F((c)->nzcv)
#define COND_NE(c) COND_NE_F((c)->nzcv)
#define COND_CS(c) COND_CS_F((c)->nzcv)
#define COND_CC(c) COND_CC_F((c)->nzcv)
#define COND_MI(c) COND_MI_F((c)->nzcv)
#define COND_PL(c) COND_PL_F((c)->nzcv)
#define COND_VS(c) COND_VS_F((c)->nzcv)
#define COND_VC(c) COND_VC_F((c)->nzcv)
#define COND_HI(c) COND_HI_F((c)->nzcv)
#define COND_LS(c) COND_LS_F((c)->nzcv)
#define COND_GE(c) COND_GE_F((c)->nzcv)
#define COND_LT(c) COND_LT_F((c)->nzcv)
#define COND_GT(c) COND_GT_F((c)->nzcv)
#define COND_LE(c) COND_LE_F((c)->nzcv)

#define MKFLAGS(n, z, cy, v) \
    (((uint64_t)(n) << 31) | ((uint64_t)(z) << 30) | ((uint64_t)(cy) << 29) | ((uint64_t)(v) << 28))

static inline uint64_t adds64_f(uint64_t *f, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    *f = MKFLAGS(r >> 63, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint64_t subs64_f(uint64_t *f, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    *f = MKFLAGS(r >> 63, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32_f(uint64_t *f, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    *f = MKFLAGS(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}
static inline uint32_t subs32_f(uint64_t *f, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    *f = MKFLAGS(r >> 31, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 31);
    return r;
}
static inline uint64_t logic64_f(uint64_t *f, uint64_t r) { *f = MKFLAGS(r >> 63, r == 0, 0, 0); return r; }
static inline uint32_t logic32_f(uint64_t *f, uint32_t r) { *f = MKFLAGS(r >> 31, r == 0, 0, 0); return r; }
static inline uint64_t adds64(Cpu *c, uint64_t a, uint64_t b) { return adds64_f(&c->nzcv, a, b); }
static inline uint64_t subs64(Cpu *c, uint64_t a, uint64_t b) { return subs64_f(&c->nzcv, a, b); }
static inline uint32_t adds32(Cpu *c, uint32_t a, uint32_t b) { return adds32_f(&c->nzcv, a, b); }
static inline uint32_t subs32(Cpu *c, uint32_t a, uint32_t b) { return subs32_f(&c->nzcv, a, b); }
static inline uint64_t logic64(Cpu *c, uint64_t r) { return logic64_f(&c->nzcv, r); }
static inline uint32_t logic32(Cpu *c, uint32_t r) { return logic32_f(&c->nzcv, r); }

#define ADC_IMPL(N, S)                                                                     \
    static inline uint##N##_t adc##N##_f(uint64_t *f, uint##N##_t a, uint##N##_t b) {      \
        return a + b + (uint##N##_t)FLAGS_C(*f);                                           \
    }                                                                                      \
    static inline uint##N##_t sbc##N##_f(uint64_t *f, uint##N##_t a, uint##N##_t b) {      \
        return a - b - 1 + (uint##N##_t)FLAGS_C(*f);                                       \
    }                                                                                      \
    static inline uint##N##_t adcs##N##_f(uint64_t *f, uint##N##_t a, uint##N##_t b) {     \
        uint##N##_t cin = FLAGS_C(*f), r = a + b + cin;                                    \
        int carry = cin ? r <= a : r < a;                                                  \
        *f = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));         \
        return r;                                                                          \
    }                                                                                      \
    static inline uint##N##_t sbcs##N##_f(uint64_t *f, uint##N##_t a, uint##N##_t b) {     \
        return adcs##N##_f(f, a, (uint##N##_t)~b);                                         \
    }                                                                                      \
    static inline uint##N##_t adc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return adc##N##_f(&c->nzcv, a, b);                                                 \
    }                                                                                      \
    static inline uint##N##_t sbc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return sbc##N##_f(&c->nzcv, a, b);                                                 \
    }                                                                                      \
    static inline uint##N##_t adcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        return adcs##N##_f(&c->nzcv, a, b);                                                \
    }                                                                                      \
    static inline uint##N##_t sbcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        return sbcs##N##_f(&c->nzcv, a, b);                                                \
    }
ADC_IMPL(64, )
ADC_IMPL(32, )

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
