// Save profiles: see save_profiles.h. The comments say where a choice comes from rather than what a line does.
#include "save_profiles.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"  // (Windows: mkdir with a mode)
#include "save_convert.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#define PATH_CAP 1100
#define PROFILES_DIR "save-profiles"
#define ACTIVE_FILE "save_profile.txt"
#define TMP_MAX_AGE (24 * 60 * 60)

static __thread char g_detail[PATH_CAP + 128];
const char *sp_detail(void) { return g_detail; }

static SpCode fail(SpCode c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static SpCode fail(SpCode c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_detail, sizeof g_detail, fmt, ap);
    va_end(ap);
    return c;
}

// ------------------------------------------------------------------ small helpers
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_link(const char *path) {  // symlink or junction, a broken one too
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

// root + "/" + rest; root NULL or "" gives rest alone (the working directory is the data root)
static bool join(char *out, size_t cap, const char *root, const char *rest) {
    int n = root && *root ? snprintf(out, cap, "%s/%s", root, rest) : snprintf(out, cap, "%s", rest);
    return n > 0 && (size_t)n < cap;
}

static bool fold_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

static bool is_default_name(const char *name) { return !name || !*name || fold_eq(name, SP_DEFAULT_NAME); }

static void mkdir_one(const char *p, bool *ok) {
    if (!is_dir(p) && mkdir(p, 0755) != 0 && !is_dir(p)) *ok = false;
}

// <root>/save-profiles, made when missing
static bool profiles_dir(const char *root, char *out, size_t cap, bool make) {
    if (!join(out, cap, root, PROFILES_DIR)) return false;
    if (!make) return true;
    bool ok = true;
    mkdir_one(out, &ok);
    return ok;
}

// ------------------------------------------------------------------ names
static const char *const RESERVED[] = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"};

// Windows treats these as devices in any case and with any extension: "nul", "NUL.txt", "com1.save"
static bool windows_device(const char *name) {
    char base[SP_NAME_MAX + 1];
    snprintf(base, sizeof base, "%s", name);
    char *dot = strchr(base, '.');
    if (dot) *dot = 0;
    size_t n = strlen(base);
    while (n && base[n - 1] == ' ') base[--n] = 0;  // ("nul .txt" is still the device)
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++)
        if (fold_eq(base, RESERVED[i])) return true;
    bool numbered = n == 4 && base[3] >= '1' && base[3] <= '9';  // COM1..COM9, LPT1..LPT9
    base[3] = 0;
    return numbered && (fold_eq(base, "COM") || fold_eq(base, "LPT"));
}

SpCode sp_validate_name(const char *name) {
    if (!name) return SP_NAME_EMPTY;
    size_t n = strlen(name);
    if (n == 0) return SP_NAME_EMPTY;
    if (n > SP_NAME_MAX) return SP_NAME_LONG;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(c < 128 && (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')')))
            return SP_NAME_CHARS;
    }
    if (name[0] == ' ' || name[n - 1] == ' ') return n == strspn(name, " ") ? SP_NAME_EMPTY : SP_NAME_CHARS;
    if (name[0] == '.' || name[n - 1] == '.' || strstr(name, "..")) return SP_NAME_CHARS;
    if (fold_eq(name, SP_DEFAULT_NAME) || windows_device(name)) return SP_NAME_RESERVED;
    return SP_OK;
}

// ------------------------------------------------------------------ where things are
bool sp_save_dir(const char *root, const char *name, char *out, size_t cap) {
    if (is_default_name(name)) return join(out, cap, root, "save");
    if (sp_validate_name(name) != SP_OK) return false;
    char rel[PATH_CAP];
    return snprintf(rel, sizeof rel, PROFILES_DIR "/%s/save", name) < (int)sizeof rel && join(out, cap, root, rel);
}

