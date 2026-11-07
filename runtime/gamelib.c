// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "game_sites.h"
#include "paths.h"
#include "rt.h"
#include "settings.h"

static const GameLibTables *g_tables;
static char g_error[256];

// ------------------------------------------------------------------ the target (gamelib.h)
// GAMELIB_ARCH / GAMELIB_ARCH_COMPAT: the -march of each (Makefile GENARCH, GENARCH_COMPAT); GAMELIB_ID_COMPAT only
// where there is a compatible target.
#ifndef GAMELIB_ARCH
#define GAMELIB_ARCH ""
#endif

// Whether this CPU runs code built for arch: the x86-64 levels the Makefile uses. Another -march (a build's own
// GENARCH) isn't checked.
static bool cpu_runs(const char *arch) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (strstr(arch, "x86-64-v3"))
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    if (strstr(arch, "x86-64-v2")) return __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
#endif
    (void)arch;
    return true;
}

bool gamelib_compat(void) {
#ifdef GAMELIB_ID_COMPAT
    static int compat = -1;
    if (compat < 0) {
        const char *e = getenv("SAVAGEWIND_COMPAT_CPU");
        compat = (e && *e && strcmp(e, "0")) || !cpu_runs(GAMELIB_ARCH);
    }
    return compat;
#else
    return false;
#endif
}

const char *gamelib_id(void) {
#ifdef GAMELIB_ID_COMPAT
    if (gamelib_compat()) return GAMELIB_ID_COMPAT;
#endif
    return GAMELIB_ID;
}

const char *gamelib_cpu_problem(void) {
#ifdef GAMELIB_ID_COMPAT
    if (!cpu_runs(GAMELIB_ARCH_COMPAT))
        return "a CPU with SSE4.2 (Intel Nehalem / 2008 or newer, AMD Bulldozer / 2011 or newer)";
    return NULL;
#else
    return cpu_runs(GAMELIB_ARCH) ? NULL
                                  : "a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or newer)";
#endif
}

// ------------------------------------------------------------------ loading

GameSites g_site;
const char *g_game_version = "";

// Required sites must be present. Optional feature groups can be absent on an
// older game build; clear their helpers instead of preventing the game loading.
static bool take_sites(const GameLibTables *t) {
    static const struct {
        const char *name;
        size_t off;
        bool optional;
    } want[] = {
#define GAME_SITE_ENTRY(n) {#n, offsetof(GameSites, n), false},
        GAME_SITES(GAME_SITE_ENTRY)
#undef GAME_SITE_ENTRY
#define GAME_SITE_ENTRY(n) {#n, offsetof(GameSites, n), true},
        GAME_SITES_OPTIONAL(GAME_SITE_ENTRY)
#undef GAME_SITE_ENTRY
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        uint32_t k = 0;
        while (k < *t->nsites && strcmp(t->sites[k].name, want[i].name)) k++;
        if (k == *t->nsites) {
            if (want[i].optional) {
                *(uint64_t *)((char *)&g_site + want[i].off) = 0;
                continue;
            }
            snprintf(g_error, sizeof g_error, "the game code has no address for %s", want[i].name);
            return false;
        }
        *(uint64_t *)((char *)&g_site + want[i].off) = t->sites[k].addr;
    }
    g_game_version = t->version;
    return true;
}

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool read_id(const char *lib, char id[128]) {
    char path[1100];
    snprintf(path, sizeof path, "%s.id", lib);
    memset(id, 0, 128);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, 127, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return true;
}

static bool id_matches(const char *lib) {
    char id[128];
    return read_id(lib, id) && !strcmp(id, gamelib_id());
}

bool gamelib_other_cpu(const char *game_dir) {
#ifdef GAMELIB_ID_COMPAT
    char lib[1100], id[128];
    snprintf(lib, sizeof lib, "%s/%s", game_dir, GAMELIB_NAME);
    return read_id(lib, id) && !strcmp(id, gamelib_compat() ? GAMELIB_ID : GAMELIB_ID_COMPAT);
#else
    (void)game_dir;
    return false;
#endif
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("SAVAGEWIND_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, gamelib_id())) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("SAVAGEWIND_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // SAVAGEWIND_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables && !take_sites(g_tables)) g_tables = NULL;
            if (g_tables) {
                LOG("[gamelib] %s (game version %s, %u sites)\n", c[i], g_game_version, *g_tables->nsites);
#ifdef GAMELIB_ID_COMPAT
                LOG("[gamelib] target: %s\n", gamelib_compat() ? "compatible (" GAMELIB_ARCH_COMPAT
                                                                  ", no FMA instructions: slower)"
                                                                : GAMELIB_ARCH);
#endif
                for (uint32_t k = 0; k < *g_tables->nmodules; k++) {
                    const GameLibModule *m = &g_tables->modules[k];
                    LOG("[gamelib]   %-8s %llx-%llx %llu functions\n", m->name, (unsigned long long)m->base,
                        (unsigned long long)(m->base + m->size), (unsigned long long)*m->count);
                }
            }
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }

#ifndef _WIN32
__attribute__((visibility("hidden")))
#endif
int simd_fma_unfused;  // (the runtime's own copy, for its code built without FMA instructions)

void gamelib_sync(void) {
    static int env = -2, cur = -1;
    if (env == -2) {  // SAVAGEWIND_FAST_FMA=0/1: in place of the setting
        const char *e = getenv("SAVAGEWIND_FAST_FMA");
        env = e && *e ? strcmp(e, "0") != 0 : -1;
    }
    int want = gamelib_compat() && (env >= 0 ? env : g_settings.fast_fma);
    if (!g_tables || want == cur) return;
    cur = want;
    if (g_tables->fma_unfused) *g_tables->fma_unfused = want;
    simd_fma_unfused = want;
    if (gamelib_compat()) LOG("[gamelib] fma: %s\n", want ? "fast (two roundings)" : "exact (as the console)");
}
