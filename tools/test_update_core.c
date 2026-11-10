// make test-update-core: the plain logic of SW Update (runtime/update_core.c), with made-up releases and file lists.
#include "../runtime/update_core.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); fails++; } } while (0)

#define H64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ASSET(n, u, s) "{\"name\":\"" n "\",\"browser_download_url\":\"" u "\",\"size\":" #s ",\"digest\":\"sha256:" H64 "\"}"
#define RELEASE(tag, draft, assets) \
    "{\"tag_name\":\"" tag "\",\"draft\":" draft ",\"prerelease\":true,\"body\":\"notes \\\"quoted\\\" \\u00e9\"," \
    "\"html_url\":\"https://example.test/" tag "\",\"author\":{\"login\":\"x\",\"n\":[1,2,{\"a\":null}]},\"assets\":[" assets "]}"
#define ZIP(v) ASSET("SavageWind-" v "-windows.zip", "https://example.test/" v ".zip", 1000)
#define SIG(v) ASSET("SavageWind-" v "-windows.zip.sig", "https://example.test/" v ".sig", 128)

static void versions(void) {
    CHECK(uc_version_valid("0.5.1-alpha.5"));
    CHECK(uc_version_valid("v0.5.1"));
    CHECK(uc_version_valid("1.0.0-rc.2"));
    CHECK(!uc_version_valid(""));
    CHECK(!uc_version_valid("0.5"));
    CHECK(!uc_version_valid("0.5.1-"));
    CHECK(!uc_version_valid("0.5.1-alpha"));
    CHECK(!uc_version_valid("0.5.1-gamma.1"));
    CHECK(!uc_version_valid("0.5.1 alpha.5"));
    CHECK(!uc_version_valid("latest"));
    CHECK(uc_version_cmp("0.5.1-alpha.5", "0.5.0-alpha.5") > 0);
    CHECK(uc_version_cmp("v0.5.0-alpha.5", "0.5.0-alpha.5") == 0);
    CHECK(uc_version_cmp("0.4.1-alpha.4", "0.5.0-alpha.5") < 0);
    CHECK(uc_version_cmp("0.10.0-alpha.1", "0.9.0-alpha.9") > 0);
    CHECK(uc_version_cmp("0.5.0-alpha.10", "0.5.0-alpha.9") > 0);
    CHECK(uc_version_cmp("1.0.0", "1.0.0-rc.9") > 0);
    CHECK(uc_version_cmp("1.0.0-beta.1", "1.0.0-alpha.9") > 0);
    CHECK(uc_version_cmp("1.0.0-rc.1", "1.0.0-beta.9") > 0);
    CHECK(uc_version_cmp("junk", "0.0.1") < 0);
}