static bool profile_dir(const char *root, const char *name, char *out, size_t cap) {
    char rel[PATH_CAP];
    return snprintf(rel, sizeof rel, PROFILES_DIR "/%s", name) < (int)sizeof rel && join(out, cap, root, rel);
}

static int cmp_name(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    for (; *x && *y; x++, y++) {
        int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
        if (d) return d;
    }
    int d = (unsigned char)*x - (unsigned char)*y;
    return d ? d : strcmp(*(char *const *)a, *(char *const *)b);
}

// The valid profile names in save-profiles, sorted. Free with names_free.
static char **names_list(const char *root, int *n) {
    char dir[PATH_CAP];
    *n = 0;
    if (!profiles_dir(root, dir, sizeof dir, false)) return NULL;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char **v = NULL;
    int cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[PATH_CAP];
        if (sp_validate_name(e->d_name) != SP_OK || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p ||
            !is_dir(p))
            continue;
        if (cnt == cap) {
            char **nv = realloc(v, (cap = cap ? cap * 2 : 8) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        if (!(v[cnt] = strdup(e->d_name))) break;
        cnt++;
    }
    closedir(d);
    if (cnt > 1) qsort(v, cnt, sizeof *v, cmp_name);
    *n = cnt;
    return v;
}

static void names_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

// The folder name a profile has now, in the list's own spelling: the name looked up without regard to case. NULL: none.
static char *find_profile(const char *root, const char *name) {
    if (is_default_name(name)) return strdup(SP_DEFAULT_NAME);
    int n;
    char **v = names_list(root, &n);
    char *found = NULL;
    for (int i = 0; i < n && !found; i++)
        if (fold_eq(v[i], name)) found = strdup(v[i]);
    names_free(v, n);
    return found;
}

// ------------------------------------------------------------------ the active profile
static void read_active_file(const char *root, char *name, size_t cap) {
    char path[PATH_CAP], buf[SP_NAME_MAX + 8] = {0};
    snprintf(name, cap, "%s", SP_DEFAULT_NAME);
    if (!join(path, sizeof path, root, ACTIVE_FILE)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (sp_validate_name(buf) != SP_OK) return;
    char *real = find_profile(root, buf);  // (a profile deleted by hand: Default)
    if (real) snprintf(name, cap, "%s", real);
    free(real);
}

void sp_active(const char *root, char *name, size_t cap) { read_active_file(root, name, cap); }

bool sp_override_active(void) {
    const char *e = getenv("SAVAGEWIND_SAVE_DIR");
    return e && *e;
}

void sp_active_save_dir(const char *root, char *out, size_t cap) {
    const char *e = getenv("SAVAGEWIND_SAVE_DIR");  // (tests: a scratch copy; wins, as it always did)
    if (e && *e) {
        snprintf(out, cap, "%s", e);
        return;
    }
    char name[SP_NAME_MAX + 1];
    sp_active(root, name, sizeof name);
    if (!sp_save_dir(root, name, out, cap)) snprintf(out, cap, "%s", "save");
}

static char g_session[PATH_CAP];

void sp_session_begin(void) {
    sp_active_save_dir(NULL, g_session, sizeof g_session);
    char name[SP_NAME_MAX + 1];
    sp_active(NULL, name, sizeof name);
    if (!sp_override_active() && !is_default_name(name)) fprintf(stderr, "[saves] profile: %s (%s)\n", name, g_session);
}

const char *sp_session_save_dir(void) {
    if (!g_session[0]) sp_active_save_dir(NULL, g_session, sizeof g_session);
    return g_session;
}

// The name is written whole by a rename: a power cut leaves the old name or the new one, never half
static SpCode write_active(const char *root, const char *name) {
    char path[PATH_CAP], tmp[PATH_CAP + 8];
    if (!join(path, sizeof path, root, ACTIVE_FILE)) return fail(SP_IO, "path too long");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return fail(SP_IO, "%s: %s", tmp, strerror(errno));
    bool ok = fprintf(f, "%s\n", name) > 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) {
        int err = errno;
        remove(tmp);
        return fail(SP_IO, "%s: %s", path, strerror(err));
    }
    return SP_OK;
}

// ------------------------------------------------------------------ listing
int sp_list(const char *root, SpProfile **out) {
    char active[SP_NAME_MAX + 1];
    sp_active(root, active, sizeof active);
    int n;
    char **names = names_list(root, &n);
    SpProfile *v = calloc((size_t)n + 1, sizeof *v);
    *out = v;
    if (!v) { names_free(names, n); return 0; }
    for (int i = 0; i <= n; i++) {
        SpProfile *p = &v[i];
        snprintf(p->name, sizeof p->name, "%s", i ? names[i - 1] : SP_DEFAULT_NAME);
        p->is_default = i == 0;
        p->active = !strcmp(p->name, active);
        char dir[PATH_CAP], s0[PATH_CAP + 8];
        ScSlot *slots = NULL;
        if (sp_save_dir(root, p->name, dir, sizeof dir) && snprintf(s0, sizeof s0, "%s/save_0", dir) < (int)sizeof s0)
            p->slots = sc_list_slots(s0, &slots);
        for (int k = 0; k < p->slots; k++)
            if (slots[k].saved > p->saved) p->saved = slots[k].saved;
        free(slots);
    }
    names_free(names, n);
    return n + 1;
}

// ------------------------------------------------------------------ locks and sizes
// A running game holds <save>.lock: the profile is in use when somebody else has it. A lock file that can't be made
// protects nothing (read-only folder) and doesn't stop the player.
static bool in_use(const char *save_dir) {
    ScLock *l = NULL;
    bool busy = sc_lock_take(save_dir, &l) == SC_LOCK_BUSY;
    sc_lock_drop(l);
    return busy;
}

static uint64_t tree_bytes(const char *dir, int depth) {
    if (depth > 16 || is_link(dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[PATH_CAP];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p ||
            stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) total += tree_bytes(p, depth + 1);
        else if (S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

static uint64_t os_free_space(const char *dir) {
#ifdef _WIN32
    ULARGE_INTEGER avail;
    return GetDiskFreeSpaceExA(dir, &avail, NULL, NULL) ? avail.QuadPart : UINT64_MAX;
#else
    struct statvfs sv;
    return statvfs(dir, &sv) == 0 ? (uint64_t)sv.f_bavail * sv.f_frsize : UINT64_MAX;
#endif
}
uint64_t (*sp_free_space_hook)(const char *dir) = os_free_space;

// ------------------------------------------------------------------ removing
// rm -rf that never goes through a link (a junction in a profile would otherwise be followed out of it)
static bool remove_tree(const char *dir, int depth) {
    if (depth > 32 || is_link(dir)) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        char p[PATH_CAP];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) ok = false;
        else if (is_link(p)) {
#ifdef _WIN32
            ok = remove(p) == 0 || RemoveDirectoryA(p);  // (the link itself, never what it points to)
#else
            ok = remove(p) == 0;
#endif
        } else if (is_dir(p)) ok = remove_tree(p, depth + 1);
        else ok = remove(p) == 0;
    }
    closedir(d);
    return ok && rmdir(dir) == 0;
}

static void tmp_name(const char *root, const char *what, char *out, size_t cap) {
    static unsigned counter;
    char rel[PATH_CAP];
    snprintf(rel, sizeof rel, PROFILES_DIR "/.%s-%lx-%u", what, (unsigned long)time(NULL), ++counter);
    join(out, cap, root, rel);
}

void sp_cleanup(const char *root) {
    char dir[PATH_CAP];
    if (!profiles_dir(root, dir, sizeof dir, false)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    char *doomed[64];
    int n = 0;
    struct dirent *e;
    time_t now = time(NULL);
    while (n < 64 && (e = readdir(d))) {
        char p[PATH_CAP];
        struct stat st;
        bool deleting = !strncmp(e->d_name, ".deleting-", 10), tmp = !strncmp(e->d_name, ".tmp-", 5);
        if (!(deleting || tmp) || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p || stat(p, &st) != 0)
            continue;
        if (deleting || now - st.st_mtime > TMP_MAX_AGE) doomed[n++] = strdup(p);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        if (doomed[i]) remove_tree(doomed[i], 0);
        free(doomed[i]);
    }
}

// ------------------------------------------------------------------ changing
SpCode sp_set_active(const char *root, const char *name) {
    char *real = find_profile(root, name);
    if (!real) return fail(SP_NOT_FOUND, "%s", name ? name : "");
    char dir[PATH_CAP];
    SpCode c = SP_OK;
    if (!sp_save_dir(root, real, dir, sizeof dir)) c = fail(SP_IO, "path too long");
    else if (in_use(dir)) c = fail(SP_IN_USE, "%s", dir);  // (a running game: starting into it would save from both)
    else c = write_active(root, real);
    free(real);
    return c;
}

// Checks the new name is valid and free; nothing else
static SpCode check_new(const char *root, const char *name) {
    SpCode c = sp_validate_name(name);
    if (c != SP_OK) return fail(c, "%s", name ? name : "");
    char *other = find_profile(root, name);
    if (other) {
        free(other);
        return fail(SP_EXISTS, "%s", name);
    }
    return SP_OK;
}

SpCode sp_create(const char *root, const char *name) {
    SpCode c = check_new(root, name);
    if (c != SP_OK) return c;
    char top[PATH_CAP], dir[PATH_CAP], save[PATH_CAP];
    if (!profiles_dir(root, top, sizeof top, true)) return fail(SP_IO, "%s: %s", top, strerror(errno));
    if (!profile_dir(root, name, dir, sizeof dir) || !sp_save_dir(root, name, save, sizeof save)) return fail(SP_IO, "path too long");
    if (mkdir(dir, 0755) != 0) return fail(errno == EEXIST ? SP_EXISTS : SP_IO, "%s: %s", dir, strerror(errno));  // (atomic: two at once can't both win)
    if (mkdir(save, 0755) != 0) {
        int err = errno;
        rmdir(dir);
        return fail(SP_IO, "%s: %s", save, strerror(err));
    }
    return SP_OK;
}

SpCode sp_copy(const char *root, const char *from, const char *to, uint64_t *bytes) {
    if (bytes) *bytes = 0;
    char *src_name = find_profile(root, from);
    if (!src_name) return fail(SP_NOT_FOUND, "%s", from ? from : "");
    SpCode c = check_new(root, to);
    char src[PATH_CAP], top[PATH_CAP], tmp[PATH_CAP], tmp_save[PATH_CAP + 8], dst[PATH_CAP];
    if (c == SP_OK && !sp_save_dir(root, src_name, src, sizeof src)) c = fail(SP_IO, "path too long");
    free(src_name);
    if (c != SP_OK) return c;
    if (!profiles_dir(root, top, sizeof top, true)) return fail(SP_IO, "%s: %s", top, strerror(errno));
    tmp_name(root, "tmp", tmp, sizeof tmp);
    if (!profile_dir(root, to, dst, sizeof dst) || snprintf(tmp_save, sizeof tmp_save, "%s/save", tmp) >= (int)sizeof tmp_save)
        return fail(SP_IO, "path too long");

    uint64_t size = is_dir(src) ? tree_bytes(src, 0) : 0;
    uint64_t need = size + size / 16 + (1u << 20);  // a margin for folder entries and the next save
    uint64_t have = sp_free_space_hook ? sp_free_space_hook(top) : UINT64_MAX;
    if (have < need) return fail(SP_NO_SPACE, "%llu bytes needed, %llu free", (unsigned long long)need, (unsigned long long)have);

    if (mkdir(tmp, 0755) != 0) return fail(SP_IO, "%s: %s", tmp, strerror(errno));
    if (is_dir(src)) {
        ScReport r = {0};
        ScCode sc = sc_backup(src, tmp_save, &r);  // (takes the source's lock; removes what it copied again on failure)
        if (sc != SC_OK) {
            c = sc == SC_IN_USE ? SP_IN_USE : SP_IO;
            fail(c, "%s", r.detail);
            sc_report_free(&r);
            remove_tree(tmp, 0);
            return c;
        }
        sc_report_free(&r);
    } else if (mkdir(tmp_save, 0755) != 0) {  // (no save folder yet: the copy of nothing is an empty profile)
        c = fail(SP_IO, "%s: %s", tmp_save, strerror(errno));
        remove_tree(tmp, 0);
        return c;
    }
    // renaming onto an existing folder would replace an empty one on Linux, so look again right before
    char *late = find_profile(root, to);
    if (late) {
        free(late);
        remove_tree(tmp, 0);
        return fail(SP_EXISTS, "%s", to);
    }
    if (rename(tmp, dst) != 0) {
        c = fail(SP_IO, "%s: %s", dst, strerror(errno));
        remove_tree(tmp, 0);
        return c;
    }
    if (bytes) *bytes = size;
    return SP_OK;
}

// The profile `name` as a folder that may be renamed or deleted: not Default, not the active one, not in use
static SpCode check_changeable(const char *root, const char *name, char **real) {
    *real = NULL;
    if (is_default_name(name)) return fail(SP_IS_DEFAULT, "%s", SP_DEFAULT_NAME);
    *real = find_profile(root, name);
    if (!*real) return fail(SP_NOT_FOUND, "%s", name);
    char active[SP_NAME_MAX + 1], save[PATH_CAP];
    sp_active(root, active, sizeof active);
    SpCode c = SP_OK;
    if (!strcmp(*real, active)) c = fail(SP_IS_ACTIVE, "%s", *real);
    else if (!sp_save_dir(root, *real, save, sizeof save)) c = fail(SP_IO, "path too long");
    else if (in_use(save)) c = fail(SP_IN_USE, "%s", save);
    if (c != SP_OK) free(*real), *real = NULL;
    return c;
}

SpCode sp_rename(const char *root, const char *from, const char *to) {
    char *real;
    SpCode c = check_changeable(root, from, &real);
    if (c != SP_OK) return c;
    char a[PATH_CAP], b[PATH_CAP];
    if (sp_validate_name(to) != SP_OK) c = fail(sp_validate_name(to), "%s", to ? to : "");
    else if (!profile_dir(root, real, a, sizeof a) || !profile_dir(root, to, b, sizeof b)) c = fail(SP_IO, "path too long");
    else if (!fold_eq(real, to)) {  // (only the case of the letters changing is a rename of the same folder: allowed)
        char *other = find_profile(root, to);
        if (other) free(other), c = fail(SP_EXISTS, "%s", to);
    }
    if (c == SP_OK && rename(a, b) != 0) c = fail(SP_IO, "%s: %s", a, strerror(errno));
    free(real);
    return c;
}

SpCode sp_delete(const char *root, const char *name) {
    char *real;
    SpCode c = check_changeable(root, name, &real);
    if (c != SP_OK) return c;
    char dir[PATH_CAP], gone[PATH_CAP];
    if (!profile_dir(root, real, dir, sizeof dir)) c = fail(SP_IO, "path too long");
    else if (is_link(dir)) c = fail(SP_LINKED, "%s", dir);
    else {
        tmp_name(root, "deleting", gone, sizeof gone);
        if (rename(dir, gone) != 0) c = fail(SP_IO, "%s: %s", dir, strerror(errno));  // (nothing changed; Windows: a program has it open)
        else if (!remove_tree(gone, 0)) c = fail(SP_IO, "%s: could not be removed completely (sp_cleanup finishes it)", gone);
    }
    free(real);
    return c;
}
