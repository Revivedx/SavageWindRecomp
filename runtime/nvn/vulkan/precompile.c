// Shader precompilation (runtime/nvn/precompile.h). Every shader binary of the game's RomFS (shader_archive.c) is
// translated and compiled to SPIR-V with the keys the game will give it, by worker threads, into the caches the game
// reads (cache/xlate, cache/spirv); then pipelines are built for them into the pipeline cache (exec.c).
//
// Keys (vk_key_normalize: only what a shader uses of them):
//  - compute: the shared memory size from its control section; nothing else varies.
//  - fragment: which of its inputs the vertex shader provides - all of them (in BOTW's archives each program's vertex
//    shader writes exactly what its fragment shader reads) - and the kind of each color target it writes: float (no
//    integer targets met).
//  - vertex: the window origin and depth mode (game-wide), and the kind of each attribute it reads: float, int or
//    uint. That depends on the models' vertex formats; in BOTW it is the same for an attribute across one archive:
//    learned from the shader key log of this player's sessions (vk_keylog_read), else what the scripted test walks
//    met (kinds[] below), else float.
// Keys of the shader key log for the same binary are added as they are: what this player's game used.
//
// Motion vector variants (Video > Anti-aliasing TAA, FSR 3: vulkan/mvec.c; exec.c draw_ picks them): with motion
// vectors on, a pass with the internal-resolution scene depth gets the motion target appended after its color targets
// (slot = the pass's color target count, exec.c begin_pass), and in it
//  - a depth-writing draw (not full-screen) uses its program's motion variant: vertex key + mvec 1 and fragment key +
//    mvec_rt (slot + 1) when it has a match in the previous frame, else the vertex key as is and fragment mvec_rt
//    0x40 | (slot + 1) (still: zero motion);
//  - a blended draw that writes no depth: fragment mvec_rt 0x80 | (slot + 1) (the reactive mask), vertex key as is.
// The keys are otherwise those of the draw without motion vectors (in the key log of 2026-10-28 all 522 motion keys
// were a logged key plus these fields). Which shaders: BOTW's motion passes are the G-buffer (6 color targets: slot 6;
// its fragment shaders write targets 0, 1 and 3, and 5 for some, never 6 or 7) and the forward pass after the lighting
// (1 target: slot 1; effects and materials blended into it write target 0 only). So, for every fragment key
// (predicted or logged) of a G-buffer shader, mvec_rt 7 and 0x47, and mvec 1 for the vertex keys of the vertex
// shaders of its programs; for every fragment key of a target-0-only shader of the materials or of an effect, the
// reactive mask 0x82 (blended draws of other archives - post effects, lights, shadows - are full-screen or not in
// those passes). Logged motion keys are taken as they are. Their libraries are built like the others' (a motion
// vertex shader's pipeline layout has the push constants: stage_layouts, from the translation's ShaderInfo.mvec).
// SAVAGEWIND_PRECOMPILE_NO_MVEC=1: none of them.
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <zlib.h>

#include "../../kernel.h"
#include "../../paths.h"
#include "../precompile.h"
#include "../shader_archive.h"
#include "shaders.h"
#include "vk.h"

bool shader_precompile_available(void) { return true; }

// Vertex attributes that are integers, by archive (the rest are float): what the scripted test walks met.
static const struct { const char *archive; int attr, kind; } kinds[] = {
    {"/uking_mat.product.sbfsha", 6, 2},    // uint (the bone indices of skinned models)
    {"/uking_grass.product.sbfsha", 2, 4},  // uint read as float
};

typedef struct { int shader; ShaderKey key; bool mvec; } Job;  // (mvec: a motion vector variant made here)
typedef struct { int vs, fs, cs; } Pair;  // (jobs: the predicted key's; vs or fs alone: that shader's libraries)

// (pipe_watch) a pipelines thread: the pair it compiles (-1: none), since when, and whether it has returned
typedef struct { struct Work *w; _Atomic int pair; _Atomic uint64_t since; _Atomic bool out; } PipeTh;

typedef struct Work {
    ShaderPrecompile *p;
    SaSet *set;
    Job *jobs;
    int njobs;
    Pair *pairs;
    int npairs;
    _Atomic int next, made, cached, failed, built, mvec_failed;
    // the pipelines step: the driver's compiles share a memory budget (pipe_take)
    int *order;                   // pairs, largest shader first
    int64_t *need_mb;             // per pair: what its compile is estimated to take
    int next_big, next_small;     // still to take: order[next_big .. next_small]
    int64_t used_mb, budget_mb;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    PipeTh th[32];
} Work;

