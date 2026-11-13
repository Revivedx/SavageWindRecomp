// savagewind <command> (runtime/cli.h, docs/cli.md): the command table and its help, shell completion, and the
// commands that don't need the start screen: settings, launch-options, saves, paths, shaders, update, log.
#include "cli.h"

#include <SDL3/SDL.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "launch_flags.h"
#include "launch_flags_boot.h"
#include "paths.h"
#include "platform.h"
#include "post_update.h"
#include "rt.h"
#include "settings.h"
#include "nvn/precompile.h"
#include "services/save_convert.h"
#include "services/save_profiles.h"

FILE *cli_out;

void cli_keep_stdout(void) {
    fflush(stdout);
#ifdef _WIN32
    int fd = _dup(1);
    cli_out = fd >= 0 ? _fdopen(fd, "w") : NULL;
#else
    // Kept open across the restart that applies the launch options (plat_exec): by then stdout is the log, so the
    // restarted program takes the copy made the first time (as SAVAGEWIND_LOG_ECHO_FD)
    const char *e = getenv("SAVAGEWIND_CLI_OUT_FD");
    int fd = e && *e && fcntl(atoi(e), F_GETFD) != -1 ? atoi(e) : dup(1);
    if (fd >= 0 && !(e && *e)) {
        char v[16];
        snprintf(v, sizeof v, "%d", fd);
        setenv("SAVAGEWIND_CLI_OUT_FD", v, 1);
    }
    cli_out = fd >= 0 ? fdopen(fd, "w") : NULL;
#endif
    if (!cli_out) cli_out = stdout;
}

bool cli_applies_launch_options(const char *cmd) {
    return !cmd || !strcmp(cmd, "run") || !strcmp(cmd, "config") || !strcmp(cmd, "setup") ||
           !strcmp(cmd, "precompile-shaders");
}

// ------------------------------------------------------------------ the commands and their help
typedef struct {
    const char *name, *aliases, *usage, *summary, *details;
    bool own_args;  // reads its own arguments (main.c doesn't)
} Command;

static const Command k_commands[] = {
    {"setup", "install build", "setup [<folder>] [--json]", "set the game files up and build the game code",
     "Sets the game files up from <folder>: the extracted exefs/ and romfs/ (the 1.9.0 update applied), and the DLC's\n"
     "RomFS folders if you have them. It checks them, copies them, builds the game code and, when that option is on,\n"
     "precompiles the shaders, as Set up game files does. With no folder, builds the game code again from the copy\n"
     "already set up. Ctrl+C stops it; the next setup starts over.\n"
     "--json: progress and the result as JSON lines (stage: copy / build / shaders; done / total bytes when copying).\n"
     "Exit status 0: the game files are ready. 130: stopped.", false},
    {"run", "play", "run [--profile <name>]", "start the game",
     "Starts the game straight away. Fails without opening anything when the game files aren't ready.\n"
     "--profile <name>: switch to that save profile first (it stays the active one, as on the Saves tab).", false},
    {"status", "", "status [--json]", "whether the game files are ready, their version and the DLC",
     "A quick look: whether the game files are set up and the game code built for this version, which version they\n"
     "are, the main module's build ID and the DLC. Exit status 0: ready to play.", false},
    {"verify", "", "verify [--json]", "check every game file and the DLC in full",
     "Checks every file of the RomFS (count and size) against the version, and reads the DLC in full. Slower than\n"
     "status. Exit status 0: all fine; 1: something is wrong (set up again).", false},
    {"delete", "", "delete --yes", "delete the game files and the game code built from them",
     "Deletes the copy of the game files and the game code built from it. Never the saves, settings or shaders.", false},
    {"settings", "", "settings [list | get <key> | set <key> <value>] [--json]", "read and change the settings",
     "list: every setting and its value, as savagewind.ini has them (the defaults when there's no file yet).\n"
     "get <key>: one value. set <key> <value>: changes it in savagewind.ini; a value Savage Wind doesn't take is\n"
     "refused and the file stays as it was. Takes effect at the next start; a running game may write over it.", true},
    {"launch-options", "flags", "launch-options [list | available [--all] | set <NAME> [<value>] | remove <NAME> | clear] [--json]",
     "the launch options Savage Wind starts with",
     "list: the saved launch options (launch-flags.txt), as Settings > Launch options shows them.\n"
     "available: the ones that can be added, with what they do (--all: the developer ones too).\n"
     "set <NAME> [<value>]: adds or changes one (a switch without a value is turned on). remove <NAME>, clear.", true},
    {"saves", "", "saves [list | backups | backup | use <profile> | create <profile> | restore <backup> --yes] [--json]",
     "save profiles and backups",
     "list: the save profiles (the active one marked) and how many saves each has.\n"
     "backups: the backups of the active profile's saves. backup: makes one now.\n"
     "use <profile>: makes it the active profile. create <profile>: a new, empty one.\n"
     "restore <backup> --yes: puts a backup back. The current saves aren't deleted: they're moved into the backups\n"
     "first, as a new dated backup. Refused while the game runs.", true},
    {"paths", "", "paths [--json]", "where Savage Wind keeps its files",
     "The data root, the game files, the settings, the launch options, the active saves and their backups, the log and\n"
     "the shader cache.", true},
    {"shaders", "", "shaders [--json]", "whether the shaders are precompiled",
     "Whether all the shaders were precompiled, how many, when, and whether that was for this version and driver\n"
     "(outdated: precompile them again).", true},
    {"precompile-shaders", "", "precompile-shaders", "precompile all the game's shaders", "", false},
    {"update", "", "update [--json]", "check for a newer version of Savage Wind",
     "Asks SW Update whether there's a newer release (releases only; a source build has none). Doesn't install it:\n"
     "Savage Wind Configuration does. Exit status 0: checked (available says whether there is one); 1: couldn't check.",
     true},
    {"log", "", "log [--follow | --previous]", "show the log",
     "Prints savagewind.log. --follow: keep printing what's added until Ctrl+C. --previous: the run before's.", true},
    {"config", "", "config", "open Savage Wind Configuration", "", false},
    {"add-to-steam", "", "add-to-steam", "add Savage Wind to Steam as a non-Steam game", "", false},
    {"completion", "", "completion bash | zsh | fish", "print a shell completion script",
     "bash:  savagewind completion bash > ~/.local/share/bash-completion/completions/savagewind\n"
     "zsh:   savagewind completion zsh > \"${fpath[1]}/_savagewind\"\n"
     "fish:  savagewind completion fish > ~/.config/fish/completions/savagewind.fish", false},
    {"help", "", "help [<command>]", "this help, or a command's", "", false},
    {"version", "", "version", "Savage Wind's version", "", false},
};
enum { N_COMMANDS = sizeof k_commands / sizeof *k_commands };

