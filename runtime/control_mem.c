// Development-only guest memory inspection. No host pointers outside the guest window.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "control.h"
#include "rt.h"
#include "memscan.h"
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#define LIMIT 100000
#define REGIONS 2048
#define SLICE_NS 1000000ull
static const char *types[] = {"u8", "u16", "u32", "u64", "s32", "f32", "f64", "hex"};
static const unsigned widths[] = {1, 2, 4, 8, 4, 4, 8, 1};
typedef union { uint64_t u; int32_t s; float f; double d; unsigned char b[8]; } Value;
typedef struct { uint64_t addr; Value old; } Candidate;
static Candidate candidates[LIMIT];
static size_t count, input_count, cursor;
static int type, mode, region_count;
static uint64_t starts[REGIONS], sizes[REGIONS];
static Value target;
static double tolerance;
static bool active, pending, capped;
// Only the worker writes candidates during an initial scan. Release/acquire on
// running publishes the final list. The presenter never waits for the worker.
static atomic_bool running, cancel_scan;
static atomic_uint_fast64_t scanned_bytes, found_count;
static uint64_t total_bytes;
static atomic_uint_fast64_t skipped_bytes;
// The initial scan's timers and counters (mem scan status), so a slow scan shows where its time goes.
static atomic_uint_fast64_t st_resident_ns, st_scan_ns, st_zero, st_compared, st_nonresident, st_faults, st_runs;
static uint64_t st_start_ns, st_end_ns, st_regions_ns;
static bool use_resident, regions_truncated;
// mode: 0 initial scan, 1 changed, 2 unchanged, 3 increased, 4 decreased, 5 equal, 6 near.

static bool integer(const char *s, uint64_t *v) {
    if (!*s || *s == '-' || *s == '+' || *s == ' ' || *s == '\t') return false;
    char *end; errno = 0;
    *v = strtoull(s, &end, s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10);
    return !errno && end != s && !*end;
}
static int get_type(const char *s) {
    for (int i = 0; i < 8; i++) if (!strcmp(s, types[i])) return i;
    return -1;
}
static bool number(const char *s, double *v) {
    char *end; errno = 0; *v = strtod(s, &end);
    return *s && end != s && !*end && !errno && isfinite(*v);
}
static bool parse(const char *s, int t, Value *v) {
    memset(v, 0, sizeof *v);
    if (t < 4) return integer(s, &v->u) && (t == 3 || v->u < (1ull << (8 * widths[t])));
    if (t == 4) {
        char *end; errno = 0; long long x = strtoll(s, &end, 0);
        if (!*s || end == s || *end || errno || x < INT32_MIN || x > INT32_MAX) return false;
        v->s = (int32_t)x; return true;
    }
    double x;
    if (!number(s, &x) || (t == 5 && fabs(x) > FLT_MAX)) return false;
    if (t == 5) v->f = (float)x; else v->d = x;
    return true;
}
static double floating(Value v) { return type == 5 ? v.f : v.d; }
static bool equal(Value a, Value b, double tol) {
    if (type >= 5) return fabs(floating(a) - floating(b)) <= tol;
    if (type == 4) return fabs((double)a.s - (double)b.s) <= tol;
    uint64_t delta = a.u > b.u ? a.u - b.u : b.u - a.u;
    return tol == 0 ? delta == 0 : (long double)delta <= (long double)tol;
}
static bool equal_as(int t, Value a, Value b, double tol) {
    int saved = type; type = t;
    bool e = equal(a, b, tol);
    type = saved;
    return e;
}
static int compare(Value a, Value b) {
    if (type >= 5) return (floating(a) > floating(b)) - (floating(a) < floating(b));
    if (type == 4) return (a.s > b.s) - (a.s < b.s);
    return (a.u > b.u) - (a.u < b.u);
}
static bool window(uint64_t a, size_t n) {
    return a >= GUEST_LO && a < GUEST_HI && n <= GUEST_HI - a;
}
// OS copies report inaccessible pages rather than raising a process exception. This also
// covers a mapping changing after validation, without SEH or a process-wide signal handler.
static bool copy_memory(uint64_t a, void *buf, size_t n, bool write) {
    if (!window(a, n)) return false;
#ifdef _WIN32
    // WriteProcessMemory can bypass read-only protection for debuggers. Require
    // ordinary writable pages explicitly before invoking it.
    uint64_t end = a + n, page = a;
    while (page < end) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)(uintptr_t)page, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & PAGE_GUARD)) return false;
        DWORD p = m.Protect & 0xff;
        bool writable = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        bool readable = writable || p == PAGE_READONLY || p == PAGE_EXECUTE_READ;
        if (!readable || (write && !writable)) return false;
        uint64_t next = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= page) return false;
        page = next;
    }
    SIZE_T done = 0;
    return (write ? WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, buf, n, &done)
                  : ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, buf, n, &done)) && done == n;