static void *worker(void *arg) {
    Work *w = arg;
    maxwell_gmem_base = HEAP_BASE;  // (as the game translates: global memory relative to the guest heap)
    maxwell_gmem_size = HEAP_MAX;
    for (int i; !atomic_load(&w->p->cancel) && (i = atomic_fetch_add(&w->next, 1)) < w->njobs;) {
        const Job *j = &w->jobs[i];
        const SaShader *s = &w->set->shaders[j->shader];
        char why[300] = "";
        int r = vk_precompile_one(s->code, s->max_size, s->stage, &j->key, why, sizeof why);
        // (a motion variant the translator refuses - varyings 30 / 31 or the motion target's slot in use - is one the
        // game cannot draw either: not a failure)
        if (r < 0 && j->mvec) atomic_fetch_add(&w->mvec_failed, 1);
        else if (r < 0 && atomic_fetch_add(&w->failed, 1) < 40)
            LOG("[precompile] %s shader %016llx of %s: %s\n",
                s->stage == STAGE_VERTEX ? "vertex" : s->stage == STAGE_FRAGMENT ? "fragment" : "compute",
                (unsigned long long)s->hash, w->set->sources[s->source], why);
        else if (r >= 0) atomic_fetch_add(r > 0 ? &w->made : &w->cached, 1);
        atomic_fetch_add(&w->p->done, 1);
    }
    return NULL;
}

// Memory: RADV takes up to ~1.3 GB to compile the largest fragment shaders (41 KB of Maxwell code), and a thread per
// core on the largest at once went past 9.5 GB on a 14 GB laptop (Radeon 660M, 12 threads; at 7 GB the kernel killed
// it). So each compile reserves an estimate from a budget (a quarter of the RAM; SAVAGEWIND_PRECOMPILE_MEM_MB) and a
// thread takes the largest pair left when it fits, else the smallest: the large ones don't end up alone at the end.
// glibc keeps what the driver frees in each thread's arena (7.1 GB at the end of a quarter of the shaders, 2.7 GB
// trimmed after the large ones): malloc_trim after those.
static int64_t pair_need_mb(const Work *w, const Pair *pr) {
    uint32_t bytes = 0;
    int ids[3] = {pr->vs, pr->fs, pr->cs};
    for (int k = 0; k < 3; k++)
        if (ids[k] >= 0 && w->set->shaders[w->jobs[ids[k]].shader].size > bytes) bytes = w->set->shaders[w->jobs[ids[k]].shader].size;
    return 16 + bytes / 32;
}

static int pipe_take(Work *w, int64_t *mb) {
    pthread_mutex_lock(&w->lock);
    int i = -1;
    while (w->next_big <= w->next_small && !atomic_load(&w->p->cancel)) {
        int64_t big = w->need_mb[w->order[w->next_big]], small = w->need_mb[w->order[w->next_small]];
        if (!w->used_mb || w->used_mb + big <= w->budget_mb) { i = w->order[w->next_big++]; *mb = big; break; }
        if (w->used_mb + small <= w->budget_mb) { i = w->order[w->next_small--]; *mb = small; break; }
        pthread_cond_wait(&w->cond, &w->lock);
    }
    if (i >= 0) w->used_mb += *mb;
    pthread_mutex_unlock(&w->lock);
    return i;
}

