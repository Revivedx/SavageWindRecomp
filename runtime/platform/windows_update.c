// "SW Update.exe" (Windows; build/sw-update.exe in a build): installs the newest release of Savage Wind over this folder.
// Its own small program, linked statically, because it replaces every DLL next to it: it can't load any of them.
//
// What it does, and the only things it does:
//   - asks GitHub for the list of releases and picks the newest one newer than this copy (sw-manifest.txt)
//   - downloads its zip and signature, checks both (a signature made with a key only the maintainers hold)
//   - unpacks to update/staging, checks every file against the zip's list of files (sw-manifest.txt)
//   - moves the old files to update/backup and the new ones into place, keeping a journal: whatever goes wrong, the
//     files go back as they were (runtime/post_update.c removes update/ once the new version has finished setting up)
//   - starts the new savagewind.exe once to see that it can run, and undoes everything if it can't
//   - starts Savage Wind Configuration, which builds the game code when needed and precompiles the shaders
//
// It only ever touches the files in those lists and its own update/ folder: extracted/, save/, cache/, savagewind.ini,
// mods and anything else a player keeps here are never opened.
//
// SW Update.exe                      a window: shows if there is a new version, with an Update button
// SW Update.exe --apply              the same, updating at once (Configuration's Update)
// SW Update.exe --check --out <f>    no window: the answer into <f> as key=value lines (Configuration, at its start)
// SW Update.exe --rollback           back to the version before the last update
// SW Update.exe --recover            finishes undoing an update that was cut off (Savage Wind Configuration.exe)
// --wait-pid <n>: first wait for that process to end; Configuration is reopened when this window closes.
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"  // (only its inflate: stbi_zlib_decode_noheader_buffer)

#include "../update_core.h"
#include "sw_update_key.h"

#define REPO "Revivedx/SavageWindRecomp"
#define FEED_URL "https://api.github.com/repos/" REPO "/releases?per_page=30"
#define DOWNLOAD_PREFIX "https://github.com/" REPO "/releases/download/"
#define RELEASES_PAGE "https://github.com/" REPO "/releases"
#define HEALTH_EXIT 73  // savagewind.exe --update-health (runtime/main.c)
#define WPATH 2048

#ifdef SW_UPDATE_TEST
#define MOVE_RETRY_MS 1200
#else
#define MOVE_RETRY_MS 20000  // (an antivirus scanning a new file holds it for a moment)
#endif

// ------------------------------------------------------------------ what went wrong, for the player
typedef struct {
    char code[32];     // stable, for tests and for Configuration
    char title[200];   // what happened
    char what[1200];   // what it means and what to do
    char tech[500];    // details for a bug report
    bool has;
} Problem;

static Problem g_p;
static FILE *g_log;
static wchar_t g_app[WPATH];        // this program's folder, as \\?\C:\...
static wchar_t g_app_plain[WPATH];  // the folder as Windows gave it, without the long-path prefix: for programs started from here
static char g_app_u8[WPATH * 3];    // the same, for the messages, as the player knows it
static volatile LONG g_cancel;
static bool g_headless;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static bool fail(const char *code, const char *title, const char *fmt, ...) {
    snprintf(g_p.code, sizeof g_p.code, "%s", code);
    snprintf(g_p.title, sizeof g_p.title, "%s", title);
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_p.what, sizeof g_p.what, fmt, a);
    va_end(a);
    g_p.has = true;
    logf_("PROBLEM %s: %s | %s | %s", g_p.code, g_p.title, g_p.what, g_p.tech);
    return false;
}

static void tech(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_p.tech, sizeof g_p.tech, fmt, a);
    va_end(a);
}

// A system error as text (English or the system's language), no line break at the end
static void err_text(DWORD e, char *out, size_t n) {
    wchar_t w[400] = L"";
    HMODULE http = GetModuleHandleW(L"winhttp.dll");
    DWORD fl = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | (http ? FORMAT_MESSAGE_FROM_HMODULE : 0);
    if (!FormatMessageW(fl, http, e, 0, w, 400, NULL)) wcscpy(w, L"unknown error");
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    out[n - 1] = 0;
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ' || out[l - 1] == '.')) out[--l] = 0;
}

// ------------------------------------------------------------------ the window's side of the work
enum { UI_STATUS = WM_APP + 1, UI_PROGRESS, UI_DONE };
typedef enum { R_NONE, R_CURRENT, R_OFFER, R_FINISHED, R_PROBLEM, R_ROLLED_BACK } Result;

static HWND g_win;
static CRITICAL_SECTION g_ui_lock;
static char g_status[400];

static void status(const char *fmt, ...) {
    char b[400];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    logf_("%s", b);
    EnterCriticalSection(&g_ui_lock);
    snprintf(g_status, sizeof g_status, "%s", b);
    LeaveCriticalSection(&g_ui_lock);
    if (g_win) PostMessageW(g_win, UI_STATUS, 0, 0);
}

static void progress(int permille) {  // 0..1000, -1: no bar
    if (g_win) PostMessageW(g_win, UI_PROGRESS, (WPARAM)permille, 0);
}

// ------------------------------------------------------------------ paths and files
static void to_wide(const char *s, wchar_t *out, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
}
static void to_u8(const wchar_t *s, char *out, int n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL)) out[0] = 0;
}

// base\rel (rel in UTF-8 with forward slashes)
static void join(wchar_t *out, const wchar_t *base, const char *rel) {
    wchar_t w[WPATH];
    to_wide(rel, w, WPATH);
    for (wchar_t *c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    swprintf(out, WPATH, L"%ls\\%ls", base, w);
}
static void app_path(wchar_t *out, const char *rel) { join(out, g_app, rel); }

static bool exists_w(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static bool app_exists(const char *rel) {
    wchar_t p[WPATH];
    app_path(p, rel);
    return exists_w(p);
}

static void make_parents(const wchar_t *file) {  // every folder above file, below g_app
    wchar_t p[WPATH];
    wcscpy(p, file);
    size_t base = wcslen(g_app);
    for (wchar_t *c = p + base + 1; *c; c++) {
        if (*c != L'\\') continue;
        *c = 0;
        CreateDirectoryW(p, NULL);
        *c = L'\\';
    }
}

// Removes a folder of ours (only ever update/... ). Files that can't go yet (the running program's old copy) stay.
static void remove_tree(const wchar_t *dir) {
    wchar_t pat[WPATH], p[WPATH];
    swprintf(pat, WPATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
            swprintf(p, WPATH, L"%ls\\%ls", dir, d.cFileName);
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else {
                SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
                if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);
                else DeleteFileW(p);
            }
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
static void remove_update_sub(const char *rel) {
    wchar_t p[WPATH];
    app_path(p, rel);
    remove_tree(p);
}

static bool disk_problem(DWORD e, const char *doing, const char *name) {
    char t[300];
    err_text(e, t, sizeof t);
    tech("%s %s: error %lu (%s)", doing, name, e, t);
    if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL)
        return fail("disk_full", "The disk is full",
                    "There is no space left on the disk that holds Savage Wind. Nothing was changed. Free some space "
                    "(a few GB) and press Retry.");
    if (e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION)
        return fail("locked_file", "A file could not be changed",
                    "Windows would not let SW Update change this file: %s\n\nSomething is using it, usually the game "
                    "still closing or an antivirus scanning it. Close Savage Wind, wait a moment and press Retry. If it "
                    "keeps happening, add the Savage Wind folder as an exclusion in your antivirus.", name);
    return fail("file_error", "A file could not be written",
                "SW Update could not write this file: %s\n\nReason: %s.\nCheck that the disk is working and press Retry.", name, t);
}

static bool write_all(const wchar_t *path, const void *data, size_t n, const char *name) {
    make_parents(path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return disk_problem(GetLastError(), "creating", name);
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        DWORD chunk = n > (1u << 24) ? (1u << 24) : (DWORD)n, w;
        if (!WriteFile(h, p, chunk, &w, NULL) || !w) {
            DWORD e = GetLastError();
            CloseHandle(h);
            return disk_problem(e, "writing", name);
        }
        p += w, n -= w;
    }
    CloseHandle(h);
    return true;
}

static uint8_t *read_all(const wchar_t *path, size_t *n) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *b = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart < (1LL << 31) && (b = (uint8_t *)malloc((size_t)sz.QuadPart + 1))) {
        size_t got = 0;
        DWORD r;
        while (got < (size_t)sz.QuadPart && ReadFile(h, b + got, (DWORD)((size_t)sz.QuadPart - got), &r, NULL) && r) got += r;
        if (got != (size_t)sz.QuadPart) free(b), b = NULL;
        else b[got] = 0, *n = got;
    }
    CloseHandle(h);
    return b;
}

