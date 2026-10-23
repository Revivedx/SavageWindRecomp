// Shaders compiled before the game draws with them: every shader of the game files (runtime/nvn/shader_scan.c),
// translated and turned into SPIR-V into the caches the renderer reads (cache/xlate, cache/spirv: shaders.c), with
// the key a draw gives it. At first use a draw then costs a file read instead of a translation and a GLSL compile
// (9.4 ms a program on a Radeon 660M).
//
// The keys, after vk_translate_cached keeps only what a shader can depend on (maxwell_input_mask):
//  - every shader: GLSL; BOTW sets window origin mode 1 (upper left: no flip) at boot and never the depth mode
//    (0: [-1, 1]), as every draw then sees them;
//  - vertex: float attributes (the integer ones a draw binds make another variant, translated at first use);
//  - fragment: every varying it reads given by its vertex shader, float render targets;
//  - compute: the shared memory size from the control section, as compute_program reads it.
#include <SDL3/SDL.h>
#include <pthread.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "../shader_scan.h"
#include "precompile.h"
#include "shaders.h"

// Every shader is translated and compiled on one worker (the translator is re-entrant): no wait between the steps.
// The driver's compile is what takes memory: ~1.3 GB for the largest fragment shaders (41 KB of Maxwell code) on
// RADV, 1.8 GB at peak for 4 threads over a sample of the rest; 12 threads on the largest first filled a 14 GB
// laptop. So workers share a memory budget, estimated from each shader's size, and take the largest shader left
// when it fits, else the smallest: big shaders don't end up alone at the end, small ones fill the budget meanwhile.
typedef struct {
    const ScanShader *shaders;
    int *order;  // largest first
    int n, next_big, next_small;  // still to take: order[next_big .. next_small]
    bool check, driver;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int64_t used_mb;
    _Atomic int done, failed_translations, failed_compiles, failed_driver, undeclared;
    _Atomic uint64_t translate_ns, spirv_ns, driver_ns;  // CPU time of each step, all workers
    FILE *times;  // (SAVAGEWIND_PRECOMPILE_TIMES=<file>) a line per shader: stage, id, code bytes, GLSL bytes, SPIR-V
                  // words, translate / SPIR-V / driver microseconds, 1 if it branches through the pc dispatch loop, attr_int_use
    void (*progress)(int done, int total);
} Pool;

static _Atomic int64_t budget_mb;  // (precompile_memory_budget) 0: the default, a quarter of the RAM
static _Atomic bool cancelled;     // (precompile_stop) no more shaders are taken
static Pool *running;              // (lock: running_lock) to wake its workers when the budget grows
static pthread_mutex_t running_lock = PTHREAD_MUTEX_INITIALIZER;

void precompile_stop(bool stop) {
    cancelled = stop;
    precompile_memory_budget(budget_mb);  // (wakes workers waiting for memory)
}

void precompile_memory_budget(int64_t mb) {
    budget_mb = mb;
    pthread_mutex_lock(&running_lock);
    if (running) {
        pthread_mutex_lock(&running->lock);
        pthread_cond_broadcast(&running->cond);
        pthread_mutex_unlock(&running->lock);
    }
    pthread_mutex_unlock(&running_lock);
}

static int64_t memory_estimate_mb(const Pool *p, const ScanShader *s) { return p->driver ? 16 + s->size / 32 : 0; }

// The next shader for a worker and the memory it takes (give_back returns it); -1: none left.
static int take(Pool *p, int64_t *mb) {
    pthread_mutex_lock(&p->lock);
    int i = -1;
    while (p->next_big <= p->next_small && !cancelled) {
        int64_t budget = budget_mb > 0 ? budget_mb : (int64_t)SDL_GetSystemRAM() / 4;
        int64_t big = memory_estimate_mb(p, &p->shaders[p->order[p->next_big]]);
        int64_t small = memory_estimate_mb(p, &p->shaders[p->order[p->next_small]]);
        if (!p->used_mb || p->used_mb + big <= budget) { i = p->order[p->next_big++]; *mb = big; break; }
        if (p->used_mb + small <= budget) { i = p->order[p->next_small--]; *mb = small; break; }
        pthread_cond_wait(&p->cond, &p->lock);
    }
    if (i >= 0) p->used_mb += *mb;
    pthread_mutex_unlock(&p->lock);
    return i;
}

