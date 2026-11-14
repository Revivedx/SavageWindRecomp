// Updating a Linux AppImage (runtime/update_appimage.h).
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (execvpe)
#endif
#include "update_appimage.h"
#include "platform.h"
#include "platform/sw_update_key.h"
#include "update_crypto.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define REPO "Revivedx/SavageWindRecomp"
#define FEED_URL "https://api.github.com/repos/" REPO "/releases?per_page=30"
#define DOWNLOAD_PREFIX "https://github.com/" REPO "/releases/download/"
#define HEALTH_EXIT 73  // savagewind --update-health (runtime/main.c)

#if defined(__x86_64__)
#define ASSET_SUFFIX "-x86_64.AppImage"
#elif defined(__aarch64__)
#define ASSET_SUFFIX "-aarch64.AppImage"
#else
#define ASSET_SUFFIX "-unsupported.AppImage"
#endif

extern char **environ;

static bool fail(UpdateProblem *p, const char *code, const char *title, const char *fmt, ...) {
    snprintf(p->code, sizeof p->code, "%s", code);
    snprintf(p->title, sizeof p->title, "%s", title);
    va_list a;
    va_start(a, fmt);
    vsnprintf(p->what, sizeof p->what, fmt, a);
    va_end(a);
    fprintf(stderr, "[update] PROBLEM %s: %s | %s\n", p->code, p->title, p->what);
    return false;
}

// $APPIMAGE only when this program is the one inside it: the runtime sets $APPDIR to its mount, which this program then
// runs from. An AppImage launcher passes its own $APPIMAGE / $APPDIR on to the games it starts (Quiver Launcher before
// 3.3.3), and a source build started from such a terminal inherits them: "updating" would replace that other program.
static char g_self_image[1100];

static void appimage_find(void) {
    const char *a = getenv("APPIMAGE"), *d = getenv("APPDIR");
    if (!a || !*a) return;
    char exe[1100], appdir[1100], real[1100];
    if (!d || !realpath(d, appdir) || !plat_exe_path(exe, sizeof exe) || !uc_path_inside(exe, appdir)) {
        fprintf(stderr, "[update] APPIMAGE=%s is another program's: this one is not an AppImage release\n", a);
        return;
    }
    // (the file itself: started through a symbolic link, the update replaces what it points to, not the link)
    snprintf(g_self_image, sizeof g_self_image, "%s", realpath(a, real) ? real : a);
}

const char *appimage_path(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, appimage_find);
    return g_self_image[0] ? g_self_image : NULL;
}

// The environment for another program. What the AppImage runtime set describes this mount, not the next program's.
// curl is the system's: libraries a launcher such as Steam injects aren't meant for it (no_ld).
static char **child_env(bool no_ld) {
    size_t n = 0;
    while (environ[n]) n++;
    char **e = (char **)calloc(n + 1, sizeof *e);
    size_t k = 0;
    static const char *const drop[] = {"APPDIR=", "APPIMAGE=", "ARGV0=", "OWD="}, *const ld[] = {"LD_LIBRARY_PATH=", "LD_PRELOAD="};
    for (size_t i = 0; e && i < n; i++) {
        bool skip = false;
        for (size_t d = 0; d < sizeof drop / sizeof *drop; d++) skip |= !strncmp(environ[i], drop[d], strlen(drop[d]));
        for (size_t d = 0; no_ld && d < sizeof ld / sizeof *ld; d++) skip |= !strncmp(environ[i], ld[d], strlen(ld[d]));
        if (!skip) e[k++] = environ[i];
    }
    return e;
}

// Runs argv (looked up in PATH), stdout into out if given; exit status, 127 if it can't start, -1 if it had to be stopped.
// While it runs, *watch_size (if given) follows the size of watch.
static int run(char *const argv[], const char *out, int timeout_s, bool no_ld, const char *watch, uint64_t watch_base,
               uint64_t watch_total, volatile float *progress, float from, float to) {
    char **env = child_env(no_ld);
    if (!env) return 127;
    pid_t pid = fork();
    if (pid < 0) {
        free(env);
        return 127;
    }
    if (!pid) {
        int fd = open(out ? out : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) dup2(fd, 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        execvpe(argv[0], argv, env);
        _exit(127);
    }
    free(env);
    time_t t0 = time(NULL);
    for (;;) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0) return -1;
        if (timeout_s && time(NULL) - t0 > timeout_s) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -1;
        }
        if (watch && progress && watch_total) {
            struct stat s;
            if (stat(watch, &s) == 0) {
                float f = (float)(s.st_size + (long long)watch_base) / (float)watch_total;
                *progress = from + (to - from) * (f > 1 ? 1 : f);
            }
        }
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
}