static bool retryable_move(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION;
}

// Renames with retries: an antivirus holds new files for a moment. false: the error in *err.
static bool move_w(const wchar_t *from, const wchar_t *to, DWORD *err) {
    make_parents(to);
    DWORD waited = 0;
    for (;;) {
        SetFileAttributesW(to, FILE_ATTRIBUTE_NORMAL);
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        *err = GetLastError();
        if (!retryable_move(*err) || waited >= MOVE_RETRY_MS) return false;
        Sleep(200);
        waited += 200;
    }
}

// ------------------------------------------------------------------ SHA-256 and the signature (Windows' own crypto)
static bool sha256(const void *data, size_t n, uint8_t out[32]) {
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, (ULONG)n, out, 32));
}
static void hex32(const uint8_t b[32], char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[64] = 0;
}

static bool signature_ok(const uint8_t hash[32], const uint8_t sig[64]) {
    uint8_t key[64];
#ifdef SW_UPDATE_TEST_PUBKEY
    if (!uc_hex_decode(SW_UPDATE_TEST_PUBKEY, key, 64)) return false;
#else
    memcpy(key, SW_UPDATE_PUBKEY, 64);
#endif
    struct { BCRYPT_ECCKEY_BLOB h; uint8_t xy[64]; } blob;
    blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.h.cbKey = 32;
    memcpy(blob.xy, key, 64);
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE k;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return false;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)&blob, sizeof blob, 0))) {
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(k, NULL, (PUCHAR)hash, 32, (PUCHAR)sig, 64, 0));
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ------------------------------------------------------------------ HTTP
static bool net_problem(DWORD e, const char *doing) {
    char t[300];
    err_text(e, t, sizeof t);
    tech("%s: WinHTTP error %lu (%s)", doing, e, t);
    switch (e) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return fail("no_internet", "No internet connection",
                        "SW Update could not find GitHub, where the updates are. Check that this PC is connected to the "
                        "internet and press Retry. You can keep playing the version you have.");
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return fail("cannot_connect", "Can't connect to GitHub",
                        "The connection to GitHub failed or was cut. Your internet may be down, or a firewall, VPN or "
                        "antivirus may be blocking SW Update. Check them and press Retry. You can keep playing the "
                        "version you have.");
        case ERROR_WINHTTP_TIMEOUT:
            return fail("timeout", "GitHub took too long to answer",
                        "The connection is very slow or GitHub is having problems. Press Retry in a moment. Whatever was "
                        "already downloaded is kept.");
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
            return fail("tls", "The connection to GitHub isn't secure",
                        "Windows could not verify GitHub's security certificate, so nothing was downloaded. The usual "
                        "cause is a wrong date or time on this PC: check it. An antivirus or VPN that inspects "
                        "connections can also cause this.");
        case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
        case ERROR_WINHTTP_AUTODETECTION_FAILED:
        case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
        case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
            return fail("proxy", "The proxy settings of this PC don't work",
                        "SW Update could not get out through the proxy configured in Windows. Check Settings > Network & "
                        "Internet > Proxy and press Retry.");
        default:
            return fail("network", "The connection failed",
                        "Something went wrong talking to GitHub (%s). Check your connection and press Retry.", t);
    }
}

typedef struct { HINTERNET s, c, r; } Http;
static void http_close(Http *h) {
    if (h->r) WinHttpCloseHandle(h->r);
    if (h->c) WinHttpCloseHandle(h->c);
    if (h->s) WinHttpCloseHandle(h->s);
    memset(h, 0, sizeof *h);
}

