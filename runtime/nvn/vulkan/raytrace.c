// Ray tracing, phase 1: see raytrace.h and docs/research/ray-tracing.md.
//
// Each triangle list of the scene's G-buffer pass is drawn with a capture program (its vertex shader also writes the
// positions, raytrace.h), every frame, into the frame's space: relative to the scene camera's position and along the
// world's axes, so no precision is lost far from the world's origin. A compute pass then traces a ray a pixel against
// the frame's structure and compares what it hits with the game's depth buffer.
//
// The structure (SAVAGEWIND_RT_MODE):
//   1 (default): a bottom-level structure a mesh, kept from frame to frame and refitted to the frame's positions, and a
//      top-level one of them all. A mesh is a draw's index range and vertex streams, and which of the frame's draws of
//      those it is (ord): no draw is matched with "the same object" of the frame before and nothing is told to be
//      static. A refit only needs the same triangles, and the same mesh somewhere else is as good a fit as before.
//   2: the same, every mesh built from nothing every frame (what the refit saves).
//   0: one bottom-level structure of everything, built from nothing every frame (the first version: the reference).
//
// Threads: everything here runs on the thread that translates the game's command lists, but flush_rec, which the
// recorder (record.c) calls in its turn.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_timer.h>

#include "../../rt.h"
#include "../../settings.h"
#include "raytrace.h"
#include "mvec.h"
#include "record.h"
#include "vk13.h"

#define VCAP (4u << 20)   // vertices a frame
#define ICAP (12u << 20)  // indices a frame
#define MAX_GEO 16384
#define NIDX 3            // index buffers: the CPU fills one while the GPU may still read the last frame's
#define NRES 8            // results in flight
#define TRASH 0x7F000000  // rc_off of a draw that was not captured: past the buffer (the program clamps to its last slot)

typedef struct { VkBuffer buf; VkDeviceMemory mem; uint64_t size, addr; void *ptr; } Buf;
#define MESH_CAP (1u << 16)      // slots of the mesh table (half used at most)
#define PIDX_CAP (32u << 20)     // indices kept for the meshes
#define AS_ARENA (384ull << 20)  // bytes of bottom-level structures
#define SCRATCH_M (96ull << 20)  // build scratch a frame (mode 1 / 2)

typedef struct { uint32_t vfirst, nverts, ifirst, ntris; uint8_t opaque, kind, cull; struct Mesh *mesh; } Geo;
// A mesh's structure (mode 1 / 2). Its indices are copied once (rebased to its lowest); the draw is told apart from a
// different mesh at the same address by a hash of some of them.
typedef struct Mesh {
    uint64_t key;  // rtr_capture's geo and ord mixed; 0: free
    VkAccelerationStructureKHR as;
    uint64_t as_addr, as_size, build_scratch, update_scratch, ihash;
    uint64_t seen, drawn;    // the last frame a draw asked for it / it was built or refitted
    uint64_t as_off, as_room;  // its structure's place in the arena
    uint32_t ifirst, count, lo, nverts;  // (count, nverts: of all its instances)
    uint32_t stride;                     // vertices an instance
    uint8_t opaque, built;
} Mesh;
typedef struct {
    uint64_t seq, frame, fs, blas_bytes, scratch_bytes;
    uint32_t ngeo, tris, verts, ncam, tris_shadow, n_built, tris_built, n_wait, meshes, resets;
    int slot;
    bool shot, used, sun;
    uint32_t w, h;
} Res;

static struct {
    PFN_vkCreateAccelerationStructureKHR create;
    PFN_vkDestroyAccelerationStructureKHR destroy;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
    PFN_vkCmdBuildAccelerationStructuresKHR build;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_addr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR props;
    PFN_vkGetBufferDeviceAddress buf_addr;
} fn;
static int wanted = -1;
static bool dev_ok, inited;
static Buf cap, idx[NIDX], blas_buf, tlas_buf, scratch, inst_buf, stats, stats_host, shot_host;
static uint64_t idx_seq[NIDX];
static int ring;
static VkAccelerationStructureKHR blas, tlas;
static Geo *geos;
static uint32_t ngeo, vcur, icur, ncam, tris_shadow;
static uint64_t cur_frame = ~0ull, flushed_frame = ~0ull, n_flush, n_full, n_dropped, n_badcam;
static bool have_origin, have_scene;
static double origin[3];
static float scene_m[16];
static const float *memo_cam;
static float memo_blk[44], memo_m[16], scene_p[2];
static int memo_kind;
static bool have_eye;
static double eye_rt[9], eye_pos[3];
static bool memo_ok;
static VkDescriptorPool dpool;
static VkDescriptorSetLayout dlayout;
static VkPipelineLayout playout;
static VkPipeline pipe;
static VkDescriptorSet dsets[NRES];
static VkImage img;
static VkDeviceMemory img_mem;
static VkImageView img_view;
static uint32_t img_w, img_h;
static VkQueryPool qpool, cpool;
static Res res[NRES];
static int res_next;
static int shot_every, log_every = 60, fast_trace, compact;
static struct { double blas, tlas, trace; uint64_t n; } tsum;
static int mode = 1;
// The traced sun shadow mask, and the direction to the sun in the frame's space (from a shadow map's camera)
// (mask: what the rays give, with the view depth in .a; maskf: that filtered, what the game's passes read)
static VkImage mask, maskf, maskh;  // (maskh: the frame before's maskf, for the filter's accumulation)
static VkDeviceMemory mask_mem, maskf_mem, maskh_mem;
static VkImageView mask_view, maskf_view, maskh_view;
static VkPipeline blur_pipe;
static VkDescriptorSet bsets[NRES];
static uint32_t mask_w, mask_h;
static VkPipeline shadow_pipe;
static VkDescriptorSet ssets[NRES];
static float sun[3];
static bool have_sun;
static struct { double t; uint64_t n; } ssum;
// Reflections: the traced colours (refl), the previous frame's lit scene (prev) and where that frame's camera was
static VkImage refl, prev_img, radd, rhist;  // (rhist: the frame before's reflections, for their accumulation)
static VkDeviceMemory refl_mem, prev_mem, radd_mem, rhist_mem;
static VkImageView refl_view, prev_view, radd_view, rhist_view, albedo_next;
static uint32_t prev_w, prev_h;
static VkFormat prev_fmt;
static uint64_t prev_img_frame = ~0ull, prev_cam_frame = ~1ull;

static double prev_rt[9], prev_pos[3];
static float prev_proj[16], scene_proj[16];
static VkDescriptorSetLayout rlayout;
static VkPipelineLayout rplayout;
static VkPipeline refl_pipe;
static VkDescriptorSet rsets[NRES];
// The colour cache (the reflections of what is not on screen): CC_K cells a triangle of every kept mesh, at the mesh's
// place in the kept indices (a third of it: the instance's custom index), each the lit colour the triangle's part had
// when it was last seen on screen. fresh: the ranges of the meshes made since the last flush, cleared there.
#define CC_K 4
static Buf ccache;
static VkPipeline fill_pipe;
static uint32_t *fresh, n_fresh, fresh_cap;
static bool cc_all = true;
static bool late_pending, hist_pending;
static uint32_t flush_ninst;     // the instances of the frame's top level so far (the late draws' follow)
static uint64_t late_frame = ~8ull;  // the last frame the reflections were traced again
static int cc_on = -1;  // SAVAGEWIND_RT_REFL_CACHE=0: none (only what is on screen is reflected)
static struct { uint32_t draws, verts; } late, late_log;  // scene draws after the frame's structure was built
static uint64_t cpu_ns, cpu_ns_log;  // the translating thread's time in here this frame (captures and the flush)
static Mesh *meshes;
static uint32_t n_meshes, n_tomb, pidx_cur, n_resets, scratch_align = 256, sweep_at;
static uint64_t as_cur, as_used, n_evicted;
#define TOMB 2ull          // (a key: a mesh let go; real keys are odd)
#define KEEP_FRAMES 240    // a mesh no draw asked for in this many frames is let go
// Room let go in the two arenas, taken again first-fit (a mesh that comes back asks for the sizes it had)
typedef struct { uint64_t off, size; } Hole;
static Hole *holes[2];  // [0] structures (bytes), [1] indices
static uint32_t n_holes[2], holes_cap[2];
static Buf pidx, as_arena, inst_ring[NIDX];
static struct { uint64_t geo, frame; uint32_t n; } *ords;  // how many draws of a mesh so far this frame
static struct { VkAccelerationStructureKHR as; uint64_t seq; } *dead;  // replaced structures, destroyed once unused
static uint32_t n_dead, dead_cap;
static uint32_t build_budget = 600000;  // triangles built from nothing a frame (mode 1); the rest wait a frame

// Video > Ray tracing and its rows (settings.h rt...). SAVAGEWIND_RT set (testing): the variables alone say what is
// on, as before there were settings.
static bool from_env(void) { return getenv("SAVAGEWIND_RT") != NULL; }
bool rtr_wanted(void) {
    if (wanted < 0) { const char *e = getenv("SAVAGEWIND_RT"); wanted = e ? atoi(e) > 0 : g_settings.rt; }
    return wanted;
}
static bool supported;
void rtr_set_supported(bool can) { supported = can; }
bool rtr_supported(void) { return supported; }
bool rtr_on(void) { return dev_ok; }
static int shadow_mode = -1;  // SAVAGEWIND_RT_SHADOWS
static int shadow_env(void) {
    if (shadow_mode < 0) { const char *e = getenv("SAVAGEWIND_RT_SHADOWS"); shadow_mode = e ? atoi(e) : !from_env() && g_settings.rt_shadows ? 4 : 0; }
    return shadow_mode;
}
bool rtr_reflections(void) {
    static int v = -1;
    if (v < 0) v = getenv("SAVAGEWIND_RT_REFLECT") ? atoi(getenv("SAVAGEWIND_RT_REFLECT")) > 0 : !from_env() && g_settings.rt_reflections;
    return dev_ok && mode && v;
}
int rtr_shadows(void) { return !dev_ok || !mode || !shadow_env() ? 0 : shadow_env() == 2 ? 2 : 1; }
bool rtr_shadow_passes(void) {
    static int v = -1;
    if (v < 0) v = getenv("SAVAGEWIND_RT_SHADOW") != NULL || rtr_shadows();
    return v;
}
bool rtr_pending(void) { return ngeo != 0 && flushed_frame != cur_frame; }

static struct { uint32_t draws, verts, dw; uint64_t fs; } left[7], left_log[7];
void rtr_left_out(int why, uint32_t count, bool depth_write, uint64_t fs) {
    left[why].draws++, left[why].verts += count, left[why].dw += depth_write;
    if (depth_write || !left[why].fs) left[why].fs = fs;
}

void rtr_device_ready(bool have) {
    if (!have) return;
#define LOAD(field, name) fn.field = (PFN_##name)vkGetDeviceProcAddr(g_vk.dev, #name)
    LOAD(create, vkCreateAccelerationStructureKHR);
    LOAD(destroy, vkDestroyAccelerationStructureKHR);
    LOAD(sizes, vkGetAccelerationStructureBuildSizesKHR);
    LOAD(build, vkCmdBuildAccelerationStructuresKHR);
    LOAD(as_addr, vkGetAccelerationStructureDeviceAddressKHR);
    LOAD(props, vkCmdWriteAccelerationStructuresPropertiesKHR);
    LOAD(buf_addr, vkGetBufferDeviceAddress);
    if (!fn.buf_addr) fn.buf_addr = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g_vk.dev, "vkGetBufferDeviceAddressKHR");
#undef LOAD
    dev_ok = fn.create && fn.destroy && fn.sizes && fn.build && fn.as_addr && fn.props && fn.buf_addr;
    const char *e;
    if ((e = getenv("SAVAGEWIND_RT_SHOT"))) shot_every = atoi(e);
    if ((e = getenv("SAVAGEWIND_RT_LOG")) && atoi(e) > 0) log_every = atoi(e);
    fast_trace = (e = getenv("SAVAGEWIND_RT_BUILD")) && !strcmp(e, "trace");
    compact = getenv("SAVAGEWIND_RT_COMPACT") != NULL;
    if ((e = getenv("SAVAGEWIND_RT_MODE"))) mode = atoi(e);
    if ((e = getenv("SAVAGEWIND_RT_BUILD_BUDGET")) && atoi(e) > 0) build_budget = (uint32_t)atoi(e);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ap};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (ap.minAccelerationStructureScratchOffsetAlignment > scratch_align) scratch_align = ap.minAccelerationStructureScratchOffsetAlignment;
    // (the translator's setup ran before the device was made: the game's shadow passes' variant is chosen here,
    //  before any shader is translated or read from the cache)
    maxwell_rt_shadow = rtr_shadows();
    // SAVAGEWIND_RT_LIGHT_SMOOTH=0: the light buffer as the game reads it (shader.h maxwell_rt_lb)
    maxwell_rt_lb = maxwell_rt_shadow && !(getenv("SAVAGEWIND_RT_LIGHT_SMOOTH") && !atoi(getenv("SAVAGEWIND_RT_LIGHT_SMOOTH")));
    maxwell_rt_refl = !rtr_reflections() ? 0 : getenv("SAVAGEWIND_RT_REFLECT") ? atoi(getenv("SAVAGEWIND_RT_REFLECT")) : 1;
    // SAVAGEWIND_RT_REFL_MAX=<0..1>: the most of a sharp reflection lookup the traced colour takes (0.95: still water a mirror)
    if (getenv("SAVAGEWIND_RT_REFL_MAX")) snprintf(maxwell_rt_refl_max, sizeof maxwell_rt_refl_max, "%.3ff", fmin(fmax(atof(getenv("SAVAGEWIND_RT_REFL_MAX")), 0.0), 1.0));
    maxwell_rt_radd = !maxwell_rt_refl ? 0 : getenv("SAVAGEWIND_RT_REFL_ADD") ? (float)atof(getenv("SAVAGEWIND_RT_REFL_ADD")) : 0.5f;
    LOG("[rt] ray tracing %s (mode %d, build: %s%s; shadow passes %s)\n", dev_ok ? "on" : "off: a command is missing", mode,
        fast_trace ? "fast trace" : "fast build", compact ? ", compaction size asked" : "", rtr_shadow_passes() ? "captured" : "not captured");
}

// ---- memory (its own allocations: buffers whose addresses are taken need memory made for that)
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

static void buf_free(Buf *b) {
    if (!b->buf) return;
    vkDestroyBuffer(g_vk.dev, b->buf, NULL);
    vkFreeMemory(g_vk.dev, b->mem, NULL);
    *b = (Buf){0};
}

