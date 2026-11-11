// Savage Wind in the Steam library (runtime/steam_shortcuts.h).
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "steam_shortcuts.h"

#include <ctype.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ the binary VDF of shortcuts.vdf
// A map is a list of (type byte, NUL-terminated key, value), ended by 0x08. Types: 0x00 map, 0x01 string, 0x02 int32,
// 0x03 float32, 0x06 colour, 0x07 / 0x0A 64-bit. Read into a tree and written back byte for byte.
typedef struct Node {
    uint8_t type;
    char *key;
    uint8_t *val;  // (strings with their NUL)
    size_t len;
    struct Node *kids;
    int nkids, cap;
} Node;

static void node_free(Node *n) {
    for (int i = 0; i < n->nkids; i++) node_free(&n->kids[i]);
    free(n->kids);
    free(n->key);
    free(n->val);
}

static Node *node_add(Node *m) {
    if (m->nkids == m->cap) {
        int c = m->cap ? m->cap * 2 : 8;
        Node *k = (Node *)realloc(m->kids, (size_t)c * sizeof *k);
        if (!k) return NULL;
        m->kids = k, m->cap = c;
    }
    Node *n = &m->kids[m->nkids++];
    memset(n, 0, sizeof *n);
    return n;
}

static bool parse_map(const uint8_t *d, size_t n, size_t *at, Node *m, int depth) {
    if (depth > 16) return false;
    for (;;) {
        if (*at >= n) return false;
        uint8_t t = d[(*at)++];
        if (t == 0x08) return true;
        const uint8_t *k = d + *at, *z = (const uint8_t *)memchr(k, 0, n - *at);
        if (!z) return false;
        Node *c = node_add(m);
        if (!c) return false;
        c->type = t;
        c->key = strdup((const char *)k);
        *at += (size_t)(z - k) + 1;
        size_t len;
        switch (t) {
            case 0x00:
                if (!parse_map(d, n, at, c, depth + 1)) return false;
                continue;
            case 0x01: {
                const uint8_t *e = (const uint8_t *)memchr(d + *at, 0, n - *at);
                if (!e) return false;
                len = (size_t)(e - (d + *at)) + 1;
                break;
            }
            case 0x02: case 0x03: case 0x04: case 0x06: len = 4; break;
            case 0x07: case 0x0A: len = 8; break;
            default: return false;
        }
        if (*at + len > n || !(c->val = (uint8_t *)malloc(len))) return false;
        memcpy(c->val, d + *at, len);
        c->len = len;
        *at += len;
    }
}