static void pipe_give_back(Work *w, int64_t mb) {
    pthread_mutex_lock(&w->lock);
    w->used_mb -= mb;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static const int64_t *sort_need;
static int by_need_desc(const void *a, const void *b) {
    int64_t x = sort_need[*(const int *)a], y = sort_need[*(const int *)b];
    return x < y ? 1 : x > y ? -1 : *(const int *)a - *(const int *)b;
}

static void *pipe_worker(void *arg) {
    PipeTh *t = arg;
    Work *w = t->w;
    int64_t mb;
    for (int i; (i = pipe_take(w, &mb)) >= 0;) {
        atomic_store(&t->since, SDL_GetTicksNS());
        atomic_store(&t->pair, i);
        const Pair *pr = &w->pairs[i];
        bool ok = false;
        if (pr->cs >= 0) {
            const Job *c = &w->jobs[pr->cs];
            const SaShader *s = &w->set->shaders[c->shader];
            ShaderInfo *si = vk_xlate_read(s->code, s->max_size, s->stage, &c->key);
            if (si) ok = vk_pso_warm_compute(si, s->local);
            maxwell_free(si);
        } else if (pr->vs < 0 || pr->fs < 0) {  // (graphics pipeline libraries: a shader alone)
            const Job *j = &w->jobs[pr->vs >= 0 ? pr->vs : pr->fs];
            const SaShader *sh = &w->set->shaders[j->shader];
            ShaderInfo *si = vk_xlate_read(sh->code, sh->max_size, sh->stage, &j->key);
            if (si) ok = vk_pso_warm_stage(si, sh->stage);
            maxwell_free(si);
        } else {
            const Job *v = &w->jobs[pr->vs], *f = &w->jobs[pr->fs];
            const SaShader *vs = &w->set->shaders[v->shader], *fs = &w->set->shaders[f->shader];
            ShaderInfo *vi = vk_xlate_read(vs->code, vs->max_size, vs->stage, &v->key);
            ShaderInfo *fi = vi ? vk_xlate_read(fs->code, fs->max_size, fs->stage, &f->key) : NULL;
            if (vi && fi) ok = vk_pso_warm(vi, fi, v->key.attr_kind);
            maxwell_free(vi);
            maxwell_free(fi);
        }
        if (ok) atomic_fetch_add(&w->built, 1);
#ifdef __GLIBC__
        if (mb >= 128) malloc_trim(0);
#endif
        atomic_store(&t->pair, -1);
        pipe_give_back(w, mb);
        atomic_fetch_add(&w->p->done, 1);
    }
    atomic_store(&t->out, true);
    return NULL;
}

// The pipelines threads, waited for. A driver can take a compile and never give it back: on AMD's Windows driver, with
// its own shader compiler, after fragment shaders went through spirv-opt (0.5.2, the sky fix), setup stopped at
// 21502 of 21503 for good (#359), and Esc could not stop it either - it waited for that thread. So when no compile has
// finished for PIPE_STALL_S seconds, or setup is stopped, the compiles still running are logged (their shaders) and
// left to the driver: their threads are detached and the work they can still touch (w) is kept, never freed. Returns
// whether any was left. SAVAGEWIND_PRECOMPILE_STALL_S=<seconds> (180).
#define PIPE_STALL_S 180
static bool pipe_watch(Work *w, pthread_t *th, int nt) {
    static int stall_s = -1;
    if (stall_s < 0) { const char *e = getenv("SAVAGEWIND_PRECOMPILE_STALL_S"); stall_s = e && atoi(e) > 0 ? atoi(e) : PIPE_STALL_S; }
    int last = atomic_load(&w->p->done);
    uint64_t last_t = SDL_GetTicksNS();
    for (;;) {
        int out = 0;
        for (int i = 0; i < nt; i++) out += atomic_load(&w->th[i].out);
        if (out == nt) break;
        SDL_Delay(100);
        uint64_t now = SDL_GetTicksNS();
        int d = atomic_load(&w->p->done);
        if (d != last) last = d, last_t = now;
        bool cancel = atomic_load(&w->p->cancel);
        if (!cancel && now - last_t < (uint64_t)stall_s * 1000000000ull) continue;
        pthread_mutex_lock(&w->lock);  // (nothing more taken: a thread that comes back leaves)
        w->next_big = 1, w->next_small = 0;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
        for (int k = 0; k < (cancel ? 100 : 20); k++) {  // (2 s for the ones between compiles; stopped, 10 s to finish theirs)
            out = 0;
            for (int i = 0; i < nt; i++) out += atomic_load(&w->th[i].out);
            if (out == nt) break;
            SDL_Delay(100);
        }
        bool left = false;
        for (int i = 0; i < nt; i++) {
            if (atomic_load(&w->th[i].out)) { pthread_join(th[i], NULL); continue; }
            int pi = atomic_load(&w->th[i].pair);
            uint64_t hv = 0, hf = 0, hc = 0;
            if (pi >= 0) {
                const Pair *pr = &w->pairs[pi];
                if (pr->vs >= 0) hv = w->set->shaders[w->jobs[pr->vs].shader].hash;
                if (pr->fs >= 0) hf = w->set->shaders[w->jobs[pr->fs].shader].hash;
                if (pr->cs >= 0) hc = w->set->shaders[w->jobs[pr->cs].shader].hash;
            }
            LOG("[precompile] a pipeline compile the driver has not given back for %.0f s, left to it (%s): vertex %016llx "
                "fragment %016llx compute %016llx\n", (now - atomic_load(&w->th[i].since)) / 1e9, cancel ? "setup stopped" : "stalled",
                (unsigned long long)hv, (unsigned long long)hf, (unsigned long long)hc);
            pthread_detach(th[i]);
            left = true;
        }
        return left;
    }
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    return false;
}

static int pair_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(Pair)); }

static void scan_progress(float f, void *user) {
    ShaderPrecompile *p = user;
    atomic_store(&p->done, (int)(f * 1000));
}

float shader_precompile_progress(const ShaderPrecompile *p) {
    // reading the archives 2%, translating and compiling 33%, pipelines 65% (a first start: ~45 s and ~2 minutes)
    int st = atomic_load(&p->state), d = atomic_load(&p->done), t = atomic_load(&p->total);
    float f = t > 0 ? (float)d / (float)t : 0;
    if (f > 1) f = 1;
    switch (st) {
    case SP_READING: return 0.02f * f;
    case SP_COMPILING: return 0.02f + 0.33f * f;
    case SP_PIPELINES: return 0.35f + 0.65f * f;
    case SP_DONE: return 1;
    default: return 0;
    }
}

// The jobs: shader and key, each pair once (a hash set of them: the key log repeats the predicted keys)
typedef struct { Job *jobs; int n, cap; uint64_t *set; size_t set_cap, set_n; } Jobs;

static uint64_t job_hash(int shader, const ShaderKey *k) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)shader;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h ? h : 1;
}

static void add_job(Jobs *J, int shader, const ShaderKey *k, bool mvec) {
    uint64_t h = job_hash(shader, k);
    if ((J->set_n + 1) * 2 > J->set_cap) {
        size_t cap = J->set_cap ? J->set_cap * 2 : 1 << 15;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < J->set_cap; i++) {
            if (!J->set[i]) continue;
            size_t j = J->set[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = J->set[i];
        }
        free(J->set);
        J->set = t;
        J->set_cap = cap;
    }
    size_t j = h & (J->set_cap - 1);
    for (; J->set[j]; j = (j + 1) & (J->set_cap - 1))
        if (J->set[j] == h) return;  // (that one is there already)
    J->set[j] = h;
    J->set_n++;
    Job **jobs = &J->jobs;
    int *n = &J->n, *cap = &J->cap;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4096;
        *jobs = realloc(*jobs, sizeof **jobs * (size_t)*cap);
    }
    (*jobs)[*n].shader = shader;
    (*jobs)[*n].key = *k;
    (*jobs)[*n].mvec = mvec;
    (*n)++;
}

