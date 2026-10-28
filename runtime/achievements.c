// Achievements: checking a save image against the table, and keeping what has been earned (runtime/achievements.h).
#include "achievements.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ACH 256
#define MAX_EXTRA 16  // lines of achievements.txt for ids this build doesn't know: kept as they are
#define MAX_SLOTS 32  // playthroughs whose achievements are kept
#define MAX_TOAST 32

// ------------------------------------------------------------------ the save image
uint32_t ach_crc32(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool ach_save_ok(const AchSave *s) { return s && s->data && s->size >= 0xC + 8; }

bool ach_save_get(const AchSave *s, uint32_t hash, uint32_t *value) {
    if (!ach_save_ok(s)) return false;
    for (size_t o = 0xC; o + 8 <= s->size; o += 8)
        if (rd32(s->data + o) == hash) {
            if (value) *value = rd32(s->data + o + 4);
            return true;
        }
    return false;
}

// A flag is its name, or "#<hex>" for its crc32 itself (a flag found with tools/ach_tool diff whose name isn't known).
static uint32_t flag_hash(const char *name) { return name[0] == '#' ? (uint32_t)strtoul(name + 1, NULL, 16) : ach_crc32(name); }

bool ach_save_get_named(const AchSave *s, const char *name, uint32_t *value) { return ach_save_get(s, flag_hash(name), value); }


// ---- string arrays (horse names) ----
// The text of one slot: raw bytes up to the first NUL (anything after it is left-over from a longer name). ASCII stored as UTF-16LE
// ("S\0a\0...") is understood too. Blanks at both ends are cut.
static void slot_decode(const uint8_t *raw, int n, char *out) {
    int w = 0;
    bool u16 = n >= 4 && raw[0] && !raw[1] && raw[2] && !raw[3];
    if (u16) {
        for (int i = 0; i + 1 < n && w < ACH_STR_MAX - 1; i += 2) {
            unsigned u = (unsigned)raw[i] | (unsigned)raw[i + 1] << 8;
            if (!u) break;
            out[w++] = u < 0x80 ? (char)u : '?';
        }
    } else {
        for (int i = 0; i < n && raw[i] && w < ACH_STR_MAX - 1; i++) out[w++] = (char)raw[i];
    }
    out[w] = 0;
    while (w && (out[w - 1] == ' ' || out[w - 1] == '\t' || out[w - 1] == '\r' || out[w - 1] == '\n')) out[--w] = 0;
    int lead = 0;
    while (out[lead] == ' ' || out[lead] == '\t') lead++;
    if (lead) memmove(out, out + lead, (size_t)(w - lead + 1));
}

int ach_save_strings(const AchSave *s, uint32_t hash, int per, char (*out)[ACH_STR_MAX], int max) {
    if (!ach_save_ok(s) || per < 1 || per > 64 || max < 1) return 0;
    uint8_t raw[4 * 64];
    int n = 0, got = 0;
    for (size_t o = 0xC; o + 8 <= s->size && n < max; o += 8) {
        if (rd32(s->data + o) != hash) continue;
        memcpy(raw + 4 * got, s->data + o + 4, 4);  // (the characters are in file order)
        if (++got < per) continue;
        slot_decode(raw, 4 * per, out[n++]);
        got = 0;
    }
    return n;
}

static bool same_text(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

static bool at_least(const AchSave *s, const char *name, uint32_t min) {
    uint32_t v;
    return ach_save_get_named(s, name, &v) && (int32_t)v >= (int32_t)min;  // (the flags are s32)
}

bool ach_eval(const AchDef *d, const AchSave *s, uint32_t *cur, uint32_t *target) {
    uint32_t c = 0, t = d->min ? d->min : 1;
    switch (d->kind) {
    case ACH_FLAG: {
        // `flag` may list alternative names ("A,B"): the game's counter has had more than one candidate name, so the
        // highest value among those present in the save counts.
        char list[256];
        snprintf(list, sizeof list, "%s", d->flag);
        int32_t sv = 0;
        for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
            uint32_t v;
            if (ach_save_get_named(s, n, &v) && (int32_t)v > sv) sv = (int32_t)v;
        }
        c = sv < 0 ? 0 : (uint32_t)sv;
        if (c > t) c = t;  // (progress never shows past the goal)
        break;
    }
    case ACH_COUNT:
        for (int i = d->first; i <= d->last; i++) {
            char name[96];
            snprintf(name, sizeof name, d->flag, i);
            if (at_least(s, name, 1)) c++;
        }
        if (c > t) c = t;
        break;
    case ACH_ALL: {
        char list[512];
        snprintf(list, sizeof list, "%s", d->flag);
        t = 0;
        for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
            t++;
            if (at_least(s, n, 1)) c++;
        }
        break;
    }
    case ACH_STR: {
        char slots[16][ACH_STR_MAX];
        int n = ach_save_strings(s, flag_hash(d->flag), d->first > 0 ? d->first : 16, slots, 16);
        for (int i = 0; i < n; i++) {
            if (!slots[i][0]) continue;  // (empty slot)
            bool ok = d->n_list == 0;
            for (int k = 0; k < d->n_list && !ok; k++) ok = same_text(slots[i], d->list[k]);
            c += ok;
        }
        if (c > t) c = t;
        break;
    }
    case ACH_ANY:
        t = 0;
        for (int i = 0; i < d->n_list; i++) {
            char list[512];
            snprintf(list, sizeof list, "%s", d->list[i]);
            uint32_t got = 0, all = 0;
            for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
                all++;
                got += at_least(s, n, 1);
            }
            if (got > c || (got == c && all < t) || !t) c = got, t = all;  // (the group closest to complete)
            if (got == all && all) { c = t = all; break; }
        }
        if (!t) t = 1;
        break;
    case ACH_META:  // (ach_update fills it in from what the playthrough has)
        t = (uint32_t)(g_ach_count > 1 ? g_ach_count - 1 : 1);
        break;
    case ACH_SET:
    case ACH_SUM:
        if (d->kind == ACH_SET && !d->min) t = (uint32_t)d->n_list;
        for (int i = 0; i < d->n_list; i++) {
            uint32_t v;
            if (!ach_save_get_named(s, d->list[i], &v) || (int32_t)v < 1) continue;
            c = d->kind == ACH_SET ? c + 1 : c + (uint32_t)(int32_t)v;
            if (c > t) { c = t; break; }
        }
        break;
    }
    if (cur) *cur = c;
    if (target) *target = t;
    return c >= t;
}