static void buf_make(Buf *b, uint64_t size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &b->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, b->buf, &mr);
    uint32_t t = ~0u;
    if (host) {
        t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t == ~0u) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else {
        t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (t == ~0u) FATAL("[rt] no memory type for a ray tracing buffer\n");
    VkMemoryAllocateFlagsInfo fl = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fl, .allocationSize = mr.size, .memoryTypeIndex = t};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &b->mem));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, b->buf, b->mem, 0));
    b->size = size;
    b->ptr = NULL;
    if (host) VK_CHECK(vkMapMemory(g_vk.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr));
    VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->buf};
    b->addr = fn.buf_addr(g_vk.dev, &da);
}

// Everything submitted so far is done on the GPU (before something it may use is destroyed; what is recorded for the
// pending submission and not submitted must not use it)
static void wait_idle(void) {
    uint64_t s = vk_pending_seq();
    if (s > 1) vk_wait_seq(s - 1);
}

static const char *k_check_src =
    "#version 460\n"
    "#extension GL_EXT_ray_query : require\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;\n"
    "layout(set = 0, binding = 1) uniform sampler2D depth_tex;\n"
    "layout(set = 0, binding = 2, rgba8) uniform writeonly image2D out_img;\n"
    "layout(set = 0, binding = 3, std430) buffer Stats { uint st[16]; };\n"
    "layout(push_constant, std430) uniform PC { float m[16]; ivec2 size; int flip_y; int depth01; float p22; float p23; };\n"
    // (the point of the pixel n at clip w, as the capture programs place their vertices; the view depth of an NDC z)
    "vec3 point(vec2 n, float w) {\n"
    "  vec4 c = vec4(n * w, 0.0, w);\n"
    "  return vec3(dot(vec4(m[0], m[1], m[2], m[3]), c), dot(vec4(m[4], m[5], m[6], m[7]), c),\n"
    "              dot(vec4(m[8], m[9], m[10], m[11]), c)) + vec3(m[12], m[13], m[14]);\n"
    "}\n"
    "float clip_w(float z) { return p23 / (z + p22); }\n"
    "bool trace(vec3 o, vec3 d, uint flags, out float t, out uint id) {\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, tlas, flags | gl_RayFlagsCullBackFacingTrianglesEXT, 1u, o, 0.0, d, 1.0);\n"
    "  while (rayQueryProceedEXT(q)) {}\n"
    "  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) { t = 1.0; id = 0u; return false; }\n"
    "  t = rayQueryGetIntersectionTEXT(q, true);\n"
    "  id = uint(rayQueryGetIntersectionGeometryIndexEXT(q, true)) * 2654435761u + uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)) * 40503u;\n"
    "  return true;\n"
    "}\n"
    // (an alpha-tested triangle on the ray where the depth buffer says: the pixel is one its texture kept, behind
    //  others whose texture let it through)
    "bool alpha_at(vec3 o, vec3 d, vec3 pg, float tol) {\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, tlas, gl_RayFlagsCullOpaqueEXT | gl_RayFlagsCullBackFacingTrianglesEXT, 1u, o, 0.0, d, 1.0);\n"
    "  bool found = false;\n"
    "  while (rayQueryProceedEXT(q)) {\n"
    "    if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;\n"
    "    if (length(o + d * rayQueryGetIntersectionTEXT(q, false) - pg) / tol < 0.02) found = true;\n"
    "  }\n"
    "  return found;\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (any(greaterThanEqual(p, size))) return;\n"
    "  float d = texelFetch(depth_tex, p, 0).r;\n"
    "  vec2 n = (vec2(p) + 0.5) / vec2(size) * 2.0 - 1.0;\n"
    "  if ((flip_y & 1) != 0) n.y = -n.y;\n"
    "  vec3 p0 = point(n, clip_w(-1.0)), p1 = point(n, clip_w(1.0));\n"
    "  vec3 pg = point(n, clip_w(d * 2.0 - 1.0));\n"
    "  vec3 dir = p1 - p0;\n"
    "  float ta, to; uint ia, io;\n"
    "  bool ha = trace(p0, dir, gl_RayFlagsOpaqueEXT, ta, ia);\n"             // every triangle
    "  bool ho = trace(p0, dir, gl_RayFlagsCullNoOpaqueEXT, to, io);\n"       // without the alpha-tested ones
    "  bool sky = d >= 0.9999999;\n"
    "  float dg = length(pg - p0), tol = max(dg, 0.25);\n"
    "  float ea = length(p0 + dir * ta - pg) / tol, eo = length(p0 + dir * to - pg) / tol;\n"
    "  atomicAdd(st[0], 1u);\n"
    "  if (d <= 0.0) atomicAdd(st[2], 1u);\n"
    "  vec3 col;\n"
    "  if (sky) {\n"
    "    atomicAdd(st[1], 1u);\n"
    "    if (ho) { atomicAdd(st[9], 1u); col = vec3(1.0, 0.5, 0.0); } else col = vec3(0.25);\n"
    "  } else if (ha && ea < 0.005) { atomicAdd(st[3], 1u); col = vec3(0.1, 0.9, 0.1);\n"
    "  } else if (ha && ea < 0.02) { atomicAdd(st[4], 1u); col = vec3(0.5, 0.9, 0.1);\n"
    "  } else if (ho && eo < 0.02) { atomicAdd(st[5], 1u); col = vec3(0.1, 0.8, 0.8);\n"
    "  } else if (alpha_at(p0, dir, pg, tol)) { atomicAdd(st[10], 1u); col = vec3(0.1, 0.5, 0.6);\n"
    "  } else if (!ha) { atomicAdd(st[6], 1u); col = vec3(1.0, 0.0, 0.0);\n"
    "  } else if (length(dir * ta) < dg) { atomicAdd(st[7], 1u); col = vec3(1.0, 0.0, 1.0);\n"
    "  } else { atomicAdd(st[8], 1u); col = vec3(0.1, 0.2, 1.0); }\n"
    "  float shade = ha ? 0.45 + 0.55 * float((ia >> 8) & 255u) / 255.0 : 1.0;\n"
    "  int view = flip_y >> 8;\n"
    "  if (view == 1) { col = vec3(fract(log2(max(dg, 0.01))), sky ? 1.0 : 0.0, 0.0); shade = 1.0; }\n"
    "  if (view == 2) { col = vec3(fract(log2(max(length(dir * ta), 0.01))), ha ? 0.0 : 1.0, 0.0); shade = 1.0; }\n"
    "  imageStore(out_img, p, vec4(col * shade, 1.0));\n"
    "}\n";

// The sun shadow mask: for every pixel of the game's shadow pass, the point the depth buffer has there and a ray from
// it rays to the sun, spread over its disc. Pushed: the check's (size: the mask's), then vec4 sun (.w: the disc's half
// angle), ivec2 depth size, int all (1: every mesh casts; 0: only what has no alpha test; 2: as 0, and the rays that
// cross an alpha-tested mesh are counted in .b), int rays.
static const char *k_shadow_src =
    "#version 460\n"
    "#extension GL_EXT_ray_query : require\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;\n"
    "layout(set = 0, binding = 1) uniform sampler2D depth_tex;\n"
    "layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D out_img;\n"
    "layout(push_constant, std430) uniform PC { float m[16]; ivec2 size; int flip_y; int depth01; float p22; float p23; vec4 sun;\n"
    "                                            ivec2 dsize; int all; int rays; float far_t; };\n"
    "vec3 point(vec2 n, float w) {\n"
    "  vec4 c = vec4(n * w, 0.0, w);\n"
    "  return vec3(dot(vec4(m[0], m[1], m[2], m[3]), c), dot(vec4(m[4], m[5], m[6], m[7]), c),\n"
    "              dot(vec4(m[8], m[9], m[10], m[11]), c)) + vec3(m[12], m[13], m[14]);\n"
    "}\n"
    "float clip_w(float z) { return p23 / (z + p22); }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (any(greaterThanEqual(p, size))) return;\n"
    "  vec2 uv = (vec2(p) + 0.5) / vec2(size);\n"
    "  float d = texelFetch(depth_tex, min(ivec2(uv * vec2(dsize)), dsize - 1), 0).r;\n"
    "  if (d >= 0.9999999) { imageStore(out_img, p, vec4(1.0, 1.0, 0.0, 60000.0)); return; }\n"
    "  vec2 n = uv * 2.0 - 1.0;\n"
    "  if ((flip_y & 1) != 0) n.y = -n.y;\n"
    "  vec3 eye = vec3(m[12], m[13], m[14]);\n"
    "  vec3 pg = point(n, clip_w(d * 2.0 - 1.0));\n"
    "  float dist = length(pg - eye);\n"
    // (off the surface: towards the eye by what the depth buffer's steps are worth there, and a little up the light)
    "  vec3 o = pg + normalize(eye - pg) * (0.02 + 0.004 * dist) + sun.xyz * (0.02 + 0.002 * dist);\n"
    "  vec3 tx = normalize(cross(sun.xyz, abs(sun.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), ty = cross(sun.xyz, tx);\n"
    "  float rot = fract(52.9829189 * fract(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));\n"  // (interleaved gradient noise)
    // (rays: the count, and above its low byte a step that goes up a frame: the pattern turns, and the filter's
    //  accumulation over the frames takes it out)
    "  int nr = rays & 255;\n"
    "  rot = fract(rot + float(rays >> 8) * 0.618034);\n"
    "  uint flags = gl_RayFlagsTerminateOnFirstHitEXT | (all == 1 ? gl_RayFlagsOpaqueEXT : all == 0 ? gl_RayFlagsCullNoOpaqueEXT : 0u);\n"
    "  float lit = 0.0, thru = 0.0;\n"
    "  for (int i = 0; i < nr; i++) {\n"
    "    float a = 6.2831853 * (rot + float(i) * 0.618034), r = sun.w * sqrt((float(i) + 0.5) / float(nr));\n"
    "    vec3 dir = normalize(sun.xyz + (tx * cos(a) + ty * sin(a)) * r);\n"
    "    rayQueryEXT q;\n"
    "    rayQueryInitializeEXT(q, tlas, flags, uint(depth01), o, float((flip_y >> 8) & 0xFFF) * 0.01 + float(flip_y >> 20) * 0.001 * dist, dir, far_t);\n"
    "    bool cand = false;\n"
    // (an alpha-tested triangle on the way: the ray ends there and the cascades, which hold every caster, decide)
    "    while (rayQueryProceedEXT(q)) { cand = true; rayQueryTerminateEXT(q); }\n"
    "    if (cand) { lit += 1.0; thru += 1.0; }\n"
    "    else if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) lit += 1.0;\n"
    "  }\n"
    "  imageStore(out_img, p, vec4(lit / float(nr), 1.0, thru / float(nr), min(clip_w(d * 2.0 - 1.0), 60000.0)));\n"
    "}\n";

// The mask filtered: 5x5, binomial, among the texels at about the same depth (the rays' few steps into a soft edge,
// without a near thing's shadow bleeding onto what is behind it)
static const char *k_blur_src =
    "#version 460\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 1) uniform sampler2D src;\n"
    "layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D out_img;\n"
    "layout(set = 0, binding = 4) uniform sampler2D hist;\n"
    "layout(push_constant, std430) uniform PC { float m[16]; ivec2 size; int flip_y; int acc; float p22; float p23; float pm[16]; };\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (any(greaterThanEqual(p, size))) return;\n"
    "  vec4 c = texelFetch(src, p, 0);\n"
    "  const float g[3] = float[3](0.375, 0.25, 0.0625);\n"
    "  vec2 s = vec2(0.0), lo = c.rb, hi = c.rb;\n"
    "  float ws = 0.0;\n"
    "  for (int dy = -2; dy <= 2; dy++)\n"
    "    for (int dx = -2; dx <= 2; dx++) {\n"
    "      vec4 t = texelFetch(src, clamp(p + ivec2(dx, dy), ivec2(0), size - 1), 0);\n"
    "      float w = g[abs(dx)] * g[abs(dy)] * exp(-abs(t.a - c.a) / (0.03 * c.a + 0.05));\n"
    "      s += t.rb * w;\n"
    "      ws += w;\n"
    "      if (w > 0.001) { lo = min(lo, t.rb); hi = max(hi, t.rb); }\n"
    "    }\n"
    "  vec4 o = vec4(s.x / ws, c.g, s.y / ws, c.a);\n"
    // (over the frames: a fifth of this frame's on what this point of the surface had, found where it was on screen
    //  the frame before (pm) and taken only if what was there was as far from that eye as this point: the rays'
    //  pattern, which turns every frame, averages out; .a keeps the view depth for the next frame's test)
    "  if (acc != 0 && c.a < 59999.0) {\n"
    "    vec2 n = (vec2(p) + 0.5) / vec2(size) * 2.0 - 1.0;\n"
    "    if ((flip_y & 1) != 0) n.y = -n.y;\n"
    "    vec4 cw = vec4(n * c.a, 0.0, c.a);\n"
    "    vec4 h = vec4(vec3(dot(vec4(m[0], m[1], m[2], m[3]), cw), dot(vec4(m[4], m[5], m[6], m[7]), cw),\n"
    "                       dot(vec4(m[8], m[9], m[10], m[11]), cw)) + vec3(m[12], m[13], m[14]), 1.0);\n"
    "    float pw = dot(vec4(pm[12], pm[13], pm[14], pm[15]), h);\n"
    "    if (pw > 0.0) {\n"
    "      vec2 sxy = vec2(dot(vec4(pm[0], pm[1], pm[2], pm[3]), h), dot(vec4(pm[4], pm[5], pm[6], pm[7]), h)) / pw;\n"
    "      if ((flip_y & 1) != 0) sxy.y = -sxy.y;\n"
    "      vec2 huv = sxy * 0.5 + 0.5;\n"
    "      if (all(greaterThan(huv, vec2(0.0))) && all(lessThan(huv, vec2(1.0)))) {\n"
    "        vec4 hv = textureLod(hist, huv, 0.0);\n"
    // (the history held to what this frame's rays gave around the texel: where a shadow has just left, they all
    //  reached the sun and nothing of it stays, so a walking character's shadow has no trail; inside a penumbra
    //  they differ and the history keeps averaging)
    "        if (hv.g > 0.99 && abs(hv.a - pw) < 0.04 * pw + 0.1) o.rb = mix(clamp(hv.rb, lo, hi), o.rb, 0.2);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  imageStore(out_img, p, o);\n"
    "}\n";

