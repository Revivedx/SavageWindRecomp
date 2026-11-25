// Draws of the Vulkan command executor (exec.c): index expansion, the program/pipeline lookup, link and wait, and
// the state a draw records. Split from exec.c; the shared state and helpers are in exec/internal.h.
#include <SDL3/SDL.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../ui_layout.h"
#ifdef _WIN32
#define NOUSER
#include <windows.h>
#endif

#include "../../kernel.h"
#include "../../control.h"
#include "../../prof.h"
#include "../../platform.h"
#include "shaders.h"
#include "vk.h"
#include "exec/internal.h"
#include "exec_feedback.h"
#include "exec_draw.h"
#include "bloom_scale.h"
#include "exec_copy.h"
#include "vram.h"
#include "fsr3.h"
#include "fsr_api.h"
#include "mvec.h"
#include "xess.h"
#include "../output_scale.h"
#include "record.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../color_grade.h"
#include "../../colour_vision.h"
#include "../output_scale.h"
#include "framegen.h"
#include "dlss.h"
#include "../output_scale.h"
#include "../../game_sites.h"
#include "../../framepace.h"
#include "../../services/fs_layout.h"
#include "../../perfcount.h"
#include "../hud_scale.h"

#define PROFD_T0() uint64_t prof_t0_ = profd ? SDL_GetTicksNS() : 0
#define PROFD_ADD(v) do { if (profd) v += SDL_GetTicksNS() - prof_t0_; } while (0)
#define PZ_NOW() (prof_enabled() ? prof_zone_begin(NULL) : (ProfZone){0})
#define PZ_SPAN(name, a, b) do { \
    static ProfSite pz_site_ = {name, 0}; \
    ProfZone pz_a_ = (a), pz_b_ = (b); \
    if (pz_a_.t0 && pz_b_.t0 && pz_a_.gen == pz_b_.gen) \
        prof_acc_add_gen(pz_a_.gen, &pz_site_, pz_b_.t0 - pz_a_.t0); \
} while (0)


// (exec.c and other modules) the draw path's dependencies
uint64_t renderer_present_count(void);
bool renderer_texture_presented(NvnTexture *t);
void renderer_save_texture_tex(TexBackend *b, const char *path);
uint32_t nvn_depth_mode(void);
uint32_t nvn_window_origin(void);
bool vk_trace_tex(NvnTexture *t);
void vk_trace_event(NvnTexture *t, uint64_t key, NvnTexture *const *srcs, int nsrcs, const char *fmt, ...);
uint64_t vk_hash_bytes(const void *p, uint64_t n);

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// (debugging) SAVAGEWIND_DEBUG_PICK=<x>,<y>[;<x>,<y>...] (up to 4 points, first-target pixels): in a debug frame each
// draw copies those pixels of its first target afterwards (its own pass), and vk_pick_report logs, per point, the
// draws that changed it ("[pick]"): the last one is what the frame shows there. SAVAGEWIND_DEBUG_PICK_RT=<i>: target i
// of each pass instead of the first (a G-buffer's albedo, say), skipping passes that have none.
typedef struct { uint64_t vs, fs, hash; void *img; uint32_t w, h, fmt, no; } PickRec;
static struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; int n, npt, on, rt; int pt[4][2]; uint32_t draws; PickRec rec[4096]; } pick = {.on = -1};

static void pick_after_draw(void) {
    if (pick.on < 0) {
        pick.on = 0;
        for (const char *e = getenv("SAVAGEWIND_DEBUG_PICK"); e && *e && pick.npt < 4;) {
            if (sscanf(e, "%d,%d", &pick.pt[pick.npt][0], &pick.pt[pick.npt][1]) != 2) break;
            pick.npt++;
            e = strchr(e, ';');
            if (e) e++;
        }
        pick.on = pick.npt > 0;
        const char *rt = getenv("SAVAGEWIND_DEBUG_PICK_RT");
        pick.rt = rt ? atoi(rt) : 0;
        if (pick.rt < 0 || pick.rt > 7) pick.rt = 0;
    }
    pick.draws++;
    const int t = pick.rt;
    if (!pick.on || st.rt.ncolor <= t || !st.rt.color[t] || !st.rt.color[t]->backend || pick.n >= 4096) return;
    TexBackend *b = st.rt.color[t]->backend;
    if ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->samples > 1 || b->width < 64) return;
    if (!pick.buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 * 4 * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &pick.buf, &pick.mem, &info));
        pick.ptr = info.pMappedData;
    }
    uint32_t layer = st.rt.has_cview[t] && (st.rt.cview[t].setmask & 2) ? st.rt.cview[t].min_layer : 0;
    uint32_t level = st.rt.has_cview[t] && (st.rt.cview[t].setmask & 1) ? st.rt.cview[t].base_level : 0;
    if (level >= b->levels || layer >= b->layers) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int k = 0; k < pick.npt; k++) {
        uint32_t x = (uint32_t)pick.pt[k][0] * b->width / 1600 >> level, y = (uint32_t)pick.pt[k][1] * b->height / 900 >> level;
        memset(pick.ptr + ((size_t)pick.n * 4 + k) * 16, 0, 16);
        if (x >= level_extent(b->width, level) || y >= level_extent(b->height, level)) continue;
        VkBufferImageCopy c = {.bufferOffset = ((VkDeviceSize)pick.n * 4 + k) * 16,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                               .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {1, 1, 1}};
        rec_CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, pick.buf, 1, &c);
    }
    PickRec *r = &pick.rec[pick.n++];
    r->vs = st.program->stages[0].code_addr;
    r->fs = st.program->stages[1].code_addr;
    r->hash = g_cur_fs_hash;
    r->img = (void *)b->image;
    r->w = b->width; r->h = b->height; r->fmt = st.rt.color[t]->format; r->no = pick.draws;
}

void vk_pick_report(void) {
    if (!pick.n) return;
    renderer_flush();
    for (int k = 0; k < pick.npt; k++) {
        LOG("[pick] point %d (%d,%d of 1600x900): draws that changed it\n", k, pick.pt[k][0], pick.pt[k][1]);
        for (int i = 0; i < pick.n; i++) {
            const uint8_t *v = pick.ptr + ((size_t)i * 4 + k) * 16;
            int j = i - 1;  // the previous draw into the same image
            while (j >= 0 && pick.rec[j].img != pick.rec[i].img) j--;
            if (j >= 0 && !memcmp(v, pick.ptr + ((size_t)j * 4 + k) * 16, 16)) continue;
            const uint32_t *u = (const uint32_t *)v;
            LOG("[pick]   draw %u fs %llx hash %016llx vs %llx | target %ux%u f%#x [%p] | %08x %08x %08x %08x\n", pick.rec[i].no,
                (unsigned long long)pick.rec[i].fs, (unsigned long long)pick.rec[i].hash, (unsigned long long)pick.rec[i].vs,
                pick.rec[i].w, pick.rec[i].h, pick.rec[i].fmt, pick.rec[i].img, u[0], u[1], u[2], u[3]);
        }
    }
    pick.n = 0;
    pick.draws = 0;
}

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    // Quad strip: each quad (v0 v1 v3 v2) is the strip's two triangles (v0 v1 v2) and (v1 v2 v3), the same pixels in the
    // same order. The map's Hero's Path draws its line as one at the two nearer zooms (#441): left out, it was missing.
    case 8: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

int lst_on = -1;  // (SAVAGEWIND_VK_LIST_STATS, see exec_list)
// With "Asynchronous shaders" on, a draw whose program or pipeline is still being built is skipped. That is fine for
// what the game draws again every frame, not for what it draws once into a texture it keeps: BOTW copies each terrain
// tile it streams in once into its layer of the height, normal and material arrays, and a skipped copy left material
// 0 (grass) there for good: green bands on the sand outside Gerudo Town, in ~3 runs of 4. Draws into array, 3D and
// cube textures that don't follow the output scale (the scene's own targets do) wait for their build instead.
static bool draw_must_happen(void) {
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        const NvnTexture *t = st.rt.color[i];
        if (!t || (t->target != 2 && t->target != 3 && t->target != 4 && t->target != 8 && t->target != 9)) continue;
        const TexBackend *b = t->backend;
        if (!b || !b->scalable) return true;
    }
    return false;
}
// HDR: remember the float picture a draw (or copy) writes into the game's presented 8-bit image; present_locked uses
// it instead of the clipped copy. Any other draw into that image forgets it.
static void hdr_record_draw(Prog *pr) {
    if (!vk_hdr_game_enabled() || !st.rt.ncolor || !st.rt.color[0] || !pr || !pr->fi) return;
    NvnTexture *rt = st.rt.color[0];
    if (vk_texture_scale_group(rt) != RS_OUT) return;
    TexBackend *rb = vk_tex(rt, true);
    if (!rb || !rb->presented || rb->hdr_upgraded) return;
    VkImage src = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    int n = 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        if (!t) continue;
        n++;
        TexBackend *b = vk_tex(t, false);
        if (b && b->hdr_upgraded && b->image && b->width == rb->width && b->height == rb->height && b->layers == 1 &&
            b->depth == 1 && b->samples == 1) {
            src = b->image, fmt = b->format, w = b->width, h = b->height;
        }
    }
    vk_hdr_record(rb->image, n == 1 ? src : VK_NULL_HANDLE, fmt, w, h);
}

