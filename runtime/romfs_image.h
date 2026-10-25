// The game's RomFS image as one readable thing (runtime/romfs_image.c), without the image on disk: served from the
// romfs/ folder the setup copies (the header and tables laid out in memory by runtime/romfs_build.c, each file's data
// read from its own file, the gaps zeros), so the game data isn't kept twice. The game's reads (runtime/services/fs.c)
// and the shader precompilation (runtime/nvn/shader_archive.c) both go through it.
#pragma once
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RomfsImage RomfsImage;

// The image of the folder dir (extracted/romfs), or, with no folder there, dir.bin (the image file older versions
// built). NULL = neither could be read (why: a short message).
RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size);
uint64_t romfs_image_size(const RomfsImage *img);
// n bytes of the image at off, past its end zeros; any thread. n, or -1 when a file couldn't be read (logged).
ssize_t romfs_image_pread(RomfsImage *img, void *dst, uint64_t n, uint64_t off);
void romfs_image_close(RomfsImage *img);

#ifdef __cplusplus
}
#endif
