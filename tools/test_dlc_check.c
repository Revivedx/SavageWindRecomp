// Synthetic downloadable-content validator tests; no game files needed. make test-dlc-check
#include "dlc_check.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned g_checks;
#define CHECK(x) do { assert(x); ++g_checks; } while (0)

static const char *const ALL_DLC1_DIRS[] = {
    "Game", "Map", "Movie", "NavMesh", "Pack", "Physics", "Sound", "System", "Terrain", "UI", "Voice",
};

static char g_root[2048];

static void mk_dir_abs(const char *p) {
#ifdef _WIN32
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0700);
#endif
}

static void mk_dir(const char *rel) {
    char tmp[4096];
    assert(snprintf(tmp, sizeof tmp, "%s/%s", g_root, rel) < (int)sizeof tmp);
    for (char *s = tmp + strlen(g_root) + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mk_dir_abs(tmp);
            *s = '/';
        }
    }
    mk_dir_abs(tmp);
}

static void write_bytes(const char *rel, const void *data, size_t len) {
    char p[4096], dir[4096];
    assert(snprintf(p, sizeof p, "%s/%s", g_root, rel) < (int)sizeof p);
    strcpy(dir, rel);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mk_dir(dir);
    }
    FILE *f = fopen(p, "wb");
    assert(f != NULL);
    if (len) assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static void write_str(const char *rel, const char *s) {
    write_bytes(rel, s, strlen(s));
}

static void rm_tree_abs(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        assert(unlink(p) == 0);
        return;
    }
    DIR *d = opendir(p);
    assert(d != NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096];
        assert(snprintf(sub, sizeof sub, "%s/%s", p, e->d_name) < (int)sizeof sub);
        rm_tree_abs(sub);
    }
    closedir(d);
    assert(rmdir(p) == 0);
}

static void reset_root(void) {
    DIR *d = opendir(g_root);
    assert(d != NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096];
        assert(snprintf(sub, sizeof sub, "%s/%s", g_root, e->d_name) < (int)sizeof sub);
        rm_tree_abs(sub);
    }
    closedir(d);
}

