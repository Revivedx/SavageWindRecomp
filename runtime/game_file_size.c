#include "game_file_size.h"
#include "game_files.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    RtGameFilesSize result;
    bool (*cancelled)(void *);
    void *user;
} Scan;

static bool stopped(Scan *s) {
    if (s->result.cancelled || (s->cancelled && s->cancelled(s->user))) {
        s->result.cancelled = true;
        s->result.complete = false;
        return true;
    }
    return false;
}

static void add(Scan *s, unsigned long long bytes) {
    if (bytes > ULLONG_MAX - s->result.bytes) s->result.complete = false;
    else s->result.bytes += bytes;
}

#ifdef _WIN32
static wchar_t *child_path(const wchar_t *parent, const wchar_t *name) {
    size_t a = wcslen(parent), b = wcslen(name);
    wchar_t *p = malloc((a + b + 2) * sizeof *p);
    if (p) { memcpy(p, parent, a * sizeof *p); p[a] = L'\\'; memcpy(p + a + 1, name, (b + 1) * sizeof *p); }
    return p;
}

// Absolute, extended-length paths keep the worker independent of relative-path parsing and preserve Unicode names.
static wchar_t *windows_root(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *input = malloc((size_t)n * sizeof *input);
    if (!input) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, n)) { free(input); return NULL; }
    for (wchar_t *p = input; *p; ++p) if (*p == L'/') *p = L'\\';
    DWORD len = GetFullPathNameW(input, 0, NULL, NULL);
    wchar_t *full = len ? malloc((size_t)len * sizeof *full) : NULL;
    DWORD got = full ? GetFullPathNameW(input, len, full, NULL) : 0;
    free(input);
    if (!got || got >= len) { free(full); return NULL; }
    if (!wcsncmp(full, L"\\\\?\\", 4)) return full;
    bool unc = !wcsncmp(full, L"\\\\", 2);
    const wchar_t *prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    size_t prefix_len = wcslen(prefix), skip = unc ? 2 : 0;
    wchar_t *extended = malloc((prefix_len + got - skip + 1) * sizeof *extended);
    if (extended) {
        memcpy(extended, prefix, prefix_len * sizeof *extended);
        memcpy(extended + prefix_len, full + skip, (got - skip + 1) * sizeof *extended);
    }
    free(full);
    return extended;
}

static void windows_entry(Scan *s, const wchar_t *path, DWORD attributes, DWORD high, DWORD low) {
    if (stopped(s) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { add(s, ((unsigned long long)high << 32) | low); return; }
    wchar_t *pattern = child_path(path, L"*");
    if (!pattern) { s->result.complete = false; return; }
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &entry, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) s->result.complete = false;
        return;
    }
    for (;;) {
        if (stopped(s)) break;
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..") &&
            !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                wchar_t *child = child_path(path, entry.cFileName);
                if (!child) s->result.complete = false;
                else { windows_entry(s, child, entry.dwFileAttributes, 0, 0); free(child); }
            } else add(s, ((unsigned long long)entry.nFileSizeHigh << 32) | entry.nFileSizeLow);
        }
        if (!FindNextFileW(find, &entry)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) s->result.complete = false;
            break;
        }
    }
    FindClose(find);
}
#elif defined(__linux__)
// One metadata lookup per child, relative to the open parent rather than resolving its whole path again.
static void linux_directory(Scan *s, int fd) {
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); s->result.complete = false; return; }
    while (!stopped(s)) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) s->result.complete = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { s->result.complete = false; continue; }
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) s->result.complete = false;
            else linux_directory(s, child);
        } else if (st.st_size < 0) s->result.complete = false;
        else add(s, (unsigned long long)st.st_size);
    }
    closedir(dir);
}
#else
// macOS retains the portable traversal; native bulk metadata fetching needs its own platform validation.
static void portable_entry(Scan *s, const char *path) {
    if (stopped(s)) return;
    struct stat st;
    if (lstat(path, &st)) { s->result.complete = false; return; }
    if (S_ISLNK(st.st_mode)) return;
    if (stat(path, &st)) { s->result.complete = false; return; }
    if (!S_ISDIR(st.st_mode)) { add(s, (unsigned long long)st.st_size); return; }
    DIR *dir = opendir(path);
    if (!dir) { s->result.complete = false; return; }
    while (!stopped(s)) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) s->result.complete = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t len = strlen(path) + strlen(entry->d_name) + 2;
        char *child = malloc(len);
        if (!child) { s->result.complete = false; break; }
        snprintf(child, len, "%s/%s", path, entry->d_name);
        portable_entry(s, child);
        free(child);
    }
    closedir(dir);
}
#endif

RtGameFilesSize rt_game_files_size_scan(const char *game_dir, bool (*cancelled)(void *), void *user) {
    Scan s = {.result = {.complete = true}, .cancelled = cancelled, .user = user};
    if (!game_dir || !*game_dir) { s.result.complete = false; return s.result; }
#ifdef _WIN32
    wchar_t *root = windows_root(game_dir);
    if (!root) { s.result.complete = false; return s.result; }
#endif
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES && !stopped(&s); ++i) {
#ifdef _WIN32
        wchar_t name[64];
        if (!MultiByteToWideChar(CP_UTF8, 0, GAME_FILES[i], -1, name, 64)) { s.result.complete = false; break; }
        wchar_t *path = child_path(root, name);
        if (!path) { s.result.complete = false; break; }
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &data))
            windows_entry(&s, path, data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow);
        else {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) s.result.complete = false;
        }
        free(path);
#else
        size_t len = strlen(game_dir) + strlen(GAME_FILES[i]) + 2;
        char *path = malloc(len);
        if (!path) { s.result.complete = false; break; }
        snprintf(path, len, "%s/%s", game_dir, GAME_FILES[i]);
        struct stat st;
        if (lstat(path, &st)) {
            if (errno != ENOENT && errno != ENOTDIR) s.result.complete = false;
        } else if (!S_ISLNK(st.st_mode)) {
#if defined(__linux__)
            if (S_ISDIR(st.st_mode)) {
                int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) s.result.complete = false;
                else linux_directory(&s, fd);
            } else if (st.st_size < 0) s.result.complete = false;
            else add(&s, (unsigned long long)st.st_size);
#else
            portable_entry(&s, path);
#endif
        }
        free(path);
#endif
    }
#ifdef _WIN32
    free(root);
#endif
    return s.result;
}

unsigned long long rt_game_files_size(const char *game_dir) { return rt_game_files_size_scan(game_dir, NULL, NULL).bytes; }