static bool word_in(const char *word, const char *list) {
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, word, n)) return true;
        p += len + (e ? 1 : 0);
    }
    return false;
}

static const Command *find_command(const char *arg) {
    if (!arg) return NULL;
    for (int i = 0; i < N_COMMANDS; i++)
        if (!strcmp(arg, k_commands[i].name) || word_in(arg, k_commands[i].aliases)) return &k_commands[i];
    return NULL;
}

const char *cli_command(const char *arg) {
    if (!arg) return NULL;
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) return "help";
    if (!strcmp(arg, "--version")) return "version";
    const Command *c = find_command(arg);
    return c ? c->name : NULL;
}

bool cli_owns_args(const char *cmd) {
    const Command *c = find_command(cmd);
    return c && c->own_args;
}

int cli_help(const char *topic) {
    if (topic) {
        const Command *c = find_command(topic);
        if (!c) {
            fprintf(stderr, "savagewind help: no command %s (savagewind help lists them)\n", topic);
            return 2;
        }
        printf("usage: savagewind %s\n\n%s.\n", c->usage, c->summary);
        if (c->aliases[0]) printf("Also: %s\n", c->aliases);
        if (c->details[0]) printf("\n%s\n", c->details);
        return 0;
    }
    printf("usage: savagewind [<command>] [options]\n\n"
           "With no command, Savage Wind starts as usual: the game when it's set up, the start screen when not.\n\n");
    for (int i = 0; i < N_COMMANDS; i++) printf("  %-20s %s\n", k_commands[i].name, k_commands[i].summary);
    printf("\nsavagewind help <command> says more. --json (where a command has it) is for launchers: JSON on stdout.\n"
           "A first argument that isn't a command is the game files folder, as before; --play, --config and the\n"
           "other flags work as they always have.\n");
    return 0;
}

// ------------------------------------------------------------------ shell completion
// The subcommands each command takes (completion only).
static const char *subwords(const char *cmd) {
    if (!strcmp(cmd, "settings")) return "list get set";
    if (!strcmp(cmd, "launch-options")) return "list available set remove clear";
    if (!strcmp(cmd, "saves")) return "list backups backup use create restore";
    if (!strcmp(cmd, "completion")) return "bash zsh fish";
    if (!strcmp(cmd, "log")) return "--follow --previous";
    if (!strcmp(cmd, "help")) return "";
    return "--json";
}

