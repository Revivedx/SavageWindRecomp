// Per-object motion vectors (mvec.c): the scene draws of each frame, matched to the same draws of the previous
// frame, with copies of the inputs their vertex shaders read (the game's constant buffers, the instanced streams),
// kept for the frame after. See docs/motion-vectors-spike.md for why it works this way.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

#define MVEC_CB 14    // the game's constant buffers, hardware slots 3..16
#define MVEC_INST 4   // instanced streams copied per draw
#define MVEC_FEAT 48  // a draw's fingerprint: the first 12 floats of each of its first 4 constant buffers
// A motion program's inputs (maxwell/translate.c, ShaderKey mvec): the previous frame's history buffer at this binding
// of the vertex set, and push constants: uint pb[14] (where each constant buffer slot 3..16 starts, in 16-byte units)
// then uint pbn[14] (its size, in 16-byte units)
#define MVEC_HISTORY_BINDING 63
#define MVEC_PUSH 112

typedef struct { uint64_t addr, size; uint32_t stride, divisor; } MvecStream;

// A scene draw as draw_ sees it (exec.c): what identifies it and where its vertex shader's inputs are in guest memory
typedef struct {
    uint64_t frame;  // presented frames so far (renderer_present_count)
    uint64_t gen;    // guest copy generation (exec.c gcopy_gen): equal ranges of one generation hold equal bytes
    uint64_t vs, fs;  // shader code addresses
    uint64_t iaddr;   // index buffer (0: not indexed)
    uint32_t count, first, inst, base_inst;
    int32_t base_vertex;
    int nstream;
    MvecStream stream[16];
    uint32_t vs_streams;  // streams the vertex shader's attributes read
    int ncb;
    // rows: the vec4s the vertex shader reads of it (ShaderInfo cbuf_rows; NULL: any)
    struct { uint8_t slot; uint64_t addr, size; const uint64_t *rows; } cb[MVEC_CB];
} MvecDrawIn;

// One recorded draw: its identity, its match in the previous frame and its copied inputs (offsets in its frame's
// history buffer, mvec_records)
typedef struct {
    uint64_t key;
    uint64_t key2;  // the key without the index buffer and range (mvec.c prev_match_loose)
    uint32_t ord;   // among this frame's draws with the same key, in draw order
    int32_t prev;   // the matching record of the previous frame, -1: none
    float dist;     // its fingerprint's distance from that record's (mvec.c fingerprint_distance)
    uint8_t nfeat;
    float feat[MVEC_FEAT];  // the fingerprint: what tells apart draws of one key (the object's own constants)
    uint8_t ncb, ninst;
    uint8_t cb_slot[MVEC_CB];
    uint32_t cb_off[MVEC_CB], cb_size[MVEC_CB];
    uint8_t inst_stream[MVEC_INST];
    uint32_t inst_off[MVEC_INST], inst_size[MVEC_INST];
    uint64_t hash;  // of its inputs: the constants its vertex shader reads (but the camera block's matrices) and instanced streams
} MvecRec;

bool mvec_enabled(void);  // Video > Anti-aliasing TAA, or SAVAGEWIND_MVEC=1
// Records a scene draw of the current frame: its index among the frame's records, -1 when not recorded (off, or the
// history buffer is full: grown for the next frame).
int mvec_record(const MvecDrawIn *d);
// A frame's camera, from its scene camera block (the constant buffer at slot 3 that most scene draws share; the scene
// shaders make clip = proj * view * position): the view (rows 0-2, a rigid 3x4) and the projection (rows 7-10, an
// OpenGL-style 4x4, its near plane moving with the camera's distance), row-major as the shaders read them; vp = proj *
// view and its inverse. valid: found and checked against the world camera's projection (rt_world_projection).
typedef struct { float view[12], proj[16], vp[16], inv_vp[16]; bool valid; } MvecCamera;
// The camera of the previous frame (previous) or of the one before it, once its draws are all recorded
const MvecCamera *mvec_camera(bool previous);
// The camera reprojection of the frame being drawn, once its scene is (at the composite): m = the previous frame's
// view-projection * the inverse of this frame's (row-major), which takes this frame's clip position of a point that
// did not move to the previous frame's; jitter = this frame's jitter minus the previous frame's, in the game's NDC
// (from their projections' offsets: rt_persp_offset), what the vectors are cleaned of. false: no camera for either.
bool mvec_reprojection(float m[16], float jitter[2]);
// This frame's camera, for TAA (exec.c mvec_taa) and a temporal upscaler: its jitter (the projection's offset in the
// game's NDC, as mvec_reprojection's), near and far planes and vertical field of view (radians); false: none found yet
bool mvec_camera_now(float jitter[2], float *near_, float *far_, float *fov_y);
// SAVAGEWIND_MVEC_CAMONLY=1: (testing) camera vectors everywhere, the object vectors ignored; 2: where there is an
// object vector, the camera vector minus it (zero on still geometry when both are right)
int mvec_camera_only(void);

// The current and the previous frame's records and history buffers (NULL / 0 when there is no previous frame)
const MvecRec *mvec_records(bool previous, int *n, VkBuffer *history);
// The push constants of the current frame's record `rec` (mvec_record's index): its match's constant buffer copies in
// the previous frame's history. false: no match, or the match lacks a buffer this draw reads (no motion program).
bool mvec_push(int rec, uint32_t pc[MVEC_PUSH / 4]);
// Whether the current frame's record `rec` read the same inputs as its match (its hash): it stood still, and the
// camera's motion is its vector (exec.c draws it with the still variant, ShaderKey mvec_rt 0x40, not its motion
// program: most of the world stands still, and the motion programs, their vertex shaders run twice, cost ~5% fps)
bool mvec_still(int rec);
void mvec_drawn(void);
void mvec_masked(void);  // a blended effect was drawn with its reactive mask program (statistics)  // a recorded draw was drawn with its motion program (statistics)
// The previous frame's history buffer, for MVEC_HISTORY_BINDING (any valid buffer before there is one)
VkDescriptorBufferInfo mvec_history_info(void);
// SAVAGEWIND_MVEC_VIEW=<pixels>: the vectors drawn over the scene (exec.c mvec_scene_view), full colour at that length
// (default 16; negative: raw, red = written, green / blue = |x| / |y| NDC times its size); 0 when off
float mvec_view_enabled(void);