static bool curl_problem(int rc, UpdateProblem *p) {
    switch (rc) {
        case 127:
            return fail(p, "no_curl", "curl is not installed",
                        "Savage Wind uses your system's curl program to download updates and could not find it. Install "
                        "curl with your distribution's package manager, or get the new AppImage from the project's "
                        "GitHub releases page.");
        case 6:
            return fail(p, "no_internet", "No internet connection",
                        "Savage Wind could not find GitHub, where the updates are. Check that this device is connected "
                        "to the internet and try again. You can keep playing the version you have.");
        case 5:
            return fail(p, "proxy", "The proxy settings don't work",
                        "curl could not get out through the configured proxy. Check your proxy settings and try again.");
        case 7:
        case 52:
        case 55:
        case 56:
            return fail(p, "cannot_connect", "Can't connect to GitHub",
                        "The connection to GitHub failed or was cut. Your internet may be down, or a firewall or VPN may "
                        "be blocking it. Check them and try again. You can keep playing the version you have.");
        case -1:
        case 28:
            return fail(p, "timeout", "GitHub took too long to answer",
                        "The connection is very slow or GitHub is having problems. Try again in a moment. Whatever was "
                        "already downloaded is kept.");
        case 35:
        case 51:
        case 58:
        case 60:
        case 77:
        case 80:
            return fail(p, "tls", "The connection to GitHub isn't secure",
                        "GitHub's security certificate could not be verified, so nothing was downloaded. The usual cause "
                        "is a wrong date or time on this device: check it. (curl error %d)", rc);
        case 23:
            return fail(p, "disk_full", "The download could not be written",
                        "There may be no space left on the disk that holds the AppImage. Free some space and try again.");
        default:
            return fail(p, "network", "The connection failed",
                        "Something went wrong talking to GitHub (curl error %d). Check your connection and try again.", rc);
    }
}

static long header_number(const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    long v = -1;
    size_t l = strlen(name);
    while (fgets(line, sizeof line, f))
        if (!strncasecmp(line, name, l) && line[l] == ':') v = strtol(line + l + 1, NULL, 10);
    fclose(f);
    return v;
}

static bool http_problem(long code, const char *headers, UpdateProblem *p) {
    if (code == 403 || code == 429) {
        long left = header_number(headers, "x-ratelimit-remaining"), reset = header_number(headers, "x-ratelimit-reset");
        if (left == 0 || code == 429) {
            long mins = reset > 0 ? (reset - (long)time(NULL)) / 60 + 1 : 60;
            if (mins < 1) mins = 1;
            return fail(p, "rate_limited", "GitHub asked to wait",
                        "This internet connection has made too many requests to GitHub for now. This resets by itself in "
                        "about %ld minute(s): try again then. You can keep playing the version you have.", mins);
        }
        return fail(p, "forbidden", "GitHub refused the request",
                    "GitHub did not allow this request (HTTP %ld). A VPN or a shared network can cause it. Try again later.", code);
    }
    if (code == 404)
        return fail(p, "not_found", "The update isn't on GitHub",
                    "The release or its download could not be found (HTTP 404). It may have just been replaced. Try again later.");
    if (code >= 500)
        return fail(p, "github_down", "GitHub is having problems",
                    "GitHub answered with an error of its own (HTTP %ld). Nothing is wrong on your side: try again in a few minutes.", code);
    return fail(p, "bad_response", "GitHub sent an unexpected answer", "Savage Wind received HTTP %ld. Try again later.", code);
}

static char *read_text(const char *path, size_t max, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    if (out_n) *out_n = n;
    return b;
}

