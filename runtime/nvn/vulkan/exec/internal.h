// Shared state of the Vulkan command executor (exec.c) and the modules it is being split into.
//
// exec.c stays the command-list interpreter and owns the state below; an extracted module includes this header and
// reads the state directly or calls the entry points declared here as they move. This first header exposes only the
// state every later module needs.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../shaders.h"
#include "../vk.h"

// (an environment switch read once: the debug logs that would otherwise call getenv per command)
#define ENV_ONCE(name) ({ static int env_once_ = -1; if (env_once_ < 0) env_once_ = getenv(name) != NULL; env_once_; })

// The render targets bound by the command list (begin_pass reads them).
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

// What the last commands bound: the game's NVN state, written by exec_list and read by draws, passes, clears and
// copies.
typedef struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    float line_width;
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    bool depth_clamp;
    bool rasterizer_discard;  // nvnCommandBufferSetRasterizerDiscard: draw() skips draws (no transform feedback here)
    uint64_t copy_row_stride, copy_image_stride;
} ExecState;

// Command buffer state a draw recorded last, so a draw records only what differs (see dyn_check).
typedef struct {
    uint64_t epoch;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t ndyn, dyn[32];  // dynamic uniform-buffer offsets bound with them
    uint32_t dt, dw, dop, sten, cull, front, topo, restart, bias_on;
    uint32_t sten_face[2][8];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float blend[4], bias[3], line_w;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ibuf;
    VkDeviceSize ioff;
    uint32_t itype;
    VkBuffer vbuf[17];
    VkDeviceSize voff[17];
} ExecDrawCache;

extern ExecState st;
extern ExecDrawCache dc;

// A translated program (exec.c owns the translation and the tables; draws use it by value).
#define MAX_BIND 96
typedef struct Prog {
    ShaderInfo *vi, *fi;
    struct Prog *hdr;           // (hdr.c) the HDR tonemap variant of this program, made once; draws use it after scene_post
    bool hdr_tried;             // (the variant was attempted: no retry every frame when it is not ours)
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    int8_t ui;                  // vertex shader of the game's layouts (freecam_hud_drop): 0 unknown, 1 yes, -1 no
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    struct StageBuildRef { void *v, *f; } stages;  // (the ProgBuild's StageBuilds: graphics pipeline libraries)
    bool vs_uses_vid;
    uint8_t fs_gather;  // (up_fix) the fragment stage's first gathered texture: 0 not looked yet, 1 none, 2 + its slot
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    // Its motion vector variants once they are built (draw_, mvec.c), by the pass's motion target slot: [0] the motion
    // program, [1] the reactive mask's. (Looked up through get_program for every scene draw, their keys made anew
    // each time, they cost the translating thread ~0.2 ms a frame.)
    void *mv[2][8];
    // The fragment material cache (stage_set_reuse, SAVAGEWIND_VK_STAGE_REUSE): whether set 1 can be kept, and the
    // uniform buffers it compares. (Last: in the middle they moved the fields every draw reads, ~2% of the render
    // thread at 720p with the cache off.)
    bool reuse_stage[2];
    uint32_t reuse_mask[2];
    uint8_t set_idx[2][MAX_BIND];
    uint8_t set_n[2];
    bool has_rscale[2], has_tint[2];
} Prog;

// One stage's SPIR-V module and bindings, per translated shader: programs share their vertex and fragment shaders
// (the same ShaderInfo, vk_translate_cached), so a stage seen in another program costs no SPIR-V load, reflection or
// module creation again.
typedef struct {
    const ShaderInfo *si;
    VkShaderModule m;
    int nbinds;
    VkBindingInfo binds[MAX_BIND];
    // graphics pipeline libraries (g_vk.gpl): the stage's descriptor set layout (set 0 vertex, 1 fragment), a pipeline
    // layout of it alone, and the stage's libraries by variant (gpl_stage_lib)
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout lib_layout;
    _Atomic uint64_t lib[8];
} StageBuild;