#elif defined(__APPLE__)
    if (write) return mach_vm_write(mach_task_self(), a, (vm_offset_t)buf, (mach_msg_type_number_t)n) == KERN_SUCCESS;
    mach_vm_size_t done = 0;
    return mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)buf, &done) == KERN_SUCCESS && done == n;
#else
    struct iovec local = {buf, n}, remote = {(void *)(uintptr_t)a, n};
    ssize_t done = write ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0)
                         : process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return done >= 0 && (size_t)done == n;
#endif
}
static void address(ControlReply *r, const char *key, uint64_t a) {
    char s[32]; snprintf(s, sizeof s, "0x%" PRIx64, a); control_field_str(r, key, s);
}
static void value_field(ControlReply *r, const char *key, Value v, int t) {
    char s[64];
    if (t < 4) snprintf(s, sizeof s, "%" PRIu64, v.u);
    else if (t == 4) snprintf(s, sizeof s, "%" PRId32, v.s);
    else if (t == 5) snprintf(s, sizeof s, "%.9g", (double)v.f);
    else snprintf(s, sizeof s, "%.17g", v.d);
    control_field_str(r, key, s);
}
static void list(ControlReply *r, size_t n) {
    control_field_str(r, "type", types[type]);
    control_field_int(r, "count", (int64_t)count);
    control_field_bool(r, "partial", pending);
    if (pending && mode) { control_field_int(r, "checked", (int64_t)cursor); control_field_int(r, "of", (int64_t)input_count); }
    control_field_bool(r, "capped", capped);
    if (pending) control_field_str(r, "continue", mode ? "mem rescan continue" : "mem scan continue");
    if (n > count) n = count;
    control_field_int(r, "shown", (int64_t)n);
    for (size_t i = 0; i < n; i++) {
        char key[48]; snprintf(key, sizeof key, "candidate.%zu.addr", i); address(r, key, candidates[i].addr);
        snprintf(key, sizeof key, "candidate.%zu.value", i); value_field(r, key, candidates[i].old, type);
    }
}
// Rescan reads: in place under the crash guard (copy_memory's VirtualQuery + ReadProcessMemory cost about a millisecond
// each on the PC, so one rescan step per frame). A candidate came from a committed page; if it has gone away, it drops.
typedef struct { void *buf; size_t n; } PeekCtx;
static size_t peek_fn(const void *p, size_t n, void *ctx) { PeekCtx *c = ctx; memcpy(c->buf, p, c->n); return n; }
static bool peek(uint64_t a, void *buf, size_t n) {
    if (!window(a, n)) return false;
    PeekCtx c = {buf, n}; size_t ret;
    return ms_guarded((const void *)(uintptr_t)a, n, peek_fn, &c, &ret);
}
static void slice(ControlReply *r) {
    uint64_t deadline = plat_now_ns() + SLICE_NS;
    unsigned steps = 0;
    while (pending && steps++ < 8192 && plat_now_ns() < deadline) {
        if (cursor == input_count) { pending = false; break; }
        Candidate c = candidates[cursor++]; Value v = {0};
        if (!peek(c.addr, v.b, widths[type])) continue;
        bool keep = mode == 1 ? !equal(v, c.old, tolerance) : mode == 2 ? equal(v, c.old, tolerance) :
                    mode == 3 ? compare(v, c.old) > 0 : mode == 4 ? compare(v, c.old) < 0 : equal(v, target, tolerance);
        if (keep) candidates[count++] = (Candidate){c.addr, v};
    }
    list(r, 20);
}

// The initial scan (a worker thread): the regions mapped once when the scan starts, read in place (with resident=1 only
// in runs of pages resident in RAM, via ms_resident; off by default because a page the OS trimmed out of RAM still
// holds data and would be missed), each run compared by ms_scan under ms_guarded, so a page
// that goes away mid-scan (a reboot decommitting the guest) costs that run, not the game.
#define CHUNK (1024 * 1024)  // (also ms_guarded's limit where it copies: Linux, macOS)
static MsMatch match;
static bool skip_zero;
static uint32_t offsets[LIMIT];
typedef struct { uint64_t guest; } RunCtx;

static size_t run_cb(const void *p, size_t n, void *vctx) {
    const RunCtx *c = vctx;
    MsStats st = {0, 0};
    size_t k = ms_scan(p, n, &match, skip_zero, offsets, LIMIT - count, &st);
    unsigned w = widths[type];
    for (size_t i = 0; i < k; i++) {
        Value v = {0};
        memcpy(v.b, (const unsigned char *)p + offsets[i], w);
        candidates[count++] = (Candidate){c->guest + offsets[i], v};
    }
    atomic_fetch_add_explicit(&st_zero, st.zero_bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_compared, st.compared, memory_order_relaxed);
    return k;
}

