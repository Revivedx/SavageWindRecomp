// What the game does around SW Update (runtime/post_update.h).
#include "post_update.h"

#if defined(_WIN32) || defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "update_appimage.h"
#endif

void vk_cache_path(char *out, size_t n, const char *name);  // (runtime/nvn/vulkan/shaders.h)
#ifdef _WIN32
#define MAKE_DIR(p) mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0755)
#endif
#include "paths.h"

extern const char *rt_version(void);  // (runtime/ui/launcher.mm)

static bool file_there(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    out[k] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

static void write_line(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

static void cache_file(char *out, size_t n, const char *name) { vk_cache_path(out, n, name); }

static void remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
            else {
#ifdef _WIN32
                SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
#endif
                remove(p);
            }
        }
        closedir(d);
    }
    rmdir(dir);
}

#ifdef _WIN32
// update/journal.txt (runtime/platform/windows_update.c) there and not ending in "C": the swap didn't finish
static bool journal_cut_off(const wchar_t *journal) {
    FILE *f = _wfopen(journal, L"rb");
    if (!f) return false;
    char tail[3] = {0};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, n >= 3 ? n - 3 : 0, SEEK_SET);
    size_t k = fread(tail, 1, 3, f);
    fclose(f);
    if (k >= 2 && tail[k - 2] == 'C' && tail[k - 1] == '\n' && (k == 2 || tail[0] == '\n')) return false;
    return true;
}

bool post_update_recover_if_cut(void) {
    wchar_t self[1200], dir[1200], journal[1300], tool[1300], line[1400];
    DWORD n = GetModuleFileNameW(NULL, self, 1200);
    if (!n || n >= 1200) return false;
    wcscpy(dir, self);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    *slash = 0;
    swprintf(journal, 1300, L"%ls\\update\\journal.txt", dir);
    if (!journal_cut_off(journal)) return false;
    swprintf(tool, 1300, L"%ls\\SW Update.exe", dir);
    swprintf(line, 1400, L"\"%ls\" --recover", tool);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(tool, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (journal_cut_off(journal)) return false;  // (it couldn't: SW Update has said so, and this must not loop)
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(self, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else
bool post_update_recover_if_cut(void) { return false; }  // (the AppImage is replaced in one step)
#endif

bool post_update_shipped(void) {
#ifdef _WIN32
    char p[1100];
    return rt_resource("sw-manifest.txt", NULL, p, sizeof p);
#else
    return appimage_path() != NULL;
#endif
}

bool post_update_pending(const char *version) {
    if (!post_update_shipped() || !file_there("extracted/exefs/main")) return false;
    char p[1100], last[80] = "";
    cache_file(p, sizeof p, "last-version.txt");
    return !read_line(p, last, sizeof last) || strcmp(last, version) != 0;
}

#ifdef _WIN32
// What is in a folder of the driver's, for the log: which folders a driver really uses here varies between versions
static void log_folder_names(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[update] %s: not there\n", dir);
        return;
    }
    char names[800] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t k = strlen(names);
        snprintf(names + k, sizeof names - k, "%s%s", k ? ", " : "", e->d_name);
    }
    closedir(d);
    fprintf(stderr, "[update] %s: %s\n", dir, names);
}

// Everything inside dir, the folder itself stays. Never through a link. Files in use stay until the next time.
static void clear_folder(const char *dir, int *removed, int *kept) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (rt_is_link(p) || stat(p, &st) != 0) ++*kept;
        else if (S_ISDIR(st.st_mode)) {
            clear_folder(p, removed, kept);
            rmdir(p);
        } else {
            SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
            if (remove(p) == 0) ++*removed;
            else ++*kept;
        }
    }
    closedir(d);
}

