#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#endif

typedef struct {
    char dir[MOD_PATH_CAP];
    char name[MOD_NAME_CAP];
} ModActiveLayer;

typedef struct {
    char dir[MOD_PATH_CAP];
    char id[MOD_ID_CAP];
    bool uses_romfs_subdir;
} ModDiscovered;

typedef struct {
    char id[MOD_ID_CAP];
    bool enabled;
} ModSavedEntry;

typedef struct {
    char *rel;
    int   mod_idx;
} ModPathRef;

typedef struct {
    ModPathRef *items;
    int         count;
    int         cap;
} ModPathRefList;

static bool            g_mod_session_started = false;
static int             g_mod_session_mods;  // mods this run loaded (each one an overlay's list names, each Mods tab mod)
static ModActiveLayer *g_mod_layers = NULL;
static int             g_mod_layer_count = 0;
static int             g_mod_layer_cap = 0;

static char          (*g_mod_session_ids)[MOD_ID_CAP] = NULL;
static int             g_mod_session_id_count = 0;
static int             g_mod_session_id_cap = 0;

static void mod_str_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static int mod_strcasecmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        int la = tolower(ca);
        int lb = tolower(cb);
        if (la != lb) return la - lb;
        if (ca == '\0') return 0;
    }
}

static bool mod_starts_with_ci(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (!*s) return false;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool mod_ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (slen <= xlen) return false;
    return mod_strcasecmp(s + slen - xlen, suffix) == 0;
}

static void mod_trim_trailing_slashes(char *path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        if (len == 3 && path[1] == ':' && isalpha((unsigned char)path[0])) break;
        path[--len] = '\0';
    }
}

static void mod_resolve_root(const char *root, char *out, size_t cap) {
    if (!root || !root[0]) {
        mod_str_copy(out, cap, ".");
        return;
    }
    mod_str_copy(out, cap, root);
    mod_trim_trailing_slashes(out);
}

static bool mod_join_path(char *out, size_t cap, const char *a, const char *b) {
    if (!out || cap == 0) return false;
    if (!a || !*a) {
        while (b && (*b == '/' || *b == '\\')) b++;
        size_t blen = b ? strlen(b) : 0;
        if (blen >= cap) return false;
        mod_str_copy(out, cap, b ? b : "");
        return true;
    }
    size_t alen = strlen(a);
    while (alen > 1 && (a[alen - 1] == '/' || a[alen - 1] == '\\')) {
        if (alen == 3 && a[1] == ':' && isalpha((unsigned char)a[0])) break;
        alen--;
    }
    while (b && (*b == '/' || *b == '\\')) b++;
    size_t blen = b ? strlen(b) : 0;
    bool need_slash = (alen > 0 && blen > 0 && a[alen - 1] != '/' && a[alen - 1] != '\\');
    size_t total = alen + (need_slash ? 1 : 0) + blen;
    if (total >= cap) return false;
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    if (blen > 0) memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return true;
}

static bool mod_is_dir(const char *path) {
    if (!path || !*path) return false;
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static bool mod_is_reg(const char *path) {
    if (!path || !*path) return false;
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISREG(sb.st_mode);
}

static void mod_mkdir_one(const char *dir) {
    if (!dir || !*dir || strcmp(dir, ".") == 0) return;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

bool mods_is_ignored_top_file(const char *name) {
    if (!name || !*name) return false;
    if (mod_starts_with_ci(name, "readme")) return true;
    if (mod_starts_with_ci(name, "license")) return true;
    if (mod_strcasecmp(name, "info.json") == 0) return true;
    if (mod_strcasecmp(name, "meta.json") == 0) return true;
    if (mod_strcasecmp(name, "rules.txt") == 0) return true;
    if (mod_strcasecmp(name, ".ds_store") == 0) return true;
    if (mod_strcasecmp(name, "thumbs.db") == 0) return true;
    if (mod_ends_with_ci(name, ".md")) return true;
    if (mod_ends_with_ci(name, ".py")) return true;
    if (mod_ends_with_ci(name, ".bat")) return true;
    if (mod_ends_with_ci(name, ".sh")) return true;
    if (mod_ends_with_ci(name, ".ps1")) return true;
    return false;
}

static bool mod_find_entry_ci(const char *dir, const char *target, bool want_dir,
                              char *name_out, size_t name_cap,
                              char *path_out, size_t path_cap) {
    DIR *d = opendir(dir);
    if (!d) return false;
    char best_name[MOD_ID_CAP] = {0};
    char best_path[MOD_PATH_CAP] = {0};
    int best_rank = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (mod_strcasecmp(e->d_name, target) != 0) continue;
        char cand[MOD_PATH_CAP];
        if (!mod_join_path(cand, sizeof cand, dir, e->d_name)) continue;
        if (want_dir ? !mod_is_dir(cand) : !mod_is_reg(cand)) continue;
        int rank = (strcmp(e->d_name, target) == 0) ? 2 : 1;
        if (rank == 1 && mod_strcasecmp(target, "romfs") == 0 && strcmp(e->d_name, "RomFS") == 0) {
            rank = 2;
        }
        if (rank > best_rank || (rank == best_rank && strcmp(e->d_name, best_name) < 0)) {
            best_rank = rank;
            mod_str_copy(best_name, sizeof best_name, e->d_name);
            mod_str_copy(best_path, sizeof best_path, cand);
        }
    }
    closedir(d);
    if (best_rank < 0) return false;
    if (name_out && name_cap > 0) mod_str_copy(name_out, name_cap, best_name);
    if (path_out && path_cap > 0) mod_str_copy(path_out, path_cap, best_path);
    return true;
}

static size_t mod_utf8_trim_cap(const char *s, size_t max_bytes) {
    size_t len = strlen(s);
    if (len <= max_bytes) return len;
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0u) == 0x80u) {
        cut--;
    }
    return cut;
}

