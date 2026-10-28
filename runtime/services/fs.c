// fsp-srv: RomFS as a raw storage image, save data and SD card backed by host directories.
//
// RomFS overlay (development): SAVAGEWIND_ROMFS_OVERLAY=<dir>, a folder laid out like the RomFS. Its files replace
// the game's files of the same path, and files the game doesn't have are added (into directories it has). The
// image the game reads stays one virtual RomFS: patched copies of its tables, the host files appended past its end.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "../achievements.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../ipc.h"
#include "../language.h"
#include "../romfs_image.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_TARGET_LOCKED MAKE_RESULT(2, 7)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)

const char *g_save_dir = "save";

// ------------------------------------------------------------------ RomFS file index (offset -> path)
typedef struct { uint64_t start, size; char *path; int fd; const uint8_t *mem; } Overlay;
// in-image patches (copies of RomFS tables with changes): header, dir meta, file hash, file meta
typedef struct { uint64_t off, size; uint8_t *data; } Patch;
// Lets the runtime see which game files are being streamed (e.g. which level is loading).
typedef struct { uint64_t off, size; char *path; uint32_t meta; } RomFile;
// Redirected voice files that got bigger than the game's own (voice_redirect), for rstb_patch
typedef struct { const char *path; uint64_t grow; } GrownFile;
// A RomFS image the game reads and what Savage Wind changes in it: the game's own (base + update, extracted/romfs)
// and each add-on content's (extracted/aoc<index>). Both get the same changes (voices, overlay folder,
// ResourceSizeTable, HUD letters); the functions below work on R, the image being indexed.
typedef struct {
    RomfsImage *img;         // (runtime/romfs_image.h; NULL: not opened)
    const char *ov_sub;      // where its files are in the overlay folder ("": the game's own)
    Patch patches[8];
    int npatches;
    Overlay *ov;
    int ov_n, ov_cap;
    uint8_t *ov_meta;
    uint64_t ov_meta_off, ov_meta_size, ov_end;
    RomFile *rom_files;
    int rom_nfiles;
    uint64_t rom_data_off;
    GrownFile *grown;
    int ngrown;
    uint8_t *rstb_blob;      // the patched ResourceSizeTable served from memory (rstb_patch)
    uint8_t *hud_pack;       // Pack/Bootup.pack with the HUD letters patched (hud_letters_patch)
    uint8_t *voice_banks;    // the text language's voice banks with the chosen voices' lengths (bars_hybrid)
} RomImage;
#define AOC_MAX 8
static RomImage images[1 + AOC_MAX];  // [0] the game's, [index] add-on content
static RomImage *R = &images[0];

// Reboot: the next boot builds its own index / overlay when the game opens the RomFS again.
RT_ON_RESET(fs_reset) {
    for (int k = 0; k <= AOC_MAX; k++) {
        RomImage *m = &images[k];
        for (int i = 0; i < m->ov_n; i++) {
            if (m->ov[i].fd >= 0) close(m->ov[i].fd);
            free(m->ov[i].path);
        }
        for (int i = 0; i < m->npatches; i++) free(m->patches[i].data);  // (ov_meta and the extended table are among these)
        for (int i = 0; i < m->rom_nfiles; i++) free(m->rom_files[i].path);
        free(m->ov), free(m->rom_files), free(m->grown), free(m->rstb_blob), free(m->hud_pack), free(m->voice_banks);
        romfs_image_close(m->img);
        *m = (RomImage){0};
    }
    R = &images[0];
    g_save_dir = "save";
}

// The RomFS overlay folder (SAVAGEWIND_ROMFS_OVERLAY), or NULL
static const char *overlay_dir(void) {
    const char *e = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    return e && *e ? e : NULL;
}

// Host path of the overlay's replacement for RomFS file `path` (relative, e.g. "Actor/ActorInfo.product.sbyml"); an
// add-on content's files are in the folder's aoc/0010/
static bool overlay_file(const char *path, char *out, size_t cap) {
    const char *dir = overlay_dir();
    if (!dir) return false;
    snprintf(out, cap, "%s/%s%s", dir, R->ov_sub ? R->ov_sub : "", path);
    struct stat sb;
    return stat(out, &sb) == 0 && S_ISREG(sb.st_mode);
}

static int rom_cmp(const void *a, const void *b) {
    const RomFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void rom_dir_path(const uint8_t *dmeta, uint32_t dsize, uint32_t dir, char *out, size_t cap) {
    char parts[16][256];
    int n = 0;
    while (dir != 0 && dir < dsize && n < 16) {  // root (offset 0) has an empty name
        uint32_t parent, namelen;
        memcpy(&parent, dmeta + dir, 4);
        memcpy(&namelen, dmeta + dir + 0x14, 4);
        if (namelen > 255) namelen = 255;
        memcpy(parts[n], dmeta + dir + 0x18, namelen);
        parts[n][namelen] = 0;
        n++;
        dir = parent;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strlcat(out, parts[i], cap); strlcat(out, "/", cap); }
}

// ---- files the overlay ADDS (names the game's RomFS doesn't have). The game parses the
// RomFS tables itself, so they're extended: a copy of the file-meta table with the new entries appended (linked
// into their directory's file list and the name hash buckets) is placed past the end of the image, the header,
// directory table and hash table are patched to match. New files must go into existing directories.
static uint32_t romfs_hash(uint32_t parent, const char *name, uint32_t len) {
    uint32_t hash = parent ^ 123456789;
    for (uint32_t i = 0; i < len; i++) { hash = (hash >> 5) | (hash << 27); hash ^= (uint8_t)name[i]; }
    return hash;
}

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char rel[1024]; char host[1200]; uint64_t size; } NewFile;

static void walk_new(const char *root, const char *rel, char **known, int nknown, NewFile **out, int *n, int *cap) {
    char dirp[1200];
    snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!rel[0] && R == &images[0] && !strcmp(e->d_name, "aoc")) continue;  // (the add-on contents' files)
        char r2[1024], h2[1200];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk_new(root, r2, known, nknown, out, n, cap); continue; }
        const char *key = r2;
        if (bsearch(&key, known, nknown, sizeof *known, path_cmp)) continue;  // replaces an existing file
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, sizeof **out * *cap); }
        strlcpy((*out)[*n].rel, r2, sizeof (*out)[*n].rel);
        strlcpy((*out)[*n].host, h2, sizeof (*out)[*n].host);
        (*out)[*n].size = (uint64_t)sb.st_size;
        (*n)++;
    }
    closedir(d);
}

// directory entry offset for "a/b/c" (NULL-terminated walk from the root, offset 0), -1 if missing
static int64_t dir_lookup(const uint8_t *dm, uint64_t dsz, const char *path) {
    uint32_t cur = 0;
    char buf[1024];
    strlcpy(buf, path, sizeof buf);
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        uint32_t child;
        memcpy(&child, dm + cur + 8, 4);
        while (child != 0xFFFFFFFFu && child + 0x18 <= dsz) {
            uint32_t nl;
            memcpy(&nl, dm + child + 0x14, 4);
            if (nl == strlen(tok) && !memcmp(dm + child + 0x18, tok, nl)) break;
            memcpy(&child, dm + child + 4, 4);
        }
        if (child == 0xFFFFFFFFu || child + 0x18 > dsz) return -1;
        cur = child;
    }
    return cur;
}

