// fsp-srv: RomFS as a raw storage image, save data and SD card backed by host directories.
//
// RomFS overlay (development): SAVAGEWIND_ROMFS_OVERLAY=<dir>, a folder laid out like the RomFS. Its files replace
// the game's files of the same path, and files the game doesn't have are added (into directories it has). The
// image the game reads stays one virtual RomFS: patched copies of its tables, the host files appended past its end.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "../achievements.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../ipc.h"
#include "../language.h"
#include "../platform.h"
#include "../nvn/glyphs.h"
#include "../nvn/hw_illust.h"
#include "../nvn/text_upscale.h"
#include "../romfs_image.h"
#include "../settings.h"
#include "../shrine_sensor.h"
#include "../music_volume.h"
#include "archive.h"
#include "fs_layout.h"
#include "save_profiles.h"
#include "../mods.h"
#include "../perfcount.h"
#include "../platform.h"
#include "../game_place.h"

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
// Files that got bigger than the game's own, by their ResourceSizeTable name, for rstb_patch: redirected voices
// (voice_redirect), the HUD's layout (hud_letters_patch) and the overlay folder's replacements (mod: a mod's own table
// already sizes those)
typedef struct { const char *path; uint64_t grow; bool mod; } GrownFile;
// A RomFS image the game reads and what Savage Wind changes in it: the game's own (base + update, extracted/romfs)
// and each add-on content's (extracted/aoc<index>). Both get the same changes (voices, overlay folder,
// ResourceSizeTable, HUD letters); the functions below work on R, the image being indexed.
typedef struct {
    RomfsImage *img;         // (runtime/romfs_image.h; NULL: not opened)
    const char *ov_sub;      // where its files are in the overlay folder ("": the game's own)
    Patch patches[16];
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
    size_t hud_pack_size;
    uint8_t *title_pack;     // Pack/Title.pack with the title screen's footer and Achievements button (title_footer_patch)
    uint8_t *voice_banks;    // the text language's voice banks with the chosen voices' lengths (bars_hybrid)
    uint8_t **voice_packs;   // Pack/TitleBG_*.pack with the chosen voices' NV_Custom_NoActor.bars / StaticVoicePack.ssarc
    int nvoice_packs;
    uint8_t *font_arcs[8];   // Font/Font_*.sbfarc with the HUD letters' cells as wide as the letters (font_widths_patch)
} RomImage;
#define AOC_MAX 8
static RomImage images[1 + AOC_MAX];  // [0] the game's, [index] add-on content
static RomImage *R = &images[0];
static FsInventoryClipLayout inventory_clip_layout;
static _Atomic bool inventory_clip_layout_valid;
static _Atomic bool game_image_open;  // images[0] is open and indexed (fs_game_files_open)
static pthread_mutex_t game_file_lock = PTHREAD_MUTEX_INITIALIZER;  // held while fs_game_file reads images[0]

// Reboot: the next boot builds its own index / overlay when the game opens the RomFS again.
RT_ON_RESET(fs_reset) {
    pthread_mutex_lock(&game_file_lock);  // (first: no fs_game_file reads the image while it closes, or after)
    atomic_store_explicit(&game_image_open, false, memory_order_release);
    pthread_mutex_unlock(&game_file_lock);
    for (int k = 0; k <= AOC_MAX; k++) {
        RomImage *m = &images[k];
        for (int i = 0; i < m->ov_n; i++) {
            if (m->ov[i].fd >= 0) close(m->ov[i].fd);
            free(m->ov[i].path);
        }
        for (int i = 0; i < m->npatches; i++) free(m->patches[i].data);  // (ov_meta and the extended table are among these)
        for (int i = 0; i < m->rom_nfiles; i++) free(m->rom_files[i].path);
        free(m->ov), free(m->rom_files), free(m->grown), free(m->rstb_blob), free(m->hud_pack), free(m->title_pack), free(m->voice_banks);
        for (int p = 0; p < m->nvoice_packs; p++) free(m->voice_packs[p]);
        free(m->voice_packs);
        for (int f = 0; f < 8; f++) free(m->font_arcs[f]);
        romfs_image_close(m->img);
        *m = (RomImage){0};
    }
    R = &images[0];
    atomic_store_explicit(&inventory_clip_layout_valid, false, memory_order_release);
    g_save_dir = "save";
}

// The overlay layer support lives in runtime/mods.c (mods_find_file, mods_active_*)

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

static void walk_new(const char *root, const char *rel, char **known, int nknown,
                     char **seen, int nseen, NewFile **out, int *n, int *cap) {
    char dirp[1200];
    if (rel[0]) snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    else snprintf(dirp, sizeof dirp, "%s", root);
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
        if (S_ISDIR(sb.st_mode)) { walk_new(root, r2, known, nknown, seen, nseen, out, n, cap); continue; }
        if (!rel[0] && mods_is_ignored_top_file(e->d_name)) continue;
        const char *key = r2;
        if (bsearch(&key, known, nknown, sizeof *known, path_cmp)) continue;  // replaces an existing file
        if (seen && nseen > 0 && bsearch(&key, seen, nseen, sizeof *seen, path_cmp)) continue;  // higher layer won
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
    int nlayers = mods_active_count();
    if (nlayers <= 0) return;
    char **known = malloc(sizeof *known * R->rom_nfiles);
    for (int i = 0; i < R->rom_nfiles; i++) known[i] = R->rom_files[i].path;
    qsort(known, R->rom_nfiles, sizeof *known, path_cmp);

    char sub[256];
    sub[0] = '\0';
    if (R->ov_sub && R->ov_sub[0]) {
        const char *s = R->ov_sub;
        while (*s == '/' || *s == '\\') s++;
        strlcpy(sub, s, sizeof sub);
        size_t sl = strlen(sub);
        while (sl > 0 && (sub[sl - 1] == '/' || sub[sl - 1] == '\\')) sub[--sl] = '\0';
    }

    NewFile *nf = NULL;
    int n = 0, cap = 0;
    for (int layer = 0; layer < nlayers; layer++) {
        const char *dir = mods_active_dir(layer);
        if (!dir || !*dir) continue;
        char base[1100];
        strlcpy(base, dir, sizeof base);
        size_t bl = strlen(base);
        while (bl > 1 && (base[bl - 1] == '/' || base[bl - 1] == '\\')) base[--bl] = '\0';

        char root[1200];
        if (sub[0]) snprintf(root, sizeof root, "%s/%s", base, sub);
        else strlcpy(root, base, sizeof root);

        char **seen = NULL;
        int nseen = n;
        if (nseen > 0) {
            seen = malloc(sizeof *seen * nseen);
            for (int k = 0; k < nseen; k++) seen[k] = nf[k].rel;
            qsort(seen, nseen, sizeof *seen, path_cmp);
        }

        NewFile *layer_nf = NULL;
        int ln = 0, lcap = 0;
        walk_new(root, "", known, R->rom_nfiles, seen, nseen, &layer_nf, &ln, &lcap);
        free(seen);

        if (ln > 0) {
            if (n + ln > cap) {
                while (cap < n + ln) cap = cap ? cap * 2 : 64;
                nf = realloc(nf, sizeof *nf * cap);
            }
            memcpy(nf + n, layer_nf, sizeof *nf * ln);
            n += ln;
        }
        free(layer_nf);
    }

    free(known);
    if (!n) { free(nf); return; }
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
    free(nf);
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

static uint64_t loaded_size(const uint8_t head[8], uint64_t size);
static void rstb_name(const char *path, char *out, size_t cap);

// TitleBG_<lang>.pack embeds Voice/<vl>/NV_Custom_NoActor.bars (Champion power voices, #1007) and StaticVoicePack.ssarc.
// Rebuild each non-chosen pack with the chosen voice folder's files while keeping its own SARC entry names.
static int titlebg_voice_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz, const char *pre, size_t pl,
                               RomFile **chosen, int nc) {
    int patched = 0;
    for (int i = 0; i < R->rom_nfiles; i++) {
        const char *p = R->rom_files[i].path;
        size_t pn = strlen(p);
        if (strncmp(p, "Pack/TitleBG_", 13) || pn <= 5 || strcmp(p + pn - 5, ".pack")) continue;
        size_t sz = R->rom_files[i].size;
        if (sz < 0x20 || sz > (16u << 20)) continue;
        uint8_t *pack = malloc(sz);
        if (!pack) continue;
        if (romfs_image_pread(R->img, pack, sz, R->rom_files[i].off) != (ssize_t)sz ||
            memcmp(pack, "SARC", 4) || memcmp(pack + 6, "\xFF\xFE", 2)) {
            free(pack);
            continue;
        }
        int cnt = sarc_count(pack, sz);
        uint32_t data_off = rd32(pack + 12);
        if (cnt <= 0 || cnt > 64 || data_off < 0x20 + (uint32_t)cnt * 16 || data_off > sz) {
            free(pack);
            continue;
        }
        uint8_t *rep[64] = {0};
        const uint8_t *src[64] = {0};
        size_t src_sz[64] = {0};
        bool any = false;
        for (int k = 0; k < cnt; k++) {
            const char *name = NULL;
            if (!sarc_node(pack, sz, k, &name, &src[k], &src_sz[k])) { any = false; break; }
            if (!name || strncmp(name, "Voice/", 6) || !strncmp(name, pre, pl) || !strchr(name + 6, '/')) continue;
            RomFile key = {.path = (char *)name}, *kp = &key;
            RomFile **hit = bsearch(&kp, chosen, nc, sizeof *chosen, by_name_after);
            if (!hit) continue;
            char host[1200];
            struct stat sb;
            bool ov = mods_find_file(R->ov_sub, (*hit)->path, host, sizeof host, NULL, 0) && !stat(host, &sb);
            size_t vn = ov ? (size_t)sb.st_size : (*hit)->size;
            uint8_t *buf = malloc(vn ? vn : 1);
            bool got = false;
            if (buf && ov) {
                FILE *f = fopen(host, "rb");
                got = f && fread(buf, 1, vn, f) == vn;
                if (f) fclose(f);
            } else if (buf) {
                got = romfs_image_pread(R->img, buf, vn, (*hit)->off) == (ssize_t)vn;
            }
            if (got) {
                rep[k] = buf;
                src[k] = buf;
                src_sz[k] = vn;
                any = true;
            } else {
                free(buf);
            }
        }
        if (!any) {
            for (int k = 0; k < cnt; k++) free(rep[k]);
            free(pack);
            continue;
        }
        uint32_t s_off[64], e_off[64];
        size_t out_sz = data_off;
        for (int k = 0; k < cnt; k++) {
            out_sz = (out_sz + 7) & ~7ull;
            s_off[k] = (uint32_t)(out_sz - data_off);
            e_off[k] = (uint32_t)(s_off[k] + src_sz[k]);
            out_sz += src_sz[k];
        }
        uint8_t *out = calloc(1, out_sz);
        if (out) {
            memcpy(out, pack, data_off);
            uint32_t total = (uint32_t)out_sz;
            memcpy(out + 8, &total, 4);
            for (int k = 0; k < cnt; k++) {
                memcpy(out + 0x20 + k * 16 + 8, &s_off[k], 4);
                memcpy(out + 0x20 + k * 16 + 12, &e_off[k], 4);
                if (src_sz[k]) memcpy(out + data_off + s_off[k], src[k], src_sz[k]);
            }
            meta_copy(fmeta, foff, fsz);
            uint64_t rel = R->ov_end - R->rom_data_off, size64 = out_sz;
            memcpy(R->ov_meta + R->rom_files[i].meta + 8, &rel, 8);
            memcpy(R->ov_meta + R->rom_files[i].meta + 0x10, &size64, 8);
            if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
            R->ov[R->ov_n++] = (Overlay){R->ov_end, size64, NULL, -1, out};
            R->ov_end = (R->ov_end + size64 + 0xF) & ~0xFull;
            R->voice_packs = realloc(R->voice_packs, sizeof *R->voice_packs * (R->nvoice_packs + 1));
            R->voice_packs[R->nvoice_packs++] = out;
            patched++;
        }
        for (int k = 0; k < cnt; k++) free(rep[k]);
        free(pack);
    }
    return patched;
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
    int n = 0, nh = 0, nov = 0;
    uint64_t *ov_at = calloc(nc, sizeof *ov_at);  // (where a chosen file's overlay copy is appended; 0: not yet)
    struct Hybrid { int file; uint8_t *data; } *hy = malloc(sizeof *hy * R->rom_nfiles);
    for (int i = 0; i < R->rom_nfiles; i++) {
        const char *p = R->rom_files[i].path;
        if (strncmp(p, "Voice/", 6) || !strncmp(p, pre, pl) || !strncmp(p, "Voice/Common/", 13)) continue;
        if (!strchr(p + 6, '/')) continue;
        RomFile *key = &R->rom_files[i], **hit = bsearch(&key, chosen, nc, sizeof *chosen, by_name_after);
        if (!hit) continue;
        // A chosen-folder file the overlay folder replaces (a voice mod) is the one that plays: its host file
        char host[1200];
        struct stat sb;
        bool ov = mods_find_file(R->ov_sub, (*hit)->path, host, sizeof host, NULL, 0) && !stat(host, &sb);
        nov += ov;
        size_t pn = strlen(p), on = R->rom_files[i].size, vn = ov ? (size_t)sb.st_size : (*hit)->size;
        if (pn > 5 && !strcmp(p + pn - 5, ".bars") && on < (1u << 20) && vn < (1u << 20)) {
            uint8_t *own = malloc(on ? on : 1), *vb = malloc(vn ? vn : 1);
            bool got = false;
            if (ov) {
                FILE *f = fopen(host, "rb");
                got = f && fread(vb, 1, vn, f) == vn;
                if (f) fclose(f);
            } else got = romfs_image_pread(R->img, vb, vn, (*hit)->off) == (ssize_t)vn;
            bool ok = romfs_image_pread(R->img, own, on, R->rom_files[i].off) == (ssize_t)on && got &&
                      bars_hybrid(own, on, vb, vn);
            free(vb);
            if (ok) { hy[nh++] = (struct Hybrid){i, own}; n++; continue; }
            free(own);
        }
        uint8_t *meta = meta_copy(fmeta, foff, fsz);
        if (ov) {  // (the host file appended past the end of the image, as the overlay below does)
            uint64_t *at = &ov_at[hit - chosen], size = vn;  // (one appended copy for every language that plays it)
            if (!*at) {
                if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
                R->ov[R->ov_n++] = (Overlay){R->ov_end, size, strdup(host), -1, NULL};
                *at = R->ov_end;
                R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
            }
            uint64_t rel = *at - R->rom_data_off;
            memcpy(meta + R->rom_files[i].meta + 8, &rel, 8);
            memcpy(meta + R->rom_files[i].meta + 0x10, &size, 8);
        } else memcpy(meta + R->rom_files[i].meta + 8, fmeta + (*hit)->meta + 8, 16);  // (data offset, size)
        uint64_t own_loaded = on, new_loaded = vn;
        if (pn > 6 && !strcmp(p + pn - 6, ".ssarc") && on >= 8 && vn >= 8) {
            uint8_t h1[8], h2[8];
            if (romfs_image_pread(R->img, h1, 8, R->rom_files[i].off) == 8) own_loaded = loaded_size(h1, on);
            if (ov) {
                FILE *f = fopen(host, "rb");
                if (f) { if (fread(h2, 1, 8, f) == 8) new_loaded = loaded_size(h2, vn); fclose(f); }
            } else if (romfs_image_pread(R->img, h2, 8, (*hit)->off) == 8) new_loaded = loaded_size(h2, vn);
        }
        if (new_loaded > own_loaded) {
            R->grown = realloc(R->grown, sizeof *R->grown * (R->ngrown + 1));
            R->grown[R->ngrown++] = (GrownFile){p, new_loaded - own_loaded};
        }
        n++;
    }
    int np = titlebg_voice_patch(fmeta, foff, fsz, pre, pl, chosen, nc);
    free(chosen), free(ov_at);
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
    LOG("[fs] voices: %d files play %s's voices (%d banks keep the text's subtitle markers, %d from the overlay folder, %d TitleBG packs)\n",
        n, vf, nh, nov, np);
}