void ach_def_flags(const AchDef *d, void (*cb)(const char *name, void *user), void *user) {
    switch (d->kind) {
    case ACH_FLAG: {
        char list[256];
        snprintf(list, sizeof list, "%s", d->flag);
        for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) cb(n, user);
        break;
    }
    case ACH_COUNT:
        for (int i = d->first; i <= d->last; i++) {
            char name[96];
            snprintf(name, sizeof name, d->flag, i);
            cb(name, user);
        }
        break;
    case ACH_ALL: {
        char list[512];
        snprintf(list, sizeof list, "%s", d->flag);
        for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) cb(n, user);
        break;
    }
    case ACH_STR:
        cb(d->flag, user);
        break;
    case ACH_SET:
    case ACH_SUM:
        for (int i = 0; i < d->n_list; i++) cb(d->list[i], user);
        break;
    case ACH_ANY:
        for (int i = 0; i < d->n_list; i++) {
            char list[512];
            snprintf(list, sizeof list, "%s", d->list[i]);
            for (char *save = NULL, *n = strtok_r(list, ",", &save); n; n = strtok_r(NULL, ",", &save)) cb(n, user);
        }
        break;
    case ACH_META:
        break;
    }
}

// ------------------------------------------------------------------ what has been earned
// Achievements belong to a save: each game_data.sav has its own, what its contents meet (in BOTW what a playthrough has
// done only adds up, so its save says it all). The ones shown are those of the save being played: the one the game
// loaded (opened to read) or saved (opened to write, or wrote over the one being played) last. A save the game writes
// is announced against the one that was being played: what it has that that one hadn't gets a notice (and the times of
// the rest carry over). So a new game starts from nothing - its first save meets nothing - while the old saves keep
// theirs, and loading one shows them again without notices. A save copied in from elsewhere counts as it is.
typedef struct {
    char key[512];  // normalized path of the game_data.sav
    bool used;
    bool unlocked[MAX_ACH];
    time_t at[MAX_ACH];
    uint32_t cur[MAX_ACH], target[MAX_ACH];
    char extra[MAX_EXTRA][300];  // lines of ids this build doesn't know, kept as they are
    int n_extra;
    time_t written;  // when the save was written (its file time; when it was read, without one)
    bool writing;    // the game opened it to write: its next contents are the play's (ach_set_active)
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state_path[1024];
static bool blocked;
static Slot slots[MAX_SLOTS];
static int active_slot = -1;  // the save being played
static int toasts[MAX_TOAST], toast_head, toast_n;

static void norm_key(const char *path, char *out, size_t n) {  // "a//b/./c" -> "a/b/c"
    size_t k = 0;
    while (path[0] == '.' && path[1] == '/') path += 2;
    for (; *path && k + 1 < n; path++) {
        if (*path == '\\') continue;
        if (*path == '/' && k && out[k - 1] == '/') continue;
        out[k++] = *path;
    }
    out[k] = 0;
}

// "save/save_1/0/game_data.sav" -> "save/save_1": the profile folder a save file belongs to (the watcher's groups)
void ach_game_key(const char *save_path, char *out, size_t n) {
    norm_key(save_path, out, n);
    char *sl = strrchr(out, '/');  // the file name off: what's left is the slot folder
    if (!sl) return;
    *sl = 0;
    sl = strrchr(out, '/');  // the slot off: the profile folder
    if (sl && sl != out) *sl = 0;
}

static int find_key(const char *key, bool create) {  // (lock held) key: a normalized save path
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].used && !strcmp(slots[i].key, key)) return i;
    if (!create) return -1;
    int pick = -1;
    for (int i = 0; i < MAX_SLOTS && pick < 0; i++)
        if (!slots[i].used) pick = i;
    for (int i = 0; i < MAX_SLOTS && pick < 0; i++)  // full: forget the one written longest ago
        if (i != active_slot && (pick < 0 || slots[i].written < slots[pick].written)) pick = i;
    if (pick < 0) return -1;
    memset(&slots[pick], 0, sizeof slots[pick]);
    slots[pick].used = true;
    snprintf(slots[pick].key, sizeof slots[pick].key, "%s", key);
    return pick;
}