// Reflections: for every pixel, the view reflected in the G-buffer's normal there, and a ray along it against the
// scene's meshes. What it hits is looked for where it was on screen in the previous frame (pm: this frame's space to
// that frame's clip space) and takes the lit colour it had; .a is how far that can be trusted (0: no hit, or not on
// screen then; it fades towards the screen's edges). Pushed: the check's, then mode (debug | normals in view space <<
// 8) in place of depth01, and pm.
static const char *k_refl_src =
    "#version 460\n"
    "#extension GL_EXT_ray_query : require\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;\n"
    "layout(set = 0, binding = 1) uniform sampler2D depth_tex;\n"
    "layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D out_img;\n"
    "layout(set = 0, binding = 3) uniform sampler2D normal_tex;\n"
    "layout(set = 0, binding = 4) uniform sampler2D prev_tex;\n"
    "layout(set = 0, binding = 5, rgba8) uniform writeonly image2D dbg_img;\n"
    "layout(set = 0, binding = 6) uniform sampler2D albedo_tex;\n"
    "layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D add_img;\n"
    "layout(set = 0, binding = 8) uniform sampler2D hist_tex;\n"
    "layout(set = 0, binding = 9, std430) buffer CC { uint cc[]; };\n"
    "vec3 cc_dec(uint v) { vec3 c = vec3(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u)) / 1023.0; return c * c * 8.0; }\n"
    "uint cc_enc(vec3 c) { uvec3 u = uvec3(sqrt(clamp(c, vec3(0.0), vec3(8.0)) / 8.0) * 1023.0 + 0.5); return u.x | u.y << 10 | u.z << 20 | 1u << 30; }\n"
    "uint cc_sub(vec2 b) { return 1.0 - b.x - b.y > 0.5 ? 1u : b.x > 0.5 ? 2u : b.y > 0.5 ? 3u : 0u; }\n"
    "layout(push_constant, std430) uniform PC { float m[16]; ivec2 size; int flip_y; int mode; float p22; float p23; float pm[16]; float far_t; };\n"
    "vec3 point(vec2 n, float w) {\n"
    "  vec4 c = vec4(n * w, 0.0, w);\n"
    "  return vec3(dot(vec4(m[0], m[1], m[2], m[3]), c), dot(vec4(m[4], m[5], m[6], m[7]), c),\n"
    "              dot(vec4(m[8], m[9], m[10], m[11]), c)) + vec3(m[12], m[13], m[14]);\n"
    "}\n"
    "float clip_w(float z) { return p23 / (z + p22); }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (any(greaterThanEqual(p, size))) return;\n"
    // (a ray every other pixel each way: the reflection at half the scene's size, a quarter of the rays)
    "  ivec2 hp = p;\n"
    "  if (any(greaterThanEqual(hp * 2, size))) return;\n"
    // (which of the 2x2 a frame: over four frames every pixel has had its ray)
    "  p = min(hp * 2 + ivec2((mode >> 24) & 1, (mode >> 25) & 1), size - 1);\n"
    "  vec2 huv = vec2(-2.0);\n"
    "  int stg = 0;\n"
    "  bool level = false;\n"
    "  vec3 dbgN = vec3(0.0), dbgV = vec3(0.0);\n"
    "  float d = texelFetch(depth_tex, p, 0).r;\n"
    "  vec4 res = vec4(0.0);\n"
    "  vec3 dbg = vec3(0.0);\n"
    "  if (d < 0.9999999) {\n"
    "    vec2 n = (vec2(p) + 0.5) / vec2(size) * 2.0 - 1.0;\n"
    "    if ((flip_y & 1) != 0) n.y = -n.y;\n"
    "    vec3 eye = vec3(m[12], m[13], m[14]);\n"
    "    vec3 pg = point(n, clip_w(d * 2.0 - 1.0));\n"
    "    {\n"  // (where this surface point was on screen in the frame before: its reflection's history)
    "      vec4 h = vec4(pg, 1.0);\n"
    "      vec4 c = vec4(dot(vec4(pm[0], pm[1], pm[2], pm[3]), h), dot(vec4(pm[4], pm[5], pm[6], pm[7]), h), 0.0,\n"
    "                    dot(vec4(pm[12], pm[13], pm[14], pm[15]), h));\n"
    "      if (c.w > 0.0) { vec2 s = c.xy / c.w; if ((flip_y & 1) != 0) s.y = -s.y; huv = s * 0.5 + 0.5; }\n"
    "    }\n"
    "    float dist = length(pg - eye);\n"
    "    vec3 V = (pg - eye) / dist;\n"
    "    vec3 N = texelFetch(normal_tex, p, 0).xyz * 2.0 - 1.0;\n"
    "    if (((mode >> 8) & 1) != 0) {\n"  // (view space: the view's axes in this frame's, from the capture matrix)
    // (right: clip x's column; forward: clip w's; up from the two, right x forward, so the image's way up does not
    //  matter: checked on a lake, whose normal must come out straight up; the normals' z points back at the eye)
    "      vec3 X = normalize(vec3(m[0], m[4], m[8])), F = normalize(vec3(m[3], m[7], m[11]));\n"
    "      X = normalize(X - F * dot(X, F));\n"
    "      N = X * N.x + cross(X, F) * N.y - F * N.z;\n"
    "    }\n"
    "    if (((mode >> 9) & 1) != 0) N = vec3(0.0, 1.0, 0.0);\n"  // (testing, SAVAGEWIND_RT_NORMAL_VIEW=2: straight up)
    "    dbg = (mode & 255) == 3 ? vec3(texelFetch(normal_tex, p, 0).a) : N * 0.5 + 0.5;\n"
    "    dbgN = N; dbgV = V;\n"
    "    stg = 1;\n"
    "    if (dot(N, N) > 0.25) {\n"
    "      stg = 2;\n"
    "      N = normalize(N);\n"
    // (a surface that is all but level: the water. Its ripples are a normal map a few degrees deep, and a ray along
    //  each scattered the reflection into streaks; most of the way to straight up, still water is a mirror and the
    //  ripples still show. mode's bits 10..16: how far, in 64ths)
    "      level = N.y > 0.94;\n"
    "      if (level) N = normalize(mix(N, vec3(0.0, 1.0, 0.0), float((mode >> 10) & 63) / 64.0));\n"
    "      vec3 R = reflect(V, N);\n"
    "      if (dot(R, N) > 0.0) {\n"
    "        vec3 o = pg + N * (0.03 + 0.003 * dist);\n"
    // (a level surface: the water is in the structure too, since the draws after the flush are, and in layers a few
    //  centimetres apart; the ray leaves from above them all. mode's bits 29..30: 0 12 cm, 1 none, 2 30 cm)
    "        if (level) o.y += ((mode >> 29) & 3) == 1 ? 0.0 : ((mode >> 29) & 3) == 2 ? 0.3 : 0.12;\n"
    "        stg = 3;\n"
    "        rayQueryEXT q;\n"
    "        rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT, 1u, o, 0.0, R, far_t);\n"
    "        while (rayQueryProceedEXT(q)) {}\n"
    "        if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {\n"
    "          float t = rayQueryGetIntersectionTEXT(q, true);\n"
    "          if ((mode & 255) == 2) dbg = vec3(fract(log2(max(t, 0.01))), 0.2, 0.2);\n"
    "          vec4 h = vec4(o + R * t, 1.0);\n"
    "          stg = t < 0.5 ? 7 : 4;\n"
    "          vec4 c = vec4(dot(vec4(pm[0], pm[1], pm[2], pm[3]), h), dot(vec4(pm[4], pm[5], pm[6], pm[7]), h),\n"
    "                        dot(vec4(pm[8], pm[9], pm[10], pm[11]), h), dot(vec4(pm[12], pm[13], pm[14], pm[15]), h));\n"
    "          float k = 0.0;\n"
    "          vec3 sc = vec3(0.0);\n"
    "          if (c.w > 0.0) {\n"
    "            vec2 s = c.xy / c.w;\n"
    "            if ((flip_y & 1) != 0) s.y = -s.y;\n"
    "            vec2 e = smoothstep(vec2(1.0), vec2(0.9), abs(s));\n"
    "            k = e.x * e.y;\n"
    // (what the ray hit must be what is seen at that place of the screen: hidden behind something nearer, the
    //  colour there is that other thing's, which made far hillsides dark blotches on the water. The depth is this
    //  frame's, the place the frame before's: near enough. Something seen through the hit (a leaf card's gaps) passes.)
    "            float seen = clip_w(textureLod(depth_tex, s * 0.5 + 0.5, 0.0).r * 2.0 - 1.0);\n"
    "            if (((mode >> 27) & 1) == 0) k *= 1.0 - smoothstep(0.04, 0.10, (c.w - seen) / max(seen, 0.5) - 0.3 / max(seen, 0.5));\n"
    "            if (stg == 4) stg = 5;\n"
    "            if (k > 0.0) sc = textureLod(prev_tex, s * 0.5 + 0.5, 0.0).rgb;\n"
    "          }\n"
    // (not on screen, or hidden there: the colour its triangle had when it was last seen, the colour cache; the part of
    //  the triangle the ray hit, or the mean of its other parts that have one. Only for level surfaces, the water's
    //  mirror: elsewhere the surface's own reflection is a blur of the game's cube, and what a cell's flat colour adds
    //  there would be kept in the cache again, frame after frame.)
    "          vec4 cch = vec4(0.0);\n"
    "          if (((mode >> 28) & 1) != 0 && k < 1.0 && level) {\n"
    "            uint ti = (uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)) + uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true))) * 4u;\n"
    "            if (ti + 3u < uint(cc.length())) {\n"
    "              uint v = cc[ti + cc_sub(rayQueryGetIntersectionBarycentricsEXT(q, true))];\n"
    "              if ((v >> 30) != 0u) cch = vec4(cc_dec(v), 1.0);\n"
    "              else {\n"
    "                for (uint i = 0u; i < 4u; i++) { v = cc[ti + i]; if ((v >> 30) != 0u) cch += vec4(cc_dec(v), 1.0); }\n"
    "                if (cch.a > 0.0) cch /= cch.a;\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "          if ((mode & 255) == 9) k = 0.0;\n"  // (9, testing: the cache alone)
    // (and within reach; premultiplied, for the readers' filter)
    "          float a = (k + (1.0 - k) * cch.a) * (1.0 - smoothstep(1500.0, 3000.0, t));\n"
    "          if (a > 0.0) { res = vec4(mix(cch.rgb, sc, k) * a, a); if (stg == 5 || k < 1.0) stg = 6; }\n"
    "        } else if ((mode & 255) == 2) dbg = vec3(0.0, 0.0, 0.5);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    // (the gloss: the albedo's alpha, its top 3 bits, 0..7; the game's specular exponent is 2 to that less 2)
    "  float gl = ((mode >> 16) & 1) != 0 ? float((uint(texelFetch(albedo_tex, p, 0).a * 255.0 + 0.5) >> 5) & 7u) : 0.0;\n"
    "  if ((mode & 255) == 4) dbg = vec3(gl / 7.0);\n"
    "  if ((mode & 255) == 5) dbg = vec3(float(uint(texelFetch(albedo_tex, p, 0).a * 255.0 + 0.5) & 31u) / 31.0);\n"
    // (the normals target's alpha: on characters, the specular mask: metal and hair bright, cloth and skin dark)
    "  float sm = texelFetch(normal_tex, p, 0).a;\n"
    // (accumulated: a fifth of this frame's over what the surface had; none where it was off screen)
    "  if (((mode >> 26) & 1) != 0 && all(greaterThan(huv, vec2(0.0))) && all(lessThan(huv, vec2(1.0))))\n"
    "    res = mix(textureLod(hist_tex, huv, 0.0), res, 0.2);\n"
    // (6, testing: green no depth, red no normal, yellow a reflection into the surface, blue no hit, cyan a hit behind
    //  the eye of the frame before, white a hit not trusted, black a hit at once; a trusted hit keeps its colour)
    "  if ((mode & 255) == 6 && stg != 6)\n"
    "    res = vec4(stg == 0 ? vec3(0.0, 1.0, 0.0) : stg == 1 ? vec3(1.0, 0.0, 0.0) : stg == 2 ? vec3(1.0, 1.0, 0.0) : stg == 3 ? vec3(0.0, 0.0, 1.0) :\n"
    "               stg == 4 ? vec3(0.0, 1.0, 1.0) : stg == 5 ? vec3(1.0) : vec3(0.02), 1.0);\n"
    "  if ((mode & 255) == 7) res = vec4(dbgN * 0.5 + 0.5, 1.0);\n"  // (7, 8, testing: the normal used, the view direction)
    "  if ((mode & 255) == 8) res = vec4(dbgV * 0.5 + 0.5, 1.0);\n"
    "  imageStore(out_img, hp, res);\n"
    "  imageStore(add_img, hp, vec4(res.rgb * smoothstep(3.5, 6.5, gl), smoothstep(0.55, 0.9, sm)));\n"
    "  if ((mode & 255) != 0) imageStore(dbg_img, p, vec4(dbg, 1.0));\n"
    "}\n";

// The colour cache's fill (CC_K above): a ray through every other pixel each way (the reflections' pixel of the frame);
// where it hits what the depth buffer has there, that triangle's cell takes the lit colour the point had in the frame
// before (pm), mixed into what it held. Bound and pushed as the reflections'.
static const char *k_fill_src =
    "#version 460\n"
    "#extension GL_EXT_ray_query : require\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;\n"
    "layout(set = 0, binding = 1) uniform sampler2D depth_tex;\n"
    "layout(set = 0, binding = 4) uniform sampler2D prev_tex;\n"
    "layout(set = 0, binding = 9, std430) buffer CC { uint cc[]; };\n"
    "vec3 cc_dec(uint v) { vec3 c = vec3(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u)) / 1023.0; return c * c * 8.0; }\n"
    "uint cc_enc(vec3 c) { uvec3 u = uvec3(sqrt(clamp(c, vec3(0.0), vec3(8.0)) / 8.0) * 1023.0 + 0.5); return u.x | u.y << 10 | u.z << 20 | 1u << 30; }\n"
    "uint cc_sub(vec2 b) { return 1.0 - b.x - b.y > 0.5 ? 1u : b.x > 0.5 ? 2u : b.y > 0.5 ? 3u : 0u; }\n"
    "layout(push_constant, std430) uniform PC { float m[16]; ivec2 size; int flip_y; int mode; float p22; float p23; float pm[16]; float far_t; };\n"
    "vec3 point(vec2 n, float w) {\n"
    "  vec4 c = vec4(n * w, 0.0, w);\n"
    "  return vec3(dot(vec4(m[0], m[1], m[2], m[3]), c), dot(vec4(m[4], m[5], m[6], m[7]), c),\n"
    "              dot(vec4(m[8], m[9], m[10], m[11]), c)) + vec3(m[12], m[13], m[14]);\n"
    "}\n"
    "float clip_w(float z) { return p23 / (z + p22); }\n"
    "void main() {\n"
    "  ivec2 hp = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (any(greaterThanEqual(hp * 2, size))) return;\n"
    "  ivec2 p = min(hp * 2 + ivec2((mode >> 24) & 1, (mode >> 25) & 1), size - 1);\n"
    "  float d = texelFetch(depth_tex, p, 0).r;\n"
    "  if (d >= 0.9999999) return;\n"
    "  vec2 n = (vec2(p) + 0.5) / vec2(size) * 2.0 - 1.0;\n"
    "  if ((flip_y & 1) != 0) n.y = -n.y;\n"
    "  vec3 p0 = point(n, clip_w(-1.0)), p1 = point(n, clip_w(1.0)), pg = point(n, clip_w(d * 2.0 - 1.0));\n"
    "  vec3 dir = p1 - p0;\n"
    "  rayQueryEXT q;\n"
    "  rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsCullBackFacingTrianglesEXT, 1u, p0, 0.0, dir, 1.0);\n"
    "  while (rayQueryProceedEXT(q)) {}\n"
    "  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return;\n"
    "  float t = rayQueryGetIntersectionTEXT(q, true);\n"
    // (the triangle the pixel shows: not a leaf card's gap, nor something the depth buffer has and the structure not)
    "  if (length(p0 + dir * t - pg) / max(length(pg - p0), 0.25) > 0.01) return;\n"
    "  vec4 h = vec4(pg, 1.0);\n"
    "  vec3 c = vec3(dot(vec4(pm[0], pm[1], pm[2], pm[3]), h), dot(vec4(pm[4], pm[5], pm[6], pm[7]), h), dot(vec4(pm[12], pm[13], pm[14], pm[15]), h));\n"
    "  if (c.z <= 0.0) return;\n"
    "  vec2 s = c.xy / c.z;\n"
    "  if ((flip_y & 1) != 0) s.y = -s.y;\n"
    "  if (any(greaterThan(abs(s), vec2(0.98)))) return;\n"
    "  vec3 col = textureLod(prev_tex, s * 0.5 + 0.5, 0.0).rgb;\n"
    "  uint i = (uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)) + uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true))) * 4u +\n"
    "           cc_sub(rayQueryGetIntersectionBarycentricsEXT(q, true));\n"
    "  if (i >= uint(cc.length())) return;\n"
    "  uint o = cc[i];\n"
    "  if ((o >> 30) != 0u) col = mix(cc_dec(o), col, 0.3);\n"
    "  cc[i] = cc_enc(col);\n"
    "}\n";