static void pick(void) {
    UcRelease r;
    int unsig = -1;
    static const char newest_first[] =
        "[" RELEASE("v0.5.2-alpha.5", "false", ZIP("0.5.2-alpha.5") "," SIG("0.5.2-alpha.5")) ","
            RELEASE("v0.5.1-alpha.5", "false", ZIP("0.5.1-alpha.5") "," SIG("0.5.1-alpha.5")) ","
            RELEASE("v0.5.0-alpha.5", "false", ZIP("0.5.0-alpha.5")) "]";
    CHECK(uc_pick_release(newest_first, "0.5.0-alpha.5", &r, &unsig) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.5.2-alpha.5"));
    CHECK(!strcmp(r.tag, "v0.5.2-alpha.5"));
    CHECK(!strcmp(r.zip_name, "SavageWind-0.5.2-alpha.5-windows.zip"));
    CHECK(!strcmp(r.zip_url, "https://example.test/0.5.2-alpha.5.zip"));
    CHECK(!strcmp(r.sig_url, "https://example.test/0.5.2-alpha.5.sig"));
    CHECK(!strcmp(r.digest, H64));
    CHECK(r.zip_size == 1000);
    CHECK(unsig == 0);
    CHECK(uc_pick_release(newest_first, "0.5.2-alpha.5", &r, NULL) == UC_PICK_CURRENT);
    CHECK(uc_pick_release(newest_first, "9.0.0", &r, NULL) == UC_PICK_CURRENT);

    static const char oldest_first[] =
        "[" RELEASE("v0.5.1-alpha.5", "false", ZIP("0.5.1-alpha.5") "," SIG("0.5.1-alpha.5")) ","
            RELEASE("v0.6.0-alpha.6", "false", ZIP("0.6.0-alpha.6") "," SIG("0.6.0-alpha.6")) "]";
    CHECK(uc_pick_release(oldest_first, "0.5.0-alpha.5", &r, NULL) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.6.0-alpha.6"));

    static const char passed_over[] =
        "[" RELEASE("v0.9.0-alpha.9", "true", ZIP("0.9.0-alpha.9") "," SIG("0.9.0-alpha.9")) ","
            RELEASE("nightly", "false", ZIP("nightly") "," SIG("nightly")) ","
            RELEASE("v0.8.0-alpha.8", "false", ASSET("Savage_Wind-0.8.0-alpha.8-x86_64.AppImage", "https://example.test/a", 5)) ","
            RELEASE("v0.7.0-alpha.7", "false", ZIP("0.7.0-alpha.7")) ","
            RELEASE("v0.5.1-alpha.5", "false", ZIP("0.5.1-alpha.5") "," SIG("0.5.1-alpha.5")) "]";
    CHECK(uc_pick_release(passed_over, "0.5.0-alpha.5", &r, &unsig) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.5.1-alpha.5"));
    CHECK(unsig == 1);

    CHECK(uc_pick_release("[]", "0.5.0-alpha.5", &r, NULL) == UC_PICK_CURRENT);
    CHECK(uc_pick_release("", "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("<html>Rate limited</html>", "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("{\"message\":\"Not Found\"}", "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("[{\"tag_name\":\"v1.0.0\"", "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("[{\"tag_name\":\"v1.0.0\",}]", "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
    static char deep[4096];
    memset(deep, '[', sizeof deep - 1);
    CHECK(uc_pick_release(deep, "0.5.0-alpha.5", &r, NULL) == UC_PICK_BAD_JSON);
}

static void paths(void) {
    CHECK(uc_path_safe("savagewind.exe"));
    CHECK(uc_path_safe("zig/lib/std/os.zig"));
    CHECK(uc_path_safe("SW Update.exe"));
    CHECK(uc_path_safe("colour-luts/a.cube"));
    CHECK(uc_path_safe("console.txt"));  // (only the exact name is reserved)
    CHECK(uc_path_safe("com.txt"));
    CHECK(!uc_path_safe(""));
    CHECK(!uc_path_safe("/etc/passwd"));
    CHECK(!uc_path_safe("../evil.exe"));
    CHECK(!uc_path_safe("a/../../evil.exe"));
    CHECK(!uc_path_safe("a/./b"));
    CHECK(!uc_path_safe("a//b"));
    CHECK(!uc_path_safe("a/"));
    CHECK(!uc_path_safe("a\\b"));
    CHECK(!uc_path_safe("..\\evil.exe"));
    CHECK(!uc_path_safe("C:/Windows/evil.exe"));
    CHECK(!uc_path_safe("file.txt:stream"));
    CHECK(!uc_path_safe("NUL"));
    CHECK(!uc_path_safe("nul.txt"));
    CHECK(!uc_path_safe("a/COM1/b"));
    CHECK(!uc_path_safe("lpt9.log"));
    CHECK(!uc_path_safe("trailing."));
    CHECK(!uc_path_safe("trailing "));
    CHECK(!uc_path_safe("what?.exe"));
    CHECK(!strcmp(uc_strip_top("Savage Wind/zig/zig.exe"), "zig/zig.exe"));
    CHECK(!uc_strip_top("Savage Wind/"));
    CHECK(!uc_strip_top("Other/zig.exe"));
    CHECK(!uc_strip_top("savagewind.exe"));
}

static void manifests(void) {
    UcManifest m;
    char why[120];
    static const char good[] =
        "savagewind-update-manifest 1\r\nversion 0.5.1-alpha.5\n"
        "f " H64 " 12 savagewind.exe\nf " H64 " 0 zig/lib/empty file.txt\n";
    CHECK(uc_manifest_parse(good, &m, why, sizeof why));
    CHECK(m.count == 2);
    CHECK(!strcmp(m.version, "0.5.1-alpha.5"));
    CHECK(m.files[0].size == 12);
    CHECK(!strcmp(m.files[1].path, "zig/lib/empty file.txt"));
    CHECK(uc_manifest_find(&m, "SAVAGEWIND.EXE") != NULL);
    CHECK(uc_manifest_find(&m, "savagewind.ex") == NULL);
    CHECK(uc_manifest_find(&m, "zig/lib/empty") == NULL);
    uc_manifest_free(&m);

    static const char *const bad[] = {
        "",
        "hello\n",
        "savagewind-update-manifest 2\nversion 0.5.1\nf " H64 " 1 a\n",
        "savagewind-update-manifest 1\nf " H64 " 1 a\n",
        "savagewind-update-manifest 1\nversion nope\nf " H64 " 1 a\n",
        "savagewind-update-manifest 1\nversion 0.5.1\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf abc 1 a\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf " H64 " x a\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf " H64 " 1\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf " H64 " 1 ../evil.exe\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf " H64 " 1 C:/evil.exe\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nf " H64 " 1 a\nf " H64 " 1 A\n",
        "savagewind-update-manifest 1\nversion 0.5.1\nwhat is this\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        why[0] = 0;
        if (uc_manifest_parse(bad[i], &m, why, sizeof why)) { printf("FAILED: bad manifest %d accepted\n", (int)i); fails++; }
        else CHECK(why[0]);
    }
}

int main(void) {
    versions();
    pick();
    paths();
    manifests();
    printf(fails ? "update core: %d FAILED\n" : "update core: all passed\n", fails);
    return fails != 0;
}
