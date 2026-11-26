#include "mods.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond) do {                                                        \
    g_checks++;                                                                 \
    if (!(cond)) {                                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        g_fails++;                                                              \
    }                                                                           \
} while (0)

static void set_env_var(const char *name, const char *val) {
#ifdef _WIN32
    _putenv_s(name, val ? val : "");
#else
    if (val) setenv(name, val, 1);
    else unsetenv(name);
#endif
}

static void mk_dir_one(const char *p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

static void mk_dirs(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            mk_dir_one(buf);
            *p = saved;
        }
    }
    mk_dir_one(buf);
}

static void rm_rf(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return;
    if (!S_ISDIR(sb.st_mode)) {
        remove(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_rf(sub);
        }
        closedir(d);
    }
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

static void write_text(const char *root, const char *rel, const char *text) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    char *slash = strrchr(full, '/');
    if (slash) {
        *slash = '\0';
        mk_dirs(full);
        *slash = '/';
    }
    FILE *f = fopen(full, "wb");
    if (!f) {
        fprintf(stderr, "FATAL: cannot write %s\n", full);
        exit(2);
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void make_tmp_root(char *out, size_t cap, const char *tag) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(out, cap, "%s/sw_mods_test_%s_%d", tmp, tag, (int)getpid());
    for (char *p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    rm_rf(out);
    mk_dirs(out);
}

static int find_idx(const ModList *list, const char *id) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].id, id) == 0) return i;
    }
    return -1;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

static void test_ignored_top_files(void) {
    CHECK(mods_is_ignored_top_file("README"));
    CHECK(mods_is_ignored_top_file("README.txt"));
    CHECK(mods_is_ignored_top_file("readme_notes.txt"));
    CHECK(mods_is_ignored_top_file("info.json"));
    CHECK(mods_is_ignored_top_file("META.JSON"));
    CHECK(mods_is_ignored_top_file("Rules.txt"));
    CHECK(mods_is_ignored_top_file("LICENSE"));
    CHECK(mods_is_ignored_top_file("license.txt"));
    CHECK(mods_is_ignored_top_file("changelog.MD"));
    CHECK(mods_is_ignored_top_file("patch.py"));
    CHECK(mods_is_ignored_top_file("install.BAT"));
    CHECK(mods_is_ignored_top_file("setup.sh"));
    CHECK(mods_is_ignored_top_file("apply.ps1"));
    CHECK(mods_is_ignored_top_file(".DS_Store"));
    CHECK(mods_is_ignored_top_file("Thumbs.db"));

    CHECK(!mods_is_ignored_top_file("ActorInfo.product.sbyml"));
    CHECK(!mods_is_ignored_top_file("Font_EU.sbfarc"));
    CHECK(!mods_is_ignored_top_file("TitleBG.pack"));
    CHECK(!mods_is_ignored_top_file("movie.mp4"));
    CHECK(!mods_is_ignored_top_file(NULL));
    CHECK(!mods_is_ignored_top_file(""));
}

static void test_empty_and_hidden_discovery(void) {
    char root[512];
    make_tmp_root(root, sizeof root, "empty");

    ModList list;
    CHECK(mods_scan(root, &list, true));
    CHECK(list.count == 0);
    CHECK(list.enabled_count == 0);
    CHECK(list.total_conflicts == 0);
    mods_list_free(&list);

    char mods_dir[600];
    snprintf(mods_dir, sizeof mods_dir, "%s/mods", root);
    mk_dirs(mods_dir);
    write_text(root, "mods/not_a_dir.txt", "ignored file\n");
    write_text(root, "mods/.hidden_mod/Actor/foo.bin", "hidden");

    CHECK(mods_scan(root, &list, true));
    CHECK(list.count == 0);
    mods_list_free(&list);

    rm_rf(root);
}