static void init(void) {
    if (inited) return;
    inited = true;
    geos = malloc(sizeof *geos * MAX_GEO);
    buf_make(&cap, (uint64_t)VCAP * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
    for (int i = 0; i < NIDX; i++)
        buf_make(&idx[i], (uint64_t)ICAP * 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    buf_make(&inst_buf, sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    if (mode) {
        meshes = calloc(MESH_CAP, sizeof *meshes);
        ords = calloc(MESH_CAP, sizeof *ords);
        buf_make(&pidx, (uint64_t)PIDX_CAP * 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        buf_make(&as_arena, AS_ARENA, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        buf_make(&scratch, SCRATCH_M, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        for (int i = 0; i < NIDX; i++)
            buf_make(&inst_ring[i], (uint64_t)MAX_GEO * sizeof(VkAccelerationStructureInstanceKHR),
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    }
    buf_make(&stats, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    buf_make(&stats_host, 64 * NRES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkDescriptorSetLayoutBinding lb[5] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};  // (the shadow filter's history)
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = lb};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &dlayout));
    VkDescriptorPoolSize ps_[4] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4 * NRES}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * NRES},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 * NRES}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * NRES}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4 * NRES, .poolSizeCount = 4, .pPoolSizes = ps_};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &dpool));
    for (int i = 0; i < NRES; i++) {
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                          .descriptorSetCount = 1, .pSetLayouts = &dlayout};
        VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, &dsets[i]));
    }
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 152};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &playout));
    VkShaderModule m = vk_compile_glsl(k_check_src, STAGE_COMPUTE, "rt_check");
    if (!m) FATAL("[rt] the check shader did not compile\n");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = playout,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
    VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
    if (rtr_shadows()) {
        VkShaderModule sm = vk_compile_glsl(k_shadow_src, STAGE_COMPUTE, "rt_shadow");
        if (!sm) FATAL("[rt] the shadow shader did not compile\n");
        ci.stage.module = sm;
        VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &shadow_pipe));
        VkShaderModule bm = vk_compile_glsl(k_blur_src, STAGE_COMPUTE, "rt_blur");
        if (!bm) FATAL("[rt] the shadow filter did not compile\n");
        ci.stage.module = bm;
        VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &blur_pipe));
        for (int i = 0; i < NRES; i++) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                              .descriptorSetCount = 1, .pSetLayouts = &dlayout};
            VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, &ssets[i]));
            VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, &bsets[i]));
        }
    }
    if (rtr_reflections()) {
        VkDescriptorSetLayoutBinding rb[10] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo rl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 10, .pBindings = rb};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &rl, NULL, &rlayout));
        VkPushConstantRange rr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 156};
        VkPipelineLayoutCreateInfo rpl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &rlayout,
                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &rr};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &rpl, NULL, &rplayout));
        VkShaderModule rm = vk_compile_glsl(k_refl_src, STAGE_COMPUTE, "rt_reflect");
        if (!rm) FATAL("[rt] the reflection shader did not compile\n");
        VkComputePipelineCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = rplayout,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = rm, .pName = "main"}};
        VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &rci, NULL, &refl_pipe));
        cc_on = mode && (getenv("SAVAGEWIND_RT_REFL_CACHE") ? atoi(getenv("SAVAGEWIND_RT_REFL_CACHE")) != 0 : from_env() || g_settings.rt_offscreen);
        VkShaderModule fm = vk_compile_glsl(k_fill_src, STAGE_COMPUTE, "rt_fill");
        if (!fm) FATAL("[rt] the colour cache's shader did not compile\n");
        rci.stage.module = fm;
        VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &rci, NULL, &fill_pipe));
        // (without the cache: a few cells, so the binding has something)
        buf_make(&ccache, cc_on ? (uint64_t)(PIDX_CAP / 3 + 1) * CC_K * 4 : 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        LOG("[rt] reflections: the colour cache %s, %u MB\n", cc_on ? "on" : "off", (unsigned)(ccache.size >> 20));
        for (int i = 0; i < NRES; i++) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                              .descriptorSetCount = 1, .pSetLayouts = &rlayout};
            VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, &rsets[i]));
        }
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 5 * NRES};
    VK_CHECK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &qpool));
    VkQueryPoolCreateInfo ci2 = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, .queryCount = NRES};
    VK_CHECK(vkCreateQueryPool(g_vk.dev, &ci2, NULL, &cpool));
    LOG("[rt] buffers: %u MB of vertices, %u x %u MB of indices\n", (unsigned)((uint64_t)VCAP * 16 >> 20), NIDX, (unsigned)((uint64_t)ICAP * 4 >> 20));
}

VkDescriptorBufferInfo rtr_cap_info(void) {
    init();
    return (VkDescriptorBufferInfo){cap.buf, 0, VK_WHOLE_SIZE};
}

// ---- results of the frames whose work is done
static void results_take(bool all) {
    for (int i = 0; i < NRES; i++) {
        Res *r = &res[i];
        if (!r->used) continue;
        if (r->seq > vk_completed_seq()) {
            if (!all) continue;
            vk_wait_seq(r->seq);
        }
        r->used = false;
        const uint32_t *s = (const uint32_t *)((const uint8_t *)stats_host.ptr + 64 * r->slot);
        uint64_t ts[5] = {0}, csize = 0;
        vkGetQueryPoolResults(g_vk.dev, qpool, (uint32_t)r->slot * 5, 5, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT);
        if (compact) vkGetQueryPoolResults(g_vk.dev, cpool, (uint32_t)r->slot, 1, 8, &csize, 8, VK_QUERY_RESULT_64_BIT);
        double per = g_vk.props.limits.timestampPeriod / 1e6;
        double t_blas = (double)(ts[1] - ts[0]) * per, t_tlas = (double)(ts[2] - ts[1]) * per, t_trace = (double)(ts[3] - ts[2]) * per;
        tsum.blas += t_blas, tsum.tlas += t_tlas, tsum.trace += t_trace, tsum.n++;
        ssum.t += (double)(ts[4] - ts[3]) * per, ssum.n++;
        n_flush++;
        if (n_flush % (uint64_t)log_every == 1 || r->shot) {
            uint32_t geo_px = s[0] - s[1];
            double g = geo_px ? 100.0 / geo_px : 0;
            LOG("[rt] frame %llu (before fs %016llx): %u meshes, %u triangles (%u from shadow passes), %u vertices, %u cameras | "
                "structure %.1f MB (%.0f bytes a triangle%s), scratch %.1f MB | GPU ms: build %.2f, top level %.2f, 2 rays a pixel %.2f "
                "(mean of %llu: %.2f / %.2f / %.2f)\n",
                (unsigned long long)r->frame, (unsigned long long)r->fs, r->ngeo, r->tris, r->tris_shadow, r->verts, r->ncam,
                r->blas_bytes / 1048576.0, r->tris ? (double)(csize ? csize : r->blas_bytes) / r->tris : 0, csize ? ", compacted" : ", as built",
                r->scratch_bytes / 1048576.0, t_blas, t_tlas, t_trace, (unsigned long long)tsum.n, tsum.blas / tsum.n, tsum.tlas / tsum.n,
                tsum.trace / tsum.n);
            if (mode)
                LOG("[rt]   meshes: %u kept (%.1f MB of structures, %.1f MB of indices, %u resets); this frame %u built from nothing "
                    "(%u triangles), %u refitted, %u waiting | let go so far %llu\n", r->meshes, r->blas_bytes / 1048576.0,
                    pidx_cur * 4 / 1048576.0, r->resets,
                    r->n_built, r->tris_built, r->ngeo - r->n_built, r->n_wait, (unsigned long long)n_evicted);
            LOG("[rt]   %ux%u: %u pixels, sky %u (rays hit there: %u), depth 0: %u | of the %u with geometry: within 0.5%% %.2f%%, within 2%% "
                "%.2f%%, only without alpha-tested %.2f%%, an alpha-tested one there %.2f%%, no hit %.2f%%, hit nearer %.2f%%, hit farther %.2f%% | not captured: %llu full, "
                "%llu camera, %llu frames dropped\n",
                r->w, r->h, s[0], s[1], s[9], s[2], geo_px, s[3] * g, s[4] * g, s[5] * g, s[10] * g, s[6] * g, s[7] * g, s[8] * g,
                (unsigned long long)n_full, (unsigned long long)n_badcam, (unsigned long long)n_dropped);
            if (late_log.draws) LOG("[rt]   left out, drawn after the structure was built (the frame before): %u draws, %u indices\n", late_log.draws, late_log.verts);
            static const char *const why[7] = {"not a triangle list", "no depth test", "blended", "no camera block", "instanced", "program not built",
                                               "another pass writing the scene's depth"};
            for (int k = 0; k < 7; k++)
                if (left_log[k].draws)
                    LOG("[rt]   left out, %s: %u draws (%u writing depth), %u indices, e.g. fs %016llx\n", why[k], left_log[k].draws,
                        left_log[k].dw, left_log[k].verts, (unsigned long long)left_log[k].fs);
            if (rtr_shadows())
                LOG("[rt]   sun shadows (mode %d): %ux%u mask, a ray a pixel, GPU %.2f ms (mean of %llu)%s\n", shadow_env(), mask_w, mask_h,
                    ssum.n ? ssum.t / ssum.n : 0.0, (unsigned long long)ssum.n, r->sun ? "" : " | NO SUN this frame: the game's shadows");
            ssum.t = 0, ssum.n = 0;
            LOG("[rt]   CPU (translating thread): %.3f ms a frame\n", tsum.n ? cpu_ns_log / 1e6 / tsum.n : 0.0);
            cpu_ns_log = 0;
            tsum.blas = tsum.tlas = tsum.trace = 0, tsum.n = 0;
        }
        if (r->shot && shot_host.ptr) {
            char path[64];
            snprintf(path, sizeof path, "rt_%06llu.png", (unsigned long long)r->frame);
            vk_save_png(path, shot_host.ptr, r->w, r->h);
            LOG("[rt] %s saved\n", path);
        }
    }
}

static void hole_add(int a, uint64_t off, uint64_t size) {
    if (!size) return;
    if (n_holes[a] == holes_cap[a]) holes[a] = realloc(holes[a], sizeof(Hole) * (holes_cap[a] = holes_cap[a] ? holes_cap[a] * 2 : 1024));
    holes[a][n_holes[a]++] = (Hole){off, size};
}

static bool hole_take(int a, uint64_t size, uint64_t *off) {
    for (uint32_t i = 0; i < n_holes[a]; i++) {
        Hole *h = &holes[a][i];
        if (h->size < size) continue;
        *off = h->off;
        h->off += size, h->size -= size;
        if (!h->size) *h = holes[a][--n_holes[a]];
        return true;
    }
    return false;
}

// Meshes no draw asked for in a while are let go, a part of the table a frame (their structures are in no frame the
// GPU still has: they were last used KEEP_FRAMES ago); the table is made again when it holds too many of their marks.
static void meshes_sweep(void) {
    for (uint32_t i = 0; i < MESH_CAP / 64; i++) {
        Mesh *m = &meshes[sweep_at];
        sweep_at = (sweep_at + 1) & (MESH_CAP - 1);
        if (!(m->key & 1) || m->seen + KEEP_FRAMES >= cur_frame) continue;
        if (m->as) fn.destroy(g_vk.dev, m->as, NULL);
        hole_add(0, m->as_off, m->as_room);
        hole_add(1, m->ifirst, m->count);
        as_used -= m->as_room;
        *m = (Mesh){.key = TOMB};
        n_meshes--, n_tomb++, n_evicted++;
    }
    if (n_tomb < MESH_CAP / 8) return;
    Mesh *old = meshes;
    meshes = calloc(MESH_CAP, sizeof *meshes);
    for (uint32_t i = 0; i < MESH_CAP; i++) {
        if (!(old[i].key & 1)) continue;
        uint32_t ms = (uint32_t)(old[i].key >> 40) & (MESH_CAP - 1);
        while (meshes[ms].key) ms = (ms + 1) & (MESH_CAP - 1);
        meshes[ms] = old[i];
    }
    free(old);
    n_tomb = 0;
}

static void new_frame(uint64_t frame) {
    if (ngeo) n_dropped++;  // (captured and never built: no draw asked for the flush)
    cur_frame = frame;
    ngeo = vcur = icur = ncam = tris_shadow = 0;
    have_origin = have_scene = have_eye = have_sun = memo_ok = false;
    ring = (ring + 1) % NIDX;
    if (idx_seq[ring] > vk_completed_seq()) vk_wait_seq(idx_seq[ring]);
    results_take(false);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_dead; i++) {
        if (dead[i].seq <= vk_completed_seq()) fn.destroy(g_vk.dev, dead[i].as, NULL);
        else dead[k++] = dead[i];
    }
    n_dead = k;
    if (mode) meshes_sweep();
}

static void dead_add(VkAccelerationStructureKHR as) {
    if (n_dead == dead_cap) dead = realloc(dead, sizeof *dead * (dead_cap = dead_cap ? dead_cap * 2 : 256));
    dead[n_dead].as = as;
    dead[n_dead++].seq = vk_pending_seq();
}