static void draw_(CmdRec *r, bool indexed);
uint64_t stat_discarded;
// Branch-free max-index scan when primitive restart is off so clang auto-vectorizes to SIMD pmax.
static uint32_t scan_max_u16(const uint16_t *s, uint32_t count, bool prim_restart, uint16_t restart, uint32_t cur) {
    if (!prim_restart) {
        uint16_t mx = cur > 0xFFFFu ? 0xFFFFu : (uint16_t)cur;
        for (uint32_t i = 0; i < count; i++)
            if (s[i] > mx) mx = s[i];
        return mx > cur ? mx : cur;
    }
    for (uint32_t i = 0; i < count; i++)
        if (s[i] > cur && s[i] != restart) cur = s[i];
    return cur;
}

static uint32_t scan_max_u32(const uint32_t *s, uint32_t count, bool prim_restart, uint32_t restart, uint32_t cur) {
    if (!prim_restart) {
        for (uint32_t i = 0; i < count; i++)
            if (s[i] > cur) cur = s[i];
        return cur;
    }
    for (uint32_t i = 0; i < count; i++)
        if (s[i] > cur && s[i] != restart) cur = s[i];
    return cur;
}

void draw(CmdRec *r, bool indexed) {
    // Rasterizer discard: the primitives stop before rasterization, so nothing reaches the targets, depth, stencil or
    // occlusion queries. Its only output in NVN is transform feedback, which isn't implemented (cmdbuf.c), so the draw
    // has nothing left to do (a vertex shader writing storage buffers or images would be the one difference).
    if (st.rasterizer_discard) { stat_discarded++; return; }
    PROFD_T0();
    SW_ACC("exec/draw");
    draw_(r, indexed);
    PROFD_ADD(prof_draw);
}
static uint64_t prof_tp_;
// Asynchronous shaders skip a draw whose program or pipeline is still being built: fine for targets drawn every frame
// (the next frame has it), not for one drawn once and then only sampled. BOTW's pause menu blurs a capture of the
// scene once when it opens (the background stayed black) and the save dialog's picture is made once. So a draw into
// targets no pass drew in the previous frame waits for its shaders, as with synchronous shaders.
// What is safe to skip, in all: a draw into targets some pass drew in the previous frame (the scene, the G-buffer,
// shadow maps, depth pre-passes: the next frame draws them again; under an occlusion query the object counts as
// hidden for that frame), feedback-loop draws included. What waits: one-shot targets (here), texture arrays, 3D and
// cube textures the game fills once (draw_must_happen), every compute dispatch (dispatch_compute: its writes may be
// read for good), and the runtime's own pipelines (present, blits, clears: made synchronously). The "skip when the
// pass's targets were rendered in the previous frame, wait otherwise" rule and the inline link of ready stages
// (link_now) follow TotK Recomp's TOTK_PIPE_SKIP (Valiantsin Dzerakh, MIT: third_party/totk-pc-recomp/LICENSE),
// written here in this runtime's own terms.
static bool one_shot_target(void) {
    uint64_t now = renderer_present_count();
    NvnTexture *ts[9];
    int n = 0;
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) if (st.rt.color[i]) ts[n++] = st.rt.color[i];
    if (!n && st.rt.depth) ts[n++] = st.rt.depth;
    for (int i = 0; i < n; i++) {
        TexBackend *b = ts[i]->backend;
        uint64_t last = !b ? 0 : b->drawn_frame == now ? b->prev_drawn : b->drawn_frame;
        if (last + 1 < now) return true;
    }
    return false;
}

// Draws into one-shot targets (alpha3-glitches) or texture arrays, 3D and cube textures the game fills once (terrain
// tiles: region-tests) wait for their program or pipeline instead of being skipped while it builds.
// Asynchronous shaders, a pipeline whose stages' libraries are built already (the precompiled shaders', or a draw's
// before): the draw links it here, now, instead of skipping a frame for a job that takes a fraction of a millisecond
// (graphics pipeline libraries link without compiling; 0.2 ms each measured on the Steam Deck's RADV). Up to
// SAVAGEWIND_PIPE_LINK_MS (default 3) per presented frame: a burst of hundreds of new pipelines (an area streamed in,
// a fight) links that much here and hands the rest to the workers, skipping their draws until built, so no frame
// stalls on them - where a first boot linked 236 pipelines in one 109 ms frame. A stage library still to compile
// (5-50 ms) always goes to a worker. SAVAGEWIND_PIPE_LINK_MS=0: every build on a worker, as before.
static bool link_now(Pipe *pp, bool must_happen) {
    static int64_t budget = -1;
    if (budget < 0) budget = (getenv("SAVAGEWIND_PIPE_LINK_MS") ? atoi(getenv("SAVAGEWIND_PIPE_LINK_MS")) : 3) * 1000000ll;
    const Prog *pr = pp->k.prog;
    const PipeState *s = &pp->k.s;
    const StageBuild *v = pr->stages.v, *f = pr->stages.f;
    if (budget <= 0 || !g_vk.gpl || s->feedback || !v || !f) return false;
    int vv = (s->depth_clamp ? 1 : 0) | (s->poly_fill ? 2 : 0), fv = (samples_log2(s->samples) << 1) | (s->a2c ? 1 : 0);
    if (vv >= 8 || fv >= 8 || !atomic_load(&v->lib[vv]) || !atomic_load(&f->lib[fv])) return false;
    static uint64_t frame, spent;
    uint64_t now = renderer_present_count();
    if (now != frame) { frame = now; spent = 0; }
    if (!must_happen && spent >= (uint64_t)budget) return false;
    uint64_t t0 = SDL_GetTicksNS();
    build_pipeline(pp);
    uint64_t dt = SDL_GetTicksNS() - t0;
    spent += dt;
    stat_links_now++;
    // What a link costs here, logged once: the budget is checked before a link, so it holds only while links are
    // quick (#818: frames of 100-150 ms with 30-90 links on AMD's Windows driver; an RX 6700 XT measured 0.02 ms)
    static uint64_t n_links, ns_links;
    if (n_links < 16 && (n_links++, ns_links += dt, n_links == 16))
        LOG("[pso] linking on the render thread: %.2f ms a pipeline\n", ns_links / 16 / 1e6);
    return true;
}

// The first 2 ms are spent spinning: most of these builds take microseconds (a program whose modules are already built,
// a pipeline link), and a sleep costs at least a millisecond each. #707: after three hours of play, ~40 such waits a
// frame on draws into array textures held the game at 12 fps, with nothing compiled.
void wait_built(_Atomic int *state) {  // (0 building: until built or failed, 5 s at most)
    if (atomic_load(state) != 0 || sync_shaders()) return;
    uint64_t t0 = SDL_GetTicksNS();
    while (atomic_load(state) == 0 && SDL_GetTicksNS() - t0 < 5000000000ull) {
        if (job_run_one()) continue;
        if (SDL_GetTicksNS() - t0 < 2000000ull) { SDL_CPUPauseInstruction(); continue; }
        SDL_Delay(1);
    }
    stat_build_waits++;
    perfcount_add(PC_BUILD_WAITS, 1);
    perfcount_add(PC_BUILD_WAIT_NS, SDL_GetTicksNS() - t0);
}

