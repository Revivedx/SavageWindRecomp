// Synthetic file-size checks; no game data. make test-game-file-size
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#endif
#include "game_file_size.h"

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)
static char root[2048];
static void path(char *out, const char *name) { assert(snprintf(out, 4096, "%s/%s", root, name) < 4096); }
#ifdef _WIN32
static void wide(const char *s, wchar_t *out) {
    wchar_t plain[4096];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, plain, 4096));
    for (wchar_t *c = plain; *c; ++c) if (*c == L'/') *c = L'\\';
    assert(swprintf(out, 4096, L"\\\\?\\%ls", plain) > 0);
}
#endif
static void directory(const char *name) {
    char p[4096]; path(p, name);
#ifdef _WIN32
    wchar_t w[4096]; wide(p, w); assert(CreateDirectoryW(w, NULL));
#else
    assert(mkdir(p, 0700) == 0);
#endif
}
static void file(const char *name, unsigned long long bytes) {
    char p[4096]; path(p, name);
#ifdef _WIN32
    wchar_t w[4096]; wide(p, w);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    DWORD ignored;
    assert(DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ignored, NULL));
    LARGE_INTEGER size; size.QuadPart = bytes;
    assert(SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h));
    CloseHandle(h);
#else
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0);
    assert(ftruncate(fd, (off_t)bytes) == 0); close(fd);
#endif
}
static unsigned polls;
static bool cancel(void *unused) { (void)unused; return ++polls >= 3; }
static void check_size(unsigned long long bytes) {
    RtGameFilesSize size = rt_game_files_size_scan(root, NULL, NULL);
    CHECK(size.complete && !size.cancelled && size.bytes == bytes);
    CHECK(rt_game_files_size(root) == bytes);
}
static void remove_entry(const char *name, bool dir) {
    char p[4096]; path(p, name);
#ifdef _WIN32
    wchar_t w[4096]; wide(p, w);
    bool ok = dir ? RemoveDirectoryW(w) : DeleteFileW(w);
    assert(ok || GetLastError() == ERROR_FILE_NOT_FOUND);
#else
    assert((dir ? rmdir(p) : unlink(p)) == 0 || errno == ENOENT);
#endif
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--scan")) {
        RtGameFilesSize size = rt_game_files_size_scan(argv[2], NULL, NULL);
        printf("bytes=%llu complete=%d cancelled=%d\n", size.bytes, size.complete, size.cancelled);
        return size.complete && !size.cancelled ? 0 : 1;
    }
    CHECK(!rt_game_files_size_scan(NULL, NULL, NULL).complete);
    CHECK(!rt_game_files_size_scan("", NULL, NULL).complete);
#ifdef _WIN32
    wchar_t temp[1024], tmp[2048]; assert(GetTempPathW(1024, temp));
    swprintf(tmp, 2048, L"%lssw-size-%lu-%llu", temp, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryW(tmp, NULL));
    assert(WideCharToMultiByte(CP_UTF8, 0, tmp, -1, root, sizeof root, NULL, NULL));
#else
    strcpy(root, "/tmp/sw-size-XXXXXX"); assert(mkdtemp(root));
#endif
    printf("fixture=%s\n", root);
    check_size(0);
    directory("romfs"); directory("romfs/sub"); directory("exefs"); directory("unrelated");
    file("romfs/sub/.hidden", 23); file("exefs/main", 17); file("unrelated/skip", 999);
    directory("aoc1"); file("aoc1/data", 31); check_size(71);
    file("romfs/Gr\xc3\xb6\xc3\x9f" "e.bin", 43); check_size(114);
    file("romfs/large", 4294967313ULL); check_size(4294967427ULL);
    char deep[2048] = "romfs";
    for (int i=0; i<16; ++i) { strcat(deep, "/long-directory-name"); directory(deep); }
    char leaf[2048]; snprintf(leaf, sizeof leaf, "%s/data", deep); file(leaf, 29);
    check_size(4294967456ULL);
    remove_entry(leaf, false);
    for (int i=0; i<16; ++i) { remove_entry(deep, true); *strrchr(deep, '/') = 0; }
    RtGameFilesSize size = rt_game_files_size_scan(root, cancel, NULL);
    CHECK(size.cancelled && !size.complete && polls == 3);
#ifndef _WIN32
    char link[4096], outside[4096]; path(link, "romfs/linked"); path(outside, "unrelated");
    assert(symlink(outside, link) == 0); check_size(4294967427ULL);
    char blocked[4096]; path(blocked, "romfs/sub"); assert(chmod(blocked, 0000) == 0);
    size = rt_game_files_size_scan(root, NULL, NULL);
    if (geteuid() != 0) CHECK(!size.complete && !size.cancelled);
    assert(chmod(blocked, 0700) == 0);
    char unreadable[4096]; path(unreadable, "exefs/main"); assert(chmod(unreadable, 0000) == 0);
    check_size(4294967427ULL);  // file contents need not be readable to total metadata
    assert(chmod(unreadable, 0600) == 0);
    char gone[4096]; path(gone, "romfs/sub/.hidden"); assert(unlink(gone) == 0); check_size(4294967404ULL);
    remove_entry("romfs/linked", false);
#endif
    char saved[2048]; strcpy(saved, root);
    directory("unrelated/Gr\xc3\xb6\xc3\x9f" "e");
    strcat(root, "/unrelated/Gr\xc3\xb6\xc3\x9f" "e");
    directory("romfs"); file("romfs/data", 13); check_size(13);
    remove_entry("romfs/data", false); remove_entry("romfs", true);
    strcpy(root, saved);
    remove_entry("unrelated/Gr\xc3\xb6\xc3\x9f" "e", true);
    remove_entry("romfs/sub/.hidden", false); remove_entry("romfs/sub", true);
    remove_entry("romfs/Gr\xc3\xb6\xc3\x9f" "e.bin", false); remove_entry("romfs/large", false);
    remove_entry("romfs", true); remove_entry("exefs/main", false); remove_entry("exefs", true);
    remove_entry("aoc1/data", false); remove_entry("aoc1", true);
    remove_entry("unrelated/skip", false); remove_entry("unrelated", true); remove_entry("", true);
    printf("game-file size: %u checks passed\n", checks);
    return 0;
}