typedef struct { uint8_t *p; size_t n, cap; bool bad; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *q = (uint8_t *)realloc(b->p, c);
        if (!q) { b->bad = true; return; }
        b->p = q, b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void write_map(Buf *b, const Node *m) {
    for (int i = 0; i < m->nkids; i++) {
        const Node *c = &m->kids[i];
        put(b, &c->type, 1);
        put(b, c->key, strlen(c->key) + 1);
        if (c->type == 0x00) write_map(b, c);
        else put(b, c->val, c->len);
    }
    static const uint8_t end = 0x08;
    put(b, &end, 1);
}

static Node *find(Node *m, const char *key) {  // (Steam has written both "AppName" and "appname")
    for (int i = 0; i < m->nkids; i++)
        if (!strcasecmp(m->kids[i].key, key)) return &m->kids[i];
    return NULL;
}

static bool set_string(Node *m, const char *key, const char *v) {
    Node *c = find(m, key);
    if (!c && !(c = node_add(m))) return false;
    if (!c->key) c->key = strdup(key);
    c->type = 0x01;
    free(c->val);
    c->len = strlen(v) + 1;
    c->val = (uint8_t *)malloc(c->len);
    if (!c->val || !c->key) return false;
    memcpy(c->val, v, c->len);
    return true;
}
static bool set_int(Node *m, const char *key, uint32_t v) {
    Node *c = find(m, key);
    if (!c && !(c = node_add(m))) return false;
    if (!c->key) c->key = strdup(key);
    c->type = 0x02;
    free(c->val);
    c->len = 4;
    c->val = (uint8_t *)malloc(4);
    if (!c->val || !c->key) return false;
    for (int i = 0; i < 4; i++) c->val[i] = (uint8_t)(v >> (i * 8));
    return true;
}

uint32_t steam_shortcut_appid(const char *exe, const char *name) {
    uint32_t c = 0xFFFFFFFFu;
    for (int s = 0; s < 2; s++)
        for (const unsigned char *p = (const unsigned char *)(s ? name : exe); *p; p++) {
            c ^= *p;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    return ~c | 0x80000000u;
}

static bool parse_root(const uint8_t *in, size_t n, Node *root, char *why, size_t ws) {
    memset(root, 0, sizeof *root);
    if (!n) return true;
    size_t at = 0;
    if (!parse_map(in, n, &at, root, 0) || at != n) {
        snprintf(why, ws, "its shortcuts.vdf has a format this version doesn't know");
        return false;
    }
    return true;
}

int steam_vdf_count(const uint8_t *in, size_t n) {
    Node root;
    char why[100];
    if (!parse_root(in, n, &root, why, sizeof why)) {
        node_free(&root);
        return -1;
    }
    Node *s = find(&root, "shortcuts");
    int c = s && s->type == 0x00 ? s->nkids : 0;
    node_free(&root);
    return c;
}

bool steam_vdf_upsert(const uint8_t *in, size_t n, const SteamEntry *e, int count, uint8_t **out, size_t *out_n,
                      char *why, size_t ws) {
    Node root;
    bool ok = parse_root(in, n, &root, why, ws);
    Node *list = ok ? find(&root, "shortcuts") : NULL;
    if (ok && !list) {
        if (root.nkids) {
            snprintf(why, ws, "its shortcuts.vdf has no list of shortcuts");
            ok = false;
        } else if ((list = node_add(&root))) {
            list->type = 0x00;
            list->key = strdup("shortcuts");
        }
    }
    if (ok && (!list || list->type != 0x00)) {
        snprintf(why, ws, "its shortcuts.vdf has no list of shortcuts");
        ok = false;
    }
    for (int i = 0; ok && i < count; i++) {
        Node *s = NULL;
        for (int k = 0; k < list->nkids && !s; k++) {
            Node *name = find(&list->kids[k], "AppName");
            if (name && name->type == 0x01 && !strcmp((const char *)name->val, e[i].name)) s = &list->kids[k];
        }
        bool fresh = !s;
        if (fresh) {
            char idx[16];
            snprintf(idx, sizeof idx, "%d", list->nkids);
            if (!(s = node_add(list))) { ok = false; break; }
            s->type = 0x00;
            s->key = strdup(idx);
        }
        uint32_t appid = steam_shortcut_appid(e[i].exe, e[i].name);
        ok = set_int(s, "appid", appid) && set_string(s, "AppName", e[i].name) && set_string(s, "Exe", e[i].exe) &&
             set_string(s, "StartDir", e[i].start_dir) && set_string(s, "icon", e[i].icon) &&
             set_string(s, "LaunchOptions", e[i].options);
        if (ok && fresh)
            ok = set_string(s, "ShortcutPath", "") && set_int(s, "IsHidden", 0) && set_int(s, "AllowDesktopConfig", 1) &&
                 set_int(s, "AllowOverlay", 1) && set_int(s, "OpenVR", 0) && set_int(s, "Devkit", 0) &&
                 set_string(s, "DevkitGameID", "") && set_int(s, "DevkitOverrideAppID", 0) && set_int(s, "LastPlayTime", 0) &&
                 set_string(s, "FlatpakAppID", "");
        if (ok && fresh) {
            Node *tags = node_add(s);
            if (!tags) ok = false;
            else tags->type = 0x00, tags->key = strdup("tags");
        }
        if (!ok) snprintf(why, ws, "out of memory");
    }
    if (ok) {
        Buf b = {0};
        write_map(&b, &root);
        if (b.bad) {
            free(b.p);
            snprintf(why, ws, "out of memory");
            ok = false;
        } else *out = b.p, *out_n = b.n;
    }
    node_free(&root);
    return ok;
}

// ------------------------------------------------------------------ this computer's Steam
#if !defined(STEAM_VDF_ONLY) && (defined(_WIN32) || defined(__linux__))  // (STEAM_VDF_ONLY: the file format alone, for its test)
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include "update_appimage.h"
extern char **environ;
#endif
#include "paths.h"

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = s >= 0 && s < (64 << 20) ? (uint8_t *)malloc((size_t)s + 1) : NULL;
    if (b && fread(b, 1, (size_t)s, f) != (size_t)s) free(b), b = NULL;
    fclose(f);
    if (b) *n = (size_t)s;
    return b;
}

// The Steam folders here (a test can point SAVAGEWIND_STEAM_ROOT at a copy)
static int steam_roots(char roots[][1100], int max) {
    int n = 0;
    if (getenv("SAVAGEWIND_STEAM_ROOT")) {
        snprintf(roots[n++], 1100, "%s", getenv("SAVAGEWIND_STEAM_ROOT"));
        return n;
    }
#ifdef _WIN32
    wchar_t w[1024];
    DWORD sz = sizeof w;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, w, &sz) == ERROR_SUCCESS &&
        WideCharToMultiByte(CP_UTF8, 0, w, -1, roots[n], 1100, NULL, NULL))
        n++;