static void add_new_files(const uint64_t *h, const uint8_t *dmeta, uint64_t dsz, uint64_t fsz) {
    if (!overlay_dir()) return;
    char root[1100];
    snprintf(root, sizeof root, "%s/%s", overlay_dir(), R->ov_sub ? R->ov_sub : "");
    char **known = malloc(sizeof *known * R->rom_nfiles);
    for (int i = 0; i < R->rom_nfiles; i++) known[i] = R->rom_files[i].path;
    qsort(known, R->rom_nfiles, sizeof *known, path_cmp);
    NewFile *nf = NULL;
    int n = 0, cap = 0;
    walk_new(root, "", known, R->rom_nfiles, &nf, &n, &cap);
    free(known);
    if (!n) return;
    R->rom_files = realloc(R->rom_files, sizeof(RomFile) * (R->rom_nfiles + n));
    uint64_t hoff = h[5], hsz = h[6];
    uint8_t *dcopy = malloc(dsz), *hcopy = malloc(hsz), *head = malloc(0x50);
    memcpy(dcopy, dmeta, dsz);
    if (romfs_image_pread(R->img, hcopy, hsz, hoff) != (ssize_t)hsz || romfs_image_pread(R->img, head, 0x50, 0) != 0x50)
        return;
    // the file-meta table to extend: the override-patched copy if there is one
    const uint8_t *base_meta = R->ov_meta;
    uint8_t *tmp = NULL;
    if (!base_meta) { tmp = malloc(fsz); romfs_image_pread(R->img, tmp, fsz, h[7]); base_meta = tmp; }
    uint64_t extra = 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(nf[i].rel, '/');
        name = name ? name + 1 : nf[i].rel;
        extra += 0x20 + ((strlen(name) + 3) & ~3u);
    }
    uint64_t nsz = fsz + extra;
    uint8_t *meta = calloc(1, nsz);
    memcpy(meta, base_meta, fsz);
    free(tmp);
    if (!R->ov_end) R->ov_end = (romfs_image_size(R->img) + 0xF) & ~0xFull;
    uint64_t meta_at = R->ov_end;  // the new table first, then the new files' data
    uint64_t data_at = (meta_at + nsz + 0xF) & ~0xFull;
    uint32_t nb = (uint32_t)(hsz / 4);
    uint64_t o = fsz;
    int added = 0;
    for (int i = 0; i < n; i++) {
        char dirp[1024];
        strlcpy(dirp, nf[i].rel, sizeof dirp);
        char *slash = strrchr(dirp, '/');
        const char *name = slash ? slash + 1 : nf[i].rel;
        if (slash) *slash = 0; else dirp[0] = 0;
        int64_t dir = dir_lookup(dcopy, dsz, dirp);
        if (dir < 0) { LOG("[fs] overlay file %s: directory not in the game's RomFS, skipped\n", nf[i].rel); continue; }
        uint32_t nl = (uint32_t)strlen(name), d32 = (uint32_t)dir, entry = (uint32_t)o, child;
        memcpy(&child, dcopy + dir + 0xC, 4);  // dir's first file -> becomes our sibling
        uint64_t rel = data_at - R->rom_data_off, size = nf[i].size;
        uint32_t bucket = romfs_hash(d32, name, nl) % nb, prev;
        memcpy(&prev, hcopy + 4 * bucket, 4);
        memcpy(meta + o, &d32, 4);
        memcpy(meta + o + 4, &child, 4);
        memcpy(meta + o + 8, &rel, 8);
        memcpy(meta + o + 0x10, &size, 8);
        memcpy(meta + o + 0x18, &prev, 4);
        memcpy(meta + o + 0x1C, &nl, 4);
        memcpy(meta + o + 0x20, name, nl);
        memcpy(dcopy + dir + 0xC, &entry, 4);
        memcpy(hcopy + 4 * bucket, &entry, 4);
        o += 0x20 + ((nl + 3) & ~3u);
        if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
        R->ov[R->ov_n++] = (Overlay){data_at, size, strdup(nf[i].host), -1, NULL};
        R->rom_files[R->rom_nfiles++] = (RomFile){data_at, size, strdup(nf[i].rel), entry};
        data_at = (data_at + size + 0xF) & ~0xFull;
        added++;
    }
    // the new table goes before the new data (keep the overlay list sorted by start)
    if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
    memmove(R->ov + (R->ov_n - added) + 1, R->ov + (R->ov_n - added), sizeof *R->ov * added);
    R->ov[R->ov_n - added] = (Overlay){meta_at, nsz, NULL, -1, meta};
    R->ov_n++;
    R->ov_end = data_at;
    uint64_t fmeta_off = meta_at, fmeta_size = o;
    memcpy(head + 7 * 8, &fmeta_off, 8);
    memcpy(head + 8 * 8, &fmeta_size, 8);
    R->patches[R->npatches++] = (Patch){0, 0x50, head};
    R->patches[R->npatches++] = (Patch){h[3], dsz, dcopy};
    R->patches[R->npatches++] = (Patch){hoff, hsz, hcopy};
    LOG("[fs] overlay: %d new romfs files\n", added);
}

// The copy of the file-meta table that replacements patch (made on first use)
static uint8_t *meta_copy(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    if (!R->ov_meta) {
        R->ov_meta = malloc(fsz);
        memcpy(R->ov_meta, fmeta, fsz);
        R->ov_meta_off = foff;
        R->ov_meta_size = fsz;
        R->ov_end = (romfs_image_size(R->img) + 0xF) & ~0xFull;
    }
    return R->ov_meta;
}

