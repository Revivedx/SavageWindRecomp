// Control commands: the registry, the local socket and the queue other front ends use (runtime/control.h). Only the socket thread touches sockets; handlers run at the frame boundary.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "control.h"

enum { MAX_COMMANDS = 128, MAX_CLIENTS = 4, MAX_LINE = 4096, MAX_PENDING = 64, MAX_JSON = 1024 * 1024 };
#ifdef _WIN32
typedef SOCKET ControlSocket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
static bool socket_again(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR; }
static bool nonblocking(ControlSocket s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
#else
typedef int ControlSocket;
#define BAD_SOCKET (-1)
#define close_socket close
static bool socket_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static bool nonblocking(ControlSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    return s < FD_SETSIZE && flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

typedef struct { char *s; size_t len, cap; bool bad; } Json;
struct ControlReply { Json data; Json lines; char *copy; char *error; bool failed, exit_after; };
typedef struct Request {
    struct Request *next;
    int client, argc;
    uint64_t generation;
    char *line, **argv, *reply;
    const char *error;
    size_t len, sent;
    bool exit_after;
} Request;
typedef struct {
    ControlSocket socket;
    uint64_t generation;
    char line[MAX_LINE + 1], input[MAX_LINE];
    size_t used, pos, end, bytes;
    int pending;
    bool dropping, eof;
    bool checked;  // the first line was looked at (not a web page's request: http_request)
    Request *head, *tail;
} Connection;

static ControlCmd commands[MAX_COMMANDS];
static int command_count;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static Request *requests, *requests_tail, *finished, *finished_tail;
static _Atomic bool enabled, queued;  // enabled: developer mode (see control_enabled); the socket is up when listener is set
static bool dev_mode, initialized;
static bool running_for_console;  // (a handler is running for the console: help lists only what it may run)
static ControlSocket listener = BAD_SOCKET;
static Connection clients[MAX_CLIENTS];  // owned by the socket thread
static int inflight;

static void append(Json *j, const char *s, size_t n) {
    if (j->bad) return;
    if (n > MAX_JSON - j->len) { j->bad = true; return; }
    size_t need = j->len + n + 1;
    if (need > j->cap) {
        size_t cap = need * 2;
        if (cap > MAX_JSON + 1) cap = MAX_JSON + 1;
        char *p = realloc(j->s, cap);
        if (!p) { j->bad = true; return; }
        j->s = p;
        j->cap = cap;
    }
    memcpy(j->s + j->len, s, n);
    j->len += n;
    j->s[j->len] = 0;
}
static void text(Json *j, const char *s) { append(j, s, strlen(s)); }
static void string(Json *j, const char *s) {
    text(j, "\"");
    const unsigned char *p = (const unsigned char *)(s ? s : ""), *start = p;
    for (; *p && !j->bad; p++) {
        if (*p >= 0x20 && *p != '"' && *p != '\\') continue;
        append(j, (const char *)start, (size_t)(p - start));
        if (*p == '"' || *p == '\\') { char esc[] = {'\\', (char)*p}; append(j, esc, 2); }
        else { char esc[7]; snprintf(esc, sizeof esc, "\\u%04x", *p); text(j, esc); }
        start = p + 1;
    }
    append(j, (const char *)start, (size_t)(p - start));
    text(j, "\"");
}
static void field(ControlReply *r, const char *key) {
    if (r->data.len) text(&r->data, ",");
    string(&r->data, key);
    text(&r->data, ":");
}
void control_field_str(ControlReply *r, const char *key, const char *value) {
    field(r, key); string(&r->data, value);
}
void control_field_int(ControlReply *r, const char *key, int64_t value) {
    char s[32]; snprintf(s, sizeof s, "%" PRId64, value);
    field(r, key); text(&r->data, s);
}
void control_field_num(ControlReply *r, const char *key, double value) {
    char s[128];
    if (!isfinite(value)) strcpy(s, "null");
    else {
        snprintf(s, sizeof s, "%.17g", value);
        // The runtime can use the system locale, but JSON always has a decimal point.
        const char *point = localeconv()->decimal_point;
        char *p = *point ? strstr(s, point) : NULL;
        if (p && strcmp(point, ".")) { memmove(p + 1, p + strlen(point), strlen(p + strlen(point)) + 1); *p = '.'; }
    }
    field(r, key); text(&r->data, s);
}
void control_field_bool(ControlReply *r, const char *key, bool value) {
    field(r, key); text(&r->data, value ? "true" : "false");
}
void control_fail(ControlReply *r, const char *fmt, ...) {
    if (r->failed) return;
    r->failed = true;
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n >= 0 && n < MAX_JSON / 2) {
        r->error = malloc((size_t)n + 1);
        if (r->error) vsnprintf(r->error, (size_t)n + 1, fmt, ap);
    }
    va_end(ap);
}

void control_text(ControlReply *r, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (r->lines.len) text(&r->lines, "\n");
    text(&r->lines, line);
}
void control_copy(ControlReply *r, const char *copy) {
    free(r->copy);
    r->copy = copy ? strdup(copy) : NULL;
}

bool control_reply_failed(const ControlReply *r) { return r->failed; }

void control_register(const ControlCmd *cmds, int count) {
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < count; i++) {
        const ControlCmd *c = &cmds[i];
        if (!c->name || !*c->name || !c->fn) { LOG("[control] invalid command registration\n"); continue; }
        bool duplicate = false;
        for (int k = 0; k < command_count; k++) if (!strcmp(commands[k].name, c->name)) duplicate = true;
        if (duplicate) { LOG("[control] duplicate command: %s\n", c->name); continue; }
        if (command_count == MAX_COMMANDS) { LOG("[control] command table full: %s\n", c->name); continue; }
        commands[command_count++] = *c;
    }
    pthread_mutex_unlock(&registry_lock);
}
// What the caller may run: the socket and developer mode run everything; a player's console only the player safe commands.
static bool allowed(const ControlCmd *c, bool console) { return !console || dev_mode || (c->flags & CONTROL_PLAYER); }
static int by_name(const void *a, const void *b) { return strcmp((*(const ControlCmd *const *)a)->name, (*(const ControlCmd *const *)b)->name); }
static void help(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    ControlCmd snapshot[MAX_COMMANDS];
    pthread_mutex_lock(&registry_lock);
    int count = command_count;
    memcpy(snapshot, commands, (size_t)count * sizeof *snapshot);
    pthread_mutex_unlock(&registry_lock);
    field(r, "commands");
    text(&r->data, "[");
    int shown = 0;
    size_t wide = 0;
    for (int i = 0; i < count; i++) {
        if (!allowed(&snapshot[i], running_for_console)) continue;
        if (shown++) text(&r->data, ",");
        text(&r->data, "{\"name\":"); string(&r->data, snapshot[i].name);
        text(&r->data, ",\"args\":"); string(&r->data, snapshot[i].args);
        text(&r->data, ",\"help\":"); string(&r->data, snapshot[i].help);
        text(&r->data, "}");
        size_t w = strlen(snapshot[i].name) + (*snapshot[i].args ? 1 + strlen(snapshot[i].args) : 0);
        if (w > wide) wide = w;
    }
    text(&r->data, "]");
    // the console's table: sorted, the usage and the one line description lined up
    const ControlCmd *sorted[MAX_COMMANDS];
    int n = 0;
    for (int i = 0; i < count; i++) if (allowed(&snapshot[i], running_for_console)) sorted[n++] = &snapshot[i];
    qsort(sorted, (size_t)n, sizeof *sorted, by_name);
    if (wide > 40) wide = 40;
    for (int i = 0; i < n; i++) {
        char usage[160];
        snprintf(usage, sizeof usage, "%s%s%s", sorted[i]->name, *sorted[i]->args ? " " : "", sorted[i]->args);
        control_text(r, "%-*s  %s", (int)wide, usage, sorted[i]->help);
    }
}
static void ping(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    control_field_bool(r, "pong", true);
}
static void quit(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    r->exit_after = true;
}

