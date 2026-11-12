// make test-update-core: the plain logic of SW Update (runtime/update_core.c), with made-up releases and file lists.
#include "../runtime/update_core.h"
#include "../runtime/update_crypto.h"
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
    CHECK(uc_pick_release(newest_first, "0.5.0-alpha.5", "-windows.zip", &r, &unsig) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.5.2-alpha.5"));
    CHECK(!strcmp(r.tag, "v0.5.2-alpha.5"));
    CHECK(!strcmp(r.zip_name, "SavageWind-0.5.2-alpha.5-windows.zip"));
    CHECK(!strcmp(r.zip_url, "https://example.test/0.5.2-alpha.5.zip"));
    CHECK(!strcmp(r.sig_url, "https://example.test/0.5.2-alpha.5.sig"));
    CHECK(!strcmp(r.digest, H64));
    CHECK(r.zip_size == 1000);
    CHECK(unsig == 0);
    CHECK(uc_pick_release(newest_first, "0.5.2-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_CURRENT);
    CHECK(uc_pick_release(newest_first, "9.0.0", "-windows.zip", &r, NULL) == UC_PICK_CURRENT);

    static const char oldest_first[] =
        "[" RELEASE("v0.5.1-alpha.5", "false", ZIP("0.5.1-alpha.5") "," SIG("0.5.1-alpha.5")) ","
            RELEASE("v0.6.0-alpha.6", "false", ZIP("0.6.0-alpha.6") "," SIG("0.6.0-alpha.6")) "]";
    CHECK(uc_pick_release(oldest_first, "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.6.0-alpha.6"));

    static const char passed_over[] =
        "[" RELEASE("v0.9.0-alpha.9", "true", ZIP("0.9.0-alpha.9") "," SIG("0.9.0-alpha.9")) ","
            RELEASE("nightly", "false", ZIP("nightly") "," SIG("nightly")) ","
            RELEASE("v0.8.0-alpha.8", "false", ASSET("Savage_Wind-0.8.0-alpha.8-x86_64.AppImage", "https://example.test/a", 5)) ","
            RELEASE("v0.7.0-alpha.7", "false", ZIP("0.7.0-alpha.7")) ","
            RELEASE("v0.5.1-alpha.5", "false", ZIP("0.5.1-alpha.5") "," SIG("0.5.1-alpha.5")) "]";
    CHECK(uc_pick_release(passed_over, "0.5.0-alpha.5", "-windows.zip", &r, &unsig) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.version, "0.5.1-alpha.5"));
    CHECK(unsig == 1);

    CHECK(uc_pick_release("[]", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_CURRENT);
    CHECK(uc_pick_release("", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("<html>Rate limited</html>", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("{\"message\":\"Not Found\"}", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("[{\"tag_name\":\"v1.0.0\"", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
    CHECK(uc_pick_release("[{\"tag_name\":\"v1.0.0\",}]", "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
    static char deep[4096];
    memset(deep, '[', sizeof deep - 1);
    CHECK(uc_pick_release(deep, "0.5.0-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_BAD_JSON);
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

// A signature made with openssl (tools/sign_update.py) over "savage wind test vector"
#define TV_PUB "a5c179244b1d44abe6005b0157ad01948b76ac78f462cdced1c3187ea51dbd349a791b854a6451c83de7953786eed112b439eea9a4ea9d08ee377223427ddcb8"
#define TV_SIG "60a93460ab42565d2c131c39a7adc90496444140547c4f20a4762ae6d5f04018f6e26e87201d17a9b5d1f03da0d57c2662f655643234162d6f2b48cf542259d0"
#define TV_HASH "d0be736eda79f98fa648fa79ac66b75f9273008f10d91ca17df977480165502e"

static void crypto(void) {
    uint8_t h[32], want[32], sig[64], pub[64], t[64];
    UcSha256 c;
    uc_sha256_init(&c);
    uc_sha256_update(&c, "abc", 3);
    uc_sha256_final(&c, h);
    uc_hex_decode("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    uc_sha256_init(&c);
    uc_sha256_update(&c, "savage wind test vector", 23);
    uc_sha256_final(&c, h);
    uc_hex_decode(TV_HASH, want, 32);
    CHECK(!memcmp(h, want, 32));

    CHECK(uc_hex_decode(TV_SIG, sig, 64) && uc_hex_decode(TV_PUB, pub, 64));
    CHECK(uc_p256_verify(want, sig, pub));
    uint8_t bad[32];
    memcpy(bad, want, 32);
    bad[5] ^= 1;
    CHECK(!uc_p256_verify(bad, sig, pub));
    memcpy(t, sig, 64);
    t[3] ^= 1;
    CHECK(!uc_p256_verify(want, t, pub));
    memcpy(t, sig, 64);
    t[60] ^= 1;
    CHECK(!uc_p256_verify(want, t, pub));
    memcpy(t, pub, 64);
    t[10] ^= 1;  // (not on the curve)
    CHECK(!uc_p256_verify(want, sig, t));
    memset(t, 0, 64);
    CHECK(!uc_p256_verify(want, t, pub));  // r = s = 0
    memset(t, 0xFF, 64);
    CHECK(!uc_p256_verify(want, t, pub));  // r, s above the order
}

static void appimage_assets(void) {
    UcRelease r;
    static const char list[] =
        "[" RELEASE("v0.5.2-alpha.5", "false",
            ASSET("Savage_Wind-0.5.2-alpha.5-x86_64.AppImage", "https://example.test/x", 7) ","
            ASSET("Savage_Wind-0.5.2-alpha.5-x86_64.AppImage.sig", "https://example.test/x.sig", 1) ","
            ASSET("Savage_Wind-0.5.2-alpha.5-aarch64.AppImage", "https://example.test/a", 9) ","
            ASSET("Savage_Wind-0.5.2-alpha.5-aarch64.AppImage.sig", "https://example.test/a.sig", 1) ","
            ZIP("0.5.2-alpha.5")) "]";
    CHECK(uc_pick_release(list, "0.5.1-alpha.5", "-x86_64.AppImage", &r, NULL) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.zip_url, "https://example.test/x") && !strcmp(r.sig_url, "https://example.test/x.sig") && r.zip_size == 7);
    CHECK(uc_pick_release(list, "0.5.1-alpha.5", "-aarch64.AppImage", &r, NULL) == UC_PICK_UPDATE);
    CHECK(!strcmp(r.zip_url, "https://example.test/a") && !strcmp(r.sig_url, "https://example.test/a.sig") && r.zip_size == 9);
    CHECK(uc_pick_release(list, "0.5.1-alpha.5", "-windows.zip", &r, NULL) == UC_PICK_CURRENT);  // (no signature for the zip)
}

static void inside(void) {
    CHECK(uc_path_inside("/tmp/.mount_SavageAbc/usr/bin/savagewind", "/tmp/.mount_SavageAbc"));
    CHECK(uc_path_inside("/tmp/.mount_SavageAbc/usr/bin/savagewind", "/tmp/.mount_SavageAbc/"));
    CHECK(!uc_path_inside("/tmp/.mount_SavageAbcd/usr/bin/savagewind", "/tmp/.mount_SavageAbc"));  // (a longer name)
    CHECK(!uc_path_inside("/home/p/src/SavageWindRecomp/build/savagewind", "/tmp/.mount_QuiverX"));  // inherited
    CHECK(!uc_path_inside("/tmp/.mount_SavageAbc", "/tmp/.mount_SavageAbc"));
    CHECK(!uc_path_inside("/usr/bin/savagewind", ""));
}

int main(void) {
    versions();
    inside();
    crypto();
    appimage_assets();
    pick();
    paths();
    manifests();
    printf(fails ? "update core: %d FAILED\n" : "update core: all passed\n", fails);
    return fails != 0;
}