static void mod_utf8_append_cp(uint32_t cp, char *out, size_t cap, size_t *pos) {
    if (!out || cap == 0 || cp == 0) return;
    char tmp[4];
    size_t n = 0;
    if (cp <= 0x7Fu) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        tmp[0] = (char)(0xC0u | (cp >> 6));
        tmp[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp <= 0xFFFFu) {
        tmp[0] = (char)(0xE0u | (cp >> 12));
        tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else if (cp <= 0x10FFFFu) {
        tmp[0] = (char)(0xF0u | (cp >> 18));
        tmp[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    if (n > 0 && *pos + n < cap) {
        memcpy(out + *pos, tmp, n);
        *pos += n;
    }
}

static int mod_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool mod_parse_hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = mod_hex_val(p[i]);
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static void mod_collapse_crlf(char *s) {
    if (!s) return;
    char *r = s;
    char *w = s;
    while (*r) {
        if (r[0] == '\r') {
            if (r[1] == '\n') r++;
            *w++ = '\n';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void mod_json_skip_ws(const char **p) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    *p = s;
}

static bool mod_json_read_string(const char **p, char *out, size_t cap, bool collapse_crlf) {
    const char *s = *p;
    if (*s != '"') return false;
    s++;
    size_t pos = 0;
    while (*s && *s != '"') {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\\') {
            if (!*s) break;
            char esc = *s++;
            switch (esc) {
            case '"':  case '\\': case '/':
                if (out && pos + 1 < cap) out[pos++] = esc;
                break;
            case 'b':
                if (out && pos + 1 < cap) out[pos++] = '\b';
                break;
            case 'f':
                if (out && pos + 1 < cap) out[pos++] = '\f';
                break;
            case 'n':
                if (out && pos + 1 < cap) out[pos++] = '\n';
                break;
            case 'r':
                if (!collapse_crlf && out && pos + 1 < cap) out[pos++] = '\r';
                break;
            case 't':
                if (out && pos + 1 < cap) out[pos++] = '\t';
                break;
            case 'u': {
                uint32_t cp = 0;
                if (mod_parse_hex4(s, &cp)) {
                    s += 4;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && s[0] == '\\' && s[1] == 'u') {
                        uint32_t lo = 0;
                        if (mod_parse_hex4(s + 2, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                            s += 6;
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                        }
                    }
                    mod_utf8_append_cp(cp, out, cap, &pos);
                }
                break;
            }
            default:
                if (out && pos + 1 < cap) out[pos++] = esc;
                break;
            }
        } else {
            if (collapse_crlf && ch == '\r') {
                if (*s == '\n') s++;
                if (out && pos + 1 < cap) out[pos++] = '\n';
            } else if (out && pos + 1 < cap) {
                out[pos++] = (char)ch;
            }
        }
    }
    if (*s == '"') s++;
    *p = s;
    if (out && cap > 0) {
        out[pos] = '\0';
        size_t safe = mod_utf8_trim_cap(out, pos);
        out[safe] = '\0';
        if (collapse_crlf) mod_collapse_crlf(out);
    }
    return true;
}

static void mod_json_skip_value(const char **p) {
    mod_json_skip_ws(p);
    const char *s = *p;
    if (*s == '"') {
        mod_json_read_string(&s, NULL, 0, false);
        *p = s;
        return;
    }
    if (*s == '{' || *s == '[') {
        int depth = 0;
        while (*s) {
            if (*s == '"') {
                mod_json_read_string(&s, NULL, 0, false);
                continue;
            }
            if (*s == '{' || *s == '[') {
                depth++;
            } else if (*s == '}' || *s == ']') {
                depth--;
                if (depth <= 0) {
                    s++;
                    break;
                }
            }
            s++;
        }
        *p = s;
        return;
    }
    while (*s && *s != ',' && *s != '}' && *s != ']' &&
           *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        s++;
    }
    *p = s;
}

static void mod_parse_json_file(const char *path, ModEntry *entry, bool *have_explicit_desc) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(65536 + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, 65536, f);
    fclose(f);
    buf[n] = '\0';

    const char *p = buf;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    mod_json_skip_ws(&p);
    if (*p != '{') {
        free(buf);
        return;
    }
    p++;

    for (;;) {
        mod_json_skip_ws(&p);
        if (*p == '\0' || *p == '}') break;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') break;
        char key[64];
        if (!mod_json_read_string(&p, key, sizeof key, false)) break;
        mod_json_skip_ws(&p);
        if (*p != ':') break;
        p++;
        mod_json_skip_ws(&p);
        if (*p == '"') {
            char val[MOD_DESC_CAP];
            mod_json_read_string(&p, val, sizeof val, true);
            if (val[0]) {
                if (strcmp(key, "name") == 0 && !entry->name[0]) {
                    mod_str_copy(entry->name, sizeof entry->name, val);
                } else if (strcmp(key, "version") == 0 && !entry->version[0]) {
                    mod_str_copy(entry->version, sizeof entry->version, val);
                } else if (strcmp(key, "author") == 0 && !entry->author[0]) {
                    mod_str_copy(entry->author, sizeof entry->author, val);
                } else if (strcmp(key, "description") == 0) {
                    if (!entry->description[0] || !*have_explicit_desc) {
                        mod_str_copy(entry->description, sizeof entry->description, val);
                        *have_explicit_desc = true;
                    }
                } else if (strcmp(key, "desc") == 0 && !entry->description[0]) {
                    mod_str_copy(entry->description, sizeof entry->description, val);
                }
            }
        } else {
            mod_json_skip_value(&p);
        }
    }
    free(buf);
}

static void mod_parse_readme_fallback(ModEntry *entry) {
    if (entry->description[0]) return;
    char readme_path[MOD_PATH_CAP];
    if (!mod_find_entry_ci(entry->mod_dir, "README.txt", false, NULL, 0, readme_path, sizeof readme_path)) {
        return;
    }
    FILE *f = fopen(readme_path, "rb");
    if (!f) return;
    char raw[2048 + 1];
    size_t n = fread(raw, 1, 2048, f);
    int extra = fgetc(f);
    fclose(f);
    raw[n] = '\0';

    const char *p = raw;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) return;

    char cleaned[2048 + 1];
    size_t w = 0;
    int nl_run = 0;
    while (*p && w + 1 < sizeof cleaned) {
        char c = *p++;
        if (c == '\r') {
            if (*p == '\n') p++;
            c = '\n';
        }
        if (c == '\n') {
            nl_run++;
            if (nl_run <= 2) cleaned[w++] = '\n';
        } else {
            nl_run = 0;
            cleaned[w++] = c;
        }
    }
    while (w > 0 && (cleaned[w - 1] == ' ' || cleaned[w - 1] == '\t' || cleaned[w - 1] == '\n')) {
        w--;
    }
    cleaned[w] = '\0';

    const size_t limit = 400;
    if (w > limit || extra != EOF) {
        size_t keep = (w > limit - 3) ? mod_utf8_trim_cap(cleaned, limit - 3) : w;
        while (keep > 0 && (cleaned[keep - 1] == ' ' || cleaned[keep - 1] == '\t' || cleaned[keep - 1] == '\n')) {
            keep--;
        }
        memcpy(entry->description, cleaned, keep);
        memcpy(entry->description + keep, "...", 4);
    } else {
        mod_str_copy(entry->description, sizeof entry->description, cleaned);
    }
}

static void mod_parse_metadata(ModEntry *entry) {
    bool have_explicit_desc = false;
    char json_path[MOD_PATH_CAP];

    if (mod_find_entry_ci(entry->mod_dir, "info.json", false, NULL, 0, json_path, sizeof json_path)) {
        mod_parse_json_file(json_path, entry, &have_explicit_desc);
    }
    if (mod_find_entry_ci(entry->mod_dir, "meta.json", false, NULL, 0, json_path, sizeof json_path)) {
        mod_parse_json_file(json_path, entry, &have_explicit_desc);
    }
    if (entry->uses_romfs_subdir) {
        if (mod_find_entry_ci(entry->host_dir, "info.json", false, NULL, 0, json_path, sizeof json_path)) {
            mod_parse_json_file(json_path, entry, &have_explicit_desc);
        }
        if (mod_find_entry_ci(entry->host_dir, "meta.json", false, NULL, 0, json_path, sizeof json_path)) {
            mod_parse_json_file(json_path, entry, &have_explicit_desc);
        }
    }

    mod_parse_readme_fallback(entry);
    if (!entry->name[0]) {
        mod_str_copy(entry->name, sizeof entry->name, entry->id);
    }
}

static int mod_cmp_discovered(const void *a, const void *b) {
    const ModDiscovered *da = (const ModDiscovered *)a;
    const ModDiscovered *db = (const ModDiscovered *)b;
    int c = mod_strcasecmp(da->id, db->id);
    return c ? c : strcmp(da->id, db->id);
}

static bool mod_discover_all(const char *root, ModDiscovered **out_items, int *out_count) {
    *out_items = NULL;
    *out_count = 0;

    char mods_dir[MOD_PATH_CAP];
    if (!mod_join_path(mods_dir, sizeof mods_dir, root, "mods")) return true;
    if (!mod_is_dir(mods_dir)) return true;

    DIR *d = opendir(mods_dir);
    if (!d) return true;

    ModDiscovered *items = NULL;
    int count = 0;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= MOD_ID_CAP) continue;

        char full[MOD_PATH_CAP];
        if (!mod_join_path(full, sizeof full, mods_dir, e->d_name)) continue;
        if (!mod_is_dir(full)) continue;

        if (count == cap) {
            int ncap = cap ? cap * 2 : 16;
            ModDiscovered *grown = realloc(items, sizeof(*items) * (size_t)ncap);
            if (!grown) {
                free(items);
                closedir(d);
                return false;
            }
            items = grown;
            cap = ncap;
        }
        ModDiscovered *dst = &items[count++];
        memset(dst, 0, sizeof *dst);
        mod_str_copy(dst->id, sizeof dst->id, e->d_name);
        mod_str_copy(dst->dir, sizeof dst->dir, full);
        dst->uses_romfs_subdir = false;
    }
    closedir(d);

    if (count > 1) qsort(items, (size_t)count, sizeof *items, mod_cmp_discovered);
    *out_items = items;
    *out_count = count;
    return true;
}

