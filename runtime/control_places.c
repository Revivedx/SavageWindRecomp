// Named places for `goto` and `places` (see control_places.h). The lookup is plain C on a static table.
#include "control_places.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLACE(kind, number, key, name, sub, x, y, z, ground) {kind, number, key, name, sub, x, y, z, ground},
static const Place table[] = {
#include "control_places_data.inc"
};
#undef PLACE
#define PLACE_TOTAL ((int)(sizeof table / sizeof *table))

static const char *const kind_names[PLACE_KINDS] = {"shrine", "tower", "stable", "village", "landmark", "korok"};

const char *place_kind_name(PlaceKind kind) { return kind >= 0 && kind < PLACE_KINDS ? kind_names[kind] : "?"; }

int place_kind_count(PlaceKind kind) {
    int n = 0;
    for (int i = 0; i < PLACE_TOTAL; i++) n += table[i].kind == kind;
    return n;
}

const Place *place_by_number(PlaceKind kind, int number) {
    for (int i = 0; i < PLACE_TOTAL; i++)
        if (table[i].kind == kind && table[i].number == number) return &table[i];
    return NULL;
}

// (case-insensitive equality; strcasecmp is not on every platform the runtime builds for)
static bool ieq_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}
static bool word_is(const char *word, const char *a, const char *b, const char *c) {
    return ieq_eq(word, a) || (b && ieq_eq(word, b)) || (c && ieq_eq(word, c));
}

int place_kind_from_word(const char *w) {
    if (word_is(w, "shrine", "shrines", "dungeon")) return PLACE_SHRINE;
    if (word_is(w, "tower", "towers", NULL)) return PLACE_TOWER;
    if (word_is(w, "stable", "stables", NULL)) return PLACE_STABLE;
    if (word_is(w, "village", "villages", NULL) || word_is(w, "town", "city", "towns")) return PLACE_VILLAGE;
    if (word_is(w, "landmark", "landmarks", NULL)) return PLACE_LANDMARK;
    if (word_is(w, "korok", "koroks", "seed") || ieq_eq(w, "seeds")) return PLACE_KOROK;
    return -1;
}

// ---- matching text: lower case, apostrophes dropped, anything else that is not a letter or digit separates words
static void normalize(char *dst, size_t n, const char *src) {
    size_t o = 0;
    bool gap = false;
    for (const unsigned char *s = (const unsigned char *)src; *s && o + 2 < n; s++) {
        if (*s == '\'') continue;
        if (s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0x99) { s += 2; continue; }  // (a typographic apostrophe)
        if (isalnum(*s)) {
            if (gap && o) dst[o++] = ' ';
            gap = false;
            dst[o++] = (char)tolower(*s);
        } else gap = true;
    }
    dst[o] = 0;
}