// [a, a + n): one run of resident pages. On a fault, page by page, so readable neighbours still count.
static void scan_run(uint64_t a, size_t n) {
    atomic_fetch_add_explicit(&st_runs, 1, memory_order_relaxed);
    size_t before = count, ret;
    RunCtx c = {a};
    if (ms_guarded((const void *)(uintptr_t)a, n, run_cb, &c, &ret)) return;
    count = before;  // (the matches before the fault come again below)
    atomic_fetch_add_explicit(&st_faults, 1, memory_order_relaxed);
    for (uint64_t q = a; q < a + n && count < LIMIT;) {
        size_t part = (size_t)((((q & ~4095ull) + 4096) < a + n ? ((q & ~4095ull) + 4096) : a + n) - q);
        size_t mark = count;
        RunCtx pc = {q};
        if (!ms_guarded((const void *)(uintptr_t)q, part, run_cb, &pc, &ret)) {
            count = mark;
            atomic_fetch_add(&skipped_bytes, part);
        }
        q += part;
    }
}

static void *scan_worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    static uint8_t resident[CHUNK / 4096 + 2];
    unsigned w = widths[type];
    for (int i = 0; i < region_count && count < LIMIT; i++) {
        for (uint64_t off = 0; off < sizes[i] && count < LIMIT;) {
            if (atomic_load_explicit(&cancel_scan, memory_order_relaxed)) goto finished;
            uint64_t a = starts[i] + off;
            // chunks end on a page boundary (and a whole value) so the pages line up with ms_resident's
            uint64_t lim = ((a & ~4095ull) + CHUNK);
            size_t n = (size_t)((starts[i] + sizes[i] < lim ? starts[i] + sizes[i] : lim) - a);
            n -= n % w;
            if (!n) n = w;
            uint64_t page0 = a & ~4095ull;
            size_t npages = (size_t)((((a + n + 4095) & ~4095ull) - page0) / 4096);
            uint64_t t0 = plat_now_ns();
            bool known = use_resident && ms_resident(page0, npages, resident);
            uint64_t t1 = plat_now_ns();
            atomic_fetch_add_explicit(&st_resident_ns, t1 - t0, memory_order_relaxed);
            if (!known) scan_run(a, n);
            else {
                // runs of resident pages, clipped to [a, a + n) and to whole values
                for (size_t pg = 0; pg < npages && count < LIMIT;) {
                    size_t e = pg;
                    bool res = resident[pg];
                    while (e < npages && resident[e] == res) e++;
                    uint64_t lo = page0 + pg * 4096, hi = page0 + e * 4096;
                    if (lo < a) lo = a;
                    if (hi > a + n) hi = a + n;
                    lo = (lo + w - 1) / w * w;
                    if (hi > lo) {
                        size_t len = (size_t)(hi - lo) / w * w;
                        if (!res) atomic_fetch_add_explicit(&st_nonresident, hi - lo, memory_order_relaxed);
                        else if (len) scan_run(lo, len);
                    }
                    pg = e;
                }
            }
            atomic_fetch_add_explicit(&st_scan_ns, plat_now_ns() - t1, memory_order_relaxed);
            off += n;
            atomic_fetch_add_explicit(&scanned_bytes, n, memory_order_relaxed);
            atomic_store_explicit(&found_count, count, memory_order_relaxed);
        }
    }