static int find_slot(const char *save_path, bool create) {  // (lock held)
    char key[512];
    norm_key(save_path, key, sizeof key);
    return find_key(key, create);
}

// (lock held) What is shown: the save being played, or before the game opens one, the one written last
static int view_slot(void) {
    if (active_slot >= 0) return active_slot;
    int k = -1;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].used && (k < 0 || slots[i].written > slots[k].written)) k = i;
    return k;
}

static int find_id(const char *id) {
    for (int i = 0; i < g_ach_count && i < MAX_ACH; i++)
        if (!strcmp(g_ach_defs[i].id, id)) return i;
    return -1;
}

static bool is_save_path(const char *p) {
    size_t n = strlen(p);
    return n >= 13 && !strcmp(p + n - 13, "game_data.sav");
}

void ach_state_load(const char *path) {
    pthread_mutex_lock(&lock);
    snprintf(state_path, sizeof state_path, "%s", path);
    memset(slots, 0, sizeof slots);
    active_slot = -1;
    toast_head = toast_n = 0;
    FILE *f = fopen(path, "r");
    char line[600];
    int cur = -1;
    // Format: "@<tab>path of a game_data.sav<tab>file time", then its lines "id<tab>unix time". (Records of whole profile
    // folders, from builds that kept one per playthrough, are dropped: the saves are read again anyway.)
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '@' && line[1] == '\t') {
            char *tab = strchr(line + 2, '\t');
            long long written = tab ? atoll(tab + 1) : 0;
            if (tab) *tab = 0;
            cur = is_save_path(line + 2) ? find_key(line + 2, true) : -1;
            if (cur >= 0) slots[cur].written = (time_t)written;
            continue;
        }
        if (cur < 0 || line[0] == '!') continue;  // (no record / an older build's notes)
        char *tab = strchr(line, '\t');
        long long at = tab ? atoll(tab + 1) : 0;
        if (tab) *tab = 0;
        int i = find_id(line);
        if (i >= 0) slots[cur].unlocked[i] = true, slots[cur].at[i] = (time_t)at;
        else if (slots[cur].n_extra < MAX_EXTRA) snprintf(slots[cur].extra[slots[cur].n_extra++], sizeof slots[cur].extra[0], "%.200s\t%lld", line, at);
    }
    if (f) fclose(f);
    pthread_mutex_unlock(&lock);
}