static const char *feed_url(void) {
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_FEED")) return getenv("SW_UPDATE_TEST_FEED");
#endif
    return FEED_URL;
}

bool appimage_find_update(const char *work, const char *current, UcRelease *out, bool *available, UpdateProblem *p) {
    *available = false;
    char body[1100], headers[1100], code_f[1100];
    snprintf(body, sizeof body, "%s/update-feed.json", work);
    snprintf(headers, sizeof headers, "%s/update-feed.headers", work);
    snprintf(code_f, sizeof code_f, "%s/update-feed.code", work);
    char *const argv[] = {"curl", "-sS", "-L", "--connect-timeout", "15", "--max-time", "45", "-A", "SavageWind-SWUpdate",
                          "-H", "Accept: application/vnd.github+json", "-D", headers, "-o", body, "-w", "%{http_code}",
                          (char *)feed_url(), NULL};
    int rc = run(argv, code_f, 60, true, NULL, 0, 0, NULL, 0, 0);
    bool ok = false;
    if (rc) curl_problem(rc, p);
    else {
        char *c = read_text(code_f, 16, NULL);
        long code = c ? strtol(c, NULL, 10) : 0;
        free(c);
        if (code != 200) http_problem(code, headers, p);
        else {
            char *json = read_text(body, 8u << 20, NULL);
            int unsig = 0;
            UcPick pick = json ? uc_pick_release(json, current, ASSET_SUFFIX, out, &unsig) : UC_PICK_BAD_JSON;
            free(json);
            if (unsig) fprintf(stderr, "[update] %d newer release(s) have no signature: passed over\n", unsig);
            if (pick == UC_PICK_BAD_JSON)
                fail(p, "bad_response", "GitHub sent an unexpected answer",
                     "The list of releases could not be read. A captive portal (hotel or airport Wi-Fi) can cause this. Try again later.");
            else {
                ok = true;
                *available = pick == UC_PICK_UPDATE;
#ifndef SW_UPDATE_TEST
                if (*available && (strncmp(out->zip_url, DOWNLOAD_PREFIX, sizeof DOWNLOAD_PREFIX - 1) ||
                                   strncmp(out->sig_url, DOWNLOAD_PREFIX, sizeof DOWNLOAD_PREFIX - 1))) {
                    *available = false;
                    ok = fail(p, "bad_response", "GitHub sent an unexpected answer",
                              "The download address is not where Savage Wind's releases are. Nothing was downloaded.");
                }
#endif
            }
        }
    }
    remove(body), remove(headers), remove(code_f);
    return ok;
}

static void hex32(const uint8_t b[32], char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[64] = 0;
}

static bool signature_ok(const uint8_t hash[32], const uint8_t sig[64]) {
#ifdef SW_UPDATE_TEST_PUBKEY
    uint8_t key[64];
    return uc_hex_decode(SW_UPDATE_TEST_PUBKEY, key, 64) && uc_p256_verify(hash, sig, key);
#else
    return uc_p256_verify(hash, sig, SW_UPDATE_PUBKEY);
#endif
}

static uint64_t file_size(const char *p) {
    struct stat s;
    return stat(p, &s) == 0 ? (uint64_t)s.st_size : 0;
}

static bool folder_of(const char *file, char *out, size_t n) {
    snprintf(out, n, "%s", file);
    char *s = strrchr(out, '/');
    if (!s) return false;
    if (s == out) s[1] = 0;
    else *s = 0;
    return true;
}

