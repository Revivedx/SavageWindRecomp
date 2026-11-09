// Launch options at start and at PLAY: see launch_flags_boot.h.
#include "launch_flags_boot.h"

#include <stdio.h>
#include <string.h>

#include "launch_flags.h"
#include "paths.h"
#include "platform.h"

static bool g_off;

bool launch_flags_disabled(void) { return g_off; }

bool launch_flags_path(char *out, size_t n) {
    char root[1024];
    const char *known = rt_data_root();  // (set once rt_paths_init has run: the start screen)
    if (known && *known) snprintf(root, sizeof root, "%s", known);
    else if (!rt_peek_data_root(root, sizeof root)) return false;
    return (size_t)snprintf(out, n, "%s/" LF_FILE, root) < n;
}

void launch_flags_boot(int argc, char **argv) {
    bool config = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], LF_OFF_SWITCH)) g_off = true;
        else if (!strcmp(argv[i], "--config")) config = true;
    }
    LfList none = {.n = 0};
    if (g_off) {
        if (lf_applied()[0]) lf_apply(&none);  // (a restart from an earlier start with them)
        fprintf(stderr, "[flags] " LF_OFF_SWITCH ": the saved launch options are not used\n");
        return;
    }
    if (config) return;  // (the start screen is always clean; PLAY restarts with the flags)
    char path[1100];
    static LfList l;
    if (!launch_flags_path(path, sizeof path)) return;
    lf_load(&l, path);  // (no file: an empty list)
    if (lf_in_effect(&l)) return;
    int n = lf_apply(&l);
    fprintf(stderr, "[flags] %d launch option(s) from %s: restarting with them\n", n, path);
    plat_exec(NULL);
    fprintf(stderr, "[flags] could not restart; going on with the options set from here\n");
}

void launch_flags_play(const char *root) {
    if (g_off) return;
    char path[1100], exe[4096];
    static LfList l;
    if (!launch_flags_path(path, sizeof path)) return;
    lf_load(&l, path);
    if (lf_in_effect(&l)) return;
    lf_apply(&l);
    if (!plat_exe_path(exe, sizeof exe)) return;
    char *av[4] = {exe, (char *)"--play", NULL, NULL};
    if (root && *root && strcmp(root, "extracted")) av[2] = (char *)root;
    fprintf(stderr, "[flags] launch options changed: restarting into the game\n");
    plat_exec(av);
    fprintf(stderr, "[flags] could not restart\n");
}