#else
    const char *home = getenv("HOME");
    if (home) {
        static const char *const cands[] = {"%s/.local/share/Steam", "%s/.steam/steam",
                                            "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
        for (size_t i = 0; i < sizeof cands / sizeof *cands && n < max; i++) {
            char p[1100], r[1100];
            snprintf(p, sizeof p, cands[i], home);
            if (!realpath(p, r)) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= !strcmp(roots[k], r);
            if (!dup) snprintf(roots[n++], 1100, "%s", r);
        }
    }
#endif
    (void)max;
    return n;
}

#ifdef _WIN32
static bool steam_running(void) {
    if (getenv("SAVAGEWIND_STEAM_ROOT")) return getenv("SAVAGEWIND_TEST_STEAM_RUNNING") != NULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e = {sizeof e};
    bool found = false;
    if (Process32FirstW(snap, &e))
        do found |= !_wcsicmp(e.szExeFile, L"steam.exe");
        while (!found && Process32NextW(snap, &e));
    CloseHandle(snap);
    return found;
}
static void run_steam(const char *root, const char *args, bool wait) {
    char exe[1200];
    snprintf(exe, sizeof exe, "%s/steam.exe", root);
    wchar_t w[1200], wa[100];
    MultiByteToWideChar(CP_UTF8, 0, exe, -1, w, 1200);
    MultiByteToWideChar(CP_UTF8, 0, args, -1, wa, 100);
    SHELLEXECUTEINFOW s;
    memset(&s, 0, sizeof s);
    s.cbSize = sizeof s;
    s.fMask = SEE_MASK_NOCLOSEPROCESS;
    s.lpFile = w;
    s.lpParameters = wa;
    s.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&s) && s.hProcess) {
        if (wait) WaitForSingleObject(s.hProcess, 30000);
        CloseHandle(s.hProcess);
    }
}
#else
static bool steam_running(void) {
    if (getenv("SAVAGEWIND_STEAM_ROOT")) return getenv("SAVAGEWIND_TEST_STEAM_RUNNING") != NULL;
    const char *home = getenv("HOME");
    char p[1100];
    static const char *const pids[] = {"%s/.steam/steam.pid", "%s/.var/app/com.valvesoftware.Steam/.steam/steam.pid"};
    for (size_t i = 0; home && i < sizeof pids / sizeof *pids; i++) {
        snprintf(p, sizeof p, pids[i], home);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        long pid = 0;
        if (fscanf(f, "%ld", &pid) != 1) pid = 0;
        fclose(f);
        char comm[64] = "", cp[64];
        snprintf(cp, sizeof cp, "/proc/%ld/comm", pid);
        FILE *c = pid > 0 ? fopen(cp, "r") : NULL;
        if (c) {
            if (!fgets(comm, sizeof comm, c)) comm[0] = 0;
            fclose(c);
        }
        if (strstr(comm, "steam")) return true;
    }
    return false;
}
// The system's steam, without what the AppImage's runtime or a launcher put in the environment
static void run_steam(const char *root, const char *args, bool wait) {
    bool flatpak = strstr(root, "com.valvesoftware.Steam") != NULL;
    pid_t pid = fork();
    if (pid < 0) return;
    if (!pid) {
        if (!wait) {
            setsid();
            if (fork()) _exit(0);  // (not our child: it outlives this program)
        }
        static const char *const drop[] = {"APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH", "LD_PRELOAD"};
        for (size_t i = 0; i < sizeof drop / sizeof *drop; i++) unsetenv(drop[i]);
        if (flatpak) execlp("flatpak", "flatpak", "run", "com.valvesoftware.Steam", args, (char *)NULL);
        else execlp("steam", "steam", args, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}
#endif

static void unsetenv_running(void) {
#ifdef _WIN32
    _putenv("SAVAGEWIND_TEST_STEAM_RUNNING=");
#else
    unsetenv("SAVAGEWIND_TEST_STEAM_RUNNING");
#endif
}

bool steam_supported(void) {
#ifdef _WIN32
    char p[1100];
    return rt_resource("sw-manifest.txt", NULL, p, sizeof p);
#else
    return appimage_path() != NULL;
#endif
}

// This copy's two entries
static int our_entries(SteamEntry *e, char bufs[][1200]) {
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (!n || n >= 1024) return 0;
    char exe[1100];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, exe, sizeof exe, NULL, NULL);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", exe);
    char *s = strrchr(dir, '\\');
    if (!s) return 0;
    *s = 0;
    snprintf(bufs[0], 1200, "\"%s\"", exe);
    snprintf(bufs[1], 1200, "\"%s\\Savage Wind Configuration.exe\"", dir);
    snprintf(bufs[2], 1200, "\"%s\\\"", dir);
    snprintf(bufs[3], 1200, "%s", exe);
    e[0] = (SteamEntry){"Savage Wind", bufs[0], bufs[2], bufs[3], "--game"};
    e[1] = (SteamEntry){"Savage Wind Configuration", bufs[1], bufs[2], bufs[3], ""};
#else
    const char *ai = appimage_path();
    if (!ai) return 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", ai);
    char *s = strrchr(dir, '/');
    if (!s) return 0;
    *s = 0;
    // (the icon is inside the AppImage, gone once it closes: a copy in the data folder)
    char src[1100];
    bufs[3][0] = 0;
    if (rt_resource("icon.png", NULL, src, sizeof src)) {
        snprintf(bufs[3], 1200, "%s/steam-icon.png", rt_data_root());
        size_t n = 0;
        uint8_t *png = slurp(src, &n);
        FILE *f = png ? fopen(bufs[3], "wb") : NULL;
        if (f) fwrite(png, 1, n, f), fclose(f);
        else bufs[3][0] = 0;
        free(png);
    }
    snprintf(bufs[0], 1200, "\"%s\"", ai);
    snprintf(bufs[2], 1200, "\"%s/\"", dir);
    e[0] = (SteamEntry){"Savage Wind", bufs[0], bufs[2], bufs[3], "--game"};
    e[1] = (SteamEntry){"Savage Wind Configuration", bufs[0], bufs[2], bufs[3], "--config"};
#endif
    return 2;
}

// Each signed-in user's config/shortcuts.vdf, written. false: why.
static bool write_all(char roots[][1100], int nroots, const SteamEntry *e, int ne, int *users, char *why, size_t ws) {
    *users = 0;
    for (int r = 0; r < nroots; r++) {
        char ud[1200];
        snprintf(ud, sizeof ud, "%s/userdata", roots[r]);
        DIR *d = opendir(ud);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *id = de->d_name;
            bool digits = *id && strspn(id, "0123456789") == strlen(id);
            if (!digits || !strcmp(id, "0")) continue;
            char cfg[1300], file[1400], tmp[1450], bak[1450];
            snprintf(cfg, sizeof cfg, "%s/%s/config", ud, id);
            if (!exists(cfg)) continue;
            snprintf(file, sizeof file, "%s/shortcuts.vdf", cfg);
            snprintf(tmp, sizeof tmp, "%s.savagewind-tmp", file);
            snprintf(bak, sizeof bak, "%s.savagewind-backup", file);
            size_t n = 0;
            uint8_t *in = exists(file) ? slurp(file, &n) : NULL;
            if (exists(file) && !in) {
                closedir(d);
                snprintf(why, ws, "Steam's list of shortcuts couldn't be read (%s)", file);
                return false;
            }
            uint8_t *out = NULL;
            size_t on = 0;
            char w2[200] = "";
            bool ok = steam_vdf_upsert(in, n, e, ne, &out, &on, w2, sizeof w2);
            if (ok && steam_vdf_count(out, on) < 0) ok = false, snprintf(w2, sizeof w2, "the result didn't read back");
            if (ok) {
                FILE *f = fopen(tmp, "wb");
                ok = f && fwrite(out, 1, on, f) == on;
                if (f && fclose(f)) ok = false;
                if (ok && in) {
                    FILE *b = fopen(bak, "wb");
                    ok = b && fwrite(in, 1, n, b) == n;
                    if (b && fclose(b)) ok = false;
                }
#ifdef _WIN32
                if (ok) {
                    wchar_t wt[1450], wf[1450];
                    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wt, 1450);
                    MultiByteToWideChar(CP_UTF8, 0, file, -1, wf, 1450);
                    ok = MoveFileExW(wt, wf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                }
#else
                if (ok) ok = rename(tmp, file) == 0;
#endif
                if (!ok) {
                    remove(tmp);
                    snprintf(w2, sizeof w2, "it couldn't be written");
                }
            }
            free(in);
            free(out);
            if (!ok) {
                closedir(d);
                snprintf(why, ws, "Steam's list of shortcuts was left as it was: %s (%s)", w2, file);
                return false;
            }
            fprintf(stderr, "[steam] %s: Savage Wind entries written\n", file);
            ++*users;
        }
        closedir(d);
    }
    if (!*users) {
        snprintf(why, ws, "No Steam account was found on this computer. Open Steam and sign in once, then try again.");
        return false;
    }
    return true;
}