// The new file complete at part. false: p says why (what was downloaded stays, unless it's bad).
static bool download(const UcRelease *r, const char *part, volatile float *progress, UpdateProblem *p) {
    for (int attempt = 1;; attempt++) {
        uint64_t have = file_size(part);
        if (have > r->zip_size) remove(part), have = 0;
        if (have == r->zip_size) return true;
        char *const argv[] = {"curl", "-sS", "-L", "--fail", "-C", "-", "--connect-timeout", "15", "--speed-limit", "1024",
                              "--speed-time", "45", "-A", "SavageWind-SWUpdate", "-o", (char *)part, (char *)r->zip_url, NULL};
        int rc = run(argv, NULL, 0, true, part, 0, r->zip_size, progress, 0.0f, 0.8f);
        if (!rc && file_size(part) == r->zip_size) return true;
        if (rc == 33) {  // (the server can't continue from there: from the start)
            remove(part);
            continue;
        }
        if (rc == 22) return http_problem(404, "/nonexistent", p);
        bool passing = !rc || rc == 7 || rc == 18 || rc == 28 || rc == 52 || rc == 55 || rc == 56 || rc == -1;
        if (!passing) return curl_problem(rc, p);
        fprintf(stderr, "[update] download attempt %d stopped (curl %d)\n", attempt, rc);
        if (attempt >= 4)
            return fail(p, "download_interrupted", "The download keeps getting cut off",
                        "The connection dropped several times before the download finished. What was downloaded is kept: "
                        "try again to continue from there.");
        sleep(2);
    }
}

bool appimage_install(const char *work, const UcRelease *r, volatile float *progress, UpdateProblem *p) {
    const char *self = appimage_path();
    if (!self) return fail(p, "not_appimage", "This copy can't update itself", "Only the AppImage releases can update themselves.");
    char dir[1100], part[1200], sigf[1300], old[1200];
    if (!folder_of(self, dir, sizeof dir))
        return fail(p, "wrong_folder", "The AppImage's location isn't known", "Get the new AppImage from the project's GitHub releases page.");
    snprintf(part, sizeof part, "%s.part", self);
    snprintf(sigf, sizeof sigf, "%s/update.sig", work);
    snprintf(old, sizeof old, "%s.old", self);
    if (access(dir, W_OK))
        return fail(p, "read_only", "Savage Wind can't write in this folder",
                    "The system does not allow writing in:\n%s\n\nMove the AppImage somewhere in your home folder and try again.", dir);
    struct statvfs vfs;
    if (statvfs(dir, &vfs) == 0) {
        unsigned long long have = (unsigned long long)vfs.f_bavail * vfs.f_frsize, need = r->zip_size * 2 + (1ull << 28);
#ifdef SW_UPDATE_TEST
        if (getenv("SW_UPDATE_TEST_FREE_MB")) have = strtoull(getenv("SW_UPDATE_TEST_FREE_MB"), NULL, 10) << 20;
#endif
        if (have < need)
            return fail(p, "disk_full", "Not enough free disk space",
                        "The update needs about %.1f GB free on this disk and there are %.1f GB. Nothing was changed. Free some space and try again.",
                        need / 1073741824.0, have / 1073741824.0);
    }
    bool from_earlier = file_size(part) > 0;
    uint8_t hash[32];
    char hex[65];
    for (;;) {
        *progress = 0;
        if (!download(r, part, progress, p)) return false;
        *progress = 0.8f;
        if (!uc_sha256_file(part, hash)) {
            remove(part);
            return fail(p, "file_error", "The download could not be read", "The downloaded file disappeared or can't be opened. Try again.");
        }
        hex32(hash, hex);
        bool bad = file_size(part) != r->zip_size || (r->digest[0] && strcmp(hex, r->digest));
        if (!bad) break;
        remove(part);
        if (!from_earlier)
            return fail(p, "corrupt_download", "The download arrived damaged",
                        "The file does not match what GitHub says it should be, so it was deleted and nothing was installed. Try again to download it again.");
        fprintf(stderr, "[update] what was kept from an earlier download doesn't match the release: downloading it again\n");
        from_earlier = false;
    }
    *progress = 0.85f;
    char *const sig_argv[] = {"curl", "-sS", "-L", "--fail", "--connect-timeout", "15", "--max-time", "45", "-A", "SavageWind-SWUpdate",
                              "-o", sigf, (char *)r->sig_url, NULL};
    int rc = run(sig_argv, NULL, 60, true, NULL, 0, 0, NULL, 0, 0);
    if (rc) {
        remove(sigf);
        return rc == 22 ? http_problem(404, "/nonexistent", p) : curl_problem(rc, p);
    }
    char *sig_hex = read_text(sigf, 4096, NULL);
    remove(sigf);
    uint8_t sig[64];
    bool sig_read = false;
    if (sig_hex) {
        sig_hex[strcspn(sig_hex, "\r\n ")] = 0;
        sig_read = uc_hex_decode(sig_hex, sig, 64);
    }
    free(sig_hex);
    if (!sig_read || !signature_ok(hash, sig)) {
        remove(part);
        return fail(p, "bad_signature", "This update did not pass the security check",
                    "The download is not signed by the Savage Wind team, so it was deleted and NOT installed. Do not look for "
                    "this file elsewhere. Please report this on the project's GitHub page.");
    }
    fprintf(stderr, "[update] signature ok, sha256 %s\n", hex);

    *progress = 0.9f;
    if (chmod(part, 0755)) {
        remove(part);
        return fail(p, "file_error", "The new version could not be made runnable", "Reason: %s.", strerror(errno));
    }
    char *const health[] = {part, "--update-health", NULL};
    rc = run(health, NULL, 90, false, NULL, 0, 0, NULL, 0, 0);
    fprintf(stderr, "[update] health check: exit %d\n", rc);
#ifdef SW_UPDATE_TEST
    if (getenv("SW_UPDATE_TEST_SKIP_HEALTH")) rc = HEALTH_EXIT;
#endif
    if (rc != HEALTH_EXIT) {
        remove(part);
        return fail(p, "health_failed", "The new version did not start correctly",
                    "The new AppImage stopped with code %d, so it was not installed and your version was left as it was. If "
                    "this folder is on a drive mounted noexec, move the AppImage to your home folder. Otherwise please report this "
                    "on the project's GitHub page.", rc);
    }
    remove(old);
    if (link(self, old) == 0) {  // (the new file in place in one step: there is never a moment without an AppImage)
        if (rename(part, self)) {
            int e = errno;
            remove(old);
            remove(part);
            return fail(p, "file_error", "The AppImage could not be replaced", "Reason: %s. Your version was left as it was.", strerror(e));
        }
    } else {  // (a file system without hard links)
        if (rename(self, old)) {
            int e = errno;
            remove(part);
            return fail(p, "file_error", "The AppImage could not be replaced", "Reason: %s. Your version was left as it was.", strerror(e));
        }
        if (rename(part, self)) {
            int e = errno;
            rename(old, self);
            remove(part);
            return fail(p, "file_error", "The AppImage could not be replaced", "Reason: %s. Your version was left as it was.", strerror(e));
        }
    }
    *progress = 1;
    fprintf(stderr, "[update] installed %s\n", r->version);
    return true;
}

