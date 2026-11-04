// Where the player is (game_place.h). game_place_on_read keeps the last stage the game loaded; game_place_text turns it
// and Link's position into text.
#include "game_place.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef enum { INTERIOR_SHRINE, INTERIOR_BEAST, INTERIOR_TRIAL } InteriorKind;
#define PLACE_LANGUAGES 16
typedef struct { float x, z, radius; bool town; const char *name[PLACE_LANGUAGES]; } PlaceEntry;
typedef struct { const char *pack, *name[PLACE_LANGUAGES]; InteriorKind kind; } InteriorEntry;

// The names are the game's own text, so the table is never committed: make writes it from your extracted/ (Makefile)
#if __has_include("game_place_table.inc")
#include "game_place_table.inc"
#else  // (no game files: no names, English)
static const char *const place_languages[PLACE_LANGUAGES] = {"USen"};
static const char *const place_texts[PLACE_LANGUAGES][5] = {{"In the main menu", "Exploring", "Inside a shrine", "Divine Beast", "Interior"}};
static const PlaceEntry place_table[] = {{0, 0, 1, false, {NULL}}};
static const char *const region_names[1][PLACE_LANGUAGES] = {{"Hyrule"}};
static const char region_grid[80][101] = {{0}};
static const InteriorEntry interior_table[] = {{"", {"Interior"}, INTERIOR_TRIAL}};
#endif

#define COUNT(a) (sizeof(a) / sizeof *(a))
enum { STAGE_TITLE = -2, STAGE_FIELD = -1 };  // or an index of interior_table
static atomic_int stage = STAGE_TITLE;
static int lang;  // an index of place_languages

void game_place_set_language(const char *language) {
    if (!strcmp(language, "en")) language = "USen";
    if (!strcmp(language, "es")) language = "USes";
    for (int i = 0; i < PLACE_LANGUAGES; i++)
        if (place_languages[i] && !strcmp(language, place_languages[i])) lang = i;
}

void game_place_on_read(const char *path) {
    if (!path) return;
    // The main field is coming back. Its map files stay cached over a shrine, these are read again; none of them is read
    // inside one (unlike UI/MapTex/MainField/, the map screen's)
    static const char *const field[] = {"Map/MainField/", "Physics/StaticCompound/MainField/",
                                        "Physics/TeraMeshRigidBody/MainField/", "NavMesh/MainField/"};
    for (size_t i = 0; i < COUNT(field); i++)
        if (!strncmp(path, field[i], strlen(field[i]))) { atomic_store(&stage, STAGE_FIELD); return; }
    if (strncmp(path, "Pack/", 5)) return;
    const char *name = path + 5;
    if (!strcmp(name, "Title.pack")) { atomic_store(&stage, STAGE_TITLE); return; }
    size_t len = strcspn(name, ".");
    if (strcmp(name + len, ".pack")) return;
    for (size_t i = 0; i < COUNT(interior_table); i++)
        if (strlen(interior_table[i].pack) == len && !strncmp(name, interior_table[i].pack, len)) {
            atomic_store(&stage, (int)i);
            return;
        }
}

const char *game_place_name(float x, float z) {
    const char *best = NULL;
    float best_score = 1;  // (a town scores below 0: inside one, its name wins)
    for (size_t i = 0; i < COUNT(place_table); i++) {
        const PlaceEntry *p = &place_table[i];
        float dx = x - p->x, dz = z - p->z, score = (dx * dx + dz * dz) / (p->radius * p->radius);
        if (score >= 1) continue;
        if (p->town) score -= 1;
        if (score < best_score) { best_score = score; best = p->name[lang]; }
    }
    return best;
}

const char *game_place_region(float x, float z) {
    int col = (int)((x + 5000.0f) / 100.0f), row = (int)((z + 4000.0f) / 100.0f);
    if (x < -5000.0f || z < -4000.0f || col >= 100 || row >= 80) return NULL;
    char c = region_grid[row][col];
    return c < 'a' ? NULL : region_names[c - 'a'][lang];
}

bool game_place_text(float x, float z, bool has_link, char *details, size_t details_size, char *state, size_t state_size) {
    int s = atomic_load(&stage);
    if (s == STAGE_TITLE && has_link) {  // (Continue: the map files may come before the title leaves)
        int title = STAGE_TITLE;
        atomic_compare_exchange_strong(&stage, &title, STAGE_FIELD);
        s = STAGE_FIELD;
    }
    if (s == STAGE_TITLE) {
        snprintf(details, details_size, "%s", place_texts[lang][0]);
        snprintf(state, state_size, "%s", "");
        return true;
    }
    if (!has_link) return false;
    if (s >= 0) {
        snprintf(details, details_size, "%s", interior_table[s].name[lang]);
        snprintf(state, state_size, "%s", place_texts[lang][2 + interior_table[s].kind]);
        return true;
    }
    const char *name = game_place_name(x, z), *region = game_place_region(x, z);
    snprintf(details, details_size, "%s", name ? name : place_texts[lang][1]);
    snprintf(state, state_size, "%s", region ? region : "Hyrule");
    return true;
}