static void all_names(char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < N_COMMANDS; i++) {
        snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? " " : "", k_commands[i].name);
        if (k_commands[i].aliases[0]) snprintf(out + strlen(out), n - strlen(out), " %s", k_commands[i].aliases);
    }
}

int cli_completion(const char *shell) {
    char names[1024];
    all_names(names, sizeof names);
    if (shell && !strcmp(shell, "bash")) {
        printf("# savagewind completion bash\n_savagewind() {\n"
               "    local cur=${COMP_WORDS[COMP_CWORD]}\n"
               "    if [ \"$COMP_CWORD\" -eq 1 ]; then COMPREPLY=($(compgen -W \"%s\" -- \"$cur\")); return; fi\n"
               "    case ${COMP_WORDS[1]} in\n", names);
        for (int i = 0; i < N_COMMANDS; i++)
            printf("        %s) COMPREPLY=($(compgen -W \"%s\" -- \"$cur\")) ;;\n", k_commands[i].name,
                   !strcmp(k_commands[i].name, "help") ? names : subwords(k_commands[i].name));
        printf("        setup|install|build) COMPREPLY=($(compgen -d -- \"$cur\")) ;;\n"
               "    esac\n}\ncomplete -F _savagewind savagewind\n");
        return 0;
    }
    if (shell && !strcmp(shell, "zsh")) {
        printf("#compdef savagewind\n_savagewind() {\n"
               "    if (( CURRENT == 2 )); then compadd -- %s; return; fi\n"
               "    case $words[2] in\n", names);
        for (int i = 0; i < N_COMMANDS; i++) {
            const char *w = !strcmp(k_commands[i].name, "help") ? names : subwords(k_commands[i].name);
            if (!strcmp(k_commands[i].name, "setup")) printf("        setup|install|build) _files -/ ;;\n");
            else if (*w) printf("        %s) compadd -- %s ;;\n", k_commands[i].name, w);
        }
        printf("    esac\n}\ncompdef _savagewind savagewind\n");
        return 0;
    }
    if (shell && !strcmp(shell, "fish")) {
        printf("# savagewind completion fish\ncomplete -c savagewind -f\n");
        for (int i = 0; i < N_COMMANDS; i++)
            printf("complete -c savagewind -n '__fish_use_subcommand' -a %s -d '%s'\n", k_commands[i].name, k_commands[i].summary);
        for (int i = 0; i < N_COMMANDS; i++) {
            const char *w = !strcmp(k_commands[i].name, "help") ? names : subwords(k_commands[i].name);
            if (*w) printf("complete -c savagewind -n '__fish_seen_subcommand_from %s' -a '%s'\n", k_commands[i].name, w);
        }
        printf("complete -c savagewind -n '__fish_seen_subcommand_from setup install build' -F\n");
        return 0;
    }
    fprintf(stderr, "usage: savagewind completion bash | zsh | fish\n");
    return 2;
}

// ------------------------------------------------------------------ shared
void cli_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c; c++) {
        if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
        else if (*c < 0x20) fprintf(f, "\\u%04x", *c);
        else fputc(*c, f);
    }
    fputc('"', f);
}

bool cli_has(int argc, char **argv, const char *opt) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], opt)) return true;
    return false;
}

// The arguments that aren't options, in order
static int positional(int argc, char **argv, const char **out, int max) {
    int n = 0;
    for (int i = 0; i < argc && n < max; i++)
        if (argv[i][0] != '-') out[n++] = argv[i];
    return n;
}

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;
}
void cli_catch_stop(void) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
}
bool cli_stop_requested(void) { return g_stop != 0; }

// An absolute path for one relative to the data root (the working directory)
static void absolute(const char *p, char *out, size_t n) {
    bool abs = p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
    if (abs) snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", rt_data_root(), p);
}

static void json_field(bool *first, const char *key, const char *value) {
    fprintf(cli_out, "%s", *first ? "{" : ",");
    *first = false;
    cli_json_str(cli_out, key);
    fputc(':', cli_out);
    cli_json_str(cli_out, value);
}

// ------------------------------------------------------------------ settings
typedef struct {
    char key[64], value[1100];
} Setting;