// AMD's Windows driver keeps a Vulkan shader cache of its own, apart from Savage Wind's: emptied with them, as Adrenalin's
// Reset Shader Cache does. Only the Vulkan one (DirectX games keep theirs); the driver makes it again as games run. No
// other driver has this folder.
static void clear_amd_vulkan_cache(void) {
    const char *local = getenv("LOCALAPPDATA"), *profile = getenv("USERPROFILE");
    if (!local || !*local) return;
    char amd[1100], vk[1200], low[1100];
    snprintf(amd, sizeof amd, "%s/AMD", local);
    if (!file_there(amd)) return;  // (not an AMD PC)
    log_folder_names(amd);
    if (profile && *profile) {
        snprintf(low, sizeof low, "%s/AppData/LocalLow/AMD", profile);
        if (file_there(low)) log_folder_names(low);
    }
    snprintf(vk, sizeof vk, "%s/VkCache", amd);
    if (!file_there(vk)) {
        fprintf(stderr, "[update] AMD Vulkan cache: no VkCache folder here\n");
        return;
    }
    if (rt_is_link(amd) || rt_is_link(vk)) {
        fprintf(stderr, "[update] AMD Vulkan cache: a link, left alone\n");
        return;
    }
    int removed = 0, kept = 0;
    clear_folder(vk, &removed, &kept);
    fprintf(stderr, "[update] AMD Vulkan cache: %d files removed, %d left\n", removed, kept);
}

#endif

void post_update_reset_shaders(const char *version) {
    if (!post_update_pending(version)) return;
    char p[1100], done[80] = "";
    cache_file(p, sizeof p, "shaders-reset-version.txt");
    if (read_line(p, done, sizeof done) && !strcmp(done, version)) return;  // (an update cut off after this: keep what's made)
    // Only what is derived from the shaders. Not vk_shader_keys.list or vk_pipelines.list: those record what this
    // player's game has drawn, are what the precompilation works from, and nothing can make them again but playing.
    static const char *const dirs[] = {"xlate", "spirv"};
    static const char *const files[] = {"vk_pipelines.bin", "vk_precompiled.bin", "precompiled.txt"};
    char q[1100];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        cache_file(q, sizeof q, dirs[i]);
        remove_tree(q);
        MAKE_DIR(q);
    }
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        cache_file(q, sizeof q, files[i]);
        remove(q);
    }
    cache_file(q, sizeof q, "");
    MAKE_DIR(q);
#ifdef _WIN32
    clear_amd_vulkan_cache();
#endif
    write_line(p, version);
    fprintf(stderr, "[update] new version %s: shader caches emptied\n", version);
}

#ifdef _WIN32
static bool update_dir(char *out, size_t n) {
    char m[1100];
    if (!rt_resource("sw-manifest.txt", NULL, m, sizeof m)) return false;
    char *s = strrchr(m, '/'), *b = strrchr(m, '\\');
    if (b > s) s = b;
    if (!s) return false;
    *s = 0;
    snprintf(out, n, "%s/update", m);
    return true;
}
#endif

void post_update_finish(const char *version) {
    if (!post_update_shipped()) return;
    char p[1100];
    cache_file(p, sizeof p, "");
    MAKE_DIR(p);
    cache_file(p, sizeof p, "last-version.txt");
    write_line(p, version);
    cache_file(p, sizeof p, "update-failed.txt");
    remove(p);
#ifdef _WIN32
    if (update_dir(p, sizeof p)) remove_tree(p);
#else
    appimage_remove_old();
#endif
    fprintf(stderr, "[update] version %s is set up\n", version);
}

void post_update_tidy(const char *version) {
    char q[1100], last[80] = "";
    if (!post_update_shipped()) return;
    cache_file(q, sizeof q, "last-version.txt");
    if (!read_line(q, last, sizeof last) || strcmp(last, version)) return;
#ifdef _WIN32
    char p[1100];
    if (update_dir(p, sizeof p) && file_there(p)) remove_tree(p);
#else
    appimage_remove_old();
#endif
}

void post_update_mark_failed(const char *version) {
    if (!post_update_shipped()) return;
    char p[1100];
    cache_file(p, sizeof p, "update-failed.txt");
    write_line(p, version);
}