// The game sets aside memory for a resource by its path's entry in System/Resource/ResourceSizeTable (the RSTB: the
// file's size plus room for what it builds from it, ~850 bytes for a voice bank). A redirected bank bigger than the
// path's own file then runs out: the 0xa8-byte object that parses it (after the 'BARS' check) gets no memory and the
// game writes through the null pointer ("accessing 0x10": pt-BR text with Japanese voices, at the title screen, where
// it loads the Demo*.bars banks). So the RSTB the game reads is a copy whose entries for those paths grow by as much
// as their data did (and a little: the room varies with alignment), served from memory like Version.txt. Its format:
// Yaz0 over "RSTB", u32 CRC count, u32 name count, {crc32(path), size} sorted by crc32, then {char[128], size}.
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

bool fs_inventory_clip_layout(FsInventoryClipLayout *out) {
    if (!out || !atomic_load_explicit(&inventory_clip_layout_valid, memory_order_acquire)) return false;
    *out = inventory_clip_layout;
    return true;
}

typedef struct { float x, y, sx, sy; unsigned kind; } LayoutPane;
enum { PANE_OTHER, PANE_ROOT, PANE_SLIDE, PANE_INOUT, PANE_CUT, PANE_PORCH_LEFT, PANE_PORCH_MIDDLE };

static bool layout_name(const uint8_t *sec, const char *name) { return !strncmp((const char *)sec + 12, name, 24); }

static bool layout_part(const uint8_t *sec, size_t n, const char *part) {
    size_t k = strlen(part) + 1;
    for (size_t i = 84; i + k <= n; i++) if (!memcmp(sec + i, part, k)) return true;
    return false;
}

// Follow only the named branch of PauseMenu_00's pane hierarchy. Positions and sizes come from the
// player's loaded layout, never from a particular window size or a copied game asset.
static bool pause_inventory_panes(const uint8_t *d, size_t n, float *root_w, float *root_h,
                                  LayoutPane *cut, float *cut_w, float *cut_h, LayoutPane *left) {
    if (n < 20 || memcmp(d, "FLYT", 4) || d[4] != 0xFF || d[5] != 0xFE) return false;
    size_t p = (size_t)d[6] | (size_t)d[7] << 8;
    LayoutPane stack[16] = {{0}}, last = {0}, middle = {0};
    unsigned depth = 0, found = 0;
    while (p + 8 <= n) {
        const uint8_t *sec = d + p;
        uint32_t sz;
        memcpy(&sz, sec + 4, 4);
        if (sz < 8 || sz > n - p) return false;
        if (!memcmp(sec, "pas1", 4)) {
            if (depth == 16) return false;
            stack[depth++] = last;
        } else if (!memcmp(sec, "pae1", 4)) {
            if (!depth) return false;
            depth--;
        } else if (!memcmp(sec, "pan1", 4) || !memcmp(sec, "prt1", 4) ||
                   !memcmp(sec, "pic1", 4) || !memcmp(sec, "txt1", 4) ||
                   !memcmp(sec, "wnd1", 4) || !memcmp(sec, "bnd1", 4)) {
            if (sz < 84) return false;
            float v[10];
            memcpy(v, sec + 44, sizeof v);
            LayoutPane parent = depth ? stack[depth - 1] : (LayoutPane){.sx = 1, .sy = 1};
            last = (LayoutPane){parent.x + parent.sx * v[0], parent.y + parent.sy * v[1],
                                parent.sx * v[6], parent.sy * v[7], PANE_OTHER};
            if (depth == 0 && layout_name(sec, "RootPane")) {
                if (found & 1) return false;
                *root_w = v[8], *root_h = v[9];
                last.kind = PANE_ROOT;
                found |= 1;
            } else if (parent.kind == PANE_ROOT && layout_name(sec, "N_Slide_01")) {
                last.kind = PANE_SLIDE;
            } else if (parent.kind == PANE_SLIDE && layout_name(sec, "N_InOut_00")) {
                last.kind = PANE_INOUT;
            } else if (parent.kind == PANE_INOUT && layout_name(sec, "N_Cut_00")) {
                if (found & 2 || sec[11] != 2) return false;  // center origin; other origins need different bounds
                *cut = last; *cut_w = v[8] * last.sx; *cut_h = v[9] * last.sy;
                last.kind = PANE_CUT;
                found |= 2;
            } else if (parent.kind == PANE_CUT && layout_name(sec, "Pa_PagePorch_00")) {
                if (found & 4 || !layout_part(sec, sz, "PaPage20_00")) return false;
                *left = last;
                last.kind = PANE_PORCH_LEFT;
                found |= 4;
            } else if (parent.kind == PANE_CUT && layout_name(sec, "Pa_PagePorch_01")) {
                if (found & 8 || !layout_part(sec, sz, "PaPage20_00")) return false;
                middle = last;
                last.kind = PANE_PORCH_MIDDLE;
                found |= 8;
            }
        }
        p += sz;
    }
    return p == n && depth == 0 && found == 15 && left->x < middle.x &&
           isfinite(*root_w) && isfinite(*root_h) && *root_w > 0 && *root_h > 0 &&
           isfinite(*cut_w) && isfinite(*cut_h) && *cut_w > 0 && *cut_h > 0 &&
           isfinite(left->x) && isfinite(left->sx) && left->sx > 0;
}

// PaPage20_00 is the grid of inventory items. Its rightmost item column is the part of
// the previous page visible through N_Cut_00's left edge.
static bool pause_inventory_last_column(const uint8_t *d, size_t n, float *x, float *half_w,
                                        float *root_w, float *root_h) {
    if (n < 20 || memcmp(d, "FLYT", 4) || d[4] != 0xFF || d[5] != 0xFE) return false;
    size_t p = (size_t)d[6] | (size_t)d[7] << 8;
    LayoutPane stack[16] = {{0}}, last = {0};
    unsigned depth = 0, count = 0, roots = 0;
    *x = -INFINITY;
    while (p + 8 <= n) {
        const uint8_t *sec = d + p;
        uint32_t sz;
        memcpy(&sz, sec + 4, 4);
        if (sz < 8 || sz > n - p) return false;
        if (!memcmp(sec, "pas1", 4)) {
            if (depth == 16) return false;
            stack[depth++] = last;
        } else if (!memcmp(sec, "pae1", 4)) {
            if (!depth) return false;
            depth--;
        } else if (!memcmp(sec, "pan1", 4) || !memcmp(sec, "prt1", 4) ||
                   !memcmp(sec, "pic1", 4) || !memcmp(sec, "txt1", 4) ||
                   !memcmp(sec, "wnd1", 4) || !memcmp(sec, "bnd1", 4)) {
            if (sz < 84) return false;
            float v[10];
            memcpy(v, sec + 44, sizeof v);
            LayoutPane parent = depth ? stack[depth - 1] : (LayoutPane){.sx = 1, .sy = 1};
            last = (LayoutPane){parent.x + parent.sx * v[0], parent.y + parent.sy * v[1],
                                parent.sx * v[6], parent.sy * v[7], PANE_OTHER};
            if (depth == 0 && layout_name(sec, "RootPane")) {
                if (roots++) return false;
                *root_w = v[8], *root_h = v[9];
                last.kind = PANE_ROOT;
            } else if (parent.kind == PANE_ROOT && !memcmp(sec, "prt1", 4) &&
                       !strncmp((const char *)sec + 12, "Pa_Btn_", 7)) {
                if (!isfinite(last.x) || !isfinite(last.sx) || !isfinite(v[8])) return false;
                if (last.x > *x) *x = last.x, *half_w = v[8] * last.sx * 0.5f;
                count++;
            }
        }
        p += sz;
    }
    return p == n && depth == 0 && roots == 1 && count > 1 &&
           isfinite(*root_w) && isfinite(*root_h) && *root_w > 0 && *root_h > 0 &&
           isfinite(*x) && isfinite(*half_w) && *half_w > 0;
}