static void draw_(CmdRec *r, bool indexed) {
    if (profd) prof_tp_ = SDL_GetTicksNS();
    ProfZone pz0 = PZ_NOW();
    fg_before_draw();
    if (!st.program) { stat_skipped++; return; }
    static int no_key_gen = -1;
    if (no_key_gen < 0) no_key_gen = getenv("SAVAGEWIND_VK_NO_KEY_GEN") != NULL;
    static struct { uint64_t gen; uint32_t origin, dmode; NvnProgram *np; Prog *pr; bool points; } pm;
    uint32_t origin = nvn_window_origin(), dmode = nvn_depth_mode();
    Prog *pr;
    bool vkey_flip = origin == 0;
    bool points = r->x[0] == 0;  // (a point list: its vertex shader writes a point size, ShaderKey.point_size)
    if (!no_key_gen && pm.pr && pm.gen == key_gen && pm.np == st.program && pm.origin == origin && pm.dmode == dmode &&
        pm.points == points) {
        pr = pm.pr;
    } else {
        ShaderKey vkey = {0}, fkey = {0};
        vkey.glsl = fkey.glsl = 1;
        vkey.flip_y = fkey.flip_y = origin == 0;
        vkey.depth_minus_one = dmode == 0;
        vkey.point_size = points;
        for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
        for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(rt_format(i));
        pr = get_program(st.program, &vkey, &fkey);
        pm.gen = key_gen; pm.np = st.program; pm.origin = origin; pm.dmode = dmode; pm.pr = pr; pm.points = points;
    }
    if (pr && atomic_load(&pr->state) == 0 && (one_shot_target() || draw_must_happen())) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) {
        if (st.rt.ncolor && vk_trace_tex(st.rt.color[0]))
            vk_trace_event(st.rt.color[0], 2, NULL, 0, "draw skipped: program %s", pr ? "not compiled yet" : "missing");
        stat_skipped++;
        return;
    }
    //if (profd) prof_program += SDL_GetTicksNS() - prof_program_start;
    {   // fragment-shader hash: skip lists (SAVAGEWIND_SKIP_FSHASH), debug logs, GPU profile labels
        static uint64_t last_fs, last_hash;
        static bool last_skip;
        static FsSel skip[16];
        static int nskip = -1;
        if (nskip < 0) nskip = fs_sel_parse(getenv("SAVAGEWIND_SKIP_FSHASH"), skip, 16);
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            // (by code address, like the program table: 512 bytes hashed byte by byte at every fragment shader change
            // cost 1% of the translation)
            static struct { uint64_t fs, hash; } memo[512];
            uint32_t ms = (uint32_t)((fs * 0x9E3779B97F4A7C15ull) >> 55);  // (code addresses are 256-aligned)
            if (fs && memo[ms].fs == fs) last_hash = memo[ms].hash;
            else {
                last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
                memo[ms].fs = fs; memo[ms].hash = last_hash;
            }
            last_skip = false;
            for (int i = 0; i < nskip; i++) last_skip |= fs_sel_match(&skip[i], fs, last_hash);
        }
        extern volatile int g_skip_fs_off;
        if (last_skip && !g_skip_fs_off) { stat_skipped++; return; }
        if (freecam_hud_drop(pr, r)) { stat_skipped++; return; }
        if (g_debug_frame && getenv("SAVAGEWIND_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    // Video > Advanced: Bloom off: the bloom's draws (bloom_scale.h) clear their images instead, so the glow added over
    // the scene is black; Depth of field off: the distance blur's draw (fs f7731492223eb16a, blended over the distant
    // pixels) is skipped.
    if (!g_settings.bloom) {
        bool bloom = false;
        for (int i = 0; i < st.rt.ncolor; i++) {
            NvnTexture *t = st.rt.color[i];
            if (t && bloom_native_target(g_cur_fs_hash, t->target, t->format, t->width, t->height, t->samples)) {
                static const float zero[4];
                clear(i, 0, zero, 0xF, false, 0, false, 0, 0);
                bloom = true;
            }
        }
        if (bloom) { stat_skipped++; return; }
    }
    if (!g_settings.depth_of_field && g_cur_fs_hash == UINT64_C(0xf7731492223eb16a)) { stat_skipped++; return; }
    // The bloom's fixed tap offsets must address adjacent console-size texels.
    // Enlarging those images spaces the taps apart and produces separate copies
    // of emissive details; odd mip scratch sizes also stopped inheriting scale.
    // Recognize the actual bloom shaders, not arbitrary small HDR scene targets.
    static int bloom_scaled = -1;
    if (bloom_scaled < 0) bloom_scaled = getenv("SAVAGEWIND_BLOOM_SCALED") != NULL;
    if (!bloom_scaled) {
        for (int i = 0; i < st.rt.ncolor; i++) {
            NvnTexture *t = st.rt.color[i];
            if (!t || !bloom_native_target(g_cur_fs_hash, t->target, t->format, t->width, t->height, t->samples)) continue;
            TexBackend *b = vk_tex(t, true);
            if (b->native_post) continue;
            vk_end_pass();
            if (vk_native_post_target(t)) {
                static unsigned logged;
                if (logged++ < 12) LOG("[bloom] native working target %ux%u, levels %u, fs %016llx\n",
                                      t->width, t->height, t->levels, (unsigned long long)g_cur_fs_hash);
            }
            st.rt_dirty = true;
        }
    }
    map_background_before_cursor();
    if (g_debug_frame) { debug_draw(pr, r, indexed); pass_dbg_draws++; }
    {   // (debugging) SAVAGEWIND_LOG_FS_CB=<fs hash>:<k>:<first float>:<count>: each frame, those floats of fragment cbuf k
        // (c[k], as the debug frame's fcb<k>) at the first draw of that shader: a parameter's course over a scripted walk
        static int on = -1;
        static uint64_t want, last_frame = ~0ull;
        static unsigned k, first, count;
        if (on < 0) {
            const char *e = getenv("SAVAGEWIND_LOG_FS_CB");
            unsigned long long h;
            on = e && sscanf(e, "%llx:%u:%u:%u", &h, &k, &first, &count) == 4 && k >= 3 && k < 21;
            want = on ? h : 0;
        }
        if (on && g_cur_fs_hash == want && renderer_present_count() != last_frame) {
            last_frame = renderer_present_count();
            Range u = st.ubuf[1][k - 3];
            char buf[1024];
            int o = snprintf(buf, sizeof buf, "[fscb] frame %llu c%u@%llx:", (unsigned long long)last_frame, k, (unsigned long long)u.addr);
            for (unsigned i = 0; i < count && u.addr && (first + i + 1) * 4 <= u.size && o < (int)sizeof buf - 16; i++)
                o += snprintf(buf + o, sizeof buf - o, " %g", ((const float *)(uintptr_t)u.addr)[first + i]);
            LOG("%s\n", buf);
        }
    }
    {   // (SAVAGEWIND_TRACE_TEX) a draw into a traced texture: its fragment shader and the textures it samples
        bool any = false;
        for (int i = 0; i < st.rt.ncolor && i < 8 && !any; i++) any = vk_trace_tex(st.rt.color[i]);
        if (any) {
            NvnTexture *srcs[16];
            int ns = 0;
            char buf[240];
            int o = 0;
            for (int sg = 0; sg < 2; sg++) {
                ShaderInfo *si = sg ? pr->fi : pr->vi;
                for (int i = 0; i < si->ntex && ns < 16; i++) {
                    NvnTexture *t = shader_texture_object(sg, &si->tex[i]);
                    srcs[ns++] = t;
                    if (t && o < (int)sizeof buf - 12) o += snprintf(buf + o, sizeof buf - o, " #%llu", (unsigned long long)t->debug_id);
                }
            }
            {   // the vertex stage's first uniform buffer (c[3]): its words 0x50..0x70 (BOTW's 2D quads: uv transform)
                Range u = st.ubuf[0][0];
                if (u.addr && u.size >= 0x70 && o < (int)sizeof buf - 60) {
                    const float *f = (const float *)(uintptr_t)(u.addr + 0x50);
                    o += snprintf(buf + o, sizeof buf - o, " | vs %llx c3@%llx %g %g %g %g / %g %g %g", (unsigned long long)
                                  st.program->stages[0].code_addr, (unsigned long long)u.addr, f[0], f[1], f[2], f[3], f[4],
                                  f[5], f[6]);
                }
            }
            buf[o] = 0;
            for (int i = 0; i < st.rt.ncolor && i < 8; i++)
                if (st.rt.color[i])
                    vk_trace_event(st.rt.color[i], g_cur_fs_hash + (uint64_t)i, srcs, ns,
                                   "draw as target %d (of %d)%s, fs %llx hash %016llx out %#x, mask %#x, samples%s", i,
                                   st.rt.ncolor, feedback_draw_writes_color(pr->fi->out_mask, st.channel_mask, i) ? "" : " NOT WRITTEN",
                                   (unsigned long long)st.program->stages[1].code_addr, (unsigned long long)g_cur_fs_hash,
                                   pr->fi->out_mask, st.channel_mask, buf);
        }
    }
    {   // (debugging) SAVAGEWIND_LOG_DRAW_INTO=<target address hex>: each draw into that texture, every frame: the layer,
        // viewport and scissor, and per sampled texture its object, image state and a hash of its game memory now
        static uint64_t into = ~0ull;
        if (into == ~0ull) { const char *e = getenv("SAVAGEWIND_LOG_DRAW_INTO"); into = e ? strtoull(e, NULL, 16) : 0; }
        if (into && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->addr == into) {
            char buf[512];
            int o = 0;
            for (int sg = 0; sg < 2; sg++) {
                ShaderInfo *si = sg ? pr->fi : pr->vi;
                for (int i = 0; i < si->ntex && o < (int)sizeof buf - 120; i++) {
                    NvnTexture *t = shader_texture_object(sg, &si->tex[i]);
                    if (!t) continue;
                    const TexBackend *b = t->backend;
                    uint64_t mh = t->addr && t->format == 0x25 && t->target == 1 && t->width <= 1024 && t->height <= 1024
                                      ? vk_hash_bytes((const void *)(uintptr_t)t->addr, (uint64_t)t->width * t->height * 4) : 0;
                    o += snprintf(buf + o, sizeof buf - o, " | %c%d #%llu %ux%u@%llx%s img %d up %d dyn %d mem %08llx data %08llx",
                                  sg ? 't' : 'v', i, (unsigned long long)t->debug_id, t->width, t->height, (unsigned long long)t->addr,
                                  t->finalized ? " FIN" : "", b != NULL, b ? b->uploaded : 0, b ? b->dynamic : 0,
                                  (unsigned long long)(mh & 0xFFFFFFFF), (unsigned long long)(b ? b->data_hash & 0xFFFFFFFF : 0));
                }
            }
            LOG("[into] frame %llu layer %u vp %.0f %.0f %.0f %.0f sc %d %d %d %d fs %llx%s\n", (unsigned long long)renderer_present_count(),
                st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) ? st.rt.cview[0].min_layer : 0, st.viewport[0][0],
                st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2],
                st.scissor[0][3], (unsigned long long)st.program->stages[1].code_addr, buf);
        }
    }
    fb_draws++;
    if (lst_on > 0) lst_draw(pr);
    uint64_t prof_stage_start = profd ? SDL_GetTicksNS() : 0;
    if (det_on()) {  // (a draw into a color target left out of the open pass: the pass ends, the next has it, det_tab)
        uint8_t det = det_current(), want = 0;
        for (int i = 0; i < st.rt.ncolor && i < 8; i++) if (st.rt.color[i] && feedback_draw_writes_color(pr->fi->out_mask, st.channel_mask, i)) want |= (uint8_t)(1u << i);
        if (want & det) {
            det_note(det_current_key(), want);
            if (in_pass && !st.rt_dirty) st.rt_dirty = true;
            stat_det_miss++;
        }
        det_draw = det_current();
    }
    if (pr->vi->ntex > 0 || pr->vi->uses_gmem) {  // (its vertex stage reads what earlier GPU work may have written)
        if (in_pass && !st.rt_dirty && pass_frag_only) { st.rt_dirty = true; stat_frag_restarts++; }
        pass_vs_reads_next = true;  // (whichever pass begins next, also one resolve_feedback starts: a full barrier)
    }
    resolve_feedback(pr);
    det_draw = 0;
    if (profd) {
        uint64_t now = SDL_GetTicksNS();
        prof_feedback_resolve += now - prof_stage_start;
        prof_stage_start = now;
    }
    if (vk_hdr_game_enabled() && pr->fi && pr->fi->ntex == 2 && !pr->hdr && !pr->hdr_tried)
        hdr_make_tonemap_prog(pr);
    scene_post(pr, r);
    if (pr->hdr && vk_hdr_game_enabled()) pr = pr->hdr;  // (the game's tonemap draw: the extended-curve variant, HDR only)
    fg_draw(pr, r);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    if (profd) {
        uint64_t now = SDL_GetTicksNS();
        prof_snapshots += now - prof_stage_start;
        prof_stage_start = now;
    }
    {
        uint32_t sm = st.program->stage_mask, all = ~0u;  // (geometry and tessellation stages: every bound buffer)
        uint32_t cbufs[6] = {pr->vi->cbuf_mask, pr->fi->cbuf_mask, sm & 4 ? all : 0, sm & 8 ? all : 0, sm & 16 ? all : 0, 0};
        gw_check(cbufs, true, indexed ? r->x[3] : 0, indexed ? (uint64_t)r->x[2] * 4 : 0);
    }
    if (profd) {
        uint64_t now = SDL_GetTicksNS();
        prof_gw_check += now - prof_stage_start;
        prof_stage_start = now;
    }
    begin_pass();
    if (pass_void) { stat_skipped++; return; }
    // ---- motion vectors (mvec.c): an opaque draw of a pass with the motion target is recorded (its identity and the
    // vertex shader's inputs, for the next frame); matched with one of the previous frame, it is drawn with its motion
    // program: the vertex shader also run on the previous frame's inputs, the fragment shader also writing the distance
    uint32_t mvec_seq = 0;
    bool mvec_draw = false;
    // Full-screen triangles and quads that write depth (BOTW fills the scene's depth with several: sky, far plane)
    // keep no vector: drawn after the geometry, their camera-independent positions would cover it with zeros (they get
    // the camera's motion in the fallback).
    bool fullscreen = r->x[2] <= 6 && st.viewport[0][2] >= pass_w * 0.9f / pass_sx && st.viewport[0][3] >= pass_h * 0.9f / pass_sy;
    MvecDrawIn *mdp = pass_mvec >= 0 && st.ds[0] && st.ds[1] ? mvec_draw_begin(renderer_present_count()) : NULL;
    if (mdp) {
#define md (*mdp)  // (mvec.c's place for the draw being described: only what the head counts is handed on)
        md.h.iaddr = 0, md.h.first = 0, md.h.base_vertex = 0;
        md.h.ncb = 0, md.h.ninst = 0, md.h.skey = 0;
        md.h.gen = __atomic_load_n(&gcopy_gen, __ATOMIC_RELAXED);
        md.h.pkey = mvec_mix(st.program->stages[0].code_addr, st.program->stages[1].code_addr);
        md.h.vi = pr->vi;
        md.h.count = (uint32_t)r->x[2];
        if (indexed) {
            md.h.iaddr = r->x[3]; md.h.base_vertex = (int32_t)r->x[4]; md.h.base_inst = (uint32_t)r->x[5];
            md.h.inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        } else {
            md.h.first = (uint32_t)r->x[1]; md.h.base_inst = (uint32_t)r->x[3]; md.h.inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        }
        // the streams the vertex shader's attributes read: per-vertex ones are part of the draw's identity (their
        // addresses), instanced ones are inputs (copied by mvec.c)
        uint32_t vs_streams = 0;
        for (uint32_t m = pr->vi->attr_mask & 0xFFFF & ((1u << (st.nattrib < 16 ? st.nattrib : 16)) - 1); m; m &= m - 1)
            vs_streams |= 1u << ((st.attrib[__builtin_ctz(m)] >> 8) & 0x1F);
        for (uint32_t m = vs_streams & 0xFFFF; m; m &= m - 1) {
            int s = __builtin_ctz(m);
            if (s >= st.nstream || !st.vbuf[s].addr) continue;
            if (!st.stream[s][1]) md.h.skey = mvec_mix(md.h.skey, st.vbuf[s].addr);
            else if (md.h.ninst < MVEC_INST)
                md.in[md.h.ninst++] = (MvecInst){st.vbuf[s].addr, st.vbuf[s].size, st.stream[s][0], st.stream[s][1], (uint32_t)s, 0};
        }
        for (uint32_t m = (uint32_t)(pr->vi->cbuf_mask >> 3) & 0x3FFF; m && md.h.ncb < MVEC_CB; m &= m - 1) {  // (the game's buffers: cbuf_info's k >= 3)
            int k = 3 + __builtin_ctz(m);
            Range u = st.ubuf[0][k - 3];
            if (!u.addr) continue;
            uint64_t size = pr->vi->cbuf_size[k] ? pr->vi->cbuf_size[k] : 256;
            if ((pr->vi->cbuf_dynamic & (1u << k)) && u.size) size = u.size > 0x10000 ? 0x10000 : u.size;
            if (size > u.size && u.size) size = u.size;
            md.cb[md.h.ncb++] = (MvecCb){u.addr, (uint32_t)size, (uint8_t)k, {0}};
        }
        md.h.flags = 0;  // does the vertex shader sample a texture the GPU draws into (mvec.c w_draw: then no motion of its own)
        for (int i = 0; i < pr->vi->ntex && !md.h.flags; i++) {
            NvnTexture *et = shader_texture_object(0, &pr->vi->tex[i]);
            if (et && et->backend && ((TexBackend *)et->backend)->gpu_owned) md.h.flags = MVEC_DRAWN_TEXTURE;
        }
#undef md
        mvec_seq = mvec_draw_commit();
        // Its motion program: the vertex shader also run on the previous frame's inputs when the draw moved, which
        // mvec.c's thread tells the recorder (the push constants' mflag, rec_CmdPushConstantsMvec); a draw that stood
        // still or got no match writes zero, the camera's motion (mvec_fill), over what a draw behind it wrote.
        // Until that program is built, or when it can't be (its varyings 30 / 31 taken): the still variant, zero.
        if (!fullscreen) {
            Prog *mp = pr->mv[0][pass_mvec];
            if (mp) {
                pr = mp;
                mvec_draw = true;
            } else {
                ShaderKey mvk = {0}, mfk = {0};
                mvk.glsl = mfk.glsl = 1;
                mvk.flip_y = mfk.flip_y = origin == 0;
                mvk.depth_minus_one = dmode == 0;
                for (int i = 0; i < st.nattrib; i++) mvk.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
                for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) mfk.rt_kind[i] = (uint8_t)rt_kind(rt_format(i));  // (the bound view's, as fkey's)
                mvk.mvec = 1;
                mfk.mvec_rt = (uint8_t)(pass_mvec + 1);
                mp = get_program(st.program, &mvk, &mfk);
                if (mp && atomic_load(&mp->state) == 1) {  // (else built later)
                    pr->mv[0][pass_mvec] = mp;
                    pr = mp;
                    mvec_draw = true;
                } else {
                    mvk.mvec = 0;
                    mfk.mvec_rt = (uint8_t)(0x40 | (pass_mvec + 1));
                    mp = get_program(st.program, &mvk, &mfk);
                    if (mp && atomic_load(&mp->state) == 1) pr = mp;
                }
            }
        }
    }
    // ---- reactive mask (mvec.c): a blended effect in a pass with the motion target (not writing depth, not full-screen:
    // those are BOTW's fog and light composites) marks its pixels in the motion target's .z (its alpha, kept by MAX), for
    // a temporal upscaler to trust its history less there
    // (debugging) SAVAGEWIND_MVEC_SHOW_UNMATCHED=1: the motion passes' depth-writing draws that got no vector of their
    // own (no match in the previous frame: the camera's motion) marked as the reactive mask is, magenta in
    // SAVAGEWIND_MVEC_VIEW
    static int show_unmatched = -1;
    if (show_unmatched < 0) show_unmatched = getenv("SAVAGEWIND_MVEC_SHOW_UNMATCHED") != NULL;
    if (pass_mvec >= 0 && ((!(st.ds[0] && st.ds[1]) && (st.color_state[0] & 1)) || (show_unmatched && st.ds[0] && st.ds[1] && !mvec_draw)) && !fullscreen) {
        Prog *mp = pr->mv[1][pass_mvec];
        if (!mp) {
            ShaderKey mvk = {0}, mfk = {0};
            mvk.glsl = mfk.glsl = 1;
            mvk.flip_y = mfk.flip_y = origin == 0;
            mvk.depth_minus_one = dmode == 0;
            for (int i = 0; i < st.nattrib; i++) mvk.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
            for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) mfk.rt_kind[i] = (uint8_t)rt_kind(rt_format(i));  // (the bound view's, as fkey's)
            mfk.mvec_rt = (uint8_t)(0x80 | (pass_mvec + 1));
            mp = get_program(st.program, &mvk, &mfk);
            if (mp && atomic_load(&mp->state) != 1) mp = NULL;  // (built for the next frames)
            if (mp) pr->mv[1][pass_mvec] = mp;
        }
        if (mp) pr = mp, mvec_masked();
    }
    if (st.rt.depth && !depth_read_only()) pass_ds_written = true;
    for (int i = 0; i < st.rt.ncolor && i < 8; i++)
        if (st.rt.color[i] && feedback_draw_writes_color(pr->fi->out_mask, st.channel_mask, i)) {
            pass_color_written |= (uint8_t)(1u << i);
            det_written |= (uint8_t)(1u << i);
            pass_color_unsynced |= (uint8_t)(1u << i);
        }
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }

    uint64_t prof_tk_ = profd ? SDL_GetTicksNS() : 0;
    if (profd) prof_pre += prof_tk_ - prof_tp_;
    ProfZone pz1 = PZ_NOW();
    PZ_SPAN("exec/draw/pre", pz0, pz1);
    // ---- pipeline
    static struct { uint64_t gen; Prog *pr; uint8_t cls, fb; } km;
    Pipe *pp;
    if (!no_key_gen && pipe_memo.pp && km.gen == key_gen && km.pr == pr && km.cls == cls && km.fb == draw_feedback) {
        pp = pipe_memo.pp;
        goto have_pipe;
    }
    km.gen = key_gen; km.pr = pr; km.cls = cls; km.fb = draw_feedback;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.s.ncolor = pass_ncolor;
    // only the pass's formats: the slots past them hold earlier passes' (not part of the pipeline, and different
    // leftovers made duplicate pipelines)
    memcpy(k.s.color, pass_color, sizeof k.s.color[0] * (pass_ncolor < 8 ? pass_ncolor : 8));
    k.s.depth = pass_depth;
    k.s.stencil = pass_stencil;
    k.s.samples = pass_samples;
    k.s.feedback = draw_feedback;
    k.s.depth_clamp = st.depth_clamp && g_vk.depth_clamp;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.s.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.s.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.s.attr_bind[i] = ZERO_BINDING;
            k.s.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.s.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.s.attr_off[i] = off;
        k.s.attr_bind[i] = stream;
        k.s.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.s.used_streams & (1u << s))) continue;
        k.s.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.s.divisor[s] = s < st.nstream && k.s.stride[s] ? st.stream[s][1] : 0;
        if (k.s.divisor[s] > 1 && !g_vk.divisor) k.s.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.s.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.s.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.s.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(rt_format(i))) {
            k.s.blend_on |= (uint8_t)(1u << i);
            memcpy(k.s.blend[i] + 1, st.blend[i] + 1, 6);  // (the factors and ops; 0 is the target index, 7 padding)
        }
    }
    if (pass_mvec >= 0 && (pr->fi->mvec || pr->fi->mvec_still)) k.s.write_mask[pass_mvec] = 0xF;  // (mvec.c: only motion and still programs write it)
    if (pass_mvec >= 0 && pr->fi->mvec_mask) {  // (the reactive mask: .z alone, the strongest effect kept)
        static const uint8_t max_blend[8] = {0, 2, 2, 2, 2, 5, 5, 0};  // (ONE, ONE, MAX: blend_factor, blend_op)
        k.s.write_mask[pass_mvec] = 0x4;
        k.s.blend_on |= (uint8_t)(1u << pass_mvec);
        memcpy(k.s.blend[pass_mvec], max_blend, 8);
    }
    k.s.a2c = st.ms[0xC] != 0;
    k.s.topo_class = cls;
    k.s.poly_fill = st.poly[2] != 1;
    pp = pipe_memo.pp;
    if (!pp || memcmp(&pipe_memo.k, &k, sizeof k)) {
        uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
        if (!ph) ph = 1;
        if (!pipe_tab) pipe_tab_grow();
        uint32_t pslot = pipe_slot_of(ph, pipe_cap);
        while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (pipe_cap - 1);
        pp = pipe_tab[pslot].pipe;
        if (!pp) {
            if ((pipe_n + 1) * 2 > pipe_cap) {  // (an insertion: grow first, then find the slot again)
                pipe_tab_grow();
                pslot = pipe_slot_of(ph, pipe_cap);
                while (pipe_tab[pslot].key) pslot = (pslot + 1) & (pipe_cap - 1);
            }
            pp = calloc(1, sizeof *pp);
            pp->k = k;
            pipe_tab[pslot].key = ph;
            pipe_tab[pslot].pipe = pp;
            pipe_n++;
            bool must = one_shot_target() || draw_must_happen();
            VkPipeline ready = sync_shaders() ? VK_NULL_HANDLE : warm_ready(pr->vi, pr->fi, &k.s);
            if (ready) { pp->pipe = ready; atomic_store(&pp->state, 1); atomic_fetch_add(&stat_warm, 1); }
            else if (sync_shaders() || !link_now(pp, must)) run_job(build_pipeline, pp);  // (asynchronous: a link alone is done here)
        }
        pipe_memo.k = k;
        pipe_memo.pp = pp;
    }