// Every setting as settings_save would write it now (in another file: savagewind.ini isn't touched). The count.
static int settings_now(Setting **out) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.cli-%d", settings_file(), (int)SDL_GetTicks());
    *out = NULL;
    if (!settings_save_to(tmp)) return -1;
    FILE *f = fopen(tmp, "r");
    int n = 0, cap = 0;
    char line[1280];
    while (f && fgets(line, sizeof line, f)) {
        Setting s;
        s.value[0] = 0;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %1099[^\r\n]", s.key, s.value) < 1) continue;
        if (n == cap) {
            Setting *grown = realloc(*out, sizeof **out * (cap = cap ? cap * 2 : 128));
            if (!grown) break;
            *out = grown;
        }
        (*out)[n++] = s;
    }
    if (f) fclose(f);
    remove(tmp);
    return n;
}

static const Setting *find_setting(const Setting *s, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].key, key)) return &s[i];
    return NULL;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) b[n] = 0, *len = (size_t)n;
    return b;
}

static bool write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

// savagewind.ini with `key = value` in place of that key's line (added at the end when there's none)
static char *ini_with(const char *ini, const char *key, const char *value) {
    size_t cap = strlen(ini) + strlen(key) + strlen(value) + 8, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    bool done = false;
    for (const char *p = ini; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p + 1) : strlen(p);
        const char *k = p;
        while (k < p + len && (*k == ' ' || *k == '\t')) k++;
        size_t kl = strlen(key);
        const char *after = k + kl;
        while (after < p + len && (*after == ' ' || *after == '\t')) after++;
        if (!done && !strncmp(k, key, kl) && after < p + len && *after == '=') {
            n += (size_t)snprintf(out + n, cap - n, "%s = %s\n", key, value);
            done = true;
        } else {
            memcpy(out + n, p, len);
            n += len;
        }
        p += len;
    }
    if (!done) {
        if (n && out[n - 1] != '\n') out[n++] = '\n';
        n += (size_t)snprintf(out + n, cap - n, "%s = %s\n", key, value);
    }
    out[n] = 0;
    return out;
}

static int cmd_settings(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    const char *a[3] = {0};
    int na = positional(argc, argv, a, 3);
    const char *sub = na ? a[0] : "list";
    Setting *s;
    int n = settings_now(&s);
    if (n < 0) {
        fprintf(cli_out, "The settings couldn't be read (the data root isn't writable?)\n");
        return 1;
    }
    int rc = 0;
    if (!strcmp(sub, "list")) {
        bool first = true;
        for (int i = 0; i < n; i++) {
            if (json) json_field(&first, s[i].key, s[i].value);
            else fprintf(cli_out, "%s = %s\n", s[i].key, s[i].value);
        }
        if (json) fprintf(cli_out, "%s\n", first ? "{}" : "}");
    } else if (!strcmp(sub, "get") && na == 2) {
        const Setting *f = find_setting(s, n, a[1]);
        if (!f) fprintf(cli_out, "No setting %s (savagewind settings list shows them)\n", a[1]), rc = 1;
        else if (json) { bool first = true; json_field(&first, f->key, f->value); fprintf(cli_out, "}\n"); }
        else fprintf(cli_out, "%s\n", f->value);
    } else if (!strcmp(sub, "set") && na == 3) {
        const Setting *was = find_setting(s, n, a[1]);
        const char *path = settings_file();
        size_t len = 0;
        char *before = read_file(path, &len);
        char *after = NULL;
        if (!was) {
            fprintf(cli_out, "No setting %s (savagewind settings list shows them)\n", a[1]);
            rc = 1;
        } else if (strchr(a[2], '\n') || strlen(a[2]) >= sizeof was->value) {
            fprintf(cli_out, "That value can't be used for %s\n", a[1]);
            rc = 1;
        } else if (!before && !settings_save_to(path)) {
            fprintf(cli_out, "%s couldn't be written\n", path);
            rc = 1;
        } else if (!before && !(before = read_file(path, &len))) {
            fprintf(cli_out, "%s couldn't be read\n", path);
            rc = 1;
        } else if (!(after = ini_with(before, a[1], a[2])) || !write_file(path, after, strlen(after))) {
            fprintf(cli_out, "%s couldn't be written\n", path);
            rc = 1;
        } else {
            // read back as Savage Wind reads it at a start, then written out as it saves it: what it made of the value
            settings_reload();
            settings_save();
            Setting *now;
            int m = settings_now(&now);
            const Setting *is = m > 0 ? find_setting(now, m, a[1]) : NULL;
            bool refused = !is || (!strcmp(is->value, was->value) && strcmp(a[2], was->value));
            if (refused) {
                write_file(path, before, len);  // (the file as it was)
                fprintf(cli_out, "Savage Wind doesn't take %s for %s; it stays %s\n", a[2], a[1], was->value);
                rc = 1;
            } else if (json) {
                bool first = true;
                json_field(&first, is->key, is->value);
                fprintf(cli_out, "}\n");
            } else fprintf(cli_out, "%s = %s\n", is->key, is->value);
            free(now);
        }
        free(before);
        free(after);
    } else {
        fprintf(cli_out, "usage: savagewind settings [list | get <key> | set <key> <value>] [--json]\n");
        rc = 2;
    }
    free(s);
    return rc;
}

