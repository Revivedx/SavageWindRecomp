// Control commands: one registry of named commands that more than one front end can run. The local socket (tools/swctl.py,
// scripts, AI agents; off unless SAVAGEWIND_CONTROL=<port> is set) runs all of them; the player safe ones (CONTROL_PLAYER)
// are also what a normal start offers a player. Protocol and command list: docs/CONTROL.md.
//
// Threads: the server thread only reads lines and queues them. Every command runs in control_frame(), once per
// presented frame on the thread that presents, so a command never runs in the middle of a frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct ControlReply ControlReply;  // what a command answers with (opaque; use the functions below)

// A command handler. argv[0] is the command name, argv[1..argc-1] the arguments (quotes already resolved).
// Answer with control_ok() / control_fail() (at most one; nothing counts as control_ok with no data).
typedef void (*ControlFn)(int argc, char **argv, ControlReply *r);

#define CONTROL_PLAYER 1u  // ControlCmd.flags: reads state or copies text, cannot break anything: any player may run it

typedef struct {
    const char *name;  // "time", "shot", ...; subcommands are plain arguments: "set hour 12" is name "set"
    const char *args;  // usage of the arguments for `help`, e.g. "<hour 0-24 | free>" ("" if none)
    const char *help;  // one line: what it does
    ControlFn fn;
    unsigned flags;    // CONTROL_PLAYER, or 0: developer mode only (writes memory or files, moves things, ends the game)
} ControlCmd;

// Register a table of commands (once, e.g. from control_game_init); the names must be unique.
void control_register(const ControlCmd *cmds, int count);

// Answers. A reply is a JSON object: control_field_*() add members to "data" (strings are escaped for you).
void control_field_str(ControlReply *r, const char *key, const char *value);
void control_field_int(ControlReply *r, const char *key, int64_t value);
void control_field_num(ControlReply *r, const char *key, double value);
void control_field_bool(ControlReply *r, const char *key, bool value);
// Front end extras, ignored by the socket (scripts read the fields): control_text() adds a line the console prints
// instead of listing the fields (when a command adds none, the console prints the fields as `key: value`);
// control_copy() is text the console puts on the clipboard (the last call wins).
void control_text(ControlReply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void control_copy(ControlReply *r, const char *text);
bool control_reply_failed(const ControlReply *r);  // has control_fail been called (a command that runs another one)
void control_fail(ControlReply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));  // {"ok":false,"error":...}

// Start the server if SAVAGEWIND_CONTROL is set (does nothing otherwise). Called once at startup.
void control_init(void);
// Once per presented frame: run the queued commands. Cheap when nothing is queued.
void control_frame(uint64_t frame);

// The game's commands (state, pos, camera, time, shot, ...), registered by control_init() through this.
void control_game_init(void);
// Called by control_frame() at the start of every frame, also when no command is queued, but only when the
// endpoint is on (the game side keeps its fps average with it).
void control_game_frame(uint64_t frame);

// Live input hook: frame publication on the presenter; apply may run on guest threads.
// A NULL lx requests buttons only. Otherwise all four axis pointers must be valid.
void control_input_frame(uint64_t frame);
bool control_enabled(void);      // developer mode (SAVAGEWIND_CONTROL=<port> or SAVAGEWIND_DEV=1): the commands that are not player safe
bool control_endpoint_on(void);  // is the socket server listening (SAVAGEWIND_CONTROL)?
void control_fly_wheel(float notches);  // mouse wheel while flying: faster / slower
bool control_fly_toggle(void);   // fly mode on/off (the hotkey, bound as bind_key_fly); false when it could not turn on
void control_input_apply(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

// ---- a front end that is not the socket (the in-game console): lines go in from its thread, run in control_frame()
typedef struct {
    char *text;     // what to print: lines separated by \n, no trailing newline (malloc; free with control_console_free)
    char *copy;     // for the clipboard, or NULL (malloc)
    bool failed;
    bool exit_after;  // the command was `exit`: the console front end ends the game
} ControlConsoleResult;

bool control_console_submit(const char *line);          // queue a line (any thread); false: queue full or empty
bool control_console_poll(ControlConsoleResult *out);   // next finished result, in order (any thread); false: none
void control_console_free(ControlConsoleResult *r);
// Names of the commands the console may run that start with `prefix`, sorted; returns the count and copies up to
// `max` pointers (valid for the life of the process).
int control_console_names(const char *prefix, const char **out, int max);
// For tests and `report`: run a line now on the calling thread, with the console's rights. Not for other threads.
void control_exec_line(const char *line, bool console, ControlConsoleResult *out);
