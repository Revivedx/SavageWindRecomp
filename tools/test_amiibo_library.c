#include <assert.h>
#include <stdio.h>
#include "../runtime/amiibo_library.h"

int main(void) {
    assert(amiibo_from_key("guardian") == AMIIBO_GUARDIAN);
    assert(amiibo_from_key("unknown") == -1);
    assert(amiibo_matches(AMIIBO_GUARDIAN, "  GUARDIAN wild  ", AMIIBO_SERIES_BOTW));
    assert(!amiibo_matches(AMIIBO_GUARDIAN, "guardian smash", 0));
    assert(!amiibo_matches(AMIIBO_GUARDIAN, "", AMIIBO_SERIES_SMASH));
    assert(amiibo_matches(AMIIBO_LINK_MM, "majora link", AMIIBO_SERIES_ZELDA));
    assert(amiibo_matches(AMIIBO_TOON_LINK_WW, "wind toon", 0));
    assert(amiibo_matches(AMIIBO_LINK_SMASH, "link smash", AMIIBO_SERIES_SMASH));
    assert(!amiibo_matches(-1, "", 0));
    assert(AMIIBO_COUNT > 256);
    bool card = false, metroid = false;
    for (int i = AMIIBO_LEGACY_COUNT; i < AMIIBO_COUNT; i++) {
        assert(amiibo_from_key(virtual_amiibos[i].key) == i);
        assert(amiibo_series(i) > 0 && amiibo_series(i) < AMIIBO_SERIES_COUNT);
        assert(amiibo_matches(i, virtual_amiibos[i].character_name, 0));
        if (!strcmp(virtual_amiibos[i].kind, "Card")) {
            assert(amiibo_matches(i, "card", amiibo_series(i))); card = true;
        }
        if (!strcmp(virtual_amiibos[i].game, "Metroid")) {
            assert(amiibo_matches(i, "metroid", amiibo_series(i))); metroid = true;
        }
    }
    assert(card && metroid);
    int list[AMIIBO_RECENT_MAX] = {0}, n = 0;
    amiibo_recent_push(list, &n, AMIIBO_ARCHER);
    amiibo_recent_push(list, &n, AMIIBO_GUARDIAN);
    amiibo_recent_push(list, &n, AMIIBO_ARCHER);
    assert(n == 2 && list[0] == AMIIBO_ARCHER && list[1] == AMIIBO_GUARDIAN);
    for (int i = 0; i < AMIIBO_COUNT; i++) amiibo_recent_push(list, &n, i);
    assert(n == AMIIBO_RECENT_MAX);
    for (int i = 0; i < n; i++) assert(list[i] == AMIIBO_COUNT - 1 - i);
    amiibo_recent_push(list, &n, AMIIBO_COUNT - 4);
    assert(list[0] == AMIIBO_COUNT - 4 && list[1] == AMIIBO_COUNT - 1);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) assert(list[i] != list[j]);
    puts("PASS: amiibo multiword search, series filters and bounded unique recent list");
}
