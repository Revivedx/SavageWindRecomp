// RomFS image layout (romfs_build.h). Layout as the game's own image: a 0x50-byte header, file data from 0x200 (each
// file 16-byte aligned), then the directory hash table, directory entries, file hash table and file entries. The image
// itself is never written: runtime/romfs_image.c serves it from this layout and the files. Entries are found by hash
// (the game's RomFS code walks the tables itself), so the order inside them only
// has to be consistent: directories and files are sorted by name, children before siblings' children.
#include "romfs_build.h"

#include "platform.h"  // (Windows: POSIX mkdir)

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#define EMPTY 0xFFFFFFFFu

typedef struct Dir Dir;
typedef struct {
    char *name, *path;
    uint64_t size, data_off;
    uint32_t off;  // entry offset in the file table
    Dir *parent;
} File;
struct Dir {
    char *name, *path;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles;
    uint32_t off;  // entry offset in the directory table
};

static int by_name_dir(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_name_file(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

static Dir *scan(const char *path, const char *name, Dir *parent, int depth) {
    if (depth > 32) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    dir->name = strdup(name), dir->path = strdup(path), dir->parent = parent;
    int cd = 0, cf = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;  // (".", "..", and hidden files such as .DS_Store aren't the game's)
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            Dir *c = scan(p, e->d_name, dir, depth + 1);
            free(p);
            if (!c) continue;
            if (dir->ndirs == cd) dir->dirs = realloc(dir->dirs, sizeof *dir->dirs * (cd = cd ? cd * 2 : 8));
            dir->dirs[dir->ndirs++] = c;
        } else if (S_ISREG(st.st_mode)) {
            File *f = calloc(1, sizeof *f);
            f->name = strdup(e->d_name), f->path = p, f->size = (uint64_t)st.st_size, f->parent = dir;
            if (dir->nfiles == cf) dir->files = realloc(dir->files, sizeof *dir->files * (cf = cf ? cf * 2 : 16));
            dir->files[dir->nfiles++] = f;
        } else {
            free(p);
        }
    }
    closedir(d);
    if (dir->ndirs) qsort(dir->dirs, dir->ndirs, sizeof *dir->dirs, by_name_dir);
    if (dir->nfiles) qsort(dir->files, dir->nfiles, sizeof *dir->files, by_name_file);
    return dir;
}

static void free_tree(Dir *d) {
    if (!d) return;
    for (int i = 0; i < d->ndirs; i++) free_tree(d->dirs[i]);
    for (int i = 0; i < d->nfiles; i++) free(d->files[i]->name), free(d->files[i]->path), free(d->files[i]);
    free(d->dirs), free(d->files), free(d->name), free(d->path), free(d);
}

// flat lists, in table order
typedef struct { Dir **dirs; File **files; int nd, nf, cd, cf; } Lists;
static void flatten(Dir *d, Lists *l) {
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nf == l->cf) l->files = realloc(l->files, sizeof *l->files * (l->cf = l->cf ? l->cf * 2 : 256));
        l->files[l->nf++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (l->nd == l->cd) l->dirs = realloc(l->dirs, sizeof *l->dirs * (l->cd = l->cd ? l->cd * 2 : 64));
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i], l);
}

// the RomFS name hash and bucket count (as the game's RomFS code computes them)
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

uint64_t romfs_dir_bytes(const char *dir) {
    Dir *t = scan(dir, "", NULL, 0);
    if (!t) return 0;
    Lists l = {0};
    flatten(t, &l);
    uint64_t n = 0;
    for (int i = 0; i < l.nf; i++) n += l.files[i]->size;
    free(l.dirs), free(l.files);
    free_tree(t);
    return n;
}

