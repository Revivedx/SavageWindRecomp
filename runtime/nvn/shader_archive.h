// The game's NVN shader binaries, found in its own RomFS (extracted/romfs, read through runtime/romfs_image.h), for
// translating them ahead of time (runtime/nvn/vulkan/precompile.c). Where BOTW keeps them:
//  - shader archives (*.sbfsha: materials, terrain, foliage; *.bfsha inside *.ssarc): FSHA files holding nn::gfx
//    shader files (BNSH), each a list of variations (programs) with their stages
//  - agl / gsys / terrain shader archives (*.sharcb inside *.ssarc): post effects, shadows, lighting, system shaders
//  - effects (*.sesetlist, also inside packs and event packs): VFXB files holding BNSH files of particle shaders
//  - the layout shader (Layout/Common.sblarc: bgsh/__ArchiveShader.bnsh)
//  - and two BNSH files in the data of the game's executable (exefs/main, next to the image)
// Yaz0-compressed files and SARC archives (*.pack, *.ssarc, *.sblarc) are opened on the way. The same binary in several
// files (effects repeat theirs) is kept once.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t stage;              // STAGE_VERTEX, STAGE_FRAGMENT or STAGE_COMPUTE (maxwell/shader.h)
    uint32_t size;              // bytes of code as the game uploads it (graphics: 0x30-byte NVN header, SPH, code)
    uint32_t max_size;          // what the runtime passes as the code's size: the control section's constant
                                // offset, or 0x10000
    uint32_t local[3], shared;  // compute: group size and shared memory bytes (control section)
    uint64_t hash;              // of the code the translator reads (shader_code_hash)
    uint8_t *code;
    uint16_t source;            // first archive it was found in (SaSet.sources)
} SaShader;

typedef struct { int32_t vs, fs, cs; uint16_t source; } SaProgram;  // indices into SaSet.shaders, -1: none

typedef struct {
    SaShader *shaders;
    int nshaders, cap_shaders;
    SaProgram *programs;
    int nprograms, cap_programs;
    char **sources;
    int nsources;
    uint64_t files, bytes;       // RomFS files opened, bytes read
    void *index;                 // (scan state: shader hash table)
} SaSet;

// A hash of the bytes the translator reads of a stage's code (maxwell_binary_size; max_size as the runtime passes it):
// what names a shader binary in the shader key log (vulkan/shaders.c) and here. bin_size (may be NULL): that size.
uint64_t shader_code_hash(const uint8_t *code, size_t max_size, int stage, uint32_t *bin_size);

// Reads every shader archive of the RomFS folder romfs (extracted/romfs) into set, and the executable's shader files
// (exefs/main next to it, when there). progress (may be NULL): part done, 0..1.
// false: the image could not be read (why).
bool shader_archive_scan(const char *romfs, SaSet *set, void (*progress)(float done, void *user), void *user,
                         char *why, size_t why_size);
int shader_archive_find(const SaSet *set, uint64_t hash);  // index in set->shaders, -1: not there
void shader_archive_free(SaSet *set);

#ifdef __cplusplus
}
#endif