static void test_layouts_metadata_and_file_counts(void) {
    char root[512];
    make_tmp_root(root, sizeof root, "meta");

    // 1) Container layout (RomFS/ subdir) + UTF-8 JSON + nested object ignored
    write_text(root, "mods/beta_container/info.json",
               "\xEF\xBB\xBF{\n"
               "  \"dependencies\": { \"name\": \"WRONG_NESTED\" },\n"
               "  \"name\": \"Caf\\u00e9 Textures\",\n"
               "  \"version\": \"1.2.0\",\n"
               "  \"author\": \"Modder\\\"One\\\"\",\n"
               "  \"description\": \"Line 1\\r\\nLine 2\"\n"
               "}\n");
    write_text(root, "mods/beta_container/RomFS/Font/Font_EU.sbfarc", "FONT_DATA");
    write_text(root, "mods/beta_container/RomFS/System/Version.txt", "1.9");
    write_text(root, "mods/beta_container/README.txt", "Should not overwrite info.json description\r\n");

    // 2) Direct / flat layout + README.txt fallback with >400 chars and CRLF + ignored top docs
    char long_readme[700];
    memset(long_readme, 'A', 450);
    long_readme[10] = '\r';
    long_readme[11] = '\n';
    long_readme[450] = '\0';
    write_text(root, "mods/alpha_flat/readme.txt", long_readme);
    write_text(root, "mods/alpha_flat/info.json", "{ \"version\": \"0.5\" }\n");
    write_text(root, "mods/alpha_flat/install.ps1", "echo ignored\n");
    write_text(root, "mods/alpha_flat/rules.txt", "[Definition]\n");
    write_text(root, "mods/alpha_flat/Actor/Link.sbactorpack", "LINK");
    write_text(root, "mods/alpha_flat/SubDir/README.txt", "INCLUDED");

    // 3) Container layout (romfs/) with meta.json inside host_dir and "desc"
    write_text(root, "mods/gamma_romfs/romfs/meta.json",
               "{ \"desc\": \"Short desc via meta\" }\n");
    write_text(root, "mods/gamma_romfs/romfs/Event/Intro.sbeventpack", "EVT");

    ModList list;
    CHECK(mods_scan(root, &list, false));
    CHECK(list.count == 3);
    CHECK(list.enabled_count == 0);

    // Newly discovered mods appear alphabetically: alpha_flat, beta_container, gamma_romfs
    CHECK(strcmp(list.items[0].id, "alpha_flat") == 0);
    CHECK(strcmp(list.items[1].id, "beta_container") == 0);
    CHECK(strcmp(list.items[2].id, "gamma_romfs") == 0);

    // Alpha flat checks
    CHECK(!list.items[0].uses_romfs_subdir);
    CHECK(strcmp(list.items[0].rel_dir, "mods/alpha_flat") == 0);
    CHECK(ends_with(list.items[0].host_dir, "/mods/alpha_flat"));
    CHECK(strcmp(list.items[0].name, "alpha_flat") == 0);  // fallback name = id
    CHECK(strcmp(list.items[0].version, "0.5") == 0);
    CHECK(strchr(list.items[0].description, '\r') == NULL);
    CHECK(strncmp(list.items[0].description, "AAAAAAAAAA\nAAAA", 15) == 0);
    CHECK(strlen(list.items[0].description) <= 400);
    CHECK(ends_with(list.items[0].description, "..."));
    CHECK(list.items[0].file_count == 2);  // Actor/Link.sbactorpack + SubDir/README.txt
    CHECK(list.items[0].total_bytes == 4u + 8u);

    // Beta container checks
    CHECK(list.items[1].uses_romfs_subdir);
    CHECK(strcmp(list.items[1].rel_dir, "mods/beta_container/RomFS") == 0);
    CHECK(ends_with(list.items[1].host_dir, "/mods/beta_container/RomFS"));
    CHECK(strcmp(list.items[1].name, "Caf\xC3\xA9 Textures") == 0);
    CHECK(strcmp(list.items[1].version, "1.2.0") == 0);
    CHECK(strcmp(list.items[1].author, "Modder\"One\"") == 0);
    CHECK(strcmp(list.items[1].description, "Line 1\nLine 2") == 0);
    CHECK(list.items[1].file_count == 2);
    CHECK(list.items[1].total_bytes == 9u + 3u);

    // Gamma container checks (meta.json inside romfs/)
    CHECK(list.items[2].uses_romfs_subdir);
    CHECK(strcmp(list.items[2].rel_dir, "mods/gamma_romfs/romfs") == 0);
    CHECK(strcmp(list.items[2].description, "Short desc via meta") == 0);
    CHECK(list.items[2].file_count == 2);  // meta.json inside romfs/ + Intro.sbeventpack (not a flat root)

    mods_list_free(&list);
    rm_rf(root);
}