static void state_save_locked(void) {  // (lock held)
    if (!state_path[0]) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# Savage Wind achievements, per save: \"@<tab>save file<tab>file time\", then id <tab> unlock time (unix) for each.\n");
    for (int k = 0; k < MAX_SLOTS; k++) {
        const Slot *sl = &slots[k];
        if (!sl->used) continue;
        fprintf(f, "@\t%s\t%lld\n", sl->key, (long long)sl->written);
        for (int i = 0; i < g_ach_count && i < MAX_ACH; i++)
            if (sl->unlocked[i]) fprintf(f, "%s\t%lld\n", g_ach_defs[i].id, (long long)sl->at[i]);
        for (int i = 0; i < sl->n_extra; i++) fprintf(f, "%s\n", sl->extra[i]);
    }
    bool ok = fflush(f) == 0 && !ferror(f);
    fclose(f);
    if (!ok) { remove(tmp); return; }
#ifdef _WIN32
    remove(state_path);  // (rename doesn't replace there)
#endif
    if (rename(tmp, state_path)) remove(tmp);
}

void ach_set_blocked(bool b) {
    pthread_mutex_lock(&lock);
    blocked = b;
    pthread_mutex_unlock(&lock);
}

void ach_game_gone(const char *save_path) {
    pthread_mutex_lock(&lock);
    int k = find_slot(save_path, false);
    if (k >= 0) {
        if (k == active_slot) active_slot = -1;
        memset(&slots[k], 0, sizeof slots[k]);
        state_save_locked();
    }
    pthread_mutex_unlock(&lock);
}

// Saves the game loaded: the watcher reads them again even unchanged (ach_wants_reread), quietly
enum { REREAD_FREE, REREAD_WANTED, REREAD_QUIET };
static struct { char path[512]; int state; } reread[4];

void ach_set_active(const char *save_path) {
    pthread_mutex_lock(&lock);
    int i = find_slot(save_path, true);
    if (i >= 0) slots[i].writing = true;
    pthread_mutex_unlock(&lock);
}

void ach_game_loaded(const char *save_path) {
    pthread_mutex_lock(&lock);
    int i = find_slot(save_path, true);
    if (i >= 0) {
        active_slot = i;
        int r = 0;
        while (r < 4 && !(reread[r].state != REREAD_FREE && !strcmp(reread[r].path, slots[i].key))) r++;  // (asked already)
        if (r == 4)
            for (r = 0; r < 4 && reread[r].state != REREAD_FREE; r++) {}
        static int oldest;  // (all taken - by loads whose files were never read since: the oldest goes)
        if (r == 4) r = oldest, oldest = (oldest + 1) % 4;
        snprintf(reread[r].path, sizeof reread[r].path, "%s", slots[i].key), reread[r].state = REREAD_WANTED;
    }
    pthread_mutex_unlock(&lock);
}

bool ach_wants_reread(const char *save_path) {
    char path[512];
    norm_key(save_path, path, sizeof path);
    pthread_mutex_lock(&lock);
    bool want = false;
    for (int r = 0; r < 4; r++)
        if (reread[r].state == REREAD_WANTED && !strcmp(reread[r].path, path)) reread[r].state = REREAD_QUIET, want = true;
    pthread_mutex_unlock(&lock);
    return want;
}

int ach_update(const char *save_path, const AchSave *s, time_t now) { return ach_update_at(save_path, s, now, 0); }

