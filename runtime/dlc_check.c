// Downloadable content check (dlc_check.h): tells whether the game will see and mount both DLC packs, and when not,
// which setup mistake kept it from working (#497, #470, #383, #188).
#include "dlc_check.h"
#include "romfs_build.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static const char *const DLC1_DIRS[] = {
    "Game", "Map", "Movie", "NavMesh", "Pack", "Physics", "Sound", "System", "Terrain", "UI", "Voice",
};

static bool join2(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool reg_nonempty(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool can_read_bytes(const char *p) {
    int fd = open(p, O_RDONLY | O_BINARY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok && st.st_size > 0) {
        unsigned char b;
        ok = read(fd, &b, 1) == 1;
    }
    close(fd);
    return ok;
}

// Printable ASCII without '%' (the start screen's trv replaces "%s" in status lines).
static void sanitize_text(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t w = 0;
    for (; src && src[w] && w + 1 < cap; w++) {
        unsigned char c = (unsigned char)src[w];
        dst[w] = c >= 0x20 && c <= 0x7e && c != '%' ? (char)c : '?';
    }
    dst[w] = 0;
}

static bool is_dlc_romfs_dir(const char *dir) {
    char aoc_ver[2048], game_ver[2048];
    return join2(aoc_ver, sizeof aoc_ver, dir, "System/AocVersion.txt") &&
           join2(game_ver, sizeof game_ver, dir, "System/Version.txt") &&
           reg_nonempty(aoc_ver) && !file_exists_path(game_ver);
}

static bool read_aoc_version(const char *dir, char *out, size_t cap) {
    out[0] = 0;
    char p[2048];
    if (!join2(p, sizeof p, dir, "System/AocVersion.txt")) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    char raw[64] = "";
    size_t n = fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    raw[n] = 0;
    char *a = raw, *b = raw + n;
    if (!strncmp(a, "\xEF\xBB\xBF", 3)) a += 3;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) *--b = 0;
    sanitize_text(out, cap, a);
    return out[0] != 0;
}

// True when a direct subfolder of dir holds System/AocVersion.txt (e.g. extracted/aoc1/romfs/).
static bool has_nested_dlc(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[2048];
        if (join2(sub, sizeof sub, dir, e->d_name) && is_dir_path(sub) && is_dlc_romfs_dir(sub)) found = true;
    }
    closedir(d);
    return found;
}

