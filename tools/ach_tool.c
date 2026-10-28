// Helper for the achievements (runtime/achievements.h).
//   ach_tool verify <game_data.sav>        for each achievement: which of its flag names exist in the save, and its progress
//   ach_tool get <game_data.sav> <name>... the value of flags by name or "#<hex crc32>" ("not in the save" otherwise)
//   ach_tool guess <game_data.sav>         which of a list of likely flag names (Korok seeds, shrines, ...) exist, with value
//   ach_tool horses <game_data.sav>        the 6 horse-name slots of the save (what "A Little Horse" and "Satoru Iwata" read)
//   ach_tool diff <before.sav> <after.sav> [names.txt]
//        the entries (crc32 + value) that changed between two saves: do one thing in the game (collect one Korok seed),
//        save, and see what it touched. A value that went up by exactly 1 is marked "<-- +1". Names are shown for the
//        flags this tool knows (the achievements' own and the guess list), and for any in names.txt (one name per line).
//        An unknown flag can still be used by its hash: "#<hex crc32>" in achievements_defs.c.
//   ach_tool counter <s0.sav> <s1.sav> <s2.sav> [...]
//        saves taken in order, one more of the same thing between each (collect a Korok seed, save, copy; again): only
//        the entries that went up by exactly 1 at every step. A counter is there; the one-off flags of each seed aren't.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/achievements.h"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { perror(p); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Names that might be the flags of things worth an achievement (guesses: `guess` says which ones a save really has)
static const char *const guesses[] = {
    "HiddenKorokNum", "HiddenKorokNumTotal", "KorokHiddenNum", "HiddenKorokCount", "HiddenKorokGetNum",
    "KorokNutsNum", "KorokNuts", "KorokNum", "KorokSeedNum", "KorokSeed", "KorokSeeds", "KorokNutsCount", "KorokCount",
    "KorokGetNum", "KorokNutsGetNum", "KorokNutsTotal", "KorokNutsNumTotal", "KorokNutsNumAll", "KorokTotal", "FoundKorokNum",
    "GetKorokNum", "GetKorokNutsNum", "Korok_Num", "Korok_Count", "KorokClearNum", "KorokFindNum", "KorokNutsFindNum",
    "NutsNum", "KorokMaskNum", "KorokMask", "HestuNutsNum", "HestuNum", "Hestu_Count", "Korok_Total", "Korok_Nuts_Num",
    "Kokok_Num", "KorokGet", "KorokNutsMax", "KorokNutsGet", "Obj_KorokNuts", "KorokNutsNumHave", "KorokNutsHave",
    "MaxHartValue", "CurrentHart", "MaxStaminaAmount", "CurrentStamina", "CurrentRupee", "PlayTime", "IsGet_PlayerStole2",
    "PictureBookGetNum", "RupeeNum", "DungeonClearCounter", "ClearDungeonCount", "DungeonClearNum", "TowerOpenNum",
    "Open_Tower_Num", "TowerNum", "MemoryClearNum", "FairyNum", "GreatFairyNum", "HeroSoulNum", "Guardian_Num",
};

typedef struct { uint32_t hash; uint32_t occ; uint32_t value; } Ent;
static int cmp_ent(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    return x->hash != y->hash ? (x->hash < y->hash ? -1 : 1) : x->occ != y->occ ? (x->occ < y->occ ? -1 : 1) : 0;
}
// the entries of a save sorted by (crc32, how many entries of that crc32 came before): vectors and arrays repeat a hash
static Ent *entries(const uint8_t *d, size_t n, size_t *count) {
    size_t cap = n > 0xC ? (n - 0xC) / 8 : 0, k = 0;
    Ent *e = malloc((cap + 1) * sizeof *e);
    for (size_t o = 0xC; o + 8 <= n; o += 8) e[k++] = (Ent){rd32(d + o), 0, rd32(d + o + 4)};
    // occurrence numbers in file order, then sort

    for (size_t i = 0; i < k; i++) e[i].occ = (uint32_t)i;
    qsort(e, k, sizeof *e, cmp_ent);
    uint32_t run = 0;
    for (size_t i = 0; i < k; i++) {
        run = i && e[i].hash == e[i - 1].hash ? run + 1 : 0;
        e[i].occ = run;
    }
    *count = k;
    return e;
}

static const char *known_name(uint32_t h, char (*extra)[256], uint32_t *extra_h, size_t n_extra) {
    for (size_t i = 0; i < sizeof guesses / sizeof guesses[0]; i++) if (ach_crc32(guesses[i]) == h) return guesses[i];
    for (size_t i = 0; i < n_extra; i++) if (extra_h[i] == h) return extra[i];
    for (int i = 0; i < g_ach_count; i++) {
        const AchDef *d = &g_ach_defs[i];
        if (d->kind == ACH_FLAG && !strchr(d->flag, ',') && ach_crc32(d->flag) == h) return d->flag;
    }
    return NULL;
}