// (lock held) Achievement i of the save sl, from whether its contents meet it; announced when `play` and the save that
// was being played (prev) hadn't it. Returns whether the record changed.
static bool settle(Slot *sl, const Slot *prev, int i, bool met, time_t now, bool play, int *fresh) {
    bool had = prev && prev->unlocked[i];
    if (!met) {
        if (!sl->unlocked[i]) return false;
        sl->unlocked[i] = false, sl->at[i] = 0;  // (a save of another playthrough written over this one)
        return true;
    }
    if (blocked) return false;
    // what the save had before doesn't matter for the notice (the slot may hold another playthrough's old save): only
    // what the save being played had
    time_t at = play ? (had && prev->at[i] ? prev->at[i] : now) : sl->unlocked[i] ? sl->at[i] : now;
    bool changed = !sl->unlocked[i] || sl->at[i] != at;
    sl->unlocked[i] = true, sl->at[i] = at;
    if (play && !had && toast_n < MAX_TOAST) {
        toasts[(toast_head + toast_n++) % MAX_TOAST] = i;
        (*fresh)++;
    }
    return changed;
}

int ach_update_at(const char *save_path, const AchSave *s, time_t now, time_t written) {
    if (!ach_save_ok(s)) return 0;
    int fresh = 0;
    bool dirty = false;
    pthread_mutex_lock(&lock);
    int k = find_slot(save_path, true);
    if (k < 0) { pthread_mutex_unlock(&lock); return 0; }
    Slot *sl = &slots[k];
    bool quiet = false;  // (a save the game loaded: shown as it is, nothing announced)
    for (int r = 0; r < 4; r++)
        if (reread[r].state == REREAD_QUIET && !strcmp(reread[r].path, sl->key)) reread[r].state = REREAD_FREE, quiet = true;
    // the play's save: written by the game (opened to write, or the one being played saved over); announced against
    // the one being played until now
    bool play = !quiet && (sl->writing || k == active_slot);
    static Slot prev_copy;  // (lock held: one at a time)
    const Slot *prev = NULL;
    if (play && active_slot >= 0) prev_copy = slots[active_slot], prev = &prev_copy;
    time_t when = written ? written : now;
    if (sl->written != when) sl->written = when, dirty = true;
    sl->writing = false;
    for (int i = 0; i < g_ach_count && i < MAX_ACH; i++) {
        if (g_ach_defs[i].kind == ACH_META) continue;
        uint32_t cur, target;
        bool met = ach_eval(&g_ach_defs[i], s, &cur, &target);
        sl->cur[i] = cur, sl->target[i] = target;
        dirty |= settle(sl, prev, i, met, now, play, &fresh);
    }
    // ACH_META: every other achievement of the save unlocked (after them: its notice comes last)
    for (int i = 0; i < g_ach_count && i < MAX_ACH; i++) {
        if (g_ach_defs[i].kind != ACH_META) continue;
        uint32_t have = 0, all = 0;
        for (int j = 0; j < g_ach_count && j < MAX_ACH; j++)
            if (g_ach_defs[j].kind != ACH_META) all++, have += sl->unlocked[j];
        sl->cur[i] = have, sl->target[i] = all;
        dirty |= settle(sl, prev, i, have == all && all, now, play, &fresh);
    }
    if (play || quiet) active_slot = k;
    if (dirty) state_save_locked();
    pthread_mutex_unlock(&lock);
    return fresh;
}

void ach_info(int i, AchInfo *out) {
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= g_ach_count || i >= MAX_ACH) return;
    pthread_mutex_lock(&lock);
    out->def = &g_ach_defs[i];
    int k = view_slot();
    if (k >= 0) {
        const Slot *sl = &slots[k];
        out->unlocked = sl->unlocked[i];
        out->at = sl->at[i];
        out->cur = sl->unlocked[i] && sl->target[i] ? sl->target[i] : sl->cur[i];
        out->target = sl->target[i];
    }
    pthread_mutex_unlock(&lock);
}

int ach_unlocked_total(void) {
    int n = 0;
    pthread_mutex_lock(&lock);
    int k = view_slot();
    for (int i = 0; k >= 0 && i < g_ach_count && i < MAX_ACH; i++) n += slots[k].unlocked[i];
    pthread_mutex_unlock(&lock);
    return n;
}

int ach_toast_pending(void) {
    pthread_mutex_lock(&lock);
    int n = toast_n;
    pthread_mutex_unlock(&lock);
    return n;
}

bool ach_toast_take(int *index) {
    bool ok = false;
    pthread_mutex_lock(&lock);
    if (toast_n) {
        *index = toasts[toast_head];
        toast_head = (toast_head + 1) % MAX_TOAST, toast_n--;
        ok = true;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
