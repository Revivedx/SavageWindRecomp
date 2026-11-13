// Per-object motion vectors, step 1: draw identity and the previous frame's inputs.
//
// BOTW writes its constants into fresh memory every frame (three quarters of the constant buffers that move between
// frames carry the same bytes), so "the previous frame's data at this address" doesn't exist. Instead each scene draw
// (targets at the internal resolution, RS_SCENE) is identified by a key - its shaders, the non-instanced vertex streams
// its vertex shader reads, its index range - and an ordinal among the frame's draws with that key, and matched to the
// draw of the previous frame with the same key and ordinal (93-99% of them in the spike). The bytes its vertex shader
// reads that change between frames are copied, at draw time, into the frame's history buffer: the game's constant
// buffers (bones and per-object matrices included: no scene vertex shader reads global memory) and the instanced
// streams (rewritten every frame, in place). Per-vertex streams are not: the game never rewrites them between frames.
//
// History buffers (write-combined, often across the PCIe BAR: the CPU writes them and must never read them back, which
// took 17 us a draw): MVEC_SLOTS host-visible buffers, one per frame in flight and the frame being recorded, each
// holding one frame's copies; a frame's copies are read again during the next frame (as "previous"), so a slot is
// reused only once the GPU is done with the frames that read it. A frame that fills its slot stops recording (its
// remaining draws get no motion) and the slot is grown before its next use.
// On with Video > Anti-aliasing TAA (mvec_enabled; SAVAGEWIND_MVEC=1 / 0 whatever the setting).
// SAVAGEWIND_MVEC_LOG=<frames>: statistics every that many frames (default: none).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"
#include "../output_scale.h"
#include "mvec.h"

#define MVEC_SLOTS 5
#define HIST_START (2ull << 20)  // (the Great Plateau uses ~0.3 MB a frame; a fuller frame grows it, losing one frame's motion)
#define DEDUPE 4096  // copies of one (address, size, generation) shared within a frame
#define HCACHE 1024  // alternate per-shader hash slices of a shared constant buffer
#define CB_L1_SETS 1024  // 2-way set-associative cache (2048 entries) for hot shared (addr, rows, camera) CB hits
#define ROW_SIG_N 256    // L1 cache for immutable ShaderInfo pos_cbuf_rows signatures within an epoch
#define LOOSE_H 4096 // hash buckets for prev_match_loose non-instanced records
#define INDEX 32768  // (key, ordinal) -> record of the previous frame (order mode / stats)
#define STILL_CB 1024

typedef struct { uint64_t addr, size, gen, hash; const uint64_t *rows; uint32_t off, epoch; uint8_t camera; } CbL1;
typedef struct { const uint64_t *rows; uint64_t r[4], rsig; uint32_t epoch; uint8_t camera, cam_only; } RowSigEntry;
typedef struct { uint64_t addr, size, gen, hash, rsig; uint32_t off, epoch; } Dedupe;
typedef struct { uint64_t addr, size, gen, hash, rows[4]; uint32_t epoch; uint8_t has_rows, camera; } HashCache;
typedef struct { uint32_t cur_off, prev_off, cur_h, prev_h, epoch; uint8_t ok; } StillCbCache;
typedef struct { uint64_t key; int32_t lo, n; uint32_t epoch, ord; } KeyRange;  // candidates in pkeys + current-frame ordinal
typedef struct { uint64_t key; uint32_t ord, epoch; int32_t rec; } Index;  // (an entry of another epoch: empty)

typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    uint8_t *ptr, *stage;  // ptr: host-visible Vulkan buffer; stage: CPU-cached staging copy flushed once per frame
    uint64_t cap, used, grow, seq;  // seq: the last submission that may read it
    uint64_t frame;                 // the frame it holds (~0: none)
    MvecRec *rec;
    int nrec, cap_rec;
    bool full;
    MvecCamera cam;  // (camera_finish, once the frame is recorded)
} Slot;

static Slot slots[MVEC_SLOTS];
static int cur = -1, prev = -1;  // slots of the frame being recorded and of the one before it
static uint64_t cur_frame = ~0ull;
static CbL1 cb_l1[CB_L1_SETS * 2];
static RowSigEntry row_cache[ROW_SIG_N];
static Dedupe dedupe[DEDUPE];
static HashCache hcache[HCACHE];
static StillCbCache still_cb[STILL_CB];
static Index prev_index[INDEX];
// Bumped each frame: the tables' entries of other frames count as empty (clearing ~1 MB of tables took 0.1-0.2 ms a frame)
static uint32_t epoch, loose_epoch;
static bool loose_ready;
static int last_rec = -1;
static const uint64_t *last_cb_rows[MVEC_CB];
// The previous frame's records grouped by key (pidx: indices in draw order), and which are taken
static int32_t *pidx;
static uint32_t *uniq_h, *rec_h;
static bool *ptaken;
static int npk, cap_pk;
static int last_camc;
static uint64_t cb_align;
// O(1) bucket chains by key2 (prev_match_loose) for non-instanced previous records, and taken flags
static int32_t head2[LOOSE_H];
static uint32_t head2_ep[LOOSE_H];
static int32_t *next2;
static bool *rtaken;
// pkeys' ranges by key: a hash table of at least twice the previous frame's records (the two binary searches a draw took
// ~0.1 ms a frame)
static KeyRange *krange;
static uint32_t krange_mask, krange_used;

static struct {  // since the last log line
    uint64_t dist_hist[2][6];  // chosen matches' fingerprint distances: [key with one candidate / several][bucket]
    uint64_t not_ordinal;      // matches the draw order alone would have made differently
    // draws with the scene shaders' layout (camera block at slot 3, the object's 3x4 world matrix at slot 4) whose object
    // moved more than 1 cm since its match, matched by fingerprint and as draw order alone would have: mismatches look
    // like jumps (most of the world stands still)
    uint64_t moved_checked, moved_order, moved_fp;
    uint64_t masked;  // blended effects drawn into the reactive mask
    uint64_t loose;   // matched by shaders and vertex buffers (prev_match_loose)
    uint64_t still;   // matched draws whose inputs did not change (mvec_still)
    uint64_t frames, draws, matched, repeated, unmatched, dropped, cb_bytes, inst_bytes, copied, dedupe_hits, max_used, motion,
        cameras;    // frames with a valid camera (camera_finish)
} mstat;
// The frame's slot-3 copies by guest address (the camera block: the one most draws read, camera_finish)
#define CAMC 32
static struct { uint64_t addr; uint32_t off, size, n; } camc[CAMC];
static int ncamc;