// The mesh table full, or no room for indices or structures: everything is let go and made again as it is drawn (a
// frame of full builds; what a budget and an eviction by age will replace)
static void meshes_reset(void) {
    wait_idle();
    for (uint32_t i = 0; i < MESH_CAP; i++) if (meshes[i].key && meshes[i].as) fn.destroy(g_vk.dev, meshes[i].as, NULL);
    memset(meshes, 0, sizeof *meshes * MESH_CAP);
    n_meshes = n_tomb = pidx_cur = 0;
    cc_all = true, n_fresh = 0;
    as_cur = as_used = 0;
    n_holes[0] = n_holes[1] = 0;
    ngeo = 0;  // (this frame's draws so far point into the table)
    n_resets++;
}

static uint64_t index_hash(uint64_t iaddr, uint32_t itype, uint32_t count) {
    uint32_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
    const uint8_t *s = (const uint8_t *)(uintptr_t)iaddr;
    uint64_t h = mvec_mix(count, itype), w;
    uint32_t n = count * isz;
    if (n <= 96) { for (uint32_t i = 0; i < n; i++) h = mvec_mix(h, s[i]); return h; }
    static int full = -1;  // (testing) SAVAGEWIND_RT_FULLHASH=1: every index
    if (full < 0) full = getenv("SAVAGEWIND_RT_FULLHASH") != NULL;
    if (full) { for (uint32_t i = 0; i + 8 <= n; i += 8) { memcpy(&w, s + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; } return h; }
    for (uint32_t i = 0; i < 32; i += 8) { memcpy(&w, s + i, 8); h = mvec_mix(h, w); }
    for (uint32_t i = 0; i < 32; i += 8) { memcpy(&w, s + (n / 2 & ~7u) + i, 8); h = mvec_mix(h, w); }
    for (uint32_t i = 0; i < 32; i += 8) { memcpy(&w, s + n - 32 + i, 8); h = mvec_mix(h, w); }
    return h;
}

// The mesh of a draw (mode 1 / 2): found, or made (its indices copied, its structure given room). NULL: no room.
static Mesh *mesh_get(uint64_t geo, uint64_t iaddr, uint32_t itype, uint32_t count1, uint32_t first, uint32_t inst, bool opaque) {
    uint32_t count = count1 * inst;  // (an instanced draw: its instances one after the other, as one mesh)
    uint32_t os = (uint32_t)(geo * 0x9E3779B97F4A7C15ull >> 48) & (MESH_CAP - 1);
    while (ords[os].frame == cur_frame && ords[os].geo != geo) os = (os + 1) & (MESH_CAP - 1);
    if (ords[os].frame != cur_frame) ords[os].frame = cur_frame, ords[os].geo = geo, ords[os].n = 0;
    uint64_t key = mvec_mix(geo, ords[os].n++) | 1;
    uint32_t ms = (uint32_t)(key >> 40) & (MESH_CAP - 1);
    while (meshes[ms].key && meshes[ms].key != key) ms = (ms + 1) & (MESH_CAP - 1);
    Mesh *m = &meshes[ms];
    uint64_t ih = mvec_mix(iaddr ? index_hash(iaddr, itype, count1) : mvec_mix(count1, first), inst);
    if (m->key && m->ihash == ih && m->count == count && m->opaque == opaque) { m->seen = cur_frame; return m; }
    if (!m->key && n_meshes + n_tomb >= MESH_CAP / 2) return NULL;
    // its indices, rebased
    uint64_t ifirst = pidx_cur;
    bool ihole = hole_take(1, count, &ifirst);
    if (!ihole && (uint64_t)pidx_cur + count > PIDX_CAP) return NULL;
    uint32_t *dst = (uint32_t *)pidx.ptr + ifirst, lo = ~0u, hi = 0;
    if (iaddr) {
        if (itype == 0) {
            const uint8_t *s = (const uint8_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count1; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        } else if (itype == 1) {
            const uint16_t *s = (const uint16_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count1; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        } else {
            const uint32_t *s = (const uint32_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count1; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        }
        if (hi - lo >= (1u << 21)) { if (ihole) hole_add(1, ifirst, count); return NULL; }  // (a restart index or garbage)
        for (uint32_t i = 0; i < count1; i++) dst[i] -= lo;
    } else {
        lo = first, hi = first + count1 - 1;
        for (uint32_t i = 0; i < count1; i++) dst[i] = i;
    }
    uint32_t stride = hi - lo + 1;
    if ((uint64_t)stride * inst >= (1u << 21)) { if (ihole) hole_add(1, ifirst, count); return NULL; }
    for (uint32_t k = 1; k < inst; k++)
        for (uint32_t i = 0; i < count1; i++) dst[k * count1 + i] = dst[i] + k * stride;
    hi = lo + stride * inst - 1;
    VkAccelerationStructureGeometryKHR g = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexStride = 16, .maxVertex = hi - lo,
                               .indexType = VK_INDEX_TYPE_UINT32}};
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
    VkAccelerationStructureBuildSizesInfoKHR bs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    uint32_t ntris = count / 3;
    fn.sizes(g_vk.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &ntris, &bs);
    uint64_t room = (bs.accelerationStructureSize + 255) & ~255ull, at = as_cur;
    bool ahole = hole_take(0, room, &at);
    if (!ahole && at + room > AS_ARENA) { if (ihole) hole_add(1, ifirst, count); return NULL; }
    if (m->key) {  // (another mesh where this one was: the old one's room is free once this frame's work is recorded)
        if (m->as) dead_add(m->as);
        hole_add(0, m->as_off, m->as_room);
        hole_add(1, m->ifirst, m->count);
        as_used -= m->as_room;
    } else n_meshes++;
    *m = (Mesh){.key = key, .as_size = bs.accelerationStructureSize, .build_scratch = bs.buildScratchSize,
                .update_scratch = bs.updateScratchSize, .ihash = ih, .seen = cur_frame, .as_off = at, .as_room = room,
                .ifirst = (uint32_t)ifirst, .count = count, .lo = lo, .nverts = hi - lo + 1, .stride = stride, .opaque = opaque};
    as_used += room;
    if (cc_on > 0) {  // (its cells of the colour cache held another mesh's colours; next to the last range: one range)
        uint32_t f0 = (uint32_t)ifirst / 3, fn_ = count / 3;
        if (n_fresh && fresh[n_fresh * 2 - 2] + fresh[n_fresh * 2 - 1] == f0) fresh[n_fresh * 2 - 1] += fn_;
        else {
            if (n_fresh == fresh_cap) fresh = realloc(fresh, sizeof *fresh * 2 * (fresh_cap = fresh_cap ? fresh_cap * 2 : 256));
            fresh[n_fresh * 2] = f0, fresh[n_fresh * 2 + 1] = fn_;
            n_fresh++;
        }
    }
    VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = as_arena.buf,
                                               .offset = at, .size = bs.accelerationStructureSize,
                                               .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    VK_CHECK(fn.create(g_vk.dev, &ci, NULL, &m->as));
    VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                      .accelerationStructure = m->as};
    m->as_addr = fn.as_addr(g_vk.dev, &ai);
    if (!ahole) as_cur = at + room;
    if (!ihole) pidx_cur += count;
    return m;
}

static bool invert4(const double m[16], double o[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30)) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

static void view_position(const float *v, double p[3]) {  // the camera of a rigid 3x4 view, row-major
    p[0] = -((double)v[0] * v[3] + (double)v[4] * v[7] + (double)v[8] * v[11]);
    p[1] = -((double)v[1] * v[3] + (double)v[5] * v[7] + (double)v[9] * v[11]);
    p[2] = -((double)v[2] * v[3] + (double)v[6] * v[7] + (double)v[10] * v[11]);
}

// rc_m of a camera block: clip -> the frame's space (world axes, relative to the frame's origin); false: not a camera
static bool camera_matrix(const float *cam, int kind, float out[16]) {
    if (memo_ok && kind == memo_kind && cam == memo_cam && !memcmp(cam, memo_blk, sizeof memo_blk)) { memcpy(out, memo_m, sizeof memo_m); return true; }
    const float *v = cam, *pf = cam + 28;
    for (int i = 0; i < 44; i++) if (!isfinite(cam[i])) return false;
    double det = (double)v[0] * ((double)v[5] * v[10] - (double)v[6] * v[9]) - (double)v[1] * ((double)v[4] * v[10] - (double)v[6] * v[8]) +
                 (double)v[2] * ((double)v[4] * v[9] - (double)v[5] * v[8]);
    if (!(fabs(fabs(det) - 1.0) < 1e-3)) return false;  // (not a rotation: some other block at slot 3)
    double p[16], pinv[16], pos[3], nm[12];
    for (int i = 0; i < 16; i++) p[i] = pf[i];
    view_position(v, pos);
    bool persp = pf[15] == 0.0f && pf[14] == -1.0f;
    if (persp) {
        // view = nm * clip: view z is minus clip w, and x and y follow from it; clip z is not used (far away z / w is
        // 1 minus very little, and the depth it gave back was noise: the first check matched only what stood near)
        if (pf[0] == 0.0f || pf[5] == 0.0f || pf[1] != 0.0f || pf[3] != 0.0f || pf[4] != 0.0f || pf[7] != 0.0f || pf[12] != 0.0f || pf[13] != 0.0f)
            return false;
        double n_[12] = {1.0 / p[0], 0, 0, p[2] / p[0], 0, 1.0 / p[5], 0, p[6] / p[5], 0, 0, 0, -1};
        memcpy(nm, n_, sizeof nm);
    } else {  // (an orthographic one: clip w is 1, the inverse's rows are the affine map)
        if (!invert4(p, pinv)) return false;
        memcpy(nm, pinv, sizeof nm);
        // (a shadow map's camera looks along its -z, down the light: its +z, in the world's axes, points at the sun)
        if (kind == 2 && !have_sun) { sun[0] = v[8], sun[1] = v[9], sun[2] = v[10]; have_sun = true; }
    }
    if (!have_origin) {
        if (persp) memcpy(origin, pos, sizeof origin);
        else {  // (a shadow map's camera before the scene's: the previous frame's scene camera is near enough)
            const MvecCamera *mc = mvec_camera(true);
            if (!mc || !mc->valid) return false;
            view_position(mc->view, origin);
        }
        have_origin = true;
    }
    double rt[9] = {v[0], v[4], v[8], v[1], v[5], v[9], v[2], v[6], v[10]};  // (the view's rotation, transposed)
    // The scene's pass may hold more than one world, each drawn with a camera of its own that stands for the same eye:
    // inside a Divine Beast the dungeon and the land around it are 3.7 km apart in their coordinates and line up on
    // screen. What they share is the view space: a perspective camera of that pass is placed by the first one's
    // rotation and position, whatever its own.
    bool eye = persp && kind == 1;
    if (eye) {
        if (!have_eye) { memcpy(eye_rt, rt, sizeof rt); memcpy(eye_pos, pos, sizeof pos); have_eye = true; }
        memcpy(rt, eye_rt, sizeof rt);
        memcpy(pos, eye_pos, sizeof pos);
    }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += rt[r * 3 + k] * nm[k * 4 + c];
            out[r * 4 + c] = (float)s;
        }
    for (int k = 0; k < 3; k++) out[12 + k] = (float)(pos[k] - origin[k]);
    out[15] = 0;
    memo_cam = cam;
    memo_kind = kind;
    memcpy(memo_blk, cam, sizeof memo_blk);
    memcpy(memo_m, out, sizeof memo_m);
    memo_ok = true;
    return true;
}

static bool capture_(uint64_t frame, uint64_t geo, const float *cam, uint64_t iaddr, uint32_t itype, uint32_t count, uint32_t first,
                     int32_t base_vertex, uint32_t inst, uint32_t base_inst, bool opaque, int cull, int kind, float pc[RTR_PUSH / 4]);
bool rtr_capture(uint64_t frame, uint64_t geo, const float *cam, uint64_t iaddr, uint32_t itype, uint32_t count, uint32_t first,
                 int32_t base_vertex, uint32_t inst, uint32_t base_inst, bool opaque, int cull, int kind, float pc[RTR_PUSH / 4]) {
    uint64_t t0 = SDL_GetTicksNS();
    bool ok = capture_(frame, geo, cam, iaddr, itype, count, first, base_vertex, inst, base_inst, opaque, cull, kind, pc);
    cpu_ns += SDL_GetTicksNS() - t0;
    return ok;
}

