// Tests for the named places behind `goto` and `places` (runtime/control_places.c). No game needed; make test-places.
#include <stdio.h>
#include <string.h>

#include "../runtime/control_places.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

// Look a query up by splitting it at spaces, like goto does.
static const Place *find(const char *query, const float *from, char *why) {
    static char copy[256];
    char *words[16];
    int n = 0;
    snprintf(copy, sizeof copy, "%s", query);
    for (char *w = strtok(copy, " "); w && n < 16; w = strtok(NULL, " ")) words[n++] = w;
    why[0] = 0;
    return place_find(n, words, from, why, 400);
}
static bool named(const char *query, const char *name) {
    char why[400];
    const Place *p = find(query, NULL, why);
    if (!p || strcmp(p->name, name)) printf("  \"%s\" gave %s (%s)\n", query, p ? p->name : "nothing", why);
    return p && !strcmp(p->name, name);
}

int main(void) {
    char why[400];
    CHECK(place_kind_count(PLACE_SHRINE) == 136);
    CHECK(place_kind_count(PLACE_KOROK) == 900);
    CHECK(place_kind_count(PLACE_TOWER) == 15);
    CHECK(place_kind_count(PLACE_STABLE) == 15);

    // every position is a real place on the map (x -5000..5000, z -4000..4000)
    for (int k = 0; k < PLACE_KINDS; k++) {
        for (int n = -1; n <= 1000; n++) {
            const Place *p = place_by_number((PlaceKind)k, n);
            if (!p) continue;
            CHECK(p->x > -6000 && p->x < 6000 && p->z > -5000 && p->z < 5000 && p->y > -100 && p->y < 2200);
            CHECK(p->kind == (PlaceKind)k && p->name[0] && p->key[0]);
        }
    }

    // by number
    CHECK(named("shrine 5", "Ne'ez Yohma Shrine"));
    CHECK(named("go to shrine 5", "Ne'ez Yohma Shrine"));
    CHECK(named("to the shrine 5", "Ne'ez Yohma Shrine"));
    CHECK(named("SHRINE 005", "Ne'ez Yohma Shrine"));
    CHECK(named("stable 2", "East Akkala Stable"));
    CHECK(named("tower 7", "Great Plateau Tower"));
    CHECK(find("shrine 136", NULL, why) == NULL && strstr(why, "no shrine 136"));
    CHECK(find("shrine 99999999999", NULL, why) == NULL);
    CHECK(find("korok 901", NULL, why) == NULL && strstr(why, "1 to 900"));
    CHECK(find("korok 1", NULL, why) && find("korok 900", NULL, why));
    CHECK(find("korok 1", NULL, why) != find("korok 2", NULL, why));

    // by the game's id or by name
    CHECK(find("korok H23", NULL, why) && !strcmp(find("korok h23", NULL, why)->name, "H23"));
    CHECK(find("h23", NULL, why) && find("h23", NULL, why)->kind == PLACE_KOROK);
    CHECK(named("shrine neez yohma", "Ne'ez Yohma Shrine"));
    CHECK(named("ne'ez yohma", "Ne'ez Yohma Shrine"));
    CHECK(named("oman au", "Oman Au Shrine"));
    CHECK(named("shrine pushing power", "Ne'ez Yohma Shrine"));  // (the quest title)
    CHECK(named("stable wetland", "Wetland Stable"));
    CHECK(named("wetland", "Wetland Stable"));
    CHECK(named("hateno", "Hateno Village"));
    CHECK(named("hateno tower", "Hateno Tower"));
    CHECK(named("korok forest", "Korok Forest"));  // (a kind word that starts a name)
    CHECK(named("hyrule castle", "Hyrule Castle"));

    // several matches name them, none says so
    CHECK(find("stable", NULL, why) == NULL && strstr(why, "Which stable"));
    CHECK(find("ancient", NULL, why) == NULL && strstr(why, "matches 2") && strstr(why, "Akkala"));
    CHECK(named("south akkala", "South Akkala Stable"));
    CHECK(find("zzzz", NULL, why) == NULL && strstr(why, "No place called"));
    CHECK(find("", NULL, why) == NULL && strstr(why, "Where to"));

    // nearest
    float near_hateno[3] = {3600, 360, 2100};
    const Place *p = find("nearest village", near_hateno, why);
    CHECK(p && !strcmp(p->name, "Hateno Village"));
    p = find("korok nearest", near_hateno, why);
    CHECK(p && p->kind == PLACE_KOROK);
    CHECK(find("korok nearest", NULL, why) == NULL && strstr(why, "no position"));

    // lists
    const Place *out[10];
    CHECK(place_list(PLACE_SHRINE, 0, NULL, out, 10) == 136);
    char *akkala[] = {(char *)"akkala"};
    int n = place_list(PLACE_STABLE, 1, akkala, out, 10);
    CHECK(n == 2);
    CHECK(place_list(-1, 0, NULL, out, 10) == 136 + 15 + 15 + 9 + 9);  // (Koroks only by kind)

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
