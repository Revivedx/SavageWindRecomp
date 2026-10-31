// Compare kernels for the optional development memory scanner. No guest pointers are retained.
#include "memscan.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define MS_X86 1
#define AVX2 __attribute__((target("avx2")))
#endif

typedef size_t (*Kernel)(const unsigned char *, size_t, const MsMatch *, uint32_t *, size_t, size_t *);

// memcpy permits raw snapshot storage without aliasing typed objects.
#define SCALAR(NAME, TYPE, LOW, HIGH) \
static size_t scalar_##NAME(const unsigned char *p, size_t n, const MsMatch *m, \
                           uint32_t *out, size_t max, size_t *used) { \
    const TYPE lo = (LOW), hi = (HIGH); \
    size_t count = 0, i = 0; \
    for (; i < n && count < max; i += sizeof(TYPE)) { \
        TYPE v; memcpy(&v, p + i, sizeof v); \
        if (v >= lo && v <= hi) out[count++] = (uint32_t)i; \
    } \
    *used = i; return count; \
}
SCALAR(u8, uint8_t, m->ulo, m->uhi)
SCALAR(u16, uint16_t, m->ulo, m->uhi)
SCALAR(u32, uint32_t, m->ulo, m->uhi)
SCALAR(u64, uint64_t, m->ulo, m->uhi)
SCALAR(s32, int32_t, m->slo, m->shi)
SCALAR(f32, float, m->flo, m->fhi)
SCALAR(f64, double, m->flo, m->fhi)
#undef SCALAR

static bool zero_scalar(const unsigned char *p, size_t n) {
    size_t i = 0;
    for (; n - i >= 8; i += 8) {
        uint64_t v; memcpy(&v, p + i, sizeof v);
        if (v) return false;
    }
    for (; i < n; i++) if (p[i]) return false;
    return true;
}

#ifdef MS_X86
static AVX2 bool zero_avx2(const unsigned char *p, size_t n) {
    size_t i = 0;
    for (; n - i >= 128; i += 128) {
        __m256i a = _mm256_or_si256(_mm256_loadu_si256((const __m256i *)(p+i)),
                                  _mm256_loadu_si256((const __m256i *)(p+i+32)));
        __m256i b = _mm256_or_si256(_mm256_loadu_si256((const __m256i *)(p+i+64)),
                                  _mm256_loadu_si256((const __m256i *)(p+i+96)));
        a = _mm256_or_si256(a, b);
        if (!_mm256_testz_si256(a, a)) return false;
    }
    for (; n - i >= 32; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(p+i));
        if (!_mm256_testz_si256(a, a)) return false;
    }
    return zero_scalar(p+i, n-i);
}