// ------------------------------------------------------------------ launch options
static const char *const k_groups[] = {"play", "other", "dev", "internal", "launcher", "build"};

static int cmd_launch_options(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    const char *a[3] = {0};
    int na = positional(argc, argv, a, 3);
    const char *sub = na ? a[0] : "list";
    char path[1100];
    if (!launch_flags_path(path, sizeof path)) {
        fprintf(cli_out, "The launch options file can't be found (no data root)\n");
        return 1;
    }
    static LfList l;
    lf_load(&l, path);
    if (!strcmp(sub, "list")) {
        if (json) fprintf(cli_out, "[");
        for (int i = 0; i < l.n; i++) {
            if (json) {
                fprintf(cli_out, "%s{\"name\":", i ? "," : "");
                cli_json_str(cli_out, l.flag[i].name);
                fprintf(cli_out, ",\"value\":");
                cli_json_str(cli_out, l.flag[i].value);
                fprintf(cli_out, ",\"skipped\":%s}", lf_skipped(&l.flag[i]) ? "true" : "false");
            } else fprintf(cli_out, "%s=%s%s\n", l.flag[i].name, l.flag[i].value, lf_skipped(&l.flag[i]) ? "  (not used)" : "");
        }
        if (json) fprintf(cli_out, "]\n");
        else if (!l.n) fprintf(cli_out, "No launch options saved.\n");
        return 0;
    }
    if (!strcmp(sub, "available")) {
        bool all = cli_has(argc, argv, "--all"), first = true;
        if (json) fprintf(cli_out, "[");
        for (int i = 0; i < LF_TABLE_N; i++) {
            const LfInfo *f = &LF_TABLE[i];
            if (f->group > LF_GROUP_DEV || (f->group == LF_GROUP_DEV && !all)) continue;
            if (json) {
                fprintf(cli_out, "%s{\"name\":", first ? "" : ",");
                cli_json_str(cli_out, f->name);
                fprintf(cli_out, ",\"kind\":\"%s\",\"group\":\"%s\",\"example\":",
                        f->kind == LF_KIND_SWITCH ? "switch" : f->kind == LF_KIND_NUMBER ? "number" : "text", k_groups[f->group]);
                cli_json_str(cli_out, f->example);
                fprintf(cli_out, ",\"description\":");
                cli_json_str(cli_out, f->desc);
                fprintf(cli_out, "}");
            } else fprintf(cli_out, "%-36s %s\n", f->name, f->desc[0] ? f->desc : "-");
            first = false;
        }
        if (json) fprintf(cli_out, "]\n");
        return 0;
    }
    if ((!strcmp(sub, "set") && na >= 2) || (!strcmp(sub, "remove") && na == 2)) {
        char name[LF_NAME_MAX];
        snprintf(name, sizeof name, "%s", a[1]);
        lf_normalize_name(name);
        if (!strcmp(sub, "remove")) {
            int at = lf_index(&l, name);
            if (at < 0) { fprintf(cli_out, "%s isn't among the launch options\n", name); return 1; }
            lf_remove(&l, at);
        } else {
            char value[LF_VALUE_MAX];
            const LfInfo *f = lf_find(name);
            snprintf(value, sizeof value, "%s", na == 3 ? a[2] : f && f->kind != LF_KIND_SWITCH && f->example[0] ? f->example : "1");
            lf_normalize_value(value);
            LfError e = lf_set(&l, name, value);
            if (e != LF_OK) { fprintf(cli_out, "%s: %s\n", name, lf_error_text(e)); return 1; }
        }
        if (!lf_save(&l, path)) { fprintf(cli_out, "%s couldn't be written\n", path); return 1; }
        fprintf(cli_out, "Saved. Savage Wind starts with them from its next start.\n");
        return 0;
    }
    if (!strcmp(sub, "clear")) {
        lf_clear(&l);
        if (!lf_save(&l, path)) { fprintf(cli_out, "%s couldn't be written\n", path); return 1; }
        fprintf(cli_out, "No launch options now.\n");
        return 0;
    }
    fprintf(cli_out, "usage: savagewind launch-options [list | available [--all] | set <NAME> [<value>] | remove <NAME> | clear]\n");
    return 2;
}

