// Test program for the save importer (runtime/services/save_convert.c), with no SDL and no game files. Built and driven
// by tools/test_save_convert.py (make test-save-convert), which compares its output with tools/botw_save_convert.py.
//
//   test_save_convert convert --src <folder> --bootup <Bootup.pack> --dst <new folder> [--allow-switch]
//       sc_format_load + sc_convert into <new folder>/save_0, then one line per converted file, as the Python tool
//       reports it; exit status 0 when it converted, 1 when it refused (the reason follows RESULT)
//   test_save_convert slots <save_0 folder>
//       sc_list_slots: SLOT <name> <saved (unix time)> <play seconds or -1> <has picture>
//   test_save_convert holdlock <save folder> / locked <save folder>
//       a second process holding the game's lock on the save folder, and one asking whether it is held
//   test_save_convert selftest <scratch folder>
//       the safety of backup, install and discard in folders made under <scratch folder> (left for the caller to
//       remove): the player's saves are never lost
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_realloc_at, realloc_calls;
static void *test_realloc(void *p, size_t n) {
    if (++realloc_calls == fail_realloc_at) { errno = ENOMEM; return NULL; }
    return realloc(p, n);
}
#define realloc test_realloc
#include "../runtime/services/save_convert.c"  // (the whole file: the test reaches its helpers)
#undef realloc

static int convert_cmd(int argc, char **argv) {
    const char *src = NULL, *bootup = NULL, *dst = NULL;
    unsigned flags = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--src") && i + 1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--bootup") && i + 1 < argc) bootup = argv[++i];
        else if (!strcmp(argv[i], "--dst") && i + 1 < argc) dst = argv[++i];
        else if (!strcmp(argv[i], "--allow-switch")) flags |= SC_ALLOW_SWITCH;
    }
    if (!src || !bootup || !dst) {
        fprintf(stderr, "usage: test_save_convert convert --src <folder> --bootup <Bootup.pack> --dst <new folder>\n");
        return 2;
    }
    ScReport r = {0};
    ScFormat *fmt = NULL;
    ScStage *stage = NULL;
    if (sc_format_load(bootup, &fmt, &r) != SC_OK || sc_convert(src, dst, fmt, flags, &stage, &r) != SC_OK) {
        printf("RESULT error %d %s\n", r.code, r.detail);
        sc_format_free(fmt);
        sc_report_free(&r);
        return 1;
    }
    printf("ROOT %s\n", r.root);
    for (int i = 0; i < r.nfiles; i++) {
        const ScFlagFile *f = &r.files[i];
        printf("FILE %s 0x%X 0x%X %u %u %u %u\n", f->file, f->src_rev, f->dst_rev, f->kept, f->defaulted, f->dropped, f->resized);
    }
    printf("COUNTS slots=%d tracker=%d album=%d pict=%d written=%d\n", r.nslots, r.n_tracker, r.n_album, r.n_pict, r.n_written);
    printf("RESULT ok\n");
    sc_format_free(fmt);
    sc_report_free(&r);
    sc_stage_keep(stage);  // (the converted folder stays: the caller compares it)
    return 0;
}

// Fail each reallocation in turn, including after files have already been written.
static int oom_cmd(const char *src, const char *bootup, const char *dst) {
    for (int at = 1; ; at++) {
        realloc_calls = 0, fail_realloc_at = at;
        ScReport r = {0};
        ScFormat *fmt = NULL;
        ScStage *st = NULL;
        ScCode c = sc_format_load(bootup, &fmt, &r);
        if (c == SC_OK) c = sc_convert(src, dst, fmt, 0, &st, &r);
        bool reached = realloc_calls >= at;
        fail_realloc_at = 0;
        if (c != SC_OK && (st || exists(dst))) {
            fprintf(stderr, "allocation %d left a stage behind\n", at);
            return 1;
        }
        sc_stage_discard(st);
        sc_format_free(fmt);
        sc_report_free(&r);
        if (!reached) { printf("PASS %d allocation failures\n", at - 1); return 0; }
    }
}

typedef struct { int checks, after; } CancelAfter;
static bool cancel_after(void *p) {
    CancelAfter *c = p;
    return ++c->checks >= c->after;
}

