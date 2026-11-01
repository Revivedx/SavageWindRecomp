// Guest address -> recompiled function lookup, and host (HLE) function registry.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamelib.h"
#include "hle.h"
#include "kernel.h"

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[MAX_MODULES];  // one per module of the game code (runtime/gamelib.h)
static int ntables;

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("SAVAGEWIND_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

static void hle_unimplemented(Cpu *c);

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

static __thread uint64_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (x0=%llx x1=%llx lr=%llx) -> returning 0\n", hle_slots[i].name, c->x[0],
            c->x[1], c->x[30]);
    c->x[0] = 0;
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

static uint64_t hle_next = HLE_HEAP;
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {  // (after gamelib_load)
    const GameLibTables *g = gamelib_tables();
    ntables = 0;
    for (uint32_t i = 0; i < *g->nmodules && i < MAX_MODULES; i++) {
        const GameLibModule *m = &g->modules[i];
        uint64_t n = *m->count;
        if (n) tables[ntables++] = (FnTable){m->addrs, m->fns, n, m->addrs[0], m->addrs[n - 1]};
    }
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < ntables; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

// Small direct-mapped cache in front of the binary search
#define DCACHE 4096
static __thread struct { uint64_t addr; GuestFn fn; } dcache[DCACHE];

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();  // (HLE calls aren't cached below, so every one passes here: a reboot's safe point)
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f) {
        uint64_t off;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", pc, c->x[30], s ? s : "?", off);
    }
    unsigned h = (unsigned)(pc >> 2) & (DCACHE - 1);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    KThread *t = c->thread;  // (a better thread waiting for this one's core gets it: kernel.c kernel_preempt)
    if (t && t->core_held >= 0 && __builtin_expect(g_core_best_wait[t->core_held].v < t->prio, 0)) kernel_preempt(t);
    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);
    GuestFn f = dcache[h].addr == c->pc ? dcache[h].fn : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < ntables; t++) total += tables[t].count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < ntables; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    const char *s = plat_symbol(pc, offset);
    return s ? s : rt_host_fn_name(pc, offset);
}

void rt_fatal_trace(void) {
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    Cpu *c = kernel_current_cpu();
    if (c) {
        fprintf(stderr, "guest: sp=%llx lr=%llx\n", c->sp, c->x[30]);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoint: the page is protected; accesses in [lo, hi) are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the next re-arm.
uint64_t g_watch_lo, g_watch_hi;
int g_fault_is_write;
static uint64_t watch_page;
static uint64_t retired_pages[8];  // pages watched before a retarget: the protect thread may still close one
// SAVAGEWIND_WATCH_STEP=1 (Windows): the page closes again right after each access (single-step) instead of on
// the 100 us timer below, so every access in the range is seen.
static int watch_step = -1;
static bool watch_stepping(void) {
    if (watch_step < 0) watch_step = getenv("SAVAGEWIND_WATCH_STEP") != NULL;
    return watch_step;
}
// A second range, the node SAVAGEWIND_WATCH_FOLLOW moves to (its own page; the first range stays watched too).
static uint64_t node_lo, node_hi, node_page;

uint64_t rt_watch_step_page(uint64_t fa) {
    uint64_t page = fa & ~(PLAT_PAGE - 1);
    return watch_stepping() && (page == watch_page || page == node_page) ? page : 0;
}

bool rt_watch_in_range(uint64_t fa) { return (fa >= g_watch_lo && fa < g_watch_hi) || (fa >= node_lo && fa < node_hi); }

// SAVAGEWIND_WATCH_FOLLOW=<off>[:<len>] (hex, with WATCH_STEP): each time a write stores a pointer at watch_lo + off
// (a list head's next), the second range moves to [pointer, pointer + len) (default 0x20: a block header), so the
// writes to the newest node are caught. Returns that node's page to close now, or 0.
uint64_t rt_watch_after_write(uint64_t addr, uint64_t value) {
    static int follow = -1;
    static uint64_t off, len;
    if (follow < 0) {
        const char *e = getenv("SAVAGEWIND_WATCH_FOLLOW");
        follow = e != NULL;
        if (e) { off = strtoull(e, (char **)&e, 16); len = *e == ':' ? strtoull(e + 1, NULL, 16) : 0x20; }
    }
    if (!follow || addr != g_watch_lo + off || !value || (value & 7) || (value >= g_watch_lo && value < g_watch_hi))
        return 0;
    uint64_t old = node_page;
    node_lo = value;
    node_hi = value + len;
    node_page = value & ~(PLAT_PAGE - 1);
    if (old && old != node_page && old != watch_page) {
        static int nretired_nodes;  // (another thread's single-step may close it again: rt_fault_watch reopens it)
        retired_pages[nretired_nodes++ & 7] = old;
        plat_protect(old, PLAT_PAGE, PLAT_PROT_RW);
    }
    fprintf(stderr, "[watch] node %llx+%llx\n", (unsigned long long)value, (unsigned long long)len);
    return node_page;
}

static void *watch_thread(void *a) {
    (void)a;
    int r = plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE);
    fprintf(stderr, "[watch] page %llx protect -> %d\n", (unsigned long long)watch_page, r);
    if (watch_stepping()) return NULL;
    for (;;) { plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE); usleep(100); }
    return NULL;
}
void watch_arm(void) {
    if (!g_watch_lo || watch_page) return;
    watch_page = g_watch_lo & ~(PLAT_PAGE - 1);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}

