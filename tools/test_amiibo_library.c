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