have_pipe:
    if (atomic_load(&pp->state) == 0 && (one_shot_target() || draw_must_happen())) wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) {
        if (st.rt.ncolor && vk_trace_tex(st.rt.color[0]))
            vk_trace_event(st.rt.color[0], 3, NULL, 0, "draw skipped: pipeline %s", pp->state ? "failed" : "compiling");
        stat_skipped++;
        return;
    }
    if (no_dyn_cache) vk_dyn_forget();
    dyn_check();
    if (!DYN_SAME(pipe, pp->pipe)) rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = profd ? SDL_GetTicksNS() : 0;
    if (profd) prof_key += prof_td_ - prof_tk_;
    ProfZone pz2 = PZ_NOW();
    PZ_SPAN("exec/draw/key", pz1, pz2);

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("SAVAGEWIND_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (!DYN_SAME(dt, (uint32_t)dt)) rec_CmdSetDepthTestEnable(cb, dt);
    if (!DYN_SAME(dw, (uint32_t)(dt && st.ds[1]))) rec_CmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). SAVAGEWIND_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("SAVAGEWIND_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!DYN_SAME(dop, (uint32_t)dop)) rec_CmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    if (!DYN_SAME(sten, (uint32_t)sten)) rec_CmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t *c = dc.sten_face[f];
            uint32_t fail = stencil_op(o & 0xF), pass = stencil_op((o >> 8) & 0xF), zfail = stencil_op((o >> 4) & 0xF);
            uint32_t cmp = compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
            if (c[0] != fail || c[1] != pass || c[2] != zfail || c[3] != cmp) {
                rec_CmdSetStencilOp(cb, face, fail, pass, zfail, cmp);
                c[0] = fail; c[1] = pass; c[2] = zfail; c[3] = cmp;
            }
            if (c[4] != (st.stencil_vmask[f] & 0xFF)) rec_CmdSetStencilCompareMask(cb, face, c[4] = st.stencil_vmask[f] & 0xFF);
            if (c[5] != (st.stencil_mask[f] & 0xFF)) rec_CmdSetStencilWriteMask(cb, face, c[5] = st.stencil_mask[f] & 0xFF);
            if (c[6] != (uint32_t)st.stencil_ref[f]) rec_CmdSetStencilReference(cb, face, c[6] = st.stencil_ref[f]);
        }
    }
    if (memcmp(dc.blend, st.blend_color, sizeof dc.blend)) {
        rec_CmdSetBlendConstants(cb, st.blend_color);
        memcpy(dc.blend, st.blend_color, sizeof dc.blend);
    }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (!DYN_SAME(cull, (uint32_t)cm)) rec_CmdSetCullMode(cb, cm);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // SAVAGEWIND_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("SAVAGEWIND_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey_flip) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (!DYN_SAME(front, (uint32_t)ff)) rec_CmdSetFrontFace(cb, ff);
    VkPrimitiveTopology pt = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (!DYN_SAME(topo, (uint32_t)pt)) rec_CmdSetPrimitiveTopology(cb, pt);
    bool restart = st.prim_restart && native && (mode == 3 || mode == 5);
    if (!DYN_SAME(restart, (uint32_t)restart)) rec_CmdSetPrimitiveRestartEnable(cb, restart);
    {   // nvnCommandBufferSetLineWidth, at the target's scale (BOTW's noise meter strokes its wave at 3 px, then 1.5 px)
        float lw = 1.0f;
        if (cls == 1 && g_vk.wide_lines) {
            const float *range = g_vk.props.limits.lineWidthRange;
            lw = st.line_width * pass_sy;
            lw = lw < range[0] ? range[0] : lw > range[1] ? range[1] : lw;
        }
        if (dc.line_w != lw) {  // (NaN after dyn_check: always set)
            rec_CmdSetLineWidth(cb, lw);
            dc.line_w = lw;
        }
    }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("SAVAGEWIND_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (!DYN_SAME(bias_on, (uint32_t)on)) rec_CmdSetDepthBiasEnable(cb, on);
        if (memcmp(dc.bias, f, sizeof f)) {
            rec_CmdSetDepthBias(cb, f[1], f[2], f[0]);
            memcpy(dc.bias, f, sizeof f);
        }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // per-draw tint (translate.c `ltint`): no game-specific highlight sets one yet
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (profd) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROFD_T0();
    ProfZone pz3 = PZ_NOW();
    PZ_SPAN("exec/draw/dyn", pz2, pz3);
    uint32_t dyn[64];
    int ndyn = 0;
    hud_clip = hud_cut = false;  // (hud_patch, while the vertex stage's constant buffers are bound)
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndyn);
    PROFD_ADD(prof_sets);
    ProfZone pz4 = PZ_NOW();
    PZ_SPAN("exec/draw/sets", pz3, pz4);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = profd ? SDL_GetTicksNS() : 0;
    sdw_fix.s = 0;
    shadow_copy_note(pr->fi);
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        // The map's animated refraction must sample the background, before the
        // interactive cursor and its labels. Sampling the finished UI produces
        // a second, displaced scanline-shaped copy of "Mark" as the cursor moves.
        if (g_cur_fs_hash == 0xe91b1fb6e1eb920bULL && i == 1 && map_background &&
            map_background_frame == renderer_present_count()) {
            fii[i].imageView = post_view(map_background);
            fii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            sc[0] = sc[1] = 1;
        }

        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
        if (sc[0] > 1 && wh[0] == wh[1] && !sdw_fix.s) {  // a shadow map made bigger (its texel size: cbuf_info)
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            if (t && vk_texture_scale_group(t) == RS_SHADOW) { sdw_fix.gw = wh[0]; sdw_fix.gh = wh[1]; sdw_fix.s = sc[0]; }
        }
    }
    up_fix.sx = 0;
    {   // a scaled scene's half-size buffers brought back by nearest depth: the texel grid and step scaled (up_fix)
        static int off = -1;
        if (off < 0) off = getenv("SAVAGEWIND_NO_UPSAMPLE_FIX") != NULL;
        bool scaled = !off && st.rt.ncolor && st.rt.color[0] && (pass_sx > 1 || pass_sy > 1);
        if (scaled && !pr->fs_gather) {  // (the first gathered texture's slot, from the translated source: 2 + slot)
            const char *p = pr->fi->msl ? strstr(pr->fi->msl, "textureGather(t") : NULL;
            int slot = p ? atoi(p + 15) : -1;
            pr->fs_gather = slot >= 0 && slot < pr->fi->ntex && slot < 64 ? (uint8_t)(2 + slot) : 1;
        }
        int gs = scaled ? pr->fs_gather - 2 : -1;
        NvnTexture *gt = gs >= 0 && fabsf(tex_sc[1][gs][0] - pass_sx) < 0.01f * pass_sx &&
                         fabsf(tex_sc[1][gs][1] - pass_sy) < 0.01f * pass_sy ? shader_texture_object(1, &pr->fi->tex[gs]) : NULL;
        TexBackend *gb = gt ? gt->backend : NULL;
        if (gb && gt->width && gt->height) {
            up_fix.gw = st.rt.color[0]->width, up_fix.gh = st.rt.color[0]->height;
            up_fix.sx = (float)gb->width / (float)gt->width, up_fix.sy = (float)gb->height / (float)gt->height;
            up_fix.tw = gt->width, up_fix.th = gt->height, up_fix.aw = gb->width, up_fix.ah = gb->height;
            ndyn = 0;  // (the vertex set again, with its constants: the first of the draw's dynamic offsets)
            vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndyn);
        }
    }
    if (profd) prof_tex += SDL_GetTicksNS() - prof_tx_;
    ProfZone pz5 = PZ_NOW();
    PZ_SPAN("exec/draw/tex", pz4, pz5);
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    // A full-screen draw into a smaller, fixed-size capture samples the reshaped final image.
    // Derive the crop from the source's guest/backing dimensions, never a window or thumbnail size.
    // Expand the viewport about its center so the target clips the original guest aspect.
    // The vertex-count guard admits simple full-screen triangles/quads; other capture meshes need
    // coverage information before opting in. Scaled blur targets and partial draws stay untouched.
    if (st.rt.ncolor == 1 && st.rt.color[0] && pr->fi->ntex == 1 && r->x[2] <= 6 &&
        vp[0] == 0 && vp[1] == 0 && vp[2] == st.rt.color[0]->width && vp[3] == st.rt.color[0]->height) {
        NvnTexture *dst = st.rt.color[0], *src = shader_texture_object(1, &pr->fi->tex[0]);
        TexBackend *d = dst->backend, *s = src ? src->backend : NULL;
        if (d && !d->scalable && pass_w == d->width && pass_h == d->height &&
            s && s->rs_group == RS_OUT && src->target == 1 && dst->target == 1 &&
            src->levels == 1 && s->samples == 1 && d->samples == 1 &&
            d->aspect == VK_IMAGE_ASPECT_COLOR_BIT && s->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
            dst->width < src->width && dst->height < src->height &&
            // A whole-pixel target can round the source aspect by up to one destination pixel.
            fabs((double)dst->width - (double)dst->height * src->width / src->height) <= 1 &&
            st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 &&
            (int64_t)st.scissor[0][0] + st.scissor[0][2] >= dst->width &&
            (int64_t)st.scissor[0][1] + st.scissor[0][3] >= dst->height) {
            float shape = (float)((double)s->width * src->height / ((double)s->height * src->width));
            float sx = vp_sx * fmaxf(shape, 1), sy = vp_sy / fminf(shape, 1);
            float w = dst->width * sx, h = dst->height * sy, x = (pass_w - w) * 0.5f, y = (pass_h - h) * 0.5f;
            const VkPhysicalDeviceLimits *lim = &g_vk.props.limits;
            if (isfinite(w) && isfinite(h) && w > 0 && h > 0 &&
                w <= lim->maxViewportDimensions[0] && h <= lim->maxViewportDimensions[1] &&
                x >= lim->viewportBoundsRange[0] && y >= lim->viewportBoundsRange[0] &&
                x + w <= lim->viewportBoundsRange[1] && y + h <= lim->viewportBoundsRange[1]) {
                vp_sx = sx; vp_sy = sy; vp_ox = x; vp_oy = y;
                if (ENV_ONCE("SAVAGEWIND_LOG_COPIES"))
                    LOG("[copy] fixed-aspect capture %ux%u backing %ux%u -> %ux%u: viewport %.3fx%.3f at %.3f,%.3f\n",
                        src->width, src->height, s->width, s->height, dst->width, dst->height, w, h, x, y);
            }
        }
    }
    {
        float k = 1.0f;  // (horizontal stretch of a camera-attached effect: none, the view keeps its 16:9 shape)
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f | depth range %.7f %.7f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy, zn, zf);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = profd ? SDL_GetTicksNS() : 0;
    ProfZone pz6 = PZ_NOW();
    // (Video > Game's FXAA: fxaa_patch; off under an upscaler that anti-aliases, rt_temporal_upscaler)
    fxaa_off = fxaa_shader(g_cur_fs_hash) && (!g_settings.game_fxaa || rt_temporal_upscaler());
    if (fxaa_off) {
        static uint64_t logged[2];
        int i = g_cur_fs_hash == logged[0] ? -1 : !logged[0] ? 0 : g_cur_fs_hash == logged[1] ? -1 : !logged[1] ? 1 : -1;
        if (i >= 0) LOG("[fxaa] the game's FXAA off (fs hash %016llx)\n", (unsigned long long)(logged[i] = g_cur_fs_hash));
    }
    VkDescriptorSet fset = stage_set_reuse(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, dyn, &ndyn);
    sdw_fix.s = 0, up_fix.sx = 0, fxaa_off = false;
    if (profd) prof_sets += SDL_GetTicksNS() - prof_t1_;
    PZ_SPAN("exec/draw/sets", pz6, PZ_NOW());
    VkDescriptorSet sets[2] = {vset, fset};
    if (ndyn > 32) ndyn = 32;
    if (dc.layout != pr->layout || dc.sets[0] != vset || dc.sets[1] != fset || dc.ndyn != (uint32_t)ndyn ||
        memcmp(dc.dyn, dyn, (size_t)ndyn * 4)) {
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, (uint32_t)ndyn, dyn);
        dc.layout = pr->layout; dc.sets[0] = vset; dc.sets[1] = fset;
        dc.ndyn = (uint32_t)ndyn;
        memcpy(dc.dyn, dyn, (size_t)ndyn * 4);
    }
    if (mvec_draw) rec_CmdPushConstantsMvec(cb, pr->layout, mvec_seq);
    if (pr->fi->hdr) {  // (live HDR tone-mapping settings: hdr.c vk_hdr_params)
        VkHdrParams hp;
        vk_hdr_params(&hp);
        rec_CmdPushConstants(cb, pr->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof hp, &hp);
    }

    uint64_t prof_t2_ = profd ? SDL_GetTicksNS() : 0;
    ProfZone pz7 = PZ_NOW();
    uint64_t prof_index_start = prof_t2_;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_fetch(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                if (profd) prof_index_direct++;
                race_probe_note(iaddr, (uint64_t)count * isz, true);
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                if (profd) prof_index_copy++;
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) max_index = scan_max_u16(src, count, st.prim_restart, (uint16_t)restart, max_index);
                else max_index = scan_max_u32(src, count, st.prim_restart, restart, max_index);
            }
        } else if (native) {  // 8-bit indices: widen to 16
            if (profd) prof_index_copy++;
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            if (profd) prof_index_copy++;
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && (dc.ibuf != ib.buf || dc.ioff != ib.off || dc.itype != (uint32_t)itype_vk)) {
        rec_CmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
        dc.ibuf = ib.buf; dc.ioff = ib.off; dc.itype = itype_vk;
    }
    if (profd) {
        uint64_t now = SDL_GetTicksNS();
        prof_indices += now - prof_index_start;
        prof_index_start = now;
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    VkBuffer vbufs[17];
    VkDeviceSize voffs[17];
    uint32_t vdirty = 0;
    for (uint32_t sm = pipe_memo.k.s.used_streams & ((1u << 17) - 1); sm; sm &= sm - 1) {
        int s = __builtin_ctz(sm);
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_fetch(v.addr, v.size, &dbuf, &doff)) {
                if (profd) prof_vertex_direct++;
                race_probe_note(v.addr, v.size, false);
                if (dc.vbuf[s] != dbuf || dc.voff[s] != doff) {
                    vbufs[s] = dbuf; voffs[s] = doff;
                    dc.vbuf[s] = dbuf; dc.voff[s] = doff;
                    vdirty |= 1u << s;
                }
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) max_index = scan_max_u16(scan_src, icount, st.prim_restart, (uint16_t)scan_restart, max_index);
                else max_index = scan_max_u32(scan_src, icount, st.prim_restart, scan_restart, max_index);
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = pipe_memo.k.s.stride[s], need;
            if (!stride) need = 256;
            else if (pipe_memo.k.s.divisor[s]) need = ((uint64_t)base_inst + inst + pipe_memo.k.s.divisor[s] - 1) / pipe_memo.k.s.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else {
                if (profd) prof_vertex_copy++;
                sp = guest_copy(v.addr, need ? need : 16);
            }
        }
        VkDeviceSize off = sp.off;
        if (dc.vbuf[s] != sp.buf || dc.voff[s] != off) {
            vbufs[s] = sp.buf; voffs[s] = off;
            dc.vbuf[s] = sp.buf; dc.voff[s] = off;
            vdirty |= 1u << s;
        }
    }
    while (vdirty) {
        int first = __builtin_ctz(vdirty), end = first;
        while ((vdirty >> end) & 1u) end++;
        uint32_t run = (uint32_t)(end - first);
        rec_CmdBindVertexBuffers(cb, (uint32_t)first, run, &vbufs[first], &voffs[first]);
        vdirty &= ~(((1u << run) - 1u) << first);
    }

    if (profd) prof_vertex_bind += SDL_GetTicksNS() - prof_index_start;
    if (profd) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    PZ_SPAN("exec/draw/vtx", pz7, PZ_NOW());
    // ---- draw (inside an occlusion query while SAMPLES_PASSED counts)
    int oq = -1;
    if (oq_counting && oq_nrun < OQ_RUN && oq_on()) {
        uint32_t i = oq_next++ % OQ_SLOTS;
        if (oq_seq[i] != vk_pending_seq()) {  // (not reused within one submission)
            if (oq_seq[i] > vk_completed_seq()) vk_wait_seq(oq_seq[i]);
            oq_seq[i] = vk_pending_seq();
            oq_scale[i] = vp_sx * vp_sy > 0 ? vp_sx * vp_sy : 1;
            rec_CmdResetQueryPool(vk_upload_cmd(), oq_pool, i, 1);
            rec_CmdBeginQuery(cb, oq_pool, i, g_vk.occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
            oq_run[oq_nrun++] = i;
            oq = (int)i;
        }
    }
    if (use_index ? icount : count) hdr_record_draw(pr);
    if (!use_index) rec_CmdDraw(cb, count, inst, first, base_inst);
    else rec_CmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    if (oq >= 0) rec_CmdEndQuery(cb, oq_pool, (uint32_t)oq);
    stat_draws++;
    draws_total++;
    chunk_draws++;
    if (pr->vi->writes_gmem || pr->fi->writes_gmem) {
        bar_other = true;
        if (pr->vi->writes_gmem) gw_note(0);
        if (pr->fi->writes_gmem) gw_note(1);
        if (ENV_ONCE("SAVAGEWIND_LOG_HOLD")) LOG("[hold] frame %llu draw %llu: vs %llx (%d) fs %llx (%d) fs hash %016llx\n",
                                               (unsigned long long)renderer_present_count(), (unsigned long long)stat_draws,
                                               (unsigned long long)st.program->stages[0].code_addr, pr->vi->writes_gmem,
                                               (unsigned long long)st.program->stages[1].code_addr, pr->fi->writes_gmem,
                                               (unsigned long long)g_cur_fs_hash);
    }
    cum_draws++;
    if (g_debug_frame) pick_after_draw();
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
    {   // (the profiler) a draw in the GPU row now open
        GpSub *g = &gp_sub[cur_sub];
        if (g->started && g->n && g->gen == prof_generation() && prof_enabled()) g->draws[g->n - 1]++;
    }
}


