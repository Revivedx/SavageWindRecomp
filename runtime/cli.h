// savagewind <command>: the start screen's actions from a terminal, a script or another launcher (docs/cli.md).
// runtime/cli.c has the commands and their help; the ones that need the start screen's own checks (setup, status,
// verify, delete) are in runtime/ui/launcher.mm, and main.c starts them all.
#pragma once
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// The commands' own output: stdout as it was when the program started (plat_init sends stdout to the log).
extern FILE *cli_out;
void cli_keep_stdout(void);  // before plat_init
// Whether the command starts with the saved launch options (runtime/launch_flags.h): the ones that start the game or
// its build. The others only read or change files.
bool cli_applies_launch_options(const char *cmd);

// The command an argument names ("install" -> "setup"), or NULL: a first argument that isn't a command keeps its
// old meaning (the game files folder).
const char *cli_command(const char *arg);
// Commands that read their own arguments (settings get <key>...): main.c doesn't look at them.
bool cli_owns_args(const char *cmd);
// help [<command>], completion <shell>: before anything starts. Exit status.
int cli_help(const char *topic);
int cli_completion(const char *shell);
// settings, launch-options, saves, paths, shaders, update, log: argv are the command's own arguments. Exit status.
int cli_run(const char *cmd, int argc, char **argv);

// JSON output: a string, quoted and escaped.
void cli_json_str(FILE *f, const char *s);
bool cli_has(int argc, char **argv, const char *opt);

// Ctrl+C / termination asked for (installed by cli_catch_stop): long commands stop at the next chance.
void cli_catch_stop(void);
bool cli_stop_requested(void);

// (runtime/ui/launcher.mm)
int launcher_setup_cli(const char *root, const char *src, bool json);
int launcher_status_cli(const char *root, bool json);
int launcher_verify_cli(const char *root, bool json);
int launcher_delete_cli(const char *root);

#ifdef __cplusplus
}
#endif