static void push(Request **head, Request **tail, Request *q) {
    q->next = NULL;
    if (*tail) (*tail)->next = q;
    else *head = q;
    *tail = q;
}
static void free_request(Request *q) {
    free(q->argv); free(q->line); free(q->reply); free(q);
    inflight--;
}
static bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
// Split a NUL terminated line into words in place: double quotes keep spaces in one word, \" and \\ work inside them.
// argv needs room for strlen(line) / 2 + 1 words. Returns NULL, or what is wrong with the line.
static const char *tokenize(char *line, char **argv, int *argc) {
    char *src = line, *dst = line;
    const char *error = NULL;
    *argc = 0;
    while (*src && !error) {
        while (space(*src)) src++;
        if (!*src) break;
        argv[(*argc)++] = dst;
        bool quoted = false;
        while (*src && (quoted || !space(*src))) {
            if (*src == '"') { quoted = !quoted; src++; }
            else if (quoted && *src == '\\' && (src[1] == '"' || src[1] == '\\')) { src++; *dst++ = *src++; }
            else *dst++ = *src++;
        }
        if (quoted) error = "unterminated double quote";
        while (space(*src)) src++;
        *dst++ = 0;
    }
    return error;
}
static bool enqueue(int slot, const char *error) {
    Connection *c = &clients[slot];
    Request *q = calloc(1, sizeof *q);
    if (!q) return false;
    q->client = slot;
    q->generation = c->generation;
    q->error = error;
    if (!error) {
        q->line = malloc(c->used + 1);
        q->argv = malloc((c->used + 1) * sizeof *q->argv);
        if (!q->line || !q->argv) { free(q->line); free(q->argv); free(q); return false; }
        memcpy(q->line, c->line, c->used);
        q->line[c->used] = 0;
        if (memchr(q->line, 0, c->used)) q->error = "request contains NUL";
        else q->error = tokenize(q->line, q->argv, &q->argc);
        if (!q->argc && !q->error) { free(q->line); free(q->argv); free(q); return true; }
    }
    inflight++;
    c->pending++;
    pthread_mutex_lock(&queue_lock);
    push(&requests, &requests_tail, q);
    queued = true;
    pthread_mutex_unlock(&queue_lock);
    return true;
}

bool control_enabled(void) { return dev_mode; }
bool control_endpoint_on(void) { return enabled; }

// Run one command (on the frame hook). console: the caller is the in-game console, not the socket.
static void execute(int argc, char **argv, bool console, ControlReply *r) {
    ControlFn fn = NULL;
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < command_count; i++)
        if (!strcmp(commands[i].name, argv[0]) && allowed(&commands[i], console)) { fn = commands[i].fn; break; }
    pthread_mutex_unlock(&registry_lock);
    if (!fn) { control_fail(r, "Unknown command: %s", argv[0]); return; }
    running_for_console = console;
    fn(argc, argv, r);
    running_for_console = false;
}
static void reply_free(ControlReply *r) { free(r->data.s); free(r->lines.s); free(r->copy); free(r->error); }

// ---- the reply as one JSON line
static char *reply_json(ControlReply *r, size_t *len) {
    Json out = {0};
    if (r->failed || r->data.bad) {
        text(&out, "{\"ok\":false,\"error\":");
        string(&out, r->data.bad ? "reply too large or out of memory" : r->error ? r->error : "command failed");
        text(&out, "}\n");
    } else {
        text(&out, "{\"ok\":true,\"data\":{");
        if (r->data.len) append(&out, r->data.s, r->data.len);
        text(&out, "}}\n");
    }
    if (out.bad) {
        free(out.s);
        out.s = strdup("{\"ok\":false,\"error\":\"reply too large or out of memory\"}\n");
        out.len = out.s ? strlen(out.s) : 0;
    }
    *len = out.len;
    return out.s;
}

// ---- the reply as lines for the console: `key: value`, nested keys dotted, a list of objects one line each
// (the JSON is our own, so it is well formed; this reads just enough of it)
typedef struct { const char *p; Json *out; char prefix[160]; } Flat;
static void flat_ws(Flat *f) { while (*f->p == ' ' || *f->p == ',' || *f->p == '\n') f->p++; }
static void flat_string(Flat *f, Json *dst) {  // at the opening quote; appends the unescaped text
    f->p++;
    while (*f->p && *f->p != '"') {
        if (*f->p == '\\' && f->p[1]) {
            f->p++;
            char c = *f->p++;
            if (c == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4 && f->p[i]; i++) v = v * 16 + (unsigned)(f->p[i] <= '9' ? f->p[i] - '0' : (f->p[i] | 32) - 'a' + 10);
                f->p += 4;
                char ch = v < 0x80 ? (char)v : '?';
                append(dst, &ch, 1);
            } else { char ch = c == 'n' ? '\n' : c == 't' ? '\t' : c; append(dst, &ch, 1); }
        } else append(dst, f->p++, 1);
    }
    if (*f->p == '"') f->p++;
}
static void flat_scalar(Flat *f, Json *dst) {  // a string, number, true, false or null
    if (*f->p == '"') { flat_string(f, dst); return; }
    const char *start = f->p;
    while (*f->p && *f->p != ',' && *f->p != '}' && *f->p != ']') f->p++;
    size_t n = (size_t)(f->p - start);
    char tok[64];
    char *end;
    bool plain = n > 0 && n < sizeof tok;
    if (plain) { memcpy(tok, start, n); tok[n] = 0; }
    // 17 digit doubles print as 0.10000000000000001: show 7 significant digits instead (whole numbers stay as they are)
    if (plain && memchr(tok, '.', n)) {
        double v = strtod(tok, &end);
        if (end == tok + n) {
            char pretty[40];
            snprintf(pretty, sizeof pretty, "%.7g", v);
            const char *point = localeconv()->decimal_point;
            char *q = strstr(pretty, point);
            if (q && strcmp(point, ".")) { memmove(q + 1, q + strlen(point), strlen(q + strlen(point)) + 1); *q = '.'; }
            text(dst, pretty);
            return;
        }
    }
    append(dst, start, n);
}
static void flat_value(Flat *f, const char *key);
static void flat_object(Flat *f, const char *prefix) {  // at '{'
    f->p++;
    for (;;) {
        flat_ws(f);
        if (*f->p != '"') break;
        Json k = {0};
        flat_string(f, &k);
        while (*f->p == ' ' || *f->p == ':') f->p++;
        char full[160];
        snprintf(full, sizeof full, "%s%s%s", prefix, *prefix ? "." : "", k.s ? k.s : "");
        free(k.s);
        flat_value(f, full);
    }
    if (*f->p == '}') f->p++;
}
static void flat_line(Flat *f, const char *key, const char *value) {
    if (f->out->len) text(f->out, "\n");
    text(f->out, key);
    text(f->out, ": ");
    text(f->out, value);
}
static void flat_value(Flat *f, const char *key) {
    if (*f->p == '{') { flat_object(f, key); return; }
    if (*f->p == '[') {
        f->p++;
        int index = 0;
        for (;;) {
            flat_ws(f);
            if (*f->p == ']' || !*f->p) break;
            char full[200];
            snprintf(full, sizeof full, "%s.%d", key, index++);
            if (*f->p == '{') {  // an object in a list: its members on one line
                Json one = {0};
                Json *keep = f->out;
                f->out = &one;
                flat_object(f, "");
                f->out = keep;
                for (char *c = one.s; c && *c; c++) if (*c == '\n') *c = ' ';
                flat_line(f, full, one.s ? one.s : "");
                free(one.s);
            } else {
                Json v = {0};
                flat_scalar(f, &v);
                flat_line(f, full, v.s ? v.s : "");
                free(v.s);
            }
        }
        if (*f->p == ']') f->p++;
        return;
    }
    Json v = {0};
    flat_scalar(f, &v);
    flat_line(f, key, v.s ? v.s : "");
    free(v.s);
}
static char *reply_lines(ControlReply *r) {
    if (r->failed) return strdup(r->error ? r->error : "command failed");
    if (r->data.bad) return strdup("reply too large or out of memory");
    if (r->lines.len) return strdup(r->lines.s);
    if (!r->data.len) return strdup("ok");
    Json wrapped = {0}, out = {0};
    text(&wrapped, "{");
    append(&wrapped, r->data.s, r->data.len);
    text(&wrapped, "}");
    Flat f = {wrapped.s, &out, {0}};
    if (!wrapped.bad && wrapped.s) flat_object(&f, "");
    free(wrapped.s);
    if (out.bad || !out.s) { free(out.s); return strdup("reply too large or out of memory"); }
    return out.s;
}

// ---- the console's queue: lines in from the UI thread, results out; commands run in control_frame()
typedef struct Line { struct Line *next; char *text; ControlConsoleResult result; } Line;
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
static Line *console_in, *console_in_tail, *console_out, *console_out_tail;
static int console_pending;
enum { CONSOLE_MAX_PENDING = 16, CONSOLE_MAX_LINE = 1024 };

void control_exec_line(const char *line, bool console, ControlConsoleResult *out) {
    memset(out, 0, sizeof *out);
    size_t n = strlen(line);
    char *copy = malloc(n + 1);
    char **argv = malloc((n / 2 + 2) * sizeof *argv);
    ControlReply r = {0};
    int argc = 0;
    if (!copy || !argv) control_fail(&r, "Out of memory");
    else {
        memcpy(copy, line, n + 1);
        const char *error = tokenize(copy, argv, &argc);
        if (error) control_fail(&r, "%s", error);
        else if (!argc) control_fail(&r, "Empty command");
        else execute(argc, argv, console, &r);
    }
    out->failed = r.failed || r.data.bad;
    out->exit_after = r.exit_after && !r.failed;
    out->text = reply_lines(&r);
    out->copy = r.copy; r.copy = NULL;
    reply_free(&r);
    free(argv); free(copy);
}
bool control_console_submit(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || strlen(line) >= CONSOLE_MAX_LINE) return false;
    Line *q = calloc(1, sizeof *q);
    if (!q) return false;
    q->text = strdup(line);
    if (!q->text) { free(q); return false; }
    pthread_mutex_lock(&console_lock);
    bool room = console_pending < CONSOLE_MAX_PENDING;
    if (room) {
        console_pending++;
        if (console_in_tail) console_in_tail->next = q; else console_in = q;
        console_in_tail = q;
    }
    pthread_mutex_unlock(&console_lock);
    if (!room) { free(q->text); free(q); }
    return room;
}
static void console_pump(void) {
    pthread_mutex_lock(&console_lock);
    Line *batch = console_in;
    console_in = console_in_tail = NULL;
    pthread_mutex_unlock(&console_lock);
    while (batch) {
        Line *q = batch;
        batch = q->next;
        q->next = NULL;
        control_exec_line(q->text, true, &q->result);
        free(q->text); q->text = NULL;
        if (q->result.exit_after) { LOG("console: exit\n"); fflush(stderr); exit(0); }  // (like the window's close button, so the game's normal exit runs)
        pthread_mutex_lock(&console_lock);
        if (console_out_tail) console_out_tail->next = q; else console_out = q;
        console_out_tail = q;
        pthread_mutex_unlock(&console_lock);
    }
}
bool control_console_poll(ControlConsoleResult *out) {
    pthread_mutex_lock(&console_lock);
    Line *q = console_out;
    if (q) {
        console_out = q->next;
        if (!console_out) console_out_tail = NULL;
        console_pending--;
    }
    pthread_mutex_unlock(&console_lock);
    if (!q) return false;
    *out = q->result;
    free(q);
    return true;
}
void control_console_free(ControlConsoleResult *r) { free(r->text); free(r->copy); memset(r, 0, sizeof *r); }
int control_console_names(const char *prefix, const char **out, int max) {
    const ControlCmd *hit[MAX_COMMANDS];
    int n = 0;
    size_t len = strlen(prefix);
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < command_count; i++)
        if (allowed(&commands[i], true) && !strncmp(commands[i].name, prefix, len)) hit[n++] = &commands[i];
    pthread_mutex_unlock(&registry_lock);  // (the table only grows, and names are string literals: the pointers stay valid)
    qsort(hit, (size_t)n, sizeof *hit, by_name);
    for (int i = 0; i < n && i < max; i++) out[i] = hit[i]->name;
    return n;
}

void control_frame(uint64_t frame) {
    if (!initialized) return;
    control_place_frame();
    if (dev_mode) control_game_frame(frame);
    console_pump();
    if (!queued) return;
    pthread_mutex_lock(&queue_lock);
    Request *batch = requests;
    requests = requests_tail = NULL;
    queued = false;
    pthread_mutex_unlock(&queue_lock);
    while (batch) {
        Request *q = batch;
        batch = q->next;
        ControlReply r = {0};
        if (q->error) control_fail(&r, "%s", q->error);
        else execute(q->argc, q->argv, false, &r);
        q->reply = reply_json(&r, &q->len);
        q->exit_after = r.exit_after;
        reply_free(&r);
        pthread_mutex_lock(&queue_lock);
        push(&finished, &finished_tail, q);
        pthread_mutex_unlock(&queue_lock);
    }
}

static void disconnect(int slot) {
    Connection *c = &clients[slot];
    close_socket(c->socket);
    while (c->head) { Request *q = c->head; c->head = q->next; free_request(q); }
    uint64_t generation = c->generation + 1;
    memset(c, 0, sizeof *c);
    c->socket = BAD_SOCKET;
    c->generation = generation;
}
static void stop(void) { fflush(stderr); _exit(0); }
// A web page in the player's browser can send to 127.0.0.1 too: an HTTP request whose body is lines of commands
// (fetch with no-cors, a form, a WebSocket handshake). Such a connection starts with an HTTP request line; it is
// closed before any of its lines run. (The protocol's own first line is a command, never "WORD /path" or "HTTP/".)
static bool http_request(const char *line, size_t n) {
    static const char *const methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ",
                                          "CONNECT ", "TRACE "};
    for (size_t i = 0; i < sizeof methods / sizeof *methods; i++) {
        size_t m = strlen(methods[i]);
        if (n >= m && !memcmp(line, methods[i], m)) return true;
    }
    for (size_t i = 0; i + 5 <= n; i++)
        if (!memcmp(line + i, "HTTP/", 5)) return true;
    return false;
}
static bool buffered(int slot) {
    Connection *c = &clients[slot];
    while (c->pos < c->end && c->pending < MAX_PENDING && inflight < MAX_CLIENTS * MAX_PENDING) {
        char ch = c->input[c->pos++];
        if (!c->checked && (ch == '\n' || c->used == MAX_LINE)) {
            c->checked = true;
            if (http_request(c->line, c->used)) {
                LOG("[control] closed a connection that sent an HTTP request (a web page?)\n");
                return false;
            }
        }
        if (ch == '\n') {
            if (!c->dropping && !enqueue(slot, NULL)) return false;
            c->used = 0; c->dropping = false;
        } else if (!c->dropping) {
            if (c->used == MAX_LINE) {
                if (!enqueue(slot, "line exceeds 4096 bytes")) return false;
                c->used = 0; c->dropping = true;
            } else c->line[c->used++] = ch;
        }
    }
    return true;
}
static void *serve(void *arg) {
    (void)arg;
    uint64_t exit_deadline = 0;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        Request *batch = finished;
        finished = finished_tail = NULL;
        pthread_mutex_unlock(&queue_lock);
        while (batch) {
            Request *q = batch;
            batch = q->next;
            Connection *c = &clients[q->client];
            if (q->exit_after && !exit_deadline) exit_deadline = plat_now_ns() + 1000000000;
            if (c->socket == BAD_SOCKET || c->generation != q->generation) { free_request(q); continue; }
            if (!q->reply || q->len > MAX_JSON - c->bytes) { int slot = q->client; free_request(q); disconnect(slot); continue; }
            c->bytes += q->len;
            push(&c->head, &c->tail, q);
        }
        // Give exit's reply a bounded chance to drain, even if its client stopped reading.
        if (exit_deadline && plat_now_ns() >= exit_deadline) stop();
        fd_set reads, writes;
        FD_ZERO(&reads); FD_ZERO(&writes);
        FD_SET(listener, &reads);
        ControlSocket high = listener;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Connection *c = &clients[i];
            if (c->socket == BAD_SOCKET) continue;
            if (!buffered(i) || (c->eof && !c->pending)) { disconnect(i); continue; }
            if (!c->eof && c->pos == c->end && c->pending < MAX_PENDING && inflight < MAX_CLIENTS * MAX_PENDING)
                FD_SET(c->socket, &reads);
            if (c->head) FD_SET(c->socket, &writes);
            if (c->socket > high) high = c->socket;
        }
        struct timeval timeout = {0, 10000};
        if (select((int)(high + 1), &reads, &writes, NULL, &timeout) < 0) continue;
        if (FD_ISSET(listener, &reads)) {
            ControlSocket s = accept(listener, NULL, NULL);
            if (s != BAD_SOCKET) {
                int slot = 0;
                while (slot < MAX_CLIENTS && clients[slot].socket != BAD_SOCKET) slot++;
                if (slot == MAX_CLIENTS || !nonblocking(s)) close_socket(s);
                else {
#ifdef SO_NOSIGPIPE
                    int on = 1;
                    if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on)) { close_socket(s); continue; }
#endif
                    clients[slot].socket = s;
                    // This socket was not in this select call's sets (a descriptor may have been reused).
                    FD_CLR(s, &reads); FD_CLR(s, &writes);
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Connection *c = &clients[i];
            if (c->socket == BAD_SOCKET) continue;
            if (FD_ISSET(c->socket, &writes)) {
                Request *q = c->head;
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                int n = (int)send(c->socket, q->reply + q->sent, (int)(q->len - q->sent), flags);
                if (n < 0 && socket_again()) continue;
                if (n <= 0) { disconnect(i); continue; }
                q->sent += (size_t)n; c->bytes -= (size_t)n;
                if (q->sent == q->len) {
                    bool exit_after = q->exit_after;
                    c->head = q->next;
                    if (!c->head) c->tail = NULL;
                    c->pending--; free_request(q);
                    if (exit_after) stop();
                }
            }
            if (FD_ISSET(c->socket, &reads)) {
                int n = (int)recv(c->socket, c->input, sizeof c->input, 0);
                if (n < 0 && socket_again()) continue;
                if (n < 0) { disconnect(i); continue; }
                if (!n) { c->eof = true; continue; }  // allow a write-half-closed client to read its replies
                c->pos = 0; c->end = (size_t)n;
                if (!buffered(i)) disconnect(i);
            }
        }
    }
    return NULL;
}