static void test_persistence_reconcile_mutators_and_conflicts(void) {
    char root[512];
    make_tmp_root(root, sizeof root, "order");

    write_text(root, "mods/mod_a/info.json", "{ \"name\": \"Mod Alpha\" }\n");
    write_text(root, "mods/mod_a/Actor/Common.sbactorpack", "A_COMMON");
    write_text(root, "mods/mod_a/Actor/OnlyA.sbactorpack", "ONLY_A");

    write_text(root, "mods/mod_b/info.json", "{ \"name\": \"Mod Beta\" }\n");
    write_text(root, "mods/mod_b/Actor/Common.sbactorpack", "B_COMMON");
    write_text(root, "mods/mod_b/Pack/Shared.pack", "B_SHARED");

    write_text(root, "mods/mod_c/romfs/info.json", "{ \"name\": \"Mod Gamma\" }\n");
    write_text(root, "mods/mod_c/romfs/Pack/Shared.pack", "C_SHARED");

    write_text(root, "mods/mod_disabled/Actor/Common.sbactorpack", "DIS_COMMON");

    // Seed mods.txt with comments, a deleted mod ID, and reversed order (mod_b #1, mod_a #2, mod_c #3)
    write_text(root, "mods.txt",
               "# user comment\r\n"
               "\r\n"
               "1 mod_b\r\n"
               "0 deleted_mod\r\n"
               "1 mod_a\r\n"
               "1 mod_c\r\n");

    ModList list;
    CHECK(mods_scan(root, &list, true));
    CHECK(list.count == 4);
    CHECK(list.enabled_count == 3);
    CHECK(strcmp(list.items[0].id, "mod_b") == 0 && list.items[0].enabled);
    CHECK(strcmp(list.items[1].id, "mod_a") == 0 && list.items[1].enabled);
    CHECK(strcmp(list.items[2].id, "mod_c") == 0 && list.items[2].enabled);
    CHECK(strcmp(list.items[3].id, "mod_disabled") == 0 && !list.items[3].enabled);

    // Conflicts across enabled mods:
    // - Actor/Common.sbactorpack: mod_b (#1) beats mod_a (#2)
    // - Pack/Shared.pack: mod_b (#1) beats mod_c (#3)
    // mod_disabled does NOT contribute to conflicts.
    CHECK(list.total_conflicts == 2);
    CHECK(list.items[0].wins_conflicts == 2);
    CHECK(list.items[0].loses_conflicts == 0);
    CHECK(list.items[0].sample_count == 2);
    CHECK(strstr(list.items[0].samples[0], "Actor/Common.sbactorpack") != NULL);
    CHECK(strstr(list.items[0].samples[0], "Mod Alpha") != NULL);

    CHECK(list.items[1].wins_conflicts == 0);
    CHECK(list.items[1].loses_conflicts == 1);
    CHECK(list.items[1].sample_count == 1);
    CHECK(strstr(list.items[1].samples[0], "Mod Beta") != NULL);

    CHECK(list.items[2].wins_conflicts == 0);
    CHECK(list.items[2].loses_conflicts == 1);
    CHECK(list.items[2].sample_count == 1);
    CHECK(strstr(list.items[2].samples[0], "Mod Beta") != NULL);

    CHECK(list.items[3].wins_conflicts == 0);
    CHECK(list.items[3].loses_conflicts == 0);
    mods_list_free(&list);

    // Move mod_a higher (-1) -> mod_a becomes #1, mod_b becomes #2
    CHECK(mods_move(root, "mod_a", -1));
    CHECK(!mods_move(root, "no_such_mod", -1));

    // Toggle mod_c off, verify mods.txt.tmp is cleaned up
    CHECK(mods_set_enabled(root, "mod_c", false));
    CHECK(!mods_set_enabled(root, "no_such_mod", true));

    char tmp_check[600];
    snprintf(tmp_check, sizeof tmp_check, "%s/mods.txt.tmp", root);
    struct stat sb;
    CHECK(stat(tmp_check, &sb) != 0);

    CHECK(mods_scan(root, &list, true));
    CHECK(strcmp(list.items[0].id, "mod_a") == 0 && list.items[0].enabled);
    CHECK(strcmp(list.items[1].id, "mod_b") == 0 && list.items[1].enabled);
    CHECK(strcmp(list.items[2].id, "mod_c") == 0 && !list.items[2].enabled);
    CHECK(list.total_conflicts == 1);  // Only Actor/Common.sbactorpack (mod_a wins over mod_b)
    CHECK(list.items[0].wins_conflicts == 1);
    CHECK(list.items[1].loses_conflicts == 1);
    mods_list_free(&list);

    // Enable all / disable all
    CHECK(mods_set_all_enabled(root, true));
    CHECK(mods_scan(root, &list, false));
    CHECK(list.enabled_count == 4);
    mods_list_free(&list);

    CHECK(mods_set_all_enabled(root, false));
    CHECK(mods_scan(root, &list, false));
    CHECK(list.enabled_count == 0);
    mods_list_free(&list);

    // Reorder clamping bounds check
    CHECK(mods_move(root, "mod_disabled", -100));
    CHECK(mods_scan(root, &list, false));
    CHECK(strcmp(list.items[0].id, "mod_disabled") == 0);
    CHECK(find_idx(&list, "mod_a") >= 0);
    mods_list_free(&list);

    rm_rf(root);
}

