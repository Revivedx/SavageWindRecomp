// make test-steam-shortcuts: Steam's shortcuts.vdf read and written (runtime/steam_shortcuts.c), on made-up files and,
// given as arguments, on real ones (copies): every entry kept byte for byte, ours added once and then updated in place.
#include "../runtime/steam_shortcuts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static const SteamEntry OURS[2] = {
    {"Savage Wind", "\"/home/u/Games/Savage_Wind.AppImage\"", "\"/home/u/Games/\"", "/home/u/icon.png", "--game"},
    {"Savage Wind Configuration", "\"/home/u/Games/Savage_Wind.AppImage\"", "\"/home/u/Games/\"", "/home/u/icon.png", "--config"},
};
static const SteamEntry MOVED[2] = {
    {"Savage Wind", "\"/mnt/sd/Savage_Wind.AppImage\"", "\"/mnt/sd/\"", "", "--game"},
    {"Savage Wind Configuration", "\"/mnt/sd/Savage_Wind.AppImage\"", "\"/mnt/sd/\"", "", "--config"},
};

static bool has(const uint8_t *b, size_t n, const char *s) {
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(b + i, s, l)) return true;
    return false;
}

static void with_file(const uint8_t *in, size_t n, const char *what) {
    int before = steam_vdf_count(in, n), already = 0;
    CHECK(before >= 0);
    for (int k = 0; k < 2; k++) {  // (ours there already, from an earlier copy: updated, not added)
        char a1[80], a2[80];
        snprintf(a1, sizeof a1, "AppName%c%s%c", 0, OURS[k].name, 0);
        snprintf(a2, sizeof a2, "appname%c%s%c", 0, OURS[k].name, 0);
        size_t l = 9 + strlen(OURS[k].name);
        bool found = false;
        for (size_t i = 0; i + l <= n && !found; i++) found = !memcmp(in + i, a1, l) || !memcmp(in + i, a2, l);
        already += found;
    }
    uint8_t *same = NULL, *a = NULL, *b = NULL, *c = NULL;
    size_t sn = 0, an = 0, bn = 0, cn = 0;
    char why[200];
    // nothing to add: the very same bytes back
    CHECK(steam_vdf_upsert(in, n, OURS, 0, &same, &sn, why, sizeof why));
    if (n) CHECK(sn == n && !memcmp(same, in, n));
    CHECK(steam_vdf_upsert(in, n, OURS, 2, &a, &an, why, sizeof why));
    CHECK(steam_vdf_count(a, an) == before + 2 - already);
    CHECK(has(a, an, "Savage Wind Configuration") && has(a, an, "--config") && has(a, an, "--game"));
    // the original entries are still there, unchanged, at the start
    if (n > 2 && !already) CHECK(an > n && !memcmp(a, in, n - 2));
    // again: nothing added twice
    CHECK(steam_vdf_upsert(a, an, OURS, 2, &b, &bn, why, sizeof why));
    CHECK(bn == an && !memcmp(a, b, an));
    // the AppImage moved: the same two entries point at it
    CHECK(steam_vdf_upsert(b, bn, MOVED, 2, &c, &cn, why, sizeof why));
    CHECK(steam_vdf_count(c, cn) == before + 2 - already);
    CHECK(has(c, cn, "/mnt/sd/Savage_Wind.AppImage") && !has(c, cn, "/home/u/Games/Savage_Wind.AppImage"));
    printf("  %s: %d shortcuts, %d of ours already there, then updated in place\n", what, before, already);
    free(same), free(a), free(b), free(c);
}

int main(int argc, char **argv) {
    with_file(NULL, 0, "no file yet");
    static const uint8_t empty[] = "\0shortcuts\0\x08\x08";
    with_file(empty, sizeof empty - 1, "empty list");

    uint8_t *out = NULL;
    size_t on = 0;
    char why[200] = "";
    static const uint8_t junk[] = "\0shortcuts\0\x01name\0no end";
    CHECK(!steam_vdf_upsert(junk, sizeof junk - 1, OURS, 2, &out, &on, why, sizeof why) && why[0]);
    static const uint8_t unknown[] = "\0shortcuts\0\x09odd\0\x08\x08";
    CHECK(!steam_vdf_upsert(unknown, sizeof unknown - 1, OURS, 2, &out, &on, why, sizeof why));
    static const uint8_t trailing[] = "\0shortcuts\0\x08\x08xx";
    CHECK(!steam_vdf_upsert(trailing, sizeof trailing - 1, OURS, 2, &out, &on, why, sizeof why));
    CHECK(steam_vdf_count(junk, sizeof junk - 1) == -1);

    uint32_t id = steam_shortcut_appid("\"/a\"", "b");
    CHECK(id & 0x80000000u);
    CHECK(id == steam_shortcut_appid("\"/a\"", "b") && id != steam_shortcut_appid("\"/a\"", "c"));

    for (int i = 1; i < argc; i++) {  // real files (copies)
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("FAILED: can't read %s\n", argv[i]); fails++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *b = (uint8_t *)malloc((size_t)n);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) fails++;
        fclose(f);
        with_file(b, (size_t)n, argv[i]);
        free(b);
    }
    printf(fails ? "steam shortcuts: %d FAILED\n" : "steam shortcuts: all passed\n", fails);
    return fails != 0;
}
