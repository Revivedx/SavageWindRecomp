// Checks runtime/romfs_image.c (the RomFS image the game reads, served from a folder) without the game's files: a
// folder of made-up files is laid out and read back through the image, and every file is found the way the game
// finds it (by its name's hash, in the image's own tables). Also: reads at random offsets against one whole read,
// reads from several threads at once over more files than the reader keeps open, and the romfs.bin fallback.
//   make build/test_romfs && build/test_romfs [seed]      (works in build/romfs_test, removed afterwards)
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"  // (Windows: POSIX mkdir)
#include "romfs_image.h"

#define ROOT "build/romfs_test"
#define EMPTY 0xFFFFFFFFu

static uint64_t g_rng;
static uint64_t rnd(void) {  // splitmix64
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static int g_fail;
#define CHECK(c, ...) \
    do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ------------------------------------------------------------------ the made-up folder
typedef struct { char rel[400]; uint8_t *data; size_t size; } TestFile;
static TestFile g_files[200];
static int g_nfiles;
static char g_made[300][420];  // everything created, to remove in reverse order
static int g_nmade;

static void make_dir(const char *rel) {
    snprintf(g_made[g_nmade], sizeof g_made[0], ROOT "/romfs%s%s", *rel ? "/" : "", rel);
    mkdir(g_made[g_nmade++], 0755);
}

static void make_file(const char *rel, size_t size, bool listed) {
    uint8_t *d = malloc(size ? size : 1);
    for (size_t i = 0; i < size; i++) d[i] = (uint8_t)rnd();
    snprintf(g_made[g_nmade], sizeof g_made[0], ROOT "/romfs/%s", rel);
    FILE *f = fopen(g_made[g_nmade++], "wb");
    if (!f || fwrite(d, 1, size, f) != size || fclose(f)) { fprintf(stderr, "can't write %s\n", rel); exit(2); }
    if (!listed) { free(d); return; }
    TestFile *t = &g_files[g_nfiles++];
    snprintf(t->rel, sizeof t->rel, "%s", rel);
    t->data = d, t->size = size;
}

static void make_tree(void) {
    mkdir("build", 0755);
    mkdir(ROOT, 0755);
    make_dir("");
    make_dir("Pack");
    make_dir("Sound");
    make_dir("Sound/Resource");
    make_dir("Sound/Resource/Stream");
    make_dir("Empty");
    make_file("Version.txt", 5, true);
    make_file("zero.bin", 0, true);
    make_file("one.bin", 1, true);
    make_file("Pack/odd.pack", 17, true);  // (the next file's data starts 16-byte aligned)
    make_file("Pack/big.pack", (3u << 20) + 7, true);
    make_file("Sound/Resource/Stream/deep.bfstm", 4096, true);
    char name[400];
    memset(name, 'n', 200);
    snprintf(name + 200, sizeof name - 200, ".long");
    make_file(name, 33, true);
    make_file(".DS_Store", 10, false);  // (hidden files aren't the game's: left out)
    for (int i = 0; i < 80; i++) {      // (more files than the reader keeps open)
        snprintf(name, sizeof name, "Sound/Resource/s%02d.bars", i);
        make_file(name, 100 + rnd() % 5000, true);
    }
}

static void remove_tree(void) {
    for (int i = g_nmade - 1; i >= 0; i--) remove(g_made[i]);
    remove(ROOT "/romfs.bin");
    remove(ROOT "/moved");
    rmdir(ROOT);
}

// ------------------------------------------------------------------ finding a file as the game does
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t name_hash(uint32_t parent, const char *name, size_t n) {
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < n; i++) h = ((h >> 5) | (h << 27)) ^ (uint8_t)name[i];
    return h;
}

// the entry of name under parent in a table (dir entries: 0x18 bytes before the name, files: 0x20), EMPTY if none
static uint32_t lookup(const uint8_t *img, const uint64_t *h, bool dir, uint32_t parent, const char *name, size_t n) {
    uint64_t hash_off = dir ? h[1] : h[5], hash_size = dir ? h[2] : h[6], meta = dir ? h[3] : h[7];
    uint32_t head = dir ? 0x18 : 0x20, next_at = dir ? 0x10 : 0x18;
    uint32_t e = rd32(img + hash_off + 4 * (name_hash(parent, name, n) % (hash_size / 4)));
    while (e != EMPTY) {
        const uint8_t *p = img + meta + e;
        if (rd32(p) == parent && rd32(p + head - 4) == n && !memcmp(p + head, name, n)) return e;
        e = rd32(p + next_at);
    }
    return EMPTY;
}