static bool capture_(uint64_t frame, uint64_t geo, const float *cam, uint64_t iaddr, uint32_t itype, uint32_t count, uint32_t first,
                     int32_t base_vertex, uint32_t inst, uint32_t base_inst, bool opaque, int cull, int kind, float pc[RTR_PUSH / 4]) {
    memset(pc, 0, RTR_PUSH);
    int32_t off = TRASH;
    memcpy(&pc[16], &off, 4);
    init();
    if (frame != cur_frame) new_frame(frame);
    // (the frame's structure is built. The scene's draws that come after it, the characters and the water, are captured
    //  all the same while the reflections wait to be traced again: rtr_reflect_late adds them to the top level)
    bool after = frame == flushed_frame;
    if (after && !(kind == 1 && late_pending && mode && cam)) { if (kind == 1) late.draws++, late.verts += count; return false; }
    if (count < 3 || count % 3) return false;
    if (!inst) inst = 1;
    if (inst > 1 && (!mode || (uint64_t)count * inst > 600000)) return false;  // (mode 0 has no instances; a field of grass is too much)
    if (ngeo == MAX_GEO || (!mode && (uint64_t)icur + count > ICAP)) { n_full++; return false; }
    float m[16];
    // (a draw of the scene's pass whose vertex shader reads no camera block makes its clip position some other way:
    //  it is the scene's clip space all the same, and the scene camera's matrix places it)
    if (!cam) {
        if (!have_scene) { n_badcam++; return false; }
        memcpy(m, scene_m, sizeof m);
    } else if (!camera_matrix(cam, kind, m)) { n_badcam++; return false; }
    uint32_t *dst = (uint32_t *)idx[ring].ptr + icur, lo = ~0u, hi = 0;
    Mesh *mesh = NULL;
    if (mode) {
        mesh = mesh_get(geo, iaddr, itype, count, first, inst, opaque);
        if (!mesh) {
            static uint64_t last;
            if (last != frame && !after) { last = frame; meshes_reset(); mesh = mesh_get(geo, iaddr, itype, count, first, inst, opaque); }
            if (!mesh) { n_full++; return false; }
        }
        lo = mesh->lo, hi = mesh->lo + mesh->nverts - 1;
        off = iaddr ? (int32_t)vcur - (int32_t)lo - base_vertex : (int32_t)vcur - (int32_t)first;
    } else if (iaddr) {
        if (itype == 0) {
            const uint8_t *s = (const uint8_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        } else if (itype == 1) {
            const uint16_t *s = (const uint16_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        } else {
            const uint32_t *s = (const uint32_t *)(uintptr_t)iaddr;
            for (uint32_t i = 0; i < count; i++) { uint32_t x = s[i]; dst[i] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
        }
        if (hi - lo >= (1u << 21)) return false;  // (a restart index or garbage)
        for (uint32_t i = 0; i < count; i++) dst[i] -= lo;
        off = (int32_t)vcur - (int32_t)lo - base_vertex;  // (gl_VertexIndex: the index plus the base vertex)
    } else {
        lo = first, hi = first + count - 1;
        for (uint32_t i = 0; i < count; i++) dst[i] = i;
        off = (int32_t)vcur - (int32_t)first;
    }
    uint32_t nverts = hi - lo + 1;
    if ((uint64_t)vcur + nverts > VCAP - 1) { n_full++; return false; }  // (the last slot: what TRASH clamps to)
    if (after && !have_scene) return false;
    if (kind == 1 && cam) {
        if (!have_scene) {
            if (cam[43] != 0.0f || cam[42] != -1.0f) { n_badcam++; return false; }  // (the scene's camera is a perspective)
            memcpy(scene_m, m, sizeof scene_m);
            scene_p[0] = cam[38], scene_p[1] = cam[39];
            memcpy(scene_proj, cam + 28, sizeof scene_proj);
            have_scene = true;
            ncam = 1;
        }
        else if (memcmp(scene_m, m, sizeof m) && ncam < 2) {
            ncam = 2;
            static int logged;
            if (logged++ < 3)
                LOG("[rt] a second camera in the G-buffer pass: projection %g %g %g %g / %g %g (the scene's %g %g), position from the "
                    "first %g %g %g\n", cam[28], cam[33], cam[30], cam[34], cam[38], cam[39], scene_p[0], scene_p[1], m[12], m[13], m[14]);
        }
    } else if (kind != 1) {
        tris_shadow += count / 3;
    }
    if (cull && cam && cam[28] * cam[33] < 0.0f) cull = 3 - cull;  // (a projection that mirrors: the windings swap)
    geos[ngeo++] = (Geo){vcur, nverts, icur, mesh ? mesh->count / 3 : count / 3, opaque, (uint8_t)kind, (uint8_t)cull, mesh};
    int32_t si[2] = {mesh ? (int32_t)mesh->stride : 0, (int32_t)base_inst};
    memcpy(&pc[17], si, 8);
    vcur += nverts;
    if (!mode) icur += count;
    memcpy(pc, m, 64);
    memcpy(&pc[16], &off, 4);
    return true;
}

// ---- the frame's structures and the check
typedef struct {
    VkCommandBuffer cb;
    uint32_t ngeo;
    VkAccelerationStructureGeometryKHR *geo;
    VkAccelerationStructureBuildRangeInfoKHR *range;
    VkBuildAccelerationStructureFlagsKHR flags;
    VkAccelerationStructureBuildGeometryInfoKHR *infos;  // (mode 1 / 2: a build or a refit a mesh, ngeo of them)
    const VkAccelerationStructureBuildRangeInfoKHR **pranges;
    uint64_t inst_addr;
    uint32_t ninst;
    uint64_t scratch_addr;
    VkImageView depth;
    VkSampler sampler;
    int slot;
    uint32_t w, h;
    float pc[22];
    bool shot, img_new;
    bool reflect, refl_on, refl_new, prev_new;  // (reflections: refl_on: the image exists; reflect: traced this frame)
    VkImageView normal, albedo;
    VkSampler lsampler;
    uint32_t *fresh, n_fresh;  // (the colour cache's ranges to clear; cc_all: all of it)
    bool cc_use;  // (Video > Off-screen reflections, as the frame was set up)
    bool cc_all, fill;  // (fill: the colour cache is filled in this pass)
    float rpc[39];
    bool shadows, mask_new;  // (the sun shadow mask: traced, or marked not this frame's when there is no sun)
    uint32_t mw, mh;
    float spc[33];
    float bpc[38];  // (the shadow filter's: the check's, the mask's size, whether it accumulates, pm)
} Job;

static Job late_job;

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The reflections' pass: at the flush, or later in the frame (rtr_reflect_late) with the G-buffer as it is then.
static void refl_body(VkCommandBuffer cb, Job *j, bool fill) {
    VkWriteDescriptorSetAccelerationStructureKHR wa = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                       .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
    VkDescriptorImageInfo di0 = {j->sampler, j->depth, VK_IMAGE_LAYOUT_GENERAL};
    if (j->refl_new) {
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = refl, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        ib.image = radd;
        vkCmdPipelineBarrier2(cb, &di);
        ib.image = rhist;
        vkCmdPipelineBarrier2(cb, &di);
        VkClearColorValue cz = {.float32 = {0, 0, 0, 0}};
        VkImageSubresourceRange rz = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, rhist, VK_IMAGE_LAYOUT_GENERAL, &cz, 1, &rz);
        barrier(cb);
    }
    if (j->reflect) {
        VkDescriptorSet rs = rsets[j->slot];
        VkDescriptorImageInfo r_out = {VK_NULL_HANDLE, refl_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_nrm = {j->sampler, j->normal, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_prev = {j->lsampler, prev_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_dbg = {VK_NULL_HANDLE, img_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_alb = {j->sampler, j->albedo ? j->albedo : j->normal, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_add = {VK_NULL_HANDLE, radd_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo r_hist = {j->lsampler, rhist_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo r_cc = {ccache.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet rw[10] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 9, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &r_cc},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 8, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &r_hist},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 6, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &r_alb},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 7, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r_add},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &wa, .dstSet = rs, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r_out},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &r_nrm},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 4, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &r_prev},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rs, .dstBinding = 5, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r_dbg}};
        vkUpdateDescriptorSets(g_vk.dev, 10, rw, 0, NULL);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rplayout, 0, 1, &rs, 0, NULL);
        vkCmdPushConstants(cb, rplayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 156, j->rpc);
        if (fill) {  // (the colour cache first: what is on screen this frame, for the rays that leave it)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fill_pipe);
            vkCmdDispatch(cb, ((j->w + 1) / 2 + 7) / 8, ((j->h + 1) / 2 + 7) / 8, 1);
            barrier(cb);
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, refl_pipe);
        vkCmdDispatch(cb, ((j->w + 1) / 2 + 7) / 8, ((j->h + 1) / 2 + 7) / 8, 1);
        barrier(cb);
    } else {  // (.a 0: nothing traced, the game's cube stays)
        VkClearColorValue cc = {.float32 = {0, 0, 0, 0}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, refl, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
        vkCmdClearColorImage(cb, radd, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
    }
}

static void refl_late_rec(void *arg) {
    Job *j = arg;
    VkCommandBuffer cb = j->cb;
    for (uint32_t i = 0; i < j->n_fresh; i++)
        vkCmdFillBuffer(cb, ccache.buf, (uint64_t)j->fresh[i * 2] * CC_K * 4, (uint64_t)j->fresh[i * 2 + 1] * CC_K * 4, 0);
    if (j->n_fresh) barrier(cb);
    free(j->fresh);
    if (j->ngeo) {  // (the meshes drawn after the flush, and the top level again with them)
        fn.build(cb, j->ngeo, j->infos, j->pranges);
        barrier(cb);
        VkAccelerationStructureGeometryKHR tg = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = j->inst_addr}};
        VkAccelerationStructureBuildGeometryInfoKHR ti = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .dstAccelerationStructure = tlas, .geometryCount = 1, .pGeometries = &tg, .scratchData.deviceAddress = j->scratch_addr};
        VkAccelerationStructureBuildRangeInfoKHR tr = {.primitiveCount = j->ninst};
        const VkAccelerationStructureBuildRangeInfoKHR *ptr = &tr;
        fn.build(cb, 1, &ti, &ptr);
        barrier(cb);
    }
    refl_body(cb, j, cc_on > 0 && j->cc_use);
    barrier(cb);
    free(j->geo);
    free(j->range);
    free(j->infos);
    free(j->pranges);
    free(j);
}

// The frame's reflections kept for the next frame's accumulation: after the last trace of the frame (rtr_reflect_done)
static void refl_hist_rec(void *arg) {
    Job *j = arg;
    if (j->reflect && refl && rhist) {
        barrier(j->cb);
        VkImageCopy hc = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {(j->w + 1) / 2, (j->h + 1) / 2, 1}};
        vkCmdCopyImage(j->cb, refl, VK_IMAGE_LAYOUT_GENERAL, rhist, VK_IMAGE_LAYOUT_GENERAL, 1, &hc);
        barrier(j->cb);
    }
    free(j);
}

static void flush_rec(void *arg) {
    Job *j = arg;
    VkCommandBuffer cb = j->cb;
    uint32_t q = (uint32_t)j->slot * 5;
    vkCmdResetQueryPool(cb, qpool, q, 5);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, q);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = j->flags, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = blas,
        .geometryCount = j->ngeo, .pGeometries = j->geo, .scratchData.deviceAddress = j->scratch_addr};
    const VkAccelerationStructureBuildRangeInfoKHR *pr = j->range;
    if (j->infos) { if (j->ngeo) fn.build(cb, j->ngeo, j->infos, j->pranges); }
    else fn.build(cb, 1, &bi, &pr);
    barrier(cb);
    if (compact) {
        vkCmdResetQueryPool(cb, cpool, (uint32_t)j->slot, 1);
        fn.props(cb, 1, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, cpool, (uint32_t)j->slot);
    }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, q + 1);
    VkAccelerationStructureGeometryKHR tg = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = j->inst_addr}};
    VkAccelerationStructureBuildGeometryInfoKHR ti = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = tlas, .geometryCount = 1, .pGeometries = &tg, .scratchData.deviceAddress = j->scratch_addr};
    VkAccelerationStructureBuildRangeInfoKHR tr = {.primitiveCount = j->ninst};
    const VkAccelerationStructureBuildRangeInfoKHR *ptr = &tr;
    fn.build(cb, 1, &ti, &ptr);
    if (j->img_new) {
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
    }
    vkCmdFillBuffer(cb, stats.buf, 0, 64, 0);
    barrier(cb);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, q + 2);
    VkDescriptorSet set = dsets[j->slot];
    VkWriteDescriptorSetAccelerationStructureKHR wa = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                       .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
    VkDescriptorImageInfo di0 = {j->sampler, j->depth, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di1 = {VK_NULL_HANDLE, img_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo db = {stats.buf, 0, 64};
    VkWriteDescriptorSet w[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &wa, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di0},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di1},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &db}};
    vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 88, j->pc);
    static int check = -1;  // SAVAGEWIND_RT_CHECK=0: no rays (what the capture and the structures cost alone)
    if (check < 0) check = getenv("SAVAGEWIND_RT_CHECK") ? atoi(getenv("SAVAGEWIND_RT_CHECK")) != 0 : from_env();  // (testing only: never from the options)
    if (check) vkCmdDispatch(cb, (j->w + 7) / 8, (j->h + 7) / 8, 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, q + 3);
    if (j->mw) {
        if (j->mask_new) {
            VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = mask, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
            vkCmdPipelineBarrier2(cb, &di);
            ib.image = maskf;
            vkCmdPipelineBarrier2(cb, &di);
            ib.image = maskh;
            vkCmdPipelineBarrier2(cb, &di);
            VkClearColorValue cz = {.float32 = {0, 0, 0, 0}};
            VkImageSubresourceRange rz = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, maskh, VK_IMAGE_LAYOUT_GENERAL, &cz, 1, &rz);
            barrier(cb);
        }
        if (j->shadows) {
            VkDescriptorSet ss = ssets[j->slot];
            VkDescriptorImageInfo dm = {VK_NULL_HANDLE, mask_view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet sw[4];
            memcpy(sw, w, sizeof sw);
            for (int i = 0; i < 4; i++) sw[i].dstSet = ss;
            sw[2].pImageInfo = &dm;
            vkUpdateDescriptorSets(g_vk.dev, 4, sw, 0, NULL);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, shadow_pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &ss, 0, NULL);
            vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 132, j->spc);
            vkCmdDispatch(cb, (j->mw + 7) / 8, (j->mh + 7) / 8, 1);
            barrier(cb);
            VkDescriptorSet bs = bsets[j->slot];
            VkDescriptorImageInfo bsrc = {j->sampler, mask_view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo bdst = {VK_NULL_HANDLE, maskf_view, VK_IMAGE_LAYOUT_GENERAL};
            for (int i = 0; i < 4; i++) sw[i].dstSet = bs;
            sw[1].pImageInfo = &bsrc;
            sw[2].pImageInfo = &bdst;
            vkUpdateDescriptorSets(g_vk.dev, 4, sw, 0, NULL);
            VkDescriptorImageInfo bhist = {vk_default_sampler(true), maskh_view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet hw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bs, .dstBinding = 4, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bhist};
            vkUpdateDescriptorSets(g_vk.dev, 1, &hw, 0, NULL);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, blur_pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &bs, 0, NULL);
            vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 152, j->bpc);
            vkCmdDispatch(cb, (j->mw + 7) / 8, (j->mh + 7) / 8, 1);
            barrier(cb);
            VkImageCopy mc = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {j->mw, j->mh, 1}};
            vkCmdCopyImage(cb, maskf, VK_IMAGE_LAYOUT_GENERAL, maskh, VK_IMAGE_LAYOUT_GENERAL, 1, &mc);
        } else {  // (.g 0: not this frame's, the game's own shadow stays)
            VkClearColorValue cc = {.float32 = {1, 0, 0, 1}};
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, maskf, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
        }
    }
    if (j->refl_on && cc_on > 0) {
        if (j->cc_all) vkCmdFillBuffer(cb, ccache.buf, 0, VK_WHOLE_SIZE, 0);
        for (uint32_t i = 0; i < j->n_fresh && !j->cc_all; i++)
            vkCmdFillBuffer(cb, ccache.buf, (uint64_t)j->fresh[i * 2] * CC_K * 4, (uint64_t)j->fresh[i * 2 + 1] * CC_K * 4, 0);
        if (j->cc_all || j->n_fresh) barrier(cb);
    }
    if (j->refl_on) refl_body(cb, j, cc_on > 0 && j->fill && j->cc_use);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, q + 4);
    barrier(cb);
    VkBufferCopy bc = {0, 64ull * (uint64_t)j->slot, 64};
    vkCmdCopyBuffer(cb, stats.buf, stats_host.buf, 1, &bc);
    if (j->shot) {
        VkBufferImageCopy ic = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {j->w, j->h, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, shot_host.buf, 1, &ic);
    }
    free(j->geo);
    free(j->range);
    free(j->infos);
    free(j->pranges);
    free(j->fresh);
    free(j);
}