// ------------------------------------------------------------------ saves
static const char *sp_text(SpCode c) {
    switch (c) {
    case SP_OK: return "done";
    case SP_NAME_EMPTY: return "the name is empty";
    case SP_NAME_LONG: return "the name is too long";
    case SP_NAME_CHARS: return "a name can only have letters, digits, spaces and - _ . ( )";
    case SP_NAME_RESERVED: return "that name is reserved";
    case SP_EXISTS: return "a profile with that name exists";
    case SP_NOT_FOUND: return "there's no such profile";
    case SP_IS_DEFAULT: return "the Default profile can't be changed that way";
    case SP_IS_ACTIVE: return "it's the active profile";
    case SP_IN_USE: return "the game is running with it";
    case SP_NO_SPACE: return "there isn't enough free space";
    case SP_LINKED: return "its folder is a link";
    default: return "a file or folder couldn't be read or written";
    }
}

// The active profile's backups folder: <save>-backups
static void backups_dir(const char *save, char *out, size_t n) { snprintf(out, n, "%s-backups", save); }

typedef struct { char name[64][128]; int n; } Names;
static SDL_EnumerationResult SDLCALL collect(void *u, const char *dir, const char *name) {
    (void)dir;
    Names *l = u;
    if (l->n < 64 && name[0] != '.' && strcmp(name, "auto-last.txt")) snprintf(l->name[l->n++], sizeof l->name[0], "%s", name);
    return SDL_ENUM_CONTINUE;
}
static int by_name(const void *a, const void *b) { return strcmp(a, b); }

static bool copy_tree(const char *from, const char *to) {
    SDL_PathInfo in;
    if (!SDL_GetPathInfo(from, &in)) return false;
    if (in.type != SDL_PATHTYPE_DIRECTORY) return SDL_CopyFile(from, to);
    if (!SDL_CreateDirectory(to)) return false;
    Names l = {0};
    if (!SDL_EnumerateDirectory(from, collect, &l) || l.n >= 64) return false;  // (a save folder has a handful)
    for (int i = 0; i < l.n; i++) {
        char a[1100], b[1100];
        snprintf(a, sizeof a, "%s/%s", from, l.name[i]);
        snprintf(b, sizeof b, "%s/%s", to, l.name[i]);
        if (!copy_tree(a, b)) return false;
    }
    return true;
}