static int by_name_after(const void *a, const void *b) {  // (RomFile * by the path after "Voice/<lang>/")
    const RomFile *x = *(RomFile *const *)a, *y = *(RomFile *const *)b;
    return strcmp(strchr(x->path + 6, '/') + 1, strchr(y->path + 6, '/') + 1);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// One AMTA (a line's metadata) of a little-endian BARS bank: its offset, line name, DATA and MARK sections, or false
typedef struct { uint32_t at, data, mark; const char *name; } Amta;
static bool bars_amta(const uint8_t *d, size_t n, uint32_t i, Amta *a) {
    uint32_t cnt = rd32(d + 12);
    if (16 + (uint64_t)cnt * 12 > n || i >= cnt) return false;
    uint32_t at = rd32(d + 16 + cnt * 4 + i * 8), wave = rd32(d + 20 + cnt * 4 + i * 8);
    if (wave != 0xFFFFFFFFu || (uint64_t)at + 0x1C > n || memcmp(d + at, "AMTA\xFF\xFE", 6)) return false;  // (streamed)
    uint64_t data = at + (uint64_t)rd32(d + at + 0xC), mark = at + (uint64_t)rd32(d + at + 0x10);
    uint64_t strg = at + (uint64_t)rd32(d + at + 0x18);
    if (data + 0x24 > n || memcmp(d + data, "DATA", 4) || mark + 12 > n || memcmp(d + mark, "MARK", 4) ||
        mark + 12 + (uint64_t)rd32(d + mark + 8) * 16 > n || strg + 9 > n || memcmp(d + strg, "STRG", 4) ||
        !memchr(d + strg + 8, 0, n - strg - 8))
        return false;
    *a = (Amta){at, (uint32_t)data, (uint32_t)mark, (const char *)d + strg + 8};
    return true;
}

// The game turns a cutscene subtitle's pages at the markers (AMTA MARK: {id, name, sample, length}) of the line in its
// voice bank, Voice/<lang>/<Demo>.bars; the markers are timed for that folder's own text, tagged with the text languages
// they serve ("USen,EUen", "EUnl"), and the banks hold no audio (that is in Stream_*/). Another language's bank has
// other markers: pt-BR text with Japanese voices showed 5 of King Rhoam's 13 pages (Demo154_1, #169). So the bank
// the game reads stays its own (`own`, patched in place) with each line's DATA (its length in samples, the only field
// that differs) from the chosen voices' bank, and its markers moved by the ratio of the two lengths. Its size doesn't
// change, so the ResourceSizeTable fits. False for a bank laid out otherwise (the NV_Custom_* ones carry their sounds):
// that one is redirected whole.
static bool bars_hybrid(uint8_t *own, size_t on, const uint8_t *vb, size_t vn) {
    if (on < 16 || vn < 16 || memcmp(own, "BARS", 4) || memcmp(vb, "BARS", 4) || memcmp(own + 8, "\xFF\xFE", 2) ||
        memcmp(vb + 8, "\xFF\xFE", 2))
        return false;
    uint32_t cnt = rd32(own + 12), vcnt = rd32(vb + 12);
    Amta a, v;
    for (uint32_t i = 0; i < vcnt; i++)
        if (!bars_amta(vb, vn, i, &v)) return false;
    for (uint32_t i = 0; i < cnt; i++) {
        if (!bars_amta(own, on, i, &a)) return false;
        uint32_t j = 0;
        while (j < vcnt && bars_amta(vb, vn, j, &v) && strcmp(v.name, a.name)) j++;
        if (j == vcnt) continue;  // (a line the voices don't have: left as is)
        uint64_t dn = 8 + (uint64_t)rd32(vb + v.data + 4);
        if (rd32(own + a.data + 4) != dn - 8 || a.data + dn > on || v.data + dn > vn) continue;
        uint64_t len = rd32(own + a.data + 0x20), vlen = rd32(vb + v.data + 0x20);
        memcpy(own + a.data, vb + v.data, dn);
        if (!len) continue;
        for (uint32_t m = 0, nm = rd32(own + a.mark + 8); m < nm; m++)
            for (int f = 8; f <= 12; f += 4) {  // (sample, length)
                uint8_t *p = own + a.mark + 12 + m * 16 + f;
                uint32_t s = (uint32_t)((rd32(p) * vlen + len / 2) / len);
                memcpy(p, &s, 4);
            }
    }
    return true;
}

// Voices in another language than the text (runtime/language.c): every Voice/<lang>/<path> but Common's takes the
// data of the chosen folder's <path> when it has one (the .bars banks and the Stream_*/ dialogue streams). (The game reads its text language's folder or a fallback;
// whichever it is, it gets the chosen voices.) Only the file-meta entries change: the data stays in the image, but
// for the banks that keep their own subtitle markers (bars_hybrid), served from memory.
static void voice_redirect(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    const char *vf = language_voice_folder();
    if (!vf) return;
    char pre[64];
    snprintf(pre, sizeof pre, "Voice/%s/", vf);
    size_t pl = strlen(pre);
    RomFile **chosen = malloc(sizeof *chosen * R->rom_nfiles);
    int nc = 0;
    for (int i = 0; i < R->rom_nfiles; i++)
        if (!strncmp(R->rom_files[i].path, pre, pl)) chosen[nc++] = &R->rom_files[i];
    if (!nc) { LOG("[fs] voices: the RomFS has no %s folder, the game's own voices play\n", pre); free(chosen); return; }
    qsort(chosen, nc, sizeof *chosen, by_name_after);
    int n = 0, nh = 0;
    struct Hybrid { int file; uint8_t *data; } *hy = malloc(sizeof *hy * R->rom_nfiles);
    for (int i = 0; i < R->rom_nfiles; i++) {
        const char *p = R->rom_files[i].path;
        if (strncmp(p, "Voice/", 6) || !strncmp(p, pre, pl) || !strncmp(p, "Voice/Common/", 13)) continue;
        if (!strchr(p + 6, '/')) continue;
        RomFile *key = &R->rom_files[i], **hit = bsearch(&key, chosen, nc, sizeof *chosen, by_name_after);
        if (!hit) continue;
        size_t pn = strlen(p), on = R->rom_files[i].size, vn = (*hit)->size;
        if (pn > 5 && !strcmp(p + pn - 5, ".bars") && on < (1u << 20) && vn < (1u << 20)) {
            uint8_t *own = malloc(on ? on : 1), *vb = malloc(vn ? vn : 1);
            bool ok = romfs_image_pread(R->img, own, on, R->rom_files[i].off) == (ssize_t)on &&
                      romfs_image_pread(R->img, vb, vn, (*hit)->off) == (ssize_t)vn && bars_hybrid(own, on, vb, vn);
            free(vb);
            if (ok) { hy[nh++] = (struct Hybrid){i, own}; n++; continue; }
            free(own);
        }
        uint8_t *meta = meta_copy(fmeta, foff, fsz);
        memcpy(meta + R->rom_files[i].meta + 8, fmeta + (*hit)->meta + 8, 16);  // (data offset, size)
        if ((*hit)->size > R->rom_files[i].size) {
            R->grown = realloc(R->grown, sizeof *R->grown * (R->ngrown + 1));
            R->grown[R->ngrown++] = (GrownFile){p, (*hit)->size - R->rom_files[i].size};
        }
        n++;
    }
    free(chosen);
    if (nh) {  // the banks, one after another past the end of the image
        uint8_t *meta = meta_copy(fmeta, foff, fsz);
        uint64_t total = 0;
        for (int k = 0; k < nh; k++) total += (R->rom_files[hy[k].file].size + 0xF) & ~0xFull;
        R->voice_banks = calloc(1, total);
        for (uint64_t k = 0, at = 0; k < (uint64_t)nh; k++) {
            RomFile *f = &R->rom_files[hy[k].file];
            uint64_t rel = R->ov_end + at - R->rom_data_off;
            memcpy(R->voice_banks + at, hy[k].data, f->size);
            memcpy(meta + f->meta + 8, &rel, 8);
            free(hy[k].data);
            at += (f->size + 0xF) & ~0xFull;
        }
        if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
        R->ov[R->ov_n++] = (Overlay){R->ov_end, total, NULL, -1, R->voice_banks};
        R->ov_end = (R->ov_end + total + 0xF) & ~0xFull;
    }
    free(hy);
    LOG("[fs] voices: %d files play %s's voices (%d banks keep the text's subtitle markers)\n", n, vf, nh);
}

// The game sets aside memory for a resource by its path's entry in System/Resource/ResourceSizeTable (the RSTB: the
// file's size plus room for what it builds from it, ~850 bytes for a voice bank). A redirected bank bigger than the
// path's own file then runs out: the 0xa8-byte object that parses it (after the 'BARS' check) gets no memory and the
// game writes through the null pointer ("accessing 0x10": pt-BR text with Japanese voices, at the title screen, where
// it loads the Demo*.bars banks). So the RSTB the game reads is a copy whose entries for those paths grow by as much
// as their data did (and a little: the room varies with alignment), served from memory like Version.txt. Its format:
// Yaz0 over "RSTB", u32 CRC count, u32 name count, {crc32(path), size} sorted by crc32, then {char[128], size}.
static uint8_t *yaz0_unpack(const uint8_t *d, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(d, "Yaz0", 4)) return NULL;
    size_t size = (size_t)d[4] << 24 | (size_t)d[5] << 16 | (size_t)d[6] << 8 | d[7], s = 16, w = 0;
    uint8_t *o = malloc(size ? size : 1);
    while (w < size && s < n) {
        uint8_t code = d[s++];
        for (int b = 0; b < 8 && w < size && s < n; b++) {
            if (code & (0x80 >> b)) { o[w++] = d[s++]; continue; }
            if (s + 1 >= n) break;
            size_t dist = (size_t)((d[s] & 0xF) << 8 | d[s + 1]) + 1, cnt = d[s] >> 4;
            s += 2;
            if (!cnt) cnt = s < n ? (size_t)d[s++] + 0x12 : 0;
            else cnt += 2;
            if (dist > w) { free(o); return NULL; }  // (damaged)
            for (size_t i = 0; i < cnt && w < size; i++, w++) o[w] = o[w - dist];
        }
    }
    if (w < size) { free(o); return NULL; }
    *out_n = size;
    return o;
}