bool post_update_failed_before(const char *version) {
    char p[1100], v[80] = "";
    cache_file(p, sizeof p, "update-failed.txt");
    return read_line(p, v, sizeof v) && !strcmp(v, version);
}

bool post_update_can_rollback(void) {
#ifdef _WIN32
    char p[1100], q[1200];
    if (!update_dir(p, sizeof p)) return false;
    snprintf(q, sizeof q, "%s/journal.txt", p);
    return file_there(q);
#else
    return appimage_has_old();
#endif
}

// ------------------------------------------------------------------ asking SW Update
static pthread_mutex_t g_check_lock = PTHREAD_MUTEX_INITIALIZER;
static UpdateState g_state = UPDATE_IDLE;
static char g_version[64], g_message[512];

#ifdef _WIN32
static bool start_tool(const char *args, HANDLE *out_process, DWORD flags) {
    char exe[1100];
    if (!rt_resource("SW Update.exe", NULL, exe, sizeof exe)) return false;
    wchar_t wexe[1200], line[1800], wargs[500];
    if (!MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, 1200) || !MultiByteToWideChar(CP_UTF8, 0, args, -1, wargs, 500)) return false;
    swprintf(line, 1800, L"\"%ls\" %ls", wexe, wargs);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    AllowSetForegroundWindow(ASFW_ANY);
    if (!CreateProcessW(wexe, line, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    if (out_process) *out_process = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return true;
}

static void *check_thread(void *arg) {
    (void)arg;
    char out[1100], args[1300];
    snprintf(out, sizeof out, "%s/update-check.txt", rt_data_root());
    remove(out);
    snprintf(args, sizeof args, "--check --out \"%s\"", out);
    HANDLE p = NULL;
    UpdateState st = UPDATE_ERROR;
    char version[64] = "", message[512] = "";
    if (!start_tool(args, &p, CREATE_NO_WINDOW)) snprintf(message, sizeof message, "SW Update is missing from this folder");
    else {
        if (WaitForSingleObject(p, 45000) != WAIT_OBJECT_0) {
            TerminateProcess(p, 1);
            snprintf(message, sizeof message, "GitHub took too long to answer");
        }
        CloseHandle(p);
        if (!message[0]) {
            FILE *f = fopen(out, "rb");
            char line[1600];
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!strcmp(line, "status=available")) st = UPDATE_AVAILABLE;
                else if (!strcmp(line, "status=current")) st = UPDATE_CURRENT;
                else if (!strncmp(line, "version=", 8)) snprintf(version, sizeof version, "%s", line + 8);
                else if (!strncmp(line, "title=", 6)) snprintf(message, sizeof message, "%s", line + 6);
            }
            if (f) fclose(f);
            else snprintf(message, sizeof message, "SW Update did not answer");
            remove(out);
        }
    }
    fprintf(stderr, "[update] check: state %d %s %s\n", (int)st, version, message);
    pthread_mutex_lock(&g_check_lock);
    g_state = st;
    snprintf(g_version, sizeof g_version, "%s", version);
    snprintf(g_message, sizeof g_message, "%s", message);
    pthread_mutex_unlock(&g_check_lock);
    return NULL;
}

#else  // the AppImage asks by itself, with the system's curl
static UcRelease g_release;
static volatile float g_install_progress = -1;

static void *check_thread(void *arg) {
    (void)arg;
    char work[1100];
    cache_file(work, sizeof work, "");
    MAKE_DIR(work);
    UcRelease r;
    UpdateProblem pb = {{0}};
    bool available = false, ok = appimage_find_update(work, rt_version(), &r, &available, &pb);
    pthread_mutex_lock(&g_check_lock);
    g_state = !ok ? UPDATE_ERROR : available ? UPDATE_AVAILABLE : UPDATE_CURRENT;
    if (available) {
        g_release = r;
        snprintf(g_version, sizeof g_version, "%s", r.version);
    }
    if (!ok) snprintf(g_message, sizeof g_message, "%s", pb.title);
    pthread_mutex_unlock(&g_check_lock);
    fprintf(stderr, "[update] check: state %d %s %s\n", (int)g_state, g_version, g_message);
    return NULL;
}
#endif

