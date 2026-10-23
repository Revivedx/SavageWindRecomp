// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkQueue present_queue;     // a second queue of the same family for presentation (device.c), or queue
    uint32_t qfam, qfam_count;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, wide_lines;
    bool depth_clamp;          // depthClamp feature (nvnCommandBufferSetDepthClamp: shadow maps keep occluders before the near plane)
    bool feedback_loop;        // attachment feedback loops in GENERAL layout + barriers inside dynamic rendering (exec.c)
    bool gpl;                  // graphics pipeline libraries with fast linking: pipelines linked from per-stage parts (exec.c)
    // (with gpl) depth clamp and polygon mode set at draw time (VK_EXT_extended_dynamic_state3): a vertex shader's
    // part doesn't depend on them
    PFN_vkCmdSetDepthClampEnableEXT set_depth_clamp;
    PFN_vkCmdSetPolygonModeEXT set_polygon_mode;
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
void vk_present_idle(void);  // device.c: the presentation thread has no present queued or in progress
void vk_dyn_forget(void);    // exec.c: graphics pipeline / dynamic state of the main command buffer changed elsewhere

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
// Render-target scale groups (textures.c tex_scale_group): none, the 3D scene's (internal resolution), the final
// image's (the window's resolution)
enum { RS_NONE = 0, RS_SCENE = 1, RS_OUT = 2 };

typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    bool slices_renderable;  // 3D image made 2D-array compatible: its slices are render targets
    bool feedback_ok;        // made with VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT (sampled while it is rendered to)
    bool feedback_seen;      // a draw sampled it while writing it: its passes begin with the feedback loop enabled
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    bool scalable, presented;
    uint8_t rs_group;            // RS_*: which output scale its allocation follows (textures.c)
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    // textures.c: one image per memory and shape, shared by every texture made there
    uint64_t key_addr;           // 0: not shared
    uint32_t key[7];             // target, format, width, height, depth, levels, samples
    struct TexBackend *key_next;
    int refs;                    // textures holding the image
    int all, idle;               // slots + 1 in textures.c's lists of all images / images no texture holds
    uint64_t idle_frame, used_frame;  // when it was let go / last asked for by a finalized texture (zombies)
    uint64_t serial;             // which image this is: every image made gets a new one (exec.c's linear targets)
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
void vk_make_scalable_like(NvnTexture *t, NvnTexture *like);  // a pass's attachments share one scale
uint8_t vk_texture_scale_group(NvnTexture *t);
void renderer_get_scene_scale(float *sx, float *sy);
void renderer_set_scene_scale(float sx, float sy);
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
bool vk_replace_file(const char *tmp, const char *path);  // rename over an existing file, atomically