static void mod_read_mods_txt(const char *root, ModSavedEntry **out_items, int *out_count) {
    *out_items = NULL;
    *out_count = 0;

    char txt_path[MOD_PATH_CAP];
    if (!mod_join_path(txt_path, sizeof txt_path, root, "mods.txt")) return;
    FILE *f = fopen(txt_path, "rb");
    if (!f) return;

    ModSavedEntry *items = NULL;
    int count = 0;
    int cap = 0;
    char line[1024];
    bool first_line = true;

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if (first_line) {
            first_line = false;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p || *p == '#' || *p == ';') continue;

        if ((p[0] != '0' && p[0] != '1') || (p[1] != ' ' && p[1] != '\t')) continue;
        bool enabled = (p[0] == '1');
        p += 2;
        while (*p == ' ' || *p == '\t') p++;

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n')) {
            p[--len] = '\0';
        }
        if (len == 0 || len >= MOD_ID_CAP) continue;

        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(items[i].id, p) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (count == cap) {
            int ncap = cap ? cap * 2 : 16;
            ModSavedEntry *grown = realloc(items, sizeof(*items) * (size_t)ncap);
            if (!grown) {
                free(items);
                fclose(f);
                return;
            }
            items = grown;
            cap = ncap;
        }
        items[count].enabled = enabled;
        mod_str_copy(items[count].id, sizeof items[count].id, p);
        count++;
    }
    fclose(f);

    *out_items = items;
    *out_count = count;
}

