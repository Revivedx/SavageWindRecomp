// On-demand profiler: the recorder and the capture file (runtime/prof.h has the markers and the overview).
//
// Memory: one pool of fixed chunks, claimed atomically and written through private thread cursors. The writer closes
// admission and waits for their short writes before reading or clearing the pool. Registration uses bounded slots;
// steady-state markers do not take a mutex. The pool is allocated on first capture and reused. Unused records stay
// zero; after producer completion the writer scans up to the first zero kind. A full pool stops capture and logs it.
//
// File (little endian), read by tools/swprof.py:
//   header   "SWPROF01", u32 version, u32 record size, u64 ticks0, ns0, ticks1, ns1 (the cycle counter and
//            plat_now_ns at start and stop: the tool derives the counter's rate), u64 unix seconds at start,
//            u32 sites, threads, chunks, u32 reserved
//   sites    per site: u32 id, u16 length, name
//   threads  per thread: u32 index, u32 reserved, u64 os thread id, char name[32]
//   modules  (the header's last u32 says how many) per module: u64 base, size, bias, u32 kind, char name[196]
//   chunks   per chunk: u32 thread, u32 records, then the records
#include "prof.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <pthread.h>
#endif

#include "rt.h"

#define CHUNK_RECS 8192  // 256 KB
#define MAX_THREADS 256
#define FILE_VERSION 1

int g_prof_on;
uint32_t g_prof_gen = 1;
static atomic_uint g_prof_seq;
static atomic_ullong g_prof_frame;
__thread ProfCursor *t_prof;
static __thread char t_name[32];

enum { IDLE, CAPTURING, WRITING };
static atomic_int state;
static atomic_bool stop_requested;  // the pool filled up: the presenting thread ends the capture at the next frame
static atomic_bool full_stop_pending;  // the next toggle acknowledges the automatic stop
static int gpu_on;
static void (*gpu_finish)(void);
static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t cursor_key;
static pthread_once_t cursor_once = PTHREAD_ONCE_INIT;
static bool frame_seen;

static pthread_mutex_t site_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *site_names[PROF_MAX_SITES];
static uint32_t nsites;  // ids 1..nsites

static struct ProfThread {
    ProfCursor c;
    ProfAcc acc;
    char name[32];
    uint64_t tid;
    bool owned;
} *threads;
static atomic_uint nthreads;

static ProfRec *pool;
static uint16_t *chunk_thread;
static uint32_t pool_chunks;
static atomic_uint next_chunk;

static uint64_t ticks0, ns0, ticks1, ns1, unix0;
static char last_file[256];

uint32_t prof_register(ProfSite *s) {
    pthread_mutex_lock(&site_lock);
    if (!__atomic_load_n(&s->id, __ATOMIC_RELAXED)) {
        uint32_t id;
        if (nsites + 1 < PROF_MAX_SITES) {
            site_names[++nsites] = s->name;
            id = nsites;
        } else {
            id = PROF_MAX_SITES - 1;
            site_names[id] = "(too many markers)";
        }
        __atomic_store_n(&s->id, id, __ATOMIC_RELEASE);
    }
    uint32_t id = __atomic_load_n(&s->id, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&site_lock);
    return id;
}

static uint64_t os_thread_id(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
#endif
}

void prof_thread_name(const char *name) {
    prof_sampler_register_thread();
    snprintf(t_name, sizeof t_name, "%s", name ? name : "");
    pthread_mutex_lock(&registry_lock);
    if (t_prof) snprintf(threads[t_prof->thread].name, sizeof threads[0].name, "%s", t_name);
    pthread_mutex_unlock(&registry_lock);
}

// ---- names made at run time: copied, deduplicated by a hash table (open addressing)
#define DYN_SLOTS 4096
static uint16_t dyn_slot[DYN_SLOTS];  // site id, 0: free
static uint32_t dyn_count;