static void inventory_layout_from_common(const uint8_t *arc, size_t an) {
    uint32_t adoff, as, ae;
    int pa = sarc_find(arc, an, "blyt/PauseMenu_00.bflyt", &adoff, &as, &ae);
    if (pa < 0) return;
    float root_w = 0, root_h = 0, cut_w = 0, cut_h = 0, page_root_w = 0, page_root_h = 0;
    float last_x = 0, last_half_w = 0;
    LayoutPane cut = {0}, left = {0};
    if (!pause_inventory_panes(arc + adoff + as, ae - as, &root_w, &root_h, &cut, &cut_w, &cut_h, &left)) return;
    if (sarc_find(arc, an, "blyt/PaPage20_00.bflyt", &adoff, &as, &ae) < 0 ||
        !pause_inventory_last_column(arc + adoff + as, ae - as, &last_x, &last_half_w,
                                     &page_root_w, &page_root_h)) return;
    if (fabsf(page_root_w - root_w) > 0.01f || fabsf(page_root_h - root_h) > 0.01f) return;
    FsInventoryClipLayout layout = {
        .root_w = root_w, .root_h = root_h,
        .clip_x = root_w * 0.5f + cut.x - cut_w * 0.5f,
        .clip_y = root_h * 0.5f - cut.y - cut_h * 0.5f,
        .clip_w = cut_w, .clip_h = cut_h,
        .previous_item_x = root_w * 0.5f + left.x + left.sx * last_x,
    };
    // The original left edge crosses the previous item's right half; its center gives a half-item hint.
    if (!isfinite(layout.clip_x) || !isfinite(layout.clip_y) || !isfinite(layout.previous_item_x) ||
        !(layout.clip_x < layout.previous_item_x &&
          layout.previous_item_x < layout.clip_x + last_half_w * left.sx &&
          layout.previous_item_x < layout.clip_x + layout.clip_w)) return;
    inventory_clip_layout = layout;
    atomic_store_explicit(&inventory_clip_layout_valid, true, memory_order_release);
    LOG("[fs] Inventory clip layout: root %.0fx%.0f, pane %.1f %.1f %.1fx%.1f, previous item x %.1f\n",
        root_w, root_h, layout.clip_x, layout.clip_y, layout.clip_w, layout.clip_h, layout.previous_item_x);
}