static bool mod_append_entry(ModList *list, const ModEntry *src) {
    if (list->count == list->cap) {
        int ncap = list->cap ? list->cap * 2 : 16;
        ModEntry *grown = realloc(list->items, sizeof(*list->items) * (size_t)ncap);
        if (!grown) return false;
        list->items = grown;
        list->cap = ncap;
    }
    list->items[list->count++] = *src;
    if (src->enabled) list->enabled_count++;
    return true;
}

static bool mod_walk_files(const char *host_root, const char *rel,
                           bool skip_ignored_top,
                           ModEntry *entry, int mod_idx,
                           ModPathRefList *refs) {
    char dir_path[MOD_PATH_CAP];
    if (rel && rel[0]) {
        if (!mod_join_path(dir_path, sizeof dir_path, host_root, rel)) return true;
    } else {
        mod_str_copy(dir_path, sizeof dir_path, host_root);
    }

    DIR *d = opendir(dir_path);
    if (!d) return true;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char child_rel[MOD_PATH_CAP];
        if (rel && rel[0]) {
            if (!mod_join_path(child_rel, sizeof child_rel, rel, e->d_name)) continue;
        } else {
            mod_str_copy(child_rel, sizeof child_rel, e->d_name);
        }

        char child_host[MOD_PATH_CAP];
        if (!mod_join_path(child_host, sizeof child_host, host_root, child_rel)) continue;

        struct stat sb;
        if (stat(child_host, &sb) != 0) continue;

        if (S_ISDIR(sb.st_mode)) {
            if (!mod_walk_files(host_root, child_rel, skip_ignored_top, entry, mod_idx, refs)) {
                closedir(d);
                return false;
            }
            continue;
        }
        if (!S_ISREG(sb.st_mode)) continue;

        if ((!rel || !rel[0]) && skip_ignored_top && mods_is_ignored_top_file(e->d_name)) {
            continue;
        }

        entry->file_count++;
        if (sb.st_size > 0) entry->total_bytes += (uint64_t)sb.st_size;

        if (refs) {
            if (refs->count == refs->cap) {
                int ncap = refs->cap ? refs->cap * 2 : 128;
                ModPathRef *grown = realloc(refs->items, sizeof(*refs->items) * (size_t)ncap);
                if (!grown) {
                    closedir(d);
                    return false;
                }
                refs->items = grown;
                refs->cap = ncap;
            }
            size_t rlen = strlen(child_rel);
            char *copy = malloc(rlen + 1);
            if (!copy) {
                closedir(d);
                return false;
            }
            memcpy(copy, child_rel, rlen + 1);
            refs->items[refs->count].rel = copy;
            refs->items[refs->count].mod_idx = mod_idx;
            refs->count++;
        }
    }
    closedir(d);
    return true;
}

static int mod_cmp_path_ref(const void *a, const void *b) {
    const ModPathRef *ra = (const ModPathRef *)a;
    const ModPathRef *rb = (const ModPathRef *)b;
    int c = strcmp(ra->rel, rb->rel);
    if (c != 0) return c;
    return (ra->mod_idx > rb->mod_idx) - (ra->mod_idx < rb->mod_idx);
}

static void mod_add_sample(ModEntry *entry, const char *text) {
    if (!entry || entry->sample_count >= MOD_CONFLICT_SAMPLES || !text) return;
    mod_str_copy(entry->samples[entry->sample_count], MOD_SAMPLE_CAP, text);
    entry->sample_count++;
}

static void mod_analyze_conflicts(ModList *out, ModPathRefList *refs) {
    if (refs->count < 2) return;
    qsort(refs->items, (size_t)refs->count, sizeof *refs->items, mod_cmp_path_ref);

    int i = 0;
    while (i < refs->count) {
        int j = i + 1;
        while (j < refs->count && strcmp(refs->items[j].rel, refs->items[i].rel) == 0) {
            j++;
        }
        int span = j - i;
        if (span >= 2) {
            int win_idx = refs->items[i].mod_idx;
            int first_lose_idx = refs->items[i + 1].mod_idx;
            ModEntry *winner = &out->items[win_idx];

            out->total_conflicts++;
            winner->wins_conflicts++;

            if (winner->sample_count < MOD_CONFLICT_SAMPLES) {
                char msg[MOD_SAMPLE_CAP];
                if (span == 2) {
                    snprintf(msg, sizeof msg, "%s (overrides %s)",
                             refs->items[i].rel, out->items[first_lose_idx].name);
                } else {
                    snprintf(msg, sizeof msg, "%s (overrides %s +%d)",
                             refs->items[i].rel, out->items[first_lose_idx].name, span - 2);
                }
                mod_add_sample(winner, msg);
            }

            for (int k = i + 1; k < j; k++) {
                int lose_idx = refs->items[k].mod_idx;
                if (lose_idx == win_idx) continue;
                ModEntry *loser = &out->items[lose_idx];
                loser->loses_conflicts++;
                if (loser->sample_count < MOD_CONFLICT_SAMPLES) {
                    char msg[MOD_SAMPLE_CAP];
                    snprintf(msg, sizeof msg, "%s (overridden by %s)",
                             refs->items[k].rel, winner->name);
                    mod_add_sample(loser, msg);
                }
            }
        }
        i = j;
    }
}

static void mod_populate_entry(const ModDiscovered *disc, bool enabled, ModEntry *entry) {
    memset(entry, 0, sizeof *entry);
    mod_str_copy(entry->id, sizeof entry->id, disc->id);
    mod_str_copy(entry->mod_dir, sizeof entry->mod_dir, disc->dir);
    entry->enabled = enabled;

    char romfs_name[MOD_ID_CAP] = {0};
    char romfs_path[MOD_PATH_CAP] = {0};
    if (mod_find_entry_ci(disc->dir, "romfs", true, romfs_name, sizeof romfs_name, romfs_path, sizeof romfs_path)) {
        entry->uses_romfs_subdir = true;
        mod_str_copy(entry->host_dir, sizeof entry->host_dir, romfs_path);
        snprintf(entry->rel_dir, sizeof entry->rel_dir, "mods/%s/%s", disc->id, romfs_name);
    } else {
        entry->uses_romfs_subdir = false;
        mod_str_copy(entry->host_dir, sizeof entry->host_dir, disc->dir);
        snprintf(entry->rel_dir, sizeof entry->rel_dir, "mods/%s", disc->id);
    }
    mod_parse_metadata(entry);
}

