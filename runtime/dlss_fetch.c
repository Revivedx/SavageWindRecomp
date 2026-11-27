// NVIDIA's DLSS libraries, downloaded from NVIDIA's repository (dlss_fetch.h). Windows goes through lib_fetch.c
// (WinHTTP). Linux x86-64 uses the system's curl, as the AppImage update does, without LD_PRELOAD.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (execvpe)
#endif
#include "dlss_fetch.h"
#include "lib_fetch.h"
#include "paths.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

static const LibFile k_files[] = {
    {"lib/Windows_x86_64/rel/nvngx_dlss.dll", "nvngx_dlss.dll", 58956912,
     "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"},
    {"lib/Windows_x86_64/rel/nvngx_dlssg.dll", "nvngx_dlssg.dll", 7506544,
     "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"},
    {"LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt", 26620, "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"},
};
static const LibFetch k_dlss = {"NVIDIA", "/NVIDIA/DLSS/v" DLSS_FETCH_VERSION "/", DLSS_FETCH_VERSION, k_files,
                                (int)(sizeof k_files / sizeof *k_files)};

#include <windows.h>

bool dlss_fetch_offered(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

bool dlss_fetch_present(void) { return lib_fetch_present(&k_dlss); }

bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    return lib_fetch_run(&k_dlss, progress, why, why_size);
}

bool dlss_fetch_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return false;
    char *s = strrchr(out, '\\'), *f = strrchr(out, '/');
    if (f > s) s = f;
    if (!s || s == out) return false;
    *s = 0;
    return true;
}

const char *dlss_fetch_core(void) { return NULL; }

#elif defined(__linux__) && defined(__x86_64__)

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const LibFile k_files[] = {
    {"lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16", "libnvidia-ngx-dlss.so.310.9.16", 59481944,
     "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6"},
    {"LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt", 26620, "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"},
};

// The driver's NGX core. The soname is enough where the loader already searches the driver; NixOS keeps it in
// /run/opengl-driver/lib, and the packaged layouts use /usr/lib and /usr/lib64.
static const char *const k_cores[] = {
    "libnvidia-ngx.so.1",
    "/run/opengl-driver/lib/libnvidia-ngx.so.1",
    "/usr/lib/libnvidia-ngx.so.1",
    "/usr/lib64/libnvidia-ngx.so.1",
};

const char *dlss_fetch_core(void) {
    static int tried;
    static char found[160];
    if (tried) return found[0] ? found : NULL;
    tried = 1;
    for (unsigned i = 0; i < sizeof k_cores / sizeof *k_cores; i++) {
        void *h = dlopen(k_cores[i], RTLD_LAZY | RTLD_LOCAL);
        if (!h) continue;
        dlclose(h);
        snprintf(found, sizeof found, "%s", k_cores[i]);
        return found;
    }
    return NULL;
}

bool dlss_fetch_offered(void) { return dlss_fetch_core() != NULL; }

static bool exe_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return false;
    char *s = strrchr(out, '/');
    if (!s) return false;
    if (s == out) s[1] = 0;
    else *s = 0;
    return true;
}

bool dlss_fetch_dir(char *out, size_t n) {
    char exe[1024];
    if (exe_dir(exe, sizeof exe) && access(exe, W_OK) == 0) return (size_t)snprintf(out, n, "%s", exe) < n;
    const char *data = rt_data_root();
    if (!data || !data[0]) return false;
    return (size_t)snprintf(out, n, "%s", data) < n;
}

static uint64_t file_size(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISREG(s.st_mode) ? (uint64_t)s.st_size : 0;
}

static bool file_at(const char *dir, const char *name, char *out, size_t n) {
    return (size_t)snprintf(out, n, "%s/%s", dir, name) < n;
}

bool dlss_fetch_present(void) {
    char dir[1024], path[1200];
    if (!dlss_fetch_dir(dir, sizeof dir)) return false;
    for (int i = 0; i < (int)(sizeof k_files / sizeof *k_files); i++) {
        if (!file_at(dir, k_files[i].name, path, sizeof path) || file_size(path) != k_files[i].size) return false;
    }
    return true;
}

static char **curl_env(void) {
    size_t n = 0;
    while (environ[n]) n++;
    char **e = (char **)calloc(n + 1, sizeof *e);
    if (!e) return NULL;
    size_t k = 0;
    static const char *const drop[] = {"LD_PRELOAD=", "LD_LIBRARY_PATH=", "APPDIR=", "APPIMAGE=", "ARGV0=", "OWD="};
    for (size_t i = 0; i < n; i++) {
        bool skip = false;
        for (size_t d = 0; d < sizeof drop / sizeof *drop; d++)
            skip |= !strncmp(environ[i], drop[d], strlen(drop[d]));
        if (!skip) e[k++] = environ[i];
    }
    return e;
}

