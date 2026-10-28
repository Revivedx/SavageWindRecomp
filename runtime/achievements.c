// Achievements: checking a save image against the table, and keeping what has been earned (runtime/achievements.h).
#include "achievements.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ACH 256
#define MAX_EXTRA 16  // lines of achievements.txt for ids this build doesn't know: kept as they are
#define MAX_SLOTS 32  // playthroughs whose achievements are kept
#define MAX_TOAST 32
#define MAX_STALE 16  // slots of one playthrough noted as left from the one before (the game has up to 8)

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
    }
}

// ------------------------------------------------------------------ what has been earned
// Achievements belong to a playthrough, not to the player: the game keeps one per profile (the folder that holds the
// numbered slots with the autosaves and manual saves), a new game wipes it, and the achievements shown are those of the
// playthrough being played. Once earned (and announced) in a playthrough one stays earned and is never announced again,
// whichever of its saves is loaded; a new game (the save has no progress at all, or its files were all deleted) starts over.
// The game doesn't delete the old playthrough's saves for a new game: it writes the new one over one slot and leaves the
// others as they were. So the new game's first save also notes the other slots' files as they are then (`stale`: path and
// file time): those don't count while they stay the same. Not their age: a save copied in from elsewhere (a backup, a
// 100% save) keeps the time it was written at, older than any new game, and still counts.
typedef struct {
    char key[512];  // normalized path of the game_data.sav
    bool used;
    char stale_path[MAX_STALE][300];  // the old playthrough's saves left in the other slots, and their file times
    time_t stale_at[MAX_STALE];
    int n_stale;
    bool unlocked[MAX_ACH];
    time_t at[MAX_ACH];
    uint32_t cur[MAX_ACH], target[MAX_ACH];
    char extra[MAX_EXTRA][300];  // lines of ids this build doesn't know, kept as they are
    int n_extra;
    time_t touched;
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state_path[1024];
static bool blocked;
static Slot slots[MAX_SLOTS];
static int active_slot = -1, latest_slot = -1;  // the save the game opened last / the one read last
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

// "save/save_1/0/game_data.sav" -> "save/save_1": the playthrough (profile folder) a save file belongs to
void ach_game_key(const char *save_path, char *out, size_t n) {
    norm_key(save_path, out, n);
    char *sl = strrchr(out, '/');  // the file name off: what's left is the slot folder
    if (!sl) return;
    *sl = 0;
    sl = strrchr(out, '/');  // the slot off: the profile folder
    if (sl && sl != out) *sl = 0;
}

static int find_key(const char *key, bool create) {  // (lock held) key: a playthrough as ach_game_key gives it
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].used && !strcmp(slots[i].key, key)) return i;
    if (!create) return -1;
    int pick = -1;
    for (int i = 0; i < MAX_SLOTS && pick < 0; i++)
        if (!slots[i].used) pick = i;
    for (int i = 0; i < MAX_SLOTS && pick < 0; i++)  // full: forget the one touched longest ago
        if (i != active_slot && i != latest_slot && (pick < 0 || slots[i].touched < slots[pick].touched)) pick = i;
    if (pick < 0) return -1;
    memset(&slots[pick], 0, sizeof slots[pick]);
    slots[pick].used = true;
    snprintf(slots[pick].key, sizeof slots[pick].key, "%s", key);
    return pick;
}

static int find_slot(const char *save_path, bool create) {  // (lock held)
    char key[512];
    ach_game_key(save_path, key, sizeof key);
    return find_key(key, create);
}

static int view_slot(void) { return active_slot >= 0 ? active_slot : latest_slot; }  // (lock held)

static int find_id(const char *id) {
    for (int i = 0; i < g_ach_count && i < MAX_ACH; i++)
        if (!strcmp(g_ach_defs[i].id, id)) return i;
    return -1;
}