bool mods_scan(const char *root, ModList *out, bool analyze_conflicts) {
    if (!out) return false;
    memset(out, 0, sizeof *out);

    char resolved_root[MOD_PATH_CAP];
    mod_resolve_root(root, resolved_root, sizeof resolved_root);

    ModDiscovered *disc = NULL;
    int disc_count = 0;
    if (!mod_discover_all(resolved_root, &disc, &disc_count)) return false;

    if (disc_count == 0) return true;

    ModSavedEntry *saved = NULL;
    int saved_count = 0;
    mod_read_mods_txt(resolved_root, &saved, &saved_count);

    bool *used = calloc((size_t)disc_count, sizeof(bool));
    if (!used) {
        free(disc);
        free(saved);
        return false;
    }

    for (int i = 0; i < saved_count; i++) {
        for (int j = 0; j < disc_count; j++) {
            if (used[j]) continue;
            if (strcmp(saved[i].id, disc[j].id) == 0) {
                ModEntry e;
                mod_populate_entry(&disc[j], saved[i].enabled, &e);
                if (!mod_append_entry(out, &e)) {
                    free(used);
                    free(disc);
                    free(saved);
                    mods_list_free(out);
                    return false;
                }
                used[j] = true;
                break;
            }
        }
    }

    for (int j = 0; j < disc_count; j++) {
        if (used[j]) continue;
        ModEntry e;
        mod_populate_entry(&disc[j], false, &e);
        if (!mod_append_entry(out, &e)) {
            free(used);
            free(disc);
            free(saved);
            mods_list_free(out);
            return false;
        }
    }

    free(used);
    free(disc);
    free(saved);

    ModPathRefList refs = {0};
    for (int i = 0; i < out->count; i++) {
        ModEntry *e = &out->items[i];
        bool skip_ignored_top = !e->uses_romfs_subdir;
        ModPathRefList *collect = (analyze_conflicts && e->enabled) ? &refs : NULL;
        if (!mod_walk_files(e->host_dir, "", skip_ignored_top, e, i, collect)) {
            for (int r = 0; r < refs.count; r++) free(refs.items[r].rel);
            free(refs.items);
            mods_list_free(out);
            return false;
        }
    }

    if (analyze_conflicts) {
        mod_analyze_conflicts(out, &refs);
    }

    for (int r = 0; r < refs.count; r++) free(refs.items[r].rel);
    free(refs.items);
    return true;
}

void mods_list_free(ModList *out) {
    if (!out) return;
    free(out->items);
    memset(out, 0, sizeof *out);
}

bool mods_save(const char *root, const ModList *list) {
    if (!list) return false;

    char resolved_root[MOD_PATH_CAP];
    mod_resolve_root(root, resolved_root, sizeof resolved_root);
    mod_mkdir_one(resolved_root);

    char dst_path[MOD_PATH_CAP];
    char tmp_path[MOD_PATH_CAP];
    if (!mod_join_path(dst_path, sizeof dst_path, resolved_root, "mods.txt")) return false;
    if (!mod_join_path(tmp_path, sizeof tmp_path, resolved_root, "mods.txt.tmp")) return false;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;

    fprintf(f, "# Savage Wind Mod Manager load order\n");
    fprintf(f, "# Format: 1 <id> (enabled) or 0 <id> (disabled), top = highest priority (#1)\n");
    for (int i = 0; i < list->count; i++) {
        if (!list->items[i].id[0]) continue;
        fprintf(f, "%d %s\n", list->items[i].enabled ? 1 : 0, list->items[i].id);
    }

    bool ok = !ferror(f);
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

#ifdef _WIN32
    if (!MoveFileExA(tmp_path, dst_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp_path);
        return false;
    }
#else
    if (rename(tmp_path, dst_path) != 0) {
        remove(tmp_path);
        return false;
    }
#endif
    return true;
}

bool mods_set_enabled(const char *root, const char *id, bool enabled) {
    if (!id || !*id) return false;
    ModList list;
    if (!mods_scan(root, &list, false)) return false;

    int found = -1;
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].id, id) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        mods_list_free(&list);
        return false;
    }

    list.items[found].enabled = enabled;
    bool ok = mods_save(root, &list);
    mods_list_free(&list);
    return ok;
}

bool mods_set_all_enabled(const char *root, bool enabled) {
    ModList list;
    if (!mods_scan(root, &list, false)) return false;

    for (int i = 0; i < list.count; i++) {
        list.items[i].enabled = enabled;
    }
    bool ok = mods_save(root, &list);
    mods_list_free(&list);
    return ok;
}

bool mods_move(const char *root, const char *id, int delta) {
    if (!id || !*id) return false;
    ModList list;
    if (!mods_scan(root, &list, false)) return false;

    int pos = -1;
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].id, id) == 0) {
            pos = i;
            break;
        }
    }
    if (pos < 0) {
        mods_list_free(&list);
        return false;
    }

    int target = pos + delta;
    if (target < 0) target = 0;
    if (target >= list.count) target = list.count - 1;

    if (target != pos) {
        ModEntry moved = list.items[pos];
        if (target < pos) {
            memmove(&list.items[target + 1], &list.items[target],
                    sizeof(ModEntry) * (size_t)(pos - target));
        } else {
            memmove(&list.items[pos], &list.items[pos + 1],
                    sizeof(ModEntry) * (size_t)(target - pos));
        }
        list.items[target] = moved;
    }

    bool ok = mods_save(root, &list);
    mods_list_free(&list);
    return ok;
}

bool mods_env_override(char *out, size_t cap) {
    const char *e = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    if (!e || !*e) return false;
    if (out && cap > 0) {
        mod_str_copy(out, cap, e);
        mod_trim_trailing_slashes(out);
    }
    return true;
}

static bool mod_push_layer(const char *dir, const char *name) {
    if (g_mod_layer_count == g_mod_layer_cap) {
        int ncap = g_mod_layer_cap ? g_mod_layer_cap * 2 : 8;
        ModActiveLayer *grown = realloc(g_mod_layers, sizeof(*g_mod_layers) * (size_t)ncap);
        if (!grown) return false;
        g_mod_layers = grown;
        g_mod_layer_cap = ncap;
    }
    ModActiveLayer *dst = &g_mod_layers[g_mod_layer_count++];
    mod_str_copy(dst->dir, sizeof dst->dir, dir);
    mod_str_copy(dst->name, sizeof dst->name, name);
    return true;
}

static bool mod_push_session_id(const char *id) {
    if (g_mod_session_id_count == g_mod_session_id_cap) {
        int ncap = g_mod_session_id_cap ? g_mod_session_id_cap * 2 : 8;
        char (*grown)[MOD_ID_CAP] = realloc(g_mod_session_ids, sizeof(*g_mod_session_ids) * (size_t)ncap);
        if (!grown) return false;
        g_mod_session_ids = grown;
        g_mod_session_id_cap = ncap;
    }
    mod_str_copy(g_mod_session_ids[g_mod_session_id_count++], MOD_ID_CAP, id);
    return true;
}

// savagewind.log names the mods each run loads, so that a bug report shows them (docs/FAQ.md, "Can I use mods?": a
// problem seen with mods is tried without them first). A mod manager that deploys one merged folder for
// SAVAGEWIND_ROMFS_OVERLAY (SWMM+) names the mods merged into it in MOD_LIST_FILE at that folder's root: one mod per
// line, highest priority first; lines that start with '#' are notes. The log copies those lines. Without the file it
// gives the folder's file count and top folders.
#define MOD_LIST_FILE  "savagewind-mods.txt"
#define MOD_LIST_LINES 200  // the lines copied; the rest are counted
#define MOD_LIST_LINE  240  // bytes of one line kept

// The mods MOD_LIST_FILE names in `dir` (-1: no such file), its lines copied to the log
static int mod_log_list_file(const char *dir) {
    char path[MOD_PATH_CAP];
    if (!mod_join_path(path, sizeof path, dir, MOD_LIST_FILE)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int mods = 0, lines = 0, skipped = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        bool whole = n > 0 && line[n - 1] == '\n';
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!whole && n == sizeof line - 1) {  // (a longer line: the rest of it is dropped)
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        }
        for (char *p = line; *p; p++)
            if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = ' ';
        const char *s = line;
        while (*s == ' ') s++;
        if (!*s) continue;
        if (*s != '#') mods++;
        if (lines >= MOD_LIST_LINES) {
            skipped++;
            continue;
        }
        lines++;
        fprintf(stderr, "[mods]     %.*s\n", (int)mod_utf8_trim_cap(s, MOD_LIST_LINE), s);
    }
    fclose(f);
    if (skipped) fprintf(stderr, "[mods]     (%d more lines)\n", skipped);
    return mods;
}

// For a folder with no MOD_LIST_FILE: how many files it holds and its first top folders
static void mod_log_folder(const char *dir) {
    static ModEntry count;  // (only file_count and total_bytes are used)
    memset(&count, 0, sizeof count);
    mod_walk_files(dir, "", false, &count, 0, NULL);
    char tops[256] = "";
    size_t at = 0;
    int n = 0, more = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char child[MOD_PATH_CAP];
            if (!mod_join_path(child, sizeof child, dir, e->d_name) || !mod_is_dir(child)) continue;
            if (n < 8 && at + strlen(e->d_name) + 3 < sizeof tops) {
                at += (size_t)snprintf(tops + at, sizeof tops - at, "%s%s", n ? ", " : "", e->d_name);
                n++;
            } else {
                more++;
            }
        }
        closedir(d);
    }
    fprintf(stderr, "[mods]     no " MOD_LIST_FILE " (no list of the mods in it): %d file%s, %.1f MB%s%s%s\n",
            count.file_count, count.file_count == 1 ? "" : "s", count.total_bytes / 1048576.0, n ? "; folders " : "", tops,
            more ? ", ..." : "");
}

void mods_session_begin(const char *root) {
    free(g_mod_layers);
    g_mod_layers = NULL;
    g_mod_layer_count = 0;
    g_mod_layer_cap = 0;

    free(g_mod_session_ids);
    g_mod_session_ids = NULL;
    g_mod_session_id_count = 0;
    g_mod_session_id_cap = 0;

    g_mod_session_started = true;
    g_mod_session_mods = 0;

    char env_dir[MOD_PATH_CAP];
    bool has_env = mods_env_override(env_dir, sizeof env_dir);
    if (has_env) {
        mod_push_layer(env_dir, "SAVAGEWIND_ROMFS_OVERLAY");
    }

    ModList list;
    bool scanned = mods_scan(root, &list, false);
    if (scanned) {
        for (int i = 0; i < list.count; i++) {
            if (!list.items[i].enabled) continue;
            mod_push_layer(list.items[i].host_dir, list.items[i].name);
            mod_push_session_id(list.items[i].id);
        }
    }

    if (!g_mod_layer_count) {
        fprintf(stderr, "[mods] none (no SAVAGEWIND_ROMFS_OVERLAY, no mod turned on in the Mods tab)\n");
    } else {
        fprintf(stderr, "[mods] active overlay layers (%d), highest priority first:\n", g_mod_layer_count);
        int layer = 0, mods = 0;
        if (has_env) {
            fprintf(stderr, "[mods]   #%d SAVAGEWIND_ROMFS_OVERLAY (%s)\n", ++layer, env_dir);
            int listed = mod_log_list_file(env_dir);
            if (listed < 0) mod_log_folder(env_dir);
            mods += listed < 0 ? 1 : listed;
        }
        for (int i = 0; scanned && i < list.count; i++) {
            const ModEntry *e = &list.items[i];
            if (!e->enabled) continue;
            fprintf(stderr, "[mods]   #%d %s%s%s (%s: %d file%s, %.1f MB)\n", ++layer, e->name, e->version[0] ? " " : "",
                    e->version, e->rel_dir, e->file_count, e->file_count == 1 ? "" : "s", e->total_bytes / 1048576.0);
            mods++;
        }
        fprintf(stderr, "[mods] %d mod%s loaded: try a problem without them first (docs/FAQ.md)\n", mods,
                mods == 1 ? "" : "s");
        g_mod_session_mods = mods;
    }
    if (scanned) mods_list_free(&list);
}