bool appimage_has_old(void) {
    const char *self = appimage_path();
    char old[1200];
    if (!self) return false;
    snprintf(old, sizeof old, "%s.old", self);
    return access(old, F_OK) == 0;
}

void appimage_remove_old(void) {
    const char *self = appimage_path();
    char old[1200];
    if (!self) return;
    snprintf(old, sizeof old, "%s.old", self);
    remove(old);
}

bool appimage_go_back(UpdateProblem *p) {
    const char *self = appimage_path();
    char old[1200];
    if (!self) return fail(p, "not_appimage", "This copy can't update itself", "Only the AppImage releases can update themselves.");
    snprintf(old, sizeof old, "%s.old", self);
    if (access(old, F_OK))
        return fail(p, "no_backup", "There is no earlier version to go back to",
                    "The previous version is only kept until an update finishes setting up.");
    if (rename(old, self)) return fail(p, "file_error", "The previous version could not be put back", "Reason: %s.", strerror(errno));
    return true;
}

void appimage_relaunch(UpdateProblem *p) {
    const char *self = appimage_path();
    if (!self) {
        fail(p, "not_appimage", "This copy can't update itself", "Only the AppImage releases can update themselves.");
        return;
    }
    char path[1100];
    snprintf(path, sizeof path, "%s", self);
    char **env = child_env(false);
    char *const argv[] = {path, "--config", NULL};
    fprintf(stderr, "[update] starting the new version\n");
    fflush(NULL);
    if (env) execve(path, argv, env);
    int e = errno;
    free(env);
    fail(p, "relaunch", "The new version is installed but could not be started",
         "Reason: %s. Close this window and start Savage Wind again.", strerror(e));
}
#endif