// Points the watchpoint at [lo, hi) (runtime/kernel.c SAVAGEWIND_WATCH_TRAP follows a list node with it).
void watch_retarget(uint64_t lo, uint64_t hi) {
    uint64_t old = watch_page;
    g_watch_lo = lo;
    g_watch_hi = hi;
    fprintf(stderr, "[watch] trap now on %llx+%llx\n", (unsigned long long)lo, (unsigned long long)(hi - lo));
    if (!old) { watch_arm(); return; }
    static int nretired;
    retired_pages[nretired++ & 7] = old;
    watch_page = lo & ~(PLAT_PAGE - 1);
    if (watch_page != old) plat_protect(old, PLAT_PAGE, PLAT_PROT_RW);
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    uint64_t page = fa & ~(PLAT_PAGE - 1);
    bool retired = false;
    for (int i = 0; i < 8; i++) retired |= retired_pages[i] && retired_pages[i] == page;
    if (retired && page != watch_page && page != node_page) {
        plat_protect(page, PLAT_PAGE, PLAT_PROT_RW);
        return true;
    }
    bool on_node_page = node_page && page == node_page;
    if (!on_node_page && (!watch_page || fa < watch_page || fa >= watch_page + PLAT_PAGE)) return false;
    static int log_writes = -1;  // SAVAGEWIND_WATCH_WRITES=<n>: every write (not one line per pc), up to n lines
    if (log_writes < 0) log_writes = getenv("SAVAGEWIND_WATCH_WRITES") ? atoi(getenv("SAVAGEWIND_WATCH_WRITES")) : 0;
    if (log_writes && rt_watch_in_range(fa)) {
        static _Atomic int nlogged;
        if (g_fault_is_write && nlogged++ < log_writes) {
            uint64_t off = 0;
            const char *nm = host_sym(pc, &off);
            bool node = fa >= node_lo && fa < node_hi;
            if (!nm) {  // our own executable: its offset from rt_fault_watch maps to a function with nm/addr2line
                nm = "savagewind";
                off = pc - (uint64_t)(uintptr_t)&rt_fault_watch;
            }
            fprintf(stderr, "[watch] %.6f write %s+%#llx by %s%s%llx (thread %x)\n", plat_now_ns() / 1e9,
                    node ? "node" : "head", (unsigned long long)(fa - (node ? node_lo : g_watch_lo)), nm,
                    !strcmp(nm, "savagewind") ? " rt_fault_watch+" : "+", (unsigned long long)off,
                    kernel_current_thread_handle());
            extern void kernel_log_guest_chain(void);
            kernel_log_guest_chain();
        }
    } else if (fa >= g_watch_lo && fa < g_watch_hi) {
        static uint64_t seen[256];  // one line per host pc (the module offset maps to f_<addr> with nm)
        static int nseen;
        uint64_t off = 0;
        const char *nm = host_sym(pc, &off);
        if (!nm) nm = "?";
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= seen[i] == pc;
        if (!found && nseen < 256) {
            seen[nseen++] = pc;
            fprintf(stderr, "[watch] +%#llx of %llx by %s+%llx\n", (unsigned long long)(fa - g_watch_lo),
                    (unsigned long long)g_watch_lo, nm, (unsigned long long)off);
            extern void kernel_log_guest_chain(void);
            kernel_log_guest_chain();  // (the guest calls that led there: lr and frame records)
        }
    }
    plat_protect(page, PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }
