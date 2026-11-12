// Launch options without any UI or platform code: see launch_flags.h.
#include "launch_flags.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h"  // (setenv, unsetenv)
#endif

#define PREFIX "SAVAGEWIND_"

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = toupper((unsigned char)*a), y = toupper((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
}

static bool ci_prefix(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) return false;
    return true;
}

const char *lf_error_text(LfError e) {
    switch (e) {
    case LF_OK: return "";
    case LF_ERR_EMPTY: return "Type the name of the flag first.";
    case LF_ERR_BAD_NAME: return "A flag name has only letters, digits and underscores, and doesn't start with a digit.";
    case LF_ERR_NAME_LONG: return "That name is too long.";
    case LF_ERR_BAD_VALUE: return "The value can't have line breaks or other control characters.";
    case LF_ERR_VALUE_LONG: return "That value is too long.";
    case LF_ERR_INTERNAL: return "Savage Wind uses this one itself, so it can't be set here.";
    case LF_ERR_LAUNCHER: return "This one would skip or lock this screen, so it can't be set here.";
    case LF_ERR_BUILD: return "This one is for building the game code, not for playing, so it can't be set here.";
    case LF_ERR_SYSTEM: return "This is a system setting, so it can't be set here.";
    case LF_ERR_SWITCH_OFF:
        return "This flag is on whenever it is in the list, a 0 doesn't turn it off. Remove it from the list instead.";
    case LF_ERR_FULL: return "The list is full. Remove a flag first.";
    }
    return "";
}

const LfInfo *lf_find(const char *name) {
    for (int i = 0; i < LF_TABLE_N; i++)
        if (!ci_cmp(LF_TABLE[i].name, name)) return &LF_TABLE[i];
    return NULL;
}

static char *trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
    return s;
}

void lf_normalize_name(char *name) {
    trim(name);
    if (!ci_prefix(name, PREFIX) && strlen(name) + sizeof PREFIX <= LF_NAME_MAX + 8) {
        // A short name ("FPS_CAP") means the listed flag of that name, so it must not end up as a different variable
        char full[LF_NAME_MAX + 8];
        snprintf(full, sizeof full, PREFIX "%s", name);
        if (lf_find(full)) memcpy(name, full, strlen(full) + 1);
    }
    if (ci_prefix(name, PREFIX))
        for (char *c = name; *c; c++) *c = (char)toupper((unsigned char)*c);
}

void lf_normalize_value(char *value) { trim(value); }

// Variables the process (or the way it finds its files and window) depends on
static bool system_name(const char *n) {
    static const char *const names[] = {"PATH", "HOME", "USERPROFILE", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "TMPDIR",  // i18n-skip
                                        "SHELL", "USER", "LOGNAME", "DISPLAY", "WAYLAND_DISPLAY", "XDG_DATA_HOME",  // i18n-skip
                                        "XDG_RUNTIME_DIR", "APPDATA", "LOCALAPPDATA", "COMSPEC"};  // i18n-skip
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!ci_cmp(n, names[i])) return true;
    return ci_prefix(n, "LD_") || ci_prefix(n, "DYLD_");  // i18n-skip
}

bool lf_value_is_off(const char *v) {
    return !ci_cmp(v, "0") || !ci_cmp(v, "off") || !ci_cmp(v, "false") || !ci_cmp(v, "no");
}

LfError lf_check(const char *name, const char *value) {
    if (!name || !*name) return LF_ERR_EMPTY;
    size_t len = strlen(name);
    if (len >= LF_NAME_MAX) return LF_ERR_NAME_LONG;
    if (isdigit((unsigned char)name[0])) return LF_ERR_BAD_NAME;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') return LF_ERR_BAD_NAME;
    if (value) {
        if (strlen(value) >= LF_VALUE_MAX) return LF_ERR_VALUE_LONG;
        for (const char *c = value; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == 0x7f) return LF_ERR_BAD_VALUE;
    }
    if (system_name(name)) return LF_ERR_SYSTEM;
    if (ci_prefix(name, PREFIX "LAUNCHER_")) return LF_ERR_LAUNCHER;  // (any test hook of the start screen, listed or not)  // i18n-skip
    const LfInfo *i = lf_find(name);
    if (i) {
        if (i->group == LF_GROUP_INTERNAL) return LF_ERR_INTERNAL;
        if (i->group == LF_GROUP_LAUNCHER) return LF_ERR_LAUNCHER;
        if (i->group == LF_GROUP_BUILD) return LF_ERR_BUILD;
        if (i->kind == LF_KIND_SWITCH && value && *value && lf_value_is_off(value)) return LF_ERR_SWITCH_OFF;
    }
    return LF_OK;
}

bool lf_skipped(const LfFlag *f) {
    const LfInfo *i = lf_find(f->name);
    return i && i->kind == LF_KIND_SWITCH && lf_value_is_off(f->value);
}

void lf_clear(LfList *l) { l->n = 0; }

int lf_index(const LfList *l, const char *name) {
    for (int i = 0; i < l->n; i++)
        if (!ci_cmp(l->flag[i].name, name)) return i;
    return -1;
}

LfError lf_set(LfList *l, const char *name, const char *value) {
    char n[LF_NAME_MAX + 8], v[LF_VALUE_MAX + 8];
    snprintf(n, sizeof n, "%s", name ? name : "");
    snprintf(v, sizeof v, "%s", value ? value : "");
    lf_normalize_name(n);
    lf_normalize_value(v);
    if (!v[0]) snprintf(v, sizeof v, "1");
    LfError e = lf_check(n, v);
    if (e) return e;
    int at = lf_index(l, n);
    if (at < 0) {
        if (l->n >= LF_MAX_FLAGS) return LF_ERR_FULL;
        at = l->n++;
    }
    snprintf(l->flag[at].name, sizeof l->flag[at].name, "%s", n);
    snprintf(l->flag[at].value, sizeof l->flag[at].value, "%s", v);
    return LF_OK;
}

void lf_remove(LfList *l, int index) {
    if (index < 0 || index >= l->n) return;
    memmove(&l->flag[index], &l->flag[index + 1], (size_t)(l->n - index - 1) * sizeof l->flag[0]);
    l->n--;
}

int lf_parse(LfList *l, const char *text) {
    int skipped = 0;
    l->n = 0;
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;  // (Notepad's byte order mark)
    while (*text) {
        const char *end = strpbrk(text, "\r\n");
        size_t len = end ? (size_t)(end - text) : strlen(text);
        char line[LF_NAME_MAX + LF_VALUE_MAX + 16];
        if (len >= sizeof line) {  // too long to be a valid line
            skipped++;
        } else {
            memcpy(line, text, len);
            line[len] = 0;
            char *s = trim(line);
            if (*s && *s != '#') {
                char *eq = strchr(s, '=');
                if (eq) *eq = 0;
                if (lf_set(l, s, eq ? eq + 1 : "") != LF_OK) skipped++;
            }
        }
        text += len;
        while (*text == '\r' || *text == '\n') text++;
    }
    return skipped;
}

size_t lf_format(const LfList *l, char *out, size_t cap) {
    static const char head[] =
        "# Savage Wind launch options: environment flags set at every start. One NAME=value per line.\n"  // i18n-skip
        "# Change them in Settings > Prelaunch. Delete this file, or start Savage Wind with " LF_OFF_SWITCH ", to start without them.\n";  // i18n-skip
    size_t n = 0;
    int w = snprintf(out, cap, "%s", head);
    if (w < 0 || (size_t)w >= cap) return 0;
    n = (size_t)w;
    for (int i = 0; i < l->n; i++) {
        w = snprintf(out + n, cap - n, "%s=%s\n", l->flag[i].name, l->flag[i].value);
        if (w < 0 || (size_t)w >= cap - n) return 0;
        n += (size_t)w;
    }
    return n;
}

bool lf_load(LfList *l, const char *path) {
    l->n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static char buf[32768];  // (a hand-made file bigger than this is cut, not trusted)
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    lf_parse(l, buf);
    return true;
}

bool lf_save(const LfList *l, const char *path) {
    if (!l->n) return remove(path) == 0 || fopen(path, "rb") == NULL;  // (nothing saved = no file)
    static char buf[32768];
    size_t len = lf_format(l, buf, sizeof buf);
    if (!len) return false;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);  // i18n-skip
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
#ifdef _WIN32
    remove(path);  // (rename won't replace a file there)
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

// ---- the environment

static uint32_t fnv(uint32_t h, const char *s) {
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

void lf_signature(const LfList *l, char *out, size_t cap) {
    int order[LF_MAX_FLAGS], n = 0;
    for (int i = 0; i < l->n; i++) {  // (what applying sets, by name: the same list in another order is the same)
        if (lf_skipped(&l->flag[i])) continue;
        int j = n++;
        while (j > 0 && ci_cmp(l->flag[order[j - 1]].name, l->flag[i].name) > 0) order[j] = order[j - 1], j--;
        order[j] = i;
    }
    if (!n) {
        if (cap) out[0] = 0;
        return;
    }
    uint32_t h = 2166136261u;
    for (int k = 0; k < n; k++) h = fnv(fnv(fnv(h, l->flag[order[k]].name), "="), l->flag[order[k]].value), h = fnv(h, "\n");
    size_t at = (size_t)snprintf(out, cap, "%08x:", (unsigned)h);
    for (int k = 0; k < n && at < cap; k++)
        at += (size_t)snprintf(out + at, cap - at, "%s%s", k ? "," : "", l->flag[order[k]].name);
}

const char *lf_applied(void) {
    const char *e = getenv(LF_APPLIED_ENV);
    return e ? e : "";
}

int lf_apply(const LfList *l) {
    char prev[LF_MAX_FLAGS * (LF_NAME_MAX + 1) + 16];
    snprintf(prev, sizeof prev, "%s", lf_applied());
    char *names = strchr(prev, ':');
    for (char *p = names ? names + 1 : NULL; p && *p;) {  // (what an earlier apply set: gone unless wanted again)
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        unsetenv(p);
        p = comma ? comma + 1 : p + strlen(p);
    }
    int set = 0;
    for (int i = 0; i < l->n; i++) {
        if (lf_skipped(&l->flag[i])) continue;
        if (setenv(l->flag[i].name, l->flag[i].value, 1) == 0) set++;
    }
    char sig[LF_MAX_FLAGS * (LF_NAME_MAX + 1) + 16];
    lf_signature(l, sig, sizeof sig);
    if (sig[0]) setenv(LF_APPLIED_ENV, sig, 1);
    else unsetenv(LF_APPLIED_ENV);
    return set;
}

bool lf_in_effect(const LfList *l) {
    char sig[LF_MAX_FLAGS * (LF_NAME_MAX + 1) + 16];
    lf_signature(l, sig, sizeof sig);
    return !strcmp(sig, lf_applied());
}

// ---- suggestions

typedef struct {
    int rank, group, index;
} Cand;

static const char *short_name(const char *n) { return ci_prefix(n, PREFIX) ? n + sizeof PREFIX - 1 : n; }

static bool ci_contains(const char *hay, const char *needle) {
    for (; *hay; hay++)
        if (ci_prefix(hay, needle)) return true;
    return false;
}

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    if (x->group != y->group) return x->group - y->group;
    return x->index - y->index;  // (the table is sorted by name)
}

int lf_suggest(const char *typed, const LfInfo **out, int max) {
    char t[LF_NAME_MAX + 8];
    snprintf(t, sizeof t, "%s", typed ? typed : "");
    trim(t);
    enum { CAP = 1024 };
    static Cand c[CAP];  // (UI thread; the table is a few hundred names)
    int n = 0;
    for (int i = 0; i < LF_TABLE_N && n < CAP; i++) {
        const LfInfo *f = &LF_TABLE[i];
        if (f->group > LF_GROUP_DEV) continue;
        int rank;
        if (!t[0]) rank = f->group == LF_GROUP_PLAY ? 0 : -1;
        else if (ci_prefix(f->name, t)) rank = 0;
        else if (ci_prefix(short_name(f->name), t)) rank = 1;
        else if (ci_contains(short_name(f->name), t)) rank = 2;
        else if (ci_contains(f->name, t)) rank = 3;
        else rank = -1;
        if (rank >= 0) c[n++] = (Cand){rank, (int)f->group, i};
    }
    qsort(c, (size_t)n, sizeof *c, cand_cmp);
    int count = n < max ? n : max;
    for (int k = 0; k < count; k++) out[k] = &LF_TABLE[c[k].index];
    return count;
}