bool romfs_layout(const char *dir, RomfsLayout *out, char *why, size_t why_size) {
    #define FAIL(...) do { if (why) snprintf(why, why_size, __VA_ARGS__); goto fail; } while (0)
    bool ok = false;
    uint8_t *dtab = NULL, *ftab = NULL;
    uint32_t *dbuckets = NULL, *fbuckets = NULL;
    Lists l = {0};
    memset(out, 0, sizeof *out);
    Dir *root = scan(dir, "", NULL, 0);
    if (!root) FAIL("can't read %s", dir);
    // table order: root, then each directory's subdirectories as flatten() lists them; files likewise
    if (l.nd == l.cd) l.dirs = realloc(l.dirs, sizeof *l.dirs * (l.cd = 64));
    l.dirs[l.nd++] = root;
    flatten(root, &l);
    if (!l.nf) FAIL("no files in %s", dir);

    // entry offsets
    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < l.nd; i++) l.dirs[i]->off = dsize, dsize += 0x18 + align4((uint32_t)strlen(l.dirs[i]->name));
    for (int i = 0; i < l.nf; i++) l.files[i]->off = fsize, fsize += 0x20 + align4((uint32_t)strlen(l.files[i]->name));
    // file data offsets (relative to the data start), each 16-byte aligned
    uint64_t data = 0;
    for (int i = 0; i < l.nf; i++) {
        data = align16(data);
        l.files[i]->data_off = data;
        data += l.files[i]->size;
    }

    uint32_t nbd = bucket_count((uint32_t)l.nd), nbf = bucket_count((uint32_t)l.nf);
    dbuckets = malloc(4 * nbd), fbuckets = malloc(4 * nbf);
    for (uint32_t i = 0; i < nbd; i++) dbuckets[i] = EMPTY;
    for (uint32_t i = 0; i < nbf; i++) fbuckets[i] = EMPTY;
    dtab = calloc(1, dsize), ftab = calloc(1, fsize);
    for (int i = 0; i < l.nd; i++) {
        Dir *d = l.dirs[i];
        uint8_t *e = dtab + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0, nl = (uint32_t)strlen(d->name);
        uint32_t sib = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sib = d->parent->dirs[k + 1]->off;
        uint32_t h = name_hash(parent, d->name) % nbd;
        put32(e, parent);
        put32(e + 4, sib);
        put32(e + 8, d->ndirs ? d->dirs[0]->off : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->off : EMPTY);
        put32(e + 16, dbuckets[h]);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        dbuckets[h] = d->off;
    }
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        uint8_t *e = ftab + f->off;
        uint32_t nl = (uint32_t)strlen(f->name), sib = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sib = f->parent->files[k + 1]->off;
        uint32_t h = name_hash(f->parent->off, f->name) % nbf;
        put32(e, f->parent->off);
        put32(e + 4, sib);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        put32(e + 24, fbuckets[h]);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        fbuckets[h] = f->off;
    }

    // header: tables after the file data (4-byte aligned)
    uint64_t dhash_off = (ROMFS_DATA_START + data + 3) & ~3ull;
    uint64_t dmeta_off = dhash_off + 4ull * nbd, fhash_off = dmeta_off + dsize, fmeta_off = fhash_off + 4ull * nbf;
    uint64_t hv[10] = {0x50, dhash_off, 4ull * nbd, dmeta_off, dsize, fhash_off, 4ull * nbf, fmeta_off, fsize,
                       ROMFS_DATA_START};
    memcpy(out->header, hv, sizeof hv);
    // the tables, one blob from dhash_off: dir hash, dir entries, file hash, file entries
    out->tables_off = dhash_off;
    out->tables_size = fmeta_off + fsize - dhash_off;
    out->tables = malloc(out->tables_size);
    memcpy(out->tables, dbuckets, 4ull * nbd);
    memcpy(out->tables + (dmeta_off - dhash_off), dtab, dsize);
    memcpy(out->tables + (fhash_off - dhash_off), fbuckets, 4ull * nbf);
    memcpy(out->tables + (fmeta_off - dhash_off), ftab, fsize);
    out->size = fmeta_off + fsize;
    out->files = malloc(sizeof *out->files * l.nf);
    out->nfiles = l.nf;
    for (int i = 0; i < l.nf; i++)  // (in data order: sorted by start)
        out->files[i] = (RomfsLayoutFile){ROMFS_DATA_START + l.files[i]->data_off, l.files[i]->size,
                                          strdup(l.files[i]->path)};
    ok = true;
fail:
    free(dtab), free(ftab), free(dbuckets), free(fbuckets), free(l.dirs), free(l.files);
    free_tree(root);
    return ok;
    #undef FAIL
}

void romfs_layout_free(RomfsLayout *l) {
    for (int i = 0; i < l->nfiles; i++) free(l->files[i].path);
    free(l->files), free(l->tables);
    memset(l, 0, sizeof *l);
}

// ------------------------------------------------------------------ copying the player's folder
static bool copy_file(const char *src, const char *dst, uint64_t size, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t)) {
    remove(dst);
#ifdef __APPLE__
    if (clonefile(src, dst, 0) == 0) goto linked;  // (APFS: a copy-on-write clone, instant)
#endif
#ifdef _WIN32
    if (CreateHardLinkA(dst, src, NULL)) goto linked;  // (same NTFS volume: a hard link)
#else
    if (link(src, dst) == 0) goto linked;  // (same volume: a hard link)
#endif
    {
        FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
        bool ok = in && out;
        static uint8_t buf[1 << 20];
        while (ok) {
            size_t n = fread(buf, 1, sizeof buf, in);
            if (!n) break;
            if (fwrite(buf, 1, n, out) != n) ok = false;
            *done += n;
            if (progress) progress(*done, total);
        }
        if (in) fclose(in);
        if (out && fclose(out) != 0) ok = false;
        return ok;
    }
linked:
    *done += size;
    if (progress) progress(*done, total);
    return true;
}

static bool copy_dir(Dir *d, const char *dst, uint64_t *done, uint64_t total, void (*progress)(uint64_t, uint64_t)) {
    mkdir(dst, 0755);
    for (int i = 0; i < d->nfiles; i++) {
        size_t n = strlen(dst) + strlen(d->files[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->files[i]->name);
        bool ok = copy_file(d->files[i]->path, p, d->files[i]->size, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    for (int i = 0; i < d->ndirs; i++) {
        size_t n = strlen(dst) + strlen(d->dirs[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->dirs[i]->name);
        bool ok = copy_dir(d->dirs[i], p, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    return true;
}

bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t)) {
    Dir *t = scan(src, "", NULL, 0);
    if (!t) return false;
    Lists l = {0};
    flatten(t, &l);
    uint64_t total = 0, done = 0;
    for (int i = 0; i < l.nf; i++) total += l.files[i]->size;
    bool ok = copy_dir(t, dst, &done, total, progress);
    free(l.dirs), free(l.files);
    free_tree(t);
    return ok;
}