// Yaz0 with literal bytes only (a 0xFF flag before every 8): what the game's decoder reads, no search needed
static uint8_t *yaz0_pack_literal(const uint8_t *d, size_t n, size_t *out_n) {
    uint8_t *o = malloc(16 + n + (n + 7) / 8), *w = o;
    memcpy(w, "Yaz0", 4);
    w[4] = (uint8_t)(n >> 24), w[5] = (uint8_t)(n >> 16), w[6] = (uint8_t)(n >> 8), w[7] = (uint8_t)n;
    memset(w + 8, 0, 8);
    w += 16;
    for (size_t i = 0; i < n; i += 8) {
        size_t k = n - i < 8 ? n - i : 8;
        *w++ = 0xFF;
        memcpy(w, d + i, k);
        w += k;
    }
    *out_n = (size_t)(w - o);
    return o;
}

// Yaz0 with back-references (greedy, hash chains over the 4 KB window): for files whose size the game holds to
// (a pack's resources are read into buffers of about their size). hdr: the original's 16 bytes (its alignment).
static uint8_t *yaz0_pack(const uint8_t *d, size_t n, const uint8_t *hdr, size_t *out_n) {
    enum { WIN = 0x1000, MAXM = 0x111, HBITS = 16, DEPTH = 48 };
    uint8_t *o = malloc(16 + n + (n + 7) / 8 + 16);
    int32_t *head = malloc(sizeof *head << HBITS), *prev = malloc(sizeof *prev * WIN);
    memset(head, 0xff, sizeof *head << HBITS);
    memcpy(o, hdr, 16);
    o[4] = (uint8_t)(n >> 24), o[5] = (uint8_t)(n >> 16), o[6] = (uint8_t)(n >> 8), o[7] = (uint8_t)n;
    size_t w = 16, s = 0;
#define YHASH(p) ((uint32_t)((d[p] << 16 | d[(p) + 1] << 8 | d[(p) + 2]) * 2654435761u) >> (32 - HBITS))
#define YADD(p) do { if ((p) + 2 < n) { uint32_t h_ = YHASH(p); prev[(p) & (WIN - 1)] = head[h_]; head[h_] = (int32_t)(p); } } while (0)
    while (s < n) {
        size_t flag_at = w++;
        uint8_t flag = 0;
        for (int b = 0; b < 8 && s < n; b++) {
            size_t best = 0, dist = 0;
            if (s + 2 < n) {
                size_t lim = n - s < MAXM ? n - s : MAXM;
                int32_t c = head[YHASH(s)];
                for (int k = 0; k < DEPTH && c >= 0 && s - (size_t)c <= WIN; k++) {
                    size_t l = 0;
                    while (l < lim && d[c + l] == d[s + l]) l++;
                    if (l > best) { best = l, dist = s - (size_t)c; if (l == lim) break; }
                    int32_t nx = prev[c & (WIN - 1)];
                    if (nx >= c) break;  // (the ring slot was reused: older than the window)
                    c = nx;
                }
            }
            if (best < 3) {
                flag |= (uint8_t)(0x80 >> b);
                o[w++] = d[s];
                YADD(s);
                s++;
                continue;
            }
            size_t dd = dist - 1;
            if (best >= 0x12) {
                o[w++] = (uint8_t)(dd >> 8), o[w++] = (uint8_t)dd, o[w++] = (uint8_t)(best - 0x12);
            } else {
                o[w++] = (uint8_t)((best - 2) << 4 | dd >> 8), o[w++] = (uint8_t)dd;
            }
            for (size_t i = 0; i < best; i++) YADD(s + i);
            s += best;
        }
        o[flag_at] = flag;
    }
#undef YHASH
#undef YADD
    free(head);
    free(prev);
    *out_n = w;
    return o;
}

// SARC (little-endian): the node of `name` (its index; start and end in the data, which begins at *data_off), or -1
static int sarc_find(const uint8_t *d, size_t n, const char *name, uint32_t *data_off, uint32_t *start, uint32_t *end) {
    if (n < 0x20 || memcmp(d, "SARC", 4) || d[6] != 0xFF || d[7] != 0xFE || memcmp(d + 0x14, "SFAT", 4)) return -1;
    uint32_t doff, cnt = d[0x1A] | d[0x1B] << 8;
    memcpy(&doff, d + 0xC, 4);
    size_t names = 0x20 + (size_t)cnt * 16 + 8;
    if (names > n || doff > n) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t attr, s, e;
        memcpy(&attr, d + 0x20 + i * 16 + 4, 4);
        memcpy(&s, d + 0x20 + i * 16 + 8, 4);
        memcpy(&e, d + 0x20 + i * 16 + 12, 4);
        size_t at = names + (size_t)(attr & 0xFFFFFF) * 4;
        if (!(attr >> 24) || at >= n || strncmp((const char *)d + at, name, n - at)) continue;
        if (s > e || doff + (uint64_t)e > n) return -1;
        *data_off = doff, *start = s, *end = e;
        return (int)i;
    }
    return -1;
}

// The HUD's prompt with its diamond of the other buttons (Layout/Common.sblarc, PaDoCommandA/B/X/Y_00: "Pick up",
// "Talk", the rune's "Activate", the paraglider) writes the dim A / B / X / Y around it as text in the HUD's normal
// font: the Switch's letters with any button icons. They become the button characters of the icon font
// (External_00.bffnt, U+E040..E043, which glyphs.c redraws for the device in use): the layouts get that font as a
// second one, and those text panes use it. The patched layouts go at the end of the archive, the archive is
// recompressed into Pack/Bootup.pack where it was (what follows it moves by a multiple of 0x2000, so every file keeps
// its alignment) and the pack is served from memory; the archive's ResourceSizeTable entry grows with it.
// SAVAGEWIND_NO_HUD_LETTERS=1: the game's letters.

#define HUD_ICON_SCALE 1.5f

// One layout (FLYT, little-endian, a single font), patched into out (room for n + 64 bytes); returns its size or 0
static size_t hud_letters_flyt(const uint8_t *b, size_t n, uint8_t *out, int *panes) {
    static const char icon_font[] = "External_00.bffnt";
    if (n < 0x14 || memcmp(b, "FLYT", 4) || b[4] != 0xFF || b[5] != 0xFE) return 0;
    size_t hs = b[6] | b[7] << 8, o = hs, w = hs;
    if (hs > n) return 0;
    memcpy(out, b, hs);
    bool fonts = false;
    while (o + 8 <= n) {
        uint32_t sz;
        memcpy(&sz, b + o + 4, 4);
        if (sz < 8 || o + sz > n) return 0;
        uint8_t *sec = out + w;
        if (!memcmp(b + o, "fnl1", 4)) {
            uint32_t first;
            memcpy(&first, b + o + 12, 4);
            if ((b[o + 8] | b[o + 9] << 8) != 1 || 12 + first >= sz) return 0;
            const char *name0 = (const char *)b + o + 12 + first;
            size_t l0 = strnlen(name0, sz - 12 - first) + 1, l1 = sizeof icon_font;
            uint32_t total = (uint32_t)((12 + 8 + l0 + l1 + 3) & ~3u), off0 = 8, off1 = (uint32_t)(8 + l0);
            memset(sec, 0, total);
            memcpy(sec, "fnl1", 4);
            memcpy(sec + 4, &total, 4);
            sec[8] = 2;
            memcpy(sec + 12, &off0, 4);
            memcpy(sec + 16, &off1, 4);
            memcpy(sec + 20, name0, l0);
            memcpy(sec + 20 + l0, icon_font, l1);
            w += total;
            o += sz;
            fonts = true;
            continue;
        }
        memcpy(sec, b + o, sz);
        if (!memcmp(sec, "txt1", 4) && sz >= 0x68) {
            uint32_t toff;
            memcpy(&toff, sec + 0x64, 4);
            if (toff && toff + 4 <= sz && !sec[toff + 1] && !sec[toff + 2] && !sec[toff + 3]) {
                const char *k = strchr("ABXY", sec[toff]);
                if (k && *k) {
                    sec[0x5A] = 1, sec[0x5B] = 0;  // (the font: the second)
                    uint16_t c = (uint16_t)(0xE040 + (k - "ABXY"));
                    memcpy(sec + toff, &c, 2);
                    float fs[2];  // the font size: an icon fills less of its cell than a letter
                    memcpy(fs, sec + 0x70, 8);
                    fs[0] *= HUD_ICON_SCALE, fs[1] *= HUD_ICON_SCALE;
                    memcpy(sec + 0x70, fs, 8);
                    (*panes)++;
                }
            }
        }
        w += sz;
        o += sz;
    }
    if (!fonts || w > n + 64) return 0;
    uint32_t total = (uint32_t)w;
    memcpy(out + 0xC, &total, 4);
    return w;
}