int main(void) {
    CHECK(rt_dlc_check(NULL).state == RT_DLC_NONE);
    CHECK(rt_dlc_check("").state == RT_DLC_NONE);
    CHECK(rt_dlc_verify(NULL).state == RT_DLC_NONE);

#ifdef _WIN32
    char temp[1024];
    assert(GetTempPathA(sizeof temp, temp) > 0);
    snprintf(g_root, sizeof g_root, "%ssw-dlc-%lu-%llu", temp, (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    for (char *c = g_root; *c; c++)
        if (*c == '\\') *c = '/';
    mk_dir_abs(g_root);
#else
    strcpy(g_root, "/tmp/sw-dlc-XXXXXX");
    assert(mkdtemp(g_root) != NULL);
#endif

    // 1. Base game only: no DLC, no warning.
    write_str("exefs/main", "NSO0");
    write_str("romfs/System/Version.txt", "1.6.0\n");
    RtDlcCheck c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_NONE && c.present_mask == 0);
    CHECK(rt_dlc_verify(g_root).state == RT_DLC_NONE);

    // 2. DLC merged into romfs/ (#188, #242).
    write_str("romfs/System/AocVersion.txt", "3.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_MERGED && c.present_mask == 0);
    reset_root();
    write_str("romfs/Pack/AocMainField.pack", "SARC");
    CHECK(rt_dlc_check(g_root).state == RT_DLC_MERGED);

    // 3. Misnamed DLC folder in root (#383), while ignoring save/ and cache/.
    reset_root();
    write_str("save/System/AocVersion.txt", "3.0\n");
    write_str("cache/System/AocVersion.txt", "3.0\n");
    CHECK(rt_dlc_check(g_root).state == RT_DLC_NONE);
    write_str("dlc1/System/AocVersion.txt", "3.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_MISNAMED && !strcmp(c.detail, "dlc1"));
    reset_root();
    write_str("dlc%s/romfs/System/AocVersion.txt", "3.0\n");  // nested + '%' sanitized
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_MISNAMED && !strcmp(c.detail, "dlc?s"));

    // 4. Only one of the two DLC folders (#470).
    reset_root();
    write_str("aoc1/System/AocVersion.txt", "3.0\n");
    write_str("aoc1/Pack/AocMainField.pack", "SARC");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_ONLY_ONE && c.only_pack == 1 && c.present_mask == 1u && !strcmp(c.version, "3.0"));
    reset_root();
    write_str("aoc2/System/AocVersion.txt", "1.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_ONLY_ONE && c.only_pack == 2 && c.present_mask == 2u);

    // 5. Nested RomFS inside aoc1 (e.g. aoc1/romfs/System/AocVersion.txt) and base game mistaken for DLC.
    reset_root();
    write_str("aoc1/romfs/System/AocVersion.txt", "3.0\n");
    write_str("aoc2/System/AocVersion.txt", "1.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_INCOMPLETE && c.nested && !strcmp(c.detail, "aoc1/System/AocVersion.txt"));
    reset_root();
    write_str("aoc1/System/AocVersion.txt", "3.0\n");
    write_str("aoc1/System/Version.txt", "1.6.0\n");
    write_str("aoc2/System/AocVersion.txt", "1.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_INCOMPLETE && !c.nested);

    // 6. Swapped aoc1 and aoc2 (data pack in aoc2).
    reset_root();
    write_str("aoc1/System/AocVersion.txt", "1.0\n");
    write_str("aoc2/System/AocVersion.txt", "3.0\n");
    write_str("aoc2/Pack/AocMainField.pack", "SARC");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_SWAPPED && c.present_mask == 3u);

    // 7. Wrong version: older Switch DLC 2.0 (lacking Movie/) and Wii U "0300".
    reset_root();
    write_str("aoc1/System/AocVersion.txt", "2.0\r\n");
    write_str("aoc1/Pack/AocMainField.pack", "SARC");
    write_str("aoc2/System/AocVersion.txt", "1.0\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_WRONG_VERSION && !strcmp(c.version, "2.0"));
    write_str("aoc1/System/AocVersion.txt", "0300\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_WRONG_VERSION && !strcmp(c.version, "0300"));
    write_str("aoc1/System/AocVersion.txt", "2.%s\n");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_WRONG_VERSION && !strcmp(c.version, "2.?s"));

    // 8. Incomplete 3.0 tree: missing folder, empty pack, empty Movie/.
    reset_root();
    write_str("aoc1/System/AocVersion.txt", "\xEF\xBB\xBF" "3.0\r\n");
    write_str("aoc2/System/AocVersion.txt", "1.0\n");
    for (size_t i = 0; i + 1 < sizeof ALL_DLC1_DIRS / sizeof ALL_DLC1_DIRS[0]; i++) {
        char d[64];
        snprintf(d, sizeof d, "aoc1/%s", ALL_DLC1_DIRS[i]);
        mk_dir(d);
    }
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_INCOMPLETE && !strcmp(c.detail, "aoc1/Voice"));
    mk_dir("aoc1/Voice");
    write_bytes("aoc1/Pack/AocMainField.pack", "", 0);
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_INCOMPLETE && !strcmp(c.detail, "aoc1/Pack/AocMainField.pack"));
    write_str("aoc1/Pack/AocMainField.pack", "SARC-DATA");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_INCOMPLETE && !strcmp(c.detail, "aoc1/Movie"));

    // 9. Complete valid DLC 3.0: both fast check and deep RomFS layout + read verification pass.
    write_str("aoc1/Movie/Demo655_0.mp4", "MP4-VIDEO");
    c = rt_dlc_check(g_root);
    CHECK(c.state == RT_DLC_READY && c.present_mask == 3u && !strcmp(c.version, "3.0"));
    c = rt_dlc_verify(g_root);
    CHECK(c.state == RT_DLC_READY && c.files == 4 && c.bytes == 8 + 4 + 9 + 9 && !strcmp(c.version, "3.0"));
    CHECK(!strcmp(rt_dlc_state_name(c.state), "ready"));

    rm_tree_abs(g_root);
    printf("test_dlc_check: %u checks passed\n", g_checks);
    return 0;
}
