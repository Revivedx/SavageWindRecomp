// Optional feature helpers may be absent without hiding a missing required site.
#include <assert.h>
#include "../runtime/gamelib.c"

int main(void) {
    GameLibSite required[] = {
#define SITE(n) {#n, 0x1234},
        GAME_SITES(SITE)
#undef SITE
    };
    GameLibSite complete[] = {
#define SITE(n) {#n, 0x5678},
        GAME_SITES(SITE)
        GAME_SITES_OPTIONAL(SITE)
#undef SITE
    };
    uint32_t count = sizeof complete / sizeof complete[0];
    GameLibTables tables = {.version = "synthetic", .sites = complete, .nsites = &count};
    assert(take_sites(&tables));
    assert(g_site.pouch_sort == 0x5678);
    count = sizeof required / sizeof required[0];
    tables.sites = required;
    assert(take_sites(&tables));
    assert(!strcmp(g_game_version, "synthetic"));
    assert(g_site.opt_table == 0x1234);
#define CLEARED(n) assert(g_site.n == 0);
    GAME_SITES_OPTIONAL(CLEARED)
#undef CLEARED
    count--;
    assert(!take_sites(&tables));
    assert(strstr(g_error, "main_end"));
    puts("ok: optional helpers clear between libraries; required sites remain mandatory");
}
