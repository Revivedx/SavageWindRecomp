// On-demand profiler: the recorder (runtime/prof.c) and its markers. Analysis: tools/swprof.py.
//
// Off (the normal state) a marker costs one load and one branch. A capture is started with prof_start() (the
// `prof` command, the hotkey, SAVAGEWIND_PROF=<first frame>[:<frames>]) and runs until prof_stop(); then one
// file, profile/swprof-<time>.swprof, is written on a thread of its own. The recorder is built so that a running
// capture uses a cheap cycle counter and a private cursor per thread. First use claims a bounded thread slot and
// registers names; steady-state writes do not allocate or take a mutex. Analysis happens afterwards.
//
//   SW_ZONE("name");      the rest of this block, as one event with a start and an end. Zones nest per thread.
//   SW_ACC("a/b");        like a zone, but the time and the calls are added up per thread and frame and written once a
//                         frame: for work done thousands of times a frame (a draw's steps), where an event each would
//                         fill the buffer. "/" in the name nests it under "a" in the report.
//   SW_TALLY("name");     only counts the calls (per thread and frame, like SW_ACC but with no clock reads): for events
//                         thousands of times a frame, like cache misses
//   SW_COUNT("name", n);  a number for this moment (draws, bytes, queue depth)
//   prof_frame(n);        the presenting thread, once per frame: frame n starts
//   prof_thread_name(s);  names the calling thread in the report
//
// Compile the SW_* marker macros away with -DSW_PROF_DISABLE (direct recorder calls remain).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- control (any thread; the command layer, the hotkey and the environment variable call these)
bool prof_start(void);             // false: already running, still writing the last file, or no memory
void prof_stop(void);              // ends the capture; the file is written in the background
bool prof_toggle(void);            // start, stop, or acknowledge a full buffer; true = now capturing
bool prof_active(void);
// One line of state for a command: capturing / writing / idle, how full the buffer is, the last file.
void prof_status(char *out, size_t n);
void prof_frame(uint64_t frame);   // the presenting thread, at the start of each frame
void prof_thread_name(const char *name);
// `prof [start | stop | toggle | status | report]`: argv as a command line gives it (argv[0] is "prof"). The answer
// goes to out; false = it failed (nothing to stop, still writing, ...). No front end is needed to build or use this.
bool prof_command(int argc, char **argv, char *out, size_t n);
// The last file written ("" before the first), for a command to report.
const char *prof_last_file(void);
void prof_shutdown(void);          // stop and wait for the file (normal exit / before a game reboot)
void prof_gpu_finish_hook(void (*finish)(void));  // backend drains its pending timestamps on the writer thread
bool prof_gpu_enabled(uint32_t generation);

// ---- markers
// GPU events are written with prof_put_gpu() from the code that reads the GPU's timestamps (nvn/vulkan/exec.c).
typedef struct { const char *name; uint32_t id; } ProfSite;  // id 0: not registered yet
typedef struct { uint64_t t0, t1, val; uint32_t site, kind; } ProfRec;  // 32 bytes. kind: low 8 bits PROF_*, ACC: calls << 8
enum { PROF_ZONE = 1, PROF_COUNTER = 2, PROF_FRAME = 3, PROF_ACC = 4, PROF_GPU = 5, PROF_SAMPLE = 6, PROF_STACK = 7 };
enum { PROF_GPU_COUNTER = PROF_COUNTER | 0x100 };  // a counter admitted during GPU drain too
// PROF_GPU: a stretch of GPU work (t0..t1 in cycle counter ticks, val = draws | frame << 32), written by whichever
// thread reads the GPU's timestamps back; the tool shows these as a thread of their own, "gpu".

// PROF_SAMPLE (the sampler, runtime/sampler.c): t0 = time, t1 = os thread id, val = kernel handle, kind >> 8 = the stack's
// depth; the PROF_STACK records after it hold the addresses, three to a record in t0, t1, val (innermost first).

// ---- the sampler (runtime/sampler.c): prof_start and the capture's writer call these
enum { PROF_MODULE_RUNTIME = 1, PROF_MODULE_GAME = 2, PROF_MODULE_OTHER = 3 };
typedef struct { uint64_t base, size, bias; uint32_t kind; char name[196]; } ProfModule;  // bias: add to a symbol's address
void prof_sampler_begin(void);
void prof_sampler_end(void);  // waits for the sampler thread to finish
int prof_sampler_modules(ProfModule *out, int max);
void prof_sampler_register_thread(void);  // called by prof_thread_name, including while recording is off