// The motion vector variants (see the top): added to J for its keys so far; returns how many.
enum { MVEC_GBUFFER_SLOT = 6, MVEC_FORWARD_SLOT = 1 };  // (BOTW's motion passes: the G-buffer, the forward pass)
static uint32_t fs_omap(const uint8_t *code) {
    uint32_t omap;
    memcpy(&omap, code + 0x30 + 0x48, 4);  // (the SPH's 4 bits per color target, as vk_key_normalize reads it)
    return omap;
}
static bool gbuffer_fs(const uint8_t *code) {
    uint32_t o = fs_omap(code);
    return (o & 0xF0FF) == 0xF0FF && !(o >> 24);  // (targets 0, 1 and 3; nothing at 6 or 7)
}
static bool mask_source(const char *src) {  // (the materials' archive and the effects: what draws blended into the scene)
    size_t n = strlen(src), m = strlen("/uking_mat.product.sbfsha");
    return strstr(src, ".sesetlist") || (n >= m && !strcmp(src + n - m, "/uking_mat.product.sbfsha"));
}

static int add_mvec_jobs(Jobs *J, const SaSet *set) {
    bool *gb_vs = calloc((size_t)(set->nshaders ? set->nshaders : 1), 1);  // vertex shaders of G-buffer programs
    for (int i = 0; i < set->nprograms; i++) {
        const SaProgram *g = &set->programs[i];
        if (g->cs < 0 && g->vs >= 0 && g->fs >= 0 && gbuffer_fs(set->shaders[g->fs].code)) gb_vs[g->vs] = true;
    }
    int n0 = J->n;
    for (int i = 0; i < n0; i++) {
        Job j = J->jobs[i];  // (a copy: add_job may move J->jobs)
        const SaShader *s = &set->shaders[j.shader];
        if (s->stage == STAGE_VERTEX && !j.key.mvec && gb_vs[j.shader]) {
            j.key.mvec = 1;
            add_job(J, j.shader, &j.key, true);
        } else if (s->stage == STAGE_FRAGMENT && !j.key.mvec_rt) {
            if (gbuffer_fs(s->code)) {
                j.key.mvec_rt = MVEC_GBUFFER_SLOT + 1;
                add_job(J, j.shader, &j.key, true);
                j.key.mvec_rt = 0x40 | (MVEC_GBUFFER_SLOT + 1);
                add_job(J, j.shader, &j.key, true);
            } else if (fs_omap(s->code) == 0xF && mask_source(set->sources[s->source])) {
                j.key.mvec_rt = 0x80 | (MVEC_FORWARD_SLOT + 1);
                add_job(J, j.shader, &j.key, true);
            }
        }
    }
    free(gb_vs);
    return J->n - n0;
}

// ---- the pipeline seed: vk_pipelines.seed, the pipeline list of a playthrough in a form any install can use.
// The list (exec.c) names shaders by translation id, which depends on the translator build and is unknown until the
// shader is translated here; the seed names them by the hash of their binary (shader_code_hash, as the shader key
// log does) and the key, with the pipeline's fixed state as the list holds it. `savagewind --export-pipeline-seed
// <file>` writes one from this install's list (a developer's, after playing through the game); the precompilation
// reads the one in the data root or next to the executable (a shipped file): its keys join the translations to make,
// and at the end its records join the list, so a fresh install's first boot pre-warms every pipeline the playthrough
// met instead of linking them where first drawn. A seed of another key or state layout (another build of those
// structures) is ignored. Nothing of the game is in it: hashes, keys and render state.
// File: SeedHeader, then the records deflated (zlib): per record kind (1 graphics, 2 compute), two binary hashes,
// two keys and the state (state_size bytes; compute: the group size in the first 12).
typedef struct { char magic[8]; uint32_t version, nrecs, key_size, state_size, zsize, reserved; } SeedHeader;
#define SEED_MAGIC "SWPSEED1"
#define SEED_NAME "vk_pipelines.seed"

static size_t seed_rec_size(void) { return 4 + 16 + 2 * sizeof(ShaderKey) + vk_pso_state_size(); }

// The data root's, else the shipped one: next to the executable in a release (Makefile dist, tools/linux/package.sh),
// runtime/nvn/vulkan/vk_pipelines.seed in a source checkout.
static bool seed_find(char *path, size_t n) {
    snprintf(path, n, "%s/%s", rt_data_root(), SEED_NAME);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return rt_resource(SEED_NAME, "runtime/nvn/vulkan/" SEED_NAME, path, n);
}

