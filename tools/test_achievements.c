// Tests of the achievements core and watcher (runtime/achievements*.c) on made-up saves. Run: tools/test_achievements.sh
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#ifdef _WIN32  // (MinGW: mkdir takes one argument, no mkdtemp)
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define SLEEP_S(n) Sleep((n) * 1000)
#else
#define MKDIR(p) mkdir(p, 0755)
#define SLEEP_S(n) sleep(n)
#endif

#include "../runtime/achievements.h"

const char *g_save_dir = "save";
static bool warp;
bool test_warp_active(void) { return warp; }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { uint8_t *d; size_t n; } Buf;
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void add(Buf *b, const char *name, int32_t v) {
    b->d = realloc(b->d, b->n + 8);
    put32(b->d + b->n, name[0] == '#' ? (uint32_t)strtoul(name + 1, NULL, 16) : ach_crc32(name));
    put32(b->d + b->n + 4, (uint32_t)v);
    b->n += 8;
}
static Buf new_save(void) { Buf b = {calloc(1, 0xC), 0xC}; return b; }  // (0xC-byte header)
// A string array (the horse names): `n_slots` slots of 16 entries, 4 characters each, all under the same hash. `raw` is the bytes of
// each slot (up to 64; the rest stays NUL), `len` how many of them are given.
static void add_slots(Buf *b, uint32_t hash, int n_slots, const char *const *raw, const int *len) {
    for (int i = 0; i < n_slots; i++)
        for (int e = 0; e < 16; e++) {
            uint8_t chunk[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) { int pos = e * 4 + k; if (raw[i] && pos < len[i]) chunk[k] = (uint8_t)raw[i][pos]; }
            b->d = realloc(b->d, b->n + 8);
            put32(b->d + b->n, hash);
            memcpy(b->d + b->n + 4, chunk, 4);
            b->n += 8;
        }
}
static AchSave as(const Buf *b) { AchSave s = {b->d, b->n}; return s; }
static int idx(const char *id) {
    for (int i = 0; i < g_ach_count; i++) if (!strcmp(g_ach_defs[i].id, id)) return i;
    return -1;
}
#define SLOT_A "save/save_1/0/game_data.sav"
#define SLOT_A2 "save/save_1/1/game_data.sav"  // (another slot of the same playthrough)
#define SLOT_B "save/save_2/0/game_data.sav"   // (another playthrough)
static bool is_unlocked(const char *id) { AchInfo in; ach_info(idx(id), &in); return in.unlocked; }
static void write_file(const char *path, const Buf *b) { FILE *f = fopen(path, "wb"); fwrite(b->d, 1, b->n, f); fclose(f); }
static void write_file_at(const char *path, const Buf *b, time_t when) {  // ... with that file time
    write_file(path, b);
    struct utimbuf ut = {when, when};
    utime(path, &ut);
}