typedef struct { const AchSave *s; int found, total; } Tally;
static void tally(const char *name, void *u) {
    Tally *t = u;
    t->total++;
    t->found += ach_save_get_named(t->s, name, NULL);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "verify")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        AchSave s = {d, n};
        if (!ach_save_ok(&s)) { fprintf(stderr, "%s doesn't look like a game_data.sav\n", argv[2]); return 2; }
        int missing = 0;
        for (int i = 0; i < g_ach_count; i++) {
            const AchDef *a = &g_ach_defs[i];
            Tally t = {&s, 0, 0};
            ach_def_flags(a, tally, &t);
            uint32_t cur, target;
            bool met = ach_eval(a, &s, &cur, &target);
            const char *state = t.found == 0 ? "NO FLAG IN SAVE" : t.found < t.total && a->kind != ACH_COUNT ? "SOME FLAGS MISSING" : "ok";
            missing += t.found == 0 || (t.found < t.total && a->kind != ACH_COUNT);
            printf("%-14s %-18s flags %3d/%-3d  progress %u/%u %s\n", a->id, state, t.found, t.total, cur, target, met ? "(reached)" : "");
        }
        printf("%d of %d achievements have flag names that don't match this save\n", missing, g_ach_count);
        return missing ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "get")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        AchSave s = {d, n};
        for (int i = 3; i < argc; i++) {
            uint32_t v;
            if (ach_save_get_named(&s, argv[i], &v)) printf("%s = %d (0x%08x)\n", argv[i], (int32_t)v, v);
            else printf("%s: not in the save\n", argv[i]);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "horses")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        AchSave s = {d, n};
        char slots[16][ACH_STR_MAX];
        int k = ach_save_strings(&s, 0x7b74e117u, 16, slots, 16);
        if (!k) printf("no horse-name entries (crc32 0x7b74e117) in this save\n");
        for (int i = 0; i < k; i++) printf("slot %d: %s\n", i, slots[i][0] ? slots[i] : "(empty)");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "guess")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        AchSave s = {d, n};
        int hits = 0;
        for (size_t i = 0; i < sizeof guesses / sizeof guesses[0]; i++) {
            uint32_t v;
            if (ach_save_get_named(&s, guesses[i], &v)) printf("%-24s = %d\n", guesses[i], (int32_t)v), hits++;
        }
        printf("%d of %zu guessed names are in this save\n", hits, sizeof guesses / sizeof guesses[0]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "diff")) {
        size_t na, nb, ca, cb;
        uint8_t *a = slurp(argv[2], &na), *b = slurp(argv[3], &nb);
        Ent *ea = entries(a, na, &ca), *eb = entries(b, nb, &cb);
        static char extra[20000][256];
        static uint32_t extra_h[20000];
        size_t n_extra = 0;
        if (argc >= 5) {
            FILE *f = fopen(argv[4], "r");
            char line[256];
            while (f && n_extra < 20000 && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!line[0]) continue;
                snprintf(extra[n_extra], sizeof extra[0], "%s", line);
                extra_h[n_extra++] = ach_crc32(line);
            }
            if (f) fclose(f);
        }
        size_t i = 0, j = 0;
        int shown = 0;
        while (j < cb) {
            while (i < ca && (ea[i].hash < eb[j].hash || (ea[i].hash == eb[j].hash && ea[i].occ < eb[j].occ))) i++;
            bool had = i < ca && ea[i].hash == eb[j].hash && ea[i].occ == eb[j].occ;
            if (!had || ea[i].value != eb[j].value) {
                const char *nm = known_name(eb[j].hash, extra, extra_h, n_extra);
                int32_t after = (int32_t)eb[j].value, before = had ? (int32_t)ea[i].value : 0;
                printf("#%08x  %s%d -> %d%s%s%s\n", eb[j].hash, had ? "" : "(new) ", had ? before : 0, after,
                       had && after == before + 1 ? "   <-- +1" : "", nm ? "   " : "", nm ? nm : "");
                shown++;
            }
            j++;
        }
        printf("%d entries changed. To use one in an achievement: its #<hex> as the flag in runtime/achievements_defs.c\n", shown);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "counter")) {
        int ns = argc - 2;
        Ent *e[32];
        size_t c[32];
        if (ns > 32) ns = 32;
        for (int k = 0; k < ns; k++) {
            size_t n;
            uint8_t *d = slurp(argv[2 + k], &n);
            e[k] = entries(d, n, &c[k]);
        }
        int shown = 0;
        for (size_t i = 0; i < c[0]; i++) {
            uint32_t prev = e[0][i].value;
            bool ok = true;
            for (int k = 1; k < ns && ok; k++) {
                Ent key = {e[0][i].hash, e[0][i].occ, 0};
                Ent *f = bsearch(&key, e[k], c[k], sizeof *e[k], cmp_ent);
                ok = f && (int32_t)f->value == (int32_t)prev + 1;
                if (f) prev = f->value;
            }
            if (!ok) continue;
            const char *nm = known_name(e[0][i].hash, NULL, NULL, 0);
            printf("#%08x  %d -> %d  (+1 at each of the %d steps)%s%s\n", e[0][i].hash, (int32_t)e[0][i].value, (int32_t)prev,
                   ns - 1, nm ? "   " : "", nm ? nm : "");
            shown++;
        }
        printf("%d entries went up by exactly 1 at every step\n", shown);
        return 0;
    }
    fprintf(stderr, "usage: ach_tool verify <game_data.sav> | horses <game_data.sav> | get <game_data.sav> <name|#hash>... | guess <game_data.sav> |\n"
                    "       diff <before.sav> <after.sav> [names.txt] | counter <s0.sav> <s1.sav> <s2.sav> [...]\n");
    return 2;
}