finished:
    st_end_ns = plat_now_ns();
    capped = count == LIMIT;
    pending = false;
    atomic_store_explicit(&running, false, memory_order_release);
    return NULL;
}
static void scan_status(ControlReply *r) {
    bool busy = atomic_load_explicit(&running, memory_order_acquire);
    uint64_t done = atomic_load(&scanned_bytes);
    control_field_num(r, "scanned_mb", (double)done / 1048576);
    control_field_num(r, "total_mb", (double)total_bytes / 1048576);
    control_field_num(r, "percent", total_bytes ? 100.0 * (double)done / (double)total_bytes : 100);
    control_field_int(r, "candidates", (int64_t)atomic_load(&found_count));
    control_field_bool(r, "done", !busy);
    control_field_int(r, "skipped_bytes", (int64_t)atomic_load(&skipped_bytes));
    // where the time goes (all ms), and how much memory each step left out
    double el = (double)((busy ? plat_now_ns() : st_end_ns) - st_start_ns) / 1e9;
    control_field_num(r, "elapsed_s", el);
    control_field_num(r, "mb_per_s", el > 0 ? (double)done / 1048576 / el : 0);
    control_field_num(r, "regions_ms", (double)st_regions_ns / 1e6);
    control_field_num(r, "resident_ms", (double)atomic_load(&st_resident_ns) / 1e6);
    control_field_num(r, "scan_ms", (double)atomic_load(&st_scan_ns) / 1e6);
    control_field_num(r, "nonresident_mb", (double)atomic_load(&st_nonresident) / 1048576);
    control_field_num(r, "zero_mb", (double)atomic_load(&st_zero) / 1048576);
    control_field_num(r, "compared_mb", (double)atomic_load(&st_compared) / 1048576);
    control_field_int(r, "runs", (int64_t)atomic_load(&st_runs));
    control_field_int(r, "faults", (int64_t)atomic_load(&st_faults));
    control_field_bool(r, "resident_only", use_resident);
    if (regions_truncated) control_field_bool(r, "regions_truncated", true);
    if (!busy) {
        control_field_bool(r, "cancelled", atomic_load(&cancel_scan));
        list(r, 20);
    }
}
static void region_field(ControlReply *r, unsigned i, uint64_t a, uint64_t n) {
    char key[48]; snprintf(key, sizeof key, "region.%u.start", i); address(r, key, a);
    snprintf(key, sizeof key, "region.%u.size", i); control_field_int(r, key, (int64_t)n);
    snprintf(key, sizeof key, "region.%u.scale", i); control_field_str(r, key, n >= 64 * 1048576ull ? "big" : "small");
    snprintf(key, sizeof key, "region.%u.likely_heap", i); control_field_bool(r, key, a < HEAP_BASE + HEAP_MAX && a + n > HEAP_BASE);
}
static int collect_regions(uint64_t *addr, uint64_t *size, bool *writable) {
    unsigned count_regions = 0;
#ifdef _WIN32
    uint64_t a = GUEST_LO;
    MEMORY_BASIC_INFORMATION m;
    while (a < GUEST_HI && VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) {
        uint64_t end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        DWORD p = m.Protect & 0xff;
        if (m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD) &&
            (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
             p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY))
            {
            if (count_regions == REGIONS) break;
            writable[count_regions] = p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                                      p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
            addr[count_regions] = a; size[count_regions++] = (end < GUEST_HI ? end : GUEST_HI) - a;
        }
        if (end <= a) break;
        a = end;
    }
#elif defined(__APPLE__)
    mach_vm_address_t a = GUEST_LO; mach_vm_size_t n;
    while (a < GUEST_HI) {
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &n, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &object) != KERN_SUCCESS) break;
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (a >= GUEST_HI) break;
        uint64_t lo = a < GUEST_LO ? GUEST_LO : a, end = a + n < GUEST_HI ? a + n : GUEST_HI;
        if (info.protection & VM_PROT_READ) {
            if (count_regions == REGIONS) break;
            writable[count_regions] = (info.protection & VM_PROT_WRITE) != 0;
            addr[count_regions] = lo; size[count_regions++] = end - lo;
        }
        a += n;
    }
#else
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[1024], perms[5]; uint64_t a, end;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &a, &end, perms) != 3 || perms[0] != 'r' || end <= GUEST_LO || a >= GUEST_HI) continue;
        if (a < GUEST_LO) a = GUEST_LO;
        if (end > GUEST_HI) end = GUEST_HI;
        if (count_regions == REGIONS) break;
        writable[count_regions] = perms[1] == 'w';
        addr[count_regions] = a; size[count_regions++] = end - a;
    }
    fclose(f);
