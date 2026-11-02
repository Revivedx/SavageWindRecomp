// Shader compilation and reflection for the Vulkan backend (shaders.c).
#pragma once
#include <stddef.h>

#include "vk.h"

typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;

void vk_shaders_init(void);
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);  // malloc'd, NULL on error
int vk_spirv_ensure(const char *src, int stage);  // in the SPIR-V cache: 0 was there, 1 compiled now, -1 failed
// (precompile.c) translation + SPIR-V of a binary with a key into the caches: 1 made, 0 cached already, -1 failed (why
// says why)
int vk_precompile_one(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key, char *why, size_t nwhy);
ShaderInfo *vk_xlate_read(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);  // (maxwell_free it)
// (exec.c) a pipeline built and thrown away, for the driver to compile the program's shaders (and keep them)
bool vk_pso_warm(const ShaderInfo *vi, const ShaderInfo *fi, const uint8_t attr_kind[32]);
bool vk_pso_warm_compute(const ShaderInfo *si, const uint32_t local[3]);
bool vk_pso_warm_stage(const ShaderInfo *si, int stage);  // (graphics pipeline libraries: one shader's)
// (device.c) the precompiled shaders' libraries: their cache (made when missing; NULL when not kept), saving it, and
// whether its file is there for this GPU, driver and translator
VkPipelineCache vk_precompiled_cache(void);
bool vk_precompiled_cache_save(void);
bool vk_precompiled_cache_ok(void);
// maxwell_translate through a cache (memory and cache/xlate/): the same ShaderInfo for the same binary and key, never
// freed. Failed translations are not cached.
ShaderInfo *vk_translate_cached(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
// The same, only when the translation is already in memory (no file read, no translation): else NULL.
ShaderInfo *vk_translate_peek(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
// The id of a translation vk_translate_cached returned (its cache/xlate name; 0: not cached), and the translation
// with that id from memory or cache/xlate/ (NULL: not there, or not of that stage).
uint64_t vk_translate_id(const ShaderInfo *si);
ShaderInfo *vk_translate_load_id(uint64_t id, int stage);
uint64_t vk_translate_id_for(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);  // (the id it would get)
// (exec.c) the pipeline list for the pipeline seed (precompile.c): its records, one added, the fixed state's size
size_t vk_pso_state_size(void);
int vk_pso_records(int (*fn)(void *ctx, int kind, const uint64_t id[2], const void *state, size_t n), void *ctx);
bool vk_pso_add(int kind, uint64_t id0, uint64_t id1, const void *state, size_t n);
void vk_pso_flush(void);
void vk_pso_rewarm(void);  // the list read again and pre-warmed (after the seed's records joined it in this process)
// Only what the translation reads of a key (the Shader Program Header's inputs and outputs); vk_sph_inputs: the
// vertex attributes / fragment inputs a shader reads (bit i: attribute or varying i).
void vk_key_normalize(const uint8_t *code, int stage, ShaderKey *key);
uint32_t vk_sph_inputs(const uint8_t *code, int stage);
// The shader key log (cache/vk_shader_keys.list): every shader binary and key translated, in this and earlier sessions.
typedef struct { uint64_t code; uint32_t stage, bin_size; ShaderKey key; } VkKeyLogEntry;
int vk_keylog_read(VkKeyLogEntry **out);  // malloc'd copy
void vk_keylog_flush(void);
// cache/ (or SAVAGEWIND_CACHE_DIR): out = that folder + "/" + name
const char *vk_cache_dir(void);
const char *vk_cache_dir_id(const char *id);  // a folder name for a translator / SPIR-V id
void vk_cache_cleanup(void);                  // other builds' translations and SPIR-V removed (in the background)
void vk_cache_path(char *out, size_t n, const char *name);
// What caches of translated shaders are valid for: the translator's id and the GLSL -> SPIR-V step's (spirv.c); the
// full one adds the device's translation variant (maxwell_translation_variant): what a precompilation was made under.
const char *vk_shader_version(void);
const char *vk_shader_version_full(void);
const char *vk_spirv_id(void);
VkShaderModule vk_module(const uint32_t *spv, size_t nwords);
// Descriptor bindings declared by a SPIR-V module.
int vk_reflect(const uint32_t *spv, size_t nwords, VkBindingInfo *out, int max);
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages);
