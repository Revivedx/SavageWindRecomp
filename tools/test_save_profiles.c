// Test program for the save profiles (runtime/services/save_profiles.c): names, listing, create / copy / rename /
// delete, the active profile, the legacy `save` folder, and the failure paths. No SDL and no game files; the profiles
// are tiny fake folders made under a temporary directory (never a real save). Built and run by make test-save-profiles.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <dirent.h>

#ifdef _WIN32
#include "../runtime/platform/win_compat.h"  // (setenv, unsetenv, mkdir with a mode)
#include <windows.h>
#include <direct.h>
#include <io.h>
#endif
#include "../runtime/services/save_profiles.h"
#include "../runtime/services/save_convert.h"

static int checks;
// A directory link: a symlink here, a junction on Windows (needs no privilege there)
static int make_link(const char *target, const char *link) {
#ifdef _WIN32
    char cmd[2400], t[1100], l[1100];
    snprintf(t, sizeof t, "%s", target), snprintf(l, sizeof l, "%s", link);
    for (char *c = t; *c; c++) if (*c == '/') *c = '\\';
    for (char *c = l; *c; c++) if (*c == '/') *c = '\\';
    snprintf(cmd, sizeof cmd, "cmd /c mklink /J \"%s\" \"%s\" >nul", l, t);
    return system(cmd) == 0 ? 0 : -1;
#else
    return symlink(target, link);
#endif
}

static void drop_link(const char *link) {
#ifdef _WIN32
    RemoveDirectoryA(link);
#else
    unlink(link);
#endif
}

static int mk_tmp_root(char *out, size_t cap) {
#ifdef _WIN32
    const char *t = getenv("TEMP");
    snprintf(out, cap, "%s/sp-test-%d", t ? t : ".", (int)getpid());
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
    return mkdir(out, 0755) == 0 || errno == EEXIST ? 0 : -1;
#else
    snprintf(out, cap, "/tmp/sp-test-XXXXXX");
    return mkdtemp(out) ? 0 : -1;
#endif
}

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "%s:%d: check failed: %s (%s)\n", __FILE__, __LINE__, #c, sp_detail()); exit(1); } } while (0)

static char root[512];

static void mk(const char *rel) {  // mkdir -p of root/rel
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *q = p + strlen(root) + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char *get(const char *rel) {  // contents, or NULL
    static char buf[256];
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static bool there(const char *rel) {
    char p[1100];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return stat(p, &st) == 0;
}

// A fake slot: only what sc_list_slots looks for (a numbered folder with a game_data.sav) and a marker with the profile's name
static void slot(const char *save_rel, int n, const char *marker) {
    char rel[256];
    snprintf(rel, sizeof rel, "%s/save_0/%d", save_rel, n);
    mk(rel);
    strcat(rel, "/game_data.sav");
    put(rel, marker);
}

static bool listed(const char *name) {
    SpProfile *v;
    int n = sp_list(root, &v);
    bool found = false;
    for (int i = 0; i < n; i++) found |= !strcmp(v[i].name, name);
    free(v);
    return found;
}

static void rmrf(const char *p) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd)) {}
}

static uint64_t tiny_disk(const char *dir) { (void)dir; return 1000; }
static uint64_t huge_disk(const char *dir) { (void)dir; return UINT64_MAX / 2; }

