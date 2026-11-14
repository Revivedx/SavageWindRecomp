// Plain logic of SW Update (runtime/update_core.h).
#include "update_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ versions
typedef struct { unsigned long n[3]; int pre_kind; unsigned long pre; } Ver;  // pre_kind: 0 alpha, 1 beta, 2 rc, 3 final

static bool ver_parse(const char *s, Ver *v) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        char *e;
        v->n[i] = strtoul(s, &e, 10);
        s = e;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }
    v->pre_kind = 3, v->pre = 0;
    if (!*s) return true;
    if (*s != '-') return false;
    s++;
    static const char *const kinds[3] = {"alpha.", "beta.", "rc."};
    for (int k = 0; k < 3; k++) {
        size_t l = strlen(kinds[k]);
        if (!strncmp(s, kinds[k], l)) {
            s += l;
            if (!isdigit((unsigned char)*s)) return false;
            char *e;
            v->pre = strtoul(s, &e, 10);
            v->pre_kind = k;
            return !*e;
        }
    }
    return false;
}

bool uc_version_valid(const char *v) {
    Ver t;
    return ver_parse(v, &t);
}

int uc_version_cmp(const char *a, const char *b) {
    Ver x, y;
    bool ox = ver_parse(a, &x), oy = ver_parse(b, &y);
    if (!ox || !oy) return ox - oy;  // (a version that can't be read is older than any that can)
    for (int i = 0; i < 3; i++)
        if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    if (x.pre_kind != y.pre_kind) return x.pre_kind < y.pre_kind ? -1 : 1;
    if (x.pre != y.pre) return x.pre < y.pre ? -1 : 1;
    return 0;
}

// ------------------------------------------------------------------ JSON: just enough to read the list of releases
typedef struct { const char *p; bool bad; int depth; } J;

static void j_ws(J *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

// A string into out (cut to fit; out may be NULL). \uXXXX below 0x80 is kept, anything else becomes '?': no value read
// here (tags, names, URLs, digests) needs more.
static void j_string(J *j, char *out, size_t n) {
    size_t k = 0;
    j_ws(j);
    if (*j->p != '"') { j->bad = true; return; }
    j->p++;
    for (;;) {
        char c = *j->p;
        if (!c || (unsigned char)c < 0x20) { j->bad = true; return; }
        j->p++;
        if (c == '"') break;
        if (c == '\\') {
            char e = *j->p;
            if (!e) { j->bad = true; return; }
            j->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned v = 0;
                    for (int i = 0; i < 4; i++) {
                        if (!isxdigit((unsigned char)j->p[i])) { j->bad = true; return; }
                        v = v * 16 + (unsigned)(isdigit((unsigned char)j->p[i]) ? j->p[i] - '0' : (tolower((unsigned char)j->p[i]) - 'a' + 10));
                    }
                    j->p += 4;
                    c = v < 0x80 ? (char)v : '?';
                    break;
                }
                default: c = e;
            }
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (out && n) out[k] = 0;
}

static void j_skip(J *j);

static void j_skip_container(J *j, char close) {
    if (++j->depth > 64) { j->bad = true; return; }
    j->p++;
    j_ws(j);
    if (*j->p == close) { j->p++; j->depth--; return; }
    while (!j->bad) {
        if (close == '}') {
            j_string(j, NULL, 0);
            j_ws(j);
            if (*j->p != ':') { j->bad = true; return; }
            j->p++;
        }
        j_skip(j);
        j_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == close) { j->p++; j->depth--; return; }
        j->bad = true;
    }
}

static void j_skip(J *j) {
    j_ws(j);
    char c = *j->p;
    if (c == '"') j_string(j, NULL, 0);
    else if (c == '{') j_skip_container(j, '}');
    else if (c == '[') j_skip_container(j, ']');
    else if (c == 't' && !strncmp(j->p, "true", 4)) j->p += 4;
    else if (c == 'f' && !strncmp(j->p, "false", 5)) j->p += 5;
    else if (c == 'n' && !strncmp(j->p, "null", 4)) j->p += 4;
    else if (c == '-' || isdigit((unsigned char)c)) {
        j->p++;
        while (*j->p && strchr("0123456789.eE+-", *j->p)) j->p++;
    } else j->bad = true;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static void j_string_or_skip(J *j, char *out, size_t n) {
    j_ws(j);
    if (*j->p == '"') j_string(j, out, n);
    else j_skip(j);
}

// One entry of "assets": this platform's file and its signature go into r
static void read_asset(J *j, UcRelease *r, const char *suffix) {
    char name[256] = "", url[1024] = "", digest[80] = "";
    uint64_t size = 0;
    j_ws(j);
    if (*j->p != '{') { j_skip(j); return; }
    j->p++;
    j_ws(j);
    if (*j->p == '}') { j->p++; return; }
    while (!j->bad) {
        char key[40];
        j_string(j, key, sizeof key);
        j_ws(j);
        if (*j->p != ':') { j->bad = true; return; }
        j->p++;
        if (!strcmp(key, "name")) j_string_or_skip(j, name, sizeof name);
        else if (!strcmp(key, "browser_download_url")) j_string_or_skip(j, url, sizeof url);
        else if (!strcmp(key, "digest")) j_string_or_skip(j, digest, sizeof digest);
        else if (!strcmp(key, "size")) {
            j_ws(j);
            size = strtoull(j->p, NULL, 10);
            j_skip(j);
        } else j_skip(j);
        j_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        j->bad = true;
    }
    char sig_suffix[64];
    snprintf(sig_suffix, sizeof sig_suffix, "%s.sig", suffix);
    if (ends_with(name, suffix)) {
        snprintf(r->zip_name, sizeof r->zip_name, "%s", name);
        snprintf(r->zip_url, sizeof r->zip_url, "%s", url);
        r->zip_size = size;
        r->digest[0] = 0;
        if (!strncmp(digest, "sha256:", 7) && strlen(digest) == 71) snprintf(r->digest, sizeof r->digest, "%s", digest + 7);
    } else if (ends_with(name, sig_suffix))
        snprintf(r->sig_url, sizeof r->sig_url, "%s", url);
}

static void read_release(J *j, UcRelease *r, bool *draft, const char *suffix) {
    memset(r, 0, sizeof *r);
    *draft = false;
    j_ws(j);
    if (*j->p != '{') { j->bad = true; return; }
    j->p++;
    j_ws(j);
    if (*j->p == '}') { j->p++; return; }
    while (!j->bad) {
        char key[40];
        j_string(j, key, sizeof key);
        j_ws(j);
        if (*j->p != ':') { j->bad = true; return; }
        j->p++;
        j_ws(j);
        if (!strcmp(key, "tag_name")) j_string_or_skip(j, r->tag, sizeof r->tag);
        else if (!strcmp(key, "html_url")) j_string_or_skip(j, r->page_url, sizeof r->page_url);
        else if (!strcmp(key, "draft")) {
            *draft = !strncmp(j->p, "true", 4);
            j_skip(j);
        } else if (!strcmp(key, "assets") && *j->p == '[') {
            j->p++;
            j_ws(j);
            if (*j->p == ']') j->p++;
            else
                while (!j->bad) {
                    read_asset(j, r, suffix);
                    j_ws(j);
                    if (*j->p == ',') { j->p++; continue; }
                    if (*j->p == ']') { j->p++; break; }
                    j->bad = true;
                }
        } else j_skip(j);
        j_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        j->bad = true;
    }
}

UcPick uc_pick_release(const char *json, const char *current, const char *suffix, UcRelease *out, int *skipped_unsigned) {
    J j = {json, false, 0};
    j_ws(&j);
    if (*j.p != '[') return UC_PICK_BAD_JSON;
    j.p++;
    j_ws(&j);
    bool found = false;
    int unsigned_newer = 0;
    if (*j.p == ']') j.p++;
    else
        for (;;) {
            UcRelease r;
            bool draft;
            read_release(&j, &r, &draft, suffix);
            if (j.bad) return UC_PICK_BAD_JSON;
            if (!draft && uc_version_valid(r.tag) && uc_version_cmp(r.tag, current) > 0) {
                const char *v = r.tag[0] == 'v' || r.tag[0] == 'V' ? r.tag + 1 : r.tag;
                if (!r.zip_url[0] || !r.zip_size) {
                    // (no Windows zip in this one: another platform's release, nothing to say)
                } else if (!r.sig_url[0]) unsigned_newer++;
                else if (!found || uc_version_cmp(v, out->version) > 0) {
                    *out = r;
                    snprintf(out->version, sizeof out->version, "%s", v);
                    found = true;
                }
            }
            j_ws(&j);
            if (*j.p == ',') { j.p++; continue; }
            if (*j.p == ']') { j.p++; break; }
            return UC_PICK_BAD_JSON;
        }
    if (skipped_unsigned) *skipped_unsigned = unsigned_newer;
    return found ? UC_PICK_UPDATE : UC_PICK_CURRENT;
}

// ------------------------------------------------------------------ paths and the manifest
bool uc_path_safe(const char *path) {
    if (!path || !*path || strlen(path) > 400) return false;
    const char *s = path;
    for (;;) {
        const char *e = s + strcspn(s, "/");
        size_t l = (size_t)(e - s);
        if (!l || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return false;
        if (s[l - 1] == '.' || s[l - 1] == ' ') return false;
        for (size_t i = 0; i < l; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x20 || strchr("\\:*?\"<>|", c)) return false;
        }
        // (the name before its first dot: "nul.txt" is the NUL device too)
        size_t stem = strcspn(s, "./");
        char up[6] = "";
        if (stem <= 4) {
            for (size_t i = 0; i < stem; i++) up[i] = (char)toupper((unsigned char)s[i]);
            up[stem] = 0;
            static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL"};
            for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++)
                if (!strcmp(up, reserved[i])) return false;
            if (stem == 4 && (!strncmp(up, "COM", 3) || !strncmp(up, "LPT", 3)) && up[3] >= '0' && up[3] <= '9') return false;
        }
        if (!*e) return true;
        s = e + 1;
    }
}