static void hud_letters_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    static const char pack_path[] = "Pack/Bootup.pack";
    static const char *const layouts[] = {"blyt/PaDoCommandA_00.bflyt", "blyt/PaDoCommandB_00.bflyt",
                                          "blyt/PaDoCommandX_00.bflyt", "blyt/PaDoCommandY_00.bflyt"};
    if (getenv("SAVAGEWIND_NO_HUD_LETTERS")) return;
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, pack_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles || overlay_file(pack_path, host, sizeof host)) return;
    uint64_t t0 = plat_now_ns();
    size_t pn = R->rom_files[k].size, zn = 0, an = 0, nn = 0;
    uint8_t *pack = malloc(pn), *arc = NULL, *arc2 = NULL, *z2 = NULL;
    uint32_t pdoff, ps, pe, adoff, as, ae;
    int node = -1, panes = 0;
    if (romfs_image_pread(R->img, pack, pn, R->rom_files[k].off) == (ssize_t)pn)
        node = sarc_find(pack, pn, "Layout/Common.sblarc", &pdoff, &ps, &pe);
    if (node >= 0) arc = yaz0_unpack(pack + pdoff + ps, pe - ps, &an);
    const char *why = node < 0 ? "no Layout/Common.sblarc" : !arc ? "archive not readable" : NULL;
    if (!why) {
        arc2 = malloc(an + 4 * (0x80 + 0x2000));
        memcpy(arc2, arc, an);
        nn = an;
        for (int i = 0; i < 4 && !why; i++) {
            int li = sarc_find(arc, an, layouts[i], &adoff, &as, &ae);
            if (li < 0 || ae - as > 0x2000 - 64) { why = "layout not found"; break; }
            size_t at = (nn + 0x7F) & ~(size_t)0x7F;
            memset(arc2 + nn, 0, at - nn);
            nn = at;
            size_t ln = hud_letters_flyt(arc + adoff + as, ae - as, arc2 + nn, &panes);
            if (!ln) { why = "layout not as expected"; break; }
            uint32_t s = (uint32_t)(nn - adoff), e = (uint32_t)(nn - adoff + ln);
            memcpy(arc2 + 0x20 + li * 16 + 8, &s, 4);
            memcpy(arc2 + 0x20 + li * 16 + 12, &e, 4);
            nn += ln;
        }
    }
    if (!why && panes != 12) why = "letters not as expected";
    if (why) {
        LOG("[fs] HUD letters: %s, the game's letters stay\n", why);
        free(pack), free(arc), free(arc2);
        return;
    }
    uint32_t total = (uint32_t)nn;
    memcpy(arc2 + 8, &total, 4);
    z2 = yaz0_pack(arc2, nn, pack + pdoff + ps, &zn);
    // the pack: the new archive where the old one was, what follows moved by delta (a multiple of 0x2000)
    uint64_t old = pe - ps, delta = zn > old ? (zn - old + 0x1FFF) & ~0x1FFFull : 0;
    size_t qn = pn + delta;
    free(R->hud_pack);
    R->hud_pack = calloc(1, qn);
    memcpy(R->hud_pack, pack, pdoff + ps);
    memcpy(R->hud_pack + pdoff + ps, z2, zn);
    memcpy(R->hud_pack + pdoff + pe + delta, pack + pdoff + pe, pn - pdoff - pe);
    uint32_t cnt = pack[0x1A] | pack[0x1B] << 8, ne = (uint32_t)(ps + zn), q32 = (uint32_t)qn;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t s, e;
        memcpy(&s, R->hud_pack + 0x20 + i * 16 + 8, 4);
        memcpy(&e, R->hud_pack + 0x20 + i * 16 + 12, 4);
        if (i == (uint32_t)node) { memcpy(R->hud_pack + 0x20 + i * 16 + 12, &ne, 4); continue; }
        if (s >= pe) s += (uint32_t)delta, e += (uint32_t)delta;
        memcpy(R->hud_pack + 0x20 + i * 16 + 8, &s, 4);
        memcpy(R->hud_pack + 0x20 + i * 16 + 12, &e, 4);
    }
    memcpy(R->hud_pack + 8, &q32, 4);
    free(pack), free(arc), free(arc2), free(z2);
    meta_copy(fmeta, foff, fsz);
    uint64_t rel = R->ov_end - R->rom_data_off, size = qn;
    memcpy(R->ov_meta + R->rom_files[k].meta + 8, &rel, 8);
    memcpy(R->ov_meta + R->rom_files[k].meta + 0x10, &size, 8);
    if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
    R->ov[R->ov_n++] = (Overlay){R->ov_end, size, NULL, -1, R->hud_pack};
    R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
    R->grown = realloc(R->grown, sizeof *R->grown * (R->ngrown + 1));
    R->grown[R->ngrown++] = (GrownFile){"Layout/Common.blarc", nn - an};
    LOG("[fs] HUD letters: %d button letters use the icon font (archive %zu -> %zu bytes, pack +%llu KB, %.0f ms)\n",
        panes, (size_t)old, zn, (unsigned long long)(delta >> 10), (plat_now_ns() - t0) / 1e6);
}

static void rstb_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    static const char rstb_path[] = "System/Resource/ResourceSizeTable.product.srsizetable";
    if (!R->ngrown) return;
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, rstb_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles && R != &images[0]) {  // (an add-on content's voices are streams: sized from the file)
        LOG("[fs] voices: %d bigger files, none in a ResourceSizeTable\n", R->ngrown);
        return;
    }
    if (k == R->rom_nfiles) { LOG("[fs] voices: no %s, %d bigger banks left as they are\n", rstb_path, R->ngrown); return; }
    if (overlay_file(rstb_path, host, sizeof host)) {
        LOG("[fs] voices: the overlay folder replaces the ResourceSizeTable: %d bigger banks left to it\n", R->ngrown);
        return;
    }
    size_t zn = R->rom_files[k].size, n = 0;
    uint8_t *z = malloc(zn), *d = NULL;
    if (romfs_image_pread(R->img, z, zn, R->rom_files[k].off) == (ssize_t)zn) d = yaz0_unpack(z, zn, &n);
    free(z);
    uint32_t ncrc = 0, nname = 0;
    if (d && n >= 12) memcpy(&ncrc, d + 4, 4), memcpy(&nname, d + 8, 4);
    if (!d || memcmp(d, "RSTB", 4) || 12 + (uint64_t)ncrc * 8 + (uint64_t)nname * 132 > n) {
        LOG("[fs] voices: ResourceSizeTable not as expected, %d bigger banks left as they are\n", R->ngrown);
        free(d);
        return;
    }
    int changed = 0;
    uint64_t added = 0;
    for (int g = 0; g < R->ngrown; g++) {
        uint32_t grow = (uint32_t)((R->grown[g].grow + 31) & ~31ull) + 32, *size = NULL;
        for (uint32_t i = 0; i < nname && !size; i++)  // (names first: paths whose crc32 collides)
            if (!strncmp((const char *)d + 12 + ncrc * 8 + i * 132, R->grown[g].path, 128))
                size = (uint32_t *)(d + 12 + ncrc * 8 + i * 132 + 128);
        uint32_t crc = (uint32_t)crc32(0, (const Bytef *)R->grown[g].path, (uInt)strlen(R->grown[g].path));
        for (uint32_t lo = 0, hi = ncrc; !size && lo < hi;) {
            uint32_t mid = (lo + hi) / 2, c;
            memcpy(&c, d + 12 + mid * 8, 4);
            if (c == crc) size = (uint32_t *)(d + 12 + mid * 8 + 4);
            else if (c < crc) lo = mid + 1;
            else hi = mid;
        }
        if (!size) continue;  // (no entry: the game sizes it from the file, the dialogue streams)
        uint32_t v;
        memcpy(&v, size, 4);
        v += grow;
        memcpy(size, &v, 4);
        changed++;
        added += grow;
    }
    size_t bn = 0;
    free(R->rstb_blob);
    R->rstb_blob = yaz0_pack_literal(d, n, &bn);
    free(d);
    meta_copy(fmeta, foff, fsz);
    uint64_t rel = R->ov_end - R->rom_data_off, size = bn;
    memcpy(R->ov_meta + R->rom_files[k].meta + 8, &rel, 8);
    memcpy(R->ov_meta + R->rom_files[k].meta + 0x10, &size, 8);
    if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
    R->ov[R->ov_n++] = (Overlay){R->ov_end, size, NULL, -1, R->rstb_blob};
    R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
    LOG("[fs] voices: %d files bigger than the game's own, %d ResourceSizeTable entries grown (%llu KB in all)\n",
        R->ngrown, changed, (unsigned long long)(added >> 10));
}