static void test_names(void) {
    CHECK(sp_validate_name("100%") == SP_NAME_CHARS);
    CHECK(sp_validate_name("100 percent") == SP_OK);
    CHECK(sp_validate_name("My save (2)") == SP_OK);
    CHECK(sp_validate_name("a-b_c.d") == SP_OK);
    CHECK(sp_validate_name("") == SP_NAME_EMPTY);
    CHECK(sp_validate_name(NULL) == SP_NAME_EMPTY);
    CHECK(sp_validate_name("   ") == SP_NAME_EMPTY);
    CHECK(sp_validate_name(" lead") == SP_NAME_CHARS);
    CHECK(sp_validate_name("trail ") == SP_NAME_CHARS);
    CHECK(sp_validate_name(".hidden") == SP_NAME_CHARS);
    CHECK(sp_validate_name("dot.") == SP_NAME_CHARS);
    CHECK(sp_validate_name("..") == SP_NAME_CHARS);
    CHECK(sp_validate_name(".") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a..b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("../etc") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a/b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a\\b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("C:") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a:b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a*b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a?b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a\"b") == SP_NAME_CHARS);
    CHECK(sp_validate_name("a<b>c|d") == SP_NAME_CHARS);
    CHECK(sp_validate_name("tab\there") == SP_NAME_CHARS);
    CHECK(sp_validate_name("new\nline") == SP_NAME_CHARS);
    CHECK(sp_validate_name("caf\xc3\xa9") == SP_NAME_CHARS);  // (non-ASCII: the Windows ANSI API can't be trusted with it)
    CHECK(sp_validate_name("Default") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("DEFAULT") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("nul") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("CON") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("com1") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("LPT9") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("nul.txt") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("Aux.save") == SP_NAME_RESERVED);
    CHECK(sp_validate_name("com0") == SP_OK);
    CHECK(sp_validate_name("com10") == SP_OK);
    CHECK(sp_validate_name("console") == SP_OK);
    char lng[64];
    memset(lng, 'a', sizeof lng);
    lng[SP_NAME_MAX] = 0;
    CHECK(sp_validate_name(lng) == SP_OK);
    lng[SP_NAME_MAX] = 'a';
    lng[SP_NAME_MAX + 1] = 0;
    CHECK(sp_validate_name(lng) == SP_NAME_LONG);
    // nothing invalid gets a path
    char out[1100];
    CHECK(!sp_save_dir(root, "../x", out, sizeof out));
    CHECK(!sp_save_dir(root, "a/b", out, sizeof out));
    CHECK(sp_save_dir(NULL, "Default", out, sizeof out) && !strcmp(out, "save"));
    CHECK(sp_save_dir(NULL, "Mine", out, sizeof out) && !strcmp(out, "save-profiles/Mine/save"));
}

static void test_legacy(void) {
    // an install that has never heard of profiles: only save/
    slot("save", 0, "legacy-0");
    put("save/save_0/option.sav", "opt");
    SpProfile *v;
    int n = sp_list(root, &v);
    CHECK(n == 1 && v[0].is_default && v[0].active && !strcmp(v[0].name, "Default") && v[0].slots == 1 && v[0].saved > 0);
    free(v);
    unsetenv("SAVAGEWIND_SAVE_DIR");
    char out[1100], want[1100];
    sp_active_save_dir(root, out, sizeof out);
    snprintf(want, sizeof want, "%s/save", root);
    CHECK(!strcmp(out, want));
    char name[64];
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "Default"));
    CHECK(!there("save_profile.txt") && !there("save-profiles"));  // (looking creates nothing)
    CHECK(!strcmp(get("save/save_0/0/game_data.sav"), "legacy-0"));
    // without any save folder at all
    sp_active_save_dir("/nonexistent-root", out, sizeof out);
    CHECK(!strcmp(out, "/nonexistent-root/save"));
}

static void test_override(void) {
    char out[1100];
    CHECK(sp_create(root, "Over") == SP_OK);
    CHECK(sp_set_active(root, "Over") == SP_OK);
    setenv("SAVAGEWIND_SAVE_DIR", "/tmp/scratch-save", 1);
    sp_active_save_dir(root, out, sizeof out);
    CHECK(!strcmp(out, "/tmp/scratch-save") && sp_override_active());
    sp_session_begin();
    CHECK(!strcmp(sp_session_save_dir(), "/tmp/scratch-save"));
    unsetenv("SAVAGEWIND_SAVE_DIR");
    CHECK(!sp_override_active());
    CHECK(sp_set_active(root, "Default") == SP_OK);
    CHECK(sp_delete(root, "Over") == SP_OK);
}

