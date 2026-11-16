// Saved teleport bookmarks (<data_root>/bookmarks.txt) for the Teleport overlay tab.
#pragma once
#include <stdbool.h>

#define BOOKMARK_NAME_MAX 96

typedef struct {
    char name[BOOKMARK_NAME_MAX];
    float x, y, z;
} Bookmark;

// Reloads bookmarks from disk on first use; returns the current number of bookmarks.
int bookmarks_count(void);
const Bookmark *bookmarks_at(int index);
// Adds a bookmark at (x, y, z) and saves the list; returns true on success.
bool bookmarks_add(const char *name, float x, float y, float z);
// Removes the bookmark at `index` and saves the list; returns true on success.
bool bookmarks_remove(int index);