static void inventory_layout_from_overlay(const char *host) {
    int fd = open(host, O_RDONLY);
    if (fd < 0) return;
    struct stat sb;
    // The replacement is a complete Bootup.pack. Bound the read before trusting its archive sizes.
    if (fstat(fd, &sb) || sb.st_size < 0x20 || sb.st_size > 128 * 1024 * 1024) { close(fd); return; }
    size_t pn = (size_t)sb.st_size, an = 0;
    uint8_t *pack = malloc(pn);
    uint32_t pdoff, ps, pe;
    if (pack && pread(fd, pack, pn, 0) == (ssize_t)pn &&
        sarc_find(pack, pn, "Layout/Common.sblarc", &pdoff, &ps, &pe) >= 0) {
        uint8_t *arc = yaz0_unpack(pack + pdoff + ps, pe - ps, &an);
        if (arc) inventory_layout_from_common(arc, an);
        free(arc);
    }
    free(pack);
    close(fd);
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
static size_t hud_letters_flyt(const uint8_t *b, size_t n, uint8_t *out, int which, const int pos[4], int *panes) {
    static const char icon_font[] = "External_00.bffnt";
    static const float k_act_xy[4][2] = {{26.0f, 0.0f}, {0.0f, -26.0f}, {0.0f, 26.0f}, {-26.0f, 0.0f}};
    static const float k_dim_xy[4][2] = {{26.2f, 0.8f}, {1.0f, -26.0f}, {0.8f, 26.0f}, {-25.5f, -0.2f}};
    if (n < 0x14 || memcmp(b, "FLYT", 4) || b[4] != 0xFF || b[5] != 0xFE) return 0;
    size_t hs = b[6] | b[7] << 8, o = hs, w = hs;
    if (hs > n) return 0;
    memcpy(out, b, hs);
    bool fonts = false, seen_capture = false;
    bool reorder = pos && (pos[0] != 0 || pos[1] != 1 || pos[2] != 2 || pos[3] != 3);
    int act = reorder && which >= 0 && which < 4 ? pos[which] : which;
    bool west = act == 3;
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
        if (reorder && !memcmp(sec, "txl1", 4)) {
            for (uint32_t j = 12; j + 18 <= sz; j++)
                if (!memcmp(sec + j, "Nt_KeyBtnSubBase", 16))
                    sec[j + 16] = (act == 0 || act == 3) ? 'A' : 'B';
        }
        if (!memcmp(sec, "txt1", 4) && sz >= 0x68) {
            uint32_t toff;
            memcpy(&toff, sec + 0x64, 4);
            if (toff && toff + 4 <= sz && !sec[toff + 1] && !sec[toff + 2] && !sec[toff + 3]) {
                const char *k = strchr("ABXY", sec[toff]);
                if (k && *k) {
                    int btn = (int)(k - "ABXY");
                    sec[0x5A] = 1, sec[0x5B] = 0;  // (the font: the second)
                    uint16_t c = (uint16_t)glyphs_letter_code(btn);
                    memcpy(sec + toff, &c, 2);
                    float fs[2];  // the font size: an icon fills less of its cell than a letter
                    memcpy(fs, sec + 0x70, 8);
                    // (a letter is taller than wide, 20.4 x 25.5, but the icons of External_00.bffnt are square: both sizes
                    // are the height's, or the three inactive buttons came out as ovals, #476)
                    fs[1] *= HUD_ICON_SCALE, fs[0] = fs[1];
                    memcpy(sec + 0x70, fs, 8);
                    if (reorder && pos[btn] >= 0 && pos[btn] < 4)
                        memcpy(sec + 44, k_dim_xy[pos[btn]], 8);
                    (*panes)++;
                }
            }
        }
        if (reorder && sz >= 84 &&
            (!memcmp(sec, "pan1", 4) || !memcmp(sec, "pic1", 4) || !memcmp(sec, "wnd1", 4) || !memcmp(sec, "txt1", 4))) {
            char name[25] = {0};
            memcpy(name, sec + 12, 24);
            if (!strncmp(name, "P_Base", 6)) {
                float rz = (act == 2 || act == 3) ? 180.0f : 0.0f, sx = act == 2 ? -1.0f : 1.0f;
                memcpy(sec + 64, &rz, 4);
                memcpy(sec + 68, &sx, 4);
            } else if (!strncmp(name, "N_Btn", 5)) {
                memcpy(sec + 44, k_act_xy[act], 8);
            } else if (!strncmp(name, "W_Base", 6)) {
                float wx = west ? 33.0f : -33.0f, wsx = west ? -1.0f : 1.0f;
                memcpy(sec + 44, &wx, 4);
                memcpy(sec + 68, &wsx, 4);
            } else if (!strncmp(name, "T_Guide", 7) && sz >= 96) {
                // A label left of its button (the west one, as in PaDoCommandY_00) hangs from its right end: the pane's
                // origin (pan1 +9) and the text's position in its box (txt1 +0x5C) are right (2), else left (1); the
                // alignment (+0x5D) stays the layout's own. Writing the alignment instead (2 is centred) and a user-data
                // byte for the origin left a west label centred on the button, over its letters (#1055).
                uint8_t at = west ? 2 : 1;
                if (strstr(name, "_01")) {
                    float gx = west ? -23.0f : 21.0f;
                    sec[9] = at, sec[92] = at;
                    memcpy(sec + 44, &gx, 4);
                } else if (strstr(name, "_00")) {
                    float gx[2] = {west ? 37.0f : -39.0f, 0.0f};
                    sec[9] = at, sec[92] = at;
                    memcpy(sec + 44, gx, 8);
                }
            } else if (!strncmp(name, "Nt_KeyTex", 9) && strstr(name, "_01")) {
                float kx[2] = {west ? 60.0f : -60.0f, 0.0f};
                memcpy(sec + 44, kx, 8);
            } else if (!strncmp(name, "N_Capture", 9)) {
                float cx[2] = {k_act_xy[act][0] + (west ? -60.0f : 60.0f), k_act_xy[act][1]};
                memcpy(sec + 44, cx, 8);
                seen_capture = true;
            } else if (!strncmp(name, "P_CaptureUse", 12)) {
                float px[2] = {
                    (seen_capture ? k_act_xy[act][0] : 0.0f) + (west ? -60.0f : 60.0f),
                    seen_capture ? k_act_xy[act][1] : 0.0f,
                };
                memcpy(sec + 44, px, 8);
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

// Gameplay > Sharper menus: every BNTX in an archive, its one-channel textures of one level added to the list
// (runtime/nvn/text_upscale.c). BRTI: the header's 0x16 mip count, 0x1c format, 0x24 width and height, 0x70 where the
// table of its levels' data is, from the BNTX's start.
static void menu_tex_bntx(const uint8_t *b, size_t n) {
    for (size_t p = 0x20; p + 0x78 <= n; p += 8) {
        if (memcmp(b + p, "BRTI", 4)) continue;
        uint32_t format = *(const uint32_t *)(b + p + 0x1c), w = *(const uint32_t *)(b + p + 0x24), h = *(const uint32_t *)(b + p + 0x28);
        uint16_t mips = *(const uint16_t *)(b + p + 0x16);
        uint64_t ptrs = *(const uint64_t *)(b + p + 0x70);
        if (mips != 1 || (format >> 8 != 0x1d && format >> 8 != 0x02) || ptrs + 8 > n || w < 16 || h < 16) continue;
        uint64_t at = *(const uint64_t *)(b + ptrs), bytes = (uint64_t)w * h / (format >> 8 == 0x1d ? 2 : 1);
        if (at + bytes <= n) menu_tex_add(b + at, bytes);
    }
}

static void menu_tex_arc(const uint8_t *a, size_t an, int depth) {
    if (an < 0x20 || memcmp(a, "SARC", 4)) return;
    uint32_t cnt = *(const uint16_t *)(a + 0x1A), dd = *(const uint32_t *)(a + 0xC);
    for (uint32_t i = 0; i < cnt && 0x20 + (i + 1) * 16 <= an; i++) {
        uint32_t s = *(const uint32_t *)(a + 0x20 + i * 16 + 8), e = *(const uint32_t *)(a + 0x20 + i * 16 + 12);
        if (e <= s || dd + e > an || e - s < 8) continue;
        const uint8_t *f = a + dd + s;
        if (!memcmp(f, "BNTX", 4)) menu_tex_bntx(f, e - s);
        else if (depth < 3 && !memcmp(f, "SARC", 4)) menu_tex_arc(f, e - s, depth + 1);
        else if (depth < 3 && !memcmp(f, "Yaz0", 4)) {
            size_t un = 0;
            uint8_t *u = yaz0_unpack(f, e - s, &un);
            if (u) menu_tex_arc(u, un, depth + 1);
            free(u);
        }
    }
}

static void menu_tex_scan(void) {
    static bool done;
    if (done || !g_settings.sharper_menus || R != &images[0]) return;
    done = true;
    for (int k = 0; k < R->rom_nfiles; k++) {
        const char *path = R->rom_files[k].path;
        // (most of the menus' art is in the boot pack, the rest in Layout/)
        if (strcmp(path, "Pack/Bootup.pack") && (strncmp(path, "Layout/", 7) || !strstr(path, ".sblarc"))) continue;
        size_t zn = R->rom_files[k].size, an = 0;
        uint8_t *z = malloc(zn), *arc = NULL;
        if (z && romfs_image_pread(R->img, z, zn, R->rom_files[k].off) == (ssize_t)zn) {
            if (!memcmp(z, "SARC", 4)) menu_tex_arc(z, zn, 0);
            else if ((arc = yaz0_unpack(z, zn, &an))) menu_tex_arc(arc, an, 0);
        }
        free(z);
        free(arc);
    }
    LOG("[fs] sharper menus: %d textures found\n", menu_tex_count());
}

// Gameplay > Sharper text: the font sheets in the game's own archives, so that their textures can be told from others
// (runtime/nvn/text_upscale.c). External_00 is left out: glyphs.c redraws those, and Ancient_00's sheet is too small.
static void text_sheets_scan(void) {
    static const char *const fonts[] = {"Normal_00.bffnt", "NormalS_00.bffnt", "Caption_00.bffnt", "Special_00.bffnt"};
    static const int kinds[] = {TEXT_FONT_NORMAL, TEXT_FONT_OUTLINE, TEXT_FONT_CAPTION, TEXT_FONT_OTHER};
    static bool done;
    if (done || !g_settings.sharper_text || R != &images[0]) return;
    done = true;
    for (int k = 0; k < R->rom_nfiles; k++) {
        const char *path = R->rom_files[k].path;
        if (strncmp(path, "Font/Font_", 10) || !strstr(path, ".sbfarc")) continue;
        size_t zn = R->rom_files[k].size, an = 0;
        uint8_t *z = malloc(zn), *arc = NULL;
        if (romfs_image_pread(R->img, z, zn, R->rom_files[k].off) == (ssize_t)zn) arc = yaz0_unpack(z, zn, &an);
        for (size_t i = 0; arc && i < sizeof fonts / sizeof *fonts; i++) {
            uint32_t doff, s, e;
            if (sarc_find(arc, an, fonts[i], &doff, &s, &e) < 0 || e - s < 0x40) continue;
            const uint8_t *f = arc + doff + s;
            uint32_t n = e - s, p = *(const uint16_t *)(f + 6);
            while (p + 32 <= n) {  // (FFNT sections: TGLP holds the sheets)
                uint32_t size = *(const uint32_t *)(f + p + 4);
                if (!memcmp(f + p, "TGLP", 4)) {  // (its sheets: the layers of one texture in a BNTX, texels after "BRTD")
                    uint32_t off = *(const uint32_t *)(f + p + 28), format = *(const uint16_t *)(f + p + 18);
                    uint32_t w = *(const uint16_t *)(f + p + 24), h = *(const uint16_t *)(f + p + 26);
                    uint64_t bytes = (uint64_t)w * h / (format == 8 ? 1 : 2);  // (8: R8, 12: BC4)
                    for (uint32_t a = off; a + 0x10 + bytes <= n && a < off + 0x2000; a += 4)
                        if (!memcmp(f + a, "BRTD", 4)) {
                            // (its cells: f[p + 8], f[p + 9] in size; cols and rows at p + 20, p + 22)
                            TextSheetInfo *info = calloc(1, sizeof *info);
                            if (info) {
                                *info = (TextSheetInfo){kinds[i], f[p + 8], f[p + 9], *(const uint16_t *)(f + p + 20),
                                                        *(const uint16_t *)(f + p + 22)};
                                snprintf(info->name, sizeof info->name, "%.20s/%s", path + 5, fonts[i]);
                            }
                            text_sheet_add(f + a + 0x10, bytes, info);
                            break;
                        }
                    break;
                }
                if (size < 8) break;
                p += size;
            }
        }
        free(z);
        free(arc);
    }
    LOG("[fs] sharper text: %d font sheets found\n", text_sheet_count());
}

// The title screen's footer, Layout/Title.sblarc in Pack/Title.pack: a line for the mods, "Mods installed N" (the mods this
// run loaded, runtime/mods.c), under "DLC Ver.3.0" and above the copyright. Title_00.bflyt gets T_Mods_00, a copy of
// T_AoCVersion_00 (its font, colour, size and right edge) shown where that line was (y -303), and the DLC line moves up a
// line (-283). Title_00_VerType.bflan, which puts the version line above the DLC line when there is DLC, puts it a line
// higher too (-283 without DLC, -263 with it). The archive grows by the new pane: its ResourceSizeTable entry grows too.
//
// The same pass adds the title menu's Achievements button (PR #236, by byluki). The menu has seven button parts,
// Pa_SaveFile_00..05 and Pa_SaveFileHardMode_00, all in use with the downloadable content; an eighth, Pa_SaveFile_06 (a
// copy of Pa_SaveFile_05, 40 down), reads Achievements in the menu language: its three text boxes hold it, as the game
// sets the texts of the seven it knows only. The game makes its button like the others' (runtime/ui/game_options.mm
// puts it above Options and has it open the Achievements page). ach: its text (UTF-16), NULL or SAVAGEWIND_NO_TITLE_ACH:
// none.
#define TITLE_FOOTER_UP 20.0f  // (the footer's line height: the version line's own step in Title_00_VerType)
#define TITLE_ACH_PART "Pa_SaveFile_06"

// The Achievements part: a copy of Pa_SaveFile_05's prt1 section s (sz bytes) at c, its texts ach (an UTF-16 units).
// False if the part isn't as expected (c is left to be written over).
static bool title_ach_part(const uint8_t *s, uint32_t sz, uint8_t *c, const uint16_t *ach, int an) {
    memcpy(c, s, sz);
    memset(c + 0xC, 0, 24);
    memcpy(c + 0xC, TITLE_ACH_PART, sizeof TITLE_ACH_PART - 1);
    float y;
    memcpy(&y, c + 0x30, 4);
    y -= 40;
    memcpy(c + 0x30, &y, 4);
    int added = 0;  // (its text boxes: property overrides, txt1 sections in the part, each with its text at [+0x64], a
                    //  buffer of [+0x54] bytes, [+0x56] in use)
    for (uint32_t t = 0x60; t + 0x68 <= sz; t++) {
        if (memcmp(c + t, "txt1", 4)) continue;
        uint32_t tsz, toff;
        memcpy(&tsz, c + t + 4, 4);
        memcpy(&toff, c + t + 0x64, 4);
        uint16_t cap = c[t + 0x54] | c[t + 0x55] << 8;
        if (t + tsz > sz || toff + cap > tsz || cap < 4) return false;
        int m = an < cap / 2 - 1 ? an : cap / 2 - 1;
        memset(c + t + toff, 0, cap);
        memcpy(c + t + toff, ach, (size_t)m * 2);
        uint16_t used = (uint16_t)((m + 1) * 2);
        memcpy(c + t + 0x56, &used, 2);
        added++;
        t += tsz - 1;
    }
    return added == 3;
}

static size_t title_flyt(const uint8_t *b, size_t n, uint8_t *out, size_t cap, const char *text, const uint16_t *ach,
                         int an, bool *ach_added) {
    if (n < 0x14 || memcmp(b, "FLYT", 4) || b[4] != 0xFF || b[5] != 0xFE) return 0;
    uint16_t hs, ns;
    memcpy(&hs, b + 6, 2), memcpy(&ns, b + 0x10, 2);
    const uint8_t *tmpl = NULL;
    size_t at = hs, o = hs;
    bool after_tw = false, inserted = false;
    int added = 1;  // (sections: the mods line, and the Achievements part)
    *ach_added = false;
    memcpy(out, b, hs);
    for (int i = 0; i < ns; i++) {
        uint32_t sz;
        if (o + 8 > n) return 0;
        memcpy(&sz, b + o + 4, 4);
        if (sz < 8 || o + sz > n || at + sz + 0x100 > cap) return 0;
        const uint8_t *s = b + o;
        if (!memcmp(s, "pae1", 4) && after_tw && tmpl && !inserted) {  // (the new pane: before the group's end)
            size_t len = strlen(text), tb = len * 2 + 2, ns2 = 0xA8 + ((tb + 3) & ~(size_t)3);
            uint8_t *p = out + at;
            memcpy(p, tmpl, 0xA8);
            uint32_t s32 = (uint32_t)ns2, zero = 0;
            uint16_t t16 = (uint16_t)tb;
            float y = -303.0f;
            memcpy(p + 4, &s32, 4);
            p[8] |= 1;  // (shown: the DLC line's starts hidden, the animation shows it)
            memset(p + 0xC, 0, 24);
            memcpy(p + 0xC, "T_Mods_00", 9);
            memcpy(p + 0x30, &y, 4);
            memcpy(p + 0x54, &t16, 2), memcpy(p + 0x56, &t16, 2);
            memcpy(p + 0x80, &zero, 4);  // (no text ID: the text is this one)
            memset(p + 0xA8, 0, ns2 - 0xA8);
            for (size_t k = 0; k < len; k++) p[0xA8 + 2 * k] = (uint8_t)text[k];
            at += ns2;
            inserted = true;
        }
        memcpy(out + at, s, sz);
        if (ach && !*ach_added && !memcmp(s, "prt1", 4) && sz >= 0x2c0 && !strncmp((const char *)s + 0xC, "Pa_SaveFile_05", 24) &&
            at + 2 * (size_t)sz + 0x100 <= cap && title_ach_part(s, sz, out + at + sz, ach, an)) {
            at += sz;
            *ach_added = true, added++;
        }
        if (!memcmp(s, "txt1", 4) && sz >= 0xA8) {
            char name[25] = {0};
            memcpy(name, s + 0xC, 24);
            if (!strcmp(name, "T_AoCVersion_00") || !strcmp(name, "T_AoCVersion_00_TW")) {
                float y;
                memcpy(&y, s + 0x30, 4);
                y += TITLE_FOOTER_UP;
                memcpy(out + at + 0x30, &y, 4);
                if (!strcmp(name, "T_AoCVersion_00")) tmpl = s;
                else after_tw = true;
            }
        }
        at += sz, o += sz;
    }
    if (!inserted) return 0;
    uint32_t total = (uint32_t)at;
    uint16_t ns3 = (uint16_t)(ns + added);
    memcpy(out + 0xC, &total, 4);
    memcpy(out + 0x10, &ns3, 2);
    return at;
}

static bool title_flan(uint8_t *b, size_t n) {  // the version line's two places, a line higher (in place)
    float from[2] = {-283.0f, -303.0f}, to[2] = {-283.0f + TITLE_FOOTER_UP, -303.0f + TITLE_FOOTER_UP};
    for (int k = 0; k < 2; k++) {  // (-283 first: -303 becomes -283)
        uint8_t *hit = NULL;
        int count = 0;
        for (size_t i = 0; i + 4 <= n; i += 4)
            if (!memcmp(b + i, &from[k], 4)) hit = b + i, count++;
        if (count != 1) return false;
        memcpy(hit, &to[k], 4);
    }
    return true;
}

static void title_footer_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    static const char pack_path[] = "Pack/Title.pack";
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, pack_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles || getenv("SAVAGEWIND_NO_TITLE_FOOTER")) return;
    if (mods_find_file(R->ov_sub, pack_path, host, sizeof host, NULL, 0)) {
        LOG("[fs] title footer: a mod replaces %s, left as it is\n", pack_path);
        return;
    }
    char text[48];
    snprintf(text, sizeof text, "Mods installed %d", mods_session_mod_count());
    uint16_t ach[16];
    int ach_n = 0;
    if (!getenv("SAVAGEWIND_NO_TITLE_ACH")) {  // (UTF-8 to UTF-16: the Basic Multilingual Plane)
        extern const char *rt_tr(const char *en);  // (runtime/ui/game_options.mm)
        for (const unsigned char *q = (const unsigned char *)rt_tr("Achievements"); *q && ach_n < 16;) {
            uint32_t ch = *q++;
            int more = ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
            if (more) ch &= more == 2 ? 0x0F : 0x1F;
            for (; more && (*q & 0xC0) == 0x80; more--) ch = ch << 6 | (*q++ & 0x3F);
            ach[ach_n++] = (uint16_t)ch;
        }
    }
    bool ach_added = false;
    size_t pn = R->rom_files[k].size, an = 0, zn = 0, nn = 0;
    uint8_t *pack = malloc(pn), *arc = NULL, *arc2 = NULL, *z2 = NULL;
    uint32_t pdoff, ps, pe, adoff, ls, le, ms, me;
    int node = -1, li = -1, mi = -1;
    const char *why = NULL;
    if (romfs_image_pread(R->img, pack, pn, R->rom_files[k].off) == (ssize_t)pn)
        node = sarc_find(pack, pn, "Layout/Title.sblarc", &pdoff, &ps, &pe);
    if (node >= 0) arc = yaz0_unpack(pack + pdoff + ps, pe - ps, &an);
    if (arc) {
        li = sarc_find(arc, an, "blyt/Title_00.bflyt", &adoff, &ls, &le);
        mi = sarc_find(arc, an, "anim/Title_00_VerType.bflan", &adoff, &ms, &me);
    }
    if (node < 0 || !arc) why = "no Layout/Title.sblarc";
    else if (li < 0 || mi < 0) why = "its layout or animation not found";
    if (!why) {
        size_t cap = an + 0x80 + 2 * (le - ls) + 0x400;  // (the archive, then the layout again with the new panes)
        arc2 = calloc(1, cap);
        memcpy(arc2, arc, an);
        nn = (an + 0x7F) & ~(size_t)0x7F;  // (the new layout at the end, as the archive aligns its files)
        size_t ln = title_flyt(arc + adoff + ls, le - ls, arc2 + nn, cap - nn, text, ach_n ? ach : NULL, ach_n, &ach_added);
        if (!ln) why = "the layout isn't as expected";
        else if (!title_flan(arc2 + adoff + ms, me - ms)) why = "the animation isn't as expected";
        else {
            uint32_t s = (uint32_t)(nn - adoff), e = (uint32_t)(nn - adoff + ln), total = (uint32_t)(nn + ln);
            memcpy(arc2 + 0x20 + li * 16 + 8, &s, 4);
            memcpy(arc2 + 0x20 + li * 16 + 12, &e, 4);
            nn += ln;
            memcpy(arc2 + 8, &total, 4);
        }
    }
    if (why) {
        LOG("[fs] title footer: %s, left as it is\n", why);
        free(pack), free(arc), free(arc2);
        return;
    }
    z2 = yaz0_pack(arc2, nn, pack + pdoff + ps, &zn);
    uint64_t old = pe - ps, delta = zn > old ? (zn - old + 0x1FFF) & ~0x1FFFull : 0;
    size_t qn = pn + delta;
    free(R->title_pack);
    R->title_pack = calloc(1, qn);
    memcpy(R->title_pack, pack, pdoff + ps);
    memcpy(R->title_pack + pdoff + ps, z2, zn);
    memcpy(R->title_pack + pdoff + pe + delta, pack + pdoff + pe, pn - pdoff - pe);
    uint32_t cnt = pack[0x1A] | pack[0x1B] << 8, ne = (uint32_t)(ps + zn), q32 = (uint32_t)qn;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t s, e;
        memcpy(&s, R->title_pack + 0x20 + i * 16 + 8, 4);
        memcpy(&e, R->title_pack + 0x20 + i * 16 + 12, 4);
        if (i == (uint32_t)node) { memcpy(R->title_pack + 0x20 + i * 16 + 12, &ne, 4); continue; }
        if (s >= pe) s += (uint32_t)delta, e += (uint32_t)delta;
        memcpy(R->title_pack + 0x20 + i * 16 + 8, &s, 4);
        memcpy(R->title_pack + 0x20 + i * 16 + 12, &e, 4);
    }
    memcpy(R->title_pack + 8, &q32, 4);
    free(pack), free(arc), free(arc2), free(z2);
    meta_copy(fmeta, foff, fsz);
    uint64_t rel = R->ov_end - R->rom_data_off, size = qn;
    memcpy(R->ov_meta + R->rom_files[k].meta + 8, &rel, 8);
    memcpy(R->ov_meta + R->rom_files[k].meta + 0x10, &size, 8);
    if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
    R->ov[R->ov_n++] = (Overlay){R->ov_end, size, NULL, -1, R->title_pack};
    R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
    R->grown = realloc(R->grown, sizeof *R->grown * (R->ngrown + 1));
    R->grown[R->ngrown++] = (GrownFile){"Layout/Title.blarc", nn - an};
    LOG("[fs] title footer: \"%s\" under the DLC line%s (archive %zu -> %zu bytes)\n", text,
        ach_added ? ", Achievements button (" TITLE_ACH_PART ")" : ach_n ? ", no Achievements button (Pa_SaveFile_05 not as expected)" : "",
        an, nn);
}

static void hud_letters_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    static const char pack_path[] = "Pack/Bootup.pack";
    static const char *const layouts[] = {"blyt/PaDoCommandA_00.bflyt", "blyt/PaDoCommandB_00.bflyt",
                                          "blyt/PaDoCommandX_00.bflyt", "blyt/PaDoCommandY_00.bflyt"};
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, pack_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles) return;
    if (mods_find_file(R->ov_sub, pack_path, host, sizeof host, NULL, 0)) {
        if (R == &images[0]) inventory_layout_from_overlay(host);
        return;
    }
    uint64_t t0 = plat_now_ns();
    size_t pn = R->rom_files[k].size, zn = 0, an = 0, nn = 0;
    uint8_t *pack = malloc(pn), *arc = NULL, *arc2 = NULL, *z2 = NULL;
    uint32_t pdoff, ps, pe, adoff, as, ae;
    int node = -1, panes = 0;
    if (romfs_image_pread(R->img, pack, pn, R->rom_files[k].off) == (ssize_t)pn)
        node = sarc_find(pack, pn, "Layout/Common.sblarc", &pdoff, &ps, &pe);
    if (node >= 0) arc = yaz0_unpack(pack + pdoff + ps, pe - ps, &an);
    if (R == &images[0] && arc) inventory_layout_from_common(arc, an);
    if (getenv("SAVAGEWIND_NO_HUD_LETTERS")) { free(pack), free(arc); return; }
    const char *why = node < 0 ? "no Layout/Common.sblarc" : !arc ? "archive not readable" : NULL;
    int pos[4];
    glyphs_hud_positions(pos);
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
            size_t ln = hud_letters_flyt(arc + adoff + as, ae - as, arc2 + nn, i, pos, &panes);
            if (!ln) { why = "layout not as expected"; break; }
            uint32_t s = (uint32_t)(nn - adoff), e = (uint32_t)(nn - adoff + ln);
            memcpy(arc2 + 0x20 + li * 16 + 8, &s, 4);
            memcpy(arc2 + 0x20 + li * 16 + 12, &e, 4);
            nn += ln;
        }
    }
    if (!why && panes != 12) why = "letters not as expected";
    if (arc && !why) {  // the Controls page's lines and the tips' marks to the device's buttons (runtime/nvn/hw_illust.c)
        static const char *const lay[] = {"blyt/PaAllControllerTipsNN_00.bflyt", "blyt/PaMessageTipsDrcImageNN_00.bflyt"};
        int dev = glyphs_console();
        for (int i = dev >= 0 ? 0 : 1; i < 2; i++) {  // (HW_LAYOUT_CONTROLS, _TIPS; the Switch's: the tips' face marks)
            int li = sarc_find(arc, an, lay[i], &adoff, &as, &ae);
            int moved = li >= 0 ? hw_illust_layout(dev, i, arc2 + adoff + as, ae - as, pos) : 0;
            if (dev >= 0 || moved)
                LOG("[fs] %s: %d panes moved to device %d's buttons%s\n", i ? "Tips" : "Controls page", moved, dev,
                    moved ? "" : " (layout not as expected)");
        }
    }
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
    R->hud_pack_size = qn;
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
    glyphs_hud_positions_built(pos);
    LOG("[fs] HUD letters: %d button letters use the icon font (archive %zu -> %zu bytes, pack +%llu KB, %.0f ms)\n",
        panes, (size_t)old, zn, (unsigned long long)(delta >> 10), (plat_now_ns() - t0) / 1e6);
}