static void test_create_switch(void) {
    CHECK(sp_create(root, "Full 100") == SP_OK);
    CHECK(there("save-profiles/Full 100/save"));
    CHECK(sp_create(root, "full 100") == SP_EXISTS);          // case-insensitive duplicate
    CHECK(sp_create(root, "FULL 100") == SP_EXISTS);
    CHECK(sp_create(root, "Default") == SP_NAME_RESERVED);
    CHECK(sp_create(root, "../evil") == SP_NAME_CHARS);
    CHECK(sp_create(root, "") == SP_NAME_EMPTY);
    CHECK(!there("evil") && !there("../evil"));
    CHECK(sp_create(root, "alpha") == SP_OK);
    CHECK(sp_create(root, "Beta") == SP_OK);
    SpProfile *v;
    int n = sp_list(root, &v);
    CHECK(n == 4);
    CHECK(!strcmp(v[0].name, "Default") && !strcmp(v[1].name, "alpha") && !strcmp(v[2].name, "Beta") && !strcmp(v[3].name, "Full 100"));
    CHECK(v[0].active && !v[1].active && v[1].slots == 0);
    free(v);

    CHECK(sp_set_active(root, "full 100") == SP_OK);           // (looked up without regard to case, stored as it is spelled)
    char name[64], out[1100], want[1100];
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "Full 100"));
    CHECK(!strcmp(get("save_profile.txt"), "Full 100\n"));
    unsetenv("SAVAGEWIND_SAVE_DIR");
    sp_active_save_dir(root, out, sizeof out);
    snprintf(want, sizeof want, "%s/save-profiles/Full 100/save", root);
    CHECK(!strcmp(out, want));
    CHECK(sp_set_active(root, "nope") == SP_NOT_FOUND);
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "Full 100"));
    // a garbled or stale file falls back to Default
    put("save_profile.txt", "../../etc\n");
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "Default"));
    put("save_profile.txt", "gone\n");
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "Default"));
    put("save_profile.txt", "alpha\r\n");
    sp_active(root, name, sizeof name);
    CHECK(!strcmp(name, "alpha"));
    CHECK(sp_set_active(root, "Default") == SP_OK);
    // stray things in save-profiles are not profiles
    mk("save-profiles/.tmp-x");
    mk("save-profiles/bad:name");
    put("save-profiles/afile", "x");
    n = sp_list(root, &v);
    CHECK(n == 4);
    free(v);
    rmrf((snprintf(out, sizeof out, "%s/save-profiles/bad:name", root), out));
    rmrf((snprintf(out, sizeof out, "%s/save-profiles/.tmp-x", root), out));
    rmrf((snprintf(out, sizeof out, "%s/save-profiles/afile", root), out));
}