typedef struct ProfAcc ProfAcc;
typedef struct {
    ProfRec *cur, *end;
    ProfAcc *acc;
    uint64_t acc_frame;
    uint32_t gen, acc_seq, thread, busy;
} ProfCursor;

extern int g_prof_on;
extern uint32_t g_prof_gen;  // accessed atomically, changes with every capture
extern __thread ProfCursor *t_prof;

uint32_t prof_register(ProfSite *s);
// A marker named at run time ("pass: 4 color 1920x1080"): the name is copied, the same name gives the same id. Many
// different names run out of ids; those share one.
uint32_t prof_site_dyn(const char *name);
double prof_ticks_per_ns(void);                // the cycle counter's rate, measured since the capture started

static inline uint64_t prof_ticks(void) {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#error "prof_ticks: no cycle counter for this CPU"
#endif
}

static inline int prof_enabled(void) { return __atomic_load_n(&g_prof_on, __ATOMIC_RELAXED); }
static inline uint32_t prof_generation(void) { return __atomic_load_n(&g_prof_gen, __ATOMIC_ACQUIRE); }

void prof_put_gen(uint32_t gen, uint32_t kind, uint32_t site, uint64_t t0, uint64_t t1, uint64_t val);
static inline void prof_put(uint32_t kind, uint32_t site, uint64_t t0, uint64_t t1, uint64_t val) {
    prof_put_gen(prof_generation(), kind, site, t0, t1, val);
}

static inline void prof_put_gpu(uint32_t gen, uint32_t site, uint64_t t0, uint64_t t1, uint64_t val) {
    prof_put_gen(gen, PROF_GPU, site, t0, t1, val);
}

static inline uint32_t prof_site(ProfSite *s) {
    uint32_t id = __atomic_load_n(&s->id, __ATOMIC_ACQUIRE);
    return id ? id : prof_register(s);
}

typedef struct { ProfSite *site; uint64_t t0; uint32_t gen; } ProfZone;
static inline ProfZone prof_zone_begin(ProfSite *s) {
    uint32_t gen = prof_generation();
    return (ProfZone){s, prof_ticks(), gen};
}

static inline void prof_zone_exit(ProfZone *z) {
    if (z->t0 && z->gen == prof_generation() && prof_enabled())
        prof_put_gen(z->gen, PROF_ZONE, prof_site(z->site), z->t0, prof_ticks(), 0);
}

void prof_acc_add_gen(uint32_t gen, ProfSite *s, uint64_t ticks);
static inline void prof_acc_exit(ProfZone *z) {
    if (z->t0 && z->gen == prof_generation() && prof_enabled())
        prof_acc_add_gen(z->gen, z->site, prof_ticks() - z->t0);
}

#define PROF_CAT2(a, b) a##b
#define PROF_CAT(a, b) PROF_CAT2(a, b)

#ifdef SW_PROF_DISABLE
#define SW_ZONE(name) ((void)0)
#define SW_ACC(name) ((void)0)
#define SW_COUNT(name, n) ((void)0)
#define SW_TALLY(name) ((void)0)
#else
#define SW_ZONE(name) \
    static ProfSite PROF_CAT(prof_site_, __LINE__) = {name, 0}; \
    __attribute__((cleanup(prof_zone_exit), unused)) ProfZone PROF_CAT(prof_zone_, __LINE__) = prof_enabled() ? \
        prof_zone_begin(&PROF_CAT(prof_site_, __LINE__)) : (ProfZone){0}
#define SW_ACC(name) \
    static ProfSite PROF_CAT(prof_site_, __LINE__) = {name, 0}; \
    __attribute__((cleanup(prof_acc_exit), unused)) ProfZone PROF_CAT(prof_zone_, __LINE__) = prof_enabled() ? \
        prof_zone_begin(&PROF_CAT(prof_site_, __LINE__)) : (ProfZone){0}
#define SW_TALLY(name) \
    do { \
        static ProfSite prof_ts_ = {name, 0}; \
        if (prof_enabled()) prof_acc_add_gen(prof_generation(), &prof_ts_, 0); \
    } while (0)
#define SW_COUNT(name, n) \
    do { \
        static ProfSite prof_cs_ = {name, 0}; \
        if (prof_enabled()) prof_put(PROF_COUNTER, prof_site(&prof_cs_), prof_ticks(), (uint64_t)(n), 0); \
    } while (0)
#endif

// Per-thread, per-frame totals for SW_ACC: indexed by site id.
#define PROF_MAX_SITES 2048
struct ProfAcc {
    uint64_t ticks[PROF_MAX_SITES];
    uint32_t calls[PROF_MAX_SITES];
};

#ifdef __cplusplus
}
#endif