static void rom_index(void) {
    uint64_t h[10];
    if (romfs_image_pread(R->img, h, sizeof h, 0) != (ssize_t)sizeof h) return;
    // header: size, dir hash (off,size), dir meta (off,size), file hash (off,size), file meta (off,size), data off
    uint64_t doff = h[3], dsz = h[4], foff = h[7], fsz = h[8];
    R->rom_data_off = h[9];
    uint8_t *dmeta = malloc(dsz), *fmeta = malloc(fsz);
    if (romfs_image_pread(R->img, dmeta, dsz, doff) != (ssize_t)dsz ||
        romfs_image_pread(R->img, fmeta, fsz, foff) != (ssize_t)fsz)
        return;
    int cap = 4096;
    R->rom_files = malloc(sizeof(RomFile) * cap);
    for (uint64_t o = 0; o + 0x20 <= fsz;) {
        uint32_t parent, namelen;
        uint64_t data, size;
        memcpy(&parent, fmeta + o, 4);
        memcpy(&data, fmeta + o + 8, 8);
        memcpy(&size, fmeta + o + 0x10, 8);
        memcpy(&namelen, fmeta + o + 0x1C, 4);
        char dir[1024], name[256];
        rom_dir_path(dmeta, (uint32_t)dsz, parent, dir, sizeof dir);
        uint32_t nl = namelen > 255 ? 255 : namelen;
        memcpy(name, fmeta + o + 0x20, nl);
        name[nl] = 0;
        if (R->rom_nfiles == cap) { cap *= 2; R->rom_files = realloc(R->rom_files, sizeof(RomFile) * cap); }
        char *path = malloc(strlen(dir) + nl + 1);
        strcpy(path, dir);
        strcat(path, name);
        R->rom_files[R->rom_nfiles++] = (RomFile){R->rom_data_off + data, size, path, (uint32_t)o};
        o += 0x20 + ((namelen + 3) & ~3u);
    }
    voice_redirect(fmeta, foff, fsz);
    // Overlay: replacement files are served from the overlay folder. The file-meta table is patched
    // (in a copy) to point them at data appended past the end of the image; reads there go to host files.
    for (int i = 0; i < R->rom_nfiles; i++) {
        char host[1200];
        if (!overlay_file(R->rom_files[i].path, host, sizeof host)) continue;
        struct stat sb;
        if (stat(host, &sb)) continue;
        meta_copy(fmeta, foff, fsz);
        uint64_t rel = R->ov_end - R->rom_data_off, size = (uint64_t)sb.st_size;
        memcpy(R->ov_meta + R->rom_files[i].meta + 8, &rel, 8);
        memcpy(R->ov_meta + R->rom_files[i].meta + 0x10, &size, 8);
        if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
        R->ov[R->ov_n].start = R->ov_end;
        R->ov[R->ov_n].size = size;
        R->ov[R->ov_n].path = strdup(host);
        R->ov[R->ov_n].fd = -1;
        R->ov[R->ov_n].mem = NULL;
        R->ov_n++;
        R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
    }
    // The title screen's version line shows System/Version.txt ("1.8.2") through "Ver.%s": it reads "Savage Wind
    // <major>.<minor>" instead (the "Ver." goes in fs_title_version_patch), unless the overlay folder replaces the
    // file. The line's text box holds 15 characters, so the full version is only in Savage Wind Configuration.
    for (int i = 0; i < R->rom_nfiles; i++) {
        if (strcmp(R->rom_files[i].path, "System/Version.txt")) continue;
        char host[1200];
        struct stat sb;
        if (overlay_file(R->rom_files[i].path, host, sizeof host) && !stat(host, &sb)) break;
        extern const char *rt_version(void);  // (runtime/ui/launcher.mm)
        static char text[64];
        const char *v = rt_version(), *dot = strchr(v, '.');
        int n = dot ? (int)strcspn(dot + 1, ".-") + (int)(dot - v) + 1 : (int)strlen(v);  // (up to the second '.')
        snprintf(text, sizeof text, "Savage Wind %.*s", n, v);
        if (strlen(text) > 15) snprintf(text, sizeof text, "Savage Wind");
        meta_copy(fmeta, foff, fsz);
        uint64_t rel = R->ov_end - R->rom_data_off, size = strlen(text);
        memcpy(R->ov_meta + R->rom_files[i].meta + 8, &rel, 8);
        memcpy(R->ov_meta + R->rom_files[i].meta + 0x10, &size, 8);
        if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
        R->ov[R->ov_n++] = (Overlay){R->ov_end, size, NULL, -1, (const uint8_t *)text};
        R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
        break;
    }
    hud_letters_patch(fmeta, foff, fsz);
    rstb_patch(fmeta, foff, fsz);
    if (R->ov_n) LOG("[fs] overlay: %d romfs files replaced\n", R->ov_n);
    if (R->ov_meta) { R->patches[R->npatches++] = (Patch){R->ov_meta_off, R->ov_meta_size, R->ov_meta}; }
    add_new_files(h, dmeta, dsz, fsz);
    qsort(R->rom_files, R->rom_nfiles, sizeof *R->rom_files, rom_cmp);
    free(dmeta);
    free(fmeta);
    LOG("[fs] romfs index: %d files\n", R->rom_nfiles);
}

// The title screen's version line: main formats System/Version.txt with "Ver.%s" (title_version_format; 1.8.2
// 0x71014b854e in the Ghidra project, read by FUN_7100d4b57c and FUN_7100d4bca0); "%s" leaves just "Savage Wind
// <version>". Called once main is loaded; anything else at that address is left alone.
void fs_title_version_patch(void) {
    char *fmt = (char *)(uintptr_t)g_site.title_version_format;
    if (memcmp(fmt, "Ver.%s", 7)) { LOG("[fs] title version: \"Ver.%%s\" not found, left as is\n"); return; }
    memcpy(fmt, "%s", 3);
}