// The seed's records (malloc'd, seed_rec_size each) or NULL; *why says what is wrong with the file
static uint8_t *seed_read(const char *path, int *nrecs, const char **why) {
    *nrecs = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "no file"; return NULL; }
    SeedHeader h;
    uint8_t *z = NULL, *d = NULL;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, SEED_MAGIC, 8) && h.version == 1;
    if (!ok) *why = "not a pipeline seed of this format";
    else if (h.key_size != sizeof(ShaderKey) || h.state_size != vk_pso_state_size()) { ok = false; *why = "made by another build (key or state layout)"; }
    else if (h.nrecs > (1u << 20) || h.zsize > (256u << 20)) { ok = false; *why = "damaged"; }
    if (ok) {
        size_t n = (size_t)h.nrecs * seed_rec_size();
        z = malloc(h.zsize ? h.zsize : 1);
        d = malloc(n ? n : 1);
        uLongf len = (uLongf)n;
        ok = z && d && fread(z, 1, h.zsize, f) == h.zsize && uncompress(d, &len, z, h.zsize) == Z_OK && len == n;
        if (!ok) *why = "damaged";
    }
    fclose(f);
    free(z);
    if (!ok) { free(d); return NULL; }
    *nrecs = (int)h.nrecs;
    return d;
}

static void seed_rec(const uint8_t *r, uint32_t *kind, uint64_t code[2], ShaderKey key[2], const void **state) {
    memcpy(kind, r, 4);
    memcpy(code, r + 4, 16);
    memcpy(key, r + 20, 2 * sizeof(ShaderKey));
    *state = r + 20 + 2 * sizeof(ShaderKey);
}

// The seed's keys as translations to make (before the jobs are counted)
static int seed_add_jobs(Jobs *J, const SaSet *set, const uint8_t *recs, int nrecs) {
    int n0 = J->n;
    for (int i = 0; i < nrecs; i++) {
        uint32_t kind;
        uint64_t code[2];
        ShaderKey key[2];
        const void *state;
        seed_rec(recs + (size_t)i * seed_rec_size(), &kind, code, key, &state);
        for (int k = 0; k < (kind == 1 ? 2 : 1); k++) {
            int sh = shader_archive_find(set, code[k]);
            int stage = kind == 1 ? (k == 0 ? STAGE_VERTEX : STAGE_FRAGMENT) : STAGE_COMPUTE;
            if (sh >= 0 && set->shaders[sh].stage == stage) add_job(J, sh, &key[k], key[k].mvec || key[k].mvec_rt);
        }
    }
    return J->n - n0;
}

// The seed's records into the pipeline list, by the ids their translations have with this translator
static int seed_import(const SaSet *set, const uint8_t *recs, int nrecs) {
    int added = 0;
    for (int i = 0; i < nrecs; i++) {
        uint32_t kind;
        uint64_t code[2], id[2] = {0, 0};
        ShaderKey key[2];
        const void *state;
        seed_rec(recs + (size_t)i * seed_rec_size(), &kind, code, key, &state);
        for (int k = 0; k < (kind == 1 ? 2 : 1); k++) {
            int sh = shader_archive_find(set, code[k]);
            int stage = kind == 1 ? (k == 0 ? STAGE_VERTEX : STAGE_FRAGMENT) : STAGE_COMPUTE;
            if (sh >= 0 && set->shaders[sh].stage == stage)
                id[k] = vk_translate_id_for(set->shaders[sh].code, set->shaders[sh].max_size, stage, &key[k]);
        }
        if (id[0] && (kind != 1 || id[1]) && vk_pso_add((int)kind, id[0], id[1], state, kind == 1 ? vk_pso_state_size() : 12)) added++;
    }
    vk_pso_flush();
    return added;
}

// --export-pipeline-seed: this install's list as a seed. The translation ids are matched to binaries and keys
// through the shader key log (every translation the game made is in it) and the archives.
typedef struct { uint64_t id, code; ShaderKey key; } IdMap;
typedef struct { IdMap *m; size_t cap, n; uint8_t *out; size_t nout, cap_out; int missing; } Export;

static void idmap_put(Export *e, uint64_t id, uint64_t code, const ShaderKey *key) {
    if ((e->n + 1) * 2 > e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 1 << 16;
        IdMap *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < e->cap; i++) {
            if (!e->m[i].id) continue;
            size_t j = e->m[i].id & (cap - 1);
            while (t[j].id) j = (j + 1) & (cap - 1);
            t[j] = e->m[i];
        }
        free(e->m);
        e->m = t;
        e->cap = cap;
    }
    size_t j = id & (e->cap - 1);
    while (e->m[j].id && e->m[j].id != id) j = (j + 1) & (e->cap - 1);
    if (!e->m[j].id) e->n++;
    e->m[j] = (IdMap){id, code, *key};
}

static const IdMap *idmap_get(const Export *e, uint64_t id) {
    if (!e->cap) return NULL;
    size_t j = id & (e->cap - 1);
    while (e->m[j].id && e->m[j].id != id) j = (j + 1) & (e->cap - 1);
    return e->m[j].id ? &e->m[j] : NULL;
}

