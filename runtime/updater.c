// Automatic update checking via GitHub releases API (runtime/updater.h).
#include "updater.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern const char *rt_version(void);

static _Atomic bool g_has_update = false;
static char g_latest_version[64];
static char g_release_url[256];

bool updater_has_update(void) {
    return atomic_load(&g_has_update);
}

const char *updater_latest_version(void) {
    return g_latest_version;
}

const char *updater_release_url(void) {
    return g_release_url;
}

static bool version_is_newer(const char *rem, const char *loc) {
    if (rem[0] == 'v' || rem[0] == 'V') rem++;
    if (loc[0] == 'v' || loc[0] == 'V') loc++;
    int r_maj = 0, r_min = 0, r_pat = 0;
    int l_maj = 0, l_min = 0, l_pat = 0;
    sscanf(rem, "%d.%d.%d", &r_maj, &r_min, &r_pat);
    sscanf(loc, "%d.%d.%d", &l_maj, &l_min, &l_pat);
    if (r_maj != l_maj) return r_maj > l_maj;
    if (r_min != l_min) return r_min > l_min;
    if (r_pat != l_pat) return r_pat > l_pat;

    const char *r_alpha = strstr(rem, "alpha.");
    const char *l_alpha = strstr(loc, "alpha.");
    if (r_alpha && l_alpha) {
        int ra = atoi(r_alpha + 6), la = atoi(l_alpha + 6);
        return ra > la;
    }
    if (!r_alpha && l_alpha) return true;  // remote is full release/beta, local is alpha
    return strcmp(rem, loc) > 0;
}

static void *update_thread(void *arg) {
    (void)arg;
    const char *no_check = getenv("SAVAGEWIND_NO_UPDATE_CHECK");
    if (no_check && *no_check && strcmp(no_check, "0") != 0) return NULL;

    char buf[4096] = {0};
#ifdef _WIN32
    HMODULE wininet = LoadLibraryA("wininet.dll");
    if (!wininet) return NULL;
    typedef void *HINTERNET;
    typedef HINTERNET(WINAPI *pInternetOpenA)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
    typedef HINTERNET(WINAPI *pInternetOpenUrlA)(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
    typedef BOOL(WINAPI *pInternetReadFile)(HINTERNET, LPVOID, DWORD, LPDWORD);
    typedef BOOL(WINAPI *pInternetCloseHandle)(HINTERNET);

    pInternetOpenA fnOpen = (pInternetOpenA)(void *)GetProcAddress(wininet, "InternetOpenA");
    pInternetOpenUrlA fnOpenUrl = (pInternetOpenUrlA)(void *)GetProcAddress(wininet, "InternetOpenUrlA");
    pInternetReadFile fnReadFile = (pInternetReadFile)(void *)GetProcAddress(wininet, "InternetReadFile");
    pInternetCloseHandle fnClose = (pInternetCloseHandle)(void *)GetProcAddress(wininet, "InternetCloseHandle");

    if (!fnOpen || !fnOpenUrl || !fnReadFile || !fnClose) {
        FreeLibrary(wininet);
        return NULL;
    }

    HINTERNET hNet = fnOpen("SavageWind", 1 /* INTERNET_OPEN_TYPE_DIRECT */, NULL, NULL, 0);
    if (!hNet) {
        FreeLibrary(wininet);
        return NULL;
    }

    HINTERNET hUrl = fnOpenUrl(
        hNet, "https://api.github.com/repos/Revivedx/SavageWindRecomp/releases?per_page=1",
        "User-Agent: SavageWind\r\nAccept: application/vnd.github+json\r\n", -1,
        0x80000000 /* INTERNET_FLAG_RELOAD */ | 0x04000000 /* INTERNET_FLAG_NO_CACHE_WRITE */ | 0x00800000 /* INTERNET_FLAG_SECURE */,
        0);

    DWORD bytes_read = 0;
    if (hUrl) {
        fnReadFile(hUrl, buf, sizeof(buf) - 1, &bytes_read);
        fnClose(hUrl);
    }
    fnClose(hNet);
    FreeLibrary(wininet);
#else
    FILE *f = popen("curl -s -m 5 -H \"User-Agent: SavageWind\" -H \"Accept: application/vnd.github+json\" "
                    "https://api.github.com/repos/Revivedx/SavageWindRecomp/releases?per_page=1 2>/dev/null", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        (void)n;
        pclose(f);
    }
#endif

    if (!buf[0]) return NULL;

    const char *tag_key = "\"tag_name\":";
    char *p = strstr(buf, tag_key);
    if (!p) return NULL;
    p += strlen(tag_key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\"') return NULL;
    p++;
    char remote_tag[64] = {0};
    int idx = 0;
    while (*p && *p != '\"' && idx < (int)sizeof(remote_tag) - 1) {
        remote_tag[idx++] = *p++;
    }
    remote_tag[idx] = 0;

    const char *url_key = "\"html_url\":";
    char *u = strstr(buf, url_key);
    char remote_url[256] = {0};
    if (u) {
        u += strlen(url_key);
        while (*u == ' ' || *u == '\t') u++;
        if (*u == '\"') {
            u++;
            idx = 0;
            while (*u && *u != '\"' && idx < (int)sizeof(remote_url) - 1) {
                remote_url[idx++] = *u++;
            }
            remote_url[idx] = 0;
        }
    }

    const char *current = rt_version();
    if (version_is_newer(remote_tag, current)) {
        snprintf(g_latest_version, sizeof g_latest_version, "%s", remote_tag);
        snprintf(g_release_url, sizeof g_release_url, "%s",
                 remote_url[0] ? remote_url : "https://github.com/Revivedx/SavageWindRecomp/releases");
        atomic_store(&g_has_update, true);
        fprintf(stderr, "[updater] New release available: %s (%s)\n", g_latest_version, g_release_url);
    }
    return NULL;
}

void updater_init(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, update_thread, NULL) == 0) {
        pthread_detach(th);
    }
}