#endif
    return (int)count_regions;
}
static void regions(ControlReply *r) {
    uint64_t addr[REGIONS], size[REGIONS];
    bool writable[REGIONS];
    int n = collect_regions(addr, size, writable);
    if (n < 0) { control_fail(r, "Cannot read memory mappings"); return; }
    for (int i = 0; i < n; i++) region_field(r, (unsigned)i, addr[i], size[i]);
    control_field_int(r, "regions", n);
}
static void path_cmd(int argc, char **argv, ControlReply *r);
static void cmd_mem(int argc, char **argv, ControlReply *r) {
    if (argc < 2) goto usage;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "path")) { path_cmd(argc, argv, r); return; }
    if (!strcmp(cmd, "regions") && argc == 2) { regions(r); return; }
    if (!strcmp(cmd, "scan") && argc == 3 && (!strcmp(argv[2], "status") || !strcmp(argv[2], "continue"))) {
        if (!active) goto no_scan;
        scan_status(r); return;
    }
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        if (!strcmp(cmd, "reset") && argc == 2) {
            atomic_store(&cancel_scan, true); control_field_bool(r, "cancelling", true); return;
        }
        control_fail(r, "Scan in progress"); return;
    }
    if (!strcmp(cmd, "reset") && argc == 2) {
        active = pending = capped = false; count = 0; control_field_int(r, "count", 0); return;
    }
    if (!strcmp(cmd, "read") || !strcmp(cmd, "write")) {
        bool write = !strcmp(cmd, "write"); uint64_t a, len; int t; Value v;
        if (argc < 4 || argc > 5 || !integer(argv[2], &a)) goto usage;
        if (write) {
            if (argc != 5 || (t = get_type(argv[3])) < 0 || t == 7 || !parse(argv[4], t, &v)) goto usage;
            len = widths[t];
        } else {
            t = argc == 5 ? get_type(argv[4]) : 7;
            if (t < 0 || !integer(argv[3], &len) || !len || len > 4096 || len % widths[t]) goto usage;
        }
        unsigned char buf[4096];
        if (write) memcpy(buf, v.b, (size_t)len);
        if (!copy_memory(a, buf, (size_t)len, write)) {
            control_fail(r, "Address range not accessible"); return;
        }
        address(r, "addr", a); control_field_int(r, "bytes", (int64_t)len);
        if (write) { value_field(r, "value", v, t); return; }
        if (t == 7) {
            char hex[8193]; for (size_t i = 0; i < len; i++) snprintf(hex + 2 * i, 3, "%02x", buf[i]);
            control_field_str(r, "hex", hex);
        } else for (size_t i = 0; i < len / widths[t]; i++) {
            char key[32]; v.u = 0; memcpy(v.b, buf + i * widths[t], widths[t]);
            snprintf(key, sizeof key, "value.%zu", i); value_field(r, key, v, t);
        }
        return;
    }
    if (!strcmp(cmd, "list")) {
        uint64_t n = 20; if (argc > 3 || (argc == 3 && (!integer(argv[2], &n) || n > 1000))) goto usage;
        if (!active) goto no_scan;
        list(r, (size_t)n); return;
    }
    if (!strcmp(cmd, "scan") || !strcmp(cmd, "rescan")) {
        bool rescan = !strcmp(cmd, "rescan");
        if (argc == 3 && !strcmp(argv[2], "continue")) {
            if (!pending || rescan != (mode != 0)) { control_fail(r, "No partial scan"); return; }
            slice(r); return;
        }
        if (rescan) {
            if (!active) goto no_scan;
            if (pending) { control_fail(r, "Partial scan pending"); return; }
            int next_mode = 0; Value next_target = {0}; double tol = tolerance;
            if (argc == 3) {
                const char *names[] = {"changed", "unchanged", "increased", "decreased"};
                for (int i = 0; i < 4; i++) if (!strcmp(argv[2], names[i])) next_mode = i + 1;
            } else if (argc == 4 && !strcmp(argv[2], "equal") && parse(argv[3], type, &next_target)) { next_mode = 5; tol = 0; }
            else if (argc == 5 && !strcmp(argv[2], "near") && parse(argv[3], type, &next_target) && number(argv[4], &tol) && tol >= 0) next_mode = 6;
            if (!next_mode) goto usage;
            mode = next_mode; target = next_target; tolerance = tol; input_count = count; cursor = count = 0;
        } else {
            int t; Value v; double tol = 0;
            if (argc < 4 || (t = get_type(argv[2])) < 0 || t == 7 || !parse(argv[3], t, &v)) goto usage;
            bool selected[REGIONS] = {false}, filter = false, have_from = false, have_to = false, want_resident = false;
            uint64_t from = GUEST_LO, to = GUEST_HI;
            int arg = 4;
            if (arg < argc && !strchr(argv[arg], '=')) {
                if (t < 5 || !number(argv[arg++], &tol) || tol < 0) goto usage;
            }
            for (; arg < argc; arg++) {
                const char *opt = argv[arg];
                if (!strncmp(opt, "region=", 7)) {
                    if (filter) goto usage;
                    filter = true; opt += 7;
                    do {
                        char *end; errno = 0;
                        if (*opt < '0' || *opt > '9') goto usage;
                        unsigned long idx = strtoul(opt, &end, 10);
                        if (errno || idx >= REGIONS || (*end && *end != ',')) goto usage;
                        selected[idx] = true;
                        if (!*end) break;
                        opt = end + 1;
                    } while (true);
                } else if (!strcmp(opt, "resident=0") || !strcmp(opt, "resident=1")) {
                    want_resident = opt[9] == '1';
                } else if (!strncmp(opt, "from=", 5)) {
                    if (have_from || !integer(opt + 5, &from)) goto usage;
                    have_from = true;
                } else if (!strncmp(opt, "to=", 3)) {
                    if (have_to || !integer(opt + 3, &to)) goto usage;
                    have_to = true;
                } else goto usage;
            }
            if (have_from != have_to || from < GUEST_LO || to > GUEST_HI || from >= to) goto usage;
            uint64_t addr[REGIONS], size[REGIONS];
            bool writable[REGIONS];
            uint64_t tr = plat_now_ns();
            int n = collect_regions(addr, size, writable);
            st_regions_ns = plat_now_ns() - tr;
            if (n < 0) { control_fail(r, "Cannot read memory mappings"); return; }
            regions_truncated = n == REGIONS;
            for (int i = n; i < REGIONS; i++) if (selected[i]) {
                control_fail(r, "Unknown region"); return;
            }
            region_count = 0; total_bytes = 0;
            for (int i = 0; i < n; i++) {
                if (filter ? !selected[i] : !writable[i]) continue;
                uint64_t lo = addr[i] > from ? addr[i] : from;
                uint64_t hi = addr[i] + size[i] < to ? addr[i] + size[i] : to;
                lo = (lo + widths[t] - 1) & ~(uint64_t)(widths[t] - 1);
                if (lo >= hi || hi - lo < widths[t]) continue;
                starts[region_count] = lo; sizes[region_count] = (hi - lo) / widths[t] * widths[t];
                total_bytes += sizes[region_count++];
            }
            type = t; target = v; tolerance = tol; count = 0; capped = false; mode = 0;
            atomic_store(&scanned_bytes, 0); atomic_store(&found_count, 0); atomic_store(&skipped_bytes, 0);
            match = (MsMatch){.type = t};
            if (t == 5 || t == 6) { double x = t == 5 ? (double)v.f : v.d; match.flo = x - tol; match.fhi = x + tol; }
            else if (t == 4) match.slo = match.shi = v.s;
            else match.ulo = match.uhi = v.u;
            Value zero = {0};
            skip_zero = !equal_as(t, zero, v, tol);
            use_resident = want_resident;
            atomic_store(&st_resident_ns, 0); atomic_store(&st_scan_ns, 0); atomic_store(&st_zero, 0);
            atomic_store(&st_compared, 0); atomic_store(&st_nonresident, 0); atomic_store(&st_faults, 0);
            atomic_store(&st_runs, 0);
            st_start_ns = plat_now_ns();
            atomic_store(&cancel_scan, false);
            active = pending = true;
            atomic_store(&running, true);
            pthread_t thread;
            if (pthread_create(&thread, NULL, scan_worker, NULL)) {
                atomic_store(&running, false); active = pending = false;
                control_fail(r, "Cannot start scan"); return;
            }
            pthread_detach(thread);
            control_field_bool(r, "started", true); return;
        }
        active = pending = true; slice(r); return;
    }