// The HUD diamond's letters are spare cells of the icon font (glyphs_letter_code): the power button, an arrow and two
// empty squares, narrower than a letter or with no advance at all, so the letters drawn into them came out clipped and
// off centre. Each font archive (Font/Font_*.sbfarc, Yaz0 over SARC) with External_00.bffnt gets those cells' widths
// (CWDH: left, glyph width, advance) set to the letters'; same size unpacked, so the ResourceSizeTable stays.
static void font_widths_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    if (getenv("SAVAGEWIND_NO_HUD_LETTERS")) return;
    int done = 0, nf = 0;
    for (int k = 0; k < R->rom_nfiles && nf < 8; k++) {
        const char *p = R->rom_files[k].path;
        size_t pl = strlen(p);
        if (strncmp(p, "Font/Font_", 10) || pl < 7 || strcmp(p + pl - 7, ".sbfarc")) continue;
        char host[1200];
        if (mods_find_file(R->ov_sub, p, host, sizeof host, NULL, 0)) continue;  // (a mod's own font: left as it is)
        size_t zn = R->rom_files[k].size, an = 0, outn = 0;
        if (zn < 16 || zn > (64u << 20)) continue;
        uint8_t *z = malloc(zn), *arc = NULL, *out = NULL;
        if (z && romfs_image_pread(R->img, z, zn, R->rom_files[k].off) == (ssize_t)zn && !memcmp(z, "Yaz0", 4))
            arc = yaz0_unpack(z, zn, &an);
        uint32_t doff, s, e;
        bool changed = false;
        if (arc && sarc_find(arc, an, "External_00.bffnt", &doff, &s, &e) >= 0 && e > s && doff + e <= an) {
            uint8_t *f = arc + doff + s;
            size_t n = e - s;
            for (size_t o = 0x14; o + 0x10 <= n; ) {  // (FFNT: the sections follow its 0x14-byte header)
                uint32_t sz;
                memcpy(&sz, f + o + 4, 4);
                if (sz < 8 || o + sz > n) break;
                if (!memcmp(f + o, "CWDH", 4)) {
                    uint16_t lo = f[o + 8] | f[o + 9] << 8, hi = f[o + 10] | f[o + 11] << 8;
                    for (int b = 0; b < 4; b++) {
                        int cell = glyphs_letter_index(b), letter = b;  // (A B X Y: glyphs 0-3)
                        if (cell < lo || cell > hi || letter < lo || 0x10 + 3 * (size_t)(cell - lo) + 3 > sz) continue;
                        memcpy(f + o + 0x10 + 3 * (cell - lo), f + o + 0x10 + 3 * (letter - lo), 3);
                        changed = true;
                    }
                    break;
                }
                o += sz;
            }
        }
        if (changed) out = yaz0_pack(arc, an, z, &outn);
        if (out) {
            R->font_arcs[nf++] = out;
            meta_copy(fmeta, foff, fsz);
            uint64_t rel = R->ov_end - R->rom_data_off, size = outn;
            memcpy(R->ov_meta + R->rom_files[k].meta + 8, &rel, 8);
            memcpy(R->ov_meta + R->rom_files[k].meta + 0x10, &size, 8);
            if (R->ov_n == R->ov_cap) { R->ov_cap = R->ov_cap ? R->ov_cap * 2 : 256; R->ov = realloc(R->ov, sizeof *R->ov * R->ov_cap); }
            R->ov[R->ov_n++] = (Overlay){R->ov_end, size, NULL, -1, out};
            R->ov_end = (R->ov_end + size + 0xF) & ~0xFull;
            done++;
        }
        free(z), free(arc);
    }
    if (nf || done) LOG("[fs] HUD letters: %d font archive(s) with the letters' widths\n", done);
}

