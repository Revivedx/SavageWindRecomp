// Every NVN shader in the game's RomFS folder (runtime/nvn/shader_scan.c), so they can be compiled before the game
// draws with them. --shader-scan lists them; SAVAGEWIND_SHADER_LOG=1 names the ones a session uses by the same id.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int stage;                // STAGE_VERTEX / STAGE_FRAGMENT / STAGE_COMPUTE (runtime/nvn/maxwell/shader.h)
    const uint8_t *code;      // what the program's GPU address points at: NVN's 0x30-byte header and the SPH
                              // (graphics), the instructions, then the constants at const_offset
    uint32_t size, const_offset;
    const uint8_t *control;   // the control section (shared size and group size of a compute shader)
    uint32_t control_size;
    uint64_t id;              // shader_code_id of code
} ScanShader;

typedef struct { int vs, fs; } ScanProgram;  // indexes into ScanResult.shaders: a pair some archive binds together

typedef struct {
    ScanShader *shaders;
    int nshaders;
    ScanProgram *programs;
    int nprograms;
    int files;                // files that held shaders
} ScanResult;

// Walks <romfs_dir>/{Pack,Effect,Event,System} (the only folders of BOTW's RomFS with shaders): Yaz0, SARC, the BNSH
// files in shader archives and effects, and the agl engine's .sharcb packages; then the BNSH files in the main
// module (main_nso: exefs/main, may be NULL). On a thread per core (1.5 s for 1.8.2's 14,004 shaders on 6 cores).
// false = nothing found (why: a short message).
bool shader_scan(const char *romfs_dir, const char *main_nso, ScanResult *out, char *why, size_t why_size);
void shader_scan_free(ScanResult *r);

// The id of a shader by the bytes the translator reads (maxwell_binary_size): the same for the copy the game binds
// and the one in its archive. max_size as the renderer translates it (the constants' offset, or more without them).
uint64_t shader_code_id(const uint8_t *code, uint32_t max_size, int stage);

#ifdef __cplusplus
}
#endif