usage:
    control_fail(r, "mem: use regions | read <addr> <bytes 1-4096> [u8|u16|u32|u64|s32|f32|f64|hex] | write <addr> <type> <value> | scan <type> <value> [float tolerance] [region=<n>[,<n>...]] [from=<addr> to=<addr>] [resident=1] | scan status | scan continue | rescan <changed|unchanged|increased|decreased|equal value|near value tolerance|continue> | list [0-1000] | reset");
    return;
no_scan:
    control_fail(r, "No scan");
}
// ---- for other control commands (runtime/control_mem.h)
bool control_mem_peek(uint64_t a, void *buf, size_t n) { return peek(a, buf, n); }
// Windows: in place under the crash guard, like peek (a write to a page that is not writable faults inside the guard
// and fails; copy_memory's VirtualQuery + WriteProcessMemory cost about a millisecond each). Elsewhere copy_memory.
static size_t poke_fn(const void *p, size_t n, void *ctx) { PeekCtx *c = ctx; memcpy((void *)p, c->buf, c->n); return n; }
bool control_mem_poke(uint64_t a, const void *buf, size_t n) {
    unsigned char tmp[64];
    if (n > sizeof tmp || !window(a, n)) return false;
    memcpy(tmp, buf, n);
#ifdef _WIN32
    PeekCtx c = {tmp, n}; size_t ret;
    return ms_guarded((const void *)(uintptr_t)a, n, poke_fn, &c, &ret);
#else
    (void)poke_fn;
    return copy_memory(a, tmp, n, true);  // (ms_guarded hands out a copy here, so it can't carry a write)
#endif
}