static int export_rec(void *ctx, int kind, const uint64_t id[2], const void *state, size_t n) {
    Export *e = ctx;
    const IdMap *m[2] = {idmap_get(e, id[0]), kind == 1 ? idmap_get(e, id[1]) : NULL};
    if (!m[0] || (kind == 1 && !m[1])) { e->missing++; return 0; }
    size_t rs = seed_rec_size();
    if (e->nout + rs > e->cap_out) {
        e->cap_out = e->cap_out ? e->cap_out * 2 : rs * 1024;
        e->out = realloc(e->out, e->cap_out);
    }
    uint8_t *r = e->out + e->nout;
    memset(r, 0, rs);
    uint32_t k = (uint32_t)kind;
    memcpy(r, &k, 4);
    uint64_t code[2] = {m[0]->code, m[1] ? m[1]->code : 0};
    memcpy(r + 4, code, 16);
    memcpy(r + 20, &m[0]->key, sizeof(ShaderKey));
    if (m[1]) memcpy(r + 20 + sizeof(ShaderKey), &m[1]->key, sizeof(ShaderKey));
    memcpy(r + 20 + 2 * sizeof(ShaderKey), state, n < vk_pso_state_size() ? n : vk_pso_state_size());
    e->nout += rs;
    return 1;
}

bool shader_precompile_export_seed(const char *romfs, const char *out, char *why, size_t nwhy) {
    SaSet set;
    if (!shader_archive_scan(romfs, &set, NULL, NULL, why, nwhy)) return false;
    VkKeyLogEntry *log;
    int nlog = vk_keylog_read(&log);
    Export e = {0};
    for (int i = 0; i < nlog; i++) {
        int k = shader_archive_find(&set, log[i].code);
        if (k < 0 || (int)log[i].stage != set.shaders[k].stage) continue;
        uint64_t id = vk_translate_id_for(set.shaders[k].code, set.shaders[k].max_size, set.shaders[k].stage, &log[i].key);
        idmap_put(&e, id, set.shaders[k].hash, &log[i].key);
    }
    int n = vk_pso_records(export_rec, &e);
    bool ok = false;
    uLongf zn = compressBound((uLong)e.nout);
    uint8_t *z = malloc(zn ? zn : 1);
    if (z && compress2(z, &zn, e.out ? e.out : (const uint8_t *)"", (uLong)e.nout, 9) == Z_OK) {
        SeedHeader h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, SEED_MAGIC, 8);
        h.version = 1;
        h.nrecs = (uint32_t)n;
        h.key_size = sizeof(ShaderKey);
        h.state_size = (uint32_t)vk_pso_state_size();
        h.zsize = (uint32_t)zn;
        FILE *f = fopen(out, "wb");
        ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(z, 1, zn, f) == zn;
        if (f && fclose(f)) ok = false;
        if (!ok) snprintf(why, nwhy, "cannot write %s", out);
    } else {
        snprintf(why, nwhy, "deflate failed");
    }
    LOG("[precompile] pipeline seed: %d of the list's pipelines written to %s (%d whose shaders the key log does not name; "
        "%d keys known), %.1f KB\n", n, out, e.missing, (int)e.n, zn / 1024.0);
    free(z);
    free(e.out);
    free(e.m);
    free(log);
    shader_archive_free(&set);
    return ok;
}

static void status_path(char *out, size_t n) { vk_cache_path(out, n, "precompiled.txt"); }

// cache/precompiled.txt: the shader version with the device's translation variant (vk_shader_version_full), then
// "<shaders> <YYYY-MM-DD>". Another variant (the global memory switches changed with a driver, a device or a new
// default) means the global memory shaders' translations and libraries are missing: offered again, which only redoes
// those (the rest are cache hits).
int shader_precompile_status(int *shaders, char *date, int n) {
    char path[512], version[128] = "", day[32] = "";
    int count = 0;
    status_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(version, sizeof version, f) || fscanf(f, "%d %31s", &count, day) != 2) version[0] = 0;
    fclose(f);
    version[strcspn(version, "\r\n")] = 0;
    if (!version[0]) return 0;
    if (shaders) *shaders = count;
    if (date && n > 0) snprintf(date, (size_t)n, "%s", day);
    // (the shaders' libraries go with the GPU and driver: a new driver drops them)
    return strcmp(version, vk_shader_version_full()) || !vk_precompiled_cache_ok() ? 2 : 1;
}