// Virtual RomFS image read: original bytes, with the patched file table and appended replacement files.
static ssize_t romfs_read(RomImage *m, uint8_t *dst, uint64_t size, uint64_t off) {
    if (!m->ov_n && !m->npatches) return romfs_image_pread(m->img, dst, size, off);
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (m->ov_n && o >= m->ov[0].start) {  // appended region
            int i = 0;
            while (i < m->ov_n && !(o >= m->ov[i].start && o < m->ov[i].start + ((m->ov[i].size + 0xF) & ~0xFull))) i++;
            if (i == m->ov_n) { memset(dst + done, 0, n); done += n; break; }
            uint64_t in = o - m->ov[i].start, avail = ((m->ov[i].size + 0xF) & ~0xFull) - in;
            if (n > avail) n = avail;
            ssize_t r;
            if (m->ov[i].mem) {  // in-memory blob (the extended file table)
                r = in < m->ov[i].size ? (ssize_t)(n < m->ov[i].size - in ? n : m->ov[i].size - in) : 0;
                memcpy(dst + done, m->ov[i].mem + in, (size_t)r);
            } else {
                if (m->ov[i].fd < 0) m->ov[i].fd = open(m->ov[i].path, O_RDONLY);
                r = in < m->ov[i].size ? pread(m->ov[i].fd, dst + done, n < m->ov[i].size - in ? n : m->ov[i].size - in, (off_t)in) : 0;
            }
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
        } else {
            if (m->ov_n && o + n > m->ov[0].start) n = m->ov[0].start - o;
            ssize_t r = romfs_image_pread(m->img, dst + done, n, o);
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            // overlay the patched tables (file meta, and with added files: header, dir meta, file hash)
            for (int k = 0; k < m->npatches; k++) {
                uint64_t a = o > m->patches[k].off ? o : m->patches[k].off;
                uint64_t b = o + n < m->patches[k].off + m->patches[k].size ? o + n : m->patches[k].off + m->patches[k].size;
                if (a < b) memcpy(dst + done + (a - o), m->patches[k].data + (a - m->patches[k].off), b - a);
            }
        }
        done += n;
    }
    return (ssize_t)done;
}

// Path of the game's RomFS file containing image offset `off` (NULL if none).
const char *romfs_file_at(uint64_t off) {
    const RomImage *m = &images[0];
    int lo = 0, hi = m->rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < m->rom_files[mid].off) hi = mid - 1;
        else if (off >= m->rom_files[mid].off + m->rom_files[mid].size) lo = mid + 1;
        else return m->rom_files[mid].path;
    }
    return NULL;
}

// ------------------------------------------------------------------ IStorage (raw RomFS image)
typedef struct { RomImage *img; uint64_t size; } Storage;

static void storage(Service *s, IpcReq *req, IpcResp *resp) {
    Storage *st = s->state;
    RomImage *m = st->img;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        kernel_watch_write("romfs read", b->addr, size);
        ssize_t n = romfs_read(m, (uint8_t *)(uintptr_t)b->addr, size, off);
        if (n < 0) LOG("[fs] storage read failed at %llx: %s\n", off, strerror(errno));
        if (m != &images[0]) break;
        {
            const char *path = romfs_file_at(off);
            static const char *last;
            if (path && path != last) {
                last = path;
                if (getenv("SAVAGEWIND_LOG_FILES")) LOG("[fs] read %s\n", path);
            }
        }
        static uint64_t reads, bytes;
        reads++;
        bytes += size;
        if ((reads & (reads - 1)) == 0) LOG("[fs] romfs reads: %llu (%llu KB)\n", reads, bytes >> 10);
        break;
    }
    case 4: OUT(uint64_t, 0) = m->ov_n ? m->ov_end : st->size; resp_size(resp, 8); break;  // GetSize
    default: LOG("[fs] IStorage cmd %u (ok)\n", req->cmd);
    }
}

// Opens image k from the folder dir (indexed and patched the first time); NULL when it can't be read.
static Service *open_image(int k, const char *dir, const char *name) {
    RomImage *m = &images[k];
    if (!m->img) {
        char why[256] = "";
        uint64_t t0 = plat_now_ns();
        m->img = romfs_image_open(dir, why, sizeof why);
        if (!m->img) {
            LOG("[fs] cannot read %s: %s\n", dir, why);
            return NULL;
        }
        LOG("[fs] romfs image %s: %llu MB, laid out in %.2f s\n", dir, (unsigned long long)(romfs_image_size(m->img) >> 20),
            (plat_now_ns() - t0) / 1e9);
        m->ov_sub = k ? "aoc/0010/" : "";  // (the add-on content's place in a mod's folder, as on the Wii U)
        R = m;
        rom_index();
        R = &images[0];
    }
    Storage *st = calloc(1, sizeof *st);
    st->img = m;
    st->size = romfs_image_size(m->img);
    return service_new(name, storage, st);
}

static Service *open_romfs_storage(void) {
    Service *s = open_image(0, g_romfs_dir, "IStorage(romfs)");  // extracted/romfs
    if (!s) FATAL("cannot read the game data in %s (set up the game files on the start screen)\n", g_romfs_dir);
    return s;
}

// ------------------------------------------------------------------ add-on content (DLC)
// Each add-on content index the game can mount has its own RomFS, next to the game's: extracted/aoc<index> (the
// setup copies it from the DLC's romfs folder). Its data id is the add-on content base id + index. As on the
// console, it stays its own image (the game mounts it apart and asks for it by id); it gets the same changes as the
// game's.
static void aoc_dir(int index, char *out, size_t n) {
    const char *slash = strrchr(g_romfs_dir, '/');
    int dir = slash ? (int)(slash - g_romfs_dir) : 0;
    snprintf(out, n, "%.*s%saoc%d", dir, g_romfs_dir, slash ? "/" : "", index);
}

uint64_t fs_aoc_base_id(void) { return (g_program_id & ~0xFFFull) + 0x1000; }

bool fs_aoc_present(int index) {
    if (index < 1 || index > AOC_MAX || !g_romfs_dir) return false;
    char dir[1100];
    aoc_dir(index, dir, sizeof dir);
    struct stat sb;
    return stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static Service *open_aoc_storage(int index) {
    char dir[1100];
    aoc_dir(index, dir, sizeof dir);
    Service *s = open_image(index, dir, "IStorage(aoc)");
    LOG("[fs] add-on content %d: %s%s\n", index, dir, s ? "" : " (missing)");
    extern void rt_title_menu_dlc(void);  // (runtime/ui/game_options.mm: the title menu's Downloadable Content button)
    if (s) rt_title_menu_dlc();
    return s;
}

// ------------------------------------------------------------------ host-directory filesystem
typedef struct { char root[1024]; } HostFs;
typedef struct { int fd; char path[1100]; uint8_t *patched; uint64_t patched_size; } HostFile;
// (test only) runtime/services/test_warp.c: game_data.sav read with some values rewritten (SAVAGEWIND_TEST_WARP...)
bool test_warp_active(void);
void test_warp_patch(uint8_t *data, uint64_t size);
typedef struct { DIR *d; char path[1024]; int mode; } HostDir;

static const char *in_path(IpcReq *req, int i) {
    if (i < req->nx) return (const char *)(uintptr_t)req->x[i].addr;
    if (i < req->na) return (const char *)(uintptr_t)req->a[i].addr;
    return "";
}

static void host_path(HostFs *fs, const char *p, char *out, size_t n) {
    snprintf(out, n, "%s%s%s", fs->root, p[0] == '/' ? "" : "/", p);
}

static uint32_t errno_result(void) {
    if (errno == ENOENT || errno == ENOTDIR) return FS_PATH_NOT_FOUND;
    if (errno == EEXIST) return FS_PATH_EXISTS;
    if (errno == ENOTEMPTY) return FS_NOT_EMPTY;
    return MAKE_RESULT(2, 4000);
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        kernel_watch_write("file read", b->addr, (uint64_t)size);
        ssize_t n;
        if (f->patched) {  // (test warp)
            n = off >= 0 && (uint64_t)off < f->patched_size ? (ssize_t)((uint64_t)size < f->patched_size - (uint64_t)off ? (uint64_t)size : f->patched_size - (uint64_t)off) : 0;
            memcpy((void *)(uintptr_t)b->addr, f->patched + off, (size_t)n);
        } else {
            n = pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)size, off);
        }
        if (strstr(f->path, "savedata")) {
            LOG("[save] read %s off %lld size %lld -> guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)b->addr);
        }
        OUT(int64_t, 0) = n < 0 ? 0 : n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // Write(option, offset, size) <- A
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *a = in_buf(req, 0);
        if (strstr(f->path, "savedata"))
            LOG("[save] write %s off %lld size %lld <- guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)a->addr);
        if (pwrite(f->fd, (void *)(uintptr_t)a->addr, (size_t)size, off) < 0) resp->result = errno_result();
        free(f->patched);  // (the game saved: its own data from now on)
        f->patched = NULL;
        break;
    }
    case 2: fsync(f->fd); break;
    case 3: if (ftruncate(f->fd, IN(int64_t, 0))) resp->result = errno_result(); break;
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(int64_t, 0) = sb.st_size;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[fs] IFile cmd %u (ok)\n", req->cmd);
    }
}

