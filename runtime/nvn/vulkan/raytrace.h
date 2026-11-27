// Ray tracing, phase 1 (raytrace.c, docs/research/ray-tracing.md): the scene's triangles captured in a space ray
// queries can use, an acceleration structure of them every frame, and a check of it against the game's depth buffer.
// Off unless SAVAGEWIND_RT=1 and the device has VK_KHR_acceleration_structure and VK_KHR_ray_query.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

// A capture program (maxwell/translate.c, ShaderKey rt_cap): the vertex shader also writes each vertex's position to
// the capture buffer at this binding of the vertex set, at rc_off + gl_VertexIndex. Its push constants follow the
// motion program's (mvec.h MVEC_PUSH): float rc_m[16] (row-major: the game's clip position to the frame's ray
// tracing space, before the division by w), int rc_off, int rc_stride, int rc_inst0 (an instance's vertices follow
// the one before's, rc_stride apart; rc_inst0: the draw's first instance).
#define RTR_CAP_BINDING 62
#define RTR_PUSH_OFF 116
#define RTR_PUSH 76

bool rtr_wanted(void);                // Video > Ray tracing, or SAVAGEWIND_RT (device.c: the extensions are asked for)
void rtr_set_supported(bool can);     // (device.c) the device has what ray tracing takes
bool rtr_supported(void);
void rtr_device_ready(bool have);     // (device.c, after the device is made) have: the extensions and features are on
bool rtr_on(void);
bool rtr_shadow_passes(void);         // the shadow maps' draws are captured too (SAVAGEWIND_RT_SHADOW=1, or shadows on)
VkDescriptorBufferInfo rtr_cap_info(void);
// A triangle list about to be drawn with a capture program (the translating thread): its indices are copied, room
// for its vertices is taken and pc gets its push constants. cam: its camera block (the vertex stage's constant buffer
// at slot 3: the view in rows 0-2, the projection in rows 7-10; NULL: none, the scene's camera places it). kind: 1 the scene's G-buffer, 2 a shadow map.
// cull: the faces the game culls, by their winding in its clip space (0 none, 1 counter-clockwise, 2 clockwise).
// geo: what tells its mesh from others (its index range and vertex streams, its shaders), for the kept structures.
// false: not captured (no room, or a camera it can't place); pc is then one that writes nothing that is used.
bool rtr_capture(uint64_t frame, uint64_t geo, const float *cam, uint64_t iaddr, uint32_t itype, uint32_t count, uint32_t first,
                 int32_t base_vertex, uint32_t inst, uint32_t base_inst, bool opaque, int cull, int kind, float pc[RTR_PUSH / 4]);
// A draw of the scene's G-buffer pass that is not captured (why: 0 not a triangle list, 1 no depth test, 2 blended,
// 3 no camera block, 4 instanced, 5 its capture program is not built yet), for the log
void rtr_left_out(int why, uint32_t count, bool depth_write, uint64_t fs);
bool rtr_pending(void);  // triangles captured this frame, not built yet
// Outside a pass, once the scene's depth is complete: builds the frame's acceleration structure and traces one ray a
// pixel against it, compared with depth (the scene's depth as a sampled view, w x h). flip_y / depth01: the capture
// programs' ShaderKey flip_y / depth_minus_one. fs: the fragment shader whose draw comes next (logged).
// mw x mh: the size of the traced sun shadow mask (rtr_mask_info), when shadows are on.
// normal: the G-buffer's normals as a sampled view (reflections; null: none this frame).
void rtr_flush(VkCommandBuffer cb, VkImageView depth, VkImageView normal, uint32_t w, uint32_t h, uint32_t mw, uint32_t mh,
               bool flip_y, bool depth01, uint64_t fs);
// Ray traced reflections, SAVAGEWIND_RT_REFLECT=1 (false when off or the device can't): a ray a pixel along the
// reflection of the view in the G-buffer's normal; what it hits takes its colour from the previous frame's lit scene,
// where it was on screen then (rtr_scene_done keeps that image: outside a pass, at the end of the frame's scene). The
// game's composition passes mix it into their cube reflection (maxwell/shader.h maxwell_rt_refl).
bool rtr_reflections(void);
VkDescriptorImageInfo rtr_refl_info(void);
// The reflection added where the game has none (SAVAGEWIND_RT_REFL_ADD=<strength>, default 0.5; 0: none): the traced
// colour weighed by the gloss in the albedo target's alpha (rtr_flush_albedo: its view, before rtr_flush).
VkDescriptorImageInfo rtr_radd_info(void);
void rtr_flush_albedo(VkImageView albedo);
// The reflections are traced at the flush, and again if the game draws more into the G-buffer afterwards (the water):
// rtr_reflect_late, right after those draws, with the depth, normals and albedo views (later the game reuses the
// normals' target). rtr_reflect_done, before the first light composition pass, keeps the result for the next frame.
bool rtr_reflect_waiting(void);
bool rtr_reflect_open(void);
void rtr_reflect_late(VkCommandBuffer cb, VkImageView depth, VkImageView normal, VkImageView albedo);
void rtr_reflect_done(VkCommandBuffer cb);
void rtr_scene_done(VkCommandBuffer cb, VkImage scene, VkFormat fmt, uint32_t w, uint32_t h);
// Ray traced sun shadows, SAVAGEWIND_RT_SHADOWS (0 when off or the device can't): 1: every mesh casts, in place of
// the game's cascades; 2: only what has no alpha test casts, and the game's cascades stay (the darker of the two);
// 3: as 2 without the cascades (to look at what the rays alone give); 4: what has no alpha test casts by the rays,
// and where a ray reaches the sun through an alpha-tested mesh (foliage) the game's cascades decide: nothing else
// takes their answer. SAVAGEWIND_RT_SHADOW_RAYS=<n>: rays a pixel, in a cone the sun's size (default 4);
// SAVAGEWIND_RT_SUN_SIZE=<radians>: the cone's half angle. The game's shadow passes read the mask
// (maxwell/shader.h maxwell_rt_shadow): rtr_shadows gives that 1 or 2.
int rtr_shadows(void);
VkDescriptorImageInfo rtr_mask_info(void);