bool shader_precompile_run(ShaderPrecompile *p, const char *romfs) {
    uint64_t t0 = SDL_GetTicksNS();
    atomic_store(&p->done, 0);
    atomic_store(&p->total, 1000);
    atomic_store(&p->state, SP_READING);
    SaSet set;
    if (!shader_archive_scan(romfs, &set, scan_progress, p, p->why, sizeof p->why)) {
        atomic_store(&p->state, SP_FAILED);
        return false;
    }
    // what the shader key log says: keys by binary, and attribute kinds by archive
    VkKeyLogEntry *log;
    int nlog = vk_keylog_read(&log);
    int (*seen)[32][5] = calloc((size_t)set.nsources, sizeof *seen);
    int origin[2] = {0}, depth[2] = {0};
    for (int i = 0; i < nlog; i++) {
        if (log[i].stage != STAGE_VERTEX) continue;
        origin[log[i].key.flip_y]++;
        depth[log[i].key.depth_minus_one]++;
        int k = shader_archive_find(&set, log[i].code);
        if (k < 0) continue;
        uint32_t in = vk_sph_inputs(set.shaders[k].code, STAGE_VERTEX);
        for (int a = 0; a < 32; a++)
            if ((in >> a) & 1 && log[i].key.attr_kind[a] < 5) seen[set.shaders[k].source][a][log[i].key.attr_kind[a]]++;
    }
    bool flip_y = origin[1] > origin[0], depth_minus_one = depth[1] >= depth[0];  // (BOTW: 0 and 1)
    // the jobs: per shader its predicted key, then the logged ones
    Jobs J = {0};
    for (int i = 0; i < set.nshaders; i++) {
        const SaShader *s = &set.shaders[i];
        ShaderKey k;
        memset(&k, 0, sizeof k);
        k.glsl = 1;
        if (s->stage == STAGE_VERTEX) {
            k.flip_y = flip_y;
            k.depth_minus_one = depth_minus_one;
            uint32_t in = vk_sph_inputs(s->code, STAGE_VERTEX);
            const char *src = set.sources[s->source];
            for (int a = 0; a < 32; a++) {
                if (!((in >> a) & 1)) continue;
                int best = -1, most = 0;
                for (int c = 0; c < 5; c++)
                    if (seen[s->source][a][c] > most) most = seen[s->source][a][c], best = c;
                if (best < 0) {
                    best = 0;
                    for (size_t t = 0; t < sizeof kinds / sizeof kinds[0]; t++) {
                        size_t ls = strlen(src), lt = strlen(kinds[t].archive);
                        if (kinds[t].attr == a && ls >= lt && !strcmp(src + ls - lt, kinds[t].archive)) best = kinds[t].kind;
                    }
                }
                k.attr_kind[a] = (uint8_t)best;
            }
        } else if (s->stage == STAGE_FRAGMENT) {
            k.vs_out_mask = vk_sph_inputs(s->code, STAGE_FRAGMENT);
        } else {
            k.shared_size = s->shared;
        }
        vk_key_normalize(s->code, s->stage, &k);
        add_job(&J, i, &k, false);
    }
    for (int i = 0; i < nlog; i++) {
        int k = shader_archive_find(&set, log[i].code);
        if (k >= 0 && (int)log[i].stage == set.shaders[k].stage) add_job(&J, k, &log[i].key, false);
    }
    char seed_path[1100];
    const char *seed_why = "no file";
    int nseed = 0, nseed_keys = 0;
    uint8_t *seed = seed_find(seed_path, sizeof seed_path) ? seed_read(seed_path, &nseed, &seed_why) : NULL;
    if (seed) nseed_keys = seed_add_jobs(&J, &set, seed, nseed);
    int nmvec = getenv("SAVAGEWIND_PRECOMPILE_NO_MVEC") ? 0 : add_mvec_jobs(&J, &set);
    Job *jobs = J.jobs;
    int njobs = J.n;
    free(J.set);
    free(seen);
    free(log);
    LOG("[precompile] %d shaders in %d archives (%llu files, %.0f MB) read in %.1f s: %d translations to make (%d of them "
        "motion vector variants)\n", set.nshaders, set.nsources, (unsigned long long)set.files, set.bytes / 1048576.0,
        (SDL_GetTicksNS() - t0) / 1e9, njobs, nmvec);
    if (seed) LOG("[precompile] pipeline seed %s: %d pipelines, %d keys of its own\n", seed_path, nseed, nseed_keys);
    else LOG("[precompile] pipeline seed: none (%s: %s)\n", seed_path, seed_why);
    // translate and compile them
    Work w = {.p = p, .set = &set, .jobs = jobs, .njobs = njobs};
    atomic_store(&p->done, 0);
    atomic_store(&p->total, njobs);
    atomic_store(&p->state, SP_COMPILING);
    int nt = SDL_GetNumLogicalCPUCores();
    if (nt < 1) nt = 1;
    if (nt > 32) nt = 32;
    pthread_t th[32];
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &w);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    vk_keylog_flush();
    LOG("[precompile] %d translations in %.1f s on %d threads: %d made, %d were cached, %d failed, %d motion variants the "
        "translator refuses (not drawn either)\n", njobs, (SDL_GetTicksNS() - t0) / 1e9, nt, atomic_load(&w.made),
        atomic_load(&w.cached), atomic_load(&w.failed), atomic_load(&w.mvec_failed));
    // into the pipeline cache: the compute pipelines (their whole state is known: the group size); with graphics
    // pipeline libraries (exec.c), every vertex and fragment shader's libraries, into a cache of their own
    // (cache/vk_precompiled.bin, device.c). Without them, whole pipelines only
    // with SAVAGEWIND_PRECOMPILE_PIPELINES=1, one per program: those take ~0.85 GB of pipeline cache and help less
    // (in a walk where the driver had seen nothing: 12.7 -> 8.4 s of compiles on the render thread), since a later
    // draw with other blending, target formats or write masks costs the driver much of a compile again.
    bool graphics = !g_vk.gpl && getenv("SAVAGEWIND_PRECOMPILE_PIPELINES") != NULL;
    if (g_vk.dev && !atomic_load(&p->cancel) && !getenv("SAVAGEWIND_PRECOMPILE_NO_PIPELINES")) {
        int *first = malloc(sizeof *first * (size_t)(set.nshaders ? set.nshaders : 1));  // shader -> its predicted job
        for (int i = 0; i < set.nshaders; i++) first[i] = -1;
        for (int i = 0; i < njobs; i++)
            if (first[jobs[i].shader] < 0) first[jobs[i].shader] = i;
        Pair *pairs = malloc(sizeof *pairs * (size_t)(set.nprograms ? set.nprograms : 1));
        int np = 0;
        for (int i = 0; i < set.nprograms; i++) {
            const SaProgram *g = &set.programs[i];
            Pair pr = {-1, -1, -1};
            if (g->cs >= 0) pr.cs = first[g->cs];
            else pr.vs = first[g->vs], pr.fs = first[g->fs];
            if ((g->cs >= 0 && pr.cs >= 0) || (graphics && g->cs < 0 && pr.vs >= 0 && pr.fs >= 0)) pairs[np++] = pr;
        }
        if (g_vk.gpl) {  // (every key's: the predicted, the logged ones that differ, the motion vector variants)
            pairs = realloc(pairs, sizeof *pairs * (size_t)(np + njobs + 1));
            for (int i = 0; i < njobs; i++) {
                int stage = set.shaders[jobs[i].shader].stage;
                if (stage == STAGE_COMPUTE) continue;
                pairs[np++] = stage == STAGE_VERTEX ? (Pair){i, -1, -1} : (Pair){-1, i, -1};
            }
        }
        qsort(pairs, (size_t)np, sizeof *pairs, pair_cmp);
        int nu = 0;
        for (int i = 0; i < np; i++)
            if (!nu || memcmp(&pairs[i], &pairs[nu - 1], sizeof *pairs)) pairs[nu++] = pairs[i];
        Work *pw = malloc(sizeof *pw);  // (on the heap: pipe_watch may leave threads that still touch it)
        memcpy(pw, &w, sizeof w);
        pw->pairs = pairs;
        pw->npairs = nu;
        pw->need_mb = malloc(sizeof *pw->need_mb * (size_t)(nu ? nu : 1));
        pw->order = malloc(sizeof *pw->order * (size_t)(nu ? nu : 1));
        for (int i = 0; i < nu; i++) pw->need_mb[i] = pair_need_mb(pw, &pairs[i]), pw->order[i] = i;
        sort_need = pw->need_mb;
        qsort(pw->order, (size_t)nu, sizeof *pw->order, by_need_desc);
        pw->next_big = 0;
        pw->next_small = nu - 1;
        pw->used_mb = 0;
        const char *mem = getenv("SAVAGEWIND_PRECOMPILE_MEM_MB");
        pw->budget_mb = mem && atoll(mem) > 0 ? atoll(mem) : (int64_t)SDL_GetSystemRAM() / 4;
        pthread_mutex_init(&pw->lock, NULL);
        pthread_cond_init(&pw->cond, NULL);
        atomic_store(&pw->next, 0);
        atomic_store(&p->done, 0);
        atomic_store(&p->total, nu);
        atomic_store(&p->state, SP_PIPELINES);
        uint64_t tp = SDL_GetTicksNS();
        for (int i = 0; i < nt; i++) {
            pw->th[i].w = pw;
            atomic_store(&pw->th[i].pair, -1);
            atomic_store(&pw->th[i].out, false);
            pthread_create(&th[i], NULL, pipe_worker, &pw->th[i]);
        }
        bool left = pipe_watch(pw, th, nt);
        p->pipelines = atomic_load(&pw->built);
        extern void vk_pipeline_cache_save(void);
        vk_pipeline_cache_save();
        if (g_vk.gpl) vk_precompiled_cache_save();
        LOG("[precompile] %d %s: %d built in %.1f s\n", nu, g_vk.gpl ? "shaders and compute programs (pipeline libraries)" : "programs",
            p->pipelines, (SDL_GetTicksNS() - tp) / 1e9);
        if (seed && !atomic_load(&p->cancel)) {  // (the list's header names the device: only with one)
            int added = seed_import(&set, seed, nseed);
            LOG("[precompile] pipeline seed: %d pipelines added to the pipeline list (%d were there or have no shader here)\n",
                added, nseed - added);
            if (added) vk_pso_rewarm();  // (so that the boot right after this setup pre-warms them, not only the next start)
        }
        free(first);
        if (!left) {
            free(pairs);
            free(pw->order);
            free(pw->need_mb);
            pthread_mutex_destroy(&pw->lock);
            pthread_cond_destroy(&pw->cond);
            free(pw);
        }
    }
    free(seed);
    p->shaders = set.nshaders;
    p->keys = njobs;
    p->made = atomic_load(&w.made);
    p->cached = atomic_load(&w.cached);
    p->failed = atomic_load(&w.failed);
    free(jobs);
    shader_archive_free(&set);
    if (atomic_load(&p->cancel)) {
        atomic_store(&p->state, SP_CANCELLED);
        return false;
    }
    p->seconds = (SDL_GetTicksNS() - t0) / 1e9;
    char path[512];
    status_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        time_t now = time(NULL);
        char date[32];
        strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
        fprintf(f, "%s\n%d %s\n", vk_shader_version_full(), p->shaders, date);
        fclose(f);
    }
    atomic_store(&p->state, SP_DONE);
    return true;
}