// Developer mode: the socket (SAVAGEWIND_CONTROL=<port>) or the console's developer commands alone (SAVAGEWIND_DEV=1).
static bool env_on(const char *name) { const char *v = getenv(name); return v && *v && *v != '0'; }

static bool listen_on(unsigned value) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { LOG("[control] Winsock initialization failed\n"); return false; }
#endif
    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)value);
    bool ready = listener != BAD_SOCKET && nonblocking(listener);
    int on = 1;
#ifdef _WIN32
    // (no other program may bind the same port and take the connections meant for the game)
    if (ready) ready = setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on) == 0;
#else
    if (ready) ready = setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0;
#endif
    if (!ready || bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, MAX_CLIENTS)) {
        LOG("[control] cannot listen on 127.0.0.1:%u\n", value);
        if (listener != BAD_SOCKET) close_socket(listener);
        listener = BAD_SOCKET;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].socket = BAD_SOCKET;
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, NULL)) {
        LOG("[control] cannot start socket thread\n");
        close_socket(listener); listener = BAD_SOCKET;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    pthread_detach(thread);
    return true;
}

void control_init(void) {
    const char *port = getenv("SAVAGEWIND_CONTROL");
    unsigned value = 0;
    bool serve_socket = false;
    if (port) {
        const char *p = port;
        while (*p >= '0' && *p <= '9' && value <= 65535) value = value * 10 + (unsigned)(*p++ - '0');
        if (!*port || *p || !value || value > 65535) LOG("[control] invalid port: %s\n", port);
        else serve_socket = true;
    }
    if (serve_socket && listen_on(value)) enabled = true;
    dev_mode = enabled || env_on("SAVAGEWIND_DEV");
    static const ControlCmd builtins[] = {
        {"help", "", "List the commands", help, CONTROL_PLAYER},
        {"ping", "", "Check that the game answers", ping, 0},
        {"exit", "", "Close the game", quit, CONTROL_PLAYER},
    };
    control_register(builtins, sizeof builtins / sizeof *builtins);
    control_game_init();  // (the player safe commands always; the developer ones only in developer mode)
    initialized = true;
    if (enabled) LOG("[control] listening on 127.0.0.1:%u\n", value);
    else if (dev_mode) LOG("[control] developer commands on in the console (SAVAGEWIND_DEV)\n");
}
