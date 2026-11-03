// The in-game console without the drawing (runtime/ui/console.mm draws it): the lines on screen, the history of what
// was typed, and tab completion. Plain C so tools/test_console_core.c can test it. UI thread only.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { CON_INPUT, CON_TEXT, CON_ERROR, CON_NOTE } ConKind;  // what was typed, a reply, a failed reply, a remark

// ---- the lines on screen (the newest CON_LINES stay)
#define CON_LINES 400
void console_clear(void);
void console_print(ConKind kind, const char *text);  // text may hold \n: one line each; empty lines are kept
int console_line_count(void);
const char *console_line(int index, ConKind *kind);  // 0 is the oldest; NULL out of range
unsigned console_revision(void);                     // changes whenever a line was added (to stick to the bottom)

// ---- history of typed lines (newest last, no repeats of the last one, at most CON_HISTORY)
#define CON_HISTORY 64
void console_history_add(const char *line);
// Up (dir -1) or down (dir +1) through the history, starting from `typed` (what is in the field now, kept so down
// brings it back). Returns the text to show, or NULL when there is no further entry in that direction.
const char *console_history_step(int dir, const char *typed);
void console_history_rewind(void);                   // typing or sending ends a walk through the history

// ---- completion of the first word. `names` are the candidate commands (sorted), `count` how many.
// Returns how many match what is typed; `out` gets the text the field should show: the one match and a space, or
// the longest common start of the matches; unchanged when nothing matches. Only a first word (no space yet) is completed.
int console_complete(const char *typed, const char *const *names, int count, char *out, size_t cap);