static int cancel_cmd(const char *src, const char *bootup, const char *dst) {
    ScReport r = {0};
    ScFormat *fmt = NULL;
    if (sc_format_load(bootup, &fmt, &r) != SC_OK) return 1;
    for (int at = 1; ; at++) {
        CancelAfter state = {0, at};
        ScCancel cancel = {cancel_after, &state};
        ScStage *st = NULL;
        ScCode c = sc_convert_cancel(src, dst, fmt, 0, &cancel, &st, &r);
        if ((c != SC_OK && c != SC_CANCELLED) || (c == SC_CANCELLED && (st || exists(dst)))) return 1;
        sc_stage_discard(st);
        sc_report_free(&r);
        if (c == SC_OK) break;
    }
    sc_format_free(fmt);
    puts("PASS cancellation removes the stage");
    return 0;
}

// test_save_convert slots <save_0 folder>: one line per slot
static int slots_cmd(const char *dir) {
    ScSlot *v;
    int n = sc_list_slots(dir, &v);
    for (int i = 0; i < n; i++)
        printf("SLOT %s %lld %lld %d\n", v[i].name, (long long)v[i].saved, (long long)v[i].play_seconds, v[i].has_picture);
    free(v);
    return 0;
}

// ---------------------------------------------------------------- selftest
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put(const char *dir, const char *rel, const char *text) {
    char p[1100], *s;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (s = p + 1; (s = strchr(s, '/')); *s++ = '/') {
        *s = 0;
        mkdir(p, 0755);
    }
    FILE *f = fopen(p, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char *slurp(const char *dir, const char *rel) {
    char p[1100];
    static char buf[256];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static bool is_text(const char *dir, const char *rel, const char *want) {
    char *got = slurp(dir, rel);
    return got && !strcmp(got, want);
}

static bool has(const char *dir, const char *rel) {
    char p[1100];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    return stat(p, &st) == 0;
}

// An old save folder (save_0 with a slot, an SD card folder) and a staged one, in a scratch folder
static void setup(const char *root, bool with_old) {
    if (with_old) {
        put(root, "save/save_0/0/game_data.sav", "OLD");
        put(root, "save/save_0/tracker/t0", "OLDTRACK");
        put(root, "save/sdcard/note.txt", "SD");
    }
}

// A staged folder as sc_convert leaves it, but made by hand: <name>/save_0/0/game_data.sav = "NEW", with the list of
// what was made
static ScStage *fake_stage(const char *root, const char *name) {
    (void)root;
    put(name, "save_0/0/game_data.sav", "NEW");
    ScStage *s = calloc(1, sizeof *s);
    snprintf(s->dir, sizeof s->dir, "%s", name);
    static const char *const rel[] = {"", "/save_0", "/save_0/0", "/save_0/0/game_data.sav"};
    for (int i = 0; i < 4; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s%s", name, rel[i]);
        made_add(&s->made, p, i < 3);
    }
    return s;
}

static char *scratch(const char *base, char *out, size_t cap) {
    static int counter;
    snprintf(out, cap, "%s/t%d", base, ++counter);
    mkdir(base, 0755);
    assert(mkdir(out, 0755) == 0);
    return out;
}

static void selftest(const char *base) {
    char root[1100], root2[1100], root3[1100], root4[1100];
    scratch(base, root, sizeof root);
    char save[1100], backup[1100], stage[1100];
    snprintf(save, sizeof save, "%s/save", root);

    // 1. install over an existing save: the old folder is moved whole to the backup, the new one takes its place,
    // and what the import doesn't replace (the SD card folder) is still there
    setup(root, true);
    sc_backup_path(save, backup, sizeof backup);
    CHECK(strstr(backup, "/save-backups/save-") != NULL);
    sc_stage_path(save, stage, sizeof stage);
    CHECK(strstr(stage, "/save-import-") != NULL);
    ScStage *st = fake_stage(root, stage);
    ScReport r = {0};
    CHECK(sc_stage_install(st, save, backup, &r) == SC_OK);
    CHECK(is_text(save, "save_0/0/game_data.sav", "NEW"));
    CHECK(!has(save, "save_0/tracker/t0"));
    CHECK(is_text(save, "sdcard/note.txt", "SD"));
    CHECK(is_text(backup, "save_0/0/game_data.sav", "OLD"));
    CHECK(is_text(backup, "save_0/tracker/t0", "OLDTRACK"));
    CHECK(is_text(backup, "sdcard/note.txt", "SD"));
    CHECK(!has(stage, ""));
    sc_report_free(&r);

    // 2. the staged folder is gone when the swap happens: the second step fails and the first is undone
    scratch(base, root2, sizeof root2);
    snprintf(save, sizeof save, "%s/save", root2);
    setup(root2, true);
    sc_backup_path(save, backup, sizeof backup);
    sc_stage_path(save, stage, sizeof stage);
    st = fake_stage(root2, stage);
    char gone[1100];
    snprintf(gone, sizeof gone, "%s/gone", root2);
    rename(stage, gone);  // (the stage's folder is no longer there)
    memset(&r, 0, sizeof r);
    CHECK(sc_stage_install(st, save, backup, &r) == SC_IO);
    CHECK(is_text(save, "save_0/0/game_data.sav", "OLD"));
    CHECK(is_text(save, "sdcard/note.txt", "SD"));
    CHECK(!has(backup, ""));
    CHECK(!has(root2, "save-backups"));
    // A retry must keep a backup folder that was already there, and everything in it.
    put(root2, "save-backups/keep.txt", "KEEP");
    CHECK(sc_stage_install(st, save, backup, &r) == SC_IO);
    CHECK(is_text(root2, "save-backups/keep.txt", "KEEP"));
    sc_stage_discard(st);
    sc_report_free(&r);

    // 3. no old save: nothing to back up, the staged folder just becomes the save folder
    scratch(base, root3, sizeof root3);
    snprintf(save, sizeof save, "%s/save", root3);
    sc_backup_path(save, backup, sizeof backup);
    sc_stage_path(save, stage, sizeof stage);
    st = fake_stage(root3, stage);
    memset(&r, 0, sizeof r);
    CHECK(sc_stage_install(st, save, backup, &r) == SC_OK);
    CHECK(is_text(save, "save_0/0/game_data.sav", "NEW"));
    CHECK(!has(backup, ""));
    sc_report_free(&r);

    // 4. a backup copy: the save stays, the copy is the same; a backup that can't be made leaves nothing
    memset(&r, 0, sizeof r);
    sc_backup_path(save, backup, sizeof backup);
    {   // (an old file time on the original)
        char a1[1100];
        struct utimbuf old = {1500000000, 1500000000};
        snprintf(a1, sizeof a1, "%s/save_0/0/game_data.sav", save);
        utime(a1, &old);
    }
    CHECK(sc_backup(save, backup, &r) == SC_OK && r.n_written == 1);
    CHECK(is_text(backup, "save_0/0/game_data.sav", "NEW") && is_text(save, "save_0/0/game_data.sav", "NEW"));
    {   // (the file time of a copy is the original's)
        char a1[1100], b1[1100];
        struct stat sa, sb;
        snprintf(a1, sizeof a1, "%s/save_0/0/game_data.sav", save);
        snprintf(b1, sizeof b1, "%s/save_0/0/game_data.sav", backup);
        CHECK(stat(a1, &sa) == 0 && stat(b1, &sb) == 0 && sa.st_mtime == sb.st_mtime && sb.st_mtime == 1500000000);
    }
    CHECK(sc_backup(save, backup, &r) == SC_IO);  // (exists already: untouched)
    CHECK(is_text(backup, "save_0/0/game_data.sav", "NEW"));
    char nosave[1100], blocked[1100];
    snprintf(nosave, sizeof nosave, "%s/none", root3);
    CHECK(sc_backup(nosave, backup, &r) == SC_NO_SAVE);
    put(root3, "blocker", "x");  // a file where the backup's parent folder should be
    snprintf(blocked, sizeof blocked, "%s/blocker/inside", root3);
    CHECK(sc_backup(save, blocked, &r) == SC_IO);
    CHECK(!has(root3, "blocker/inside"));
    sc_report_free(&r);

    // 5. a backup that fails halfway removes what it copied, and only that
    {
        scratch(base, root4, sizeof root4);
        char s4[1100], b4[1100], deep[1100];
        snprintf(s4, sizeof s4, "%s/save", root4);
        snprintf(b4, sizeof b4, "%s/b", root4);
        put(root4, "save/a/one", "1");
        put(root4, "save/b/two", "2");
        snprintf(deep, sizeof deep, "%s/save/b/two", root4);
        memset(&r, 0, sizeof r);
#ifndef _WIN32
        chmod(deep, 0);  // unreadable: the copy of it fails (not for root, which reads anything)
        ScCode c = sc_backup(s4, b4, &r);
        chmod(deep, 0644);
        if (geteuid() != 0) {
            CHECK(c == SC_IO);
            CHECK(!has(b4, ""));
        }
#else
        (void)deep;
#endif
        CHECK(is_text(s4, "a/one", "1"));
        sc_report_free(&r);
    }

    // 6. discarding a stage removes what the conversion made, not what somebody put there since
    st = fake_stage(root3, stage);
    put(stage, "save_0/0/mine.txt", "MINE");
    sc_stage_discard(st);
    CHECK(!has(stage, "save_0/0/game_data.sav"));
    CHECK(is_text(stage, "save_0/0/mine.txt", "MINE"));

    // 7. the profile folder is found below the one picked
    char prof[1100];
    put(root3, "cemu/mlc01/usr/save/00050000/101c9500/user/80000001/0/game_data.sav", "x");
    put(root3, "cemu/mlc01/usr/save/00050000/101c9500/user/common/readme", "x");
    snprintf(nosave, sizeof nosave, "%s/cemu", root3);
    CHECK(sc_find_profile(nosave, prof, sizeof prof) == SC_OK && strstr(prof, "/user/80000001") != NULL);
    snprintf(nosave, sizeof nosave, "%s/cemu/mlc01/usr/save/00050000/101c9500/user/common", root3);
    CHECK(sc_find_profile(nosave, prof, sizeof prof) == SC_NO_SAVE);

    // An expired search has its own result, including when the chosen folder has no save.
    CHECK(find_profile_root(nosave, prof, sizeof prof, time(NULL) - PROFILE_SECONDS - 1, NULL) == SC_SEARCH_LIMIT);
    Search over = {0};
    over.t0 = time(NULL), over.visits = PROFILE_VISITS + 1;
    find_profile(nosave, 0, &over);
    CHECK(search_over(&over) && !over.have_any);
    for (int depth = 9; depth <= 10; depth++) {
        scratch(base, root, sizeof root);
        char deep[1100];
        snprintf(deep, sizeof deep, "%s", root);
        for (int i = 0; i < depth; i++) strcat(deep, "/d");
        put(deep, "0/game_data.sav", "x");
        CHECK(sc_find_profile(root, prof, sizeof prof) == SC_OK && !strcmp(prof, deep));
    }

    // a folder holding a Switch profile first in name order and a Wii U one after it: the Wii U one
    {
        static const char sw[] = {0x30, 0x47, 0, 0}, wiiu[] = {0, 0, 0x47, 0x1B};
        char f[1100];
        put(root3, "both/a/0/game_data.sav", "");
        snprintf(f, sizeof f, "%s/both/a/0/game_data.sav", root3);
        FILE *fp = fopen(f, "wb");
        assert(fp);
        fwrite(sw, 1, 4, fp), fclose(fp);
        put(root3, "both/b/0/game_data.sav", "");
        snprintf(f, sizeof f, "%s/both/b/0/game_data.sav", root3);
        fp = fopen(f, "wb");
        assert(fp);
        fwrite(wiiu, 1, 4, fp), fclose(fp);
        snprintf(nosave, sizeof nosave, "%s/both", root3);
        CHECK(sc_find_profile(nosave, prof, sizeof prof) == SC_OK && strstr(prof, "/both/b") != NULL);
        remove(f);
        CHECK(sc_find_profile(nosave, prof, sizeof prof) == SC_OK && strstr(prof, "/both/a") != NULL);
    }

    // Cleanup removes only dated import folders, keeps a live stage, and never touches unrelated folders.
    {
        scratch(base, root, sizeof root);
        setup(root, true);
        snprintf(save, sizeof save, "%s/save", root);
        const char *old = "save-import-20261029-100000", *live = "save-import-20261030-100000";
        put(root, "save-import-20261029-100000/save_0/0/game_data.sav", "STALE");
        put(root, "save-import-20261030-100000/save_0/0/game_data.sav", "LIVE");
        put(root, "save-import-not-ours/mine", "KEEP");
        put(root, "other/mine", "KEEP");
        snprintf(stage, sizeof stage, "%s/%s/", root, live);
        ScRecovery recovery;
        CHECK(sc_cleanup_stages(save, stage, &recovery, &r) == SC_OK);
        CHECK(!recovery.imported[0] && !has(root, old) && has(root, live));
        CHECK(is_text(root, "save-import-not-ours/mine", "KEEP") && is_text(root, "other/mine", "KEEP"));
        CHECK(is_text(save, "save_0/0/game_data.sav", "OLD"));

        // A missing save means an interrupted swap: keep every import, and name the newest dated backup.
        snprintf(gone, sizeof gone, "%s/gone", root);
        CHECK(rename(save, gone) == 0);
        CHECK(sc_cleanup_stages(save, NULL, &recovery, &r) == SC_OK);
        CHECK(recovery.imported[0] && !recovery.backup[0] && has(root, live));
        put(root, "save-import-20261029-100000/save_0/0/game_data.sav", "STALE");
        put(root, "save-backups/save-20261029-100000/save_0/0/game_data.sav", "OLDER");
        put(root, "save-backups/save-20261030-100000-9/save_0/0/game_data.sav", "OLD");
        put(root, "save-backups/save-20261030-100000-10/save_0/0/game_data.sav", "NEWER");
        CHECK(sc_cleanup_stages(save, NULL, &recovery, &r) == SC_OK);
        CHECK(has(root, old) && has(root, live) && !has(save, ""));
        CHECK(strstr(recovery.imported, live) && strstr(recovery.backup, "save-20261030-100000-10"));
        sc_report_free(&r);
#ifndef _WIN32
        // A link that looks like an abandoned stage never exposes its target to the cleanup.
        CHECK(rename(gone, save) == 0);
        snprintf(stage, sizeof stage, "%s/save-import-20261028-100000", root);
        CHECK(symlink(gone, stage) == 0);
        CHECK(sc_cleanup_stages(save, NULL, &recovery, &r) == SC_OK);
        struct stat link_st;
        CHECK(lstat(stage, &link_st) == 0 && S_ISLNK(link_st.st_mode));
        CHECK(is_text(save, "save_0/0/game_data.sav", "OLD"));
        sc_report_free(&r);
#endif
    }

    // 8. slots
    put(root3, "ls/2/game_data.sav", "x");
    put(root3, "ls/10/caption.sav", "x");
    put(root3, "ls/10/caption.jpg", "x");
    put(root3, "ls/1/other.txt", "x");  // (not a slot)
    put(root3, "ls/word/game_data.sav", "x");
    snprintf(nosave, sizeof nosave, "%s/ls", root3);
    ScSlot *slots;
    int n = sc_list_slots(nosave, &slots);
    CHECK(n == 2 && slots[0].number == 2 && slots[1].number == 10 && slots[1].has_picture && !slots[0].has_picture);
    CHECK(slots[1].saved > 0 && slots[1].play_seconds == -1);
    free(slots);
    snprintf(nosave, sizeof nosave, "%s/not-there", root3);
    CHECK(sc_list_slots(nosave, &slots) == 0);
    free(slots);
#ifndef _WIN32
    // 9. a save folder that is a link is not moved (that would move the link, not the saves)
    {
        char real[1100], link[1100];
        put(root3, "real/save_0/0/game_data.sav", "OLD");
        snprintf(real, sizeof real, "%s/real", root3);
        snprintf(link, sizeof link, "%s/linked", root3);
        CHECK(symlink(real, link) == 0);
        sc_backup_path(link, backup, sizeof backup);
        sc_stage_path(link, stage, sizeof stage);
        st = fake_stage(root3, stage);
        memset(&r, 0, sizeof r);
        CHECK(sc_stage_install(st, link, backup, &r) == SC_LINKED);
        CHECK(is_text(real, "save_0/0/game_data.sav", "OLD") && !has(backup, ""));
        sc_stage_discard(st);
        sc_report_free(&r);
    }
#endif
    // 10. a running game's lock: import and backup refuse and change nothing; once the lock is let go they work, and a lock
    // file that is there but not held (a game that has ended) is no obstacle
    {
        char root5[1100];
        scratch(base, root5, sizeof root5);
        setup(root5, true);
        char save5[1100], backup5[1100], stage5[1100];
        snprintf(save5, sizeof save5, "%s/save", root5);
        sc_backup_path(save5, backup5, sizeof backup5);
        sc_stage_path(save5, stage5, sizeof stage5);
        ScLock *game = NULL, *second = NULL;
        CHECK(sc_lock_take(save5, &game) == SC_LOCK_OK && game);
        CHECK(has(root5, "save.lock") && !has(save5, ".lock"));  // (next to the folder: the swap never moves it)
        CHECK(sc_lock_take(save5, &second) == SC_LOCK_BUSY && !second);
        memset(&r, 0, sizeof r);
        CHECK(sc_backup(save5, backup5, &r) == SC_IN_USE && !has(backup5, ""));
        st = fake_stage(root5, stage5);
        memset(&r, 0, sizeof r);
        CHECK(sc_stage_install(st, save5, backup5, &r) == SC_IN_USE);
        CHECK(is_text(save5, "save_0/0/game_data.sav", "OLD") && has(stage5, "save_0") && !has(backup5, ""));
        sc_lock_drop(game);
        memset(&r, 0, sizeof r);
        CHECK(sc_backup(save5, backup5, &r) == SC_OK && is_text(backup5, "save_0/0/game_data.sav", "OLD"));
        sc_report_free(&r);
        char backup5b[1100];
        snprintf(backup5b, sizeof backup5b, "%s/backup-2", root5);
        memset(&r, 0, sizeof r);
        CHECK(sc_stage_install(st, save5, backup5b, &r) == SC_OK && is_text(save5, "save_0/0/game_data.sav", "NEW"));
        sc_report_free(&r);
        CHECK(sc_lock_take(save5, &second) == SC_LOCK_OK);  // (the import let go of it again)
        sc_lock_drop(second);
        // a folder the lock file can't be made in: nothing to wait for
        char nowhere[1100];
        snprintf(nowhere, sizeof nowhere, "%s/no-such-folder/save", root5);
        CHECK(sc_lock_take(nowhere, &second) == SC_LOCK_UNAVAILABLE && !second);
    }
    printf(failures ? "SELFTEST FAIL (%d)\n" : "SELFTEST PASS\n", failures);
}

// holdlock <save folder>: holds the game's lock until the process is stopped (tests: a second process as the game)
static int holdlock_cmd(const char *save) {
    ScLock *l = NULL;
    if (sc_lock_take(save, &l) != SC_LOCK_OK) { puts("NOLOCK"); return 1; }
    puts("READY");
    fflush(stdout);
    for (;;) sleep(60);
}

// locked <save folder>: BUSY, FREE (and let go again) or UNAVAILABLE
static int locked_cmd(const char *save) {
    ScLock *l = NULL;
    ScLockResult c = sc_lock_take(save, &l);
    puts(c == SC_LOCK_BUSY ? "BUSY" : c == SC_LOCK_OK ? "FREE" : "UNAVAILABLE");
    sc_lock_drop(l);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "convert")) return convert_cmd(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "cancel")) return cancel_cmd(argv[2], argv[3], argv[4]);
    if (argc == 5 && !strcmp(argv[1], "oom")) return oom_cmd(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "holdlock")) return holdlock_cmd(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "locked")) return locked_cmd(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "slots")) return slots_cmd(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "selftest")) {
        selftest(argv[2]);
        return failures != 0;
    }
    fprintf(stderr, "usage: test_save_convert convert ... | slots <folder> | selftest <scratch folder>\n");
    return 2;
}