uint32_t prof_site_dyn(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t id = 0;
    pthread_mutex_lock(&site_lock);
    for (uint32_t i = 0; i < DYN_SLOTS; i++) {
        uint16_t e = dyn_slot[(h + i) % DYN_SLOTS];
        if (!e) {
            if (nsites + 1 < PROF_MAX_SITES - 1 && dyn_count < DYN_SLOTS / 2) {
                char *copy = strdup(name);
                if (copy) {
                    site_names[++nsites] = copy;
                    dyn_slot[(h + i) % DYN_SLOTS] = (uint16_t)nsites;
                    dyn_count++;
                    id = nsites;
                }
            }
            break;
        }
        if (!strcmp(site_names[e], name)) { id = e; break; }
    }
    if (!id) {
        id = PROF_MAX_SITES - 1;  // out of ids: the rest share the last one
        site_names[id] = "(too many markers)";
        if (nsites < id) nsites = id;
    }
    pthread_mutex_unlock(&site_lock);
    return id;
}

double prof_ticks_per_ns(void) {
    uint64_t dn = plat_now_ns() - ns0, dt = prof_ticks() - ticks0;
    return dn > 1000000 ? (double)dt / (double)dn : 1.0;  // (less than a millisecond in: no measurement yet)
}

static void release_cursor(void *p) {
    pthread_mutex_lock(&registry_lock);
    // Keep this capture's totals until the writer consumes them. Another thread can reuse the slot next capture.
    threads[((ProfCursor *)p)->thread].owned = false;
    pthread_mutex_unlock(&registry_lock);
}
static void init_cursor_key(void) {
    if (pthread_key_create(&cursor_key, release_cursor)) abort();
    atexit(prof_shutdown);
}
static ProfCursor *claim_cursor(uint32_t gen) {
    pthread_mutex_lock(&registry_lock);
    if (gen == prof_generation() && (__atomic_load_n(&g_prof_on, __ATOMIC_SEQ_CST) ||
                                    __atomic_load_n(&gpu_on, __ATOMIC_SEQ_CST))) {
        for (unsigned i = 0; i < MAX_THREADS; i++) {
            if (threads[i].owned || threads[i].c.gen == gen) continue;
            threads[i].owned = true;
            threads[i].tid = os_thread_id();
            snprintf(threads[i].name, sizeof threads[i].name, "%s", t_name[0] ? t_name : "thread");
            t_prof = &threads[i].c;
            t_prof->thread = i;
            t_prof->acc = &threads[i].acc;
            if (pthread_setspecific(cursor_key, t_prof)) {
                threads[i].owned = false;
                t_prof = NULL;
            }
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return t_prof;
}

// A thread announces a short write, then rechecks admission. Sequential consistency orders that recheck and the
// announcement against the stop store and the writer's scan. The flag covers recording, never the timed scope.
static ProfCursor *enter(uint32_t gen, bool gpu) {
    if (gen != prof_generation()) return NULL;
    ProfCursor *c = t_prof ? t_prof : claim_cursor(gen);
    if (!c) return NULL;
    __atomic_store_n(&c->busy, 1, __ATOMIC_SEQ_CST);
    if (gen != prof_generation() || !__atomic_load_n(gpu ? &gpu_on : &g_prof_on, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&c->busy, 0, __ATOMIC_RELEASE);
        return NULL;
    }
    if (c->gen != gen) {
        c->cur = c->end = NULL;
        memset(c->acc, 0, sizeof *c->acc);
        c->acc_seq = atomic_load(&g_prof_seq);
        c->acc_frame = atomic_load(&g_prof_frame);
        c->gen = gen;
        atomic_fetch_add(&nthreads, 1);
    }
    return c;
}
static void leave(ProfCursor *c) { __atomic_store_n(&c->busy, 0, __ATOMIC_RELEASE); }

static bool append(ProfCursor *c, uint32_t kind, uint32_t site, uint64_t t0, uint64_t t1, uint64_t val) {
    if (c->cur == c->end) {
        unsigned i = atomic_fetch_add(&next_chunk, 1);
        if (i >= pool_chunks) {
            atomic_store(&stop_requested, true);
            __atomic_store_n(&g_prof_on, 0, __ATOMIC_SEQ_CST);
            return false;
        }
        chunk_thread[i] = (uint16_t)c->thread;
        c->cur = pool + (size_t)i * CHUNK_RECS;
        c->end = c->cur + CHUNK_RECS;
    }
    *c->cur++ = (ProfRec){t0, t1, val, site, kind};
    return true;
}

void prof_put_gen(uint32_t gen, uint32_t kind, uint32_t site, uint64_t t0, uint64_t t1, uint64_t val) {
    ProfCursor *c = enter(gen, kind == PROF_GPU || kind == PROF_GPU_COUNTER);
    if (!c) return;
    append(c, kind, site, t0, t1, val);
    leave(c);
    if (__builtin_expect(atomic_load_explicit(&stop_requested, memory_order_relaxed), 0)) prof_stop();
}

static bool flush_acc(ProfCursor *c) {
    uint64_t now = prof_ticks();
    for (uint32_t id = 1; id < PROF_MAX_SITES; id++)
        if (c->acc->calls[id]) {
            if (!append(c, PROF_ACC | (c->acc->calls[id] << 8), id, now, c->acc->ticks[id], c->acc_frame))
                return false;  // the writer streams the remaining totals with their original frame
            c->acc->ticks[id] = 0;
            c->acc->calls[id] = 0;
        }
    c->acc_seq = atomic_load(&g_prof_seq);
    c->acc_frame = atomic_load(&g_prof_frame);
    return true;
}

void prof_acc_add_gen(uint32_t gen, ProfSite *s, uint64_t ticks) {
    ProfCursor *c = enter(gen, false);
    if (!c) return;
    if (c->acc_seq != atomic_load(&g_prof_seq) && !flush_acc(c)) {
        leave(c);
        prof_stop();
        return;  // admission closed before this frame's addition; do not merge it into the retained frame
    }
    uint32_t id = prof_site(s);
    c->acc->ticks[id] += ticks;
    c->acc->calls[id]++;
    leave(c);
    if (__builtin_expect(atomic_load_explicit(&stop_requested, memory_order_relaxed), 0)) prof_stop();
}

void prof_gpu_finish_hook(void (*finish)(void)) { __atomic_store_n(&gpu_finish, finish, __ATOMIC_RELEASE); }
bool prof_gpu_enabled(uint32_t gen) {
    return gen == prof_generation() && __atomic_load_n(&gpu_on, __ATOMIC_ACQUIRE);
}

void prof_frame(uint64_t frame) {
    // SAVAGEWIND_PROF=<first frame>[:<frames>]: start at a frame, stop after some (without a count: when asked).
    // SAVAGEWIND_GPUPROF_AT=<frame> is kept as SAVAGEWIND_PROF=<frame>:1.
    static int env_read, env_done;
    static long env_first = -1, env_count;
    static uint64_t env_stop_at;
    static uint32_t env_gen;
    if (!env_read) {
        env_read = 1;
        const char *e = getenv("SAVAGEWIND_PROF");
        char alias[32];
        if (!(e && *e) && getenv("SAVAGEWIND_GPUPROF_AT")) {  // the old per-pass GPU timing of one frame
            snprintf(alias, sizeof alias, "%ld:1", strtol(getenv("SAVAGEWIND_GPUPROF_AT"), NULL, 10));
            e = alias;
        }
        if (e && *e) {
            char *end;
            env_first = strtol(e, &end, 10);
            if (*end == ':') env_count = strtol(end + 1, NULL, 10);
            if (env_first < 0) env_first = 0;
        }
    }
    if (env_first >= 0 && !env_done && frame >= (uint64_t)env_first && atomic_load(&state) == IDLE) {
        env_done = 1;
        if (prof_start()) {
            env_stop_at = env_count > 0 ? frame + (uint64_t)env_count : 0;
            env_gen = prof_generation();
        }
    }
    if (env_stop_at && env_gen != prof_generation()) env_stop_at = 0;
    if (!prof_enabled()) {
        if (atomic_load(&stop_requested) && atomic_load(&state) == CAPTURING) prof_stop();  // the buffer filled up
        return;
    }
    if (env_stop_at && frame >= env_stop_at) {
        env_stop_at = 0;
        prof_stop();
        return;
    }
    uint64_t now = prof_ticks();
    ProfCursor *c = enter(prof_generation(), false);
    if (!c) return;
    atomic_store(&g_prof_frame, frame);
    atomic_fetch_add(&g_prof_seq, 1);
    frame_seen = true;
    append(c, PROF_FRAME, 0, now, 0, frame);
    leave(c);
}

bool prof_active(void) { return atomic_load(&state) == CAPTURING; }
const char *prof_last_file(void) {
    static __thread char copy[256];
    pthread_mutex_lock(&control_lock);
    snprintf(copy, sizeof copy, "%s", last_file);
    pthread_mutex_unlock(&control_lock);
    return copy;
}

bool prof_start(void) {
    pthread_mutex_lock(&control_lock);
    if (atomic_load(&state) != IDLE) { pthread_mutex_unlock(&control_lock); return false; }
    pthread_once(&cursor_once, init_cursor_key);
    if (!pool) {
        const char *mb = getenv("SAVAGEWIND_PROF_MB");
        long n = mb ? atol(mb) : 256;
        if (n < 8) n = 8;
        if (n > 4096) n = 4096;
        pool_chunks = (uint32_t)((size_t)n * 1048576 / (CHUNK_RECS * sizeof(ProfRec)));
        pool = calloc(pool_chunks, CHUNK_RECS * sizeof(ProfRec));
        chunk_thread = calloc(pool_chunks, sizeof *chunk_thread);
        threads = calloc(MAX_THREADS, sizeof *threads);
        if (!pool || !chunk_thread || !threads) {
            free(pool);
            free(chunk_thread);
            pool = NULL;
            chunk_thread = NULL;
            free(threads);
            threads = NULL;
            LOG("[prof] no memory for the capture buffer (%ld MB)\n", n);
            pthread_mutex_unlock(&control_lock);
            return false;
        }
    }
    atomic_store(&next_chunk, 0);
    atomic_store(&nthreads, 0);
    atomic_store(&stop_requested, false);
    atomic_store(&full_stop_pending, false);
    unix0 = (uint64_t)time(NULL);
    ns0 = plat_now_ns();
    ticks0 = prof_ticks();
    atomic_store(&g_prof_seq, 0);
    frame_seen = false;
    __atomic_add_fetch(&g_prof_gen, 1, __ATOMIC_RELEASE);
    atomic_store(&state, CAPTURING);
    __atomic_store_n(&gpu_on, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_prof_on, 1, __ATOMIC_SEQ_CST);
    prof_sampler_begin();
    LOG("[prof] capture started (buffer %u MB)\n", (unsigned)((uint64_t)pool_chunks * CHUNK_RECS * sizeof(ProfRec) >> 20));
    pthread_mutex_unlock(&control_lock);
    return true;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

static void *writer_main(void *arg) {
    (void)arg;
    prof_sampler_end();
    void (*finish)(void) = __atomic_load_n(&gpu_finish, __ATOMIC_ACQUIRE);
    if (finish) finish();
    __atomic_store_n(&gpu_on, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&registry_lock);
    for (unsigned i = 0; i < MAX_THREADS; i++)
        while (__atomic_load_n(&threads[i].c.busy, __ATOMIC_SEQ_CST))
            plat_sleep_until_ns(plat_now_ns() + 100000);
    pthread_mutex_lock(&site_lock);
    static ProfModule modules[1024];
    int nmod = prof_sampler_modules(modules, 1024);
    uint32_t used = atomic_load(&next_chunk);
    if (used > pool_chunks) used = pool_chunks;
    uint32_t map[MAX_THREADS], totals[MAX_THREADS], nt = 0, extras = frame_seen ? 1 : 0;
    for (unsigned i = 0; i < MAX_THREADS; i++) {
        map[i] = nt;
        totals[i] = 0;
        if (threads[i].c.gen != prof_generation()) continue;
        nt++;
        for (unsigned j = 1; j < PROF_MAX_SITES; j++)
            if (threads[i].acc.calls[j]) totals[i]++;
        if (totals[i]) extras++;
    }

    char dir[] = "profile", path[256];
    struct tm tmv;
    time_t t0 = (time_t)unix0;
#ifdef _WIN32
    _mkdir(dir);
    localtime_s(&tmv, &t0);
#else
    mkdir(dir, 0755);
    localtime_r(&t0, &tmv);
#endif
    snprintf(path, sizeof path, "%s/swprof-%04d%02d%02d-%02d%02d%02d.swprof", dir, tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    // two captures in the same second must not overwrite each other: -2, -3, ...
    for (unsigned n = 2;; n++) {
        FILE *e = fopen(path, "rb");
        if (!e) break;
        fclose(e);
        snprintf(path, sizeof path, "%s/swprof-%04d%02d%02d-%02d%02d%02d-%u.swprof", dir, tmv.tm_year + 1900,
                 tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, n);
    }
    FILE *f = fopen(path, "wb");
    uint64_t records = 0;
    if (!f) {
        LOG("[prof] cannot write %s\n", path);
    } else {
        fwrite("SWPROF01", 1, 8, f);
        put32(f, FILE_VERSION);
        put32(f, sizeof(ProfRec));
        put64(f, ticks0);
        put64(f, ns0);
        put64(f, ticks1);
        put64(f, ns1);
        put64(f, unix0);
        put32(f, nsites);
        put32(f, nt);
        put32(f, used + extras);
        put32(f, (uint32_t)nmod);
        for (uint32_t i = 1; i <= nsites; i++) {
            const char *n = site_names[i] ? site_names[i] : "?";
            put32(f, i);
            uint16_t len = (uint16_t)strlen(n);
            fwrite(&len, 2, 1, f);
            fwrite(n, 1, len, f);
        }
        for (unsigned i = 0; i < MAX_THREADS; i++) {
            if (threads[i].c.gen != prof_generation()) continue;
            put32(f, map[i]);
            put32(f, 0);
            put64(f, threads[i].tid);
            fwrite(threads[i].name, 1, sizeof threads[i].name, f);
        }
        for (int i = 0; i < nmod; i++) {
            put64(f, modules[i].base);
            put64(f, modules[i].size);
            put64(f, modules[i].bias);
            put32(f, modules[i].kind);
            fwrite(modules[i].name, 1, sizeof modules[i].name, f);
        }
        for (uint32_t i = 0; i < used; i++) {
            ProfRec *r = pool + (size_t)i * CHUNK_RECS;
            uint32_t n = 0;
            while (n < CHUNK_RECS && __atomic_load_n(&r[n].kind, __ATOMIC_ACQUIRE)) n++;
            put32(f, map[chunk_thread[i]]);
            put32(f, n);
            fwrite(r, sizeof(ProfRec), n, f);
            records += n;
        }
        // Final totals and the closing frame do not compete for space in a full recording pool.
        for (unsigned i = 0; i < MAX_THREADS; i++) {
            if (!totals[i]) continue;
            put32(f, map[i]); put32(f, totals[i]);
            for (unsigned j = 1; j < PROF_MAX_SITES; j++) {
                if (!threads[i].acc.calls[j]) continue;
                ProfRec r = {ticks1, threads[i].acc.ticks[j], threads[i].c.acc_frame,
                             j, PROF_ACC | (threads[i].acc.calls[j] << 8)};
                fwrite(&r, sizeof r, 1, f);
                records++;
            }
        }
        if (frame_seen) {
            ProfRec r = {ticks1, 0, atomic_load(&g_prof_frame) + 1, 0, PROF_FRAME};
            put32(f, 0); put32(f, 1); fwrite(&r, sizeof r, 1, f);
            records++;
        }
        bool failed = ferror(f) != 0;
        if (fclose(f)) failed = true;
        if (failed) { remove(path); LOG("[prof] failed writing %s (capture discarded)\n", path); }
        else {
            pthread_mutex_lock(&control_lock);
            snprintf(last_file, sizeof last_file, "%s", path);
            pthread_mutex_unlock(&control_lock);
            LOG("[prof] wrote %s: %llu events, %u threads, %.2f s\n", path, (unsigned long long)records, nt,
                (double)(ns1 - ns0) / 1e9);
        }
    }
    // back to zero for the next capture (only what was used)
    memset(pool, 0, (size_t)used * CHUNK_RECS * sizeof(ProfRec));
    pthread_mutex_unlock(&site_lock);
    pthread_mutex_unlock(&registry_lock);
    atomic_store(&state, IDLE);
    return NULL;
}

void prof_stop(void) {
    pthread_mutex_lock(&control_lock);
    if (atomic_load(&state) != CAPTURING) { pthread_mutex_unlock(&control_lock); return; }
    atomic_store(&state, WRITING);
    __atomic_store_n(&g_prof_on, 0, __ATOMIC_SEQ_CST);
    ticks1 = prof_ticks();
    ns1 = plat_now_ns();
    if (atomic_load(&stop_requested)) {
        atomic_store(&full_stop_pending, true);
        LOG("[prof] buffer full, stopping capture\n");
    }
    pthread_t th;
    bool started = pthread_create(&th, NULL, writer_main, NULL) == 0;
    if (started) pthread_detach(th);
    pthread_mutex_unlock(&control_lock);
    if (!started) writer_main(NULL);
}

void prof_shutdown(void) {
    prof_stop();
    while (atomic_load(&state) != IDLE) plat_sleep_until_ns(plat_now_ns() + 1000000);
}

bool prof_toggle(void) {
    if (atomic_exchange(&full_stop_pending, false)) {
        LOG("[prof] capture already ended (buffer full); press F6 again to start a capture\n");
        return false;
    }
    if (prof_active()) {
        prof_stop();
        return false;
    }
    return prof_start();
}

void prof_status(char *out, size_t n) {
    pthread_mutex_lock(&control_lock);
    int s = atomic_load(&state);
    uint32_t used = atomic_load(&next_chunk);
    if (used > pool_chunks) used = pool_chunks;
    if (s == CAPTURING)
        snprintf(out, n, "capturing for %.1f s, buffer %u of %u chunks used, %u threads",
                 (double)(plat_now_ns() - ns0) / 1e9, used, pool_chunks, atomic_load(&nthreads));
    else if (s == WRITING)
        snprintf(out, n, "writing the capture");
    else
        snprintf(out, n, "idle%s%s", last_file[0] ? ", last file " : "", last_file);
    pthread_mutex_unlock(&control_lock);
}

// `prof [start | stop | toggle | status | report]`, for any front end that has words (the control layer's command, a
// console): it needs nothing from them, they only hand over the arguments and show the text.
bool prof_command(int argc, char **argv, char *out, size_t n) {
    const char *a = argc > 1 ? argv[1] : "status";
    if (argc > 2) {
        snprintf(out, n, "Usage: prof [start | stop | toggle | status | report]");
        return false;
    }
    if (!strcmp(a, "start")) {
        if (prof_start()) {
            snprintf(out, n, "capture started (prof stop, or F6, ends it)");
            return true;
        }
        char st[256];
        prof_status(st, sizeof st);
        snprintf(out, n, "cannot start: %s", st);
        return false;
    }
    if (!strcmp(a, "stop")) {
        if (!prof_active()) {
            snprintf(out, n, "no capture is running");
            return false;
        }
        prof_stop();
        snprintf(out, n, "capture stopped, writing the file (prof status shows when it is done)");
        return true;
    }
    if (!strcmp(a, "toggle")) {
        bool was = prof_active();
        bool now = prof_toggle();
        if (now) snprintf(out, n, "capture started");
        else if (was) snprintf(out, n, "capture stopped, writing the file");
        else snprintf(out, n, "cannot start now (prof status says why)");
        return now || was;
    }
    if (!strcmp(a, "status")) {
        prof_status(out, n);
        return true;
    }
    if (!strcmp(a, "report")) {
        const char *file = prof_last_file();
        if (!file[0]) {
            snprintf(out, n, "no capture has been written yet");
            return false;
        }
        snprintf(out, n, "python3 tools/swprof.py %s --exe <the swr binary>", file);
        return true;
    }
    snprintf(out, n, "Unknown argument %s. Usage: prof [start | stop | toggle | status | report]", a);
    return false;
}
