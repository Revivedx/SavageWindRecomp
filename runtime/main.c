// Savage Wind entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "paths.h"
#include "nvn/maxwell/shader.h"
#include "nvn/shader_archive.h"
#ifndef __APPLE__
#include "nvn/precompile.h"
#include "nvn/vulkan/shaders.h"  // (the shader key log: --scan-shaders)
#endif
#include "romfs_image.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"

const char *g_romfs_dir;
uint64_t g_program_id;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// Newer SDKs (BOTW 1.8.2's) also take x4, called if nnMain returns, before nninitFinalizeSdkModule
// and nnosQuickExit: the finalizers of what x3 set up (the SDK's never run: the exit is quick).
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void module_fini(Cpu *c, Module *m) {  // DT_FINI, else the fini_array backwards
    LOG("fini %s (DT_FINI %llx, %llu fini_array entries)\n", m->name, m->fini, m->fini_array_sz / 8);
    if (m->fini) { guest_call(c, m->fini); return; }
    for (uint64_t i = m->fini_array_sz / 8; i-- > 0;) {
        uint64_t fn = ((uint64_t *)m->fini_array)[i];
        if (fn) guest_call(c, fn);
    }
}

// x2 runs the SDK's; x3 the others, once the heap is up (their constructors may allocate), dependencies first: the
// reverse of load order, subsdk9..0 then main. x4 undoes x3 in load order: main, then subsdk0..9.
static void rtld_init_sdk(Cpu *c) { module_init(c, loader_module("sdk")); }
static void rtld_init_main(Cpu *c) {
    for (int i = g_nmodules - 1; i >= 0; i--)
        if (strcmp(g_modules[i].name, "sdk")) module_init(c, &g_modules[i]);
    extern void rt_title_menu_prepare(void);  // runtime/ui/game_options.mm: tables the constructors filled
    rt_title_menu_prepare();
}
static void rtld_fini_main(Cpu *c) {
    LOG("nnMain returned\n");
    for (int i = 0; i < g_nmodules; i++)
        if (strcmp(g_modules[i].name, "sdk")) module_fini(c, &g_modules[i]);
}

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("SAVAGEWIND_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("SAVAGEWIND_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Going back to the start screen reboots the game inside this process: the window, GPU device, settings and shader
// caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the main thread, which then: stops every
// guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call / wait), resets the renderer (textures,
// address-keyed caches) and all per-boot host state (RT_ON_RESET hooks: services, HLE features, audio), empties the
// kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// SAVAGEWIND_REBOOT_AT=<frame>:<game | title>[,...] (test): at that presented frame, reboot the game in place, or go
// back to the start screen (with SAVAGEWIND_LAUNCHER_AUTOPLAY it plays again). Frames count on across reboots. Called
// by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("SAVAGEWIND_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    rt_request_reboot(!strcmp(what, "title") ? RT_REBOOT_TITLE : RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    extern void nvn_queue_idle(void);
    nvn_queue_idle();  // (runtime/nvn/objects.c: what the game submitted has run)
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void fs_title_version_patch(void); fs_title_version_patch(); }  // (runtime/services/fs.c)
    { extern void settings_load(void); settings_load(); }
    { extern void language_booted(void); language_booted(); }  // (runtime/language.c)
    kernel_init();
    dispatch_init();
    { extern void jpeg_debug_init(void); jpeg_debug_init(); }

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        static uint8_t npdm[0x10000];
        size_t n = fread(npdm, 1, sizeof npdm, f);
        if (n >= 0x80 && !memcmp(npdm, "META", 4)) {
            prio = npdm[0xE];
            stack_size = *(uint32_t *)(npdm + 0x1C);
            uint32_t aci = *(uint32_t *)(npdm + 0x70);  // ACI0: the program id at +0x10
            if (aci + 0x18 <= n && !memcmp(npdm + aci, "ACI0", 4)) g_program_id = *(uint64_t *)(npdm + aci + 0x10);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x, program %016llx\n", prio, stack_size, (unsigned long long)g_program_id);

    // nn::init::Start(thread_handle, args_region, init_sdk, init_main[, fini_main]): with x4 in newer SDKs
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    bool has_fini = start != 0;
    if (!start) start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    LOG("nn::init::Start at %llx (%s)\n", (unsigned long long)start, has_fini ? "init, init, fini" : "init, init");
    uint64_t init_sdk = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t init_main = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t fini_main = has_fini ? hle_register("<rtld_fini_main>", rtld_fini_main) : 0;
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_sdk, init_main, fini_main);
}