static SDL_EnumerationResult SDLCALL remove_tree_cb(void *u, const char *dir, const char *name) {
    char p[1100];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo in;
    if (SDL_GetPathInfo(p, &in) && in.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, remove_tree_cb, u);
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

static int cmd_saves(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    const char *a[2] = {0};
    int na = positional(argc, argv, a, 2);
    const char *sub = na ? a[0] : "list";
    char save[1024], backups[1100];
    sp_active_save_dir(NULL, save, sizeof save);
    backups_dir(save, backups, sizeof backups);
    if (!strcmp(sub, "list")) {
        SpProfile *p = NULL;
        int n = sp_list(NULL, &p);
        if (json) fprintf(cli_out, "[");
        for (int i = 0; i < n; i++) {
            char when[32] = "";
            if (p[i].saved) strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&p[i].saved));
            if (json) {
                fprintf(cli_out, "%s{\"name\":", i ? "," : "");
                cli_json_str(cli_out, p[i].name);
                fprintf(cli_out, ",\"active\":%s,\"slots\":%d,\"saved\":%lld}", p[i].active ? "true" : "false", p[i].slots,
                        (long long)p[i].saved);
            } else
                fprintf(cli_out, "%s %-32s %d save%s%s%s\n", p[i].active ? "*" : " ", p[i].name, p[i].slots,
                        p[i].slots == 1 ? "" : "s", when[0] ? ", last saved " : "", when);
        }
        if (json) fprintf(cli_out, "]\n");
        free(p);
        return 0;
    }
    if (!strcmp(sub, "backups")) {
        Names l = {0};
        SDL_EnumerateDirectory(backups, collect, &l);
        qsort(l.name, (size_t)l.n, sizeof l.name[0], by_name);
        if (json) fprintf(cli_out, "[");
        for (int i = 0; i < l.n; i++) {
            if (json) {
                fprintf(cli_out, "%s", i ? "," : "");
                cli_json_str(cli_out, l.name[i]);
            } else fprintf(cli_out, "%s\n", l.name[i]);
        }
        if (json) fprintf(cli_out, "]\n");
        else if (!l.n) fprintf(cli_out, "No backups yet (%s).\n", backups);
        return 0;
    }
    if (!strcmp(sub, "backup")) {
        char to[1100];
        sc_backup_path(save, to, sizeof to);
        ScReport r;
        memset(&r, 0, sizeof r);
        ScCode c = sc_backup(save, to, &r);
        sc_report_free(&r);
        if (c == SC_OK) fprintf(cli_out, "Backed up to %s\n", to);
        else fprintf(cli_out, c == SC_NO_SAVE ? "No saves to back up yet.\n" : c == SC_IN_USE ? "The game is running: close it first.\n"
                                                                                           : "The backup didn't work (the log says why).\n");
        return c == SC_OK ? 0 : 1;
    }
    if ((!strcmp(sub, "use") || !strcmp(sub, "create")) && na == 2) {
        SpCode c = !strcmp(sub, "use") ? sp_set_active(NULL, a[1]) : sp_create(NULL, a[1]);
        if (c != SP_OK) fprintf(cli_out, "%s: %s\n", a[1], sp_text(c));
        else fprintf(cli_out, !strcmp(sub, "use") ? "Now playing with the %s saves.\n" : "Made the profile %s.\n", a[1]);
        return c == SP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "restore") && na == 2) {
        if (!cli_has(argc, argv, "--yes")) {
            fprintf(cli_out, "savagewind saves restore: add --yes to put %s back (your current saves become a backup)\n", a[1]);
            return 2;
        }
        char from[1100];
        snprintf(from, sizeof from, "%s/%s", backups, a[1]);
        SDL_PathInfo in;
        if (strchr(a[1], '/') || strchr(a[1], '\\') || !strcmp(a[1], "..") || !SDL_GetPathInfo(from, &in) ||
            in.type != SDL_PATHTYPE_DIRECTORY) {
            fprintf(cli_out, "No backup %s (savagewind saves backups lists them)\n", a[1]);
            return 1;
        }
        ScLock *lock = NULL;
        if (sc_lock_take(save, &lock) == SC_LOCK_BUSY) {
            fprintf(cli_out, "The game is running with these saves: close it first.\n");
            return 1;
        }
        // The current saves move into the backups whole (nothing is deleted), then the backup is copied in its place
        char aside[1100];
        sc_backup_path(save, aside, sizeof aside);
        bool had = SDL_GetPathInfo(save, &in);
        if (had && (!SDL_CreateDirectory(backups) || !SDL_RenamePath(save, aside))) {
            sc_lock_drop(lock);
            fprintf(cli_out, "The current saves couldn't be moved aside (%s): nothing was changed.\n", SDL_GetError());
            return 1;
        }
        bool ok = copy_tree(from, save);
        if (!ok) {  // (back as it was: the copy made so far goes, the current saves come back)
            SDL_EnumerateDirectory(save, remove_tree_cb, NULL);
            SDL_RemovePath(save);
            if (had) SDL_RenamePath(aside, save);
        }
        sc_lock_drop(lock);
        if (ok) fprintf(cli_out, "Restored %s.%s%s\n", a[1], had ? " Your saves before it are in " : "", had ? aside : "");
        else fprintf(cli_out, "The backup couldn't be copied (%s): your saves are as they were.\n", SDL_GetError());
        return ok ? 0 : 1;
    }
    fprintf(cli_out, "usage: savagewind saves [list | backups | backup | use <profile> | create <profile> | restore <backup> --yes]\n");
    return 2;
}

// ------------------------------------------------------------------ paths, shaders, update, log
static int cmd_paths(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    char game[1100], settings[1100], flags[1100] = "", save[1024], saves[1100], backups[1100], cache[1100];
    absolute("extracted", game, sizeof game);
    absolute(settings_file(), settings, sizeof settings);
    launch_flags_path(flags, sizeof flags);
    sp_active_save_dir(NULL, save, sizeof save);
    absolute(save, saves, sizeof saves);
    backups_dir(saves, backups, sizeof backups);
    const char *c = getenv("SAVAGEWIND_CACHE_DIR");
    absolute(c && *c ? c : "cache", cache, sizeof cache);
    const char *rows[][2] = {{"data_root", rt_data_root()}, {"game_files", game}, {"settings", settings},
                             {"launch_options", flags}, {"saves", saves}, {"save_backups", backups},
                             {"log", plat_log_path()}, {"shader_cache", cache}};
    bool first = true;
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        if (json) json_field(&first, rows[i][0], rows[i][1]);
        else fprintf(cli_out, "%-15s %s\n", rows[i][0], rows[i][1]);
    }
    if (json) fprintf(cli_out, "}\n");
    return 0;
}