// (Savage Wind+) Video > Haze: the air's haze. BOTW's outdoor haze is its atmospheric scattering, set per climate in
// Pack/TitleBG.pack's WorldMgr/normal.bwinfo (AAMP, 20 climates): CalcMie and CalcRayleigh (the white and the blue of
// the air: lower, clearer; at 0.6x the sky darkens and turns teal) and CalcSfParamNear (0 or below: the haze close by,
// least at 0). The setting's share k of the game's haze gives those 0.7 + 0.3 k times theirs and k times the near one.
// k stays at 75 % or more (HAZE_MIN): at 50 % (0.85x) the sky, the far mountains and the ground already turn
// green-teal on the Great Plateau at day, so 0-50 are left out. The file is read at the start: one patch over its
// bytes, the same size.
static uint32_t crc32_str(const char *s) { return (uint32_t)crc32(0, (const uint8_t *)s, (uInt)strlen(s)); }
static size_t haze_done[512];  // (the value offsets changed so far)
static void haze_walk_list(uint8_t *d, size_t n, size_t o, const uint32_t h[3], const float f[3], int *changed, int depth) {
    if (o + 12 > n || depth > 8) return;
    uint16_t lo, ln, oo, on;
    memcpy(&lo, d + o + 4, 2), memcpy(&ln, d + o + 6, 2), memcpy(&oo, d + o + 8, 2), memcpy(&on, d + o + 10, 2);
    for (uint32_t i = 0; i < on; i++) {
        size_t ob = o + (size_t)oo * 4 + i * 8;
        if (ob + 8 > n) return;
        uint16_t po, pn;
        memcpy(&po, d + ob + 4, 2), memcpy(&pn, d + ob + 6, 2);
        for (uint32_t j = 0; j < pn; j++) {
            size_t pb = ob + (size_t)po * 4 + j * 8;
            if (pb + 8 > n) return;
            uint32_t ph, w;
            memcpy(&ph, d + pb, 4), memcpy(&w, d + pb + 4, 4);
            size_t vo = pb + (size_t)(w & 0xFFFFFF) * 4;
            if (w >> 24 != 1 || vo + 4 > n) continue;  // (floats only)
            bool seen = false;  // (AAMP shares equal values: one value is the data of several parameters)
            for (int k = 0; k < *changed && !seen; k++) seen = haze_done[k] == vo;
            if (seen) continue;
            for (int k = 0; k < 3; k++)
                if (ph == h[k] && *changed < 512) {
                    haze_done[*changed] = vo;
                    float v;
                    memcpy(&v, d + vo, 4);
                    v *= f[k];
                    memcpy(d + vo, &v, 4);
                    (*changed)++;
                }
        }
    }
    (void)lo, (void)ln, (void)depth;  // (the root's objects only: the climates. Lists under it hold values of the same
                                      //  names that this scaling turned teal)
}
static void haze_patch(void) {
    int pct = g_settings.haze < HAZE_MIN ? HAZE_MIN : g_settings.haze > HAZE_MAX ? HAZE_MAX : g_settings.haze;
    if (pct == 100 || R != &images[0] || R->npatches >= 16) return;
    static const char pack_path[] = "Pack/TitleBG.pack", name[] = "WorldMgr/normal.bwinfo";
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, pack_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles || mods_find_file(R->ov_sub, pack_path, host, sizeof host, NULL, 0)) return;
    uint8_t head[0x20];
    if (romfs_image_pread(R->img, head, 0x20, R->rom_files[k].off) != 0x20 || memcmp(head, "SARC", 4)) return;
    uint32_t doff;
    memcpy(&doff, head + 0xC, 4);
    if (doff > (16u << 20) || doff > R->rom_files[k].size) return;
    uint8_t *tab = malloc(doff);  // (the tables: the data starts at doff)
    uint32_t ds = 0, de = 0;
    bool found = false;
    if (tab && romfs_image_pread(R->img, tab, doff, R->rom_files[k].off) == (ssize_t)doff) {
        uint32_t cnt = tab[0x1A] | tab[0x1B] << 8;
        size_t names = 0x20 + (size_t)cnt * 16 + 8;
        for (uint32_t i = 0; i < cnt && names <= doff && !found; i++) {
            uint32_t attr;
            memcpy(&attr, tab + 0x20 + i * 16 + 4, 4);
            size_t at = names + (size_t)(attr & 0xFFFFFF) * 4;
            if (!(attr >> 24) || at + sizeof name > doff || memcmp(tab + at, name, sizeof name)) continue;
            memcpy(&ds, tab + 0x20 + i * 16 + 8, 4), memcpy(&de, tab + 0x20 + i * 16 + 12, 4);
            found = de > ds && de - ds < (4u << 20);
        }
    }
    free(tab);
    if (!found) { LOG("[fs] haze: %s not found, the game's haze stays\n", name); return; }
    size_t n = de - ds;
    uint64_t at = R->rom_files[k].off + doff + ds;
    uint8_t *d = malloc(n);
    if (!d || romfs_image_pread(R->img, d, n, at) != (ssize_t)n || memcmp(d, "AAMP", 4)) { free(d); return; }
    float kk = pct / 100.0f, sc = 0.7f + 0.3f * kk;
    const uint32_t h[3] = {crc32_str("CalcMie"), crc32_str("CalcRayleigh"), crc32_str("CalcSfParamNear")};
    const float f[3] = {sc, sc, kk};
    uint32_t pio;
    memcpy(&pio, d + 0x14, 4);
    int changed = 0;
    haze_walk_list(d, n, 0x30 + (size_t)pio, h, f, &changed, 0);
    if (!changed) { free(d); LOG("[fs] haze: no climate values found, the game's haze stays\n"); return; }
    if (getenv("SAVAGEWIND_HAZE_DUMP")) {  // (tests) the patched file, to compare
        FILE *df = fopen(getenv("SAVAGEWIND_HAZE_DUMP"), "wb");
        if (df) fwrite(d, 1, n, df), fclose(df);
    }
    R->patches[R->npatches++] = (Patch){at, n, d};
    LOG("[fs] haze %d %%: %d climate values (scattering x%.2f, near haze x%.2f)\n", pct, changed, sc, kk);
}

// The shrine sensor's volume is applied live in the mixer (runtime/shrine_sensor.c, runtime/services/audio.c): here
// its sound's ADPCM coefficients are noted from both copies of M_UI.bars (in Pack/Bootup.pack and the loose file),
// as the game reads them (a mod's overlay first), so the mixer can tell its voices apart.
static int shrine_sensor_note_range(uint64_t at, size_t n) {
    if (n == 0 || n >= (32u << 20)) return 0;
    uint8_t *d = malloc(n);
    int found = d && romfs_image_pread(R->img, d, n, at) == (ssize_t)n ? shrine_sensor_note_bars(d, n) : 0;
    free(d);
    return found;
}

static int shrine_sensor_note_host(const char *host) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    int found = 0;
    if (!fseek(f, 0, SEEK_END)) {
        long n = ftell(f);
        uint8_t *d = n > 0 && n < (32l << 20) ? malloc((size_t)n) : NULL;
        if (d && !fseek(f, 0, SEEK_SET) && fread(d, 1, (size_t)n, f) == (size_t)n) found = shrine_sensor_note_bars(d, (size_t)n);
        free(d);
    }
    fclose(f);
    return found;
}

static void shrine_sensor_note(void) {
    if (R != &images[0]) return;
    static const char pack_path[] = "Pack/Bootup.pack", name[] = "Sound/Resource/M_UI.bars";
    int found = 0;
    char host[1200];

    // In Pack/Bootup.pack: the HUD letters' copy in memory (R->hud_pack) when there is one, else the RomFS's
    if (R->hud_pack && R->hud_pack_size) {
        uint32_t doff = 0, ds = 0, de = 0;
        if (sarc_find(R->hud_pack, R->hud_pack_size, name, &doff, &ds, &de) >= 0 && de > ds &&
            (size_t)doff + de <= R->hud_pack_size)
            found += shrine_sensor_note_bars(R->hud_pack + doff + ds, de - ds);
    } else {
        int k = 0;
        while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, pack_path)) k++;
        uint8_t head[0x20];
        uint32_t doff = 0, ds = 0, de = 0;
        if (k < R->rom_nfiles && !mods_find_file(R->ov_sub, pack_path, host, sizeof host, NULL, 0) &&
            romfs_image_pread(R->img, head, 0x20, R->rom_files[k].off) == 0x20 && !memcmp(head, "SARC", 4)) {
            memcpy(&doff, head + 0xC, 4);
            uint8_t *tab = doff <= (16u << 20) && doff <= R->rom_files[k].size ? malloc(doff) : NULL;
            if (tab && romfs_image_pread(R->img, tab, doff, R->rom_files[k].off) == (ssize_t)doff) {
                uint32_t cnt = tab[0x1A] | tab[0x1B] << 8;
                size_t names = 0x20 + (size_t)cnt * 16 + 8;
                for (uint32_t i = 0; i < cnt && names <= doff; i++) {
                    uint32_t attr;
                    memcpy(&attr, tab + 0x20 + i * 16 + 4, 4);
                    size_t at = names + (size_t)(attr & 0xFFFFFF) * 4;
                    if (!(attr >> 24) || at + sizeof name > doff || memcmp(tab + at, name, sizeof name)) continue;
                    memcpy(&ds, tab + 0x20 + i * 16 + 8, 4), memcpy(&de, tab + 0x20 + i * 16 + 12, 4);
                    if (de > ds) found += shrine_sensor_note_range(R->rom_files[k].off + doff + ds, de - ds);
                    break;
                }
            }
            free(tab);
        }
    }

    // The loose Sound/Resource/M_UI.bars (a mod's own copy when it has one)
    if (mods_find_file(R->ov_sub, name, host, sizeof host, NULL, 0)) found += shrine_sensor_note_host(host);
    else {
        int k = 0;
        while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, name)) k++;
        if (k < R->rom_nfiles) found += shrine_sensor_note_range(R->rom_files[k].off, R->rom_files[k].size);
    }

    if (found) LOG("[fs] shrine sensor: %d channel%s known, volume %d%% (live)\n", found, found == 1 ? "" : "s",
                   g_settings.shrine_sensor_volume);
    else LOG("[fs] shrine sensor: its sound was not found, the volume setting does nothing\n");
}