void ach_state_load(const char *path) {
    pthread_mutex_lock(&lock);
    snprintf(state_path, sizeof state_path, "%s", path);
    memset(slots, 0, sizeof slots);
    active_slot = latest_slot = -1;
    toast_head = toast_n = 0;
    FILE *f = fopen(path, "r");
    char line[600];
    int cur = -1;
    // Format: "@<tab>path of a game_data.sav", then its lines "id<tab>unix time". (Lines before the first "@" are from the
    // old one-list-for-everyone format and belong to no save: dropped.)
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '@' && line[1] == '\t') {
            cur = find_key(line + 2, true);
            continue;
        }
        if (cur < 0) continue;
        if (!strncmp(line, "!stale\t", 7)) {  // "!stale<tab>file time<tab>path": a save left from the playthrough before
            Slot *sl = &slots[cur];
            char *p = strchr(line + 7, '\t');
            if (p && sl->n_stale < MAX_STALE) {
                sl->stale_at[sl->n_stale] = (time_t)atoll(line + 7);
                snprintf(sl->stale_path[sl->n_stale++], sizeof sl->stale_path[0], "%s", p + 1);
            }
            continue;
        }
        if (line[0] == '!') continue;  // (an older build's "!since": file ages, which copied saves defeated)
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
    fprintf(f, "# Savage Wind achievements, per save: \"@<tab>save file\", then id <tab> unlock time (unix) for each.\n");
    for (int k = 0; k < MAX_SLOTS; k++) {
        const Slot *sl = &slots[k];
        if (!sl->used) continue;
        bool any = sl->n_extra > 0 || sl->n_stale > 0;
        for (int i = 0; i < g_ach_count && i < MAX_ACH && !any; i++) any = sl->unlocked[i];
        if (!any) continue;
        fprintf(f, "@\t%s\n", sl->key);
        for (int i = 0; i < sl->n_stale; i++) fprintf(f, "!stale\t%lld\t%s\n", (long long)sl->stale_at[i], sl->stale_path[i]);
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
        bool had = false;
        for (int i = 0; i < g_ach_count && i < MAX_ACH; i++) had |= slots[k].unlocked[i], slots[k].unlocked[i] = false, slots[k].at[i] = 0;
        memset(slots[k].cur, 0, sizeof slots[k].cur);
        slots[k].n_extra = 0;
        if (had) state_save_locked();
    }
    pthread_mutex_unlock(&lock);
}

void ach_set_active(const char *save_path) {
    pthread_mutex_lock(&lock);
    int i = find_slot(save_path, true);
    if (i >= 0) active_slot = i, slots[i].touched = time(NULL);
    pthread_mutex_unlock(&lock);
}

int ach_update(const char *save_path, const AchSave *s, time_t now) { return ach_update_at(save_path, s, now, 0); }

// (lock held) A new game began with the save at `path` (normalized): the playthrough's other slots (<profile>/<n>/
// game_data.sav) hold what is left of the one before. Each is noted with its file time; it counts again once it changes.
static void note_stale(Slot *sl, const char *path) {
    sl->n_stale = 0;
    for (int n = 0; n < 32 && sl->n_stale < MAX_STALE; n++) {
        char p[600];
        struct stat st;
        snprintf(p, sizeof p, "%s/%d/game_data.sav", sl->key, n);
        if (!strcmp(p, path) || stat(p, &st)) continue;
        sl->stale_at[sl->n_stale] = st.st_mtime;
        snprintf(sl->stale_path[sl->n_stale++], sizeof sl->stale_path[0], "%s", p);
    }
}

int ach_update_at(const char *save_path, const AchSave *s, time_t now, time_t written) {
    if (!ach_save_ok(s)) return 0;
    int fresh = 0;
    bool dirty = false;
    pthread_mutex_lock(&lock);
    int k = find_slot(save_path, true);
    if (k < 0) { pthread_mutex_unlock(&lock); return 0; }
    Slot *sl = &slots[k];
    char path[512];
    norm_key(save_path, path, sizeof path);
    for (int i = 0; i < sl->n_stale; i++) {
        if (strcmp(sl->stale_path[i], path)) continue;
        if (written > 0 && written == sl->stale_at[i]) { pthread_mutex_unlock(&lock); return 0; }  // left from before
        sl->stale_at[i] = sl->stale_at[--sl->n_stale];  // (written again or replaced since: it counts now)
        memcpy(sl->stale_path[i], sl->stale_path[sl->n_stale], sizeof sl->stale_path[0]);
        dirty = true;
        break;
    }
    sl->touched = now;
    latest_slot = k;
    // A save with no progress at all on anything is a new game: whatever the playthrough had before is gone.
    bool any_progress = false;
    for (int i = 0; i < g_ach_count && i < MAX_ACH && !any_progress; i++) {
        uint32_t cur, target;
        ach_eval(&g_ach_defs[i], s, &cur, &target);
        uint32_t start = g_ach_defs[i].kind == ACH_FLAG && g_ach_defs[i].first > 0 ? (uint32_t)g_ach_defs[i].first : 0;
        any_progress = cur > start;
    }
    if (!any_progress) {
        for (int i = 0; i < g_ach_count && i < MAX_ACH; i++) dirty |= sl->unlocked[i], sl->unlocked[i] = false, sl->at[i] = 0;
        sl->n_extra = 0;
        if (written > 0) note_stale(sl, path), dirty = true;  // (the other slots' saves, as they are now, no longer count)
        if (dirty) toast_n = 0;  // (nothing of the old playthrough is announced any more)
    }
    for (int i = 0; i < g_ach_count && i < MAX_ACH; i++) {
        uint32_t cur, target;
        bool met = ach_eval(&g_ach_defs[i], s, &cur, &target);
        sl->cur[i] = cur, sl->target[i] = target;
        if (met && !sl->unlocked[i] && !blocked) {
            sl->unlocked[i] = true, sl->at[i] = now;
            dirty = true;
            // announced only when it's the playthrough being played, and only the first time ever
            if (k == active_slot && toast_n < MAX_TOAST) {
                toasts[(toast_head + toast_n++) % MAX_TOAST] = i;
                fresh++;
            }
        }
    }
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