// curl writes part. The size is reported while it runs. Exit status, 127 if curl cannot start.
static int run_curl(char *const argv[], const char *part, uint64_t base, uint64_t total,
                    void (*progress)(uint64_t, uint64_t)) {
    char **env = curl_env();
    if (!env) return 127;
    pid_t pid = fork();
    if (pid < 0) {
        free(env);
        return 127;
    }
    if (!pid) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execvpe(argv[0], argv, env);
        _exit(127);
    }
    free(env);
    for (;;) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (progress && total) {
            uint64_t got = base + file_size(part);
            progress(got > total ? total : got, total);
        }
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0) return -1;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
}

static void hex32(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static bool sha_matches(const char *path, const char *want) {
    FILE *f = fopen(path, "rb");
    struct AVSHA *sha = av_sha_alloc();
    if (!f || !sha || av_sha_init(sha, 256) < 0) {
        if (f) fclose(f);
        av_free(sha);
        return false;
    }
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) av_sha_update(sha, buf, (unsigned)n);
    bool eof = feof(f);
    fclose(f);
    uint8_t d[32];
    char h[65];
    av_sha_final(sha, d);
    av_free(sha);
    hex32(d, h);
    return eof && !strcmp(h, want);
}

static bool fetch_one(const LibFile *lf, const char *dest, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    char url[512], part[1200];
    snprintf(url, sizeof url, "https://raw.githubusercontent.com/NVIDIA/DLSS/v%s/%s", DLSS_FETCH_VERSION, lf->src);
    snprintf(part, sizeof part, "%s.part", dest);
    if (file_size(part) > lf->size) remove(part);
    char *const argv[] = {"curl", "-sS", "-L", "--fail", "-C", "-", "--connect-timeout", "15", "--speed-limit", "1024",
                          "--speed-time", "45", "-A", "SavageWind", "-o", part, url, NULL};
    int rc = run_curl(argv, part, *done, total, progress);
    if (rc == 33) {  // (the server cannot continue a partial file: from the start)
        remove(part);
        rc = run_curl(argv, part, *done, total, progress);
    }
    if (rc == 127) {
        snprintf(why, why_size, "curl is not installed");
        remove(part);
        return false;
    }
    if (rc) {
        snprintf(why, why_size, "%s: curl error %d", lf->name, rc);
        remove(part);
        return false;
    }
    if (file_size(part) != lf->size || !sha_matches(part, lf->sha256)) {
        snprintf(why, why_size, "%s: not the file from NVIDIA's release %s", lf->name, DLSS_FETCH_VERSION);
        remove(part);
        return false;
    }
    if (rename(part, dest) != 0) {
        snprintf(why, why_size, "%s: cannot replace the file (error %d)", lf->name, errno);
        remove(part);
        return false;
    }
    *done += lf->size;
    if (progress) progress(*done, total);
    return true;
}

bool dlss_fetch_run(void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size) {
    why[0] = 0;
    char dir[1024];
    if (!dlss_fetch_dir(dir, sizeof dir)) {
        snprintf(why, why_size, "no folder to write NVIDIA's library");
        return false;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        snprintf(why, why_size, "cannot create %s", dir);
        return false;
    }
    char dest[8][1200];
    bool need[8];
    uint64_t total = 0, done = 0;
    int n = (int)(sizeof k_files / sizeof *k_files);
    for (int i = 0; i < n; i++) {
        if (!file_at(dir, k_files[i].name, dest[i], sizeof dest[i])) {
            snprintf(why, why_size, "the library folder's path is too long");
            return false;
        }
        need[i] = file_size(dest[i]) != k_files[i].size;
        if (need[i]) total += k_files[i].size;
    }
    if (!total) return true;
    for (int i = 0; i < n; i++)
        if (need[i] && !fetch_one(&k_files[i], dest[i], &done, total, progress, why, why_size)) return false;
    return true;
}

#else

bool dlss_fetch_offered(void) { return false; }
bool dlss_fetch_present(void) { return false; }
bool dlss_fetch_run(void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    (void)progress;
    snprintf(why, why_size, "NVIDIA DLSS is not available on this system");
    return false;
}
bool dlss_fetch_dir(char *out, size_t n) {
    (void)out;
    (void)n;
    return false;
}
const char *dlss_fetch_core(void) { return NULL; }

#endif

bool dlss_fetch_missing(void) {
    static int missing = -1;
    if (missing == 0) return false;
    missing = dlss_fetch_offered() && !dlss_fetch_present();  // (a download in this run clears it)
    return missing;
}