static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) LOG(" %ux%u/f%#x/fl%#x@%llx[%p]", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format,
                                st.rt.color[i]->flags, (unsigned long long)st.rt.color[i]->addr,
                                st.rt.color[i]->backend ? (void *)((TexBackend *)st.rt.color[i]->backend)->image : NULL);
    if (st.rt.depth) LOG(" d%#x %ux%u@%llx[%p]", st.rt.depth->format, st.rt.depth->width, st.rt.depth->height,
                         (unsigned long long)st.rt.depth->addr,
                         st.rt.depth->backend ? (void *)((TexBackend *)st.rt.depth->backend)->image : NULL);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    for (int i = 0; i < st.rt.ncolor && i < 8; i++)  // blend func per enabled target: srcC dstC srcA dstA eqC eqA
        if ((st.color_state[0] >> i) & 1)
            LOG(" | bf%d %x %x %x %x %x %x", i, st.blend[i][1], st.blend[i][2], st.blend[i][3], st.blend[i][4], st.blend[i][5], st.blend[i][6]);
    LOG(" | fs out %#x | poly3 %#x offset %g %g %g | clamp %d\n", pr->fi->out_mask, st.poly[3], st.poly_offset[0], st.poly_offset[1],
        st.poly_offset[2], st.depth_clamp);
    for (int i = 0; i < 32 && i < st.nattrib; i++)  // vertex attributes: guest format, stream (stride, divisor), offset
        if (pr->vi->attr_mask & (1u << i)) {
            uint32_t a = st.attrib[i], s = (a >> 8) & 0x1F;
            LOG("[draw]   attr%d fmt %#x stream %u (stride %u div %u) off %u\n", i, a & 0xFF, s,
                s < (uint32_t)st.nstream ? st.stream[s][0] : 0, s < (uint32_t)st.nstream ? st.stream[s][1] : 0, a >> 13);
        }
    for (int i = 0; i < st.rt.ncolor; i++)  // target views (layer / level selections)
        if (st.rt.color[i] && st.rt.has_cview[i])
            LOG("[draw]   rt%d view: set %#x levels %u+%u layers %u+%u (texture target %u depth %u)\n", i, st.rt.cview[i].setmask,
                st.rt.cview[i].base_level, st.rt.cview[i].num_levels, st.rt.cview[i].min_layer, st.rt.cview[i].num_layers,
                st.rt.color[i]->target, st.rt.color[i]->depth);
    if (getenv("SAVAGEWIND_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        if (r->x[2] <= 6 && (pr->vi->cbuf_mask & 2)) {  // vertex compiler constants (c1, after the code)
            NvnShaderStage *ss = &st.program->stages[0];
            const float *u = (const float *)(uintptr_t)(ss->code_addr + ss->const_offset);
            LOG("[draw]   vcb1:");
            for (int k = 0; k < 16 && k * 4 < (int)(ss->const_size ? ss->const_size : 16); k++) LOG(" %.9g", u[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18 && r->x[2] <= 6; c++) {  // vertex cbufs of full-screen draws (k - 3: binding k)
            if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
            LOG("[draw]   vcb%d %llx size %u:", c + 3, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
            for (int k = 0; k < 128 && k * 4 < (int)st.ubuf[0][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {  // (binding k is ubuf k - 3)
            if (!((pr->fi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c + 3, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 128 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // vertex textures (displacement, vertex animation)
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
        LOG("[draw]   vtex%d: %ux%u f%#x lv %u -> %ux%u addr %llx\n", i, t->width, t->height, t->format, b ? b->levels : 0,
            b ? b->width : 0, b ? b->height : 0, (unsigned long long)t->addr);
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s%s addr %llx [%p]\n", i, (void *)t, t->width, t->height,
            t->format, b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0,
            b && b->scalable ? " scalable" : "", t->finalized ? " FINALIZED" : "", (unsigned long long)t->addr,
            b ? (void *)b->image : NULL);
        LOG("[draw]   shape%d: target %u depth %u levels %u -> layers %u%s\n", i, t->target, t->depth, t->levels, b ? b->layers : 0,
            b && b->base_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? " (array)" : "");
        {   // its sampler and swizzle (view swizzle if the pool entry overrides it)
            const NvnTexEntry *e = shader_texture_entry(1, &pr->fi->tex[i]);
            if (e && e->has_view)
                LOG("[draw]   view%d: set %#x levels %u+%u layers %u+%u format %#x target %u\n", i, e->view.setmask, e->view.base_level,
                    e->view.num_levels, e->view.min_layer, e->view.num_layers, e->view.format, e->view.target);
            int slot = ((int)pr->fi->tex[i].handle_offset - 0x20) / 8;
            uint64_t h = (slot >= 0 && slot < 32) ? st.tex[1][slot] : 0;
            uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
            const NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
            const uint8_t *sw = e && e->has_view && (e->view.setmask & 8) ? e->view.swizzle : t->swizzle;
            LOG("[draw]   smp%d: type %d swz %u%u%u%u%s h %llx tsc %u", i, pr->fi->tex[i].type, sw[0], sw[1], sw[2], sw[3],
                e && e->has_view ? " (view)" : "", (unsigned long long)h, tsc);
            if (sd) LOG(" filt %u/%u wrap %u %u %u lod %g..%g bias %g aniso %g cmp %u/%u", sd->min_filter, sd->mag_filter,
                        sd->wrap[0], sd->wrap[1], sd->wrap[2], sd->lod_min, sd->lod_max, sd->lod_bias, sd->max_aniso,
                        sd->compare_mode, sd->compare_func);
            LOG("\n");
        }
    }
    {   // SAVAGEWIND_DEBUG_TEXDUMP=<fs>[,...] (see FsSel): the first such draw of a debug frame saves its fragment
        // and vertex textures (build/dtex_<hash>_<i>... / _v<i>..., see debug_save_channels; depth as .pgm) and logs
        // its vertex layout, its first vertices and its uniform buffers
        static FsSel want[8];
        static uint64_t done[8];
        static int nwant = -1;
        static int all = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_ALL=1: every such draw also saves its textures whole (RGB),
                              // build/dtex_<hash>_d<draw>_<i>.png, <draw> counting the frame's draws as [pick] does
        if (nwant < 0) nwant = fs_sel_parse(getenv("SAVAGEWIND_DEBUG_TEXDUMP"), want, 8);
        if (all < 0) all = getenv("SAVAGEWIND_DEBUG_TEXDUMP_ALL") != NULL;
        for (int k = 0; k < nwant && all; k++) {
            if (!fs_sel_match(&want[k], st.program->stages[1].code_addr, g_cur_fs_hash)) continue;
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                if (!b || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_d%u_%d.png", (unsigned long long)g_cur_fs_hash, pick.draws + 1, i);
                renderer_save_texture_tex(b, path);
                LOG("[draw]   tex%d %ux%u f%#x saved as %s\n", i, t->width, t->height, t->format, path);
            }
        }
        static long only_draw = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_DRAW=<n>: that draw (numbered as [pick] does), not the first
        if (only_draw < 0) only_draw = getenv("SAVAGEWIND_DEBUG_TEXDUMP_DRAW") ? atol(getenv("SAVAGEWIND_DEBUG_TEXDUMP_DRAW")) : 0;
        static long nth = -1;  // SAVAGEWIND_DEBUG_TEXDUMP_NTH=<n>: the n-th draw of that shader in the frame (from 1)
        if (nth < 0) nth = getenv("SAVAGEWIND_DEBUG_TEXDUMP_NTH") ? atol(getenv("SAVAGEWIND_DEBUG_TEXDUMP_NTH")) : 0;
        static uint64_t seen_frame[8];
        static long seen[8];
        for (int k = 0; k < nwant; k++) {
            if (!fs_sel_match(&want[k], st.program->stages[1].code_addr, g_cur_fs_hash) || done[k] == renderer_present_count() + 1)
                continue;
            if (only_draw && pick.draws + 1 != (uint32_t)only_draw) continue;
            if (seen_frame[k] != renderer_present_count() + 1) seen_frame[k] = renderer_present_count() + 1, seen[k] = 0;
            if (nth && ++seen[k] != nth) continue;
            done[k] = renderer_present_count() + 1;
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                if (!b) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_%d.%s", (unsigned long long)g_cur_fs_hash, i,
                         b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? "pgm" : "png");
                vk_end_pass();
                if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
                    snprintf(path, sizeof path, "build/gpu3d_%llu_%ux%ux%u_f%x.raw", (unsigned long long)t->debug_id, b->width,
                             b->height, b->depth, t->format);
                    debug_save_3d(b, path);
                    continue;
                }
                if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) debug_save_depth(b, path);
                else debug_save_channels(b, path);
                LOG("[draw]   tex%d saved as %s\n", i, path);
            }
            for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // the vertex shader's textures too (vtex<i>)
                NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
                TexBackend *b = t ? (TexBackend *)t->backend : NULL;
                const NvnTexEntry *e = shader_texture_entry(0, &pr->vi->tex[i]);
                if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
                LOG("[draw]   vtex%d: %ux%ux%u f%#x target %u levels %u flags %#x stride %u addr %llx%s", i, t->width, t->height, t->depth,
                    t->format, t->target, t->levels, t->flags, (unsigned)t->stride, (unsigned long long)t->addr, t->finalized ? " FINALIZED" : "");
                if (e && e->has_view)
                    LOG(" view set %#x levels %u+%u layers %u+%u format %#x", e->view.setmask, e->view.base_level, e->view.num_levels,
                        e->view.min_layer, e->view.num_layers, e->view.format);
                {
                    int slot = ((int)pr->vi->tex[i].handle_offset - 0x20) / 8;
                    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[0][slot] : 0;
                    uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
                    const NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                    LOG(" | type %d h %llx tsc %u", pr->vi->tex[i].type, (unsigned long long)h, tsc);
                    if (sd) LOG(" filt %u/%u wrap %u %u %u lod %g..%g", sd->min_filter, sd->mag_filter, sd->wrap[0], sd->wrap[1],
                                sd->wrap[2], sd->lod_min, sd->lod_max);
                }
                LOG("\n");
                if (!b) continue;
                char path[128];
                snprintf(path, sizeof path, "build/dtex_%016llx_v%d.%s", (unsigned long long)g_cur_fs_hash, i,
                         b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? "pgm" : "png");
                vk_end_pass();
                if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) debug_save_depth(b, path);
                else debug_save_channels(b, path);
            }
            LOG("[draw]   attribs:");  // the vertex layout: format / stream / offset, and each stream's first bytes
            for (int a = 0; a < 16; a++)
                if ((pr->vi->attr_mask >> a) & 1)
                    LOG(" a%d=%#x/s%u/+%u", a, a < st.nattrib ? st.attrib[a] & 0xFF : 0, a < st.nattrib ? (st.attrib[a] >> 8) & 0x1F : 99,
                        a < st.nattrib ? st.attrib[a] >> 13 : 0);
            LOG("\n");
            for (int s = 0; s < st.nstream && s < 16; s++) {
                if (!st.vbuf[s].addr) continue;
                const float *f = (const float *)(uintptr_t)st.vbuf[s].addr;
                LOG("[draw]   stream%d stride %u divisor %u %llx size %llu:", s, st.stream[s][0], st.stream[s][1],
                    (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
                for (uint64_t k = 0; k < 64 && k * 4 < st.vbuf[s].size; k++) LOG("%s%.6g", k % 8 ? " " : "\n[draw]     ", f[k]);
                LOG("\n");
            }
            if (indexed) {
                LOG("[draw]   indices (type %llu, base vertex %lld):", (unsigned long long)r->x[1], (long long)(int32_t)r->x[4]);
                for (uint64_t k = 0; k < r->x[2] && k < 64; k++)
                    LOG(" %u", r->x[1] == 0 ? ((const uint8_t *)(uintptr_t)r->x[3])[k] : r->x[1] == 1 ? ((const uint16_t *)(uintptr_t)r->x[3])[k]
                                                                                       : ((const uint32_t *)(uintptr_t)r->x[3])[k]);
                LOG("\n");
            }
            for (uint64_t k = 0; k < r->x[2] && k < 24; k++) {  // the first vertices' attributes, raw bytes in hex
                uint64_t v = indexed ? (r->x[1] == 0 ? ((const uint8_t *)(uintptr_t)r->x[3])[k] : r->x[1] == 1 ? ((const uint16_t *)(uintptr_t)r->x[3])[k]
                                                                                                 : ((const uint32_t *)(uintptr_t)r->x[3])[k]) + (int32_t)r->x[4]
                                     : r->x[1] + k;
                LOG("[draw]   vertex %llu:", (unsigned long long)v);
                for (int a = 0; a < 16 && a < st.nattrib; a++) {
                    if (!((pr->vi->attr_mask >> a) & 1)) continue;
                    uint32_t fmt = st.attrib[a] & 0xFF, s = (st.attrib[a] >> 8) & 0x1F, off = st.attrib[a] >> 13;
                    if (s >= 16 || !st.vbuf[s].addr) continue;
                    uint32_t bpb = nvn_format_info(fmt)->bpb;
                    uint64_t at = v * st.stream[s][0] + off;
                    if (at + bpb > st.vbuf[s].size) { LOG(" a%d=(out)", a); continue; }
                    const uint8_t *p = (const uint8_t *)(uintptr_t)(st.vbuf[s].addr + at);
                    LOG(" a%d=", a);
                    for (uint32_t b = 0; b < bpb && b < 16; b++) LOG("%02x", p[b]);
                }
                LOG("\n");
            }
            for (int sg = 0; sg < 2; sg++)  // and every uniform buffer either stage reads, whole (up to 4 KiB)
                for (int c = 0; c < 18; c++) {
                    ShaderInfo *si = sg ? pr->fi : pr->vi;
                    Range u = st.ubuf[sg][c];
                    if (!((si->cbuf_mask >> (c + 3)) & 1) || !u.addr) continue;
                    const float *f = (const float *)(uintptr_t)u.addr;
                    LOG("[draw]   %ccb%d %llx size %u:", sg ? 'f' : 'v', c + 3, (unsigned long long)u.addr, (unsigned)u.size);
                    for (uint32_t k = 0; k < 1024 && k * 4 < u.size; k++)
                        LOG("%s%.6g", k % 16 == 0 ? "\n[draw]     " : k % 4 ? " " : " | ", f[k]);
                    LOG("\n");
                }
            for (int sg = 0; sg < 2; sg++) {  // the compiler constants (c1) of either stage, exact
                const NvnShaderStage *ss = &st.program->stages[sg];
                if (!ss->code_addr || !ss->const_size) continue;
                const uint32_t *u = (const uint32_t *)(uintptr_t)(ss->code_addr + ss->const_offset);
                LOG("[draw]   %ccb1 size %u:", sg ? 'f' : 'v', ss->const_size);
                for (uint32_t k = 0; k < 256 && k * 4 < ss->const_size; k++) LOG(" %08x", u[k]);
                LOG("\n");
            }
        }
    }
}

