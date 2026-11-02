// Search and recent-list policy shared by the browser, settings loader and tests.
#pragma once
#include <ctype.h>
#include <string.h>
#include "amiibo.h"

#define AMIIBO_RECENT_MAX 8
enum { AMIIBO_SERIES_ALL, AMIIBO_SERIES_BOTW, AMIIBO_SERIES_ZELDA, AMIIBO_SERIES_SMASH, AMIIBO_SERIES_COUNT };
static inline int amiibo_from_key(const char *key) {
    for (int i = 0; i < AMIIBO_COUNT; i++) if (!strcmp(key, virtual_amiibos[i].key)) return i;
    return -1;
}
static inline int amiibo_series(int figure) {
    if (virtual_amiibos[figure].type == 0) return AMIIBO_SERIES_SMASH;
    return figure <= AMIIBO_URBOSA ? AMIIBO_SERIES_BOTW : AMIIBO_SERIES_ZELDA;
}
static inline const char *amiibo_series_name(int series) {
    static const char *const names[] = {"All series", "Breath of the Wild", "The Legend of Zelda", "Super Smash Bros."};
    return names[series >= 0 && series < AMIIBO_SERIES_COUNT ? series : 0];
}
static inline bool amiibo_contains_word(const char *text, const char *word, size_t len) {
    for (; *text; text++) {
        size_t i = 0;
        while (i < len && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)word[i])) i++;
        if (i == len) return true;
    }
    return false;
}
static inline bool amiibo_matches(int figure, const char *query, int series) {
    if (figure < 0 || figure >= AMIIBO_COUNT) return false;
    int group = amiibo_series(figure);
    if (series && series != group) return false;
    while (*query) {
        while (isspace((unsigned char)*query)) query++;
        const char *word = query;
        while (*query && !isspace((unsigned char)*query)) query++;
        size_t len = (size_t)(query - word);
        if (len && !amiibo_contains_word(virtual_amiibos[figure].name, word, len) &&
            !amiibo_contains_word(virtual_amiibos[figure].key, word, len) &&
            !amiibo_contains_word(amiibo_series_name(group), word, len)) return false;
    }
    return true;
}
static inline void amiibo_recent_push(int *recent, int *count, int figure) {
    if (figure < 0 || figure >= AMIIBO_COUNT) return;
    int n = *count, found = n;
    for (int i = 0; i < n; i++) if (recent[i] == figure) { found = i; break; }
    if (found == n && n < AMIIBO_RECENT_MAX) n++;
    for (int i = (found < n ? found : n - 1); i > 0; i--) recent[i] = recent[i - 1];
    recent[0] = figure;
    *count = n;
}
