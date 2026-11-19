// Saved teleport bookmarks stored in <data_root>/bookmarks.txt as tab-separated lines.
#include "bookmarks.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BOOKMARKS 256

static Bookmark g_bookmarks[MAX_BOOKMARKS];
static int g_count;
static bool g_loaded;

static void bookmarks_path(char *buf, size_t n) {
    const char *root = rt_data_root();
    snprintf(buf, n, "%s/bookmarks.txt", root && *root ? root : ".");
}

static void clean_name(char *dst, size_t n, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < n; i++) {
        char c = src[i];
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        dst[j++] = c;
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
}

static void ensure_loaded(void) {
    if (g_loaded) return;
    g_loaded = true;
    g_count = 0;
    char path[1100];
    bookmarks_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (g_count < MAX_BOOKMARKS && fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = 0;
        float x = 0, y = 0, z = 0;
        if (sscanf(t1, "%f\t%f\t%f", &x, &y, &z) != 3) continue;
        Bookmark *b = &g_bookmarks[g_count++];
        clean_name(b->name, sizeof b->name, line);
        if (!b->name[0]) snprintf(b->name, sizeof b->name, "Bookmark %d", g_count);
        b->x = x; b->y = y; b->z = z;
    }
    fclose(f);
}

static bool save_all(void) {
    char path[1100];
    bookmarks_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < g_count; i++) {
        const Bookmark *b = &g_bookmarks[i];
        fprintf(f, "%s\t%.4f\t%.4f\t%.4f\n", b->name, b->x, b->y, b->z);
    }
    fclose(f);
    return true;
}

int bookmarks_count(void) {
    ensure_loaded();
    return g_count;
}

const Bookmark *bookmarks_at(int index) {
    ensure_loaded();
    if (index < 0 || index >= g_count) return NULL;
    return &g_bookmarks[index];
}

bool bookmarks_add(const char *name, float x, float y, float z) {
    ensure_loaded();
    if (g_count >= MAX_BOOKMARKS) return false;
    Bookmark *b = &g_bookmarks[g_count];
    clean_name(b->name, sizeof b->name, name);
    if (!b->name[0]) snprintf(b->name, sizeof b->name, "Bookmark %d", g_count + 1);
    b->x = x; b->y = y; b->z = z;
    g_count++;
    save_all();
    return true;
}

bool bookmarks_remove(int index) {
    ensure_loaded();
    if (index < 0 || index >= g_count) return false;
    if (index + 1 < g_count)
        memmove(&g_bookmarks[index], &g_bookmarks[index + 1], (size_t)(g_count - index - 1) * sizeof *g_bookmarks);
    g_count--;
    save_all();
    return true;
}