// Every place in writable guest memory that holds three floats in a row within tol of v[0..2], found by one background
// pass (about 0.3 s for 3 GB, below-normal priority; the frame never waits).
#define VEC_MAX 256
typedef struct { const float *v; float tol; uint64_t guest; size_t found; } VecCtx;
static uint64_t vec_out[VEC_MAX];
static float vec_v[3], vec_tol;
static atomic_int vec_state;  // 0 none, 1 running, 2 done (found list ready), read with acquire
static size_t vec_found;
static size_t vec_cb(const void *p, size_t n, void *vctx) {
    VecCtx *c = vctx;
    static uint32_t offs[CHUNK / 4 + 16];
    MsMatch m = {.type = MS_F32, .flo = (double)c->v[0] - c->tol, .fhi = (double)c->v[0] + c->tol};
    size_t k = ms_scan(p, n, &m, true, offs, sizeof offs / sizeof *offs, NULL);
    const unsigned char *b = p;
    for (size_t i = 0; i < k && c->found < VEC_MAX; i++) {
        if (offs[i] + 12 > n) continue;
        float y, z;
        memcpy(&y, b + offs[i] + 4, 4); memcpy(&z, b + offs[i] + 8, 4);
        if (fabsf(y - c->v[1]) <= c->tol && fabsf(z - c->v[2]) <= c->tol) vec_out[c->found++] = c->guest + offs[i];
    }
    return k;
}
static void *vec_worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    static uint64_t addr[REGIONS], size[REGIONS];
    static bool writable[REGIONS];
    int n = collect_regions(addr, size, writable);
    VecCtx c = {vec_v, vec_tol, 0, 0};
    for (int i = 0; i < n && c.found < VEC_MAX; i++) {
        if (!writable[i]) continue;
        for (uint64_t off = 0; off < size[i] && c.found < VEC_MAX;) {
            uint64_t a = addr[i] + off;
            uint64_t lim = (a & ~4095ull) + CHUNK;
            size_t len = (size_t)((addr[i] + size[i] < lim ? addr[i] + size[i] : lim) - a);
            len -= len % 4;
            if (!len) len = 4;
            c.guest = a;
            size_t ret;
            ms_guarded((const void *)(uintptr_t)a, len, vec_cb, &c, &ret);  // (a faulting chunk is just skipped)
            off += len;
        }
    }
    vec_found = c.found;
    atomic_store_explicit(&vec_state, 2, memory_order_release);
    return NULL;
}
bool control_mem_find_vec3_start(const float v[3], float tol) {
    if (atomic_load(&vec_state) == 1) return false;
    memcpy(vec_v, v, sizeof vec_v); vec_tol = tol; vec_found = 0;
    atomic_store(&vec_state, 1);
    pthread_t t;
    if (pthread_create(&t, NULL, vec_worker, NULL)) { atomic_store(&vec_state, 0); return false; }
    pthread_detach(t);
    return true;
}
int control_mem_find_vec3_poll(uint64_t *out, size_t max, size_t *n) {
    int st = atomic_load_explicit(&vec_state, memory_order_acquire);
    if (st != 2) return st;
    size_t k = vec_found < max ? vec_found : max;
    memcpy(out, vec_out, k * sizeof *out);
    *n = k;
    atomic_store(&vec_state, 0);
    return 2;
}