void update_check_start(void) {
#ifdef _WIN32
    char exe[1100];
    if (!post_update_shipped() || !rt_resource("SW Update.exe", NULL, exe, sizeof exe)) return;
#else
    if (!post_update_shipped()) return;
#endif
    pthread_mutex_lock(&g_check_lock);
    bool go = g_state == UPDATE_IDLE;
    if (go) g_state = UPDATE_CHECKING;
    pthread_mutex_unlock(&g_check_lock);
    if (!go) return;
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) pthread_detach(t);
    else g_state = UPDATE_IDLE;
}

UpdateState update_check_state(char *version, size_t vn, char *message, size_t mn) {
    pthread_mutex_lock(&g_check_lock);
    UpdateState s = g_state;
    snprintf(version, vn, "%s", g_version);
    snprintf(message, mn, "%s", g_message);
    pthread_mutex_unlock(&g_check_lock);
    return s;
}

#ifdef _WIN32
bool update_applies_here(void) { return false; }
void update_apply_here(char *title, size_t tn, char *what, size_t wn) { (void)title, (void)tn, (void)what, (void)wn; }
float update_apply_progress(void) { return -1; }

bool update_apply_start(void) {
    char a[64];
    snprintf(a, sizeof a, "--apply --wait-pid %lu", GetCurrentProcessId());
    return start_tool(a, NULL, 0);
}

bool update_rollback_start(void) {
    char a[64];
    snprintf(a, sizeof a, "--rollback --wait-pid %lu", GetCurrentProcessId());
    return start_tool(a, NULL, 0);
}
#else
bool update_applies_here(void) { return true; }
float update_apply_progress(void) { return g_install_progress; }

void update_apply_here(char *title, size_t tn, char *what, size_t wn) {
    char work[1100];
    cache_file(work, sizeof work, "");
    MAKE_DIR(work);
    UcRelease r;
    pthread_mutex_lock(&g_check_lock);
    r = g_release;
    pthread_mutex_unlock(&g_check_lock);
    UpdateProblem pb = {{0}};
    g_install_progress = 0;
    if (appimage_install(work, &r, &g_install_progress, &pb)) appimage_relaunch(&pb);  // (returns only if it couldn't)
    g_install_progress = -1;
    snprintf(title, tn, "%s", pb.title);
    snprintf(what, wn, "%s", pb.what);
}

bool update_apply_start(void) { return false; }

bool update_rollback_start(void) {
    UpdateProblem pb = {{0}};
    if (appimage_go_back(&pb)) appimage_relaunch(&pb);
    fprintf(stderr, "[update] going back: %s\n", pb.title);
    return false;
}
#endif

#else  // neither

bool post_update_recover_if_cut(void) { return false; }
bool post_update_shipped(void) { return false; }
bool post_update_pending(const char *version) { (void)version; return false; }
void post_update_reset_shaders(const char *version) { (void)version; }
void post_update_finish(const char *version) { (void)version; }
void post_update_tidy(const char *version) { (void)version; }
void post_update_mark_failed(const char *version) { (void)version; }
bool post_update_failed_before(const char *version) { (void)version; return false; }
bool post_update_can_rollback(void) { return false; }
void update_check_start(void) {}
UpdateState update_check_state(char *version, size_t vn, char *message, size_t mn) {
    if (vn) version[0] = 0;
    if (mn) message[0] = 0;
    return UPDATE_IDLE;
}
bool update_apply_start(void) { return false; }
bool update_rollback_start(void) { return false; }
bool update_applies_here(void) { return false; }
void update_apply_here(char *title, size_t tn, char *what, size_t wn) { (void)title, (void)tn, (void)what, (void)wn; }
float update_apply_progress(void) { return -1; }

#endif
