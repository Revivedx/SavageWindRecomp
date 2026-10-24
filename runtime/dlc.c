#include "dlc.h"
#include "hle.h"
#include "romfs_build.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int32_t index; int fd; uint64_t size; } AddOn;
static AddOn addons[0x1000];
static uint32_t count;
static uint64_t base_id;

RT_ON_RESET(dlc_reset) {
    for (uint32_t i = 0; i < count; i++) close(addons[i].fd);
    count = 0;
    base_id = 0;
}

static int by_index(const void *a, const void *b) {
    return ((const AddOn *)a)->index - ((const AddOn *)b)->index;
}

// Reject truncated images before advertising them to the game.
static bool image_size(int fd, uint64_t *size) {
    struct stat st;
    uint64_t h[10];
    if (fstat(fd, &st) || st.st_size < sizeof h || pread(fd, h, sizeof h, 0) != sizeof h || h[0] != sizeof h)
        return false;
    *size = (uint64_t)st.st_size;
    if (h[9] > *size || h[4] < 0x18) return false;
    for (int i = 1; i < 9; i += 2)
        if (h[i] < sizeof h || h[i] > *size || h[i + 1] > *size - h[i]) return false;
    return true;
}

void dlc_init(const char *game_root, uint64_t program_id) {
    dlc_reset();
    base_id = (program_id & ~0xFFFull) + 0x1000;
    char root[1200];
    if (snprintf(root, sizeof root, "%s/dlc", game_root) >= sizeof root) return;
    DIR *dir = opendir(root);
    if (!dir) return;  // DLC is optional.
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        bool title = len == 16;
        if (!len || strspn(name, title ? "0123456789abcdefABCDEF" : "0123456789") != len) continue;
        char *end;
        uint64_t id = strtoull(name, &end, title ? 16 : 10);
        if (*end || (title && (id < base_id || id - base_id >= 0x1000)) || (!title && id >= 0x1000)) continue;
        int32_t index = (int32_t)(title ? id - base_id : id);
        if (dlc_present(index)) { LOG("[dlc] duplicate index %d ignored\n", index); continue; }
        char folder[1500], image[1600], source[1600];
        snprintf(folder, sizeof folder, "%s/%s", root, name);
        snprintf(image, sizeof image, "%s/romfs.bin", folder);
        snprintf(source, sizeof source, "%s/romfs", folder);
        struct stat st;
        // Build once. Remove romfs.bin to rebuild after changing extracted content.
        if (stat(image, &st) != 0 && stat(source, &st) == 0 && S_ISDIR(st.st_mode)) {
            char tmp[1650], why[256];
            snprintf(tmp, sizeof tmp, "%s.tmp", image);
            LOG("[dlc] building index %d from %s\n", index, source);
            if (!romfs_build(source, tmp, NULL, why, sizeof why)) {
                LOG("[dlc] index %d: %s\n", index, why);
                unlink(tmp);
                continue;
            }
            if (rename(tmp, image)) { unlink(tmp); continue; }
        }
        int fd = open(image, O_RDONLY);
        uint64_t size;
        if (fd < 0) continue;
        if (!image_size(fd, &size)) { LOG("[dlc] invalid RomFS: %s\n", image); close(fd); continue; }
        addons[count++] = (AddOn){index, fd, size};
        LOG("[dlc] installed %016llx (%llu bytes)\n", (unsigned long long)(base_id + index),
            (unsigned long long)size);
    }
    closedir(dir);
    qsort(addons, count, sizeof *addons, by_index);
}

uint64_t dlc_base_id(void) { return base_id; }
uint32_t dlc_count(void) { return count; }
bool dlc_present(int32_t index) {
    for (uint32_t i = 0; i < count; i++) if (addons[i].index == index) return true;
    return false;
}
uint32_t dlc_list(uint32_t offset, uint32_t capacity, int32_t *indices) {
    if (offset >= count) return 0;
    uint32_t n = count - offset;
    if (n > capacity) n = capacity;
    for (uint32_t i = 0; i < n; i++) indices[i] = addons[offset + i].index;
    return n;
}
bool dlc_storage(uint64_t title_id, int *fd, uint64_t *size) {
    if (title_id < base_id || title_id - base_id >= 0x1000) return false;
    for (uint32_t i = 0; i < count; i++) if ((uint64_t)addons[i].index == title_id - base_id) {
        *fd = addons[i].fd;
        *size = addons[i].size;
        return true;
    }
    return false;
}