// Integer masks have one bit per byte. Keep the first bit of each matching lane.
// Clearing a complete lane avoids emitting a multi-byte integer more than once.
#define VECTOR(NAME, WIDTH, SETUP, MASK) \
static AVX2 size_t avx2_##NAME(const unsigned char *p, size_t n, const MsMatch *m, \
                             uint32_t *out, size_t max, size_t *used) { \
    SETUP; \
    size_t count = 0, i = 0; \
    for (; n - i >= 32; i += 32) { \
        unsigned bits = (MASK); \
        while (bits) { \
            unsigned bit = (unsigned)__builtin_ctz(bits); \
            size_t off = i + bit * (WIDTH); \
            out[count++] = (uint32_t)off; \
            if (count == max) { *used = off + sizeof_type_##NAME; return count; } \
            bits &= bits - 1; \
        } \
    } \
    size_t tail_used; \
    size_t tail = scalar_##NAME(p+i, n-i, m, out+count, max-count, &tail_used); \
    for (size_t j = 0; j < tail; j++) out[count+j] += (uint32_t)i; \
    *used = i + tail_used; return count + tail; \
}
#define LOAD _mm256_loadu_si256((const __m256i *)(p+i))
#define INT_SETUP(SUFFIX, TYPE, L, H, SIGN) \
    const __m256i sign = _mm256_set1_epi##SUFFIX(SIGN); \
    const __m256i lo = _mm256_xor_si256(_mm256_set1_epi##SUFFIX((TYPE)(L)), sign); \
    const __m256i hi = _mm256_xor_si256(_mm256_set1_epi##SUFFIX((TYPE)(H)), sign)
#define INT_MASK(SUFFIX, KEEP) \
    ((unsigned)~_mm256_movemask_epi8(_mm256_or_si256( \
        _mm256_cmpgt_epi##SUFFIX(lo, _mm256_xor_si256(LOAD, sign)), \
        _mm256_cmpgt_epi##SUFFIX(_mm256_xor_si256(LOAD, sign), hi))) & (KEEP))
enum { sizeof_type_u8=1, sizeof_type_u16=2, sizeof_type_u32=4, sizeof_type_u64=8,
       sizeof_type_s32=4, sizeof_type_f32=4, sizeof_type_f64=8 };
VECTOR(u8, 1, INT_SETUP(8, char, m->ulo, m->uhi, (char)0x80), INT_MASK(8, 0xffffffffu))
VECTOR(u16, 1, INT_SETUP(16, short, m->ulo, m->uhi, (short)0x8000), INT_MASK(16, 0x55555555u))
VECTOR(u32, 1, INT_SETUP(32, int, m->ulo, m->uhi, (int)0x80000000u), INT_MASK(32, 0x11111111u))
VECTOR(u64, 1, INT_SETUP(64x, long long, m->ulo, m->uhi, (long long)0x8000000000000000ull), INT_MASK(64, 0x01010101u))
VECTOR(s32, 1, INT_SETUP(32, int, m->slo, m->shi, 0), INT_MASK(32, 0x11111111u))
VECTOR(f32, 4,
       const __m256 lo = _mm256_set1_ps((float)m->flo); const __m256 hi = _mm256_set1_ps((float)m->fhi),
       (unsigned)_mm256_movemask_ps(_mm256_and_ps(
           _mm256_cmp_ps(_mm256_loadu_ps((const float *)(p+i)), lo, _CMP_GE_OQ),
           _mm256_cmp_ps(_mm256_loadu_ps((const float *)(p+i)), hi, _CMP_LE_OQ))))
VECTOR(f64, 8,
       const __m256d lo = _mm256_set1_pd(m->flo); const __m256d hi = _mm256_set1_pd(m->fhi),
       (unsigned)_mm256_movemask_pd(_mm256_and_pd(
           _mm256_cmp_pd(_mm256_loadu_pd((const double *)(p+i)), lo, _CMP_GE_OQ),
           _mm256_cmp_pd(_mm256_loadu_pd((const double *)(p+i)), hi, _CMP_LE_OQ))))
#undef VECTOR
#undef LOAD
#undef INT_SETUP
#undef INT_MASK

static bool have_avx2(void) {
    // 0 unknown, 1 scalar, 2 AVX2. Concurrent first calls are safe and equivalent.
    static atomic_int choice;
    int c = atomic_load_explicit(&choice, memory_order_relaxed);
    if (!c) {
        c = __builtin_cpu_supports("avx2") ? 2 : 1;
        atomic_store_explicit(&choice, c, memory_order_relaxed);
    }
    return c == 2;
}
#endif

size_t ms_scan(const void *p, size_t n, const MsMatch *match, bool skip_zero,
               uint32_t *out, size_t max, MsStats *st) {
    if (!max || !n) return 0;
    MsMatch m = *match;
    static const Kernel scalar[] = { scalar_u8, scalar_u16, scalar_u32, scalar_u64,
                                     scalar_s32, scalar_f32, scalar_f64 };
    if ((unsigned)m.type > MS_F64) return 0;
    if (m.type <= MS_U64) {
        static const uint64_t limits[] = { UINT8_MAX, UINT16_MAX, UINT32_MAX, UINT64_MAX };
        if (m.ulo > m.uhi || m.ulo > limits[m.type]) return 0;
        if (m.uhi > limits[m.type]) m.uhi = limits[m.type];
    } else if (m.type == MS_S32) {
        if (m.slo > m.shi || m.slo > INT32_MAX || m.shi < INT32_MIN) return 0;
        if (m.slo < INT32_MIN) m.slo = INT32_MIN;
        if (m.shi > INT32_MAX) m.shi = INT32_MAX;
    } else {
        if (!(m.flo <= m.fhi)) return 0;
        if (m.type == MS_F32) {
            float lo = (float)m.flo, hi = (float)m.fhi;
            if ((double)lo > m.flo) lo = nextafterf(lo, -INFINITY);
            if ((double)hi < m.fhi) hi = nextafterf(hi, INFINITY);
            m.flo = lo; m.fhi = hi;
        }
    }
    Kernel kernel = scalar[m.type];
    bool (*zero)(const unsigned char *, size_t) = zero_scalar;
#ifdef MS_X86
    if (have_avx2()) {
        static const Kernel vector[] = { avx2_u8, avx2_u16, avx2_u32, avx2_u64,
                                         avx2_s32, avx2_f32, avx2_f64 };
        kernel = vector[m.type]; zero = zero_avx2;
    }
#endif
    size_t count = 0;
    for (size_t page = 0; page < n && count < max;) {
        size_t bytes = n - page < 4096 ? n - page : 4096;
        const unsigned char *src = (const unsigned char *)p + page;
        if (skip_zero && zero(src, bytes)) {
            if (st) st->zero_bytes += bytes;
        } else {
            size_t used;
            size_t got = kernel(src, bytes, &m, out+count, max-count, &used);
            for (size_t j = 0; j < got; j++) out[count+j] += (uint32_t)page;
            count += got;
            if (st) st->compared += used;
        }
        page += bytes;
    }
    return count;
}