static void test_session_overlay_and_restart(void) {
    char root[512];
    char env_dir[512];
    make_tmp_root(root, sizeof root, "sess");
    snprintf(env_dir, sizeof env_dir, "%s/env_overlay", root);
    mk_dirs(env_dir);

    write_text(env_dir, "Font/Font_EU.sbfarc", "ENV_FONT");
    write_text(env_dir, "aoc/0010/Actor/DlcActor.sbactorpack", "ENV_DLC");

    write_text(root, "mods/top_mod/info.json", "{ \"name\": \"Top Priority\" }\n");
    write_text(root, "mods/top_mod/Font/Font_EU.sbfarc", "TOP_FONT");
    write_text(root, "mods/top_mod/Actor/Hero.sbactorpack", "TOP_HERO");

    write_text(root, "mods/low_mod/romfs/info.json", "{ \"name\": \"Low Priority\" }\n");
    write_text(root, "mods/low_mod/romfs/Font/Font_EU.sbfarc", "LOW_FONT");
    write_text(root, "mods/low_mod/romfs/Actor/Hero.sbactorpack", "LOW_HERO");
    write_text(root, "mods/low_mod/romfs/Movie/Intro.mp4", "LOW_MOVIE");

    write_text(root, "mods.txt", "1 top_mod\n1 low_mod\n");

    // Part A: pre-session standalone env override fallback (must run BEFORE mods_session_begin!)
    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", NULL);
    CHECK(!mods_env_override(NULL, 0));
    CHECK(mods_active_count() == 0);
    CHECK(mods_active_dir(0) == NULL);

    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", env_dir);
    char env_buf[MOD_PATH_CAP];
    CHECK(mods_env_override(env_buf, sizeof env_buf));
    CHECK(strcmp(env_buf, env_dir) == 0);
    CHECK(mods_active_count() == 1);
    CHECK(strcmp(mods_active_dir(0), env_dir) == 0);
    CHECK(mods_active_name(0) != NULL);

    char found[MOD_PATH_CAP];
    char mod_name[MOD_NAME_CAP];
    CHECK(mods_find_file("", "Font/Font_EU.sbfarc", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(ends_with(found, "/env_overlay/Font/Font_EU.sbfarc"));
    CHECK(mods_find_file("aoc/0010/", "Actor/DlcActor.sbactorpack", found, sizeof found, NULL, 0));
    CHECK(ends_with(found, "/env_overlay/aoc/0010/Actor/DlcActor.sbactorpack"));
    CHECK(strstr(found, "//") == NULL);

    // Part B: session_begin with SAVAGEWIND_ROMFS_OVERLAY set (env beats top_mod beats low_mod)
    mods_session_begin(root);
    CHECK(mods_active_count() == 3);
    CHECK(strcmp(mods_active_dir(0), env_dir) == 0);
    CHECK(ends_with(mods_active_dir(1), "/mods/top_mod"));
    CHECK(ends_with(mods_active_dir(2), "/mods/low_mod/romfs"));
    CHECK(strcmp(mods_active_name(1), "Top Priority") == 0);
    CHECK(strcmp(mods_active_name(2), "Low Priority") == 0);

    CHECK(mods_find_file("", "Font/Font_EU.sbfarc", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(ends_with(found, "/env_overlay/Font/Font_EU.sbfarc"));

    CHECK(mods_find_file("", "Actor/Hero.sbactorpack", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(ends_with(found, "/mods/top_mod/Actor/Hero.sbactorpack"));
    CHECK(strcmp(mod_name, "Top Priority") == 0);

    CHECK(mods_find_file("", "Movie/Intro.mp4", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(ends_with(found, "/mods/low_mod/romfs/Movie/Intro.mp4"));
    CHECK(strcmp(mod_name, "Low Priority") == 0);

    CHECK(!mods_find_file("", "Does/Not/Exist.bin", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(!mods_session_pending_restart(root));

    // Pending restart detection (env override is excluded from saved mod comparison)
    CHECK(mods_set_enabled(root, "low_mod", false));
    CHECK(mods_session_pending_restart(root));

    // Restoring exact saved enabled order clears pending restart
    CHECK(mods_set_enabled(root, "low_mod", true));
    CHECK(!mods_session_pending_restart(root));

    // Reordering enabled mods triggers pending restart even when same set of mods is enabled
    CHECK(mods_move(root, "low_mod", -1));
    CHECK(mods_session_pending_restart(root));

    // Part C: session_begin with env override cleared
    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", NULL);
    mods_session_begin(root);
    CHECK(!mods_session_pending_restart(root));
    CHECK(mods_active_count() == 2);
    CHECK(strcmp(mods_active_name(0), "Low Priority") == 0);
    CHECK(strcmp(mods_active_name(1), "Top Priority") == 0);

    CHECK(mods_find_file("", "Font/Font_EU.sbfarc", found, sizeof found, mod_name, sizeof mod_name));
    CHECK(ends_with(found, "/mods/low_mod/romfs/Font/Font_EU.sbfarc"));
    CHECK(strcmp(mod_name, "Low Priority") == 0);

    rm_rf(root);
}

// What mods_session_begin writes to stderr (savagewind.log), read back from `path`
static char *session_log(const char *root, const char *path) {
    fflush(stderr);
    int keep = dup(2);
    FILE *f = fopen(path, "wb");
    if (keep < 0 || !f) {
        fprintf(stderr, "FATAL: cannot capture stderr in %s\n", path);
        exit(2);
    }
    dup2(fileno(f), 2);
    mods_session_begin(root);
    fflush(stderr);
    dup2(keep, 2);
    close(keep);
    fclose(f);
    f = fopen(path, "rb");
    static char text[16384];
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    remove(path);
    return text;
}

static void test_session_log(void) {
    char root[512], env_dir[512], cap[600], long_line[300], want[400];
    make_tmp_root(root, sizeof root, "log");
    snprintf(env_dir, sizeof env_dir, "%s/env_overlay", root);
    snprintf(cap, sizeof cap, "%s/stderr.txt", root);
    write_text(env_dir, "Pack/TitleBG.pack", "ENV_PACK");
    write_text(env_dir, "Actor/Pack/Hero.sbactorpack", "ENV_HERO");
    write_text(root, "mods/top_mod/info.json", "{ \"name\": \"Top Priority\", \"version\": \"1.2\" }\n");
    write_text(root, "mods/top_mod/Actor/Hero.sbactorpack", "TOP_HERO");
    write_text(root, "mods.txt", "1 top_mod\n");
    char report[400];

    // No mods: one line that says so
    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", NULL);
    CHECK(mods_set_enabled(root, "top_mod", false));
    char *log = session_log(root, cap);
    CHECK(strstr(log, "[mods] none (") != NULL);
    CHECK(mods_report_line(report, sizeof report) > 0);
    CHECK(strcmp(report, "Mods: none\n") == 0);

    // An overlay with no list: its file count and top folders; the mod from mods/ with its version
    CHECK(mods_set_enabled(root, "top_mod", true));
    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", env_dir);
    log = session_log(root, cap);
    CHECK(strstr(log, "[mods] active overlay layers (2), highest priority first:\n") != NULL);
    snprintf(want, sizeof want, "[mods]   #1 SAVAGEWIND_ROMFS_OVERLAY (%s)\n", env_dir);
    CHECK(strstr(log, want) != NULL);
    CHECK(strstr(log, "[mods]     no savagewind-mods.txt (no list of the mods in it): 2 files") != NULL);
    CHECK(strstr(log, "Actor") != NULL && strstr(log, "Pack") != NULL);
    CHECK(strstr(log, "[mods]   #2 Top Priority 1.2 (mods/top_mod: 1 file,") != NULL);
    CHECK(strstr(log, "[mods] 2 mods loaded:") != NULL);
    CHECK(mods_report_line(report, sizeof report) > 0);
    CHECK(strcmp(report, "Mods: 2 overlay layers (SAVAGEWIND_ROMFS_OVERLAY, Top Priority); the mods are listed in "
                         "savagewind.log\n") == 0);

    // The list a mod manager writes: notes and mods copied (CR, control characters and blank lines dropped, a long
    // line cut), only the mods counted
    memset(long_line, 'x', 260);
    long_line[260] = 0;
    char list[600];
    snprintf(list, sizeof list, "# SWMM+ 0.2.0, profile Default\r\nLinkle 1.0.2 (BNP)\r\n\r\n  Ultimate\x01UI 2.0 (Wii U "
             "BNP)\n%s\n", long_line);
    write_text(env_dir, "savagewind-mods.txt", list);
    log = session_log(root, cap);
    CHECK(strstr(log, "[mods]     # SWMM+ 0.2.0, profile Default\n") != NULL);
    CHECK(strstr(log, "[mods]     Linkle 1.0.2 (BNP)\n") != NULL);
    CHECK(strstr(log, "[mods]     Ultimate UI 2.0 (Wii U BNP)\n") != NULL);
    long_line[240] = 0;
    snprintf(want, sizeof want, "[mods]     %s\n", long_line);
    CHECK(strstr(log, want) != NULL);
    CHECK(strstr(log, "no savagewind-mods.txt") == NULL);
    CHECK(strstr(log, "[mods] 4 mods loaded:") != NULL);

    // A short buffer still ends in a line
    CHECK(mods_report_line(report, 24) > 0);
    CHECK(strlen(report) == 23 && ends_with(report, "...\n"));

    set_env_var("SAVAGEWIND_ROMFS_OVERLAY", NULL);
    rm_rf(root);
}

int main(void) {
    test_ignored_top_files();
    test_empty_and_hidden_discovery();
    test_layouts_metadata_and_file_counts();
    test_persistence_reconcile_mutators_and_conflicts();
    test_session_overlay_and_restart();
    test_session_log();

    printf("test_mods: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