static void give_back(Pool *p, int64_t mb) {
    pthread_mutex_lock(&p->lock);
    p->used_mb -= mb;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

static void precompile_one(Pool *p, const ScanShader *s);

static void *worker(void *arg) {
    Pool *p = arg;
    // low priority: at setup the game code builds at the same time (runtime/ui/launcher.mm) and goes first
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    int64_t mb;
    for (int i; (i = take(p, &mb)) >= 0;) {
        precompile_one(p, &p->shaders[i]);
#ifdef __GLIBC__
        // The driver frees what a big compile took, but glibc keeps it in this thread's arena: 12 threads held 7.1 GB
        // at the end of a quarter of the shaders (2.7 GB at most when trimmed after the big ones; trimming after
        // every shader cost 11 % more time).
        if (mb >= 128) malloc_trim(0);
#endif
        give_back(p, mb);
        int d = atomic_fetch_add(&p->done, 1) + 1;
        if (p->progress && d % 64 == 0) p->progress(d, p->n);
    }
    return NULL;
}

static uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The key a draw (or dispatch) gives this shader, as the comment at the top says.
static ShaderKey precompile_key(const ScanShader *s) {
    ShaderKey k = {0};
    k.glsl = 1;
    if (s->stage == STAGE_COMPUTE) {
        k.shared_size = rd_u32(s->control + 0x754);
        return k;
    }
    k.flip_y = 0;
    k.depth_minus_one = 1;
    if (s->stage == STAGE_FRAGMENT) k.vs_out_mask = maxwell_input_mask(s->code, STAGE_FRAGMENT);
    return k;
}

// (SAVAGEWIND_PRECOMPILE_CHECK) inputs the code reads that the SPH doesn't declare: the canonical key (shaders.c)
// would then change what the shader reads. Fragment shaders are translated again with every varying given, as the
// canonical key hides the ones it drops.
static bool reads_undeclared(const ScanShader *s, const ShaderInfo *si, ShaderKey k, uint32_t max) {
    uint32_t read = si->attr_mask;
    if (s->stage == STAGE_FRAGMENT) {
        k.vs_out_mask = ~0u;
        ShaderInfo *raw = maxwell_translate(s->code, max, s->stage, &k);
        read = raw->attr_mask;
        maxwell_free(raw);
    }
    return read & ~maxwell_input_mask(s->code, s->stage);
}

// SPIR-V and the driver's part of one translation of s (t0, t1: when its translation started and ended).
static void compile_translation(Pool *p, const ScanShader *s, const ShaderInfo *si, uint64_t t0, uint64_t t1) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(si->msl, s->stage, "precompiled", &n);
    uint64_t t2 = SDL_GetTicksNS();
    atomic_fetch_add(&p->spirv_ns, t2 - t1);
    if (!spv) { atomic_fetch_add(&p->failed_compiles, 1); return; }
    if (p->driver) {
        bool ok;
        if (s->stage == STAGE_COMPUTE) {
            uint32_t local[3];
            for (int i = 0; i < 3; i++) {  // (as compute_program reads them)
                local[i] = rd_u32(s->control + 0x748 + 4 * i);
                if (!local[i]) local[i] = 1;
            }
            ok = vk_prebuild_compute(spv, n, local);
        } else {
            ok = vk_prebuild_stage(spv, n, s->stage);
        }
        if (!ok) atomic_fetch_add(&p->failed_driver, 1);
    }
    uint64_t t3 = SDL_GetTicksNS();
    atomic_fetch_add(&p->driver_ns, t3 - t2);
    if (p->times)
        fprintf(p->times, "%d %016llx %u %zu %zu %llu %llu %llu %d %08x\n", s->stage, (unsigned long long)s->id, s->size, strlen(si->msl), n,
                (unsigned long long)(t1 - t0) / 1000, (unsigned long long)(t2 - t1) / 1000, (unsigned long long)(t3 - t2) / 1000,
                strstr(si->msl, "switch (pc)") != NULL, si->attr_int_use);
    free(spv);
}

static const ShaderInfo *translate(Pool *p, const ScanShader *s, const ShaderKey *k, uint64_t *t0, uint64_t *t1) {
    *t0 = SDL_GetTicksNS();
    const ShaderInfo *si = vk_translate_cached(s->code, s->const_offset ? s->const_offset : s->size, s->stage, k);
    *t1 = SDL_GetTicksNS();
    atomic_fetch_add(&p->translate_ns, *t1 - *t0);
    return si;
}

static void precompile_one(Pool *p, const ScanShader *s) {
    ShaderKey k = precompile_key(s);
    uint64_t t0, t1;
    const ShaderInfo *si = translate(p, s, &k, &t0, &t1);
    if (!si->msl) {
        if (atomic_fetch_add(&p->failed_translations, 1) < 40)
            LOG("[precompile] %s %016llx: %s\n", s->stage == STAGE_VERTEX ? "vertex" : s->stage == STAGE_FRAGMENT ? "fragment" : "compute",
                (unsigned long long)s->id, si->error);
        return;
    }
    if (p->check && s->stage != STAGE_COMPUTE && reads_undeclared(s, si, k, s->const_offset ? s->const_offset : s->size))
        atomic_fetch_add(&p->undeclared, 1);
    uint32_t ints = s->stage == STAGE_VERTEX ? si->attr_int_use : 0;
    if (!ints) { compile_translation(p, s, si, t0, t1); return; }
    // Attributes the code only uses as integers: the draws bind integer formats there (uint, the kind BOTW binds),
    // so the float translation is not what they ask for. Two sets of bone indices (8-weight skinning): most draws
    // leave the second unbound, which the key gives as float. 220 of 220 vertex shaders of a session right.
    uint32_t sets[2] = {ints, ints & (0u - ints)};
    for (int v = 0; v < (sets[1] != sets[0] ? 2 : 1); v++) {
        ShaderKey kv = k;
        for (int a = 0; a < 32; a++) if ((sets[v] >> a) & 1) kv.attr_kind[a] = 2;
        const ShaderInfo *sv = translate(p, s, &kv, &t0, &t1);
        if (sv->msl) compile_translation(p, s, sv, t0, t1);
        else atomic_fetch_add(&p->failed_translations, 1);
    }
}

