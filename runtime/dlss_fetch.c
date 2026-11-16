// NVIDIA's DLSS library, downloaded from NVIDIA's repository (dlss_fetch.h). WinHTTP (part of Windows) does the
// download; libavutil's SHA-256 (linked anyway, for the movies) checks it. Each file goes to <name>.part first and is
// renamed over only once its size and hash are the recorded ones, so an interrupted or altered download never stays.
#include "dlss_fetch.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>

#define HOST L"raw.githubusercontent.com"
#define PATH L"/NVIDIA/DLSS/v" WIDEN(DLSS_FETCH_VERSION) L"/"
#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)

static const struct {
    const wchar_t *src;   // in NVIDIA/DLSS at the tag
    const wchar_t *name;  // next to savagewind.exe
    uint64_t size;
    const char *sha256;
} k_files[] = {
    {L"lib/Windows_x86_64/rel/nvngx_dlss.dll", L"nvngx_dlss.dll", 58956912,
     "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"},
    {L"LICENSE.txt", L"NVIDIA-DLSS-LICENSE.txt", 26620,
     "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"},
};
enum { NFILES = sizeof k_files / sizeof *k_files };

static bool exe_path(const wchar_t *name, wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    while (len && dir[len - 1] != L'\\' && dir[len - 1] != L'/') len--;
    dir[len] = 0;
    int n = _snwprintf(out, cap, L"%ls%ls", dir, name);
    return n > 0 && (size_t)n < cap;
}

static uint64_t file_size(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool dlss_fetch_offered(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

bool dlss_fetch_present(void) {
    for (int i = 0; i < NFILES; i++) {
        wchar_t p[MAX_PATH + 64];
        if (!exe_path(k_files[i].name, p, sizeof p / sizeof *p) || file_size(p) != k_files[i].size) return false;
    }
    return true;
}

bool dlss_fetch_missing(void) {
    static int missing = -1;
    if (missing < 0) missing = dlss_fetch_offered() && !dlss_fetch_present();
    return missing;
}

static void hex(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// One file into dest (its .part first); done / total: the progress over all files
static bool fetch_one(HINTERNET con, int i, const wchar_t *dest, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    wchar_t path[512], part[MAX_PATH + 64];
    _snwprintf(path, 512, L"%ls%ls", PATH, k_files[i].src);
    path[511] = 0;
    _snwprintf(part, MAX_PATH + 64, L"%ls.part", dest);
    part[MAX_PATH + 63] = 0;
    bool good = false;
    HANDLE f = INVALID_HANDLE_VALUE;
    struct AVSHA *sha = av_sha_alloc();
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    DWORD status = 0, sl = sizeof status;
    if (!sha || av_sha_init(sha, 256) < 0) {
        snprintf(why, why_size, "out of memory");
        goto out;
    }
    if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        snprintf(why, why_size, "no connection to %ls (error %lu)", HOST, GetLastError());
        goto out;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        snprintf(why, why_size, "%ls: HTTP %lu", k_files[i].name, status);
        goto out;
    }
    f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, why_size, "can't write next to savagewind.exe (error %lu)", GetLastError());
        goto out;
    }
    uint64_t got = 0;
    static uint8_t buf[1 << 16];
    for (;;) {
        DWORD n = 0, w = 0;
        if (!WinHttpReadData(req, buf, sizeof buf, &n)) {
            snprintf(why, why_size, "%ls: download interrupted (error %lu)", k_files[i].name, GetLastError());
            goto out;
        }
        if (!n) break;
        if (got + n > k_files[i].size) {
            snprintf(why, why_size, "%ls: larger than NVIDIA's release", k_files[i].name);
            goto out;
        }
        if (!WriteFile(f, buf, n, &w, NULL) || w != n) {
            snprintf(why, why_size, "%ls: disk full or not writable", k_files[i].name);
            goto out;
        }
        av_sha_update(sha, buf, n);
        got += n, *done += n;
        if (progress) progress(*done, total);
    }
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;
    uint8_t d[32];
    char h[65];
    av_sha_final(sha, d);
    hex(d, h);
    if (got != k_files[i].size || strcmp(h, k_files[i].sha256)) {
        snprintf(why, why_size, "%ls: not the file of NVIDIA's release %s (size or SHA-256 differs)", k_files[i].name,
                 DLSS_FETCH_VERSION);
        goto out;
    }
    if (!MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(why, why_size, "%ls: in use? (error %lu)", k_files[i].name, GetLastError());
        goto out;
    }
    good = true;
out:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!good) DeleteFileW(part);
    if (req) WinHttpCloseHandle(req);
    av_free(sha);
    return good;
}

bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    why[0] = 0;
    wchar_t dest[NFILES][MAX_PATH + 64];
    bool need[NFILES];
    uint64_t total = 0, done = 0;
    for (int i = 0; i < NFILES; i++) {
        if (!exe_path(k_files[i].name, dest[i], MAX_PATH + 64)) {
            snprintf(why, why_size, "savagewind.exe's folder has too long a path");
            return false;
        }
        need[i] = file_size(dest[i]) != k_files[i].size;
        if (need[i]) total += k_files[i].size;
    }
    if (!total) return true;
    HINTERNET ses = WinHttpOpen(L"Savage Wind", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    bool good = con != NULL;
    if (!good) snprintf(why, why_size, "no connection to %ls (error %lu)", HOST, GetLastError());
    for (int i = 0; good && i < NFILES; i++)
        if (need[i]) good = fetch_one(con, i, dest[i], &done, total, progress, why, why_size);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return good;
}

#else

bool dlss_fetch_offered(void) { return false; }
bool dlss_fetch_present(void) { return false; }
bool dlss_fetch_missing(void) { return false; }
bool dlss_fetch_run(void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    (void)progress;
    snprintf(why, why_size, "NVIDIA DLSS is for Windows");
    return false;
}

#endif