// First non-empty regular file in dir (writes its path to first_out when non-NULL).
static bool dir_first_file(const char *dir, char *first_out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        if (join2(p, sizeof p, dir, e->d_name) && reg_nonempty(p)) {
            if (first_out) snprintf(first_out, cap, "%s", p);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static bool eq_ascii_icase(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// With no aoc1..aoc8 folders, checks whether root has a sibling folder (e.g. dlc1/) holding a DLC RomFS (#383).
static bool find_misnamed_dlc(const char *root, char *name_out, size_t cap) {
    DIR *d = opendir(root);
    if (!d) return false;
    char **names = NULL;
    int n = 0, c = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (eq_ascii_icase(e->d_name, "exefs") || eq_ascii_icase(e->d_name, "romfs") ||
            eq_ascii_icase(e->d_name, "save") || eq_ascii_icase(e->d_name, "save-backups") ||
            eq_ascii_icase(e->d_name, "cache"))
            continue;
        char sub[2048];
        if (!join2(sub, sizeof sub, root, e->d_name) || !is_dir_path(sub)) continue;
        char *copy = strdup(e->d_name);
        if (!copy) continue;
        if (n == c) {
            int nc = c ? c * 2 : 8;
            char ** grown = realloc(names, (size_t)nc * sizeof *names);
            if (!grown) { free(copy); break; }
            names = grown, c = nc;
        }
        names[n++] = copy;
    }
    closedir(d);
    if (n > 1) qsort(names, (size_t)n, sizeof *names, cmp_str);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (!found) {
            char sub[2048];
            if (join2(sub, sizeof sub, root, names[i]) && (is_dlc_romfs_dir(sub) || has_nested_dlc(sub))) {
                sanitize_text(name_out, cap, names[i]);
                found = true;
            }
        }
        free(names[i]);
    }
    free(names);
    return found;
}

const char *rt_dlc_state_name(RtDlcState state) {
    switch (state) {
    case RT_DLC_NONE: return "none";
    case RT_DLC_READY: return "ready";
    case RT_DLC_ONLY_ONE: return "only-one";
    case RT_DLC_SWAPPED: return "swapped";
    case RT_DLC_WRONG_VERSION: return "wrong-version";
    case RT_DLC_INCOMPLETE: return "incomplete";
    case RT_DLC_MERGED: return "merged-into-romfs";
    case RT_DLC_MISNAMED: return "misnamed-folder";
    }
    return "unknown";
}

RtDlcCheck rt_dlc_check(const char *root) {
    RtDlcCheck out;
    memset(&out, 0, sizeof out);
    if (!root || !*root) return out;

    for (int i = 1; i <= RT_DLC_MAX; i++) {
        char name[16], dir[2048];
        snprintf(name, sizeof name, "aoc%d", i);
        if (join2(dir, sizeof dir, root, name) && is_dir_path(dir)) out.present_mask |= 1u << (i - 1);
    }

    if (!out.present_mask) {
        if (find_misnamed_dlc(root, out.detail, sizeof out.detail)) {
            out.state = RT_DLC_MISNAMED;
            return out;
        }
        char m1[2048], m2[2048];
        if ((join2(m1, sizeof m1, root, "romfs/System/AocVersion.txt") && file_exists_path(m1)) ||
            (join2(m2, sizeof m2, root, "romfs/Pack/AocMainField.pack") && file_exists_path(m2)))
            out.state = RT_DLC_MERGED;
        return out;
    }

    bool has1 = (out.present_mask & 1u) != 0, has2 = (out.present_mask & 2u) != 0;
    if (!has1 && !has2) {
        out.state = RT_DLC_INCOMPLETE;
        snprintf(out.detail, sizeof out.detail, "aoc1");
        return out;
    }

    char aoc1[2048], aoc2[2048], pack1[2048], pack2[2048];
    if (!join2(aoc1, sizeof aoc1, root, "aoc1") || !join2(aoc2, sizeof aoc2, root, "aoc2") ||
        !join2(pack1, sizeof pack1, aoc1, "Pack/AocMainField.pack") ||
        !join2(pack2, sizeof pack2, aoc2, "Pack/AocMainField.pack")) {
        out.state = RT_DLC_INCOMPLETE;
        return out;
    }

    if (has1 != has2) {
        const char *sole = has1 ? aoc1 : aoc2, *sole_name = has1 ? "aoc1" : "aoc2";
        if (!is_dlc_romfs_dir(sole)) {
            out.state = RT_DLC_INCOMPLETE;
            out.nested = has_nested_dlc(sole);
            snprintf(out.detail, sizeof out.detail, "%s/System/AocVersion.txt", sole_name);
            return out;
        }
        read_aoc_version(sole, out.version, sizeof out.version);
        out.state = RT_DLC_ONLY_ONE;
        out.only_pack = reg_nonempty(has1 ? pack1 : pack2) ? 1 : 2;
        return out;
    }

    if (!is_dlc_romfs_dir(aoc1)) {
        out.state = RT_DLC_INCOMPLETE;
        out.nested = has_nested_dlc(aoc1);
        snprintf(out.detail, sizeof out.detail, "aoc1/System/AocVersion.txt");
        return out;
    }
    if (!is_dlc_romfs_dir(aoc2)) {
        out.state = RT_DLC_INCOMPLETE;
        out.nested = has_nested_dlc(aoc2);
        snprintf(out.detail, sizeof out.detail, "aoc2/System/AocVersion.txt");
        return out;
    }

    read_aoc_version(aoc1, out.version, sizeof out.version);
    bool data1 = reg_nonempty(pack1), data2 = reg_nonempty(pack2);
    if (!data1 && data2) {
        out.state = RT_DLC_SWAPPED;
        return out;
    }

    // If aoc1 has Pack/ or Map/ (not a copy of the 1-file second DLC), check its version before its folder list so an
    // older 1.0 / 2.0 dump without Movie/ reports the version rather than a missing folder.
    char sub[2048];
    bool looks_like_dlc1 = (join2(sub, sizeof sub, aoc1, "Pack") && is_dir_path(sub)) ||
                           (join2(sub, sizeof sub, aoc1, "Map") && is_dir_path(sub));
    if (looks_like_dlc1 && strcmp(out.version, RT_DLC_VERSION) != 0) {
        out.state = RT_DLC_WRONG_VERSION;
        return out;
    }

    for (size_t i = 0; i < sizeof DLC1_DIRS / sizeof DLC1_DIRS[0]; i++) {
        if (!join2(sub, sizeof sub, aoc1, DLC1_DIRS[i]) || !is_dir_path(sub)) {
            out.state = RT_DLC_INCOMPLETE;
            snprintf(out.detail, sizeof out.detail, "aoc1/%s", DLC1_DIRS[i]);
            return out;
        }
    }
    if (!data1) {
        out.state = RT_DLC_INCOMPLETE;
        snprintf(out.detail, sizeof out.detail, "aoc1/Pack/AocMainField.pack");
        return out;
    }
    if (!join2(sub, sizeof sub, aoc1, "Movie") || !dir_first_file(sub, NULL, 0)) {
        out.state = RT_DLC_INCOMPLETE;
        snprintf(out.detail, sizeof out.detail, "aoc1/Movie");
        return out;
    }
    if (strcmp(out.version, RT_DLC_VERSION) != 0) {
        out.state = RT_DLC_WRONG_VERSION;
        return out;
    }

    out.state = RT_DLC_READY;
    return out;
}

RtDlcCheck rt_dlc_verify(const char *root) {
    RtDlcCheck out = rt_dlc_check(root);
    if (out.state != RT_DLC_READY) return out;

    for (int idx = 1; idx <= 2; idx++) {
        char name[16], dir[2048], why[128] = "";
        snprintf(name, sizeof name, "aoc%d", idx);
        if (!join2(dir, sizeof dir, root, name)) {
            out.state = RT_DLC_INCOMPLETE;
            snprintf(out.detail, sizeof out.detail, "%s", name);
            return out;
        }
        RomfsLayout layout;
        memset(&layout, 0, sizeof layout);
        if (!romfs_layout(dir, &layout, why, sizeof why) || layout.nfiles <= 0) {
            romfs_layout_free(&layout);
            out.state = RT_DLC_INCOMPLETE;
            sanitize_text(out.detail, sizeof out.detail, name);
            return out;
        }
        for (int i = 0; i < layout.nfiles; i++) {
            out.files++;
            out.bytes += layout.files[i].size;
        }
        romfs_layout_free(&layout);
    }

    // Confirm key files open and read cleanly on the host (catches unreadable permissions or offline cloud stubs).
    const char *const must_read[] = {
        "aoc1/System/AocVersion.txt",
        "aoc2/System/AocVersion.txt",
        "aoc1/Pack/AocMainField.pack",
    };
    for (size_t i = 0; i < sizeof must_read / sizeof must_read[0]; i++) {
        char p[2048];
        if (!join2(p, sizeof p, root, must_read[i]) || !can_read_bytes(p)) {
            out.state = RT_DLC_INCOMPLETE;
            snprintf(out.detail, sizeof out.detail, "%s", must_read[i]);
            return out;
        }
    }
    char movie_dir[2048], movie_file[2048] = "";
    if (!join2(movie_dir, sizeof movie_dir, root, "aoc1/Movie") ||
        !dir_first_file(movie_dir, movie_file, sizeof movie_file) || !can_read_bytes(movie_file)) {
        out.state = RT_DLC_INCOMPLETE;
        snprintf(out.detail, sizeof out.detail, "aoc1/Movie");
        return out;
    }

    return out;
}