int mods_session_mod_count(void) { return g_mod_session_mods; }

int mods_report_line(char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    int count = mods_active_count();
    if (!count) return snprintf(out, cap, "Mods: none\n");
    size_t at = (size_t)snprintf(out, cap, "Mods: %d overlay layer%s (", count, count == 1 ? "" : "s");
    for (int i = 0; i < count && at < cap; i++) {
        const char *name = mods_active_name(i);
        at += (size_t)snprintf(out + at, cap - at, "%s%s", i ? ", " : "", name ? name : "?");
    }
    if (at < cap) at += (size_t)snprintf(out + at, cap - at, "); the mods are listed in savagewind.log\n");
    if (at >= cap && cap > 5) memcpy(out + cap - 5, "...\n", 5);  // (cut: still a line)
    return (int)strlen(out);
}

bool mods_session_pending_restart(const char *root) {
    ModList cur;
    if (!mods_scan(root, &cur, false)) return false;

    int matched = 0;
    bool differs = false;
    for (int i = 0; i < cur.count; i++) {
        if (!cur.items[i].enabled) continue;
        if (matched >= g_mod_session_id_count ||
            strcmp(cur.items[i].id, g_mod_session_ids[matched]) != 0) {
            differs = true;
            break;
        }
        matched++;
    }
    if (!differs && matched != g_mod_session_id_count) {
        differs = true;
    }

    mods_list_free(&cur);
    return differs;
}

int mods_active_count(void) {
    if (g_mod_session_started) return g_mod_layer_count;
    return mods_env_override(NULL, 0) ? 1 : 0;
}

const char *mods_active_dir(int index) {
    if (g_mod_session_started) {
        if (index < 0 || index >= g_mod_layer_count) return NULL;
        return g_mod_layers[index].dir;
    }
    if (index != 0) return NULL;
    static _Thread_local char env_buf[MOD_PATH_CAP];
    if (!mods_env_override(env_buf, sizeof env_buf)) return NULL;
    return env_buf;
}

const char *mods_active_name(int index) {
    if (g_mod_session_started) {
        if (index < 0 || index >= g_mod_layer_count) return NULL;
        return g_mod_layers[index].name;
    }
    if (index != 0 || !mods_env_override(NULL, 0)) return NULL;
    return "SAVAGEWIND_ROMFS_OVERLAY";
}

bool mods_find_file(const char *ov_sub, const char *rel,
                    char *out, size_t cap,
                    char *mod_name_out, size_t mod_name_cap) {
    if (!rel || !*rel) return false;
    while (*rel == '/' || *rel == '\\') rel++;
    if (!*rel) return false;

    char clean_sub[MOD_PATH_CAP] = {0};
    if (ov_sub && *ov_sub) {
        while (*ov_sub == '/' || *ov_sub == '\\') ov_sub++;
        mod_str_copy(clean_sub, sizeof clean_sub, ov_sub);
        mod_trim_trailing_slashes(clean_sub);
        if (strcmp(clean_sub, "/") == 0) clean_sub[0] = '\0';
    }

    int count = mods_active_count();
    for (int i = 0; i < count; i++) {
        const char *dir = mods_active_dir(i);
        if (!dir || !*dir) continue;

        char base[MOD_PATH_CAP];
        if (clean_sub[0]) {
            if (!mod_join_path(base, sizeof base, dir, clean_sub)) continue;
        } else {
            mod_str_copy(base, sizeof base, dir);
            mod_trim_trailing_slashes(base);
        }

        char cand[MOD_PATH_CAP];
        if (!mod_join_path(cand, sizeof cand, base, rel)) continue;
        if (mod_is_reg(cand)) {
            if (out && cap > 0) mod_str_copy(out, cap, cand);
            if (mod_name_out && mod_name_cap > 0) {
                const char *nm = mods_active_name(i);
                mod_str_copy(mod_name_out, mod_name_cap, nm ? nm : "");
            }
            return true;
        }
    }
    return false;
}