bool steam_add_savage_wind(char *msg, size_t ms) {
    char roots[4][1100];
    int nroots = steam_roots(roots, 4);
    if (!nroots) {
        snprintf(msg, ms, "Steam isn't installed on this computer.");
        return false;
    }
    if (getenv("SteamAppId") || getenv("SteamGameId")) {  // (closing Steam would close this window too)
        snprintf(msg, ms, "Savage Wind was started from Steam. Close it and open Savage Wind Configuration outside Steam to add it.");
        return false;
    }
#ifndef _WIN32
    const char *desk = getenv("XDG_CURRENT_DESKTOP");
    if ((desk && strstr(desk, "gamescope")) || getenv("SteamGamepadUI") || getenv("GAMESCOPE_WAYLAND_DISPLAY")) {
        snprintf(msg, ms, "In Gaming Mode Steam can't be closed for a moment. Switch to Desktop Mode, then add it from there.");
        return false;
    }
#endif
    SteamEntry e[2];
    char bufs[4][1200];
    if (our_entries(e, bufs) != 2) {
        snprintf(msg, ms, "This copy of Savage Wind can't tell where it is installed.");
        return false;
    }
    bool test = getenv("SAVAGEWIND_STEAM_ROOT") != NULL;  // (a copy: the real Steam is left alone)
    bool was_running = steam_running();
    if (was_running) {
        fprintf(stderr, "[steam] closing Steam\n");
        for (int r = 0; !test && r < nroots; r++) run_steam(roots[r], "-shutdown", true);
        if (test) unsetenv_running();
        time_t t0 = time(NULL);
        while (steam_running() && time(NULL) - t0 < 60) {
#ifdef _WIN32
            Sleep(500);
#else
            usleep(500000);
#endif
        }
        if (steam_running()) {
            snprintf(msg, ms, "Steam didn't close. Close it yourself (Steam > Exit) and try again.");
            return false;
        }
#ifdef _WIN32
        Sleep(2000);  // (its last write of the file, after the process list says it's gone)
#else
        sleep(2);
#endif
    }
    int users = 0;
    char why[600];
    bool ok = write_all(roots, nroots, e, 2, &users, why, sizeof why);
    if (was_running && !test) run_steam(roots[0], "", false);
    if (!ok) {
        snprintf(msg, ms, "%s", why);
        return false;
    }
    snprintf(msg, ms, "Added to Steam: \"Savage Wind\" (straight into the game) and \"Savage Wind Configuration\".");
    return true;
}

#elif !defined(STEAM_VDF_ONLY)
bool steam_add_savage_wind(char *msg, size_t ms) {
    snprintf(msg, ms, "Not available here.");
    return false;
}
bool steam_supported(void) { return false; }
#endif