static const ScanShader *sort_base;
static int by_size_desc(const void *a, const void *b) {
    uint32_t x = sort_base[*(const int *)a].size, y = sort_base[*(const int *)b].size;
    return x < y ? 1 : x > y ? -1 : *(const int *)a - *(const int *)b;
}

bool precompile_shaders(const char *root, bool driver, void (*progress)(int done, int total), PrecompileStats *st,
                        char *why, size_t why_size) {
    memset(st, 0, sizeof *st);
    uint64_t t0 = SDL_GetTicksNS();
    char romfs[1100], main_nso[1100];
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(main_nso, sizeof main_nso, "%s/exefs/main", root);
    ScanResult r;
    if (!shader_scan(romfs, main_nso, &r, why, why_size)) return false;
    st->shaders = r.nshaders;
    st->scan_ms = (SDL_GetTicksNS() - t0) / 1000000;
    maxwell_gmem_base = HEAP_BASE;  // (as the renderer translates: global memory addresses relative to the heap)
    maxwell_gmem_size = HEAP_MAX;
    Pool p = {.shaders = r.shaders, .n = r.nshaders, .progress = progress, .driver = driver,
              .check = getenv("SAVAGEWIND_PRECOMPILE_CHECK") != NULL};
    pthread_mutex_init(&p.lock, NULL);
    pthread_cond_init(&p.cond, NULL);
    if (driver && !g_vk.dev) vk_offscreen_init();  // (the start screen has the game's device already)
    if (getenv("SAVAGEWIND_PRECOMPILE_TIMES") && (p.times = fopen(getenv("SAVAGEWIND_PRECOMPILE_TIMES"), "w")))
        setvbuf(p.times, NULL, _IOLBF, 0);
    if (getenv("SAVAGEWIND_PRECOMPILE_MEM_MB")) budget_mb = atoll(getenv("SAVAGEWIND_PRECOMPILE_MEM_MB"));
    p.order = malloc(sizeof *p.order * (size_t)r.nshaders);
    for (int i = 0; i < r.nshaders; i++) p.order[i] = i;
    sort_base = r.shaders;
    qsort(p.order, (size_t)r.nshaders, sizeof *p.order, by_size_desc);
    // (measurements) SAVAGEWIND_PRECOMPILE_SAMPLE=<n>: one shader in n, across all sizes
    int every = getenv("SAVAGEWIND_PRECOMPILE_SAMPLE") ? atoi(getenv("SAVAGEWIND_PRECOMPILE_SAMPLE")) : 1;
    if (every > 1) {
        int k = 0;
        for (int i = 0; i < p.n; i += every) p.order[k++] = p.order[i];
        p.n = k;
    }
    p.next_small = p.n - 1;
    pthread_mutex_lock(&running_lock);
    running = &p;
    pthread_mutex_unlock(&running_lock);
    uint64_t t1 = SDL_GetTicksNS();
    int nthreads = getenv("SAVAGEWIND_PRECOMPILE_THREADS") ? atoi(getenv("SAVAGEWIND_PRECOMPILE_THREADS")) : SDL_GetNumLogicalCPUCores();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    pthread_t th[64];
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, &p);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    pthread_mutex_lock(&running_lock);
    running = NULL;
    pthread_mutex_unlock(&running_lock);
    pthread_mutex_destroy(&p.lock);
    pthread_cond_destroy(&p.cond);
    if (driver) vk_pipeline_cache_save();
    if (p.times) fclose(p.times);
    st->compile_ms = (SDL_GetTicksNS() - t1) / 1000000;
    st->translate_cpu_ms = p.translate_ns / 1000000;
    st->spirv_cpu_ms = p.spirv_ns / 1000000;
    st->driver_cpu_ms = p.driver_ns / 1000000;
    st->failed_driver = p.failed_driver;
    st->failed_translations = p.failed_translations;
    st->failed_compiles = p.failed_compiles;
    st->undeclared_inputs = p.undeclared;
    st->shaders = p.n;
    st->compiled = p.done - p.failed_translations - p.failed_compiles - p.failed_driver;  // (fewer when stopped)
    st->threads = nthreads;
    free(p.order);
    shader_scan_free(&r);
    if (progress) progress(p.n, p.n);
    return true;
}