static int cmd_shaders(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    if (!shader_precompile_available()) {
        fprintf(cli_out, json ? "{\"available\":false}\n" : "Not on this platform: the shaders compile as the game draws them.\n");
        return 0;
    }
    int count = 0;
    char date[32] = "";
    int st = shader_precompile_status(&count, date, sizeof date);
    const char *state = st == 1 ? "done" : st == 2 ? "outdated" : "never";
    if (json) fprintf(cli_out, "{\"available\":true,\"state\":\"%s\",\"shaders\":%d,\"date\":\"%s\"}\n", state, count, date);
    else if (st == 0) fprintf(cli_out, "Not precompiled yet (savagewind precompile-shaders).\n");
    else
        fprintf(cli_out, "%d shaders precompiled on %s%s\n", count, date,
                st == 2 ? ", for an older version or driver: precompile them again (savagewind precompile-shaders)" : ".");
    return 0;
}

static int cmd_update(int argc, char **argv) {
    bool json = cli_has(argc, argv, "--json");
    char version[64] = "", message[512] = "";
    if (!post_update_shipped()) {  // (a source build: no SW Update, nothing released to update it to)
        if (json) fprintf(cli_out, "{\"checked\":false,\"available\":false,\"version\":\"\",\"message\":\"source build\"}\n");
        else fprintf(cli_out, "This is a source build: update it with git and make, not SW Update.\n");
        return 1;
    }
    update_check_start();
    UpdateState s = UPDATE_CHECKING;
    for (int waited = 0; waited < 60000 && !cli_stop_requested(); waited += 100) {
        s = update_check_state(version, sizeof version, message, sizeof message);
        if (s != UPDATE_CHECKING && s != UPDATE_IDLE) break;
        SDL_Delay(100);
    }
    bool checked = s == UPDATE_AVAILABLE || s == UPDATE_CURRENT;
    if (json) {
        fprintf(cli_out, "{\"checked\":%s,\"available\":%s,\"version\":", checked ? "true" : "false", s == UPDATE_AVAILABLE ? "true" : "false");
        cli_json_str(cli_out, version);
        fprintf(cli_out, ",\"message\":");
        cli_json_str(cli_out, message);
        fprintf(cli_out, "}\n");
    } else if (s == UPDATE_AVAILABLE) fprintf(cli_out, "Savage Wind %s is out. Savage Wind Configuration installs it.\n", version);
    else if (s == UPDATE_CURRENT) fprintf(cli_out, "Savage Wind is up to date.\n");
    else fprintf(cli_out, "Couldn't check for updates%s%s\n", message[0] ? ": " : ".", message);
    return checked ? 0 : 1;
}

static int cmd_log(int argc, char **argv) {
    char path[1100];
    snprintf(path, sizeof path, "%s", plat_log_path());
    if (!path[0]) { fprintf(cli_out, "This run's log goes to the terminal, not a file.\n"); return 1; }
    if (cli_has(argc, argv, "--previous")) {
        char *dot = strrchr(path, '.');
        if (dot) snprintf(dot, sizeof path - (size_t)(dot - path), ".prev.log");
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(cli_out, "No log yet (%s)\n", path); return 1; }
    char buf[65536];
    size_t n;
    bool follow = cli_has(argc, argv, "--follow");
    if (follow) cli_catch_stop();
    for (;;) {
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, cli_out);
        fflush(cli_out);
        if (!follow || cli_stop_requested()) break;
        clearerr(f);
        SDL_Delay(250);
    }
    fclose(f);
    return 0;
}

int cli_run(const char *cmd, int argc, char **argv) {
    int rc = 2;
    if (!strcmp(cmd, "settings")) rc = cmd_settings(argc, argv);
    else if (!strcmp(cmd, "launch-options")) rc = cmd_launch_options(argc, argv);
    else if (!strcmp(cmd, "saves")) rc = cmd_saves(argc, argv);
    else if (!strcmp(cmd, "paths")) rc = cmd_paths(argc, argv);
    else if (!strcmp(cmd, "shaders")) rc = cmd_shaders(argc, argv);
    else if (!strcmp(cmd, "update")) rc = cmd_update(argc, argv);
    else if (!strcmp(cmd, "log")) rc = cmd_log(argc, argv);
    fflush(cli_out);
    return rc;
}
