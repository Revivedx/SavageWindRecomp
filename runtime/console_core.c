// The in-game console without the drawing: lines, history, completion (runtime/console_core.h).
#include "console_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { ConKind kind; char *text; } Line;
static Line lines[CON_LINES];
static int first, count;  // a ring: lines[(first + i) % CON_LINES]
static unsigned revision;

static void add_line(ConKind kind, const char *text, size_t len) {
    int slot = (first + count) % CON_LINES;
    if (count == CON_LINES) { free(lines[first].text); first = (first + 1) % CON_LINES; }
    else count++;
    char *copy = malloc(len + 1);
    if (copy) { memcpy(copy, text, len); copy[len] = 0; }
    lines[slot].kind = kind;
    lines[slot].text = copy;
    revision++;
}
void console_print(ConKind kind, const char *text) {
    if (!text) return;
    for (;;) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        if (len && text[len - 1] == '\r') len--;
        add_line(kind, text, len);
        if (!eol) break;
        text = eol + 1;
    }
}
void console_clear(void) {
    for (int i = 0; i < count; i++) free(lines[(first + i) % CON_LINES].text);
    first = count = 0;
    revision++;
}
int console_line_count(void) { return count; }
const char *console_line(int index, ConKind *kind) {
    if (index < 0 || index >= count) return NULL;
    Line *l = &lines[(first + index) % CON_LINES];
    if (kind) *kind = l->kind;
    return l->text ? l->text : "";
}
unsigned console_revision(void) { return revision; }

// ---- history
static char *history[CON_HISTORY];
static int history_count;
static int walk = -1;      // -1: not walking; else the index shown
static char *walk_typed;   // what was in the field when the walk began

void console_history_rewind(void) { walk = -1; free(walk_typed); walk_typed = NULL; }
void console_history_add(const char *line) {
    console_history_rewind();
    if (!line || !*line) return;
    if (history_count && !strcmp(history[history_count - 1], line)) return;
    if (history_count == CON_HISTORY) { free(history[0]); memmove(history, history + 1, (CON_HISTORY - 1) * sizeof *history); history_count--; }
    history[history_count] = strdup(line);
    if (history[history_count]) history_count++;
}
const char *console_history_step(int dir, const char *typed) {
    if (!history_count) return NULL;
    if (walk < 0) {
        if (dir > 0) return NULL;  // already on the new line
        free(walk_typed);
        walk_typed = strdup(typed ? typed : "");
        walk = history_count - 1;
        return history[walk];
    }
    if (dir < 0) {
        if (walk > 0) walk--;
        return history[walk];
    }
    if (walk < history_count - 1) { walk++; return history[walk]; }
    walk = -1;  // past the newest: the line that was being typed
    return walk_typed ? walk_typed : "";
}

// ---- completion
int console_complete(const char *typed, const char *const *names, int count_in, char *out, size_t cap) {
    if (cap) { strncpy(out, typed, cap - 1); out[cap - 1] = 0; }
    if (strchr(typed, ' ')) return 0;
    size_t len = strlen(typed);
    int matches = 0;
    const char *first_match = NULL;
    size_t common = 0;
    for (int i = 0; i < count_in; i++) {
        if (strncmp(names[i], typed, len)) continue;
        if (!matches) { first_match = names[i]; common = strlen(names[i]); }
        else { size_t k = 0; while (k < common && names[i][k] == first_match[k]) k++; common = k; }
        matches++;
    }
    if (!matches || !cap) return matches;
    if (matches == 1) snprintf(out, cap, "%s ", first_match);
    else if (common > len) { size_t k = common < cap - 1 ? common : cap - 1; memcpy(out, first_match, k); out[k] = 0; }
    return matches;
}