static void check_files(const uint8_t *img, uint64_t size) {
    uint64_t h[10];
    memcpy(h, img, sizeof h);
    CHECK(h[0] == 0x50 && h[9] == 0x200, "header");
    for (int i = 0; i < g_nfiles; i++) {
        const TestFile *t = &g_files[i];
        uint32_t dir = 0;
        const char *s = t->rel, *slash;
        while (dir != EMPTY && (slash = strchr(s, '/'))) {
            dir = lookup(img, h, true, dir, s, (size_t)(slash - s));
            s = slash + 1;
        }
        uint32_t f = dir == EMPTY ? EMPTY : lookup(img, h, false, dir, s, strlen(s));
        CHECK(f != EMPTY, "%s not found in the tables", t->rel);
        if (f == EMPTY) continue;
        const uint8_t *e = img + h[7] + f;
        uint64_t off = h[9] + rd64(e + 8), n = rd64(e + 16);
        CHECK(n == t->size, "%s: size %llu, want %zu", t->rel, (unsigned long long)n, t->size);
        CHECK(off % 16 == 0, "%s: data not 16-byte aligned", t->rel);
        CHECK(off + n <= size && !memcmp(img + off, t->data, n), "%s: data differs", t->rel);
    }
    CHECK(lookup(img, h, false, 0, ".DS_Store", 9) == EMPTY, ".DS_Store is in the image");
}

// ------------------------------------------------------------------ reads
static bool same_read(RomfsImage *img, const uint8_t *whole, uint64_t size, uint64_t off, uint64_t n, uint8_t *buf) {
    if (romfs_image_pread(img, buf, n, off) != (ssize_t)n) return false;
    for (uint64_t i = 0; i < n; i++)
        if (buf[i] != (off + i < size ? whole[off + i] : 0)) return false;
    return true;
}

static uint64_t random_off(uint64_t size) {
    switch (rnd() % 4) {
    case 0: return rnd() % 0x400;         // the header
    case 1: return size - rnd() % 0x2000;  // the tables, and past the end
    default: return rnd() % size;
    }
}

typedef struct { RomfsImage *img; const uint8_t *whole; uint64_t size, seed; int bad; } Reader;

static void *reader(void *arg) {
    Reader *r = arg;
    uint8_t *buf = malloc(1 << 16);
    uint64_t z = r->seed;
    for (int i = 0; i < 4000; i++) {
        z = z * 6364136223846793005ull + 1442695040888963407ull;  // (own generator: rnd() isn't thread-safe)
        uint64_t off = (z >> 11) % r->size, n = 1 + (z >> 40) % (1 << 16);
        if (!same_read(r->img, r->whole, r->size, off, n, buf)) r->bad++;
    }
    free(buf);
    return NULL;
}

int main(int argc, char **argv) {
    g_rng = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
    remove_tree();
    make_tree();
    char why[256] = "";
    RomfsImage *img = romfs_image_open(ROOT "/romfs", why, sizeof why);
    if (!img) { fprintf(stderr, "romfs_image_open: %s\n", why); remove_tree(); return 1; }
    uint64_t size = romfs_image_size(img);
    uint8_t *whole = malloc(size);
    CHECK(romfs_image_pread(img, whole, size, 0) == (ssize_t)size, "whole read");
    check_files(whole, size);

    uint8_t *buf = malloc(1 << 20);
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        uint64_t off = random_off(size), n = 1 + rnd() % (rnd() % 8 ? 4096 : 1 << 20);
        if (!same_read(img, whole, size, off, n, buf)) bad++;
    }
    CHECK(!bad, "%d of 20000 random reads differ", bad);

    Reader r[8];
    pthread_t t[8];
    for (int i = 0; i < 8; i++) {
        r[i] = (Reader){img, whole, size, rnd(), 0};
        pthread_create(&t[i], NULL, reader, &r[i]);
    }
    bad = 0;
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL), bad += r[i].bad;
    CHECK(!bad, "%d of 32000 reads from 8 threads differ", bad);
    romfs_image_close(img);

    // an older version's romfs.bin, with no folder there
    FILE *f = fopen(ROOT "/romfs.bin", "wb");
    CHECK(f && fwrite(whole, 1, size, f) == size && !fclose(f), "writing romfs.bin");
    CHECK(!rename(ROOT "/romfs", ROOT "/moved"), "moving the folder away");
    img = romfs_image_open(ROOT "/romfs", why, sizeof why);
    CHECK(img && romfs_image_size(img) == size, "romfs.bin fallback: %s", img ? "wrong size" : why);
    if (img) CHECK(same_read(img, whole, size, 0x200, 4096, buf), "romfs.bin fallback: data differs");
    romfs_image_close(img);
    rename(ROOT "/moved", ROOT "/romfs");

    free(buf), free(whole);
    for (int i = 0; i < g_nfiles; i++) free(g_files[i].data);
    remove_tree();
    printf("%s: %d files, %llu-byte image\n", g_fail ? "FAILED" : "ok", g_nfiles, (unsigned long long)size);
    return g_fail ? 1 : 0;
}
