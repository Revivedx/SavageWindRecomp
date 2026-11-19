#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/game_place.c"

static char details[128], state[128];

static bool text(float x, float z, bool has_link) { return game_place_text(x, z, has_link, details, sizeof details, state, sizeof state); }

static void expect(const char *d, const char *s) {
    if (strcmp(details, d) || strcmp(state, s)) fprintf(stderr, "got '%s' / '%s', want '%s' / '%s'\n", details, state, d, s);
    assert(!strcmp(details, d) && !strcmp(state, s));
}

int main(void) {
    game_place_set_language("EUes");
    // The name and region of places whose position is known (runtime/services/test_warp.c)
    assert(!strcmp(game_place_name(1811.7f, 982.6f), "Aldea Kakariko"));
    assert(!strcmp(game_place_name(3437.5f, 2101.5f), "Aldea Hatelia"));
    assert(!strcmp(game_place_name(-254.0f, -544.6f), "Castillo de Hyrule"));
    assert(!strcmp(game_place_region(-3607.2f, -1805.5f), "Tabanta"));
    assert(!strcmp(game_place_region(1719.0f, -2456.2f), "Eldin"));
    assert(!strcmp(game_place_region(3437.5f, 2101.5f), "Necluda"));

    // Off the map: no region, no name
    assert(!game_place_region(-9000, 9000));
    assert(!game_place_name(-9000, 9000));

    // The title screen, until Link is in the world
    assert(text(0, 0, false));
    expect("En el menú principal", "");
    assert(text(1811.7f, 982.6f, true));
    expect("Aldea Kakariko", "Picos Gemelos");

    // Without a Link, the last text stays
    assert(!text(0, 0, false));

    // A shrine and back
    game_place_on_read("Pack/Dungeon000.pack");
    assert(text(5, 5, true));
    expect("Santuario de Tamuur", "Interior de santuario");
    game_place_on_read("Actor/Pack/Anything.sbactorpack");  // (other files change nothing)
    assert(text(5, 5, true));
    expect("Santuario de Tamuur", "Interior de santuario");
    game_place_on_read("Map/MainField/D-6/D-6_Static.smubin");
    assert(text(1811.7f, 982.6f, true));
    expect("Aldea Kakariko", "Picos Gemelos");

    // Leaving a shrine with the map files still cached: only the field's physics and navigation are read again
    game_place_on_read("Pack/Dungeon099.pack");
    game_place_on_read("UI/MapTex/MainField/MapTex3_A-0_0.sbmapt");  // (the map screen, inside the shrine)
    assert(text(5, 5, true));
    expect("Santuario de Ranhtoe", "Interior de santuario");
    game_place_on_read("Physics/StaticCompound/MainField/G-5-0.shksc");
    assert(text(1811.7f, 982.6f, true));
    expect("Aldea Kakariko", "Picos Gemelos");
    game_place_on_read("Pack/Dungeon099.pack");
    game_place_on_read("NavMesh/MainField/27-19.shknm2");
    assert(text(1811.7f, 982.6f, true));
    expect("Aldea Kakariko", "Picos Gemelos");

    // A Divine Beast, the add-on content's interiors
    game_place_on_read("Pack/RemainsWater.pack");
    assert(text(48, 3, true));
    expect("Vah Ruta", "Bestia Divina");
    game_place_on_read("Pack/Dungeon120.pack");
    assert(text(48, 3, true));
    expect("Santuario", "Interior de santuario");
    game_place_on_read("Pack/FinalTrial.pack");
    assert(text(48, 3, true));
    expect("Prueba final", "Interior");

    // Back to the title
    game_place_on_read("Pack/Title.pack");
    assert(text(48, 3, false));
    expect("En el menú principal", "");

    // Far from every name: exploring, in its region
    game_place_on_read("Map/MainField/D-6/D-6_Static.smubin");
    float x = 0, z = 0;
    bool found = false;
    for (x = -4900; x < 4900 && !found; x += 50)
        for (z = -3900; z < 3900 && !found; z += 50)
            if (!game_place_name(x, z) && game_place_region(x, z)) found = true;
    assert(found);
    assert(text(x - 50, z - 50, true));
    assert(!strcmp(details, "Explorando") && strlen(state) > 0);

    // English
    game_place_set_language("en");
    assert(!strcmp(game_place_name(1811.7f, 982.6f), "Kakariko Village"));
    assert(!strcmp(game_place_region(1811.7f, 982.6f), "Dueling Peaks"));
    assert(!strcmp(game_place_region(-1755, -774), "Ridgeland"));
    game_place_on_read("Pack/Title.pack");
    assert(text(0, 0, false));
    expect("In the main menu", "");
    game_place_on_read("Pack/RemainsWater.pack");
    assert(text(48, 3, true));
    expect("Vah Ruta", "Divine Beast");
    game_place_on_read("Pack/Dungeon120.pack");
    assert(text(48, 3, true));
    expect("Shrine", "Inside a shrine");
    game_place_on_read("Pack/Dungeon000.pack");
    assert(text(48, 3, true));
    assert(strstr(details, "Shrine") && strcmp(details, "Shrine"));
    game_place_on_read("Map/MainField/D-6/D-6_Static.smubin");
    assert(text(x - 50, z - 50, true));
    assert(!strcmp(details, "Exploring"));

    // The other languages: the game's own names, and an unknown language changes nothing
    game_place_set_language("EUde");
    assert(!strcmp(game_place_region(-2173, -2034), "Hebra-Turm"));
    game_place_on_read("Pack/Title.pack");
    assert(text(0, 0, false));
    expect("Im Hauptmenü", "");
    game_place_set_language("nope");
    assert(text(0, 0, false));
    expect("Im Hauptmenü", "");
    game_place_set_language("JPja");
    game_place_on_read("Pack/Dungeon000.pack");
    assert(text(5, 5, true));
    expect("タ・ムールの祠", "祠の中");
    game_place_on_read("Pack/RemainsWater.pack");
    assert(text(5, 5, true));
    expect("ヴァ・ルッタ", "神獣");
    game_place_on_read("Pack/Dungeon120.pack");
    assert(text(5, 5, true));
    expect("試練の祠", "祠の中");
    game_place_set_language("USes");
    assert(text(5, 5, true));
    expect("Santuario", "Interior de santuario");

    puts("PASS: game_place");
}
