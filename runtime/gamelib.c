// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, GAMELIB_ID);
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

static void close_lib(void *h) {
#ifdef _WIN32
    FreeLibrary((HMODULE)h);
#else
    dlclose(h);
#endif
}

static const GameLibTables *open_lib(const char *path, void **handle) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); close_lib(h); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        close_lib(h);  // (rebuilt at the same path: the next look then reads the new file, not this copy)
        return NULL;
    }
    if (handle) *handle = h;
    return t;
}

// The library was generated from these game files: its main module's build id is theirs (tools/layout.py; runtime/
// loader.c checks it too before anything runs).
static bool built_from(const GameLibTables *t, const char *main_build_id) {
    return t && t->modules && t->nmodules && *t->nmodules && t->modules[0].build_id && main_build_id &&
           *main_build_id && !strncmp(t->modules[0].build_id, main_build_id, strlen(main_build_id));
}

bool gamelib_present(const char *game_dir, const char *main_build_id) {
    if (g_tables) return built_from(g_tables, main_build_id);
    char c[3][1100];
    int n = candidates(game_dir, c);
    const char *env = getenv("SAVAGEWIND_GAME_LIB");
    for (int i = 0; i < n; i++) {
        if (!exists(c[i])) continue;
        if (!(i == 0 && env && *env) && !id_matches(c[i])) continue;  // (another build of Savage Wind's)
        void *h = NULL;
        const GameLibTables *t = open_lib(c[i], &h);  // (closed again: this only asks whether its code would do)
        bool ok = built_from(t, main_build_id);
        if (h) close_lib(h);
        if (ok) return true;
    }
    return false;
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
            g_tables = open_lib(c[i], NULL);
            if (g_tables) {
                LOG("[gamelib] %s\n", c[i]);
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

uint64_t rt_game_sym(const char *name) {
    if (!g_tables || !g_tables->symbols || !g_tables->nsymbols) return 0;
    for (uint32_t i = 0; i < *g_tables->nsymbols; i++)
        if (!strcmp(g_tables->symbols[i].name, name)) return g_tables->symbols[i].value;
    return 0;
}
