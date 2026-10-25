// POSIX bits mingw-w64 lacks (included via runtime/platform.h on Windows only).
// Build with -D_FILE_OFFSET_BITS=64 so off_t / stat sizes are 64-bit (RomFS image offsets are > 4 GB).
#pragma once
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Positional read/write (thread-safe: does not move the descriptor's file pointer).
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);

static inline int setenv(const char *k, const char *v, int overwrite) {
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v);
}
static inline int unsetenv(const char *k) { return _putenv_s(k, ""); }

static inline int fsync(int fd) { return _commit(fd); }

#ifndef _CRT_RAND_S
int __cdecl rand_s(unsigned int *v);  // (declared by stdlib.h only with _CRT_RAND_S)
#endif
static inline uint32_t arc4random(void) {
    unsigned v = 0;
    rand_s(&v);
    return v;
}

static inline uint32_t arc4random_uniform(uint32_t n) {
    if (n < 2) return 0;
    uint32_t min = -n % n, r;  // reject the low values that would bias the modulo
    do r = arc4random(); while (r < min);
    return r % n;
}

static inline size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}
static inline size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
static inline char *strsep(char **sp, const char *delim) {
    char *s = *sp;
    if (!s) return NULL;
    char *e = s + strcspn(s, delim);
    *sp = *e ? e + 1 : NULL;
    *e = 0;
    return s;
}

// POSIX mkdir(path, mode); mingw's takes only the path.
static inline int plat_mkdir(const char *p, int mode) { (void)mode; return _mkdir(p); }
#define mkdir(p, m) plat_mkdir(p, m)

// realpath(path, out[PATH_MAX]): absolute form of an existing path (links not resolved; callers check them).
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
static inline char *realpath(const char *p, char *out) {
    struct stat st;
    return stat(p, &st) == 0 ? _fullpath(out, p, PATH_MAX) : NULL;
}