// Everything a pipeline bakes in, captured by value so it can be built on a worker thread: the program, and the
// fixed state (PipeState, also what the pipeline list stores; zeroed before it is filled: compared and hashed whole).
#define ZERO_BINDING 16
typedef struct {
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
    uint8_t feedback;  // samples a color target it writes (VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
    uint8_t depth_clamp;  // nvnCommandBufferSetDepthClamp (in what was padding: older pipeline lists read it as off)
} PipeState;
typedef struct { Prog *prog; PipeState s; } PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed

// Generations that invalidate cached command-buffer state.
extern uint64_t dyn_epoch;
extern int no_dyn_cache;
extern uint64_t key_gen;   // program and pipeline keys
extern uint64_t setc_gen;  // shared descriptor sets of the submission in progress

// (after vk_cmd) a new command buffer or foreign state: nothing is known to be bound.
static inline void dyn_check(void) {
    if (dc.epoch == dyn_epoch) return;
    memset(&dc, 0xFF, sizeof dc);  // matches no value (floats: NaN)
    dc.epoch = dyn_epoch;
}
#define DYN_SAME(field, val) (dc.field == (val) ? true : (dc.field = (val), false))

// Helpers that stay in exec.c and are shared with the modules it is split into.
extern const char *snap_reason;
extern int dl_on;
extern bool g_debug_frame;
void snapshots_invalidate(NvnTexture *t);
void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
void dl_note(const char *fmt, ...);
int rt_kind(uint32_t f);
uint32_t level_extent(uint32_t v, uint32_t lv);
void hc_cpu_wrote(uint64_t addr, uint64_t size);

// (exec.c) the state the draw path reads and writes; exec_draw.c is split from exec.c and shares it here.
typedef struct { uint64_t v; uint32_t pc; bool word; } FsSel;
typedef struct { uint32_t gw, gh; float s; } SdwFix;
typedef struct { uint32_t gw, gh; float sx, sy; uint32_t tw, th, aw, ah; } UpFix;
typedef struct { uint64_t key; Pipe *pipe; } PipeSlot;
typedef struct { PipeKey k; Pipe *pp; } PipeMemo;
#define GPQ 2048  // timestamps a submission can hold
typedef struct { uint32_t n, gen; bool started; uint64_t frame, seq, submit_ticks; uint32_t site[GPQ], draws[GPQ]; } GpSub;
#define SUBS 32
#define GP_MAX 4096
#define OQ_SLOTS 1024
#define OQ_RUN 64

extern bool draw_feedback, in_pass, pass_void, pass_ds_written, pass_frag_only, pass_vs_reads_next;
extern bool oq_counting, hud_clip, hud_cut, fxaa_off, bar_other, g_gpuprof_frame;
extern uint64_t draws_total, g_cur_fs_hash, gcopy_gen, cum_draws;
extern uint64_t stat_build_waits, stat_det_miss, stat_draws, stat_skipped, stat_frag_restarts, stat_links_now;
extern _Atomic int stat_warm;
extern uint8_t det_draw, det_written, pass_color_unsynced, pass_color_written;
extern unsigned fb_draws;
extern int cur_sub, pass_dbg_draws, pass_mvec, oq_nrun, gp_n;
extern uint32_t chunk_draws, pass_ncolor, pass_samples, pass_w, pass_h, oq_next;
extern float pass_sx, pass_sy, vp_sx, vp_sy, vp_ox, vp_oy;
extern float tex_sc[2][64][2];
extern uint32_t tex_wh[2][64][2];
extern VkFormat pass_color[8], pass_depth, pass_stencil;
extern VkQueryPool oq_pool;
extern uint32_t oq_run[OQ_RUN];
extern float oq_scale[OQ_SLOTS];
extern uint64_t oq_seq[OQ_SLOTS];
extern TexBackend *map_background;
extern uint64_t map_background_frame;
extern SdwFix sdw_fix;
extern UpFix up_fix;
extern GpSub gp_sub[SUBS];
extern char gp_label[GP_MAX][112];
extern uint32_t gp_draws[GP_MAX];
extern uint32_t pipe_cap, pipe_n;
extern PipeSlot *pipe_tab;
extern PipeMemo pipe_memo;
extern int profd;
extern int lst_on;
extern uint64_t stat_discarded;
extern uint64_t prof_draw, prof_dyn, prof_feedback_resolve, prof_gw_check, prof_index_copy, prof_index_direct;
extern uint64_t prof_indices, prof_key, prof_pre, prof_sets, prof_snapshots, prof_tex;
extern uint64_t prof_vertex_bind, prof_vertex_copy, prof_vertex_direct, prof_vtx;

void wait_built(_Atomic int *state);
void build_pipeline(void *arg);
void lst_draw(Prog *pr);
void debug_save_channels(TexBackend *b, const char *path);
void debug_save_3d(TexBackend *b, const char *path);
void apply_scissor(void);
int attr_kind(uint32_t fmt);
void begin_pass(void);
void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
           uint32_t smask);
VkCompareOp compare_fn(uint8_t f);
void debug_save_depth(TexBackend *b, const char *path);
bool depth_read_only(void);
uint8_t det_current(void);
uint64_t det_current_key(void);
void det_note(uint64_t key, uint8_t written);
int det_on(void);
bool fs_sel_match(const FsSel *f, uint64_t code, uint64_t hash);
int fs_sel_parse(const char *p, FsSel *out, int max);
bool fxaa_shader(uint64_t fs_hash);
Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey);
VkSpan guest_copy(uint64_t addr, uint64_t size);
bool guest_fetch(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
void gw_check(const uint32_t cbufs[6], bool vertices, uint64_t iaddr, uint64_t isize);
void gw_note(int stage);
uint64_t hash_words(uint64_t h, const void *p, size_t n);
uint64_t hmix(uint64_t h, const void *p, size_t n);
void hdr_make_tonemap_prog(Prog *pr);
bool job_run_one(void);
void map_background_before_cursor(void);
void pass_size(int *w, int *h);
uint32_t pipe_slot_of(uint64_t h, uint32_t cap);
void pipe_tab_grow(void);
VkImageView post_view(TexBackend *d);
void race_probe_note(uint64_t addr, uint64_t size, bool index);
void resolve_feedback(Prog *pr);
uint32_t rt_format(int i);
void run_job(void (*fn)(void *), void *arg);
int samples_log2(uint32_t n);
void scene_post(Prog *pr, CmdRec *r);
VkRect2D scissor_rect(void);
void set_viewport(float x, float y, float w, float h, float zn, float zf);
const NvnTexEntry *shader_texture_entry(int stage, ShaderTexture *st_);
NvnTexture *shader_texture_object(int stage, ShaderTexture *st_);
void shadow_copy_note(ShaderInfo *fi);
VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si,
                          const float *rscale_row0, const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz,
                          const VkDescriptorImageInfo *pre_ii, uint32_t *dyn, int *ndyn);
VkDescriptorSet stage_set_reuse(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si,
                                const float *rscale_row0, const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz,
                                const VkDescriptorImageInfo *pre_ii, uint32_t *dyn, int *ndyn);
VkStencilOp stencil_op(uint8_t o);
bool sync_shaders(void);
bool oq_on(void);
void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]);
VkFormat vertex_format(uint32_t f);
VkFormat vertex_format_supported(VkFormat f);
VkPipeline warm_ready(const ShaderInfo *vi, const ShaderInfo *fi, const PipeState *s);
void fg_before_draw(void);
void fg_draw(Prog *pr, CmdRec *r);
bool freecam_hud_drop(Prog *pr, CmdRec *r);