static void directory(Service *s, IpcReq *req, IpcResp *resp) {
    HostDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Read -> B buffer of 0x310-byte entries
        IpcBuf *b = out_buf(req, 0);
        uint8_t *out = (uint8_t *)(uintptr_t)b->addr;
        int64_t max = (int64_t)(b->size / 0x310), n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
#ifdef _WIN32  // no d_type
            char full[2048];
            struct stat es;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            bool isdir = !stat(full, &es) && S_ISDIR(es.st_mode);
#else
            bool isdir = e->d_type == DT_DIR;
#endif
            if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300);
            ent[0x304] = isdir ? 0 : 1;
            if (!isdir) {
                char p[2048];
                struct stat sb;
                snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
                if (!stat(p, &sb)) *(int64_t *)(ent + 0x308) = sb.st_size;
            }
            n++;
        }
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // GetEntryCount
        long pos = telldir(d->d);
        rewinddir(d->d);
        int64_t n = 0;
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        seekdir(d->d, pos);
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    }
}

static int rm_rf(const char *p, bool keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rm_rf(c, false);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}

static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {
    HostFs *fs = s->state;
    char p[2048], p2[2048];
    host_path(fs, in_path(req, 0), p, sizeof p);
    static int log_saves = -1;  // SAVAGEWIND_LOG_SAVES=1: what the game creates, deletes and renames in its save folder
    if (log_saves < 0) log_saves = getenv("SAVAGEWIND_LOG_SAVES") && strcmp(getenv("SAVAGEWIND_LOG_SAVES"), "0");
    if (log_saves && (req->cmd <= 6 || req->cmd == 13)) {
        static const char *const what[] = {"create", "delete", "mkdir", "rmdir", "rmdir -r", "rename", "rename dir"};
        if (req->cmd == 5 || req->cmd == 6) host_path(fs, in_path(req, 1), p2, sizeof p2);
        LOG("[fs] save: %s %s%s%s\n", req->cmd == 13 ? "clean dir" : what[req->cmd], p,
            req->cmd == 5 || req->cmd == 6 ? " -> " : "", req->cmd == 5 || req->cmd == 6 ? p2 : "");
    }
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) { resp->result = errno_result(); break; }
        if (ftruncate(fd, IN(int64_t, 8))) resp->result = errno_result();
        close(fd);
        break;
    }
    case 1: if (unlink(p)) resp->result = errno_result(); break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3: if (rmdir(p)) resp->result = errno_result(); break;
    case 4: if (rm_rf(p, false)) resp->result = errno_result(); break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        if (rename(p, p2)) resp->result = errno_result();
        else if (strlen(p2) >= 13 && !strcmp(p2 + strlen(p2) - 13, "game_data.sav")) ach_set_active(p2);  // (saved via a temp file)
        break;
    case 7: {  // GetEntryType
        struct stat sb;
        if (stat(p, &sb)) { resp->result = FS_PATH_NOT_FOUND; break; }
        OUT(uint32_t, 0) = S_ISDIR(sb.st_mode) ? 0 : 1;
        resp_size(resp, 4);
        break;
    }
    case 8: {  // OpenFile(mode)
        uint32_t mode = IN(uint32_t, 0);
        int fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY);
        if (fd < 0) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        strlcpy(f->path, p, sizeof f->path);
        size_t pl = strlen(p);
        if (pl >= 13 && !strcmp(p + pl - 13, "game_data.sav")) ach_set_active(p);  // the playthrough being played (achievements)
        if (pl >= 13 && !strcmp(p + pl - 13, "game_data.sav") && test_warp_active()) {
            struct stat sb;
            if (!fstat(fd, &sb) && sb.st_size > 0 && (f->patched = malloc((size_t)sb.st_size)) &&
                pread(fd, f->patched, (size_t)sb.st_size, 0) == (ssize_t)sb.st_size) {
                f->patched_size = (uint64_t)sb.st_size;
                test_warp_patch(f->patched, f->patched_size);
            } else {
                free(f->patched);
                f->patched = NULL;
            }
        }
        ipc_return_object(resp, service_new("IFile", file, f));
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        ipc_return_object(resp, service_new("IDirectory", directory, hd));
        break;
    }
    case 10: break;  // Commit
    case 11: case 12: OUT(int64_t, 0) = 0x40000000; resp_size(resp, 8); break;
    case 13: rm_rf(p, true); break;
    case 14: memset(resp->data, 0, 0x20); resp_size(resp, 0x20); break;
    default: LOG("[fs] IFileSystem cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_host_fs(const char *sub) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (getenv("SAVAGEWIND_SAVE_DIR")) g_save_dir = getenv("SAVAGEWIND_SAVE_DIR");  // test runs use a scratch copy
    snprintf(fs->root, sizeof fs->root, "%s/%s", g_save_dir, sub);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "%s", fs->root);
    for (char *q = cmd + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    LOG("[fs] mounted host directory %s\n", fs->root);
    return service_new("IFileSystem", filesystem, fs);
}

// ------------------------------------------------------------------ fsp-srv
SERVICE("fsp-srv", fsp_srv) {
    (void)s;
    switch (req->cmd) {
    case 1: break;  // SetCurrentProcess
    case 18: ipc_return_object(resp, open_host_fs("sdcard")); break;
    case 22: break;  // CreateSaveDataFileSystem
    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        uint8_t type = IN(uint8_t, 0x8 + 0x18);  // attribute.type
        char sub[64];
        snprintf(sub, sizeof sub, "save_%u", type);
        ipc_return_object(resp, open_host_fs(sub));
        break;
    }
    case 200: ipc_return_object(resp, open_romfs_storage()); break;  // OpenDataStorageByCurrentProcess
    case 202: {  // OpenDataStorageByDataId: add-on content, or system data archives (not available)
        uint64_t id = IN(uint64_t, 8), base = fs_aoc_base_id();
        Service *aoc = id > base && id <= base + AOC_MAX ? open_aoc_storage((int)(id - base)) : NULL;
        if (aoc) { ipc_return_object(resp, aoc); break; }
        LOG("[fs] OpenDataStorageByDataId %llx (not available)\n", (unsigned long long)id);
        resp->result = MAKE_RESULT(2, 1001);
        break;
    }
    case 1005: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetGlobalAccessLogMode
    case 1003: case 1004: case 1006: break;
    case 1011: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    default: LOG("[fs] fsp-srv cmd %u (ok)\n", req->cmd);
    }
}
