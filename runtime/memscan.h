// (test and development only) The fast parts of the control endpoint's memory scan (runtime/control_mem.c):
// the compare kernels (runtime/memscan.c) and the OS helpers (runtime/memscan_os.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- compare kernels (runtime/memscan.c)
enum { MS_U8, MS_U16, MS_U32, MS_U64, MS_S32, MS_F32, MS_F64 };  // same order as control_mem.c's types

// What a value must be to match: lo <= v <= hi, in the type's own domain (integers exact when lo == hi; floats with
// a tolerance give lo = target - tol, hi = target + tol). NaN never matches.
typedef struct {
    int type;               // MS_*
    uint64_t ulo, uhi;      // MS_U8..MS_U64
    int64_t slo, shi;       // MS_S32
    double flo, fhi;        // MS_F32 / MS_F64
} MsMatch;

typedef struct {
    uint64_t zero_bytes;    // bytes skipped as all-zero 4 KB pages
    uint64_t compared;      // bytes compared
} MsStats;

// Compare the naturally aligned values in [p, p + n) (p aligned to the type's width; n a multiple of it). Writes the
// byte offsets (from p) of matches to out, at most max of them, and returns how many it wrote. With skip_zero, a
// 4 KB page (relative to p, which callers align to 4 KB) that is all zero is skipped without comparing (callers pass
// skip_zero only when 0 cannot match). Uses AVX2 where the CPU has it, else plain C; both give identical results.
size_t ms_scan(const void *p, size_t n, const MsMatch *m, bool skip_zero, uint32_t *out, size_t max, MsStats *st);

// ---- OS helpers (runtime/memscan_os.c)
// Which of the npages 4 KB pages from addr (4 KB aligned) are resident in RAM: resident[i] = 1 or 0. Returns false
// when the OS can't say (then the caller treats every page as resident). On Windows a page trimmed out of the working
// set is not resident but still holds data, so skipping non-resident pages can miss values.
bool ms_resident(uint64_t addr, size_t npages, uint8_t *resident);

// Run fn(p, n, ctx) over memory that may become inaccessible while it runs, without crashing: on Windows under SEH
// (__try), elsewhere by copying through the OS first (fn then sees a copy). Returns false if the access faulted;
// *ret gets fn's result otherwise.
typedef size_t (*MsFn)(const void *p, size_t n, void *ctx);
bool ms_guarded(const void *p, size_t n, MsFn fn, void *ctx, size_t *ret);