// --scan-shaders: what runtime/nvn/shader_archive.c finds in the game's RomFS image (a check of the enumeration the
// shader precompilation uses).
static int scan_shaders(const char *root) {
    char image[1100], why[256] = "";
    snprintf(image, sizeof image, "%s/romfs", root);
    SaSet set;
    uint64_t t0 = plat_now_ns();
    if (!shader_archive_scan(image, &set, NULL, NULL, why, sizeof why)) { fprintf(stderr, "[shaders] %s\n", why); return 1; }
    int n[6] = {0}, prog[3] = {0};
    for (int i = 0; i < set.nshaders; i++) n[set.shaders[i].stage]++;
    for (int i = 0; i < set.nprograms; i++) prog[set.programs[i].cs >= 0 ? 2 : 0]++;
    fprintf(stderr, "[shaders] %llu files (%llu MB) in %.1f s: %d archives, %d programs (%d compute), %d shaders: %d vertex, "
            "%d fragment, %d compute\n", (unsigned long long)set.files, (unsigned long long)(set.bytes >> 20),
            (plat_now_ns() - t0) / 1e9, set.nsources, set.nprograms, prog[2], set.nshaders, n[STAGE_VERTEX], n[STAGE_FRAGMENT],
            n[STAGE_COMPUTE]);
#ifndef __APPLE__
    {   // the shader key log of earlier sessions (SAVAGEWIND_CACHE_DIR): which of the shaders the game drew are in the
        // archives found, and the vertex attribute kinds they were drawn with, by archive and attribute
        VkKeyLogEntry *e;
        int n = vk_keylog_read(&e), found[6] = {0}, all[6] = {0};
        int (*kinds)[32][5] = calloc((size_t)set.nsources + 1, sizeof *kinds);
        for (int i = 0; i < n; i++) {
            if (e[i].stage > 5) continue;
            all[e[i].stage]++;
            int k = shader_archive_find(&set, e[i].code);
            if (k >= 0) found[e[i].stage]++;
            if (e[i].stage != STAGE_VERTEX) continue;
            uint32_t in = k >= 0 ? vk_sph_inputs(set.shaders[k].code, STAGE_VERTEX) : 0;
            for (int a = 0; a < 32; a++)
                if ((in >> a) & 1 && e[i].key.attr_kind[a] < 5) kinds[k >= 0 ? set.shaders[k].source : set.nsources][a][e[i].key.attr_kind[a]]++;
        }
        if (n)
            fprintf(stderr, "[shaders] key log: %d shader keys; found in the archives: vertex %d of %d, fragment %d of %d, compute %d of %d\n",
                    n, found[STAGE_VERTEX], all[STAGE_VERTEX], found[STAGE_FRAGMENT], all[STAGE_FRAGMENT], found[STAGE_COMPUTE], all[STAGE_COMPUTE]);
        for (int src = 0; src <= set.nsources && n; src++)
            for (int a = 0; a < 32; a++) {
                int *c = kinds[src][a];
                if (c[1] + c[2] + c[3] + c[4])
                    fprintf(stderr, "  attribute %2d: float %d, int %d, uint %d, int->float %d, uint->float %d  %s\n", a, c[0], c[1], c[2], c[3], c[4],
                            src < set.nsources ? set.sources[src] : "(not in the archives)");
            }
        free(kinds);
        free(e);
    }
#endif
    if (getenv("SAVAGEWIND_SCAN_LIST")) {
        int *per = calloc((size_t)set.nsources, sizeof *per);
        for (int i = 0; i < set.nshaders; i++) per[set.shaders[i].source]++;
        for (int i = 0; i < set.nsources; i++) if (per[i] || getenv("SAVAGEWIND_SCAN_LIST")[0] == '2') fprintf(stderr, "  %5d %s\n", per[i], set.sources[i]);
        free(per);
    }
    shader_archive_free(&set);
    return 0;
}

#ifndef __APPLE__
// --precompile-shaders: the shader precompilation without the start screen (runtime/nvn/precompile.h). The pipelines
// need the Vulkan device: a window is made for it (SAVAGEWIND_PRECOMPILE_NO_PIPELINES=1: translations and SPIR-V
// only, no window).
static void *precompile_progress(void *arg) {
    ShaderPrecompile *p = arg;
    while (atomic_load(&p->state) < SP_DONE) {
        fprintf(stderr, "\r[precompile] %3d%%", (int)(shader_precompile_progress(p) * 100));
        SDL_Delay(500);
    }
    return NULL;
}