static void image_make(VkImage *im, VkDeviceMemory *mem, VkImageView *view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vkCreateImage(g_vk.dev, &ii, NULL, im));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, *im, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, mem));
    VK_CHECK(vkBindImageMemory(g_vk.dev, *im, *mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *im, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, view));
}

VkDescriptorImageInfo rtr_mask_info(void) {
    if (!mask_view) return (VkDescriptorImageInfo){vk_default_sampler(false), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
    return (VkDescriptorImageInfo){vk_default_sampler(true), maskf_view, VK_IMAGE_LAYOUT_GENERAL};
}

void rtr_flush_albedo(VkImageView albedo) { albedo_next = albedo; }
bool rtr_reflect_waiting(void) { return late_pending; }
bool rtr_reflect_open(void) { return hist_pending; }
void rtr_reflect_late(VkCommandBuffer cb, VkImageView depth, VkImageView normal, VkImageView albedo) {
    if (!late_pending) return;
    late_pending = false;
    Job *j = malloc(sizeof *j);
    *j = late_job;
    j->cb = cb, j->geo = NULL, j->range = NULL, j->infos = NULL, j->pranges = NULL, j->fresh = NULL, j->n_fresh = 0, j->cc_all = false;
    j->refl_new = false;
    if (depth) j->depth = depth;
    if (normal) j->normal = normal;
    if (albedo) j->albedo = albedo;
    if (!j->normal) j->reflect = false;
    if (!j->reflect) { free(j); return; }
    late_frame = cur_frame;
    uint32_t n = ngeo, nb = 0;
    ngeo = 0;
    j->ngeo = 0;
    if (n && mode && flushed_frame == cur_frame) {
        VkAccelerationStructureGeometryKHR *g = calloc(n, sizeof *g);
        VkAccelerationStructureBuildRangeInfoKHR *rg = calloc(n, sizeof *rg);
        VkAccelerationStructureBuildGeometryInfoKHR *infos = calloc(n, sizeof *infos);
        const VkAccelerationStructureBuildRangeInfoKHR **pranges = calloc(n, sizeof *pranges);
        VkAccelerationStructureInstanceKHR *in = inst_ring[ring].ptr;
        uint64_t scur = 0;
        uint32_t tris = 0;
        for (uint32_t i = 0; i < n && flush_ninst < MAX_GEO; i++) {
            const Geo *e = &geos[i];
            Mesh *m = e->mesh;
            bool build = !m->built || mode == 2;
            uint64_t need = ((build ? m->build_scratch : m->update_scratch) + scratch_align - 1) & ~(uint64_t)(scratch_align - 1);
            if (m->drawn == cur_frame || scur + need > SCRATCH_M - 65536) continue;
            g[nb] = (VkAccelerationStructureGeometryKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .flags = m->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0,
                .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                       .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = cap.addr, .vertexStride = 16,
                                       .maxVertex = m->nverts - 1, .indexType = VK_INDEX_TYPE_UINT32,
                                       .indexData.deviceAddress = pidx.addr}};
            rg[nb] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = e->ntris, .primitiveOffset = m->ifirst * 4, .firstVertex = e->vfirst};
            infos[nb] = (VkAccelerationStructureBuildGeometryInfoKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
                .mode = build ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
                .srcAccelerationStructure = build ? VK_NULL_HANDLE : m->as, .dstAccelerationStructure = m->as, .geometryCount = 1,
                .pGeometries = &g[nb], .scratchData.deviceAddress = ((scratch.addr + scratch_align - 1) & ~(uint64_t)(scratch_align - 1)) + scur};
            pranges[nb] = &rg[nb];
            nb++;
            scur += need;
            tris += e->ntris;
            m->built = 1;
            m->drawn = cur_frame;
            in[flush_ninst++] = (VkAccelerationStructureInstanceKHR){
                .transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .instanceCustomIndex = m->ifirst / 3, .mask = 1,
                .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, .accelerationStructureReference = m->as_addr};
        }
        j->geo = g, j->range = rg, j->infos = infos, j->pranges = pranges, j->ngeo = nb, j->ninst = flush_ninst;
        idx_seq[ring] = vk_pending_seq();
        { static int ln; if (ln < 3) { ln++; LOG("[rt] late draws in the structure: %u meshes of %u draws, %u triangles\n", nb, n, tris); } }
    }
    if (cc_on > 0 && j->fresh == NULL) {  // (the late meshes' cells)
        j->fresh = fresh, j->n_fresh = n_fresh, j->cc_all = false;
        fresh = NULL, n_fresh = fresh_cap = 0;
    }
    { static int n; if (n < 2) { n++; LOG("[rt] reflections traced again after the late G-buffer draws (the water), %ux%u\n", j->w, j->h); } }
    vk_full_barrier(cb);
    rec_custom(refl_late_rec, j);
    vk_full_barrier(cb);
}
void rtr_reflect_done(VkCommandBuffer cb) {
    if (!hist_pending) return;
    hist_pending = late_pending = false;
    Job *j = malloc(sizeof *j);
    *j = late_job;
    j->cb = cb, j->geo = NULL, j->range = NULL, j->infos = NULL, j->pranges = NULL, j->fresh = NULL, j->n_fresh = 0, j->cc_all = false;
    vk_full_barrier(cb);
    rec_custom(refl_hist_rec, j);
    vk_full_barrier(cb);
}
VkDescriptorImageInfo rtr_radd_info(void) {
    if (!radd_view) return (VkDescriptorImageInfo){vk_default_sampler(false), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
    return (VkDescriptorImageInfo){vk_default_sampler(true), radd_view, VK_IMAGE_LAYOUT_GENERAL};
}
VkDescriptorImageInfo rtr_refl_info(void) {
    if (!refl_view) return (VkDescriptorImageInfo){vk_default_sampler(false), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
    return (VkDescriptorImageInfo){vk_default_sampler(true), refl_view, VK_IMAGE_LAYOUT_GENERAL};
}

void rtr_scene_done(VkCommandBuffer cb, VkImage scene, VkFormat fmt, uint32_t w, uint32_t h) {
    if (!rtr_reflections()) return;
    if (!prev_img || w != prev_w || h != prev_h || fmt != prev_fmt) {
        // (another size or format: a new image; the old one is left, the frame being recorded may read it)
        image_make(&prev_img, &prev_mem, &prev_view, fmt, w, h);
        vk_image_init_layout(cb, prev_img, VK_IMAGE_ASPECT_COLOR_BIT);
        prev_w = w, prev_h = h, prev_fmt = fmt;
        LOG("[rt] reflections: the lit scene kept, %ux%u format %d\n", w, h, fmt);
    }
    vk_full_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    rec_CmdCopyImage(cb, scene, VK_IMAGE_LAYOUT_GENERAL, prev_img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    prev_img_frame = cur_frame;
}

void rtr_flush(VkCommandBuffer cb, VkImageView depth, VkImageView normal, uint32_t w, uint32_t h, uint32_t mw, uint32_t mh,
               bool flip_y, bool depth01, uint64_t fs) {
    if (!ngeo) return;
    if (!have_scene || flushed_frame == cur_frame) { ngeo = 0; return; }
    flushed_frame = cur_frame;
    uint64_t cpu_t0 = SDL_GetTicksNS();
    late_log = late, late = (__typeof__(late)){0};  // (the frame before's)
    memcpy(left_log, left, sizeof left);
    memset(left, 0, sizeof left);
    uint32_t n = ngeo, tris = 0;
    VkAccelerationStructureGeometryKHR *g = calloc(n, sizeof *g);
    VkAccelerationStructureBuildRangeInfoKHR *rg = calloc(n, sizeof *rg);
    uint32_t *maxp = malloc(sizeof *maxp * n);
    VkAccelerationStructureBuildGeometryInfoKHR *infos = NULL;
    const VkAccelerationStructureBuildRangeInfoKHR **pranges = NULL;
    uint32_t nb = 0, n_built = 0, tris_built = 0, n_wait = 0, ninst = 0;
    uint64_t scur = 0;
    if (mode) {
        // a build (a new mesh, within the frame's budget of triangles and scratch) or a refit for every mesh drawn,
        // each with scratch of its own, and an instance for each that has a structure
        infos = calloc(n, sizeof *infos);
        pranges = calloc(n, sizeof *pranges);
        VkAccelerationStructureInstanceKHR *in = inst_ring[ring].ptr;
        VkBuildAccelerationStructureFlagsKHR mf = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        for (uint32_t i = 0; i < n; i++) {
            const Geo *e = &geos[i];
            Mesh *m = e->mesh;
            bool build = !m->built || mode == 2;
            uint64_t need = ((build ? m->build_scratch : m->update_scratch) + scratch_align - 1) & ~(uint64_t)(scratch_align - 1);
            if (m->drawn == cur_frame) continue;  // (the table gave two draws one mesh: a hash collision)
            if ((build && mode == 1 && n_built && tris_built + e->ntris > build_budget) || scur + need > SCRATCH_M - 65536) {
                n_wait++;
                continue;
            }
            g[nb] = (VkAccelerationStructureGeometryKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .flags = m->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0,
                .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                       .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = cap.addr, .vertexStride = 16,
                                       .maxVertex = m->nverts - 1, .indexType = VK_INDEX_TYPE_UINT32,
                                       .indexData.deviceAddress = pidx.addr}};
            rg[nb] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = e->ntris, .primitiveOffset = m->ifirst * 4, .firstVertex = e->vfirst};
            infos[nb] = (VkAccelerationStructureBuildGeometryInfoKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = mf, .mode = build ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
                .srcAccelerationStructure = build ? VK_NULL_HANDLE : m->as, .dstAccelerationStructure = m->as, .geometryCount = 1,
                .pGeometries = &g[nb], .scratchData.deviceAddress = ((scratch.addr + scratch_align - 1) & ~(uint64_t)(scratch_align - 1)) + scur};
            pranges[nb] = &rg[nb];
            nb++;
            scur += need;
            if (build) n_built++, tris_built += e->ntris;
            m->built = 1;
            m->drawn = cur_frame;
            tris += e->ntris;
            // The game's face culling, for rays that ask for it (gl_RayFlagsCullBackFacingTrianglesEXT: the check's, which
            // must see what the rasterizer drew): the faces it culls are the instance's back faces.
            static int wind = -1;  // (testing) SAVAGEWIND_RT_WIND=1: the other winding
            static int nocull = -1;  // (testing) SAVAGEWIND_RT_NOCULL=1: no face is culled
            if (wind < 0) wind = getenv("SAVAGEWIND_RT_WIND") != NULL, nocull = getenv("SAVAGEWIND_RT_NOCULL") != NULL;
            in[ninst++] = (VkAccelerationStructureInstanceKHR){
                .transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .instanceCustomIndex = m->ifirst / 3, .mask = e->kind == 2 ? 2 : 1,
                .flags = !e->cull || nocull ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                                  : ((e->cull == 1) != (wind != 0)) ? VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR : 0,
                .accelerationStructureReference = m->as_addr};
        }
    }
    for (uint32_t i = 0; i < n && !mode; i++) {
        const Geo *e = &geos[i];
        g[i] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = e->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0,
            .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                   .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = cap.addr, .vertexStride = 16,
                                   .maxVertex = e->nverts - 1, .indexType = VK_INDEX_TYPE_UINT32,
                                   .indexData.deviceAddress = idx[ring].addr}};
        rg[i] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = e->ntris, .primitiveOffset = e->ifirst * 4, .firstVertex = e->vfirst};
        maxp[i] = e->ntris;
        tris += e->ntris;
    }
    VkBuildAccelerationStructureFlagsKHR flags = (fast_trace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR) |
                                                 (compact ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = flags, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = n, .pGeometries = g};
    VkAccelerationStructureBuildSizesInfoKHR bs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    if (!mode) fn.sizes(g_vk.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, maxp, &bs);
    else bs.accelerationStructureSize = as_used, bs.buildScratchSize = scur;
    free(maxp);
    static uint64_t tlas_scratch;
    if (!tlas) {  // (sized for the most instances there can be)
        VkAccelerationStructureGeometryKHR tg = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR}};
        VkAccelerationStructureBuildGeometryInfoKHR ti = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1, .pGeometries = &tg};
        VkAccelerationStructureBuildSizesInfoKHR ts = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        uint32_t one = mode ? MAX_GEO : 1;
        fn.sizes(g_vk.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &one, &ts);
        tlas_scratch = ts.buildScratchSize;
        buf_make(&tlas_buf, ts.accelerationStructureSize + 4096, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = tlas_buf.buf,
                                                   .size = ts.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR};
        VK_CHECK(fn.create(g_vk.dev, &ci, NULL, &tlas));
        if (bs.buildScratchSize < ts.buildScratchSize) bs.buildScratchSize = ts.buildScratchSize;
    }
    // (grown, with room: everything submitted is waited for, and nothing recorded for the pending submission uses
    // them, one flush a frame)
    if (!mode && (bs.accelerationStructureSize > blas_buf.size || bs.buildScratchSize + 4096 > scratch.size)) {
        wait_idle();
        if (bs.accelerationStructureSize > blas_buf.size) {
            if (blas) fn.destroy(g_vk.dev, blas, NULL);
            buf_free(&blas_buf);
            uint64_t size = bs.accelerationStructureSize * 3 / 2;
            buf_make(&blas_buf, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
            VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = blas_buf.buf,
                                                       .size = size, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
            VK_CHECK(fn.create(g_vk.dev, &ci, NULL, &blas));
            VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                              .accelerationStructure = blas};
            VkAccelerationStructureInstanceKHR in = {.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .mask = 0xFF,
                                                     .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
                                                     .accelerationStructureReference = fn.as_addr(g_vk.dev, &ai)};
            memcpy(inst_buf.ptr, &in, sizeof in);
            LOG("[rt] structure buffer %.1f MB\n", size / 1048576.0);
        }
        if (bs.buildScratchSize + 4096 > scratch.size) {
            buf_free(&scratch);
            buf_make(&scratch, bs.buildScratchSize * 3 / 2 + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
    }
    bool img_made = false;
    if (w != img_w || h != img_h) {
        wait_idle();
        if (img) {
            vkDestroyImageView(g_vk.dev, img_view, NULL);
            vkDestroyImage(g_vk.dev, img, NULL);
            vkFreeMemory(g_vk.dev, img_mem, NULL);
        }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VK_CHECK(vkCreateImage(g_vk.dev, &ii, NULL, &img));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_vk.dev, img, &mr);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &img_mem));
        VK_CHECK(vkBindImageMemory(g_vk.dev, img, img_mem, 0));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &img_view));
        results_take(true);
        buf_free(&shot_host);
        buf_make(&shot_host, (uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        img_w = w, img_h = h;
        img_made = true;
    }
    bool mask_made = false;
    if (rtr_shadows() && !mask) {
        // (made once, at the size of the first pass that asks: a pass of another size reads it texel for texel all the
        //  same, and gets the corner of it or its edge; the game's shadow passes are all one size)
        image_make(&mask, &mask_mem, &mask_view, VK_FORMAT_R16G16B16A16_SFLOAT, mw, mh);
        image_make(&maskf, &maskf_mem, &maskf_view, VK_FORMAT_R16G16B16A16_SFLOAT, mw, mh);
        image_make(&maskh, &maskh_mem, &maskh_view, VK_FORMAT_R16G16B16A16_SFLOAT, mw, mh);
        mask_w = mw, mask_h = mh;
        mask_made = true;
        LOG("[rt] sun shadow mask %ux%u\n", mw, mh);
    }
    int slot = res_next;
    if (res[slot].used) { vk_wait_seq(res[slot].seq); results_take(false); }
    res_next = (res_next + 1) % NRES;
    bool shot_busy = false;
    for (int i = 0; i < NRES; i++) shot_busy |= res[i].used && res[i].shot;
    bool shot = shot_every > 0 && !shot_busy && cur_frame % (uint64_t)shot_every == 0;
    Job *j = calloc(1, sizeof *j);
    j->cb = cb, j->ngeo = mode ? nb : n, j->geo = g, j->range = rg, j->flags = flags;
    j->infos = infos, j->pranges = pranges;
    j->inst_addr = mode ? inst_ring[ring].addr : inst_buf.addr, j->ninst = mode ? ninst : 1;
    // (the top level's scratch: after the meshes' in mode 1 / 2, whose builds are done by then: the start of the buffer)
    j->scratch_addr = (scratch.addr + 4095) & ~4095ull;
    if (mode && tlas_scratch + 8192 > SCRATCH_M) FATAL("[rt] the top level's scratch does not fit\n");
    j->depth = depth, j->sampler = vk_default_sampler(false), j->slot = slot, j->w = w, j->h = h, j->shot = shot, j->img_new = img_made;
    memcpy(j->pc, scene_m, 64);
    static int view = -1, flip = -1;  // (testing) SAVAGEWIND_RT_VIEW=1: the depth buffer's distances, 2: the rays'; _FLIP: the other way up
    if (view < 0) view = getenv("SAVAGEWIND_RT_VIEW") ? atoi(getenv("SAVAGEWIND_RT_VIEW")) : 0;
    if (flip < 0) flip = getenv("SAVAGEWIND_RT_FLIP") != NULL;
    int32_t ip[4] = {(int32_t)w, (int32_t)h, ((int)flip_y ^ flip) | view << 8, depth01};
    memcpy(&j->pc[16], ip, 16);
    j->pc[20] = scene_p[0], j->pc[21] = scene_p[1];
    // this frame's space -> the previous frame's clip space: where a point was on screen then (the reflections' colour
    // and history, the shadow filter's history)
    bool cam_prev = have_eye && prev_cam_frame != ~1ull;
    float pm[16] = {0};
    if (cam_prev) {
        double v[16] = {0};
        for (int r = 0; r < 3; r++) {
            double t = 0;
            for (int c = 0; c < 3; c++) v[r * 4 + c] = prev_rt[c * 3 + r], t += prev_rt[c * 3 + r] * (origin[c] - prev_pos[c]);
            v[r * 4 + 3] = t;
        }
        v[15] = 1;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double s = 0;
                for (int k = 0; k < 4; k++) s += (double)prev_proj[r * 4 + k] * v[k * 4 + c];
                pm[r * 4 + c] = (float)s;
            }
    }
    uint64_t prev_cam_was = prev_cam_frame;
    if (have_eye) {
        memcpy(prev_rt, eye_rt, sizeof prev_rt);
        memcpy(prev_pos, eye_pos, sizeof prev_pos);
        memcpy(prev_proj, scene_proj, sizeof prev_proj);
        prev_cam_frame = cur_frame;
    }
    j->cc_use = from_env() || g_settings.rt_offscreen;
    // Video > Ray tracing distance: how far the rays go (at once)
    static const float k_far[3] = {1000.0f, 2000.0f, 4000.0f};
    int dist_i = from_env() ? 2 : g_settings.rt_distance < 0 ? 0 : g_settings.rt_distance > 2 ? 2 : g_settings.rt_distance;
    if (rtr_reflections()) {
        bool made = false;
        if (!refl) { image_make(&refl, &refl_mem, &refl_view, VK_FORMAT_R16G16B16A16_SFLOAT, (w + 1) / 2, (h + 1) / 2); made = true; }
        if (!rhist) image_make(&rhist, &rhist_mem, &rhist_view, VK_FORMAT_R16G16B16A16_SFLOAT, (w + 1) / 2, (h + 1) / 2);
        if (!radd) image_make(&radd, &radd_mem, &radd_view, VK_FORMAT_R16G16B16A16_SFLOAT, (w + 1) / 2, (h + 1) / 2);
        static int rmode = -1;  // (testing) SAVAGEWIND_RT_REFL_DEBUG: 1 the normals, 2 the rays' lengths, to the check's image
        static int nview = -1;  // SAVAGEWIND_RT_NORMAL_VIEW=1: the normals are in view space
        if (rmode < 0) rmode = getenv("SAVAGEWIND_RT_REFL_DEBUG") ? atoi(getenv("SAVAGEWIND_RT_REFL_DEBUG")) : 0;
        if (nview < 0) nview = getenv("SAVAGEWIND_RT_NORMAL_VIEW") ? atoi(getenv("SAVAGEWIND_RT_NORMAL_VIEW")) : 1;
        bool have_prev = prev_img && prev_img_frame == prev_cam_was && have_eye;
        j->refl_on = true, j->refl_new = made;
        j->reflect = have_prev && normal && w == img_w && h == img_h && refl;
        {   // (testing: SAVAGEWIND_RT_AB, as the shadows')
            static int rab = -1;
            if (rab < 0) rab = getenv("SAVAGEWIND_RT_AB") ? atoi(getenv("SAVAGEWIND_RT_AB")) : 0;
            extern uint64_t framepace_script_frame(uint64_t presented);
            if (rab > 0 && ((framepace_script_frame(cur_frame) / (uint64_t)rab) & 1)) j->reflect = false;
        }
        j->normal = normal, j->albedo = albedo_next, j->lsampler = vk_default_sampler(true);
        memcpy(j->rpc, j->pc, 88);
        static int accum = -1;  // SAVAGEWIND_RT_REFL_ACCUM=0: no accumulation over frames, the same pixel of each 2x2 every frame
        static uint32_t was_on, phase;
        if (accum < 0) accum = !(getenv("SAVAGEWIND_RT_REFL_ACCUM") && !atoi(getenv("SAVAGEWIND_RT_REFL_ACCUM")));
        static const int k_ph[4] = {0, 3, 1, 2};
        int ph = accum ? k_ph[phase++ & 3] : 0;
        static int flat = -1;  // SAVAGEWIND_RT_REFL_FLAT=<0..63>: how far level surfaces' normals go to straight up, in 64ths
        if (flat < 0) flat = getenv("SAVAGEWIND_RT_REFL_FLAT") ? atoi(getenv("SAVAGEWIND_RT_REFL_FLAT")) & 63 : 56;
        int32_t rm = rmode | nview << 8 | flat << 10 | (albedo_next ? 1 << 16 : 0) | ph << 24 | (accum && was_on ? 1 << 26 : 0) | (getenv("SAVAGEWIND_RT_REFL_NO_OCC") ? 1 << 27 : 0) | (cc_on > 0 && j->cc_use ? 1 << 28 : 0) |
                     (getenv("SAVAGEWIND_RT_REFL_LIFT") ? (atoi(getenv("SAVAGEWIND_RT_REFL_LIFT")) & 3) << 29 : 0);  // (testing: 1 none, 2 30 cm)
        was_on = j->reflect;
        memcpy(&j->rpc[19], &rm, 4);
        memcpy(&j->rpc[22], pm, 64);
        j->rpc[38] = k_far[dist_i] * 1.5f;  // (as far as before at Far: 6000)
    }
    if (mask) {
        static int sflip = -1;  // (testing) SAVAGEWIND_RT_SUN_FLIP=1: the other way along the light
        if (sflip < 0) sflip = getenv("SAVAGEWIND_RT_SUN_FLIP") != NULL;
        j->mw = mask_w, j->mh = mask_h, j->mask_new = mask_made, j->shadows = have_sun;
        // (testing) SAVAGEWIND_RT_AB=<n>: the traced shadows on for n frames, the game's own for the next n, and so
        // on: screenshots of one run a few frames apart show the two on the same scene
        static int ab = -1;
        if (ab < 0) ab = getenv("SAVAGEWIND_RT_AB") ? atoi(getenv("SAVAGEWIND_RT_AB")) : 0;
        if (ab > 0) {
            extern uint64_t framepace_script_frame(uint64_t presented);
            uint64_t sf = framepace_script_frame(cur_frame);
            if ((sf / (uint64_t)ab) & 1) j->shadows = false;
            static int last = -1;
            if (last != (int)j->shadows) LOG("[rt] ab: script frame %llu, traced shadows %s (sun %d)\n", (unsigned long long)sf, j->shadows ? "ON" : "off", have_sun);
            last = j->shadows;
        }
        memcpy(j->spc, j->pc, 88);
        int32_t ms[2] = {(int32_t)mask_w, (int32_t)mask_h};
        memcpy(&j->spc[16], ms, 8);
        // What casts: the meshes of the shadow maps' passes (instance mask 2), which are the game's own choice of
        // casters: it leaves the grass out, and with the scene's meshes casting too every blade shadowed the next
        // one and the meadows went dark and noisy. SAVAGEWIND_RT_CASTERS=3: the scene's as well (mask 1).
        static int casters = -1;
        if (casters < 0) casters = getenv("SAVAGEWIND_RT_CASTERS") ? atoi(getenv("SAVAGEWIND_RT_CASTERS")) : 2;
        memcpy(&j->spc[19], &casters, 4);
        // How far a caster must be (SAVAGEWIND_RT_TMIN=<cm>:<per mille of the distance from the eye>): the shadow
        // maps' meshes are not always the ones on screen (another level of detail), and a surface just under its
        // caster's version shadows itself
        static int tmin_cm = -1, tmin_pm;
        if (tmin_cm < 0) {
            const char *e = getenv("SAVAGEWIND_RT_TMIN");
            // (50:10 took Link's own shadow off the grass at his feet; 0:0 left rock faces dark with the Original
            //  preset's meshes; 15:4 kept both right on the Plateau)
            tmin_cm = 15, tmin_pm = 4;
            if (e) sscanf(e, "%d:%d", &tmin_cm, &tmin_pm);
        }
        int32_t fl = ((int)flip_y ^ flip) | (tmin_cm & 0xFFF) << 8 | (tmin_pm & 0x7FF) << 20;
        memcpy(&j->spc[18], &fl, 4);
        static int nrays = -1;
        static float cone;
        if (nrays < 0) {
            const char *e = getenv("SAVAGEWIND_RT_SHADOW_RAYS");
            nrays = e && atoi(e) > 0 ? atoi(e) : 6;
            if (nrays > 64) nrays = 64;
            e = getenv("SAVAGEWIND_RT_SUN_SIZE");
            // (the sun's disc is 0.0047 across its radius: shadows that sharp read as hard next to the game's own
            //  soft ones. Four times that, so an edge widens with the distance to what casts it and is seen to.)
            cone = e ? (float)atof(e) : 0.02f;
        }
        float sd[4] = {sflip ? -sun[0] : sun[0], sflip ? -sun[1] : sun[1], sflip ? -sun[2] : sun[2], cone};
        memcpy(&j->spc[24], sd, 16);
        // SAVAGEWIND_RT_SHADOW_ACCUM=0: the filter keeps nothing from a frame to the next, and the rays' pattern stays put
        static int macc = -1;
        static uint32_t mphase;
        if (macc < 0) macc = !(getenv("SAVAGEWIND_RT_SHADOW_ACCUM") && !atoi(getenv("SAVAGEWIND_RT_SHADOW_ACCUM")));
        // Video > Ray tracing samples: the rays a pixel (at once)
        int rays_now = nrays;
        if (!from_env() && !getenv("SAVAGEWIND_RT_SHADOW_RAYS")) rays_now = 1 << (g_settings.rt_samples < 0 ? 0 : g_settings.rt_samples > 3 ? 3 : g_settings.rt_samples);
        j->spc[32] = k_far[dist_i];
        int32_t ex[4] = {(int32_t)w, (int32_t)h, shadow_env() == 1 ? 1 : shadow_env() == 4 ? 2 : 0, rays_now | (macc ? (int32_t)(mphase++ & 63) << 8 : 0)};
        memcpy(&j->spc[28], ex, 16);
        memcpy(j->bpc, j->pc, 88);
        int32_t bi[4] = {(int32_t)mask_w, (int32_t)mask_h, (int)flip_y ^ flip, macc && cam_prev};
        memcpy(&j->bpc[16], bi, 16);
        memcpy(&j->bpc[22], pm, 64);
    }
    res[slot] = (Res){.seq = vk_pending_seq(), .frame = cur_frame, .fs = fs, .blas_bytes = bs.accelerationStructureSize,
                      .scratch_bytes = bs.buildScratchSize, .ngeo = mode ? nb : n, .tris = tris, .verts = vcur, .ncam = ncam,
                      .tris_shadow = tris_shadow, .n_built = n_built, .tris_built = tris_built, .n_wait = n_wait,
                      .meshes = n_meshes, .resets = n_resets, .slot = slot, .shot = shot, .used = true, .sun = have_sun, .w = w, .h = h};
    // The reflections are traced here, and again (rtr_reflect_late) if the game draws more into the G-buffer after
    // this: the water goes in after the first deferred passes, and a ray traced here leaves from the lake's bed, not
    // its surface. Not at the composition: by then the game has drawn something else over the normals' target.
    // SAVAGEWIND_RT_REFL_LATE=0: here only.
    static int late = -1;
    if (late < 0) late = !(getenv("SAVAGEWIND_RT_REFL_LATE") && !atoi(getenv("SAVAGEWIND_RT_REFL_LATE")));
    flush_ninst = ninst;
    j->fill = cur_frame - late_frame > 2;  // (filled when the reflections are traced again, if they were the frame before)
    if (cc_on > 0) {
        j->fresh = fresh, j->n_fresh = n_fresh, j->cc_all = cc_all;
        fresh = NULL, n_fresh = fresh_cap = 0, cc_all = false;
    }
    if (j->refl_on) {
        late_job = *j;
        late_pending = late;
        hist_pending = true;
    }
    idx_seq[ring] = vk_pending_seq();
    ngeo = 0;
    cpu_ns_log += cpu_ns + (SDL_GetTicksNS() - cpu_t0);
    cpu_ns = 0;
    vk_full_barrier(cb);
    rec_custom(flush_rec, j);
    vk_full_barrier(cb);
}