// Does every query word start a word of the text?
static bool words_start_words(const char *text, int nq, char (*q)[64]) {
    for (int i = 0; i < nq; i++) {
        bool found = false;
        for (const char *p = text; *p && !found; ) {
            size_t len = strlen(q[i]);
            if (!strncmp(p, q[i], len)) found = true;
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
        if (!found) return false;
    }
    return true;
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

// The query words, normalized; words that vanish (only punctuation) are dropped. Returns how many are left.
static int normalize_words(int nwords, char *const *words, char (*q)[64], int max) {
    int n = 0;
    for (int i = 0; i < nwords && n < max; i++) {
        normalize(q[n], 64, words[i]);
        if (q[n][0]) n++;
    }
    return n;
}

static bool in_kind(const Place *p, int kind) { return kind < 0 || (int)p->kind == kind; }

// stage 1: the whole query is the name, or the name without its last word ("Rito Stable" = "rito", "Oman Au Shrine" = "oman au")
static bool exact_name(const Place *p, const char *joined) {
    char nm[128];
    normalize(nm, sizeof nm, p->name);
    if (!strcmp(nm, joined)) return true;
    char *last = strrchr(nm, ' ');
    if (last && p->kind != PLACE_KOROK) { *last = 0; return !strcmp(nm, joined); }
    return false;
}

int place_list(int kind, int nwords, char *const *words, const Place **out, int max) {
    char q[8][64];
    int nq = normalize_words(nwords, words, q, 8);
    int total = 0;
    for (int i = 0; i < PLACE_TOTAL; i++) {
        const Place *p = &table[i];
        if (!in_kind(p, kind)) continue;
        if (kind < 0 && p->kind == PLACE_KOROK) continue;  // (900 ids would drown everything; ask for them by kind)
        char hay[256], nm[128], sub[128];
        normalize(nm, sizeof nm, p->name);
        normalize(sub, sizeof sub, p->sub);
        snprintf(hay, sizeof hay, "%s %s", nm, sub);
        if (nq && !words_start_words(hay, nq, q)) continue;
        if (total < max) out[total] = p;
        total++;
    }
    return total;
}

static void say_matches(char *why, size_t n, const char *what, const Place *const *m, int total, int shown) {
    size_t o = (size_t)snprintf(why, n, "\"%s\" matches %d places:", what, total);
    for (int i = 0; i < shown && o < n; i++)
        o += (size_t)snprintf(why + o, n - o, "%s %s %d %s", i ? "," : "", place_kind_name(m[i]->kind), m[i]->number, m[i]->name);
    if (total > shown && o < n) snprintf(why + o, n - o, ", ...");
}

static const Place *nearest(int kind, const float from[3], char *why, size_t why_size) {
    if (!from) { snprintf(why, why_size, "Nearest needs a world: Link has no position yet"); return NULL; }
    const Place *best = NULL;
    double best_d = 0;
    for (int i = 0; i < PLACE_TOTAL; i++) {
        const Place *p = &table[i];
        if (!in_kind(p, kind)) continue;
        double dx = p->x - from[0], dz = p->z - from[2];  // (sideways distance: the map is flat enough for "nearest")
        double d = dx * dx + dz * dz;
        if (!best || d < best_d) { best = p; best_d = d; }
    }
    if (!best) snprintf(why, why_size, "No place to be nearest");
    return best;
}

static const Place *by_key(int kind, const char *word) {
    for (int i = 0; i < PLACE_TOTAL; i++)
        if (in_kind(&table[i], kind) && ieq_eq(table[i].key, word)) return &table[i];
    return NULL;
}

static bool is_nearest(const char *w) { return ieq_eq(w, "nearest") || ieq_eq(w, "closest"); }
static bool is_filler(const char *w) { return ieq_eq(w, "go") || ieq_eq(w, "to") || ieq_eq(w, "the"); }

// allow_kind false: the first word is part of a name even if it also names a kind ("korok forest" is Korok Forest, a village)
static const Place *find(int nwords, char *const *words, const float from[3], char *why, size_t why_size, bool allow_kind) {
    int all_n = nwords;
    char *const *all_words = words;
    // leading filler: "go to the shrine 5"
    while (nwords > 0 && is_filler(words[0])) { words++; nwords--; }
    if (nwords == 0) { snprintf(why, why_size, "Where to? Try: shrine 5, korok H23, stable wetland, tower 2, hateno, nearest korok"); return NULL; }

    int kind = -1;
    bool want_nearest = false;
    if (!allow_kind) {
        // (no kind word)
    } else if (is_nearest(words[0]) && nwords >= 2 && place_kind_from_word(words[1]) >= 0) {
        kind = place_kind_from_word(words[1]);
        want_nearest = true;
        words += 2; nwords -= 2;
    } else if ((kind = place_kind_from_word(words[0])) >= 0) {
        words++; nwords--;
        if (nwords == 1 && is_nearest(words[0])) want_nearest = true;
    }
    if (want_nearest) return nearest(kind, from, why, why_size);

    if (kind >= 0 && nwords == 0) {
        snprintf(why, why_size, "Which %s? Give its number or name (places %s lists them)", place_kind_name((PlaceKind)kind), place_kind_name((PlaceKind)kind));
        return NULL;
    }
    if (nwords == 1) {
        // a number within the kind, or the game's id (Koroks: H23)
        if (kind >= 0 && all_digits(words[0])) {
            long number = strtol(words[0], NULL, 10);
            const Place *p = number <= 100000 ? place_by_number((PlaceKind)kind, (int)number) : NULL;
            if (!p) {
                int lo = 1 << 30, hi = -1;
                for (int i = 0; i < PLACE_TOTAL; i++)
                    if ((int)table[i].kind == kind) { if (table[i].number < lo) lo = table[i].number; if (table[i].number > hi) hi = table[i].number; }
                snprintf(why, why_size, "There is no %s %s (numbers go from %d to %d)", place_kind_name((PlaceKind)kind), words[0], lo, hi);
            }
            return p;
        }
        const Place *p = by_key(kind, words[0]);
        if (p && (kind >= 0 || p->kind == PLACE_KOROK)) return p;
    }

    // text: the whole name, then words starting words of the name, then of the quest title too
    char q[8][64], joined[600] = "";
    int nq = normalize_words(nwords, words, q, 8);
    if (!nq) { snprintf(why, why_size, "Where to? Try: shrine 5, korok H23, stable wetland, tower 2, hateno, nearest korok"); return NULL; }
    for (int i = 0; i < nq; i++) { strncat(joined, i ? " " : "", sizeof joined - strlen(joined) - 1); strncat(joined, q[i], sizeof joined - strlen(joined) - 1); }

    const Place *hit[PLACE_TOTAL];
    for (int stage = 0; stage < 3; stage++) {
        int n = 0;
        for (int i = 0; i < PLACE_TOTAL; i++) {
            const Place *p = &table[i];
            if (!in_kind(p, kind) || p->kind == PLACE_KOROK) continue;
            char nm[128], sub[128], hay[256];
            normalize(nm, sizeof nm, p->name);
            normalize(sub, sizeof sub, p->sub);
            bool ok;
            if (stage == 0) ok = exact_name(p, joined);
            else if (stage == 1) ok = words_start_words(nm, nq, q);
            else { snprintf(hay, sizeof hay, "%s %s", nm, sub); ok = words_start_words(hay, nq, q); }
            if (ok) hit[n++] = p;
        }
        if (n == 1) return hit[0];
        if (stage == 0 && n > 1) {  // ("hateno" is the village, not the tower or the lab)
            const Place *village = NULL;
            int villages = 0;
            for (int i = 0; i < n; i++) if (hit[i]->kind == PLACE_VILLAGE) { village = hit[i]; villages++; }
            if (villages == 1) return village;
        }
        if (n > 1) {
            char what[200] = "";
            for (int i = 0; i < nwords; i++) { strncat(what, i ? " " : "", sizeof what - strlen(what) - 1); strncat(what, words[i], sizeof what - strlen(what) - 1); }
            say_matches(why, why_size, what, hit, n, n < 6 ? n : 6);
            return NULL;
        }
    }
    if (kind >= 0 && allow_kind) return find(all_n, all_words, from, why, why_size, false);
    snprintf(why, why_size, "No place called \"%s\". Try places %s to see the names", joined, kind >= 0 ? place_kind_name((PlaceKind)kind) : "shrine");
    return NULL;
}

const Place *place_find(int nwords, char *const *words, const float from[3], char *why, size_t why_size) {
    return find(nwords, words, from, why, why_size, true);
}