int main(void) {
    char tmpl[512];
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    snprintf(tmpl, sizeof tmpl, "%s/achtest%d", tmp, (int)getpid());
    MKDIR(tmpl);
    CHECK(chdir(tmpl) == 0);

    // crc32 (the standard check value) and ids that are unique
    CHECK(ach_crc32("123456789") == 0xCBF43926u);
    for (int i = 0; i < g_ach_count; i++)
        for (int j = i + 1; j < g_ach_count; j++) CHECK(strcmp(g_ach_defs[i].id, g_ach_defs[j].id));

    // evaluation
    ach_state_load("achievements.txt");
    ach_set_active("./save//save_1/0/game_data.sav");  // (the game opened it; paths are compared normalized)
    Buf b = new_save();
    add(&b, "IsGet_PlayerStole2", 1);
    for (int i = 0; i < 4; i++) { char n[40]; snprintf(n, sizeof n, "Clear_Dungeon%03d", i); add(&b, n, 1); }
    add(&b, "Clear_Dungeon010", 0);  // present but not done
    add(&b, "MaxHartValue", 84);
    add(&b, "Clear_RemainsWater", 1); add(&b, "Clear_RemainsFire", 1); add(&b, "Clear_RemainsWind", 1);
    add(&b, "#8a94e07a", -5);
    AchSave s = as(&b);
    CHECK(ach_save_ok(&s));
    int fresh = ach_update(SLOT_A, &s, 1000);
    CHECK(is_unlocked("paraglider") && is_unlocked("shrine_1") && is_unlocked("shrine_4") && is_unlocked("hearts_20") &&
          is_unlocked("beast_ruta") && is_unlocked("beast_rudania") && is_unlocked("beast_medoh"));
    CHECK(!is_unlocked("shrine_25") && !is_unlocked("hearts_30") && !is_unlocked("beast_naboris") && !is_unlocked("beast_all") &&
          !is_unlocked("korok_100") && !is_unlocked("master_sword"));
    CHECK(fresh == 7 && ach_unlocked_total() == 7);
    AchInfo in;
    ach_info(idx("shrine_25"), &in);
    CHECK(in.cur == 4 && in.target == 25);
    ach_info(idx("hearts_30"), &in);
    CHECK(in.cur == 84 && in.target == 120);
    ach_info(idx("beast_all"), &in);
    CHECK(in.cur == 3 && in.target == 4);
    ach_info(idx("korok_100"), &in);
    CHECK(in.cur == 0);  // (negative value)
    // the Korok counter under its main name, and under the alternative one
    Buf kb = new_save();
    add(&kb, "#8a94e07a", 120);
    AchSave ks = as(&kb);
    uint32_t kc, kt;
    CHECK(ach_eval(&g_ach_defs[idx("korok_100")], &ks, &kc, &kt) && kc == 100 && kt == 100);
    CHECK(!ach_eval(&g_ach_defs[idx("korok_450")], &ks, &kc, &kt) && kc == 120 && kt == 450);
    Buf kb2 = new_save();
    add(&kb2, "#8a94e07a", 450);
    AchSave ks2 = as(&kb2);
    CHECK(ach_eval(&g_ach_defs[idx("korok_450")], &ks2, &kc, &kt) && !ach_eval(&g_ach_defs[idx("korok_900")], &ks2, &kc, &kt));

    // the long-list kinds: counting dishes, adding up kills
    {
        Buf nb = new_save();
        for (int i = 1; i <= 12; i++) { char n[48]; snprintf(n, sizeof n, "IsGet_Item_Cook_A_%02d", i); add(&nb, n, 1); }  // (A_06 isn't a dish: 11 of the 12 count)
        add(&nb, "Defeated_Enemy_Guardian_A_Num", 6);
        add(&nb, "Defeated_Enemy_Guardian_B_Num", 5);
        add(&nb, "MapTower_07", 1);
        AchSave ns = as(&nb);
        uint32_t c2, t2;
        CHECK(ach_eval(&g_ach_defs[idx("cook_10")], &ns, &c2, &t2) && c2 == 10 && t2 == 10);
        CHECK(!ach_eval(&g_ach_defs[idx("cook_50")], &ns, &c2, &t2) && c2 == 11 && t2 == 50);
        CHECK(ach_eval(&g_ach_defs[idx("guardians_10")], &ns, &c2, &t2) && c2 == 10);  // (6 + 5, capped at the goal)
        CHECK(ach_eval(&g_ach_defs[idx("first_enemy")], &ns, &c2, &t2));
        CHECK(!ach_eval(&g_ach_defs[idx("map_towers")], &ns, &c2, &t2) && c2 == 1 && t2 == 15);
        CHECK(!ach_eval(&g_ach_defs[idx("dex_creatures")], &ns, &c2, &t2) && t2 == (uint32_t)g_ach_defs[idx("dex_creatures")].n_list);
    }

    // gear, collections and horses
    {
        uint32_t c3, t3;
        // the six ancient parts: five of them are not enough
        Buf ab = new_save();
        for (int i = 26; i <= 30; i++) { char n[40]; snprintf(n, sizeof n, "IsGet_Item_Enemy_%d", i); add(&ab, n, 1); }
        add(&ab, "IsGet_Item_Enemy_31", 0);  // (the Giant Ancient Core: in the save, not found yet)
        AchSave as1 = as(&ab);
        CHECK(!ach_eval(&g_ach_defs[idx("ancient_parts")], &as1, &c3, &t3) && c3 == 5 && t3 == 6);
        Buf ab2 = new_save();  // (a flag appears once in a save: build the one with all six)
        for (int i = 26; i <= 31; i++) { char n[40]; snprintf(n, sizeof n, "IsGet_Item_Enemy_%d", i); add(&ab2, n, 1); }
        AchSave as2 = as(&ab2);
        CHECK(ach_eval(&g_ach_defs[idx("ancient_parts")], &as2, &c3, &t3) && c3 == 6);
        free(ab2.d);
        // the Dark Set needs the three pieces; the shield is one flag; the old memories need sword and shield
        Buf gb = new_save();
        add(&gb, "IsGet_Armor_160_Head", 1); add(&gb, "IsGet_Armor_160_Upper", 1); add(&gb, "IsGet_Armor_160_Lower", 0);
        add(&gb, "IsGet_Weapon_Sword_070", 1);
        AchSave gs = as(&gb);
        CHECK(!ach_eval(&g_ach_defs[idx("dark_armor")], &gs, &c3, &t3) && c3 == 2 && t3 == 3);
        CHECK(!ach_eval(&g_ach_defs[idx("hylian_shield")], &gs, &c3, &t3));
        CHECK(!ach_eval(&g_ach_defs[idx("old_memories")], &gs, &c3, &t3) && c3 == 1 && t3 == 2);
        Buf gb2 = new_save();
        add(&gb2, "IsGet_Armor_160_Head", 1); add(&gb2, "IsGet_Armor_160_Upper", 1); add(&gb2, "IsGet_Armor_160_Lower", 1);
        add(&gb2, "IsGet_Weapon_Shield_030", 1); add(&gb2, "IsGet_Weapon_Sword_070", 1);
        AchSave gs2 = as(&gb2);
        CHECK(ach_eval(&g_ach_defs[idx("dark_armor")], &gs2, &c3, &t3) &&
              ach_eval(&g_ach_defs[idx("hylian_shield")], &gs2, &c3, &t3) && ach_eval(&g_ach_defs[idx("old_memories")], &gs2, &c3, &t3));
        free(gb2.d);

        // horses: 6 slots of 64 characters under crc32 0x7b74e117
        const uint32_t H = 0x7b74e117u;
        const char *none[6] = {0};
        const int nolen[6] = {0};
        Buf hb = new_save();
        add_slots(&hb, H, 6, none, nolen);
        AchSave hs = as(&hb);
        CHECK(!ach_eval(&g_ach_defs[idx("horse_registered")], &hs, &c3, &t3) && c3 == 0);  // (no horse: every slot empty)
        CHECK(!ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3));
        char slots[16][ACH_STR_MAX];
        CHECK(ach_save_strings(&hs, H, 16, slots, 16) == 6 && !slots[0][0]);
        free(hb.d);

        const char *one[6] = {"Epona", 0, 0, 0, 0, 0};
        const int onelen[6] = {5, 0, 0, 0, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, one, onelen);
        hs = as(&hb);
        CHECK(ach_eval(&g_ach_defs[idx("horse_registered")], &hs, &c3, &t3));
        CHECK(!ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3));  // (a horse with another name)
        CHECK(ach_save_strings(&hs, H, 16, slots, 16) == 6 && !strcmp(slots[0], "Epona") && !slots[1][0]);
        free(hb.d);

        // the legend's name in any slot, in any case, with blanks around it and with left-over characters after the NUL
        const char *many[6] = {"Epona", "Roach", "  sAtoRu iwAta ", 0, 0, 0};
        const int manylen[6] = {5, 5, 15, 0, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, many, manylen);
        hs = as(&hb);
        CHECK(ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3) && ach_eval(&g_ach_defs[idx("horse_registered")], &hs, &c3, &t3));
        free(hb.d);
        const char *stale[6] = {"Satoru Iwata\0ZZZZZ", 0, 0, 0, 0, 0};
        const int stalelen[6] = {18, 0, 0, 0, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, stale, stalelen);
        hs = as(&hb);
        CHECK(ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3));
        CHECK(ach_save_strings(&hs, H, 16, slots, 16) == 6 && !strcmp(slots[0], "Satoru Iwata"));
        free(hb.d);
        const char *first[6] = {" satoru ", 0, 0, 0, 0, 0};  // (his first name alone is enough)
        const int firstlen[6] = {8, 0, 0, 0, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, first, firstlen);
        hs = as(&hb);
        CHECK(ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3));
        free(hb.d);
        const char *close[6] = {"Satoru Iwat", "Satoru Iwata Jr", "Satorus", "Iwata", 0, 0};  // (not the name)
        const int closelen[6] = {11, 15, 7, 5, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, close, closelen);
        hs = as(&hb);
        CHECK(!ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3) && ach_eval(&g_ach_defs[idx("horse_registered")], &hs, &c3, &t3));
        free(hb.d);
        // ASCII stored as UTF-16LE is read as well
        const char *wide[6] = {"S\0a\0t\0o\0r\0u\0 \0I\0w\0a\0t\0a\0", 0, 0, 0, 0, 0};
        const int widelen[6] = {24, 0, 0, 0, 0, 0};
        hb = new_save();
        add_slots(&hb, H, 6, wide, widelen);
        hs = as(&hb);
        CHECK(ach_eval(&g_ach_defs[idx("horse_iwata")], &hs, &c3, &t3));
        free(hb.d);
        // a save that has no horse entries at all: nothing, and no crash
        Buf nb2 = new_save();
        add(&nb2, "IsGet_PlayerStole2", 1);
        AchSave ns2 = as(&nb2);
        CHECK(!ach_eval(&g_ach_defs[idx("horse_registered")], &ns2, &c3, &t3) && ach_save_strings(&ns2, H, 16, slots, 16) == 0);
        free(nb2.d); free(ab.d); free(gb.d);
    }

    // toasts come out once, oldest first
    CHECK(ach_toast_pending() == 7);
    int t, n = 0;
    while (ach_toast_take(&t)) n++;
    CHECK(n == 7 && ach_toast_pending() == 0);
    CHECK(ach_update(SLOT_A, &s, 2000) == 0);  // nothing new the second time

    // the rest of the beasts
    add(&b, "Clear_RemainsElectric", 1);
    s = as(&b);
    CHECK(ach_update(SLOT_A, &s, 3000) == 2 && is_unlocked("beast_naboris") && is_unlocked("beast_all"));

    // another slot (autosave / manual save) of the same playthrough: the same achievements, nothing announced again
    Buf old = new_save();
    add(&old, "IsGet_PlayerStole2", 1);
    AchSave os = as(&old);
    ach_set_active(SLOT_A2);
    CHECK(ach_unlocked_total() == 9 && ach_toast_pending() == 2);
    CHECK(ach_update(SLOT_A2, &os, 3500) == 0 && ach_unlocked_total() == 9 && ach_toast_pending() == 2);
    ach_set_active(SLOT_A);

    // a save with no progress is a new game: starts over, and what it earns is announced again
    Buf empty = new_save();
    add(&empty, "SomethingElse", 1);
    AchSave es = as(&empty);
    CHECK(ach_update(SLOT_A2, &es, 4000) == 0 && !is_unlocked("paraglider") && ach_unlocked_total() == 0);
    CHECK(ach_update(SLOT_A, &os, 4100) == 1 && is_unlocked("paraglider") && ach_unlocked_total() == 1);

    // another playthrough keeps its own; what is shown follows the one being played
    CHECK(ach_update(SLOT_B, &s, 4500) == 0);  // (not the one being played: no notice)
    CHECK(ach_unlocked_total() == 1);          // (still showing the first)
    ach_set_active(SLOT_B);
    CHECK(ach_unlocked_total() == 9 && is_unlocked("beast_all"));
    ach_set_active(SLOT_A);
    CHECK(ach_unlocked_total() == 1);
    ach_set_active(SLOT_B);
    while (ach_toast_take(&t)) {}

    // persisted per playthrough; unknown ids kept; times kept
    FILE *f = fopen("achievements.txt", "a");
    fputs("future_one\t77\n", f);
    fclose(f);
    ach_state_load("achievements.txt");
    CHECK(ach_unlocked_total() == 0);  // (no save is being played yet)
    ach_set_active(SLOT_B);
    CHECK(ach_unlocked_total() == 9 && is_unlocked("beast_all"));
    ach_set_active(SLOT_A);
    CHECK(ach_unlocked_total() == 1);
    ach_set_active(SLOT_B);
    ach_info(idx("paraglider"), &in);
    CHECK(in.at == 4500);
    // back in the game after a restart: the same save again announces nothing
    CHECK(ach_update(SLOT_B, &s, 4600) == 0 && ach_toast_pending() == 0);
    // more progress: only the new one is announced
    Buf more = new_save();
    add(&more, "Clear_Dungeon000", 1);
    add(&more, "MaxHartValue", 120);
    s = as(&more);
    CHECK(ach_update(SLOT_B, &s, 5000) == 1 && is_unlocked("hearts_30") && is_unlocked("beast_all") && ach_unlocked_total() == 10);
    f = fopen("achievements.txt", "r");
    char all[4096] = {0};
    CHECK(fread(all, 1, sizeof all - 1, f) > 0);
    fclose(f);
    CHECK(strstr(all, "@\tsave/save_2") && strstr(all, "@\tsave/save_1") && strstr(all, "future_one\t77") &&
          strstr(all, "hearts_30\t5000"));
    // its files were all deleted: starts over, and is forgotten in the file
    ach_game_gone(SLOT_A2);
    ach_set_active(SLOT_A);
    CHECK(ach_unlocked_total() == 0);
    f = fopen("achievements.txt", "r");
    memset(all, 0, sizeof all);
    CHECK(fread(all, 1, sizeof all - 1, f) > 0);
    fclose(f);
    CHECK(!strstr(all, "@\tsave/save_1") && strstr(all, "@\tsave/save_2"));
    {
        char key[512];
        ach_game_key("./save//save_1/0/game_data.sav", key, sizeof key);
        CHECK(!strcmp(key, "save/save_1"));
        ach_game_key("save/game_data.sav", key, sizeof key);
        CHECK(!strcmp(key, "save"));
    }

    // blocked: progress, no unlocks
    unlink("achievements.txt");
    ach_state_load("achievements.txt");
    ach_set_active(SLOT_A);
    ach_set_blocked(true);
    s = as(&more);
    CHECK(ach_update(SLOT_A, &s, 90000) == 0 && ach_unlocked_total() == 0);
    ach_info(idx("hearts_30"), &in);
    CHECK(in.cur == 120 && !in.unlocked);
    ach_set_blocked(false);

    // a flag by its hash ("#<hex crc32>"), for one whose name isn't known
    {
        Buf h = new_save();
        add(&h, "SomeUnknownCounter", 2);
        AchSave hs = as(&h);
        char name[16];
        snprintf(name, sizeof name, "#%08x", ach_crc32("SomeUnknownCounter"));
        uint32_t v = 0;
        CHECK(ach_save_get_named(&hs, name, &v) && v == 2);
        CHECK(!ach_save_get_named(&hs, "#00000001", &v));
        free(h.d);
    }

    // leaving the Shrine of Resurrection: the cutscene Demo103_0 has played
    {
        Buf before = new_save(), after = new_save();
        add(&before, "IsPlayed_Demo103_0", 0);
        add(&after, "IsPlayed_Demo103_0", 1);
        AchSave sb = as(&before), sa = as(&after);
        uint32_t c, t;
        int i = idx("leave_shrine");
        CHECK(i >= 0 && !ach_eval(&g_ach_defs[i], &sb, &c, &t) && ach_eval(&g_ach_defs[i], &sa, &c, &t));
        free(before.d); free(after.d);
    }

    // the unlock sound: some audio, not clipping, ends in silence, and a short buffer is never overrun
    {
        int16_t *pcm = malloc(48000 * sizeof *pcm);
        size_t n = ach_synth_jingle(pcm, 48000, 48000);
        int peak = 0;
        for (size_t k = 0; k < n; k++) peak = abs(pcm[k]) > peak ? abs(pcm[k]) : peak;
        CHECK(n > 20000 && n < 48000 && peak > 3000 && peak < 32767 && pcm[n - 1] == 0);
        CHECK(ach_synth_jingle(pcm, 1000, 48000) <= 1000);
        free(pcm);
    }

    // a new game as the game really does it: written over one slot, the old playthrough's saves left in the others
    // (real files: the new game's first save notes them with their file times). Whatever order they're read in, those
    // don't count, after a restart too; the new game's own later saves do. So does a save copied in afterwards, though
    // it keeps an older file time than the new game's (a 100% save brought from a backup).
    {
#define SLOT_N0 "nsave/save_7/0/game_data.sav"
#define SLOT_N1 "nsave/save_7/1/game_data.sav"
#define SLOT_N2 "nsave/save_7/2/game_data.sav"
#define SLOT_N3 "nsave/save_7/3/game_data.sav"
        static const char *const dirs[] = {"nsave", "nsave/save_7", "nsave/save_7/0", "nsave/save_7/1", "nsave/save_7/2",
                                           "nsave/save_7/3"};
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) MKDIR(dirs[i]);
        const time_t T0 = 1700000000;  // (file times: Windows keeps none before 1980)
        AchSave cur_b = as(&b);
        Buf fresh_buf = new_save();
        add(&fresh_buf, "MaxHartValue", 12);  // (a real new game: three hearts)
        AchSave ng = as(&fresh_buf);
        write_file_at(SLOT_N0, &b, T0 + 100), write_file_at(SLOT_N2, &b, T0 + 150), write_file_at(SLOT_N1, &fresh_buf, T0 + 200);
        ach_set_active(SLOT_N1);
        while (ach_toast_take(&t)) {}
        CHECK(ach_update_at(SLOT_N0, &cur_b, 6000, T0 + 100) > 0 && is_unlocked("paraglider"));     // (the old playthrough)
        CHECK(ach_update_at(SLOT_N1, &ng, 6001, T0 + 200) == 0 && ach_unlocked_total() == 0);  // New Game, first save
        CHECK(ach_toast_pending() == 0);                                                  // (old notices dropped)
        CHECK(ach_update_at(SLOT_N2, &cur_b, 6002, T0 + 150) == 0 && ach_unlocked_total() == 0);  // an old save read later
        ach_state_load("achievements.txt");
        ach_set_active(SLOT_N1);
        CHECK(ach_update_at(SLOT_N0, &cur_b, 6003, T0 + 100) == 0 && ach_unlocked_total() == 0);  // after a restart too
        CHECK(ach_update_at(SLOT_N1, &ng, 6004, T0 + 200) == 0 && ach_unlocked_total() == 0);
        CHECK(ach_update_at(SLOT_N2, &os, 6005, T0 + 300) == 1 && is_unlocked("paraglider"));  // the new game's own progress
        CHECK(ach_toast_take(&t) && t == idx("paraglider"));
        write_file_at(SLOT_N3, &b, T0 + 50);  // (copied in: its file time is older than the new game's)
        CHECK(ach_update_at(SLOT_N3, &cur_b, 6006, T0 + 50) > 0 && is_unlocked("beast_ruta"));
        free(fresh_buf.d);
    }

    // a sound bank (.bars) made up here: one sound "Yaha", 14 DSP-ADPCM samples with zero coefficients and scale 1, so
    // each sample is its 4-bit value; a big-endian AMTA (as in the game's voice banks); a cut-off copy gives nothing
    {
        uint8_t bk[0x200] = {0};
        memcpy(bk, "BARS", 4), bk[8] = 0xFF, bk[9] = 0xFE, put32(bk + 0xC, 1);
        put32(bk + 0x14, 0x1C), put32(bk + 0x18, 0x48);  // (AMTA, FWAV)
        uint8_t *am = bk + 0x1C;
        memcpy(am, "AMTA", 4), am[4] = 0xFE, am[5] = 0xFF, am[0x1B] = 0x1C;  // (big-endian: STRG 0x1C on)
        memcpy(am + 0x1C, "STRG", 4), am[0x23] = 5, memcpy(am + 0x24, "Yaha", 5);
        uint8_t *fw = bk + 0x48, *info = fw + 0x40, *ci = info + 0x28, *data = info + 0x60;
        memcpy(fw, "FWAV", 4), fw[4] = 0xFF, fw[5] = 0xFE, fw[0x10] = 2;
        fw[0x14] = 0x00, fw[0x15] = 0x70, put32(fw + 0x18, 0x40);  // INFO
        fw[0x20] = 0x01, fw[0x21] = 0x70, put32(fw + 0x24, 0xA0);  // DATA
        memcpy(info, "INFO", 4), info[8] = 2, put32(info + 0xC, 32000), put32(info + 0x14, 14), put32(info + 0x1C, 1);
        put32(info + 0x24, 0x0C), put32(ci + 0xC, 0x10);  // (channel info 0x0C past the table; coefficients 0x10 past it)
        static const uint8_t frame[8] = {0x00, 0x12, 0x34, 0x56, 0x7F, 0x8E, 0xCB, 0xA9};
        memcpy(data, "DATA", 4), memcpy(data + 8, frame, 8);
        static const int16_t want[14] = {1, 2, 3, 4, 5, 6, 7, -1, -8, -2, -4, -5, -6, -7};
        int rate = 0;
        size_t count = 0;
        int16_t *pcm = ach_bars_sound(bk, sizeof bk, "Yaha", &rate, &count);
        CHECK(pcm && rate == 32000 && count == 14 && !memcmp(pcm, want, sizeof want));
        free(pcm);
        CHECK(!ach_bars_sound(bk, sizeof bk, "Yah", &rate, &count));                    // (whole names only)
        CHECK(!ach_bars_sound(bk, (size_t)(data + 12 - bk), "Yaha", &rate, &count));  // (samples cut off)
        CHECK(!ach_bars_sound(bk, 0x10, "Yaha", &rate, &count));
    }

    // too small / not a save
    AchSave tiny = {(const uint8_t *)"abc", 3};
    CHECK(!ach_save_ok(&tiny) && ach_update(SLOT_A, &tiny, 1) == 0);

    // watcher: a save appears under the save folder, then gets another slot
    unlink("achievements.txt");
    CHECK(MKDIR("save") == 0 && MKDIR("save/save_1") == 0 && MKDIR("save/save_1/0") == 0);
    write_file("save/save_1/0/game_data.sav", &b);
    ach_start();
    SLEEP_S(5);  // (first look, second look, read)
    CHECK(ach_unlocked_total() >= 9 && is_unlocked("shrine_4"));
    unlink("save/save_1/0/game_data.sav");  // (a new game: the game deletes its saves)
    SLEEP_S(3);
    ach_stop();
    CHECK(ach_unlocked_total() == 0);
    f = fopen("achievements.txt", "r");
    CHECK(f != NULL);
    if (f) fclose(f);

    free(b.d); free(empty.d); free(more.d); free(old.d);
    printf(fails ? "%d FAILED\n" : "all achievement tests passed\n", fails);
    return fails ? 1 : 0;
}
