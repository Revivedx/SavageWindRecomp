// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
// GAMELIB_CFLAGS_COMPAT: the same for the compatible target (gamelib.h gamelib_compat), where there is one
#ifndef GAMELIB_ZIGFLAGS  // (zig cc only: what makes it build what clang does with GAMELIB_CFLAGS)
#define GAMELIB_ZIGFLAGS "-Xclang -O1 -fno-vectorize -fno-slp-vectorize -g0 -fomit-frame-pointer"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif
#ifndef GAMELIB_GENARGS  // the generator's options that change the game code (RECOMP_LOCALS=1: --locals)
#define GAMELIB_GENARGS ""
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: swgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or SAVAGEWIND_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("SAVAGEWIND_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("swgen" SEP "swgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("SAVAGEWIND_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("savagewind.dll.a", "build/savagewind.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
#ifdef __linux__
    // zig's own glibc headers and stubs (lib/libc, tools/fetch_zig.sh keeps them), as old as the oldest glibc the
    // release runs on (docs/linux.md). Without a target it builds for the native libc and wants the system's headers,
    // which SteamOS and other read-only systems don't have: "failed to find libc installation:
    // LibCStdLibHeaderNotFound" (#191). The game code only takes memcpy / math / fenv from it.
#if defined(__aarch64__)
    if (t->zig) { add(a, "-target"); add(a, "aarch64-linux-gnu.2.35"); }
#else
    if (t->zig) { add(a, "-target"); add(a, "x86_64-linux-gnu.2.35"); }
#endif
#endif
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

// The modules of <work>/layout.txt (`gen layout`, tools/layout.py), in load order; their image sizes in size[].
static int read_layout(const char *path, Gen *gens, double *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char names[MAX_MODULES][16];
    char line[256];
    int n = 0;
    while (n < MAX_MODULES && fgets(line, sizeof line, f)) {
        unsigned long long base, sz;
        if (line[0] == '#' || sscanf(line, "%15s %llx %llx", names[n], &base, &sz) != 3) continue;
        memset(&gens[n], 0, sizeof gens[n]);
        gens[n].name = names[n];
        size[n++] = (double)sz;
    }
    fclose(f);
    return n;
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// The end of a failed generator's or compile's log, into this log: the work folder (and their own logs with it) is
// deleted after the build, a crash leaves a stack (faulthandler, tools/gen.py) of many lines, and a compile's last
// line is only "1 error generated", after the error itself.
static void log_gen_tail(const char *path, int exit_code) {
    if (exit_code > 128 && exit_code < 255)  // (Linux, macOS: ended by a signal; 9, killed: often out of memory)
        LOG("[gamebuild]   ended by signal %d\n", exit_code - 128);
    else  // (255 on Windows: also a crash, e.g. an access violation)
        LOG("[gamebuild]   exit status %d\n", exit_code);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char buf[16384];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    const char *line[40];
    int n = 0;
    for (char *s = buf, *e; *s; s = e) {
        e = s + strcspn(s, "\n");
        if (*e) *e++ = 0;
        size_t l = strlen(s);
        if (l && s[l - 1] == '\r') s[l - 1] = 0;
        if (!*s || strstr(s, ": progress ")) continue;  // (the progress bar's lines)
        if (n == 40) memmove(line, line + 1, sizeof line[0] * --n);
        line[n++] = s;
    }
    if (!n) LOG("[gamebuild]   (no message in its log)\n");
    for (int i = 0; i < n; i++) LOG("[gamebuild]   | %s\n", line[i]);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code is built with: this CPU's target's (gamelib.h; part of its id). zig cc reads -march=<cpu>
// with its own CPU names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU
// as -mcpu there.
static const char *target_cflags(void) {
#ifdef GAMELIB_CFLAGS_COMPAT
    if (gamelib_compat()) return GAMELIB_CFLAGS_COMPAT;
#endif
    return GAMELIB_CFLAGS;
}

static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", target_cflags());
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
    if (t->zig) add_split(a, GAMELIB_ZIGFLAGS);  // (after them: the last -O that reaches clang wins)
}

// ------------------------------------------------------------------ pieces kept between builds
// Every compiled piece of game code is kept in <data root>/cache/gamecode/, named by a hash of its C, the compiler
// (zig's executable), the flags and the headers it includes. The game code is rebuilt after every Savage Wind update,
// but most of the generated C comes out the same, so a rebuild compiles only the pieces that changed. After a good
// build only the pieces it used stay, so the folder holds about one build. SAVAGEWIND_NO_GAMECODE_CACHE=1: not used.
// Unit 0, the shipped entry file, is always compiled: it carries this build's GAMELIB_ID.
static char gc_dir[1100];
static uint64_t gc_seed;  // the hash everything but the C itself adds up to; 0: off
static uint64_t *gc_used;
static int gc_nused, gc_cap, gc_reused;

static uint64_t gc_hash_bytes(uint64_t h, const void *p_, size_t n) {  // (FNV-1a)
    const uint8_t *p = (const uint8_t *)p_;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint64_t gc_hash_file(uint64_t h, const char *path) {  // 0: unreadable
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buf[1 << 16];  // (the build's own thread only)
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = gc_hash_bytes(h, buf, n);
    fclose(f);
    return h ? h : 1;
}

static void gc_begin(const Tools *t) {
    gc_nused = gc_reused = 0;
    gc_seed = 0;
    const char *off = getenv("SAVAGEWIND_NO_GAMECODE_CACHE");
    if (off && *off && strcmp(off, "0")) return;
    uint64_t h = gc_hash_file(1469598103934665603ull, t->cc);
    if (!h) return;
    Args a = {0};  // the compile line but the file names: the flags, the target, the id define left out (unit 0 only)
    add_cflags(&a, t);
    for (int i = 0; i < a.n; i++) h = gc_hash_bytes(h, a.v[i], strlen(a.v[i]) + 1);
    args_free(&a);
    DIR *d = opendir(t->inc);  // the headers: names and contents, summed so the order readdir gives doesn't matter
    if (!d) return;
    uint64_t sum = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s" SEP "%s", t->inc, e->d_name);
        sum += gc_hash_file(gc_hash_bytes(h, e->d_name, strlen(e->d_name)), p);
    }
    closedir(d);
    h = gc_hash_bytes(h, &sum, sizeof sum);
    snprintf(gc_dir, sizeof gc_dir, "%s" SEP "cache", rt_data_root());
    mkdir(gc_dir, 0755);
    snprintf(gc_dir, sizeof gc_dir, "%s" SEP "cache" SEP "gamecode", rt_data_root());
    mkdir(gc_dir, 0755);
    gc_seed = h ? h : 1;
}

static bool gc_copy(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    static char buf[1 << 16];
    bool ok = a && b;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
    if (a) fclose(a);
    if (b && fclose(b)) ok = false;
    if (!ok && b) remove(to);
    return ok;
}

static void gc_path(uint64_t key, char *p, size_t n) {
    snprintf(p, n, "%s" SEP "%016llx.o", gc_dir, (unsigned long long)key);
}

static void gc_note(uint64_t key) {
    if (gc_nused == gc_cap) gc_used = (uint64_t *)realloc(gc_used, sizeof *gc_used * (gc_cap = gc_cap ? gc_cap * 2 : 1024));
    gc_used[gc_nused++] = key;
}

// A piece compiled before: its object put in place, true
static bool gc_take(const Unit *u) {
    uint64_t key = gc_seed ? gc_hash_file(gc_seed, u->src) : 0;
    char p[1200];
    gc_path(key, p, sizeof p);
    if (!key || !gc_copy(p, u->obj)) return false;
    gc_note(key);
    gc_reused++;
    return true;
}

static void gc_store(const Unit *u) {
    uint64_t key = gc_seed ? gc_hash_file(gc_seed, u->src) : 0;
    char p[1200], tmp[1300];
    gc_path(key, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);  // (then renamed: a build stopped midway leaves no half piece behind)
    if (!key || !gc_copy(u->obj, tmp)) return;
    remove(p);
    if (rename(tmp, p) == 0) gc_note(key);
    else remove(tmp);
}

static int gc_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// After the build: only the pieces it used stay. After a failed one that took pieces, none: the next build compiles
// everything, in case a kept piece was the problem.
static void gc_finish(bool ok, int units) {
    if (!gc_seed || (!ok && !gc_reused)) return;  // (failed with nothing taken: what it compiled is good to keep)
    if (!ok) gc_nused = 0;
    qsort(gc_used, gc_nused, sizeof *gc_used, gc_cmp);
    int removed = 0;
    DIR *d = opendir(gc_dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        unsigned long long k;
        char end;
        size_t n = strlen(e->d_name);
        if ((n != 18 && n != 22) || sscanf(e->d_name, "%16llx.%c", &k, &end) != 2 || end != 'o') continue;  // (.o, .o.tmp)
        uint64_t key = k;
        if (n == 18 && gc_nused && bsearch(&key, gc_used, gc_nused, sizeof *gc_used, gc_cmp)) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s" SEP "%s", gc_dir, e->d_name);
        if (remove(p) == 0) removed++;
    }
    if (d) closedir(d);
    LOG("[gamebuild] %d of %d pieces reused from cache/gamecode (%d old ones removed)\n", gc_reused, units - 1, removed);
}

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    char id[128];
    snprintf(id, sizeof id, "-DGAMELIB_ID=\"%s\"", gamelib_id());
    add(&a, id);
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of Savage Wind");
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    rt_remove_game_file(game_dir, ".gamebuild");
    rt_remove_game_file(game_dir, GAMELIB_NAME);
    rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    mkdir(work, 0755);
    if (t.zig) {  // its caches in the work folder too (deleted below), not ~/.cache: hundreds of MB, used once
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);
    LOG("[gamebuild] target%s: %s\n", gamelib_compat() ? " (compatible: no FMA instructions, slower)" : "",
        target_cflags());

    // 1. the modules' layout (tools/layout.py): which modules, at which addresses
    Gen gens[MAX_MODULES];
    double weight[MAX_MODULES], total = 0;  // (each module's share of the generator's progress: its size)
    int ngen = 0;
    char exefs[1100], layout[1100], layout_c[1100];
    snprintf(exefs, sizeof exefs, "%s" SEP "exefs", game_dir);
    snprintf(layout, sizeof layout, "%s" SEP "layout.txt", work);
    snprintf(layout_c, sizeof layout_c, "%s" SEP "layout.c", work);
    bool ok = true;
    {
        char log[1100];
        snprintf(log, sizeof log, "%s" SEP "gen_layout.log", work);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, "layout");
        add(&a, exefs);
        add(&a, work);
        PlatProc p = spawn(a.v, log);
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r == 0) ngen = read_layout(layout, gens, weight);
        if (!ngen) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] module layout failed: %s\n", last);
            snprintf(why, why_size, "%s",
                     p ? "the game files' code modules can't be loaded" : "the game code builder didn't start");
            ok = false;
        }
        for (int i = 0; i < ngen; i++) total += weight[i];
        for (int i = 0; i < ngen; i++) weight[i] /= total;
    }

    // 2. the generator, every module at once; their finished files are compiled while it goes on
    for (int i = 0; i < ngen && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "%s", exefs, g->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, layout);
        add(&a, g->out);
        if (strcmp(g->name, "main")) { add(&a, "--hle"); add(&a, t.hle); }
        add_split(&a, GAMELIB_GENARGS);
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    gc_begin(&t);
    Units units = {0};
    int queued[MAX_MODULES] = {0};  // chunk files of each module put in units so far
    bool tables[MAX_MODULES] = {false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
        add_unit(&units, layout_c, work, "layout");
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < ngen; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        log_gen_tail(g->log, r);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? weight[i] : g->n ? weight[i] * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
            if (next > 0 && gc_take(&units.u[next])) {  // (compiled by an earlier build: no compile)
                done_bytes += units.u[next].bytes;
                remove(units.u[next].src);
                next++;
                continue;
            }
            PlatProc p = start_compile(&t, &units.u[next]);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                log_gen_tail(u->log, r);  // (the compiler's errors: its last line is only "1 error generated", #123)
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            }
            if (r == 0 && run[k].unit > 0 && gc_seed) gc_store(u);
            done_bytes += u->bytes;
            if (run[k].unit > 0) remove(u->src);  // (generated C, done with: not kept. Unit 0 is the shipped entry file)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < ngen; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 3. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", gamelib_id()) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    gc_finish(ok, units.n);
    free(units.u);
    rt_remove_game_file(game_dir, ".gamebuild");  // the C, the objects, the logs
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