static void test_copy(void) {
    // the "100% save" scenario: a profile with its own slots, copied from the player's own
    slot("save-profiles/Full 100/save", 0, "full-0");
    slot("save-profiles/Full 100/save", 1, "full-1");
    put("save-profiles/Full 100/save/save_0/option.sav", "full-opt");
    sp_free_space_hook = huge_disk;
    uint64_t bytes = 0;
    CHECK(sp_copy(root, "Full 100", "Dev copy", &bytes) == SP_OK);
    CHECK(bytes > 0);
    CHECK(!strcmp(get("save-profiles/Dev copy/save/save_0/1/game_data.sav"), "full-1"));
    CHECK(!strcmp(get("save-profiles/Dev copy/save/save_0/option.sav"), "full-opt"));
    // independent from here on: a change in the copy does not reach the original
    put("save-profiles/Dev copy/save/save_0/1/game_data.sav", "changed");
    CHECK(!strcmp(get("save-profiles/Full 100/save/save_0/1/game_data.sav"), "full-1"));
    // no temp folder is left
    DIR *d;
    char p[1100];
    snprintf(p, sizeof p, "%s/save-profiles", root);
    d = opendir(p);
    struct dirent *e;
    while ((e = readdir(d))) CHECK(strncmp(e->d_name, ".tmp-", 5) != 0 && strncmp(e->d_name, ".deleting-", 10) != 0);
    closedir(d);

    // copying the legacy Default profile; it stays untouched
    CHECK(sp_copy(root, "Default", "Mine", NULL) == SP_OK);
    CHECK(!strcmp(get("save-profiles/Mine/save/save_0/0/game_data.sav"), "legacy-0"));
    CHECK(!strcmp(get("save/save_0/0/game_data.sav"), "legacy-0"));
    CHECK(sp_copy(root, "", "Mine2", NULL) == SP_OK);

    // refusals
    CHECK(sp_copy(root, "Full 100", "dev COPY", NULL) == SP_EXISTS);
    CHECK(sp_copy(root, "Full 100", "Default", NULL) == SP_NAME_RESERVED);
    CHECK(sp_copy(root, "Full 100", "a/b", NULL) == SP_NAME_CHARS);
    CHECK(sp_copy(root, "missing", "Other", NULL) == SP_NOT_FOUND);
    CHECK(!there("save-profiles/Other"));

    // low disk space: fails cleanly before copying anything
    sp_free_space_hook = tiny_disk;
    CHECK(sp_copy(root, "Full 100", "TooBig", NULL) == SP_NO_SPACE);
    CHECK(!there("save-profiles/TooBig"));
    snprintf(p, sizeof p, "%s/save-profiles", root);
    d = opendir(p);
    while ((e = readdir(d))) CHECK(strncmp(e->d_name, ".tmp-", 5) != 0);
    closedir(d);
    sp_free_space_hook = huge_disk;

    // a source in use by a running game (its lock held): refused, nothing left behind
    ScLock *lock = NULL;
    snprintf(p, sizeof p, "%s/save-profiles/Full 100/save", root);
    CHECK(sc_lock_take(p, &lock) == SC_LOCK_OK);
    CHECK(sp_copy(root, "Full 100", "Locked", NULL) == SP_IN_USE);  // (flock is per open file description: a second open conflicts)
    CHECK(!there("save-profiles/Locked"));
    CHECK(sp_set_active(root, "Full 100") == SP_IN_USE);
    CHECK(sp_rename(root, "Full 100", "Other name") == SP_IN_USE);
    CHECK(sp_delete(root, "Full 100") == SP_IN_USE);
    CHECK(there("save-profiles/Full 100/save/save_0/0/game_data.sav"));
    sc_lock_drop(lock);
    CHECK(sp_copy(root, "Full 100", "Locked", NULL) == SP_OK);
    CHECK(sp_delete(root, "Locked") == SP_OK);

    // a broken copy (a file that can't be read) leaves no half profile
    snprintf(p, sizeof p, "%s/save-profiles/Full 100/save/save_0/0/game_data.sav", root);
#ifdef _WIN32
    if (0) {  // (no chmod 000 there)
#else
    if (geteuid() != 0) {
#endif
        chmod(p, 0000);
        CHECK(sp_copy(root, "Full 100", "Broken", NULL) == SP_IO);
        chmod(p, 0644);
        CHECK(!there("save-profiles/Broken"));
        snprintf(p, sizeof p, "%s/save-profiles", root);
        d = opendir(p);
        while ((e = readdir(d))) CHECK(strncmp(e->d_name, ".tmp-", 5) != 0);
        closedir(d);
    }
}

static void test_rename_delete(void) {
    CHECK(sp_set_active(root, "Mine") == SP_OK);
    CHECK(sp_rename(root, "Mine", "Renamed") == SP_IS_ACTIVE);
    CHECK(sp_delete(root, "Mine") == SP_IS_ACTIVE);
    CHECK(sp_delete(root, "mine") == SP_IS_ACTIVE);          // (the name's case doesn't get around it)
    CHECK(sp_rename(root, "Default", "X") == SP_IS_DEFAULT);
    CHECK(sp_delete(root, "Default") == SP_IS_DEFAULT);
    CHECK(sp_delete(root, "") == SP_IS_DEFAULT);
    CHECK(there("save/save_0/0/game_data.sav"));
    CHECK(sp_delete(root, "nope") == SP_NOT_FOUND);
    CHECK(sp_rename(root, "nope", "x") == SP_NOT_FOUND);

    CHECK(sp_rename(root, "Dev copy", "Dev renamed") == SP_OK);
    CHECK(!there("save-profiles/Dev copy") && there("save-profiles/Dev renamed/save/save_0/0/game_data.sav"));
    CHECK(sp_rename(root, "Dev renamed", "mine") == SP_EXISTS);   // (taken: it is the active one, in another case)
    CHECK(sp_rename(root, "Dev renamed", "Full 100") == SP_EXISTS);
    CHECK(sp_rename(root, "Dev renamed", "full 100") == SP_EXISTS);
    CHECK(sp_rename(root, "Dev renamed", "../x") == SP_NAME_CHARS);
    CHECK(sp_rename(root, "Dev renamed", "Default") == SP_NAME_RESERVED);
    CHECK(sp_rename(root, "Dev renamed", "") == SP_NAME_EMPTY);
    CHECK(there("save-profiles/Dev renamed"));
    CHECK(sp_rename(root, "Dev renamed", "DEV RENAMED") == SP_OK);  // (only the letters' case)
    CHECK(listed("DEV RENAMED") && !listed("Dev renamed"));

    // delete: the others are never touched
    CHECK(sp_delete(root, "dev renamed") == SP_OK);
    CHECK(!listed("DEV RENAMED") && !there("save-profiles/DEV RENAMED"));
    CHECK(there("save-profiles/Full 100/save/save_0/1/game_data.sav"));
    CHECK(there("save-profiles/Mine/save/save_0/0/game_data.sav"));
    CHECK(there("save/save_0/0/game_data.sav"));
    DIR *d;
    char p[1100];
    snprintf(p, sizeof p, "%s/save-profiles", root);
    d = opendir(p);
    struct dirent *e;
    while ((e = readdir(d))) CHECK(strncmp(e->d_name, ".deleting-", 10) != 0);
    closedir(d);

    // a profile that is a link is not deleted (what it points to is not ours to remove)
    char target[1100], link[1100];
    snprintf(target, sizeof target, "%s/elsewhere", root);
    mkdir(target, 0755);
    put("elsewhere/precious", "keep");
    snprintf(link, sizeof link, "%s/save-profiles/Linked", root);
    CHECK(make_link(target, link) == 0);
    CHECK(listed("Linked"));
    CHECK(sp_delete(root, "Linked") == SP_LINKED);
    CHECK(!strcmp(get("elsewhere/precious"), "keep"));
    drop_link(link);

    // a link INSIDE a profile is removed as a link, never followed
    CHECK(sp_create(root, "WithLink") == SP_OK);
    snprintf(link, sizeof link, "%s/save-profiles/WithLink/save/out", root);
    CHECK(make_link(target, link) == 0);
    CHECK(sp_delete(root, "WithLink") == SP_OK);
    CHECK(!strcmp(get("elsewhere/precious"), "keep"));

    // switching back and forth never touches data
    CHECK(sp_set_active(root, "Full 100") == SP_OK);
    CHECK(sp_set_active(root, "Default") == SP_OK);
    CHECK(sp_delete(root, "Mine") == SP_OK);
    CHECK(sp_delete(root, "Mine2") == SP_OK);
}

static void test_cleanup(void) {
    mk("save-profiles/.deleting-a/save");
    put("save-profiles/.deleting-a/save/f", "x");
    mk("save-profiles/.tmp-old/save");
    mk("save-profiles/.tmp-new/save");
    char p[1100];
    snprintf(p, sizeof p, "%s/save-profiles/.tmp-old", root);
#ifdef _WIN32
    {  // (utime can't open a folder here)
        HANDLE h = CreateFileA(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        FILETIME ft = {0, 0x01B21DD2};  // (about 1970; any time long ago)
        CHECK(h != INVALID_HANDLE_VALUE && SetFileTime(h, NULL, NULL, &ft));
        CloseHandle(h);
    }
#else
    struct utimbuf old = {1, 1};  // (1970)
    utime(p, &old);
#endif
    sp_cleanup(root);
    CHECK(!there("save-profiles/.deleting-a"));
    CHECK(!there("save-profiles/.tmp-old"));
    CHECK(there("save-profiles/.tmp-new"));  // (could be a copy running right now in another Savage Wind)
    CHECK(there("save-profiles/Full 100/save/save_0/0/game_data.sav"));
}

int main(void) {
    char tmpl[1100];
    if (mk_tmp_root(tmpl, sizeof tmpl)) { perror("temp folder"); return 1; }
    snprintf(root, sizeof root, "%s", tmpl);
    unsetenv("SAVAGEWIND_SAVE_DIR");
    test_names();
    test_legacy();
    test_override();
    test_create_switch();
    test_copy();
    test_rename_delete();
    test_cleanup();
    rmrf(root);
    printf("save profiles: %d checks passed\n", checks);
    return 0;
}