// An RSTB entry: names first (paths whose crc32 collides), then the crc32 table (sorted). NULL when it has none.
static uint32_t *rstb_entry(uint8_t *d, uint32_t ncrc, uint32_t nname, const char *path) {
    for (uint32_t i = 0; i < nname; i++)
        if (!strncmp((const char *)d + 12 + ncrc * 8 + i * 132, path, 128))
            return (uint32_t *)(d + 12 + ncrc * 8 + i * 132 + 128);
    uint32_t crc = (uint32_t)crc32(0, (const Bytef *)path, (uInt)strlen(path));
    for (uint32_t lo = 0, hi = ncrc; lo < hi;) {
        uint32_t mid = (lo + hi) / 2, c;
        memcpy(&c, d + 12 + mid * 8, 4);
        if (c == crc) return (uint32_t *)(d + 12 + mid * 8 + 4);
        if (c < crc) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// A RomFS with the DLC merged into it (#188, #242): some dumping guides copy the DLC's RomFS over the game's, so its
// main-map files (Map/MainField/D-7/D-7_Dynamic.smubin...) are the DLC's versions, a few KB bigger than the game's.
// On the console the game reads those from the DLC (aoc:/0010/...), sized by their own "Aoc/0010/<path>" entries,
// which the game's RSTB has. Read from the game's own paths, the game finds them bigger than those paths' entries,
// can't load them and retries for ever: a white screen after "Open your eyes" (the Great Plateau's map sections).
// Such a RomFS has the DLC's System/AocVersion.txt or Pack/AocMainField.pack; there each path's entry grows to its
// Aoc/0010 entry when that one is bigger.
static bool romfs_has_dlc_merged(void) {
    static const char *const marks[] = {"System/AocVersion.txt", "Pack/AocMainField.pack"};
    char host[1200];
    for (int m = 0; m < 2; m++) {
        if (mods_find_file(R->ov_sub, marks[m], host, sizeof host, NULL, 0)) return true;
        for (int i = 0; i < R->rom_nfiles; i++)
            if (!strcmp(R->rom_files[i].path, marks[m])) return true;
    }
    return false;
}

// The RSTB's name for a RomFS path: Yaz0-compressed files drop the 's' their extension starts with
// (D-7_Dynamic.smubin -> D-7_Dynamic.mubin); .sarc is the one extension that really starts with an 's'.
static void rstb_name(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *dot = strrchr(out, '.');
    if (dot && dot[1] == 's' && strcmp(dot, ".sarc") && !strchr(dot, '/')) memmove(dot + 1, dot + 2, strlen(dot + 2) + 1);
}

// A file's size once loaded: a Yaz0 file's header gives it, any other is its size
static uint64_t loaded_size(const uint8_t head[8], uint64_t size) {
    return memcmp(head, "Yaz0", 4) ? size : (uint64_t)head[4] << 24 | head[5] << 16 | head[6] << 8 | head[7];
}

// The overlay folder's replacement for RomFS file i (host) loads bigger than the game's own: the game's loader takes a
// file's room from the ResourceSizeTable, so it fails to load it (a mod's bigger font or text archive, #898) unless the
// mod brings its own table. Its entry grows by the difference (rstb_patch), the game's entry keeping what it needs over
// the file's size (PR #907).
static void mod_grown(int i, const char *host, uint64_t host_size) {
    uint8_t mine[8] = {0}, theirs[8] = {0};
    FILE *f = fopen(host, "rb");
    if (!f) return;
    size_t got = fread(theirs, 1, 8, f);
    fclose(f);
    if (got < 8 || R->rom_files[i].size < 8 || romfs_image_pread(R->img, mine, 8, R->rom_files[i].off) != 8) return;
    uint64_t own = loaded_size(mine, R->rom_files[i].size), mod = loaded_size(theirs, host_size);
    if (mod <= own) return;
    char name[300];
    rstb_name(R->rom_files[i].path, name, sizeof name);
    R->grown = realloc(R->grown, sizeof *R->grown * (R->ngrown + 1));
    R->grown[R->ngrown++] = (GrownFile){strdup(name), mod - own, true};
}

// Room the model loader gets on top of the ResourceSizeTable's size for each model (.bfres), in KB. The loader makes a
// temporary heap from a model's entry and allocates one node per material from it without checking for failure; in
// the Gerudo desert (the Wasteland Tower save) a few models (FldObj_RuinStatueKnight_A-03) overflow it, and the
// allocation returns NULL: "access violation: write at 0x8" about one load in five. 64 KB held for 20 loads in a row
// (unpatched, a crash came within 7 loads each time); 256 KB held for 20. SAVAGEWIND_RSTB_MODEL_KB=n changes it, 0 leaves the table as it is.
#define RSTB_MODEL_KB 64

static uint32_t rstb_model_grow(void) {
    const char *e = getenv("SAVAGEWIND_RSTB_MODEL_KB");
    long kb = e && *e ? atol(e) : RSTB_MODEL_KB;
    return kb > 0 ? (uint32_t)kb << 10 : 0;
}

static void rstb_patch(const uint8_t *fmeta, uint64_t foff, uint64_t fsz) {
    static const char rstb_path[] = "System/Resource/ResourceSizeTable.product.srsizetable";
    bool merged = R == &images[0] && romfs_has_dlc_merged();
    uint32_t model_grow = R == &images[0] ? rstb_model_grow() : 0;
    if (!R->ngrown && !merged && !model_grow) return;
    int k = 0;
    while (k < R->rom_nfiles && strcmp(R->rom_files[k].path, rstb_path)) k++;
    char host[1200];
    if (k == R->rom_nfiles && R != &images[0]) {  // (an add-on content's voices are streams: sized from the file)
        LOG("[fs] voices: %d bigger files, none in a ResourceSizeTable\n", R->ngrown);
        return;
    }
    if (k == R->rom_nfiles) { LOG("[fs] voices: no %s, %d bigger banks left as they are\n", rstb_path, R->ngrown); return; }
    // A mod's table (the overlay folder's) needs the same growth: without it the game can't load what grew here
    struct stat over;
    bool from_overlay = mods_find_file(R->ov_sub, rstb_path, host, sizeof host, NULL, 0) && !stat(host, &over);
    size_t zn = from_overlay ? (size_t)over.st_size : R->rom_files[k].size, n = 0;
    uint8_t *z = malloc(zn), *d = NULL;
    if (from_overlay) {
        FILE *f = fopen(host, "rb");
        if (f && fread(z, 1, zn, f) == zn) d = yaz0_unpack(z, zn, &n);
        if (f) fclose(f);
    } else if (romfs_image_pread(R->img, z, zn, R->rom_files[k].off) == (ssize_t)zn) {
        d = yaz0_unpack(z, zn, &n);
    }
    free(z);
    uint32_t ncrc = 0, nname = 0;
    if (d && n >= 12) memcpy(&ncrc, d + 4, 4), memcpy(&nname, d + 8, 4);
    if (!d || memcmp(d, "RSTB", 4) || 12 + (uint64_t)ncrc * 8 + (uint64_t)nname * 132 > n) {
        LOG("[fs] voices: ResourceSizeTable not as expected, %d bigger banks left as they are\n", R->ngrown);
        free(d);
        return;
    }
    int changed = 0, dlc_changed = 0, mods = 0;
    uint64_t added = 0, dlc_added = 0;
    for (int g = 0; g < R->ngrown; g++) {
        if (R->grown[g].mod && from_overlay) continue;
        char rname[300];
        rstb_name(R->grown[g].path, rname, sizeof rname);
        uint32_t grow = (uint32_t)((R->grown[g].grow + 31) & ~31ull) + 32, *size = rstb_entry(d, ncrc, nname, rname);
        if (!size) continue;  // (no entry: the game sizes it from the file, the dialogue streams)
        uint32_t v;
        memcpy(&v, size, 4);
        if ((uint64_t)v + grow > UINT32_MAX) continue;
        v += grow;
        memcpy(size, &v, 4);
        changed++;
        added += grow;
        mods += R->grown[g].mod;
    }
    if (mods) LOG("[fs] overlay: %d ResourceSizeTable entries grown for files bigger than the game's own\n", mods);
    if (merged) {
        char name[300], aoc[320];
        for (int i = 0; i < R->rom_nfiles; i++) {
            rstb_name(R->rom_files[i].path, name, sizeof name);
            snprintf(aoc, sizeof aoc, "Aoc/0010/%s", name);
            uint32_t *base = rstb_entry(d, ncrc, nname, name), *dlc = base ? rstb_entry(d, ncrc, nname, aoc) : NULL;
            if (!dlc) continue;
            uint32_t bv, dv;
            memcpy(&bv, base, 4), memcpy(&dv, dlc, 4);
            if (dv <= bv) continue;
            memcpy(base, &dv, 4);
            dlc_changed++;
            dlc_added += dv - bv;
        }
        LOG("[fs] the RomFS has the DLC merged into it: %d ResourceSizeTable entries grown to the DLC's (%llu KB)\n",
            dlc_changed, (unsigned long long)(dlc_added >> 10));
    }
    if (model_grow) {
        int mg = 0;
        char name[300];
        for (int i = 0; i < R->rom_nfiles; i++) {
            size_t pl = strlen(R->rom_files[i].path);
            if (pl < 8 || strcmp(R->rom_files[i].path + pl - 7, ".sbfres")) continue;
            rstb_name(R->rom_files[i].path, name, sizeof name);
            uint32_t *e = rstb_entry(d, ncrc, nname, name), v;
            if (!e) continue;
            memcpy(&v, e, 4), v += model_grow, memcpy(e, &v, 4);
            mg++;
        }
        LOG("[fs] ResourceSizeTable: %d model entries grown by %u KB\n", mg, model_grow >> 10);
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
    if (R->ngrown)
        LOG("[fs] voices, HUD layout and mods: %d files bigger than the game's own, %d ResourceSizeTable entries "
            "grown (%llu KB in all)\n", R->ngrown, changed, (unsigned long long)(added >> 10));
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
        if (!mods_find_file(R->ov_sub, R->rom_files[i].path, host, sizeof host, NULL, 0)) continue;
        struct stat sb;
        if (stat(host, &sb)) continue;
        if (R == &images[0]) mod_grown(i, host, (uint64_t)sb.st_size);
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
    // file. The game keeps that file's text in 16 bytes, so the whole version comes from rt_title_version_text where
    // the game code has that patch; this is what is left without it.
    for (int i = 0; i < R->rom_nfiles; i++) {
        if (strcmp(R->rom_files[i].path, "System/Version.txt")) continue;
        char host[1200];
        struct stat sb;
        if (mods_find_file(R->ov_sub, R->rom_files[i].path, host, sizeof host, NULL, 0) && !stat(host, &sb)) break;
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
    if (R == &images[0]) title_footer_patch(fmeta, foff, fsz);
    font_widths_patch(fmeta, foff, fsz);
    text_sheets_scan();
    menu_tex_scan();
    if (R == &images[0] && !atomic_load_explicit(&inventory_clip_layout_valid, memory_order_acquire))
        LOG("[fs] Inventory clip layout unavailable; using the game's clipping\n");
    rstb_patch(fmeta, foff, fsz);
    haze_patch();
    shrine_sensor_note();
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

// The version line's own text (tools/patches.py _title_version_text, where main loads System/Version.txt's for it):
// "Savage Wind" and the whole version without -alpha.N / -beta.N, "Savage Wind 0.6.0". The game keeps Version.txt's
// text in 16 bytes, which cut it to "Savage Wind 0.6". orig: the game's own, when there is nothing here.
uint64_t rt_title_version_text(uint64_t orig) {
    static char text[48];
    if (!text[0]) {
        extern const char *rt_version(void);  // (runtime/ui/launcher.mm)
        const char *v = rt_version();
        snprintf(text, sizeof text, "Savage Wind %.*s", (int)strcspn(v, "-+ "), v);
    }
    return text[0] ? (uint64_t)(uintptr_t)text : orig;
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

// Image offset where the game's RomFS file containing `off` starts (~0 if none).
static uint64_t romfs_file_start(uint64_t off) {
    const RomImage *m = &images[0];
    int lo = 0, hi = m->rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < m->rom_files[mid].off) hi = mid - 1;
        else if (off >= m->rom_files[mid].off + m->rom_files[mid].size) lo = mid + 1;
        else return m->rom_files[mid].off;
    }
    return ~0ull;
}

// The game's own RomFS image is open (the game opens it soon after it starts; a reboot closes it): fs_game_file can read
// it from any thread
bool fs_game_files_open(void) { return atomic_load_explicit(&game_image_open, memory_order_acquire); }

// A whole file of the game's own RomFS image, as the image has it (not the overlay folder's), malloc'd (free it); NULL
// if it isn't there, or the image isn't open. For host features that read the player's game data (runtime/items.c,
// runtime/collectibles_table.c), from any thread: a reboot waits for a read in progress before it closes the image.
uint8_t *fs_game_file(const char *path, size_t *out_n) {
    RomImage *m = &images[0];
    uint8_t *d = NULL;
    pthread_mutex_lock(&game_file_lock);
    for (int k = 0; fs_game_files_open() && k < m->rom_nfiles; k++) {
        if (strcmp(m->rom_files[k].path, path)) continue;
        size_t n = m->rom_files[k].size;
        d = malloc(n ? n : 1);
        if (d && romfs_image_pread(m->img, d, n, m->rom_files[k].off) == (ssize_t)n) *out_n = n;
        else free(d), d = NULL;
        break;
    }
    pthread_mutex_unlock(&game_file_lock);
    return d;
}

// Path of the file of image m containing image offset `off` (NULL if none).
static const char *image_file_at(const RomImage *m, uint64_t off) {
    int lo = 0, hi = m->rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < m->rom_files[mid].off) hi = mid - 1;
        else if (off >= m->rom_files[mid].off + m->rom_files[mid].size) lo = mid + 1;
        else return m->rom_files[mid].path;
    }
    return NULL;
}

// Music volume (runtime/music_volume.c): at the game's first read of a music file of image m, its channels'
// coefficients are noted from the file as the game reads it (a mod's copy first): a stream's header and INFO block, or a
// whole bank. Each file once; a file that is not music costs the path test only.
static void music_note_read(RomImage *m, uint64_t off) {
    int lo = 0, hi = m->rom_nfiles - 1;
    const RomFile *f = NULL;
    while (lo <= hi && !f) {
        int mid = (lo + hi) / 2;
        if (off < m->rom_files[mid].off) hi = mid - 1;
        else if (off >= m->rom_files[mid].off + m->rom_files[mid].size) lo = mid + 1;
        else f = &m->rom_files[mid];
    }
    if (!f || !music_path(f->path)) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const RomFile *done[2048];
    static int ndone;
    pthread_mutex_lock(&lock);
    int seen = 0;
    for (int i = 0; i < ndone && !seen; i++) seen = done[i] == f;
    if (!seen && ndone < 2048) done[ndone++] = f;
    pthread_mutex_unlock(&lock);
    if (seen) return;
    size_t pl = strlen(f->path), n = f->size;
    if (pl > 6 && !strcasecmp(f->path + pl - 6, ".bfstm") && n > 0x10000) n = 0x10000;   // (header and INFO block)
    if (!n || n > (32u << 20)) return;
    uint8_t *d = malloc(n);
    if (d && romfs_read(m, d, n, f->off) == (ssize_t)n) music_note_file(f->path, d, n);
    free(d);
}

// The last RomFS files the game read, newest last, for a crash report (runtime/dispatch.c): a file that makes the
// game's parser run out of its resource heap names itself only this way (#363).
static const char *recent_files[8];
static _Atomic unsigned recent_n;
int fs_recent_files(const char **out, int max) {
    unsigned n = atomic_load(&recent_n);
    int k = 0;
    for (unsigned i = n > 8 ? n - 8 : 0; i < n && k < max; i++) out[k++] = recent_files[i & 7];
    return k;
}

// Path of the game's RomFS file containing image offset `off` (NULL if none).
const char *romfs_file_at(uint64_t off) { return image_file_at(&images[0], off); }

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
        uint64_t ft0 = plat_now_ns();
        ssize_t n = romfs_read(m, (uint8_t *)(uintptr_t)b->addr, size, off);
        perfcount_add(PC_FILE_READS, 1), perfcount_add(PC_FILE_NS, plat_now_ns() - ft0);
        if (n > 0) perfcount_add(PC_FILE_BYTES, (uint64_t)n);
        if (n < 0) LOG("[fs] storage read failed at %llx: %s\n", off, strerror(errno));
        if (n > 0) music_note_read(m, off);
        if (m != &images[0]) {  // (add-on content: its shrines and Divine Beasts)
            const char *path = image_file_at(m, off);
            static const char *last;
            if (path && path != last) {
                last = path;
                if (getenv("SAVAGEWIND_LOG_FILES")) LOG("[fs] read aoc %s\n", path);
                game_place_on_read(path);
            }
            break;
        }
        {
            const char *path = romfs_file_at(off);
            static const char *last;
            if (path && path != last) {
                last = path;
                recent_files[atomic_load(&recent_n) & 7] = path, atomic_fetch_add(&recent_n, 1);
                if (getenv("SAVAGEWIND_LOG_FILES")) LOG("[fs] read %s\n", path);
                game_place_on_read(path);
            }
            // SAVAGEWIND_LOG_STREAMS=1: every read of a stream's header (a .bfstm read from its start: the sound
            // system opening it, once per playback of a BGM or other streamed sound)
            static int log_streams = -1;
            if (log_streams < 0) log_streams = getenv("SAVAGEWIND_LOG_STREAMS") ? 1 : 0;
            size_t pl = path && log_streams ? strlen(path) : 0;
            if (pl > 6 && !strcmp(path + pl - 6, ".bfstm") && romfs_file_start(off) == off)
                LOG("[fs] stream %s (%.2f s)\n", path, plat_now_ns() / 1e9);
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
        if (!k) atomic_store_explicit(&game_image_open, true, memory_order_release);
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
typedef struct { int fd; char path[1100]; uint8_t *patched; uint64_t patched_size; bool written, save_written; } HostFile;  // (written: since its last Flush)

// The game wrote a file of a save (game_data.sav, or caption.sav next to it: Master Mode's mark): the achievements
// read the save now, as played - before the game may load it back (after Ganon's end it does at once)
static void save_written(const char *path) {
    size_t n = strlen(path);
    if (n >= 13 && !strcmp(path + n - 13, "game_data.sav")) ach_game_wrote(path);
    else if (n >= 11 && !strcmp(path + n - 11, "caption.sav") && n < 1100) {
        char g[1100];
        snprintf(g, sizeof g, "%.*sgame_data.sav", (int)(n - 11), path);
        if (!access(g, F_OK)) ach_game_wrote(g);
    }
}
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

// A save operation that failed: whether to try again (it waited). On Windows another program can hold a save file for a
// moment (OneDrive's sync of a Desktop folder, an antivirus scan, a backup): opening, writing or renaming it then fails
// with EACCES. The game takes any error there as fatal: an SDK assertion in SaveMgrThread, "svcBreak(reason=0)" while
// "Saving..." showed (#568, #567; the same here with the save files held open by another process). So those errors
// are retried for up to 30 s (only the save thread waits: the game goes on with "Saving..." up). Errors the game
// expects (a file that doesn't exist yet, or already does) aren't logged.
static bool fs_retry(int *tries, const char *op, const char *path) {
    int e = errno;
    if (e == ENOENT || e == ENOTDIR || e == EEXIST || e == ENOTEMPTY) return false;
#ifdef _WIN32
    if ((e == EACCES || e == EBUSY || e == EPERM) && *tries < 600) {
        if (!(*tries)++) LOG("[fs] %s %s: %s, another program may hold it; trying again for up to 30 s\n", op, path, strerror(e));
        plat_sleep_until_ns(plat_now_ns() + 50000000ull);
        errno = e;
        return true;
    }
#endif
    LOG("[fs] %s %s failed: %s%s\n", op, path, strerror(e),
        *tries ? " (still held by another program: OneDrive, an antivirus or a backup tool can lock the save folder)" : "");
    errno = e;
    return false;
}

// Flush (nn::fs::IFile::Flush: the game flushes each file of a save, five a save) puts the file on the disk. Its data is
// already in the system's cache (pwrite), and the flush to the disk (_commit, FlushFileBuffers on Windows) waited on
// the thread saving: 1-3 ms a file on an NVMe drive, far more on a SATA SSD or a hard disk, so "Saving..." hitched
// (PR #881). It goes to a thread of its own, which opens the file again by its path, so the game's handle is never
// held: the save still reaches the disk moments later. Queued twice before its flush starts: flushed once.
#define FLUSH_QUEUE 32
static char *flush_queue[FLUSH_QUEUE];
static int flush_n;
static pthread_mutex_t flush_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flush_cv = PTHREAD_COND_INITIALIZER;

static void *flusher(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&flush_lock);
        while (!flush_n) pthread_cond_wait(&flush_cv, &flush_lock);
        char *path = flush_queue[0];
        memmove(flush_queue, flush_queue + 1, sizeof *flush_queue * (size_t)--flush_n);
        pthread_mutex_unlock(&flush_lock);
        if (!plat_file_flush(path) && errno != ENOENT) LOG("[fs] flush %s failed: %s\n", path, strerror(errno));
        free(path);
    }
    return NULL;
}

// False when it can't be queued (no thread, or the queue is full): the caller flushes it itself
static bool flush_later(const char *path) {
    static int started;  // 0 not yet, 1 running, -1 couldn't start
    pthread_mutex_lock(&flush_lock);
    if (!started) {
        pthread_t t;
        started = pthread_create(&t, NULL, flusher, NULL) ? -1 : 1;
        if (started > 0) pthread_detach(t);
    }
    bool queued = false;
    for (int i = 0; i < flush_n && !queued; i++) queued = !strcmp(flush_queue[i], path);
    if (!queued && started > 0 && flush_n < FLUSH_QUEUE && (flush_queue[flush_n] = strdup(path))) flush_n++, queued = true;
    if (queued) pthread_cond_signal(&flush_cv);
    pthread_mutex_unlock(&flush_lock);
    return queued;
}

// The game closed the file's session: its host descriptor (a Windows handle each) and the copy of the save go.
static void file_close(Service *s) {
    HostFile *f = s->state;
    if (!f) return;
    s->state = NULL;
    if (f->fd >= 0) close(f->fd);
    if (f->save_written) save_written(f->path);
    free(f->patched);
    free(f);
}

static void dir_close(Service *s) {
    HostDir *d = s->state;
    if (!d) return;
    s->state = NULL;
    if (d->d) closedir(d->d);
    free(d);
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        kernel_watch_write("file read", b->addr, (uint64_t)size);
        #ifdef _WIN32
                if (plat_guest_sparse_heap_enabled()) plat_commit(b->addr, (uint64_t)size);
        #endif
        ssize_t n;
        if (f->patched) {  // (test warp)
            n = off >= 0 && (uint64_t)off < f->patched_size ? (ssize_t)((uint64_t)size < f->patched_size - (uint64_t)off ? (uint64_t)size : f->patched_size - (uint64_t)off) : 0;
            memcpy((void *)(uintptr_t)b->addr, f->patched + off, (size_t)n);
        } else {
            uint64_t ft0 = plat_now_ns();
            n = pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)size, off);
            perfcount_add(PC_FILE_READS, 1), perfcount_add(PC_FILE_NS, plat_now_ns() - ft0);
            if (n > 0) perfcount_add(PC_FILE_BYTES, (uint64_t)n);
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
        int tries = 0;
        ssize_t w;
        while ((w = pwrite(f->fd, (void *)(uintptr_t)a->addr, (size_t)size, off)) < 0 && fs_retry(&tries, "write", f->path)) {}
        if (w < 0) resp->result = errno_result();
        f->written = true;
        free(f->patched);  // (the game saved: its own data from now on)
        f->patched = NULL;
        break;
    }
    case 2:  // Flush
        if (f->written && !flush_later(f->path)) fsync(f->fd);  // (its result unread, as before)
        f->written = false;
        break;
    case 3: {
        int tries = 0, r;
        while ((r = ftruncate(f->fd, IN(int64_t, 0))) && fs_retry(&tries, "resize", f->path)) {}
        if (r) resp->result = errno_result();
        f->written = true;
        break;
    }
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
    int tries = 0, r;
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        int fd;
        while ((fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644)) < 0 && fs_retry(&tries, "create", p)) {}
        if (fd < 0) { resp->result = errno_result(); break; }
        while ((r = ftruncate(fd, IN(int64_t, 8))) && fs_retry(&tries, "resize", p)) {}
        if (r) resp->result = errno_result();
        close(fd);
        break;
    }
    case 1:
        while ((r = unlink(p)) && fs_retry(&tries, "delete", p)) {}
        if (r) resp->result = errno_result();
        break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3:
        while ((r = rmdir(p)) && fs_retry(&tries, "delete folder", p)) {}
        if (r) resp->result = errno_result();
        break;
    case 4:
        while ((r = rm_rf(p, false)) && fs_retry(&tries, "delete folder", p)) {}
        if (r) resp->result = errno_result();
        break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        while ((r = rename(p, p2)) && fs_retry(&tries, "rename", p)) {}
        if (r) resp->result = errno_result();
        else if (strlen(p2) >= 13 && !strcmp(p2 + strlen(p2) - 13, "game_data.sav")) ach_set_active(p2);  // (saved via a temp file)
        if (!r) save_written(p2);
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
        int fd;
        while ((fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY)) < 0 && fs_retry(&tries, "open", p)) {}
        if (fd < 0) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        strlcpy(f->path, p, sizeof f->path);
        size_t pl = strlen(p);
        f->save_written = (mode & 2) && ((pl >= 13 && !strcmp(p + pl - 13, "game_data.sav")) ||
                                         (pl >= 11 && !strcmp(p + pl - 11, "caption.sav")));
        if (pl >= 13 && !strcmp(p + pl - 13, "game_data.sav")) {  // the playthrough being played (achievements)
            ach_set_active(p);
            if (!(mode & 2)) ach_game_loaded(p);  // (opened to read: loaded; to write, it's being saved over)
            if (log_saves) LOG("[fs] save: open %s %s\n", mode & 2 ? "write" : "read", p);
        }
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
        Service *fsvc = service_new("IFile", file, f);
        fsvc->on_close = file_close;
        ipc_return_object(resp, fsvc);
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        Service *dsvc = service_new("IDirectory", directory, hd);
        dsvc->on_close = dir_close;
        ipc_return_object(resp, dsvc);
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
    g_save_dir = sp_session_save_dir();  // SAVAGEWIND_SAVE_DIR (test runs use a scratch copy), else the active save profile
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
