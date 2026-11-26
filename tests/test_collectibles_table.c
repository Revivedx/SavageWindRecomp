// Cheats > Collectible hints on the map: the table runtime/collectibles_table.c builds from the player's own game files
// (an extracted RomFS), printed as tools/gen_collectibles.py prints its own (make test-collectibles ROMFS=...).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collectibles_table.h"

static const char *romfs;

// (runtime/services/fs.c's, from the extracted RomFS)
uint8_t *fs_game_file(const char *path, size_t *out_n) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", romfs, path);
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? (size_t)n : 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
    fclose(f);
    if (d) *out_n = (size_t)n;
    return d;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_collectibles_table <extracted RomFS>\n"); return 2; }
    romfs = argv[1];
    static CollectTable t;
    char names[COLLECT_REGIONS][64];
    if (!collect_table_build(&t) || !collect_table_names(&t, "USen", names)) return 1;
    for (int r = 0; r < COLLECT_REGIONS; r++) printf("region %d %s %08x %s\n", r, t.tower_msg[r], t.tower[r], names[r]);
    for (int i = 0; i < COLLECT_KOROKS; i++)
        printf("korok %d %08x %.1f %.1f\n", t.korok[i].region, t.korok[i].found, t.korok[i].x, t.korok[i].z);
    for (int i = 0; i < COLLECT_SHRINES; i++)
        printf("shrine %d %08x %08x %.1f %.1f\n", t.shrine[i].region, t.shrine[i].found, t.shrine[i].cleared, t.shrine[i].x,
               t.shrine[i].z);
    printf("total %08x\n", t.koroks_found);
    static const char *const languages[] = {"USen", "EUen", "USes", "EUes", "USfr", "EUfr", "EUde", "EUit",
                                            "EUnl", "EUru", "USpt", "JPja", "KRko", "CNzh", "TWzh", "THth"};
    for (size_t l = 0; l < sizeof languages / sizeof *languages; l++) {  // (tools/gen_place_table.py LANGUAGES)
        char other[COLLECT_REGIONS][64];
        if (!collect_table_names(&t, languages[l], other)) return 1;
        printf("names %s", languages[l]);
        for (int r = 0; r < COLLECT_REGIONS; r++) printf("%s%s", r ? " | " : " ", other[r]);
        printf("\n");
    }
    for (int row = 0; row < 80; row++) {  // (each 100-unit cell's region, at its centre)
        char line[101];
        for (int col = 0; col < 100; col++) {
            int r = collect_table_region(&t, -5000 + (col + .5f) * 100, -4000 + (row + .5f) * 100);
            line[col] = r >= 0 ? (char)('a' + r) : '.';
        }
        line[100] = 0;
        printf("grid %s\n", line);
    }
    return 0;
}