const char *uc_strip_top(const char *zip_path) {
    static const char top[] = "Savage Wind/";
    size_t l = sizeof top - 1;
    return !strncmp(zip_path, top, l) && zip_path[l] ? zip_path + l : NULL;
}

bool uc_hex_decode(const char *hex, uint8_t *out, size_t n) {
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n * 2; i++) {
        if (!isxdigit((unsigned char)hex[i])) return false;
        int c = tolower((unsigned char)hex[i]), v = isdigit(c) ? c - '0' : c - 'a' + 10;
        if (i & 1) out[i / 2] |= (uint8_t)v;
        else out[i / 2] = (uint8_t)(v << 4);
    }
    return true;
}

bool uc_path_inside(const char *exe, const char *appdir) {
    size_t n = strlen(appdir);
    while (n > 1 && appdir[n - 1] == '/') n--;  // ("/tmp/.mount_x/" and "/tmp/.mount_x" are the same folder)
    return n && !strncmp(exe, appdir, n) && exe[n] == '/';
}

void uc_manifest_free(UcManifest *m) {
    for (int i = 0; i < m->count; i++) free(m->files[i].path);
    free(m->files);
    m->files = NULL;
    m->count = 0;
}

bool uc_manifest_parse(const char *text, UcManifest *out, char *why, size_t why_size) {
    memset(out, 0, sizeof *out);
    int cap = 0, line_no = 0;
    bool header = false;
    for (const char *s = text; *s;) {
        const char *e = s + strcspn(s, "\n");
        size_t l = (size_t)(e - s);
        if (l && s[l - 1] == '\r') l--;
        line_no++;
        char line[1024];
        if (l >= sizeof line) { snprintf(why, why_size, "line %d is too long", line_no); goto bad; }
        memcpy(line, s, l);
        line[l] = 0;
        s = *e ? e + 1 : e;
        if (!l) continue;
        if (!header) {
            if (strcmp(line, "savagewind-update-manifest 1")) { snprintf(why, why_size, "not a file list of a known format"); goto bad; }
            header = true;
        } else if (!strncmp(line, "version ", 8)) {
            snprintf(out->version, sizeof out->version, "%s", line + 8);
        } else if (!strncmp(line, "f ", 2)) {
            char *hash = line + 2, *size_s = strchr(hash, ' ');
            if (!size_s) { snprintf(why, why_size, "line %d has no size", line_no); goto bad; }
            *size_s++ = 0;
            char *path = strchr(size_s, ' ');
            if (!path) { snprintf(why, why_size, "line %d has no path", line_no); goto bad; }
            *path++ = 0;
            uint8_t raw[32];
            char *end;
            uint64_t size = strtoull(size_s, &end, 10);
            if (!uc_hex_decode(hash, raw, 32) || *end || !isdigit((unsigned char)*size_s)) {
                snprintf(why, why_size, "line %d is malformed", line_no);
                goto bad;
            }
            if (!uc_path_safe(path)) { snprintf(why, why_size, "line %d names an unsafe path", line_no); goto bad; }
            if (uc_manifest_find(out, path)) { snprintf(why, why_size, "line %d repeats a file", line_no); goto bad; }
            if (out->count == cap) {
                cap = cap ? cap * 2 : 256;
                UcFile *nf = (UcFile *)realloc(out->files, (size_t)cap * sizeof *nf);
                if (!nf) { snprintf(why, why_size, "out of memory"); goto bad; }
                out->files = nf;
            }
            UcFile *f = &out->files[out->count];
            for (int i = 0; i < 64; i++) f->sha256[i] = (char)tolower((unsigned char)hash[i]);
            f->sha256[64] = 0;
            f->size = size;
            f->path = strdup(path);
            if (!f->path) { snprintf(why, why_size, "out of memory"); goto bad; }
            out->count++;
        } else { snprintf(why, why_size, "line %d is not understood", line_no); goto bad; }
    }
    if (!header) { snprintf(why, why_size, "it is empty"); goto bad; }
    if (!uc_version_valid(out->version)) { snprintf(why, why_size, "it has no valid version"); goto bad; }
    if (!out->count) { snprintf(why, why_size, "it lists no files"); goto bad; }
    return true;
bad:
    uc_manifest_free(out);
    return false;
}

const UcFile *uc_manifest_find(const UcManifest *m, const char *path) {
    for (int i = 0; i < m->count; i++) {
        const char *a = m->files[i].path, *b = path;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (!*a && !*b) return &m->files[i];
    }
    return NULL;
}