static void mat4_mul(const float *a, const float *b, float *o) {  // row-major: o = a * b
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// Inverse of a 4x4 (row-major), by cofactors; false when singular
static bool mat4_inverse(const float *m, float *o) {
    float a[16];
    a[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    a[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    a[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    a[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    a[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    a[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    a[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    a[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    a[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    a[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    a[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    a[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    a[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    a[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    a[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    a[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = (double)m[0] * a[0] + (double)m[1] * a[4] + (double)m[2] * a[8] + (double)m[3] * a[12];
    if (fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; i++) o[i] = (float)(a[i] / det);
    return true;
}

// The same in double (mvec_reprojection)
static bool mat4_inverse_d(const double *m, double *o) {
    double a[16];
    a[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    a[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    a[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    a[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    a[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    a[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    a[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    a[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    a[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    a[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    a[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    a[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    a[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    a[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    a[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    a[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * a[0] + m[1] * a[4] + m[2] * a[8] + m[3] * a[12];
    if (fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; i++) o[i] = a[i] / det;
    return true;
}

// Whether a camera block's view and projection look like the world camera: a rigid view, and a perspective projection
// with the world camera's field of view and aspect (rt_world_projection; 2%: the field of view can ease between the
// frame that made the block and the last matrix) and (0, 0, -1, 0) as its last row. Its other entries are left free:
// the near plane moves, and an offset (the jitter) may be in [0][2] / [1][2].
static bool camera_plausible(const MvecCamera *c) {
    for (int r = 0; r < 3; r++) {
        const float *a = &c->view[r * 4];
        if (fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) > 0.01f) return false;
        for (int q = r + 1; q < 3; q++) {
            const float *b = &c->view[q * 4];
            if (fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.01f) return false;
        }
    }
    const float *p = c->proj;
    if (p[12] != 0 || p[13] != 0 || p[14] != -1 || p[15] != 0 || p[1] != 0 || p[4] != 0) return false;
    float w[4];
    rt_world_projection(w);
    if (w[0] <= 0 || w[1] <= 0) return false;
    return fabsf(p[5] * w[0] - 1) < 0.02f && fabsf(p[0] * w[0] * w[1] - 1) < 0.02f;
}

// A frame's camera, from its camera block's copy in the slot's history (the block most of the frame's draws so far
// read); false when there is none or it fails camera_plausible
static bool camera_compute(Slot *s, MvecCamera *c) {
    c->valid = false;
    int best = -1;
    for (int i = 0; i < ncamc; i++) if (best < 0 || camc[i].n > camc[best].n) best = i;
    if (best < 0 || camc[best].size < 11 * 16) return false;
    float rows[11 * 4];
    memcpy(rows, s->stage + camc[best].off, sizeof rows);
    memcpy(c->view, rows, 12 * 4);
    memcpy(c->proj, rows + 7 * 4, 16 * 4);
    float v4[16];
    memcpy(v4, c->view, 12 * 4);
    v4[12] = v4[13] = v4[14] = 0, v4[15] = 1;
    mat4_mul(c->proj, v4, c->vp);
    static bool warned;
    if (!camera_plausible(c) || !mat4_inverse(c->vp, c->inv_vp)) {
        if (!warned) LOG("[mvec] frame %llu: the scene camera block (%llx, %u draws) is not the world camera's view and "
                         "projection: no camera vectors\n", (unsigned long long)s->frame, (unsigned long long)camc[best].addr,
                         camc[best].n);
        warned = true;
        return false;
    }
    c->valid = true;
    return true;
}

// The finished frame's camera (when the next frame begins)
static void camera_finish(Slot *s) {
    if (!s->cam.valid && camera_compute(s, &s->cam)) mstat.cameras++;
    ncamc = 0;
    last_camc = 0;
}


uint64_t renderer_present_count(void);
// Whether frame generation interpolates with these vectors: only with SAVAGEWIND_FG_OBJECT_VECTORS=1. By default it
// takes the camera's and Link's box (exec.c fg_mv.frag): on grass, trees and other instanced foliage some draws match
// the wrong draw of the previous frame and get motion they don't have, and the generated frames broke them up (a
// flicker in the grass around Link while the camera turned, 2026-10-28 captures); making them also cost ~1.5-2 ms of
// CPU and ~0.6 ms of GPU a frame (RTX 3060, 1440p) with frame generation alone.
bool mvec_for_framegen(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FG_OBJECT_VECTORS") && atoi(getenv("SAVAGEWIND_FG_OBJECT_VECTORS")) != 0 &&
                     !getenv("SAVAGEWIND_FG_CAMERA_VECTORS");
    return on;
}

// On with Video > Anti-aliasing TAA or Upscaler FSR 3 (output_scale.h rt_temporal), or frame generation when it takes
// them (mvec_for_framegen), read once a presented frame so a frame is all on or all off;
// SAVAGEWIND_MVEC=1 / 0 turns it on or off whatever the setting.
bool mvec_enabled(void) {
    static int env = -2;
    if (env == -2) env = getenv("SAVAGEWIND_MVEC") ? atoi(getenv("SAVAGEWIND_MVEC")) != 0 : -1;
    if (env >= 0) return env;
    static uint64_t at = ~0ull;
    static bool on;
    uint64_t f = renderer_present_count();
    extern volatile int g_fg_want;  // (vulkan/exec.c: frame generation interpolates with them too)
    if (f != at) at = f, on = rt_temporal() || (g_fg_want && mvec_for_framegen());
    return on;
}

bool mvec_objects_enabled(void) {
    static int env = -2;
    if (env == -2) env = getenv("SAVAGEWIND_MVEC") ? atoi(getenv("SAVAGEWIND_MVEC")) != 0 : -1;
    if (env >= 0) return env != 0;
    return mvec_enabled() && g_settings.mvec_mode == MVEC_MODE_OBJECTS;
}

static void frame_begin(uint64_t frame);

void mvec_feed_camera(const float *cam_block) {
    if (!mvec_enabled() || !cam_block) return;
    uint64_t frame = renderer_present_count();
    if (cur < 0 || frame != cur_frame) frame_begin(frame);
    if (cur < 0) return;
    Slot *s = &slots[cur];
    if (s->cam.valid) return;
    memcpy(s->cam.view, cam_block, 12 * 4);
    memcpy(s->cam.proj, cam_block + 7 * 4, 16 * 4);
    float v4[16];
    memcpy(v4, s->cam.view, 12 * 4);
    v4[12] = v4[13] = v4[14] = 0, v4[15] = 1;
    mat4_mul(s->cam.proj, v4, s->cam.vp);
    if (camera_plausible(&s->cam) && mat4_inverse(s->cam.vp, s->cam.inv_vp)) {
        s->cam.valid = true;
        mstat.cameras++;
    }
}

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static uint32_t slot_hash(uint64_t key, uint32_t ord) { return (uint32_t)(mix(key, ord) >> 32) & (INDEX - 1); }

static void slot_alloc(Slot *s, uint64_t cap) {
    if (s->buf) {
        vk_wait_seq(s->seq);
        vmaDestroyBuffer(g_vk.vma, s->buf, s->mem);
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &s->buf, &s->mem, &info));
    s->ptr = info.pMappedData;
    free(s->stage);
    s->stage = malloc(cap);
    s->cap = cap;
}

// SAVAGEWIND_MVEC_LOG=<frames>: the statistics every that many frames (0, the default: none, and the costlier ones,
// the matching checks, are not gathered)
static int log_every(void) {
    static int every = -1;
    if (every < 0) every = getenv("SAVAGEWIND_MVEC_LOG") ? atoi(getenv("SAVAGEWIND_MVEC_LOG")) : 0;
    return every;
}
static void log_stats(uint64_t frame) {
    int every = log_every();
    if (every <= 0 || ++mstat.frames < (uint64_t)every) return;
    double f = (double)mstat.frames;
    LOG("[mvec] frame %llu: %.0f scene draws a frame, matched %.1f%% (%.0f with a repeated key, %.1f%% drawn with motion), "
        "unmatched %.0f, %.0f matched without their index buffer, %.0f still, "
        "not recorded %.0f; inputs %.0f KB constants + %.0f KB instance data a frame, %.0f KB copied (%.0f copies "
        "shared); history slot at most %.1f MB used of %.1f; camera found in %.0f%% of the frames; %.1f effects a frame in "
        "the reactive mask\n",
        (unsigned long long)frame, mstat.draws / f, mstat.draws ? 100.0 * mstat.matched / mstat.draws : 0.0, mstat.repeated / f,
        mstat.draws ? 100.0 * mstat.motion / mstat.draws : 0.0, mstat.unmatched / f, mstat.loose / f, mstat.still / f, mstat.dropped / f, mstat.cb_bytes / f / 1024, mstat.inst_bytes / f / 1024, mstat.copied / f / 1024,
        mstat.dedupe_hits / f, mstat.max_used / 1048576.0, cur >= 0 ? slots[cur].cap / 1048576.0 : 0.0,
        100.0 * mstat.cameras / f, mstat.masked / f);
    for (int g = 0; g < 2; g++)
        LOG("[mvec]   fingerprint distances, keys with %s: %.0f < 1e-4, %.0f < 1e-3, %.0f < 0.01, %.0f < 0.05, %.0f < 0.2, "
            "%.0f more%s", g ? "several candidates" : "one candidate", mstat.dist_hist[g][0] / f, mstat.dist_hist[g][1] / f,
            mstat.dist_hist[g][2] / f, mstat.dist_hist[g][3] / f, mstat.dist_hist[g][4] / f, mstat.dist_hist[g][5] / f,
            g ? "" : "\n");
    LOG("; %.0f a frame matched differently than by draw order\n", mstat.not_ordinal / f);
    LOG("[mvec]   of %.0f draws with a world matrix, moved > 1 cm: %.1f matched by fingerprint (%.1f by draw order)\n",
        mstat.moved_checked / f, mstat.moved_fp / f, mstat.moved_order / f);
    memset(&mstat, 0, sizeof mstat);
}

// SAVAGEWIND_MVEC_MATCH=order: (testing) match by draw order alone
static bool match_by_order(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_MVEC_MATCH") && !strcmp(getenv("SAVAGEWIND_MVEC_MATCH"), "order");
    return on;
}

// A new presented frame: the slot it records into, the previous frame's index
static void frame_begin(uint64_t frame) {
    last_rec = -1;
    loose_ready = false;
    if (cur >= 0) {
        if (slots[cur].ptr && slots[cur].used) memcpy(slots[cur].ptr, slots[cur].stage, slots[cur].used);
        camera_finish(&slots[cur]);
    }
    if (cur >= 0) {
        if (slots[cur].used > mstat.max_used) mstat.max_used = slots[cur].used;
        log_stats(frame);
    }
    int last = cur;
    cur = (int)(frame % MVEC_SLOTS);
    cur_frame = frame;
    Slot *s = &slots[cur];
    if (mvec_objects_enabled()) {
        vk_wait_seq(s->seq);  // (MVEC_SLOTS frames ago: long done in practice)
        if (!s->buf || s->grow > s->cap) slot_alloc(s, s->grow > HIST_START ? s->grow : HIST_START);
        if (!cb_align) {
            cb_align = g_vk.props.limits.minUniformBufferOffsetAlignment;
            if (cb_align < 16) cb_align = 16;
        }
    }
    s->used = 0;
    s->nrec = 0;
    s->full = false;
    s->frame = frame;
    s->cam.valid = false;
    prev = last >= 0 && slots[last].frame + 1 == frame ? last : -1;
    if (++epoch == 0) {  // (wrapped: entries 2^32 frames old would look current)
        memset(cb_l1, 0, sizeof cb_l1), memset(row_cache, 0, sizeof row_cache), memset(dedupe, 0, sizeof dedupe), memset(hcache, 0, sizeof hcache),
            memset(still_cb, 0, sizeof still_cb), memset(prev_index, 0, sizeof prev_index);
        if (krange) memset(krange, 0, (krange_mask + 1) * sizeof *krange);
        epoch = 1;
    }
    if (prev < 0) {
        if (!krange) {
            krange = calloc(4096, sizeof *krange);
            krange_mask = 4095;
        }
        krange_used = 0;
        return;
    }
    if (match_by_order() || log_every() > 0)  // (key, ordinal) -> record: draw-order matching and its statistics only
        for (int i = 0; i < slots[prev].nrec; i++) {
            const MvecRec *r = &slots[prev].rec[i];
            uint32_t h = slot_hash(r->key, r->ord);
            while (prev_index[h].epoch == epoch) h = (h + 1) & (INDEX - 1);
            prev_index[h] = (Index){r->key, r->ord, epoch, i};
        }
    npk = slots[prev].nrec;  // (by key: 2-pass direct bucket scatter into pidx preserving draw order)
    if (npk > cap_pk) {
        cap_pk = npk * 2;
        pidx = realloc(pidx, (size_t)cap_pk * sizeof *pidx);
        ptaken = realloc(ptaken, (size_t)cap_pk * sizeof *ptaken);
        next2 = realloc(next2, (size_t)cap_pk * sizeof *next2);
        rtaken = realloc(rtaken, (size_t)cap_pk * sizeof *rtaken);
        uniq_h = realloc(uniq_h, (size_t)cap_pk * sizeof *uniq_h);
        rec_h = realloc(rec_h, (size_t)cap_pk * sizeof *rec_h);
    }
    uint32_t want = 4096;
    while (want < 4u * (uint32_t)(npk + 512)) want *= 2;
    if (!krange || want > krange_mask + 1) {
        free(krange);
        krange = calloc(want, sizeof *krange);
        krange_mask = want - 1;
    }
    krange_used = 0;
    int nuniq = 0;
    const MvecRec *prec = slots[prev].rec;
    for (int i = 0; i < npk; i++) {
        uint64_t key = prec[i].key;
        uint32_t h = (uint32_t)(key >> 40) & krange_mask;
        while (krange[h].epoch == epoch && krange[h].key != key) h = (h + 1) & krange_mask;
        if (krange[h].epoch != epoch) {
            krange[h] = (KeyRange){key, 0, 1, epoch, 0};
            uniq_h[nuniq++] = h;
            krange_used++;
        } else {
            krange[h].n++;
        }
        rec_h[i] = h;
    }
    int32_t off = 0;
    for (int u = 0; u < nuniq; u++) {
        uint32_t h = uniq_h[u];
        krange[h].lo = off;
        off += krange[h].n;
        krange[h].n = 0;
    }
    for (int i = 0; i < npk; i++) {
        uint32_t h = rec_h[i];
        pidx[krange[h].lo + krange[h].n++] = i;
    }
    memset(ptaken, 0, (size_t)npk * sizeof *ptaken);
    memset(rtaken, 0, (size_t)npk * sizeof *rtaken);
}

// How far apart two fingerprints are: the mean relative difference of their values (0 equal, 1 unrelated); values
// that are no finite floats (integers, flags) count as equal or different by their bits
static float fingerprint_distance(const MvecRec *a, const MvecRec *b) {
    int n = a->nfeat < b->nfeat ? a->nfeat : b->nfeat;
    if (!n || a->nfeat != b->nfeat) return 1;
    if (!memcmp(a->feat, b->feat, (size_t)n * sizeof a->feat[0])) return 0;  // (a still object: most draws)
    float d = 0;
    for (int i = 0; i < n; i++) {
        float x = a->feat[i], y = b->feat[i];
        if (!isfinite(x) || !isfinite(y)) { d += memcmp(&x, &y, 4) ? 1.0f : 0.0f; continue; }
        float s = fabsf(x) + fabsf(y);
        d += s > 1e-6f ? fabsf(x - y) / s : 0.0f;
    }
    return d / (float)n;
}

// The previous frame's record for a draw: among the ones with its key and not taken yet, the nearest fingerprint.
// Culling shifts a key's draws by a few places, not hundreds: the search starts at the draw's own place in the key's
// order and looks MATCH_WINDOW places either way (a key can have hundreds of draws: comparing all of them took ~40 us a
// draw), and stops at an identical fingerprint (a still object, the most common case).
#define MATCH_WINDOW 24
static uint32_t krange_probe(uint64_t key, int32_t *lo, int32_t *end) {
    if (!krange) { *lo = *end = 0; return 0; }
    uint32_t h = (uint32_t)(key >> 40) & krange_mask;
    for (uint32_t step = 0; step <= krange_mask; step++, h = (h + 1) & krange_mask) {
        if (krange[h].epoch != epoch) {
            *lo = *end = 0;
            if (krange_used < (krange_mask + 1) * 3 / 4) {
                krange[h] = (KeyRange){key, 0, 0, epoch, 1};
                krange_used++;
            }
            return 0;
        }
        if (krange[h].key == key) {
            *lo = krange[h].lo;
            *end = krange[h].lo + krange[h].n;
            return krange[h].ord++;
        }
    }
    *lo = *end = 0;
    return 0;
}

static int32_t prev_match(MvecRec *r, int lo, int end) {
    if (prev < 0 || !npk || lo >= end) return -1;
    static float limit = -1;  // SAVAGEWIND_MVEC_MATCH_LIMIT: the largest fingerprint distance of a match (default 0.2)
    if (limit < 0) limit = getenv("SAVAGEWIND_MVEC_MATCH_LIMIT") ? (float)atof(getenv("SAVAGEWIND_MVEC_MATCH_LIMIT")) : 0.2f;
    int ncand = end - lo, at = lo + (int)r->ord;
    if (at >= end) at = end - 1;
    if (!ptaken[at] && limit > 0.0f) {
        int32_t pi = pidx[at];
        const MvecRec *q = &slots[prev].rec[pi];
        if (!rtaken[pi] && q->ninst == r->ninst && q->nfeat && r->nfeat == q->nfeat &&
            !memcmp(r->feat, q->feat, (size_t)r->nfeat * sizeof r->feat[0])) {
            ptaken[at] = true;
            rtaken[pi] = true;
            r->dist = 0;
            if (log_every() > 0) mstat.dist_hist[ncand > 1][0]++;
            return pi;
        }
    }
    int best = -1;
    float bd = 2;
    for (int k = 0; k <= 2 * MATCH_WINDOW && bd > 1e-6f; k++) {  // (at, at + 1, at - 1, at + 2, ...)
        int i = at + (k & 1 ? (k + 1) / 2 : -(k / 2));
        if (i < lo || i >= end || ptaken[i] || rtaken[pidx[i]]) continue;
        if (slots[prev].rec[pidx[i]].ninst != r->ninst) continue;
        float d = fingerprint_distance(r, &slots[prev].rec[pidx[i]]);
        if (d < bd) bd = d, best = i;
    }
    if (best < 0) return -1;
    float max_d = r->ninst ? fminf(limit, 0.05f) : limit;
    if (bd >= max_d) { mstat.dist_hist[ncand > 1][5]++; return -1; }  // (a new object: the camera's vector)
    ptaken[best] = true;
    rtaken[pidx[best]] = true;
    r->dist = bd;
    if (log_every() > 0) {
        static const float edges[5] = {1e-4f, 1e-3f, 0.01f, 0.05f, 0.2f};
        int bucket = 0;
        while (bucket < 5 && bd >= edges[bucket]) bucket++;
        mstat.dist_hist[ncand > 1][bucket]++;
    }
    return pidx[best];
}

// A draw whose key the previous frame did not have: one of the previous frame's draws with its shaders and vertex
// buffers and an equal fingerprint (under 0.01). BOTW draws its distant trees from one vertex pool through index
// lists it writes anew each frame, at other addresses and lengths: matched by key, they never were, and took the
// camera's motion alone, not their swaying (temporal anti-aliasing smeared them, a shimmer on every distant tree).
// Their vertices hold where they are; the motion is in the constants, which any of them gives the same.
static int32_t prev_match_loose(MvecRec *r) {
    if (prev < 0 || !npk) return -1;
    if (!loose_ready) {
        loose_ready = true;
        if (++loose_epoch == 0) {
            memset(head2_ep, 0, sizeof head2_ep);
            loose_epoch = 1;
        }
        const MvecRec *prec = slots[prev].rec;
        for (int i = npk - 1; i >= 0; i--) {
            if (prec[i].ninst) continue;
            uint32_t h = (uint32_t)(prec[i].key2 >> 40) & (LOOSE_H - 1);
            next2[i] = (head2_ep[h] == loose_epoch) ? head2[h] : -1;
            head2[h] = i;
            head2_ep[h] = loose_epoch;
        }
    }
    uint32_t h = (uint32_t)(r->key2 >> 40) & (LOOSE_H - 1);
    if (head2_ep[h] != loose_epoch) return -1;
    int best = -1;
    float bd = 0.01f;
    const MvecRec *prec = slots[prev].rec;
    for (int i = head2[h], n = 0; i >= 0 && n < 64 && bd > 1e-6f; i = next2[i]) {
        if (prec[i].key2 != r->key2) continue;
        n++;
        if (rtaken[i]) continue;
        float d = fingerprint_distance(r, &prec[i]);
        if (d < bd) bd = d, best = i;
    }
    if (best < 0) return -1;
    rtaken[best] = true;
    r->dist = bd;
    mstat.loose++;
    return best;
}

static int32_t prev_lookup(uint64_t key, uint32_t ord) {
    if (prev < 0) return -1;
    for (uint32_t h = slot_hash(key, ord);; h = (h + 1) & (INDEX - 1)) {
        if (prev_index[h].epoch != epoch) return -1;
        if (prev_index[h].key == key && prev_index[h].ord == ord) return prev_index[h].rec;
    }
}

// A hash of n bytes (mvec_still: equal inputs)
static uint64_t content_hash(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9e3779b97f4a7c15ull, v;
    for (; n >= 8; p += 8, n -= 8) memcpy(&v, p, 8), h = (h ^ v) * 0xff51afd7ed558ccdull, h ^= h >> 32;
    if (n) v = 0, memcpy(&v, p, n), h = (h ^ v) * 0xff51afd7ed558ccdull, h ^= h >> 32;
    return h;
}

// The hash of a constant buffer's vec4s that a vertex shader reads (rows: ShaderInfo cbuf_rows; NULL: all of them).
// The camera block's (camera: slot 3) matrices, vec4s 0-13, and near and far planes (vec4 14's .xy) change with the
// camera, which the fallback's vectors follow: left out. Its other values (time, wind) move things.
static uint64_t cb_hash(const uint8_t *p, uint64_t size, const uint64_t *rows, bool camera) {
    if (!rows) return camera ? (size > 58 * 4 ? content_hash(p + 58 * 4, size - 58 * 4) : 0) : content_hash(p, size);
    uint64_t h = 0, n = size / 16, a, b;
    for (uint32_t w = 0; w < 4 && w * 64 < n; w++) {
        uint64_t m = (w == 0 && camera) ? (rows[0] & ~((1ULL << 14) - 1)) : rows[w];
        for (; m; m &= m - 1) {
            uint32_t r = w * 64 + (uint32_t)__builtin_ctzll(m);
            if (r >= n) break;
            memcpy(&a, p + r * 16, 8), memcpy(&b, p + r * 16 + 8, 8);
            if (camera && r == 14) a = 0;
            h = mix(mix(h, a ^ r), b);
        }
    }
    return h;
}

static uint64_t rows_sig(const uint64_t *rows, bool camera, uint64_t r[4], bool *cam_only) {
    if (!rows) {
        r[0] = r[1] = r[2] = r[3] = 0;
        *cam_only = false;
        return (uint64_t)camera;
    }
    uint32_t idx = (uint32_t)((((uintptr_t)rows ^ (uint64_t)camera) * 0x9e3779b97f4a7c15ull) >> 56) & (ROW_SIG_N - 1);
    RowSigEntry *rc = &row_cache[idx];
    if (rc->epoch == epoch && rc->rows == rows && rc->camera == (uint8_t)camera) {
        memcpy(r, rc->r, 32);
        *cam_only = rc->cam_only != 0;
        return rc->rsig;
    }
    memcpy(r, rows, 32);
    *cam_only = camera && r[0] < (1ULL << 14) && !(r[1] | r[2] | r[3]);
    uint64_t rsig = *cam_only ? 0x10000ull : (mix(mix(r[0], r[1]), mix(r[2], r[3])) ^ 2ull ^ (uint64_t)camera);
    rc->rows = rows;
    memcpy(rc->r, r, 32);
    rc->rsig = rsig;
    rc->epoch = epoch;
    rc->camera = (uint8_t)camera;
    rc->cam_only = (uint8_t)*cam_only;
    return rsig;
}

static uint64_t cached_cb_hash(uint64_t addr, uint64_t size, uint64_t gen, const uint64_t *rows, bool camera, uint64_t rsig, const uint64_t r[4]) {
    uint32_t h = (uint32_t)(mix(mix(addr, size), mix(gen, rsig)) >> 40) & (HCACHE - 1);
    HashCache *e = &hcache[h];
    if (e->epoch == epoch && e->addr == addr && e->size == size && e->gen == gen &&
        e->has_rows == (rows != NULL) && e->camera == (uint8_t)camera &&
        (!rows || (e->rows[0] == r[0] && e->rows[1] == r[1] && e->rows[2] == r[2] && e->rows[3] == r[3])))
        return e->hash;
    uint64_t ch = cb_hash((const uint8_t *)(uintptr_t)addr, size, rows, camera);
    *e = (HashCache){addr, size, gen, ch, {r[0], r[1], r[2], r[3]}, epoch, rows != NULL, (uint8_t)camera};
    return ch;
}

static inline void cb_l1_insert(uint32_t set, uint64_t addr, uint64_t size, uint64_t gen, uint64_t hash, const uint64_t *rows, uint32_t off, uint8_t camera) {
    CbL1 *w = &cb_l1[set * 2];
    w[1] = w[0];
    w[0] = (CbL1){addr, size, gen, hash, rows, off, epoch, camera};
}

static __attribute__((noinline)) uint32_t copy_in_miss(uint64_t addr, uint64_t size, uint64_t gen, bool share, const uint64_t *rows, bool camera, uint64_t *hash, uint32_t set) {
    Slot *s = &slots[cur];
    uint64_t r[4], rsig = 0;
    bool cam_only = false;
    if (share) rsig = rows_sig(rows, camera, r, &cam_only);
    uint32_t h = (uint32_t)(mix(mix(addr, size), gen) >> 40) & (DEDUPE - 1);
    if (share && dedupe[h].epoch == epoch && dedupe[h].addr == addr && dedupe[h].size == size && dedupe[h].gen == gen) {
        mstat.dedupe_hits++;
        *hash = cam_only ? 0 : (dedupe[h].rsig == rsig ? dedupe[h].hash : cached_cb_hash(addr, size, gen, rows, camera, rsig, r));
        cb_l1_insert(set, addr, size, gen, *hash, rows, dedupe[h].off, (uint8_t)camera);
        return dedupe[h].off;
    }
    uint64_t align = cb_align ? cb_align : 16;
    uint64_t off = (s->used + align - 1) & ~(align - 1);
    if (off + size > s->cap) {
        s->full = true;
        uint64_t want = (off + size) * 2;
        if (want > s->grow) s->grow = want;
        return UINT32_MAX;
    }
    memcpy(s->stage + off, (const void *)(uintptr_t)addr, size);
    *hash = cam_only ? 0 : cb_hash(s->stage + off, size, rows, camera);
    mstat.copied += size;
    s->used = off + size;
    if (share) {
        dedupe[h] = (Dedupe){addr, size, gen, *hash, rsig, (uint32_t)off, epoch};
        cb_l1_insert(set, addr, size, gen, *hash, rows, (uint32_t)off, (uint8_t)camera);
    }
    return (uint32_t)off;
}

// Copies [addr, addr + size) of guest memory into the current slot (shared with an earlier copy of the same range in
// the same generation); UINT32_MAX when the slot is full. *hash: cb_hash's.
static inline uint32_t copy_in(uint64_t addr, uint64_t size, uint64_t gen, bool share, const uint64_t *rows, bool camera, uint64_t *hash) {
    uint32_t set = 0;
    if (share) {
        set = (uint32_t)((((addr >> 4) ^ (addr >> 15) ^ (uintptr_t)rows ^ (uint64_t)camera) * 0x9e3779b97f4a7c15ull) >> 54) & (CB_L1_SETS - 1);
        const CbL1 *w = &cb_l1[set * 2];
        for (int i = 0; i < 2; i++) {
            if (w[i].epoch == epoch && w[i].addr == addr && w[i].size == size && w[i].gen == gen &&
                w[i].rows == rows && w[i].camera == (uint8_t)camera) {
                mstat.dedupe_hits++;
                *hash = w[i].hash;
                return w[i].off;
            }
        }
    }
    return copy_in_miss(addr, size, gen, share, rows, camera, hash, set);
}

int mvec_record(const MvecDrawIn *d) {
    if (!mvec_objects_enabled()) return -1;
    if (d->frame != cur_frame) frame_begin(d->frame);
    Slot *s = &slots[cur];
    mstat.draws++;
    if (s->full) { mstat.dropped++; return -1; }
    if (s->nrec == s->cap_rec) {
        s->cap_rec = s->cap_rec ? s->cap_rec * 2 : 4096;
        s->rec = realloc(s->rec, (size_t)s->cap_rec * sizeof *s->rec);
    }
    MvecRec *r = &s->rec[s->nrec];
    r->dist = 0;
    r->nfeat = 0;
    r->ncb = 0;
    r->ninst = 0;
    r->hash = 0;
    uint64_t key = mix(mix(mix(d->vs, d->fs), d->iaddr), ((uint64_t)d->count << 32) | d->first);
    key = mix(key, (uint32_t)d->base_vertex);
    uint64_t key2 = mix(mix(d->vs, d->fs), ~0ull);
    uint64_t used_before = s->used;
    for (int i = 0; i < d->nstream && i < 16; i++) {
        const MvecStream *m = &d->stream[i];
        if (!((d->vs_streams >> i) & 1) || !m->addr) continue;
        if (!m->divisor) { key = mix(key, m->addr), key2 = mix(key2, m->addr); continue; }
        if (r->ninst == MVEC_INST) continue;
        uint64_t n = (d->base_inst + d->inst + m->divisor - 1) / m->divisor, size = n * m->stride;
        if (size > m->size && m->size) size = m->size;
        if (!size || size > (8u << 20)) continue;
        uint64_t ch;
        uint32_t off = copy_in(m->addr, size, d->gen, false, NULL, false, &ch);  // (rewritten in place: never shared across draws)
        if (off == UINT32_MAX) goto full;
        r->hash = mix(r->hash, ch);
        r->inst_stream[r->ninst] = (uint8_t)i;
        r->inst_off[r->ninst] = off;
        r->inst_size[r->ninst++] = (uint32_t)size;
        mstat.inst_bytes += size;
    }
    int32_t lo = 0, end = 0;
    r->key = key;
    r->key2 = key2;
    r->ord = krange_probe(key, &lo, &end);
    int at = lo + (int)r->ord;
    if (at >= end) at = end - 1;
    if (lo < end) __builtin_prefetch(&pidx[at], 0, 1);
    for (int k = 0; k < d->ncb && r->ncb < MVEC_CB; k++) {
        if (!d->cb[k].addr || !d->cb[k].size) continue;
        uint64_t ch;
        uint32_t off = copy_in(d->cb[k].addr, d->cb[k].size, d->gen, true, d->cb[k].rows, d->cb[k].slot == 3, &ch);
        if (off == UINT32_MAX) goto full;
        r->hash = mix(r->hash, mix(d->cb[k].slot, ch));
        if (d->cb[k].slot == 3) {  // (camera block candidates: camera_finish)
            if (ncamc && camc[last_camc].addr == d->cb[k].addr) {
                camc[last_camc].n++;
            } else {
                int i = 0;
                while (i < ncamc && camc[i].addr != d->cb[k].addr) i++;
                if (i == ncamc && ncamc < CAMC) camc[ncamc++] = (typeof(camc[0])){d->cb[k].addr, off, (uint32_t)d->cb[k].size, 0};
                if (i < ncamc) camc[i].n++, last_camc = i;
            }
        } else if (d->cb[k].size >= 48 && r->nfeat + 12 <= MVEC_FEAT) {
            memcpy(&r->feat[r->nfeat], s->stage + off, 48);
            r->nfeat += 12;
        }
        last_cb_rows[r->ncb] = d->cb[k].rows;
        r->cb_slot[r->ncb] = d->cb[k].slot;
        r->cb_off[r->ncb] = off;
        r->cb_hash[r->ncb] = (uint32_t)(ch ^ (ch >> 32));
        r->cb_size[r->ncb++] = (uint32_t)d->cb[k].size;
        mstat.cb_bytes += d->cb[k].size;
    }
    if (lo < end && prev >= 0 && at >= 0) __builtin_prefetch(&slots[prev].rec[pidx[at]], 0, 1);
    if (r->ninst > 0 && r->nfeat + 4 <= MVEC_FEAT) {
        const MvecStream *m = &d->stream[r->inst_stream[0]];
        uint64_t ioff = m->divisor ? (uint64_t)(d->base_inst / m->divisor) * m->stride : 0;
        if (m->addr && ioff + 16 <= r->inst_size[0]) {
            uint64_t avail = r->inst_size[0] - ioff;
            int nf = (int)(avail / 4);
            if (nf > 12) nf = 12;
            if (r->nfeat + nf > MVEC_FEAT) nf = MVEC_FEAT - r->nfeat;
            memcpy(&r->feat[r->nfeat], (const uint8_t *)(uintptr_t)m->addr + ioff, (size_t)nf * 4);
            r->nfeat += (uint8_t)nf;
        }
    }
    bool by_order = match_by_order();
    r->prev = by_order ? prev_lookup(key, r->ord) : prev_match(r, lo, end);
    static int loose = -1;  // SAVAGEWIND_MVEC_LOOSE=0: (testing) no prev_match_loose
    if (loose < 0) loose = !getenv("SAVAGEWIND_MVEC_LOOSE") || atoi(getenv("SAVAGEWIND_MVEC_LOOSE")) != 0;
    if (r->prev < 0 && !by_order && loose && !r->ninst) r->prev = prev_match_loose(r);
    if (log_every() > 0) {  // (statistics) the object's world position (slot 4's translation) against each matching's previous draw
        int32_t po = prev_lookup(key, r->ord), pf = by_order ? -1 : r->prev;
        if (r->nfeat >= 12 && r->cb_slot[0] == 3 && r->cb_slot[1] == 4 && po >= 0 && pf >= 0) {
            mstat.moved_checked++;
            const float *t = &r->feat[0];
            for (int m = 0; m < 2; m++) {
                const MvecRec *q = &slots[prev].rec[m ? pf : po];
                if (q->nfeat < 12) continue;
                float dx = t[3] - q->feat[3], dy = t[7] - q->feat[7], dz = t[11] - q->feat[11];
                if (dx * dx + dy * dy + dz * dz > 1e-4f) { if (m) mstat.moved_fp++; else mstat.moved_order++; }
            }
        }
        if (r->prev >= 0 && r->prev != po) mstat.not_ordinal++;
    }
    if (r->ord) mstat.repeated++;
    if (r->prev >= 0) mstat.matched++;
    else mstat.unmatched++;
    s->seq = vk_pending_seq();
    last_rec = s->nrec;
    return s->nrec++;
full:
    last_rec = -1;
    s->used = used_before;
    mstat.dropped++;
    return -1;
}

void mvec_drawn(void) { mstat.motion++; }

// Shared environment UBOs (512..2047 B, not bone palettes and not slot 3 camera) often have a single slowly ticking
// global row >= 16 (e.g. time/wind accumulator drifting < 0.002/frame) referenced statically in vertex shaders while
// vertices move < sub-pixel; skip redundant double-VS on such draws while keeping full motion on actor/world matrices,
// bone palettes (>= 2048 B), instanced streams, and active wind/material rows.
static bool still_cb_ok(const MvecRec *r, const MvecRec *p, int k) {
    if (r->cb_slot[k] != p->cb_slot[k] || r->cb_size[k] != p->cb_size[k]) return false;
    if (r->cb_slot[k] == 3 || r->cb_size[k] < 512 || r->cb_size[k] >= 2048) return false;
    if (!slots[cur].stage || !slots[prev].stage) return false;
    uint32_t co = r->cb_off[k], po = p->cb_off[k], ch = r->cb_hash[k], ph = p->cb_hash[k];
    uint32_t h = (uint32_t)(mix(mix(co, po), mix(ch, ph)) >> 40) & (STILL_CB - 1);
    StillCbCache *e = &still_cb[h];
    if (e->epoch == epoch && e->cur_off == co && e->prev_off == po && e->cur_h == ch && e->prev_h == ph)
        return e->ok != 0;
    const uint8_t *cb = slots[cur].stage + co;
    const uint8_t *pb = slots[prev].stage + po;
    const uint64_t *rows = last_cb_rows[k];
    uint32_t nrow = r->cb_size[k] / 16;
    int ndiff = 0;
    bool ok = true;
    for (uint32_t w = 0; w < 4 && ok; w++) {
        uint64_t m = rows ? rows[w] : ~0ull;
        for (; m; m &= m - 1) {
            uint32_t row = w * 64 + (uint32_t)__builtin_ctzll(m);
            if (row >= nrow) break;
            if (memcmp(cb + row * 16, pb + row * 16, 16) == 0) continue;
            if (row < 16 || ++ndiff > 1) { ok = false; break; }
            float cf[4], pf[4];
            memcpy(cf, cb + row * 16, 16);
            memcpy(pf, pb + row * 16, 16);
            for (int j = 0; j < 4; j++) {
                if (!isfinite(cf[j]) || !isfinite(pf[j]) || fabsf(cf[j] - pf[j]) >= 0.002f) {
                    ok = false; break;
                }
            }
        }
    }
    *e = (StillCbCache){co, po, ch, ph, epoch, (uint8_t)ok};
    return ok;
}

bool mvec_still(int rec) {
    static int on = -1;  // SAVAGEWIND_MVEC_STILL=0: (testing) every matched draw with its motion program
    if (on < 0) on = !getenv("SAVAGEWIND_MVEC_STILL") || atoi(getenv("SAVAGEWIND_MVEC_STILL")) != 0;
    if (!on || cur < 0 || prev < 0 || rec < 0 || rec >= slots[cur].nrec) return false;
    const MvecRec *r = &slots[cur].rec[rec];
    if (r->prev < 0) return false;
    const MvecRec *p = &slots[prev].rec[r->prev];
    if (p->hash != r->hash) {
        if (rec != last_rec || r->ninst || p->ninst || r->ncb != p->ncb) return false;
        for (int k = 0; k < r->ncb; k++)
            if (r->cb_hash[k] != p->cb_hash[k] && !still_cb_ok(r, p, k)) return false;
    }
    mstat.still++;
    return true;
}
void mvec_masked(void) { mstat.masked++; }

bool mvec_push(int rec, uint32_t pc[MVEC_PUSH / 4]) {
    if (cur < 0 || prev < 0 || rec < 0 || rec >= slots[cur].nrec) return false;
    const MvecRec *r = &slots[cur].rec[rec];
    if (r->prev < 0) return false;
    const MvecRec *p = &slots[prev].rec[r->prev];
    memset(pc, 0, MVEC_PUSH);
    for (int k = 0; k < r->ncb; k++) {
        int j = -1;
        for (int m = 0; m < p->ncb; m++) if (p->cb_slot[m] == r->cb_slot[k]) j = m;
        if (j < 0) return false;
        int s = r->cb_slot[k] - 3;
        pc[s] = p->cb_off[j] / 16;
        pc[14 + s] = (p->cb_size[j] + 15) / 16;
    }
    slots[prev].seq = vk_pending_seq();  // (read by this frame's draws too: kept until they are done)
    return true;
}

VkDescriptorBufferInfo mvec_history_info(void) {
    int i = prev >= 0 ? prev : cur;
    return (VkDescriptorBufferInfo){i >= 0 ? slots[i].buf : VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
}

int mvec_camera_only(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_MVEC_CAMONLY") ? atoi(getenv("SAVAGEWIND_MVEC_CAMONLY")) : 0;
    return on;
}

float mvec_view_enabled(void) {
    static float px;
    static bool init;
    if (!init) {  // (a negative value: raw, red = written, green / blue = |x| / |y| times its size)
        init = true;
        const char *e = getenv("SAVAGEWIND_MVEC_VIEW");
        px = e ? (atof(e) != 0 ? (float)atof(e) : 16.0f) : 0.0f;
    }
    return px;
}

bool mvec_reprojection(float m[16], float jitter[2]) {
    if (cur < 0 || prev < 0 || !slots[prev].cam.valid) return false;
    if (slots[cur].frame != slots[prev].frame + 1) return false;
    MvecCamera now;
    if (slots[cur].cam.valid) now = slots[cur].cam;
    else if (!camera_compute(&slots[cur], &now)) return false;
    // prev proj * (prev view * inverse of this view) * inverse of this proj, in double: the views hold the camera's
    // world position (thousands of meters), and the product of float matrices with an inverse lost the far plane's
    // precision: the sky's and the distance's camera vectors were wrong by ~1 pixel, in step with the jitter (a sway
    // of the sky, clouds and distant trees under TAA and FSR 3). The rigid views' relative motion cancels the position
    // before anything is rounded.
    const MvecCamera *pc_ = &slots[prev].cam;
    double vp4[16] = {0}, vc4[16] = {0}, rel[16], pp[16], pcur[16], pinv[16], t[16], md[16];
    for (int i = 0; i < 12; i++) vp4[i] = pc_->view[i], vc4[i] = now.view[i];
    vp4[15] = vc4[15] = 1;
    double vci[16] = {0}, vpi[16] = {0};  // (the inverse of a rigid view: transposed rotation, minus it times the translation)
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            vci[r * 4 + c] = vc4[c * 4 + r];
            vpi[r * 4 + c] = vp4[c * 4 + r];
        }
        vci[r * 4 + 3] = -(vc4[r] * vc4[3] + vc4[4 + r] * vc4[7] + vc4[8 + r] * vc4[11]);
        vpi[r * 4 + 3] = -(vp4[r] * vp4[3] + vp4[4 + r] * vp4[7] + vp4[8 + r] * vp4[11]);
    }
    vci[15] = vpi[15] = 1;
    double dx = vci[3] - vpi[3], dy = vci[7] - vpi[7], dz = vci[11] - vpi[11];
    double dp = dx * dx + dy * dy + dz * dz;
    double dot = vc4[2] * vp4[2] + vc4[6] * vp4[6] + vc4[10] * vp4[10];
    if (dp > 100.0 || dot < 0.82) return false;  // a cut: camera moved > 10 m or turned > 35 degrees
    for (int i = 0; i < 16; i++) pp[i] = pc_->proj[i], pcur[i] = now.proj[i];
    if (!mat4_inverse_d(pcur, pinv)) return false;
#define MUL_D(a, b, o) for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) \
        o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c]
    MUL_D(vp4, vci, rel);
    MUL_D(pp, rel, t);
    MUL_D(t, pinv, md);
#undef MUL_D
    for (int i = 0; i < 16; i++) m[i] = (float)md[i];
    // a projection offset (proj[0][2], proj[1][2]) moves the picture by minus itself in NDC
    jitter[0] = -(now.proj[2] - slots[prev].cam.proj[2]);
    jitter[1] = -(now.proj[6] - slots[prev].cam.proj[6]);
    return true;
}

bool mvec_camera_now(float jitter[2], float *near_, float *far_, float *fov_y) {
    if (cur < 0) return false;
    MvecCamera now;
    if (slots[cur].cam.valid) now = slots[cur].cam;
    else if (!camera_compute(&slots[cur], &now)) return false;
    jitter[0] = -now.proj[2], jitter[1] = -now.proj[6];
    // (OpenGL-style rows: [2][2] = -(f + n) / (f - n), [2][3] = -2fn / (f - n), [1][1] = 1 / tan(fovy / 2))
    float a = now.proj[10], b = now.proj[11];
    *near_ = b / (a - 1), *far_ = b / (a + 1), *fov_y = 2 * atanf(1 / now.proj[5]);
    return true;
}

const MvecCamera *mvec_camera(bool previous) {
    int i = previous ? prev : -1;  // (the frame being recorded has no camera yet: the previous one and the one before)
    if (!previous && prev >= 0) i = (prev + MVEC_SLOTS - 1) % MVEC_SLOTS;
    return i >= 0 && slots[i].cam.valid ? &slots[i].cam : NULL;
}

const MvecRec *mvec_records(bool previous, int *n, VkBuffer *history) {
    int i = previous ? prev : cur;
    if (i < 0) { *n = 0; *history = VK_NULL_HANDLE; return NULL; }
    *n = slots[i].nrec;
    *history = slots[i].buf;
    return slots[i].rec;
}