// Sends GET url; on true, *code is the HTTP status and h holds the open request
static bool http_get(Http *h, const char *url, uint64_t from, DWORD *code, const char *doing) {
    memset(h, 0, sizeof *h);
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_NET_ERR")) return net_problem((DWORD)atol(getenv("SW_UPDATE_TEST_NET_ERR")), doing);
#endif
    wchar_t wurl[2048], host[256], path[1800];
    to_wide(url, wurl, 2048);
    URL_COMPONENTSW u = {sizeof u};
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 1500;
    wchar_t extra[300] = L"";
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 300;
    if (!WinHttpCrackUrl(wurl, 0, 0, &u)) return net_problem(GetLastError(), doing);
    wcsncat(path, extra, 1800 - wcslen(path) - 1);
    h->s = WinHttpOpen(L"SavageWind-SWUpdate", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->s) return net_problem(GetLastError(), doing);
    WinHttpSetTimeouts(h->s, 15000, 15000, 30000, 45000);
    h->c = WinHttpConnect(h->s, host, u.nPort, 0);
    if (!h->c) goto err;
    h->r = WinHttpOpenRequest(h->c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!h->r) goto err;
    WinHttpAddRequestHeaders(h->r, L"Accept: application/vnd.github+json, application/octet-stream, */*", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (from) {
        wchar_t range[64];
        swprintf(range, 64, L"Range: bytes=%llu-", (unsigned long long)from);
        WinHttpAddRequestHeaders(h->r, range, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(h->r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto err;
    if (!WinHttpReceiveResponse(h->r, NULL)) goto err;
    DWORD sz = sizeof *code;
    if (!WinHttpQueryHeaders(h->r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, code, &sz, WINHTTP_NO_HEADER_INDEX)) goto err;
    return true;
err:;
    DWORD e = GetLastError();
    http_close(h);
    return net_problem(e, doing);
}

static long header_number(Http *h, const wchar_t *name) {
    wchar_t v[64];
    DWORD sz = sizeof v;
    if (!WinHttpQueryHeaders(h->r, WINHTTP_QUERY_CUSTOM, name, v, &sz, WINHTTP_NO_HEADER_INDEX)) return -1;
    return wcstol(v, NULL, 10);
}

static bool http_status_problem(Http *h, DWORD code, const char *doing) {
    tech("%s: HTTP %lu", doing, code);
    if (code == 403 || code == 429) {
        long left = header_number(h, L"X-RateLimit-Remaining"), reset = header_number(h, L"X-RateLimit-Reset");
        if (left == 0 || code == 429) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            long long now = (((long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000LL) / 10000000;
            long mins = reset > 0 ? (long)((reset - now) / 60) + 1 : 60;
            if (mins < 1) mins = 1;
            return fail("rate_limited", "GitHub asked to wait",
                        "This internet connection has made too many requests to GitHub for now. This resets by itself in "
                        "about %ld minute(s): try again then. You can keep playing the version you have.", mins);
        }
        return fail("forbidden", "GitHub refused the request",
                    "GitHub did not allow this download (HTTP %lu). A VPN or a shared network can cause it. Try again "
                    "later, or download the newest release by hand with the Downloads button.", code);
    }
    if (code == 404)
        return fail("not_found", "The update isn't on GitHub",
                    "The release or its download could not be found (HTTP 404). It may have just been replaced. Try again "
                    "later, or see the Downloads page.");
    if (code >= 500)
        return fail("github_down", "GitHub is having problems",
                    "GitHub answered with an error of its own (HTTP %lu). Nothing is wrong on your side: try again in a "
                    "few minutes.", code);
    return fail("bad_response", "GitHub sent an unexpected answer",
                "SW Update received HTTP %lu. Try again later, or see the Downloads page.", code);
}

// A small body into memory (NUL-terminated)
static char *http_small(const char *url, size_t max, size_t *out_n, const char *doing) {
    Http h;
    DWORD code;
    if (!http_get(&h, url, 0, &code, doing)) return NULL;
    if (code != 200) {
        http_status_problem(&h, code, doing);
        http_close(&h);
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    char *b = (char *)malloc(cap + 1);
    while (b) {
        DWORD got = 0;
        if (n == cap) {
            cap *= 2;
            char *nb = cap > max ? NULL : (char *)realloc(b, cap + 1);
            if (!nb) {
                free(b);
                b = NULL;
                tech("%s: answer larger than %zu bytes", doing, max);
                fail("bad_response", "GitHub sent an unexpected answer", "The answer was far larger than it should be. Try again later.");
                break;
            }
            b = nb;
        }
        if (!WinHttpReadData(h.r, b + n, (DWORD)(cap - n), &got)) {
            DWORD e = GetLastError();
            free(b);
            b = NULL;
            net_problem(e, doing);
            break;
        }
        if (!got) {
            b[n] = 0;
            *out_n = n;
            break;
        }
        n += got;
    }
    http_close(&h);
    return b;
}

// ------------------------------------------------------------------ this copy
static UcManifest g_old;
static char g_current[64];
static bool g_have_old;

static void load_current(void) {
    if (g_have_old) { uc_manifest_free(&g_old); g_have_old = false; }
    snprintf(g_current, sizeof g_current, "0.0.0-alpha.0");
    wchar_t p[WPATH];
    app_path(p, "sw-manifest.txt");
    size_t n;
    char *t = (char *)read_all(p, &n);
    char why[200] = "no file";
    if (t && uc_manifest_parse(t, &g_old, why, sizeof why)) {
        g_have_old = true;
        snprintf(g_current, sizeof g_current, "%s", g_old.version);
    } else logf_("this copy's sw-manifest.txt can't be used (%s): treated as an old version", why);
    free(t);
    logf_("this copy: %s", g_current);
}

// ------------------------------------------------------------------ preflight
static bool app_folder_ok(void) {
    if (app_exists("savagewind.exe")) return true;
    wchar_t tmp[WPATH];
    char tmp_u8[WPATH * 3];
    GetTempPathW(WPATH, tmp);
    to_u8(tmp, tmp_u8, sizeof tmp_u8);
    tech("folder: %s", g_app_u8);
    if (!_strnicmp(g_app_u8, tmp_u8, strlen(tmp_u8)) || strstr(g_app_u8, ".zip"))
        return fail("inside_zip", "SW Update was opened from inside a zip",
                    "It looks like SW Update was opened without unpacking the zip first. Unpack the whole zip into a "
                    "folder, then open Savage Wind Configuration from there.");
    return fail("wrong_folder", "SW Update isn't in the Savage Wind folder",
                "SW Update has to stay in the same folder as savagewind.exe, and it isn't there:\n%s\n\nPut it back, or "
                "open it from your Savage Wind folder.", g_app_u8);
}

static bool folder_writable(void) {
    wchar_t dir[WPATH], p[WPATH];
    app_path(dir, "update");
    CreateDirectoryW(dir, NULL);
    app_path(p, "update/.write-test");
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }
    DWORD e = GetLastError();
    char t[300];
    err_text(e, t, sizeof t);
    tech("write test in %s: error %lu (%s)", g_app_u8, e, t);
    if (e == ERROR_DISK_FULL) return disk_problem(e, "creating", "update/.write-test");
    return fail("read_only", "SW Update can't write in this folder",
                "Windows does not allow writing in:\n%s\n\nThis happens inside Program Files, on a read-only drive, or "
                "with Controlled Folder Access. Move the whole Savage Wind folder somewhere like C:\\Games and try again.",
                g_app_u8);
}

// The processes running from this folder, other than this one (names, comma separated)
static int running_here(char *names, size_t n) {
    names[0] = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof e};
    int count = 0;
    const wchar_t *app = g_app + 4;  // (without \\?\)
    size_t applen = wcslen(app);
    if (Process32FirstW(snap, &e))
        do {
            if (e.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (!p) continue;
            wchar_t path[WPATH];
            DWORD l = WPATH;
            if (QueryFullProcessImageNameW(p, 0, path, &l) && l > applen && path[applen] == L'\\' && !_wcsnicmp(path, app, applen)) {
                char u[300];
                to_u8(e.szExeFile, u, sizeof u);
                if (!strstr(names, u)) {
                    size_t k = strlen(names);
                    snprintf(names + k, n - k, "%s%s", k ? ", " : "", u);
                }
                count++;
            }
            CloseHandle(p);
        } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    return count;
}

static bool nothing_running(void) {
    char names[600];
    if (!running_here(names, sizeof names)) return true;
    tech("running from this folder: %s", names);
    return fail("game_running", "Savage Wind is still open",
                "These programs from this folder are still running: %s\n\nClose the game and press Retry. Your game was "
                "not closed for you, so nothing is lost.", names);
}

static bool enough_disk(uint64_t zip_size) {
    wchar_t root[WPATH];
    swprintf(root, WPATH, L"%ls\\", g_app);
    ULARGE_INTEGER avail;
    uint64_t need = zip_size * 7 + (1ull << 30);  // (the zip, the files unpacked, the old files kept until it's over)
    if (!GetDiskFreeSpaceExW(root, &avail, NULL, NULL)) return true;
    uint64_t have = avail.QuadPart;
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_FREE_MB")) have = (uint64_t)atoll(getenv("SW_UPDATE_TEST_FREE_MB")) << 20;
#endif
    logf_("free space: %llu MB, needed: %llu MB", (unsigned long long)(have >> 20), (unsigned long long)(need >> 20));
    if (have >= need) return true;
    tech("free %llu MB, needed %llu MB", (unsigned long long)(have >> 20), (unsigned long long)(need >> 20));
    return fail("disk_full", "Not enough free disk space",
                "The update needs about %.1f GB free on this disk and there are %.1f GB. Nothing was changed. Free some "
                "space and press Retry.", need / 1073741824.0, have / 1073741824.0);
}

// ------------------------------------------------------------------ choosing the release
static UcRelease g_rel;

// R_OFFER (g_rel), R_CURRENT or R_PROBLEM
static Result find_release(void) {
    status("Checking for a new version...");
    progress(-1);
    const char *url = FEED_URL;
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_FEED")) url = getenv("SW_UPDATE_TEST_FEED");
#endif
    size_t n = 0;
    char *json = http_small(url, 8u << 20, &n, "asking for the releases");
    if (!json) return R_PROBLEM;
    int unsig = 0;
    UcPick pick = uc_pick_release(json, g_current, "-windows.zip", &g_rel, &unsig);
    free(json);
    if (pick == UC_PICK_BAD_JSON) {
        tech("%zu bytes that aren't a list of releases", n);
        fail("bad_response", "GitHub sent an unexpected answer",
             "The list of releases could not be read. A captive portal (hotel or airport Wi-Fi) can cause this. Try again "
             "later, or see the Downloads page.");
        return R_PROBLEM;
    }
    if (unsig) logf_("%d newer release(s) have no signature: passed over", unsig);
    if (pick == UC_PICK_CURRENT) return R_CURRENT;
#ifndef SW_UPDATE_TEST
    if (strncmp(g_rel.zip_url, DOWNLOAD_PREFIX, sizeof DOWNLOAD_PREFIX - 1) ||
        strncmp(g_rel.sig_url, DOWNLOAD_PREFIX, sizeof DOWNLOAD_PREFIX - 1)) {
        tech("zip %s", g_rel.zip_url);
        fail("bad_response", "GitHub sent an unexpected answer", "The download address is not where Savage Wind's releases are. Nothing was downloaded.");
        return R_PROBLEM;
    }
#endif
    logf_("newest: %s (%llu bytes) %s", g_rel.version, (unsigned long long)g_rel.zip_size, g_rel.zip_url);
    return R_OFFER;
}

// ------------------------------------------------------------------ downloading
static bool cancelled(void) {
    if (!g_cancel) return false;
    fail("cancelled", "Update cancelled", "Nothing was changed. What was downloaded is kept for the next time.");
    return true;
}

static void clear_file(const wchar_t *p) {
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(p);
}

// One attempt: appends to path from its current size. true: *complete says if all of it is there.
static bool download_once(const char *url, const wchar_t *path, uint64_t total, bool *complete) {
    *complete = false;
    uint64_t have = 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) have = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (have > total) clear_file(path), have = 0;
    if (have == total) {
        *complete = true;
        return true;
    }
    Http h;
    DWORD code;
    if (!http_get(&h, url, have, &code, "downloading the update")) return false;
    if (code == 416) {  // (what we have doesn't fit what's there now: from the start)
        http_close(&h);
        clear_file(path);
        return true;
    }
    if (code == 200) have = 0;
    else if (code != 206) {
        bool r = http_status_problem(&h, code, "downloading the update");
        http_close(&h);
        return r;
    }
    make_parents(path);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, have ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        http_close(&h);
        return disk_problem(e, "creating", "the download");
    }
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)have;
    SetFilePointerEx(f, at, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    static uint8_t buf[1 << 18];
    bool ok = true;
    for (;;) {
        if (cancelled()) { ok = false; break; }
        DWORD got = 0, w;
        if (!WinHttpReadData(h.r, buf, sizeof buf, &got)) {
            ok = net_problem(GetLastError(), "downloading the update");
            break;
        }
        if (!got) break;
        if (have + got > total) got = (DWORD)(total - have);  // (never more than the size announced)
        if (!WriteFile(f, buf, got, &w, NULL) || w != got) {
            ok = disk_problem(GetLastError(), "writing", "the download");
            break;
        }
        have += got;
        progress((int)(have * 700 / total));
    }
    CloseHandle(f);
    http_close(&h);
    if (ok && have == total) *complete = true;
    return ok;
}

// The zip, complete, in update/download/. false: g_p says why (what was downloaded stays, unless it's bad).
static bool download_and_verify(wchar_t *zip_path, uint8_t **zip_out, size_t *zip_n) {
    char rel[400];
    snprintf(rel, sizeof rel, "update/download/%s", g_rel.zip_name);
    if (!uc_path_safe(g_rel.zip_name)) {
        tech("asset name %s", g_rel.zip_name);
        return fail("bad_response", "GitHub sent an unexpected answer", "The name of the download is not valid. Nothing was downloaded.");
    }
    app_path(zip_path, rel);
    double mb = g_rel.zip_size / 1048576.0;
    bool from_earlier = exists_w(zip_path);  // (kept from a download that stopped; the release may have changed since)
again:;
    int silent_tries = 0;
    for (int attempt = 1;; attempt++) {
        status("Downloading Savage Wind %s (%.0f MB)...", g_rel.version, mb);
        g_p.has = false;
        bool complete = false;
        bool ok = download_once(g_rel.zip_url, zip_path, g_rel.zip_size, &complete);
        if (ok && complete) break;
        if (!strcmp(g_p.code, "cancelled") && g_p.has) return false;
        if (!ok && strcmp(g_p.code, "cannot_connect") && strcmp(g_p.code, "timeout") && strcmp(g_p.code, "network") &&
            strcmp(g_p.code, "no_internet"))
            return false;  // (not something that a moment's wait fixes)
        logf_("download attempt %d stopped", attempt);
        if (attempt >= 4) {
            if (ok) {
                tech("the connection ended before the file did, %d attempts", attempt);
                return fail("download_interrupted", "The download keeps getting cut off",
                            "The connection dropped several times before the download finished. What was downloaded is "
                            "kept: press Retry to continue from there.");
            }
            return false;
        }
        silent_tries++;
        status("The connection dropped, trying again (%d of 3)...", silent_tries);
        Sleep(2000);
    }
    g_p.has = false;

    status("Checking the download...");
    progress(700);
    size_t n = 0;
    uint8_t *zip = read_all(zip_path, &n);
    if (!zip) {
        DWORD e = GetLastError();
        tech("reading the zip: error %lu", e);
        return fail("file_error", "The download could not be read",
                    "The downloaded file disappeared or can't be opened. An antivirus may have removed it. Add the Savage "
                    "Wind folder as an exclusion and press Retry.");
    }
    uint8_t hash[32];
    char hex[65];
    if (!sha256(zip, n, hash)) {
        free(zip);
        return fail("file_error", "The download could not be checked", "Windows' cryptography service did not answer. Restart the PC and try again.");
    }
    hex32(hash, hex);
    logf_("zip sha256 %s", hex);
    if (from_earlier && (n != g_rel.zip_size || (g_rel.digest[0] && strcmp(hex, g_rel.digest)))) {
        logf_("what was kept from an earlier download doesn't match the release: downloading it again");
        free(zip);
        clear_file(zip_path);
        from_earlier = false;
        goto again;
    }
    if (n != g_rel.zip_size || (g_rel.digest[0] && strcmp(hex, g_rel.digest))) {
        tech("size %zu of %llu, sha256 %s, GitHub says %s", n, (unsigned long long)g_rel.zip_size, hex, g_rel.digest);
        free(zip);
        clear_file(zip_path);
        return fail("corrupt_download", "The download arrived damaged",
                    "The file does not match what GitHub says it should be, so it was deleted and nothing was installed. "
                    "Press Retry to download it again.");
    }
    size_t sn = 0;
    char *sig_hex = http_small(g_rel.sig_url, 4096, &sn, "downloading the signature");
    if (!sig_hex) {
        free(zip);
        return false;
    }
    for (size_t i = sn; i && (sig_hex[i - 1] == '\n' || sig_hex[i - 1] == '\r' || sig_hex[i - 1] == ' '); i--) sig_hex[i - 1] = 0;
    uint8_t sig[64];
    bool sig_read = uc_hex_decode(sig_hex, sig, 64);
    free(sig_hex);
    if (!sig_read || !signature_ok(hash, sig)) {
        tech("zip sha256 %s, signature %s", hex, sig_read ? "doesn't match" : "unreadable");
        free(zip);
        clear_file(zip_path);
        return fail("bad_signature", "This update did not pass the security check",
                    "The download is not signed by the Savage Wind team, so it was deleted and NOT installed. Do not "
                    "look for this file elsewhere. Please report this on the project's GitHub page.");
    }
    logf_("signature ok");
    *zip_out = zip;
    *zip_n = n;
    return true;
}

// ------------------------------------------------------------------ unpacking
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct { const uint8_t *d; size_t n; const uint8_t *cd; int entries; size_t cd_size; } Zip;

static bool zip_bad(const char *why) {
    tech("zip: %s", why);
    return fail("corrupt_zip", "The update file is damaged",
                "The downloaded zip can't be unpacked (%s). Nothing was changed. Press Retry to get it again.", why);
}

static bool zip_open(Zip *z, const uint8_t *d, size_t n) {
    z->d = d, z->n = n;
    if (n < 22) return zip_bad("too short");
    size_t lim = n >= 65557 ? n - 65557 : 0;
    for (size_t i = n - 22;; i--) {
        if (rd32(d + i) == 0x06054b50) {
            z->entries = rd16(d + i + 10);
            z->cd_size = rd32(d + i + 12);
            uint32_t off = rd32(d + i + 16);
            if (z->cd_size > i || off > i - z->cd_size) return zip_bad("directory out of range");
            z->cd = d + off;
            return true;
        }
        if (i == lim) return zip_bad("no directory");
    }
}

typedef struct { char name[512]; const uint8_t *comp; uint32_t csize, usize; int method; } ZipEntry;

// The entry at *cd; moves *cd on. false: damaged
static bool zip_next(const Zip *z, const uint8_t **cd, ZipEntry *e) {
    const uint8_t *p = *cd, *end = z->cd + z->cd_size;
    if (end - p < 46 || rd32(p) != 0x02014b50) return zip_bad("bad directory entry");
    int flags = rd16(p + 8), nl = rd16(p + 28), xl = rd16(p + 30), cl = rd16(p + 32);
    e->method = rd16(p + 10);
    e->csize = rd32(p + 20), e->usize = rd32(p + 24);
    uint32_t local = rd32(p + 42);
    if (end - p < 46 + nl + xl + cl || nl >= (int)sizeof e->name) return zip_bad("bad directory entry");
    if (flags & 1) return zip_bad("encrypted");
    if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu) return zip_bad("zip64");
    memcpy(e->name, p + 46, (size_t)nl);
    e->name[nl] = 0;
    for (char *c = e->name; *c; c++)
        if (*c == '\\') *c = '/';
    *cd = p + 46 + nl + xl + cl;
    if ((size_t)local + 30 > z->n || rd32(z->d + local) != 0x04034b50) return zip_bad("bad file header");
    size_t data = (size_t)local + 30 + rd16(z->d + local + 26) + rd16(z->d + local + 28);
    if (data > z->n || e->csize > z->n - data) return zip_bad("file out of range");
    e->comp = z->d + data;
    return true;
}

// The entry's bytes in a malloc'd buffer (usize + 1 long, NUL-terminated); NULL: g_p says why
static uint8_t *zip_read(const ZipEntry *e) {
    if (e->usize > 0x7FFFFFF0u) { zip_bad("file too large"); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)e->usize + 1);
    if (!b) {
        tech("%u bytes for %s", e->usize, e->name);
        fail("out_of_memory", "Not enough memory", "This PC ran out of memory unpacking the update. Close other programs and press Retry.");
        return NULL;
    }
    if (e->method == 0) {
        if (e->csize != e->usize) { free(b); zip_bad("size mismatch"); return NULL; }
        memcpy(b, e->comp, e->usize);
    } else if (e->method == 8) {
        if (e->usize && stbi_zlib_decode_noheader_buffer((char *)b, (int)e->usize, (const char *)e->comp, (int)e->csize) != (int)e->usize) {
            free(b);
            zip_bad("a file doesn't decompress");
            return NULL;
        }
    } else {
        free(b);
        zip_bad("unknown compression");
        return NULL;
    }
    b[e->usize] = 0;
    return b;
}

static UcManifest g_new;
static bool g_have_new;

// Everything in update/staging, each file checked against the zip's list. false: g_p
static bool unpack(const uint8_t *data, size_t n) {
    Zip z;
    if (!zip_open(&z, data, n)) return false;
    const uint8_t *cd = z.cd;
    ZipEntry e;
    bool found = false;
    if (g_have_new) { uc_manifest_free(&g_new); g_have_new = false; }
    for (int i = 0; i < z.entries; i++) {
        if (!zip_next(&z, &cd, &e)) return false;
        const char *rel = uc_strip_top(e.name);
        if (rel && !strcmp(rel, "sw-manifest.txt")) {
            uint8_t *t = zip_read(&e);
            if (!t) return false;
            char why[200];
            bool ok = uc_manifest_parse((char *)t, &g_new, why, sizeof why);
            free(t);
            if (!ok) return zip_bad(why);
            g_have_new = found = true;
            break;
        }
    }
    if (!found) return zip_bad("it has no list of files");
    if (uc_version_cmp(g_new.version, g_rel.version)) {
        tech("list says %s, release says %s", g_new.version, g_rel.version);
        return fail("wrong_version", "The update is not the version it should be",
                    "The file downloaded for %s says it is version %s. It was not installed. Please report this on the "
                    "project's GitHub page.", g_rel.version, g_new.version);
    }
    if (uc_version_cmp(g_new.version, g_current) <= 0) {
        tech("%s over %s", g_new.version, g_current);
        return fail("downgrade", "That version isn't newer", "This copy is %s and the update is %s: nothing to do.", g_current, g_new.version);
    }
    uint64_t total = 0, done = 0;
    for (int i = 0; i < g_new.count; i++) total += g_new.files[i].size;
    char *seen = (char *)calloc((size_t)g_new.count, 1);
    if (!seen) return fail("out_of_memory", "Not enough memory", "This PC ran out of memory. Close other programs and press Retry.");
    remove_update_sub("update/staging");
    wchar_t dir[WPATH];
    app_path(dir, "update/staging");
    CreateDirectoryW(dir, NULL);
    status("Unpacking...");
    cd = z.cd;
    bool ok = true;
    for (int i = 0; i < z.entries && ok; i++) {
        if (cancelled()) { ok = false; break; }
        if (!zip_next(&z, &cd, &e)) { ok = false; break; }
        size_t nl = strlen(e.name);
        if (nl && e.name[nl - 1] == '/') continue;  // (a folder)
        const char *rel = uc_strip_top(e.name);
        if (rel && !strcmp(rel, "sw-manifest.txt")) rel = NULL, ok = true;
        else if (!rel || !uc_manifest_find(&g_new, rel)) {
            tech("not in the list: %s", e.name);
            ok = zip_bad("it has a file that isn't in its list");
            break;
        }
        const char *man_rel = rel ? rel : "sw-manifest.txt";
        uint8_t *b = zip_read(&e);
        if (!b) { ok = false; break; }
        if (rel) {
            const UcFile *f = uc_manifest_find(&g_new, rel);
            uint8_t h[32];
            char hex[65];
            sha256(b, e.usize, h);
            hex32(h, hex);
            if (f->size != e.usize || strcmp(hex, f->sha256)) {
                free(b);
                tech("%s: %u bytes sha256 %s, list says %llu bytes sha256 %s", rel, e.usize, hex, (unsigned long long)f->size, f->sha256);
                ok = zip_bad("a file doesn't match its list");
                break;
            }
            seen[f - g_new.files] = 1;
            done += f->size;
        }
        wchar_t p[WPATH];
        join(p, dir, man_rel);
        ok = write_all(p, b, e.usize, man_rel);
        free(b);
        progress(700 + (int)(total ? done * 200 / total : 0));
    }
    for (int i = 0; ok && i < g_new.count; i++)
        if (!seen[i]) {
            tech("missing from the zip: %s", g_new.files[i].path);
            ok = zip_bad("it is missing a file of its list");
        }
    free(seen);
    return ok;
}

// ------------------------------------------------------------------ the journal: everything needed to undo the swap
// update/journal.txt, one line per step, written to disk before the step is taken:
//   B <path>   the old file is about to move to update/backup
//   P <path>   the new file is about to move from update/staging into place
//   C          all of it done
static HANDLE g_journal = INVALID_HANDLE_VALUE;

static bool journal_line(const char *kind, const char *path) {
    char l[1024];
    int n = snprintf(l, sizeof l, "%s%s%s\n", kind, path ? " " : "", path ? path : "");
    DWORD w;
    if (!WriteFile(g_journal, l, (DWORD)n, &w, NULL) || w != (DWORD)n) return false;
    FlushFileBuffers(g_journal);
    return true;
}

static bool g_rollback_problem;
static char g_rollback_file[400];

// Undoes the journal, last step first. Safe to repeat, and to run after a power cut at any point.
static bool undo_journal(void) {
    wchar_t jp[WPATH];
    app_path(jp, "update/journal.txt");
    size_t n = 0;
    char *t = (char *)read_all(jp, &n);
    if (!t) return false;
    int count = 0, cap = 512;
    char **lines = (char **)malloc((size_t)cap * sizeof *lines);
    for (char *s = strtok(t, "\n"); s && lines; s = strtok(NULL, "\n")) {
        size_t l = strlen(s);
        if (l && s[l - 1] == '\r') s[l - 1] = 0;
        if (count == cap) {
            cap *= 2;
            char **nl = (char **)realloc(lines, (size_t)cap * sizeof *lines);
            if (!nl) break;
            lines = nl;
        }
        lines[count++] = s;
    }
    g_rollback_problem = false;
    for (int i = count - 1; i >= 0; i--) {
        char *l = lines[i];
        if (strlen(l) < 3 || l[1] != ' ' || !uc_path_safe(l + 2)) continue;
        const char *rel = l + 2;
        wchar_t target[WPATH], staged[WPATH], backup[WPATH], trash[WPATH];
        char sub[900];
        app_path(target, rel);
        snprintf(sub, sizeof sub, "update/staging/%s", rel);
        app_path(staged, sub);
        snprintf(sub, sizeof sub, "update/backup/%s", rel);
        app_path(backup, sub);
        DWORD e = 0;
        if (l[0] == 'P') {
            if (exists_w(staged) || !exists_w(target)) continue;  // (it never got there)
            snprintf(sub, sizeof sub, "update/trash/%d-%lu", i, GetTickCount());
            app_path(trash, sub);
            if (!move_w(target, trash, &e)) {
                logf_("undo: can't take away %s (error %lu)", rel, e);
                g_rollback_problem = true;
                snprintf(g_rollback_file, sizeof g_rollback_file, "%s", rel);
            }
        } else if (l[0] == 'B') {
            if (!exists_w(backup)) continue;
            if (!move_w(backup, target, &e)) {
                logf_("undo: can't put back %s (error %lu)", rel, e);
                g_rollback_problem = true;
                snprintf(g_rollback_file, sizeof g_rollback_file, "%s", rel);
            }
        }
    }
    free(lines);
    free(t);
    if (!g_rollback_problem) {
        wchar_t done[WPATH];
        app_path(done, "update/journal.undone.txt");
        MoveFileExW(jp, done, MOVEFILE_REPLACE_EXISTING);
    }
    return !g_rollback_problem;
}

static bool rollback_failed_problem(void) {
    tech("could not restore: %s", g_rollback_file);
    return fail("rollback_failed", "Savage Wind could not be put back as it was",
                "Something kept these files from being restored (last one: %s). Your saves and game files are not "
                "affected.\n\nDownload the newest release with the Downloads button and unpack it over this folder, "
                "replacing everything. Keep this window's message if you report it.", g_rollback_file);
}

static bool journal_cut_off(void) {
    wchar_t jp[WPATH];
    app_path(jp, "update/journal.txt");
    size_t n = 0;
    char *t = (char *)read_all(jp, &n);
    if (!t) return false;
    bool complete = n >= 2 && t[n - 2] == 'C' && t[n - 1] == '\n' && (n == 2 || t[n - 3] == '\n');
    free(t);
    return !complete;
}

// 0: no journal, or a finished one. 1: one that was cut off, now undone. -1: cut off, and undoing it failed.
static int recover_interrupted(void) {
    if (!journal_cut_off()) return 0;
    logf_("an earlier update was cut off: undoing it");
    status("Finishing an update that was interrupted...");
    if (undo_journal()) return 1;
    rollback_failed_problem();
    return -1;
}

static int order_rank(const char *p) {  // what must still work if the power goes during the swap goes last
    if (!_stricmp(p, "savagewind.exe")) return 1;
    if (!_stricmp(p, "Savage Wind Configuration.exe")) return 2;
    if (!_stricmp(p, "SW Update.exe")) return 3;
    return 0;
}

static bool swap_problem(const char *rel, DWORD e) {
    char t[300];
    err_text(e, t, sizeof t);
    tech("moving %s: error %lu (%s)", rel, e, t);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
        return fail("antivirus", "A new file disappeared",
                    "This file vanished before it could be installed: %s\n\nAn antivirus most likely removed it, which is "
                    "a false alarm. Your previous version was put back. Add the Savage Wind folder as an exclusion in "
                    "your antivirus and press Retry.", rel);
    if (e == ERROR_DISK_FULL)
        return disk_problem(e, "moving", rel);
    return fail("locked_file", "A file is in use",
                "This file could not be replaced: %s\n\nReason: %s.\nYour previous version was put back. Close Savage "
                "Wind, wait a moment and press Retry. If it keeps happening, add the Savage Wind folder as an exclusion "
                "in your antivirus.", rel, t);
}

static bool swap_files(void) {
    status("Installing...");
    progress(900);
#ifdef SW_UPDATE_TEST
    int crash_at = getenv("SW_UPDATE_TEST_CRASH_AT") ? atoi(getenv("SW_UPDATE_TEST_CRASH_AT")) : -1, step = 0;
#define MAYBE_CRASH() do { if (step++ == crash_at) { logf_("(test) cut off here"); TerminateProcess(GetCurrentProcess(), 99); } } while (0)
#else
#define MAYBE_CRASH() do {} while (0)
#endif
    remove_update_sub("update/backup");
    wchar_t jp[WPATH];
    app_path(jp, "update/journal.txt");
    g_journal = CreateFileW(jp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_journal == INVALID_HANDLE_VALUE) return disk_problem(GetLastError(), "creating", "update/journal.txt");
    bool ok = true;
    const char *bad_rel = "";
    DWORD bad_err = 0;
    // files of the old version the new one doesn't have
    for (int i = 0; ok && g_have_old && i < g_old.count; i++) {
        const char *rel = g_old.files[i].path;
        if (uc_manifest_find(&g_new, rel) || !app_exists(rel)) continue;
        wchar_t t[WPATH], b[WPATH];
        char sub[900];
        app_path(t, rel);
        snprintf(sub, sizeof sub, "update/backup/%s", rel);
        app_path(b, sub);
        if (!journal_line("B", rel)) { ok = false; bad_rel = rel; bad_err = GetLastError(); break; }
        MAYBE_CRASH();
        if (!move_w(t, b, &bad_err)) { ok = false; bad_rel = rel; break; }
    }
    for (int rank = 0; rank <= 3 && ok; rank++)
        for (int i = 0; ok && i < g_new.count; i++) {
            const char *rel = g_new.files[i].path;
            if (order_rank(rel) != rank) continue;
            wchar_t t[WPATH], b[WPATH], s[WPATH];
            char sub[900];
            app_path(t, rel);
            snprintf(sub, sizeof sub, "update/backup/%s", rel);
            app_path(b, sub);
            snprintf(sub, sizeof sub, "update/staging/%s", rel);
            app_path(s, sub);
            if (exists_w(t)) {
                if (!journal_line("B", rel)) { ok = false; bad_rel = rel; bad_err = GetLastError(); break; }
                MAYBE_CRASH();
                if (!move_w(t, b, &bad_err)) { ok = false; bad_rel = rel; break; }
            }
            if (!journal_line("P", rel)) { ok = false; bad_rel = rel; bad_err = GetLastError(); break; }
            MAYBE_CRASH();
            if (!move_w(s, t, &bad_err)) { ok = false; bad_rel = rel; break; }
        }
    if (ok) {  // the list of files last: from here on this copy is the new version
        wchar_t t[WPATH], b[WPATH], s[WPATH];
        app_path(t, "sw-manifest.txt");
        app_path(b, "update/backup/sw-manifest.txt");
        app_path(s, "update/staging/sw-manifest.txt");
        if (exists_w(t)) {
            if (!journal_line("B", "sw-manifest.txt") || !move_w(t, b, &bad_err)) ok = false, bad_rel = "sw-manifest.txt";
        }
        if (ok && (!journal_line("P", "sw-manifest.txt") || !move_w(s, t, &bad_err))) ok = false, bad_rel = "sw-manifest.txt";
    }
    if (ok) MAYBE_CRASH();
    if (ok && !journal_line("C", NULL)) ok = false, bad_rel = "update/journal.txt", bad_err = GetLastError();
    CloseHandle(g_journal);
    g_journal = INVALID_HANDLE_VALUE;
    if (ok) return true;
    logf_("the swap stopped at %s (error %lu): undoing", bad_rel, bad_err);
    swap_problem(bad_rel, bad_err);
    Problem keep = g_p;
    status("Something went wrong: putting your files back...");
    if (!undo_journal()) return rollback_failed_problem();
    g_p = keep;
    return false;
}

// ------------------------------------------------------------------ starting things
static bool health_check(void) {
    status("Checking the new version...");
    progress(980);
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_SKIP_HEALTH")) return true;
#endif
    wchar_t exe[WPATH], line[WPATH + 40];
    join(exe, g_app_plain, "savagewind.exe");
    swprintf(line, WPATH + 40, L"\"%ls\" --update-health", exe);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        char t[300];
        err_text(e, t, sizeof t);
        tech("starting savagewind.exe: error %lu (%s)", e, t);
        return fail("health_failed", "The new version could not be started",
                    "Windows refused to start the new savagewind.exe (%s). An antivirus is the usual cause. Your previous "
                    "version was put back. Add the Savage Wind folder as an exclusion in your antivirus and press Retry.", t);
    }
    CloseHandle(pi.hThread);
    DWORD w = WaitForSingleObject(pi.hProcess, 90000), code = 1;
    if (w == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    logf_("health check: wait %lu, exit 0x%08lx", w, code);
    if (w == WAIT_OBJECT_0 && code == HEALTH_EXIT) return true;
    tech("savagewind.exe --update-health: wait %lu, exit code 0x%08lX", w, code);
    if (code == 0xC0000135 || code == 0xC0000142 || code == 0xC000007B)
        return fail("health_failed", "The new version is missing something to run",
                    "savagewind.exe could not load one of its files (code 0x%08lX). An antivirus may have removed a "
                    "DLL. Your previous version was put back. Add the Savage Wind folder as an exclusion in your "
                    "antivirus and press Retry.", code);
    return fail("health_failed", "The new version did not start correctly",
                "savagewind.exe stopped with code 0x%08lX. Your previous version was put back and still works. Please "
                "report this on the project's GitHub page, attaching SW Update.log.", code);
}

static void open_configuration(void) {
    wchar_t exe[WPATH], line[WPATH + 40];
    // (a \\?\ path here would be the new program's own idea of where it is, and it joins paths with forward slashes)
    join(exe, g_app_plain, "savagewind.exe");
    if (!exists_w(exe)) return;
    swprintf(line, WPATH + 40, L"\"%ls\" --config", exe);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    AllowSetForegroundWindow(ASFW_ANY);
    if (CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ------------------------------------------------------------------ the work
static bool lock_folder(void) {
    static HANDLE m;
    if (m) return true;
    uint8_t h[32] = {0};
    char hex[65];
    wchar_t lower[WPATH];
    wcscpy(lower, g_app);
    _wcslwr(lower);
    sha256(lower, wcslen(lower) * sizeof(wchar_t), h);
    hex32(h, hex);
    wchar_t name[128];
    swprintf(name, 128, L"Local\\SavageWindUpdate-%.32hs", hex);
    m = CreateMutexW(NULL, FALSE, name);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        m = NULL;
        return fail("already_running", "SW Update is already running", "Another SW Update window is working on this folder. Use that one.");
    }
    return true;
}

static bool preflight(void) {
    if (!app_folder_ok() || !lock_folder() || !folder_writable()) return false;
    int r = recover_interrupted();
    if (r < 0) return false;
    if (r > 0) load_current();
    return true;
}

static bool do_apply(void) {
    if (!nothing_running() || !enough_disk(g_rel.zip_size)) return false;
    remove_update_sub("update/trash");
    wchar_t zip_path[WPATH];
    uint8_t *zip = NULL;
    size_t n = 0;
    if (!download_and_verify(zip_path, &zip, &n)) return false;
    bool ok = unpack(zip, n);
    free(zip);
    if (!ok) return false;
    if (!nothing_running()) return false;  // (it may have been opened during the download)
    if (!swap_files()) return false;
    if (!health_check()) {
        Problem keep = g_p;
        logf_("the new version doesn't start: undoing");
        status("The new version doesn't start: putting your files back...");
        if (!undo_journal()) return rollback_failed_problem();
        g_p = keep;
        return false;
    }
    remove_update_sub("update/staging");
    remove_update_sub("update/download");
    progress(1000);
    status("Done.");
    logf_("installed %s over %s", g_new.version, g_current);
    return true;
}

static bool do_rollback(void) {
    if (!app_folder_ok() || !lock_folder()) return false;
    if (!nothing_running()) return false;
    wchar_t jp[WPATH];
    app_path(jp, "update/journal.txt");
    if (!exists_w(jp))
        return fail("no_backup", "There is no earlier version to go back to",
                    "The files of the previous version are only kept until an update finishes setting up. Use the Downloads "
                    "page if you need an older release.");
    status("Going back to the previous version...");
    if (!undo_journal()) return rollback_failed_problem();
    remove_update_sub("update/staging");
    status("Done.");
    return true;
}

typedef enum { M_INTERACTIVE, M_APPLY, M_CHECK, M_ROLLBACK, M_RECOVER } Mode;
static Mode g_mode;
static DWORD g_wait_pid;
static wchar_t g_out[WPATH];
static volatile LONG g_go;     // the Update button was pressed
static HANDLE g_go_event;
static Result g_result;

static void wait_for_pid(void) {
    if (!g_wait_pid) return;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, g_wait_pid);
    if (p) {
        status("Closing Savage Wind Configuration...");
        WaitForSingleObject(p, 20000);
        CloseHandle(p);
    }
    g_wait_pid = 0;
}

static DWORD WINAPI worker(void *arg) {
    (void)arg;
    g_p.has = false;
    g_p.tech[0] = 0;
    wait_for_pid();
    load_current();
    Result r = R_PROBLEM;
    if (g_mode == M_ROLLBACK) r = do_rollback() ? R_ROLLED_BACK : R_PROBLEM;
    else if (g_mode == M_RECOVER) {
        r = R_FINISHED;
        if (app_exists("update/journal.txt") && lock_folder()) r = recover_interrupted() < 0 ? R_PROBLEM : R_FINISHED;
    } else if (preflight()) {
        r = find_release();
        if (r == R_OFFER && g_mode == M_INTERACTIVE && !g_go) {
            g_result = r;
            if (g_win) PostMessageW(g_win, UI_DONE, 0, 0);
            WaitForSingleObject(g_go_event, INFINITE);
            if (g_cancel) return 0;
            r = R_OFFER;
        }
        if (r == R_OFFER) r = do_apply() ? R_FINISHED : R_PROBLEM;
    }
    g_result = r;
    if (g_win) PostMessageW(g_win, UI_DONE, 1, 0);
    return 0;
}

// ------------------------------------------------------------------ results for other programs
static void put_kv(FILE *f, const char *k, const char *v) {
    fprintf(f, "%s=", k);
    for (; *v; v++)
        if (*v == '\n') fputs("\\n", f);
        else if (*v != '\r') fputc(*v, f);
    fputc('\n', f);
}

static void write_result(Result r) {
    if (!g_out[0]) return;
    wchar_t tmp[WPATH + 8];
    swprintf(tmp, WPATH + 8, L"%ls.tmp", g_out);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return;
    put_kv(f, "status", r == R_CURRENT ? "current" : r == R_OFFER ? "available" : r == R_FINISHED ? "installed"
                        : r == R_ROLLED_BACK ? "rolled_back" : "error");
    put_kv(f, "current", g_current);
    if (r == R_OFFER || r == R_FINISHED) {
        put_kv(f, "version", g_rel.version);
        fprintf(f, "size=%llu\n", (unsigned long long)g_rel.zip_size);
        put_kv(f, "page", g_rel.page_url);
    }
    if (r == R_PROBLEM) {
        put_kv(f, "code", g_p.code);
        put_kv(f, "title", g_p.title);
        put_kv(f, "what", g_p.what);
        put_kv(f, "tech", g_p.tech);
    }
    fclose(f);
    MoveFileExW(tmp, g_out, MOVEFILE_REPLACE_EXISTING);
}

// ------------------------------------------------------------------ the window
enum { ID_ACTION = 101, ID_DOWNLOADS, ID_CLOSE };
static HWND w_title, w_status, w_bar, w_detail, w_action, w_downloads, w_close;
static HFONT g_font, g_font_bold;
static bool g_working, g_relaunch;

static void set_text(HWND w, const char *s) {
    wchar_t b[4000];
    to_wide(s, b, 4000);
    SetWindowTextW(w, b);
}

// Line breaks as an edit control wants them
static void set_detail(const char *s) {
    char b[3000];
    size_t k = 0;
    for (; *s && k + 3 < sizeof b; s++) {
        if (*s == '\n') b[k++] = '\r';
        b[k++] = *s;
    }
    b[k] = 0;
    set_text(w_detail, b);
}

static void show_action(const char *label) {
    if (label) set_text(w_action, label);
    ShowWindow(w_action, label ? SW_SHOW : SW_HIDE);
}

static void start_worker(void) {
    g_working = true;
    g_cancel = 0;
    ResetEvent(g_go_event);
    ShowWindow(w_detail, SW_HIDE);
    ShowWindow(w_downloads, SW_HIDE);
    show_action(NULL);
    set_text(w_close, "Cancel");
    EnableWindow(w_close, TRUE);
    SendMessageW(w_bar, PBM_SETPOS, 0, 0);
    HANDLE t = CreateThread(NULL, 1 << 21, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void on_done(bool final_) {
    Result r = g_result;
    if (!final_) {  // a new version is there: the button
        char t[200];
        snprintf(t, sizeof t, "Savage Wind %s is available", g_rel.version);
        set_text(w_title, t);
        snprintf(t, sizeof t, "You have %s. The download is %.0f MB. Your saves and game files stay as they are.", g_current,
                 g_rel.zip_size / 1048576.0);
        set_text(w_status, t);
        show_action("Update");
        set_text(w_close, "Close");
        g_working = false;
        return;
    }
    g_working = false;
    set_text(w_close, "Close");
    if (r == R_FINISHED && g_mode == M_RECOVER) { PostQuitMessage(0); return; }
    if (r == R_FINISHED || r == R_ROLLED_BACK) {
        g_relaunch = true;
        PostQuitMessage(0);
        return;
    }
    if (r == R_CURRENT) {
        set_text(w_title, "Savage Wind is up to date");
        char t[200];
        snprintf(t, sizeof t, "You have version %s, the newest one.", g_current);
        set_text(w_status, t);
        SendMessageW(w_bar, PBM_SETPOS, 1000, 0);
        return;
    }
    if (r == R_PROBLEM) {
        set_text(w_title, g_p.title);
        set_text(w_status, "");
        char d[3000];
        snprintf(d, sizeof d, "%s%s%s", g_p.what, g_p.tech[0] ? "\n\nDetails: " : "", g_p.tech);
        set_detail(d);
        ShowWindow(w_detail, SW_SHOW);
        ShowWindow(w_bar, SW_HIDE);
        ShowWindow(w_downloads, SW_SHOW);
        if (strcmp(g_p.code, "already_running") && strcmp(g_p.code, "no_backup") && strcmp(g_p.code, "bad_signature") &&
            strcmp(g_p.code, "downgrade") && strcmp(g_p.code, "wrong_folder") && strcmp(g_p.code, "inside_zip") &&
            strcmp(g_p.code, "rollback_failed"))
            show_action("Retry");
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case UI_STATUS: {
            char b[400];
            EnterCriticalSection(&g_ui_lock);
            snprintf(b, sizeof b, "%s", g_status);
            LeaveCriticalSection(&g_ui_lock);
            set_text(w_status, b);
            return 0;
        }
        case UI_PROGRESS: {
            int v = (int)w;
            if (v < 0) {
                SetWindowLongPtrW(w_bar, GWL_STYLE, GetWindowLongPtrW(w_bar, GWL_STYLE) | PBS_MARQUEE);
                SendMessageW(w_bar, PBM_SETMARQUEE, TRUE, 30);
            } else {
                SendMessageW(w_bar, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrW(w_bar, GWL_STYLE, GetWindowLongPtrW(w_bar, GWL_STYLE) & ~PBS_MARQUEE);
                SendMessageW(w_bar, PBM_SETPOS, (WPARAM)v, 0);
            }
            ShowWindow(w_bar, SW_SHOW);
            return 0;
        }
        case UI_DONE:
            on_done(w != 0);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_ACTION:
                    if (g_result == R_OFFER && !g_working) {  // Update: the worker is waiting
                        InterlockedExchange(&g_go, 1);
                        g_working = true;
                        show_action(NULL);
                        set_text(w_close, "Cancel");
                        set_text(w_title, "Updating Savage Wind");
                        SetEvent(g_go_event);
                    } else if (!g_working) {  // Retry
                        InterlockedExchange(&g_go, g_mode == M_INTERACTIVE ? 1 : 0);
                        set_text(w_title, "Updating Savage Wind");
                        ShowWindow(w_bar, SW_SHOW);
                        start_worker();
                    }
                    return 0;
                case ID_DOWNLOADS:
                    ShellExecuteW(h, L"open", L"" RELEASES_PAGE, NULL, NULL, SW_SHOWNORMAL);
                    return 0;
                case ID_CLOSE:
                    if (g_working) {
                        InterlockedExchange(&g_cancel, 1);
                        EnableWindow(w_close, FALSE);
                        set_text(w_status, "Stopping...");
                        SetEvent(g_go_event);
                    } else DestroyWindow(h);
                    return 0;
            }
            break;
        case WM_CLOSE:
            if (g_working) {
                InterlockedExchange(&g_cancel, 1);
                set_text(w_status, "Stopping...");
                SetEvent(g_go_event);
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, m, w, l);
}

static int run_window(HINSTANCE inst) {
    INITCOMMONCONTROLSEX cc = {sizeof cc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&cc);
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
#define S(x) MulDiv((x), dpi, 96)
    WNDCLASSW c = {0};
    c.lpfnWndProc = wndproc;
    c.hInstance = inst;
    c.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    c.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    c.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    c.lpszClassName = L"SavageWindUpdate";
    RegisterClassW(&c);
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-S(19), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT r = {0, 0, S(560), S(290)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_win = CreateWindowExW(0, c.lpszClassName, L"SW Update", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    if (!g_win) return 1;
#define CTRL(cls, st, x, y, w_, h_, id) \
    CreateWindowExW(0, cls, L"", WS_CHILD | (st), S(x), S(y), S(w_), S(h_), g_win, (HMENU)(INT_PTR)(id), inst, NULL)
    w_title = CTRL(L"STATIC", WS_VISIBLE | SS_LEFT, 20, 16, 520, 30, 0);
    w_status = CTRL(L"STATIC", WS_VISIBLE | SS_LEFT, 20, 52, 520, 40, 0);
    w_bar = CTRL(PROGRESS_CLASSW, WS_VISIBLE | PBS_SMOOTH, 20, 100, 520, 18, 0);
    w_detail = CTRL(L"EDIT", WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 52, 520, 180, 0);
    w_action = CTRL(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, 330, 248, 100, 30, ID_ACTION);
    w_downloads = CTRL(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 20, 248, 110, 30, ID_DOWNLOADS);
    w_close = CTRL(L"BUTTON", WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 440, 248, 100, 30, ID_CLOSE);
    HWND all[] = {w_status, w_bar, w_detail, w_action, w_downloads, w_close};
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(w_title, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
    SendMessageW(w_bar, PBM_SETRANGE32, 0, 1000);
    set_text(w_downloads, "Downloads");
    set_text(w_title, g_mode == M_ROLLBACK ? "Going back to the previous version" : g_mode == M_INTERACTIVE ? "SW Update" : "Updating Savage Wind");
    ShowWindow(g_win, SW_SHOW);
    start_worker();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return g_result == R_PROBLEM;
}

// ------------------------------------------------------------------ start
static void open_log(void) {
    wchar_t p[WPATH];
    app_path(p, "SW Update.log");
    WIN32_FILE_ATTRIBUTE_DATA a;
    bool big = GetFileAttributesExW(p, GetFileExInfoStandard, &a) && a.nFileSizeLow > (1u << 20);
    g_log = _wfopen(p, big ? L"wb" : L"ab");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev, (void)cmd, (void)show;
    wchar_t exe[WPATH];
    DWORD n = GetModuleFileNameW(NULL, exe, WPATH);
    wchar_t *slash = n && n < WPATH ? wcsrchr(exe, L'\\') : NULL;
    if (!slash) return 1;
    *slash = 0;
    if (!wcsncmp(exe, L"\\\\", 2) && wcsncmp(exe, L"\\\\?\\", 4)) swprintf(g_app, WPATH, L"\\\\?\\UNC\\%ls", exe + 2);
    else if (!wcsncmp(exe, L"\\\\?\\", 4)) wcscpy(g_app, exe);
    else swprintf(g_app, WPATH, L"\\\\?\\%ls", exe);
    wcscpy(g_app_plain, exe);
    to_u8(exe, g_app_u8, sizeof g_app_u8);

    InitializeCriticalSection(&g_ui_lock);
    g_go_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_mode = M_INTERACTIVE;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--apply")) g_mode = M_APPLY;
        else if (!wcscmp(argv[i], L"--check")) g_mode = M_CHECK;
        else if (!wcscmp(argv[i], L"--rollback")) g_mode = M_ROLLBACK;
        else if (!wcscmp(argv[i], L"--recover")) g_mode = M_RECOVER;
        else if (!wcscmp(argv[i], L"--wait-pid") && i + 1 < argc) g_wait_pid = (DWORD)_wtol(argv[++i]);
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) wcsncpy(g_out, argv[++i], WPATH - 1);
#ifdef SW_UPDATE_TEST
        else if (!wcscmp(argv[i], L"--headless")) g_headless = true;
#endif
    }
    bool from_config = g_wait_pid != 0;
    if (g_mode != M_CHECK) open_log();  // (the check runs at every start: it leaves no trace unless something is updated)

    if (g_mode == M_CHECK) {
        load_current();
        Result r = find_release();
        write_result(r);
        return r == R_PROBLEM;
    }
    if (g_mode == M_RECOVER && !journal_cut_off()) return 0;  // (nothing to do: no window)
    int rc;
    if (g_headless) {
        worker(NULL);
        write_result(g_result);
        rc = g_result == R_PROBLEM;
        if (g_result == R_FINISHED && g_mode != M_RECOVER) open_configuration();
        if (g_result == R_ROLLED_BACK) open_configuration();
        return rc;
    }
    rc = run_window(inst);
    if (g_relaunch || (from_config && g_mode != M_RECOVER)) open_configuration();
    return rc;
}