// ---- mem path <root> <target> [depth=N] [window=N]: how to reach a heap cell from a known object without searching.
// Heap addresses move between runs but the layout of the game's objects does not, so a chain "read the pointer at
// root+o1, then the one at that+o2, ..., then add the last offset" finds the same cell on every boot. Breadth first
// over every pointer-looking value in the first `window` bytes of each object (shortest chains first), on a
// background thread (below-normal priority, nothing waits for it). Reads only already committed memory.
#define PATH_NODES 2000000
#define PATH_SLOTS (1u << 22)
#define PATH_OUT 32
typedef struct { uint64_t base; int32_t parent; uint16_t off; uint8_t depth; } PNode;
static atomic_int path_state;  // 0 none, 1 running, 2 done
static uint64_t path_root, path_target, path_window, path_depth;
static char path_out[PATH_OUT][96];
static atomic_uint_fast64_t path_found, path_nodes, path_ms;
static bool path_truncated;
static bool path_in(const uint64_t *addr, const uint64_t *size, int n, uint64_t v, uint64_t *end) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (v < addr[m]) hi = m - 1; else if (v >= addr[m] + size[m]) lo = m + 1; else { *end = addr[m] + size[m]; return true; }
    }
    return false;
}
static void *path_worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    static uint64_t addr[REGIONS], size[REGIONS];
    static bool writable[REGIONS];
    uint64_t t0 = plat_now_ns();
    int nr = collect_regions(addr, size, writable);
    PNode *nodes = malloc(PATH_NODES * sizeof *nodes);
    uint64_t *seen = calloc(PATH_SLOTS, sizeof *seen);
    unsigned char *buf = malloc(path_window);
    size_t n = 0;
    if (nr > 0 && nodes && seen && buf) {
        nodes[n++] = (PNode){path_root, -1, 0, 0};
        for (size_t head = 0; head < n; head++) {
            PNode nd = nodes[head];
            uint64_t end;
            if (nd.depth >= path_depth || !path_in(addr, size, nr, nd.base, &end)) continue;
            size_t len = (size_t)(end - nd.base < path_window ? end - nd.base : path_window) & ~7ull;
            if (!len || !peek(nd.base, buf, len)) continue;
            for (size_t i = 0; i < len; i += 8) {
                uint64_t v, e2;
                memcpy(&v, buf + i, 8);
                if (v < 0x10000 || (v & 3) || !path_in(addr, size, nr, v, &e2)) continue;
                if (path_target >= v && path_target < v + path_window && atomic_load(&path_found) < PATH_OUT) {
                    uint64_t offs[16]; int k = 0;
                    offs[k++] = path_target - v; offs[k++] = i;  // (built backwards)
                    for (int64_t j = (int64_t)head; j > 0 && k < 16; j = nodes[j].parent) offs[k++] = nodes[j].off;
                    char *o = path_out[atomic_load(&path_found)]; size_t used = 0; o[0] = 0;
                    for (int j = k - 1; j >= 0; j--) used += (size_t)snprintf(o + used, 96 - used, j == k - 1 ? "0x%llx" : ",0x%llx", (unsigned long long)offs[j]);
                    atomic_fetch_add(&path_found, 1);
                }
                uint64_t h = (v * 0x9E3779B97F4A7C15ull) >> 42;  // (22 bits)
                bool dup = false;
                for (;; h = (h + 1) & (PATH_SLOTS - 1)) {
                    if (!seen[h]) { seen[h] = v; break; }
                    if (seen[h] == v) { dup = true; break; }
                }
                if (dup) continue;
                if (n == PATH_NODES) { path_truncated = true; continue; }
                nodes[n++] = (PNode){v, (int32_t)head, (uint16_t)i, (uint8_t)(nd.depth + 1)};
            }
            atomic_store(&path_nodes, n);
        }
    }
    atomic_store(&path_nodes, n);
    free(nodes); free(seen); free(buf);
    atomic_store(&path_ms, (plat_now_ns() - t0) / 1000000);
    atomic_store_explicit(&path_state, 2, memory_order_release);
    return NULL;
}
static void path_cmd(int argc, char **argv, ControlReply *r) {
    int st = atomic_load_explicit(&path_state, memory_order_acquire);
    if (argc == 3 && !strcmp(argv[2], "status")) {
        control_field_str(r, "state", st == 1 ? "running" : st == 2 ? "done" : "none");
        control_field_int(r, "nodes", (int64_t)atomic_load(&path_nodes));
        if (st == 2) {
            control_field_int(r, "ms", (int64_t)atomic_load(&path_ms));
            control_field_bool(r, "truncated", path_truncated);
            uint64_t f = atomic_load(&path_found);
            control_field_int(r, "paths", (int64_t)f);
            for (uint64_t i = 0; i < f; i++) { char k[24]; snprintf(k, sizeof k, "path.%llu", (unsigned long long)i); control_field_str(r, k, path_out[i]); }
        }
        return;
    }
    uint64_t root, target, window = 0x400, depth = 4;
    if (argc < 4 || argc > 6 || !integer(argv[2], &root) || !integer(argv[3], &target)) goto usage;
    for (int i = 4; i < argc; i++) {
        uint64_t v;
        if (!strncmp(argv[i], "depth=", 6) && integer(argv[i] + 6, &v) && v >= 1 && v <= 6) depth = v;
        else if (!strncmp(argv[i], "window=", 7) && integer(argv[i] + 7, &v) && v >= 16 && v <= 0xfff8 && !(v & 7)) window = v;
        else goto usage;
    }
    if (st == 1) { control_fail(r, "Path search in progress"); return; }
    path_root = root; path_target = target; path_window = window; path_depth = depth;
    path_truncated = false; atomic_store(&path_found, 0); atomic_store(&path_nodes, 0);
    atomic_store(&path_state, 1);
    pthread_t t;
    if (pthread_create(&t, NULL, path_worker, NULL)) { atomic_store(&path_state, 0); control_fail(r, "Cannot start path search"); return; }
    pthread_detach(t);
    control_field_bool(r, "started", true);
    return;
usage:
    control_fail(r, "Usage: mem path <root> <target> [depth=1..6] [window=16..65528] | status");
}

void control_mem_init(void) {
    static const ControlCmd cmds[] = {{"mem", "regions|read|write|scan|rescan|list|reset|path", "Inspect and search guest memory", cmd_mem}};
    control_register(cmds, 1);
}