static int precompile_shaders(const char *root) {
    extern void ui_window_init(void);  // (the Vulkan device, for the pipelines; SAVAGEWIND_HIDDEN keeps the window hidden)
    if (!getenv("SAVAGEWIND_PRECOMPILE_NO_PIPELINES")) ui_window_init();
    char image[1100];
    snprintf(image, sizeof image, "%s/romfs", root);
    static ShaderPrecompile p;
    pthread_t t;
    pthread_create(&t, NULL, precompile_progress, &p);
    bool ok = shader_precompile_run(&p, image);
    pthread_join(t, NULL);
    fprintf(stderr, "\n[precompile] %s: %d shaders, %d translations (%d made, %d cached, %d failed) in %.1f s%s%s\n",
            ok ? "done" : "stopped", p.shaders, p.keys, p.made, p.cached, p.failed, p.seconds, ok ? "" : ": ", ok ? "" : p.why);
    return ok ? 0 : 1;
}
#endif

// --dump-romfs <file>: writes the RomFS image the game reads (runtime/romfs_image.h) to a file, to compare it with
// another build's or check it with RomFS tools (development). Exit status 0 when the image was written.
static int dump_romfs(const char *root, const char *out_path) {
    char dir[1100], why[256] = "";
    snprintf(dir, sizeof dir, "%s/romfs", root);
    RomfsImage *img = romfs_image_open(dir, why, sizeof why);
    if (!img) { fprintf(stderr, "[romfs] cannot read %s: %s\n", dir, why); return 1; }
    FILE *out = fopen(out_path, "wb");
    uint64_t size = romfs_image_size(img), chunk = 8u << 20;
    uint8_t *buf = malloc(chunk);
    bool ok = out != NULL;
    for (uint64_t off = 0; ok && off < size; off += chunk) {
        uint64_t n = size - off < chunk ? size - off : chunk;
        ok = romfs_image_pread(img, buf, n, off) == (ssize_t)n && fwrite(buf, 1, n, out) == n;
        if ((off / chunk) % 64 == 0) fprintf(stderr, "\r[romfs] %llu / %llu MB", (unsigned long long)(off >> 20),
                                             (unsigned long long)(size >> 20));
    }
    if (out && fclose(out) != 0) ok = false;
    if (!ok) remove(out_path);
    free(buf);
    romfs_image_close(img);
    fprintf(stderr, "\n[romfs] %s %s\n", ok ? "wrote" : "failed to write", out_path);
    return ok ? 0 : 1;
}

// The start screen (runtime/ui/launcher.mm), in one of two modes: SETUP sets the game files up and nothing else (a
// plain start before they're ready; the game starts once they are), CONFIG is the whole configuration screen - Play,
// Settings, the game files, Quit ("Savage Wind Configuration", --config; Quit to Start Screen comes back to it).
enum { LAUNCHER_SETUP, LAUNCHER_CONFIG, LAUNCHER_NONE };
void launcher_run(const char *root, int mode);
bool launcher_game_ready(const char *root);
bool launcher_config_key_held(void);  // (macOS: Option held while the app opens)

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    bool skip_launcher = getenv("SAVAGEWIND_NO_LAUNCHER") || getenv("SAVAGEWIND_INPUT");  // tests boot straight in
    bool config = false;
    static char arg_root[1024];
    const char *dump = NULL;
    bool scan = false, precompile = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (!strcmp(argv[i], "--config")) config = true;
        else if (!strcmp(argv[i], "--dump-romfs") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--scan-shaders")) scan = true;
        else if (!strcmp(argv[i], "--precompile-shaders")) precompile = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
#ifdef __APPLE__
    if (launcher_config_key_held()) config = true;  // (the Mac's "Savage Wind Configuration": Option held)
#endif
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    if (dump) return dump_romfs(root, dump);
    if (scan) return scan_shaders(root);
#ifndef __APPLE__
    if (precompile) return precompile_shaders(root);
#else
    (void)precompile;
#endif
    // A plain start goes straight into the game once the game files are set up and the game code built from them
    // matches this build; the start screen only when there is something to set up, or when asked for.
    int mode = config ? LAUNCHER_CONFIG : skip_launcher || launcher_game_ready(root) ? LAUNCHER_NONE : LAUNCHER_SETUP;
    // (tests of the start screen - screenshots, setup and delete runs - show it: the configuration screen, or
    // SAVAGEWIND_LAUNCHER_MODE=setup / config)
    const char *test_mode = getenv("SAVAGEWIND_LAUNCHER_MODE");
    if (test_mode || getenv("SAVAGEWIND_LAUNCHER_SHOT") || getenv("SAVAGEWIND_LAUNCHER_EXTRACT") ||
        getenv("SAVAGEWIND_LAUNCHER_SETUP") || getenv("SAVAGEWIND_LAUNCHER_DELETE"))
        mode = test_mode && !strcmp(test_mode, "setup") ? LAUNCHER_SETUP : LAUNCHER_CONFIG;
    for (;;) {
        if (mode != LAUNCHER_NONE) launcher_run(root, mode);
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        mode = kind == RT_REBOOT_TITLE ? LAUNCHER_CONFIG : LAUNCHER_NONE;
    }
}
