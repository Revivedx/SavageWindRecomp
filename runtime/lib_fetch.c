// A vendor's library downloaded from the vendor's repository (lib_fetch.h). WinHTTP (part of Windows) does the
// download; libavutil's SHA-256 (linked anyway, for the movies) checks it. Each file goes to <name>.part first and is
// renamed over only once its size and hash are the recorded ones, so an interrupted or altered download never stays.
#include "lib_fetch.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>

#define HOST L"raw.githubusercontent.com"

static bool exe_path(const char *name, wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH], wname[128];
    DWORD len = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    while (len && dir[len - 1] != L'\\' && dir[len - 1] != L'/') len--;
    dir[len] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128)) return false;
    int n = _snwprintf(out, cap, L"%ls%ls", dir, wname);
    return n > 0 && (size_t)n < cap;
}

static uint64_t file_size(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool lib_fetch_present(const LibFetch *l) {
    for (int i = 0; i < l->nfiles; i++) {
        wchar_t p[MAX_PATH + 64];
        if (!exe_path(l->files[i].name, p, sizeof p / sizeof *p) || file_size(p) != l->files[i].size) return false;
    }
    return true;
}

static void hex(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// One file into dest (its .part first); done / total: the progress over all files
static bool fetch_one(const LibFetch *l, HINTERNET con, int i, const wchar_t *dest, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    const LibFile *lf = &l->files[i];
    char url[512];
    wchar_t path[512], part[MAX_PATH + 64];
    snprintf(url, sizeof url, "%s%s", l->path, lf->src);
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, path, 512)) {
        snprintf(why, why_size, "%s: path too long", lf->name);
        return false;
    }
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
        snprintf(why, why_size, "%s: HTTP %lu", lf->name, status);
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
            snprintf(why, why_size, "%s: download interrupted (error %lu)", lf->name, GetLastError());
            goto out;
        }
        if (!n) break;
        if (got + n > lf->size) {
            snprintf(why, why_size, "%s: larger than %s's release", lf->name, l->vendor);
            goto out;
        }
        if (!WriteFile(f, buf, n, &w, NULL) || w != n) {
            snprintf(why, why_size, "%s: disk full or not writable", lf->name);
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
    if (got != lf->size || strcmp(h, lf->sha256)) {
        snprintf(why, why_size, "%s: not the file of %s's release %s (size or SHA-256 differs)", lf->name, l->vendor,
                 l->version);
        goto out;
    }
    if (!MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(why, why_size, "%s: in use? (error %lu)", lf->name, GetLastError());
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

bool lib_fetch_run(const LibFetch *l, void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    why[0] = 0;
    enum { MAXF = 8 };
    wchar_t dest[MAXF][MAX_PATH + 64];
    bool need[MAXF];
    uint64_t total = 0, done = 0;
    if (l->nfiles > MAXF) {
        snprintf(why, why_size, "too many files");
        return false;
    }
    for (int i = 0; i < l->nfiles; i++) {
        if (!exe_path(l->files[i].name, dest[i], MAX_PATH + 64)) {
            snprintf(why, why_size, "savagewind.exe's folder has too long a path");
            return false;
        }
        need[i] = file_size(dest[i]) != l->files[i].size;
        if (need[i]) total += l->files[i].size;
    }
    if (!total) return true;
    HINTERNET ses = WinHttpOpen(L"Savage Wind", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    bool good = con != NULL;
    if (!good) snprintf(why, why_size, "no connection to %ls (error %lu)", HOST, GetLastError());
    for (int i = 0; good && i < l->nfiles; i++)
        if (need[i]) good = fetch_one(l, con, i, dest[i], &done, total, progress, why, why_size);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return good;
}

#else

bool lib_fetch_present(const LibFetch *l) {
    (void)l;
    return false;
}
bool lib_fetch_run(const LibFetch *l, void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    (void)progress;
    snprintf(why, why_size, "%s's library is for Windows", l->vendor);
    return false;
}

#endif
