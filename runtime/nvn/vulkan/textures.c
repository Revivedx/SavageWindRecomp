// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <stdarg.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../../settings.h"
#include "../astc.h"
#include "../glyphs.h"
#include "../renderer.h"
#include "../../platform.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; SAVAGEWIND_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: return (VkFmt){getenv("SAVAGEWIND_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ================================================================== output resolution / per-frame clears
// Render targets are allocated at one of two scales (runtime/nvn/output_scale.c sets both every presented frame):
//   RS_OUT    the game's final image (1920x1080, its halves) and the display images: the window's resolution, so the
//             HUD and menus are drawn sharp at any window size
//   RS_SCENE  the 3D scene's targets (1600x900 docked, and their fractions): the internal resolution, the window's
//             with the upscaler off, smaller by the upscaler's factor with it on (exec.c upscales at the composite)
//   RS_SHADOW shadow maps (BOTW's sun cascades: D16 896x896x3): Video > Shadow resolution, a width (2048 to 8192) they
//             are made at instead of the game's; never smaller than the game's (output_scale.c shadow_map_size). Also
//             their half-size copy (R16 448x448x3, the max of each 2x2), at the same factor: water and the other
//             forward-shaded surfaces take their shadows from it (issue #101)
static float g_rs[4][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
static uint32_t g_shadow_size;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs[RS_OUT][0]; *sy = g_rs[RS_OUT][1]; }
void renderer_set_scale(float sx, float sy) { g_rs[RS_OUT][0] = sx; g_rs[RS_OUT][1] = sy; }
void renderer_get_scene_scale(float *sx, float *sy) { *sx = g_rs[RS_SCENE][0]; *sy = g_rs[RS_SCENE][1]; }
void renderer_set_scene_scale(float sx, float sy) { g_rs[RS_SCENE][0] = sx; g_rs[RS_SCENE][1] = sy; }
void renderer_set_shadow_size(uint32_t n) {  // (never past what the GPU can make: Extreme is 8192)
    g_shadow_size = n < g_vk.props.limits.maxImageDimension2D ? n : g_vk.props.limits.maxImageDimension2D;
}
uint64_t renderer_present_count(void);
static float rs_want(const TexBackend *b, int axis) {  // the scale a texture of b's group is wanted at now
    if (b->rs_group != RS_SHADOW) return g_rs[b->rs_group][axis];
    return b->guest_w && g_shadow_size > b->guest_w ? (float)g_shadow_size / (float)b->guest_w : 1.0f;  // (square)
}
static bool rs_stale(const TexBackend *b) {
    return b->rs_group && (b->sx != rs_want(b, 0) || b->sy != rs_want(b, 1));
}

// Which scale a texture takes (RS_*): the game's screen-sized targets are 16:9 2D images or 2D arrays (BOTW renders
// some half-size passes into layers of 800x450x2 arrays) with the compressible flag (render targets) or the display
// flag; shadow maps are square depth render targets, 512 or more (BOTW's sun cascades; its 192x192 depth target is no
// shadow map), and the cascades' half-size copy is a square R16 2D array of 3 layers, one per cascade (no
// render-target flag; a looser rule also took a 1024x1024x8 R16 array filled once at load, which a change of the
// setting then made again empty: the terrain went). Everything else keeps its guest size: LUTs, cubemaps, the small
// odd-sized steps of the bloom and exposure chains (sampled with normalized coordinates, they work at any size).
// SAVAGEWIND_NO_SCALE=1: none scaled.
// The sun cascades' half-size copy (R16 2D array of 3 layers, square, 256 or more: 448x448x3 in BOTW): a pass takes the
// max of each 2x2 of the cascades into it, and water, terrain, grass and the other forward-shaded surfaces do their
// shadow comparisons on it.
bool vk_is_shadow_copy(const NvnTexture *t) {
    return t && t->target == 4 && t->format == 0x06 && t->samples <= 1 && t->width == t->height && t->width >= 256 &&
           t->depth == 3;
}

// Above the console's shadow resolution, the copy's readers compare against the cascades themselves (exec.c
// shadow_copy_source): already a depth image, already at the chosen size, sampled at the same normalized coordinates and
// layers. The copy stays at the game's size. Scaling the copy with the cascades instead (as before) made it 4096x4096x3
// at Extreme, drawn every frame and sampled by every forward-shaded pixel: 1.5-1.7 ms of GPU on an RTX 5090 at
// 5120x2160. SAVAGEWIND_NO_SHADOW_DIRECT=1: the scaled copy, as before.
bool vk_shadow_direct(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_NO_SHADOW_DIRECT") == NULL;
    return on;
}

// The cascades t are made bigger than the game's (Shadow resolution above Console); at Console the copy's readers keep it
bool vk_shadow_scaled(const NvnTexture *t) { return t && g_shadow_size > t->width; }

static uint8_t tex_scale_group(const NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_NO_SCALE") == NULL;
    if (!on) return RS_NONE;
    if ((t->target == 1 || t->target == 4) && t->samples <= 1 && (t->flags & 8) && renderer_format_is_depth(t->format) &&
        t->width == t->height && t->width >= 512)
        return RS_SHADOW;
    if (!vk_shadow_direct() && vk_is_shadow_copy(t)) return RS_SHADOW;  // (the copy's readers use the cascades instead)
    if ((t->target != 1 && t->target != 4) || t->samples > 1 || !(t->flags & 9) || t->width < 32 || t->height < 16) return RS_NONE;
    float a = (float)t->width / (float)t->height;
    if (a < 1.74f || a > 1.81f) return RS_NONE;
    if (t->flags & 1) return RS_OUT;  // NVN_TEXTURE_FLAGS_DISPLAY: the images the game presents
    for (int k = 0; k < 4; k++)       // the final image's 1920x1080 and its halves (rounded either way)
        if ((t->width == 1920u >> k || t->width == (1920u + (1u << k) - 1) >> k) &&
            (t->height == 1080u >> k || t->height == (1080u + (1u << k) - 1) >> k))
            return RS_OUT;
    return RS_SCENE;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

// Bumped when the scene's render targets should start clean (a level change: a game-specific hook; none yet).
static uint32_t g_level_gen = 1;

// (Link's Awakening cleared its scaled scene targets at their first use in each frame. BOTW keeps data in them from
// frame to frame, as on the Switch, where nothing clears them: opt-in, SAVAGEWIND_FRAME_CLEAR=1.)
bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_FRAME_CLEAR") != NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("SAVAGEWIND_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);
static void evict_idle_images(void);
static bool image_format_fallback_error(VkResult r);
static bool image_out_of_memory(VkResult r);
static void log_memory_budget(const char *when);

static VmaPool image_priority_pools[VK_MAX_MEMORY_TYPES];
static bool image_priority_pool_failed[VK_MAX_MEMORY_TYPES];

static VmaAllocationCreateInfo image_allocation_info(const VkImageCreateInfo *ci) {
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
#ifdef VK_EXT_memory_priority
    if (!g_vk.memory_priority) return ai;
    uint32_t memory_type;
    VkResult r = vmaFindMemoryTypeIndexForImageInfo(g_vk.vma, ci, &ai, &memory_type);
    if (r != VK_SUCCESS) {
        LOG("[vk] could not select low-priority image memory type (%d); using default allocation\n", r);
        return ai;
    }
    if (!image_priority_pools[memory_type] && !image_priority_pool_failed[memory_type]) {
        VmaPoolCreateInfo pi = {.memoryTypeIndex = memory_type, .priority = 1.0f};
        r = vmaCreatePool(g_vk.vma, &pi, &image_priority_pools[memory_type]);
        if (r != VK_SUCCESS) {
            image_priority_pool_failed[memory_type] = true;
            LOG("[vk] could not create high-priority image memory pool for type %u (%d); using default allocation\n",
                memory_type, r);
        }
    }
    if (image_priority_pools[memory_type]) ai.pool = image_priority_pools[memory_type];
#endif
    return ai;
}

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->scalable = b->rs_group != RS_NONE;
    // (RS_SHADOW: the cascades' width, also for their half-size copy, so that both grow by the same factor)
    b->guest_w = b->rs_group == RS_SHADOW && !renderer_format_is_depth(t->format) ? t->width * 2 : t->width;
    b->sx = rs_want(b, 0);
    b->sy = rs_want(b, 1);
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    b->slices_renderable = type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (b->slices_renderable) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;  // (Vulkan 1.1) 2D views of slices
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const bool sparse = plat_guest_sparse_heap_enabled();
    VmaAllocationCreateInfo ai = image_allocation_info(&ci);
    // 2D color targets may be sampled by the draws that render into them (exec.c resolve_feedback: a feedback loop)
    b->feedback_ok = g_vk.feedback_loop && type == VK_IMAGE_TYPE_2D && samples == 1 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (b->feedback_ok) ci.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (sparse && image_out_of_memory(r)) {
        log_memory_budget("at image allocation failure");
        LOG("[vk] image allocation ran out of memory (%ux%ux%u, %u layers, %u levels); draining GPU work and retrying\n",
            tw, th, depth, layers, levels);
        vk_gmem_trace_report();
        renderer_flush();
        vk_collect_dead();
        evict_idle_images();
        VmaAllocationCreateInfo retry_ai = ai;
#ifdef VK_EXT_memory_priority
        if (g_vk.memory_priority) {
            retry_ai.pool = VK_NULL_HANDLE;
            retry_ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            retry_ai.priority = 1.0f;
            LOG("[vk] retrying image as dedicated high-priority memory\n");
        }
#endif
        r = vmaCreateImage(g_vk.vma, &ci, &retry_ai, &b->image, &b->mem, NULL);
    }
    if ((sparse ? image_format_fallback_error(r) : r != VK_SUCCESS) && b->feedback_ok) {
        b->feedback_ok = false;
        ci.usage &= ~VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    }
    if (sparse ? image_format_fallback_error(r) : r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ai = image_allocation_info(&ci);
        r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
        depthfmt = false;
    }
    if (r != VK_SUCCESS) {
        if (sparse && image_out_of_memory(r)) log_memory_budget("after allocation retry failed");
        if (sparse) LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        VK_CHECK(r);
    }
    static uint64_t serials;
    b->serial = __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED);
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    if (b->scalable && !depthfmt) {  // (made again at a new scale: no stale memory in what frames keep)
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearColorValue zero = {{0}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    }
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: SAVAGEWIND_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static bool image_format_fallback_error(VkResult r) {
    return r == VK_ERROR_FORMAT_NOT_SUPPORTED || r == VK_ERROR_FEATURE_NOT_PRESENT;
}

static bool image_out_of_memory(VkResult r) {
    return r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void log_memory_budget(const char *when) {
    VkPhysicalDeviceMemoryProperties mp;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {0};
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    vmaGetHeapBudgets(g_vk.vma, budgets);
    LOG("[vk] memory budget %s (VK_EXT_memory_budget %s)\n", when, g_vk.memory_budget ? "enabled" : "unavailable; VMA estimates");
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
        LOG("[vk]   heap %u flags %#x (device-local flag %s): size %llu MB, budget %llu MB, usage %llu MB; "
            "VMA blocks %llu MB, allocations %llu MB\n",
            i, mp.memoryHeaps[i].flags, (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "yes" : "no",
            (unsigned long long)(mp.memoryHeaps[i].size >> 20), (unsigned long long)(budgets[i].budget >> 20),
            (unsigned long long)(budgets[i].usage >> 20), (unsigned long long)(budgets[i].statistics.blockBytes >> 20),
            (unsigned long long)(budgets[i].statistics.allocationBytes >> 20));
    }
}

static void destroy_image(TexBackend *b) {
    if (b->block) {
        destroy_image(b->block);
        free(b->block);
        b->block = NULL;
    }
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// One image per memory and shape. On hardware a texture is its memory: every NVNtexture made over the same memory
// with the same shape reads and writes the same texels. BOTW 1.8.2 has several such objects at once (pool entries
// registered with an object it finalized, copies of NVNtexture objects, command buffers recorded before it made its
// frame's render targets again in the same memory), so they all take the same image. An image no texture holds any
// more waits a few frames for the next texture made there, then goes.
#define PARK_FRAMES 4
#define KEY_BUCKETS 4096
static TexBackend *g_keyed[KEY_BUCKETS];
static uint32_t key_bucket(uint64_t addr) { return (uint32_t)((addr >> 9) * 0x9E3779B97F4A7C15ull >> 40) & (KEY_BUCKETS - 1); }
static void key_of(const NvnTexture *t, uint32_t k[7]) {
    k[0] = t->target; k[1] = t->format; k[2] = t->width; k[3] = t->height; k[4] = t->depth; k[5] = t->levels; k[6] = t->samples;
}
static TexBackend *keyed_find(const NvnTexture *t) {
    uint32_t k[7];
    key_of(t, k);
    for (TexBackend *b = g_keyed[key_bucket(t->addr)]; b; b = b->key_next)
        if (b->key_addr == t->addr && !memcmp(b->key, k, sizeof k)) return b;
    return NULL;
}
static void keyed_add(TexBackend *b, const NvnTexture *t) {
    b->key_addr = t->addr;
    key_of(t, b->key);
    uint32_t i = key_bucket(t->addr);
    b->key_next = g_keyed[i];
    g_keyed[i] = b;
}
static void keyed_remove(TexBackend *b) {
    if (!b->key_addr) return;
    for (TexBackend **p = &g_keyed[key_bucket(b->key_addr)]; *p; p = &(*p)->key_next)
        if (*p == b) { *p = b->key_next; break; }
    b->key_addr = 0;
}

// Lists that know each element's slot (index + 1, kept in the element; removal swaps the last one in)
#define SLOT_LIST(name, T, slot)                                                                                    \
    static T **g_##name;                                                                                            \
    static int g_n##name, g_cap_##name;                                                                             \
    static void name##_add(T *x) {                                                                                  \
        if (g_n##name == g_cap_##name) {                                                                            \
            g_cap_##name = g_cap_##name ? g_cap_##name * 2 : 1024;                                                  \
            g_##name = realloc(g_##name, g_cap_##name * sizeof *g_##name);                                          \
        }                                                                                                           \
        g_##name[g_n##name++] = x;                                                                                  \
        x->slot = g_n##name;                                                                                        \
    }                                                                                                               \
    static void name##_remove(T *x) {                                                                               \
        if (!x->slot) return;                                                                                       \
        T *last = g_##name[--g_n##name];                                                                            \
        g_##name[x->slot - 1] = last;                                                                               \
        last->slot = x->slot;                                                                                       \
        x->slot = 0;                                                                                                \
    }
SLOT_LIST(images, TexBackend, all)           // every image (an in-place reboot destroys them all)
SLOT_LIST(idle, TexBackend, idle)            // images no texture holds
SLOT_LIST(holders, NvnTexture, backend_slot) // textures holding an image

static uint64_t g_kept_bytes;

static void free_backend(TexBackend *b) {
    if (b->kept) g_kept_bytes -= b->kept_bytes;
    keyed_remove(b);
    idle_remove(b);
    images_remove(b);
    destroy_image(b);
    free(b->pending_rgba);
    free(b);
}

// Images the GPU drew have no copy in guest memory, and BOTW makes some of them again later and samples them without
// drawing them again, as on the console, where the texels stay in that memory: the save dialog's 256x144 picture
// (blitted from the final image the first time the dialog opens) came back black when the dialog was opened again.
// Past PARK_FRAMES such an image is kept, with a hash of the guest memory under it, up to KEEP_BUDGET bytes (the oldest
// go first); a texture made there again takes it back if that memory is unchanged (else the CPU reused it: a new image).
// SAVAGEWIND_NO_KEEP_IMAGES=1: freed after PARK_FRAMES, as before.
#define KEEP_MAX_IMAGE (16ull << 20)
#define KEEP_BUDGET (192ull << 20)
static uint64_t hash_full(const uint8_t *p, uint64_t n);
static uint64_t storage_size(const NvnTexture *t);

static bool keep_ok(TexBackend *b, uint64_t *bytes) {
    static int off = -1;
    if (off < 0) off = getenv("SAVAGEWIND_NO_KEEP_IMAGES") != NULL;
    if (off || !b->gpu_owned || !b->image || !b->mem || !b->guest_size || b->key_addr < GUEST_LO || b->key_addr + b->guest_size > GUEST_HI)
        return false;
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(g_vk.vma, b->mem, &ai);
    *bytes = ai.size;
    return ai.size <= KEEP_MAX_IMAGE;
}

static void idle_expire(void) {  // (gpu lock held)
    uint64_t now = renderer_present_count(), bytes;
    for (int i = g_nidle - 1; i >= 0; i--) {
        TexBackend *b = g_idle[i];
        if (b->kept || now - b->idle_frame <= PARK_FRAMES) continue;
        if (!keep_ok(b, &bytes)) { free_backend(b); continue; }
        b->kept = true;
        b->kept_bytes = bytes;
        b->kept_hash = hash_full((const uint8_t *)(uintptr_t)b->key_addr, b->guest_size);
        g_kept_bytes += bytes;
    }
    while (g_kept_bytes > KEEP_BUDGET) {
        TexBackend *old = NULL;
        for (int i = 0; i < g_nidle; i++)
            if (g_idle[i]->kept && (!old || g_idle[i]->idle_frame < old->idle_frame)) old = g_idle[i];
        if (!old) break;
        free_backend(old);
    }
}

static void release_backend(NvnTexture *t) {  // (gpu lock held)
    TexBackend *b = t->backend;
    if (!b) return;
    holders_remove(t);
    t->backend = NULL;
    if (--b->refs > 0) return;
    if (!b->key_addr || !b->image) { free_backend(b); return; }
    b->guest_size = storage_size(t);
    b->idle_frame = renderer_present_count();
    idle_add(b);
}

// Zombies: finalized textures that still got an image (sampled through an old pool entry or a copy). Nothing will
// finalize them again, so they let it go once unused for a few frames (BOTW's main menu, cycling its artwork, ran the
// GPU out of memory without this).
static NvnTexture **g_zombies;
static int g_nzombies, g_cap_zombies;

static void zombies_expire(void) {  // (gpu lock held)
    uint64_t now = renderer_present_count();
    int k = 0;
    for (int i = 0; i < g_nzombies; i++) {
        NvnTexture *t = g_zombies[i];
        TexBackend *b = t->backend;
        if (!b) continue;
        if (now - b->used_frame > PARK_FRAMES) release_backend(t);
        else g_zombies[k++] = t;
    }
    g_nzombies = k;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        if (t->finalized) b->used_frame = renderer_present_count();
        if (rs_stale(b)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    static uint64_t expired_at = ~0ull;  // the lists run to thousands in the open world: once a frame is enough
    if (expired_at != renderer_present_count()) {
        expired_at = renderer_present_count();
        vk_collect_dead();
        zombies_expire();
        idle_expire();
    }
    b = t->addr ? keyed_find(t) : NULL;
    if (b && b->kept) {  // (idle_expire) its contents, unless the CPU wrote that memory meanwhile
        bool same = b->guest_size == storage_size(t) && hash_full((const uint8_t *)(uintptr_t)b->key_addr, b->guest_size) == b->kept_hash;
        static int nlog = -1;
        if (nlog < 0) nlog = getenv("SAVAGEWIND_LOG_KEPT") ? 20 : 0;
        if (nlog > 0) {
            nlog--;
            LOG("[vk] kept image of #%llu %ux%u fmt %#x at %llx %s after %llu frames\n", (unsigned long long)t->debug_id, t->width,
                t->height, t->format, (unsigned long long)t->addr, same ? "taken back" : "dropped (memory changed)",
                (unsigned long long)(renderer_present_count() - b->idle_frame));
        }
        g_kept_bytes -= b->kept_bytes;
        b->kept = false;
        if (!same) { free_backend(b); b = NULL; }
    }
    if (b && rs_stale(b)) { destroy_image(b); create_image(t, b); }
    if (!b) {
        b = calloc(1, sizeof *b);
        b->rs_group = tex_scale_group(t);
        create_image(t, b);
        images_add(b);
        if (t->addr) keyed_add(b, t);
    }
    idle_remove(b);
    b->refs++;
    t->backend = b;
    holders_add(t);
    if (t->finalized) {
        b->used_frame = renderer_present_count();
        if (g_nzombies == g_cap_zombies) {
            g_cap_zombies = g_cap_zombies ? g_cap_zombies * 2 : 64;
            g_zombies = realloc(g_zombies, g_cap_zombies * sizeof *g_zombies);
        }
        g_zombies[g_nzombies++] = t;
    }
    return b;
}

void backend_texture_destroyed(NvnTexture *t) {
    gpu_lock();
    release_backend(t);
    gpu_unlock();
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nimages;
    while (g_nholders > 0) release_backend(g_holders[g_nholders - 1]);
    while (g_nimages > 0) free_backend(g_images[g_nimages - 1]);
    g_nzombies = 0;
    vk_collect_dead();
    LOG("[vk] reboot: %d images released\n", n);
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// 8 bytes at each of up to 2048 places (a byte at each of 4096 was a cache miss per byte: ~50 textures a frame are
// sampled in the open world).
static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t words = n / 8, step = words > 2048 ? words / 2048 : 1;
    for (uint64_t i = 0; i < words; i += step) { uint64_t w; memcpy(&w, p + i * 8, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
    for (uint64_t i = words * 8; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_scaled(VkCommandBuffer cb, TexBackend *b, VkSpan s, VkBufferImageCopy rg, uint32_t gw, uint32_t gh);

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    bool scaled = b->scalable && (w != b->width || h != b->height);
    if (!scaled && w > b->width) w = b->width;
    if (!scaled && h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    if (scaled) { upload_scaled(cb, b, s, rg, w, h); return; }
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
static void upload_levels(NvnTexture *t, TexBackend *b, uint32_t level_mask);

// Bytes of guest storage the texels occupy: rows at the stride for pitch textures, else the block-linear size.
static uint64_t storage_size(const NvnTexture *t) {
    if (nvn_texture_flags_pitch(t->flags) && t->stride && t->target != 2)
        return t->stride * (t->layout.height ? t->layout.height : 1) * (t->layout.layers ? t->layout.layers : 1);
    return tegra_total_size(&t->layout);
}

// Whether the texture's storage is readable guest memory (a virtual pool may have been mapped since it was made).
static bool storage_readable(NvnTexture *t, uint64_t total) {
    if (t->addr < GUEST_LO && t->pool && t->pool->storage) t->addr = t->pool->storage + t->pool_offset;
    return t->addr >= GUEST_LO && t->addr + total <= GUEST_HI;
}

// written: nvnTextureWriteTexels changed the texels (checked now, and that change does not make the texture dynamic)
static void upload_texture(NvnTexture *t, TexBackend *b, bool written) {
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    if (!written && b->uploaded && b->checked_frame == frame) return;
    if (!written && b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    uint64_t total = storage_size(t);
    if (!storage_readable(t, total)) {  // storage outside guest memory: nothing to read
        static int logged;
        if (logged++ < 8)
            LOG("[vk] texture %llx (%ux%u fmt %u, %llu bytes) has storage %llx outside guest memory (pool %llx: %llx+%llx, "
                "offset %llx)\n", (unsigned long long)t->debug_id, t->width, t->height, t->format,
                (unsigned long long)total, (unsigned long long)t->addr, (unsigned long long)(uintptr_t)t->pool,
                (unsigned long long)(t->pool ? t->pool->storage : 0), (unsigned long long)(t->pool ? t->pool->size : 0),
                (unsigned long long)t->pool_offset);
        b->checked_frame = frame;
        return;
    }
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("SAVAGEWIND_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    if (!full && frame - b->full_frame >= 120 && full_hash_budget >= total) { full = true; full_hash_budget -= total; }
    if (!full) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    uint64_t h = hash_full(mem, total);
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    if (b->uploaded && h == b->data_hash) return;
    if (b->uploaded && h != b->data_hash && !written) b->dynamic = true;
    b->data_hash = h;
    b->uploaded = true;
    {   // (debugging) SAVAGEWIND_DUMP_RAW=<w>x<h>x<nvn format hex>: the guest storage of such textures as uploaded
        static const char *e = (const char *)1;
        if (e == (const char *)1) e = getenv("SAVAGEWIND_DUMP_RAW");
        unsigned w, hh, f;
        if (e && sscanf(e, "%ux%ux%x", &w, &hh, &f) == 3 && t->width == w && t->height == hh && t->format == f) {
            static int n;
            char path[160];
            snprintf(path, sizeof path, "build/raw_%llu_%u_%ux%u_f%x_l%u_d%u_lv%u_bh%u.bin", (unsigned long long)t->debug_id, n++,
                     t->width, t->height, t->format, t->layout.layers, t->layout.depth, t->layout.levels, t->layout.block_height_log2);
            FILE *fp = n <= 8 ? fopen(path, "wb") : NULL;
            if (fp) {
                fwrite(mem, 1, total, fp);
                fclose(fp);
                LOG("[vk] texture #%llu flags %#x stride %llu storage saved as %s\n", (unsigned long long)t->debug_id, t->flags,
                    (unsigned long long)t->stride, path);
            }
        }
    }
    upload_levels(t, b, ~0u);
}

// Guest texels for a scaled image (the CPU filled a render target: the title screen's save pictures): they go into a
// guest-size image of their own, then a filtered blit brings them to the allocation's size.
static void upload_scaled(VkCommandBuffer cb, TexBackend *b, VkSpan s, VkBufferImageCopy rg, uint32_t gw, uint32_t gh) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                            .extent = {gw, gh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return;
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    uint32_t lv = rg.imageSubresource.mipLevel, layer = rg.imageSubresource.baseArrayLayer;
    rg.imageSubresource.mipLevel = 0;
    rg.imageSubresource.baseArrayLayer = 0;
    rg.imageExtent = (VkExtent3D){gw, gh, 1};
    vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)gw, (int32_t)gh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                      .dstOffsets = {{0, 0, 0}, {(int32_t)level_w(b, lv), (int32_t)level_h(b, lv), 1}}};
    bool lin = format_supports(b->format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_defer_image(img, mem);
}

// Guest memory -> image for the levels in level_mask (bit per level), every layer or slice.
static void upload_levels(NvnTexture *t, TexBackend *b, uint32_t level_mask) {
    const FormatInfo *fi = t->layout.fmt;
    VkFmt mf = fmt_of(t->format);
    if (!t->addr || t->target == 10 || !fi || !fi->bpb || !storage_readable(t, storage_size(t))) return;
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    bool linear = nvn_texture_flags_pitch(t->flags);
    uint64_t layer_size = linear ? t->stride * (t->layout.height ? t->layout.height : 1) : tegra_layer_size(&t->layout);
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            if (!((level_mask >> lv) & 1)) continue;
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * (t->stride ? t->stride : row), row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            if (lv == 0 && layer == 0 && t->format == 0x49 && t->pool &&  // (the game's button glyphs: device icons)
                glyphs_apply(t->addr, t->pool->storage, t->pool->storage + t->pool->size, t->format, t->width,
                             t->height, lin, l.wb, l.hb))
                b->glyph_gen = glyphs_generation();
            if (lv == 0 && layer == 0 && t->format == 0x01 && !linear &&  // (the HUD's button textures, likewise)
                glyphs_apply_layout(t->format, l.w, l.h, lin, row))
                b->glyph_gen = glyphs_generation();
            if (t->target == 2 && lv == 0 && getenv("SAVAGEWIND_DUMP_TEX3D")) {  // (debugging) 3D texels as uploaded
                char path[128];
                snprintf(path, sizeof path, "build/tex3d_%llu_%ux%ux%u_f%x.raw", (unsigned long long)t->debug_id, l.w, l.h, l.d,
                         t->format);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(lin, 1, sz, f);
                    fclose(f);
                    LOG("[vk] 3D texture #%llu at %llx flags %#x stride %llu levels %u bh %u bd %u saved as %s\n",
                        (unsigned long long)t->debug_id, (unsigned long long)t->addr, t->flags, (unsigned long long)t->stride, t->levels,
                        l.block_height_log2, l.block_depth_log2, path);
                }
            }
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            if (b->scalable && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->depth == 1 &&
                (l.w != level_w(b, lv) || l.h != level_h(b, lv))) {
                upload_scaled(cb, b, s, rg, l.w, l.h);
                continue;
            }
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
}

// (debugging) SAVAGEWIND_TRACE_TEX=<w>x<h>[,<w>x<h>...] or all: textures keep a history of their last writes (draws
// into them with the textures those sample, copies, guest-memory uploads). When one is copied to a buffer (the CPU
// reads it: a screenshot), its history is logged, then those of the textures it was made from, 4 levels deep.
static int trace_mode = -1;  // 0 off, 1 the listed sizes, 2 every texture
static int trace_w[8], trace_h[8], trace_nsizes;
bool vk_trace_tex(NvnTexture *t) {
    if (trace_mode < 0) {
        const char *e = getenv("SAVAGEWIND_TRACE_TEX");
        trace_mode = !e ? 0 : !strcmp(e, "all") ? 2 : 1;
        for (const char *p = e; trace_mode == 1 && p && *p && trace_nsizes < 8;) {
            if (sscanf(p, "%dx%d", &trace_w[trace_nsizes], &trace_h[trace_nsizes]) == 2) trace_nsizes++;
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    if (!trace_mode || !t) return false;
    if (trace_mode == 2) return true;
    for (int i = 0; i < trace_nsizes; i++)
        if ((int)t->width == trace_w[i] && (int)t->height == trace_h[i]) return true;
    return false;
}
#define TRACE_EVENTS 8
#define TRACE_SRCS 6
typedef struct {
    uint64_t mem;  // the texels' guest address (objects over the same memory share it), else the object
    unsigned n;
    uint64_t last_frame, last_key;  // an event repeated in a frame with the same key (a draw's shader) is kept once
    NvnTexture *src[TRACE_SRCS];    // the textures it was last made from (sampled by draws into it, copied)
    unsigned nsrc;
    char ev[TRACE_EVENTS][320];
} TraceHist;
static TraceHist *trace_tab;
#define TRACE_CAP (1u << 14)
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t trace_mem(NvnTexture *t) { return t->addr ? t->addr : (uint64_t)(uintptr_t)t; }
static TraceHist *trace_entry(NvnTexture *t) {  // (lock held)
    if (!trace_tab && !(trace_tab = calloc(TRACE_CAP, sizeof *trace_tab))) return NULL;
    uint64_t m = trace_mem(t);
    uint32_t i = (uint32_t)((m >> 4) * 0x9E3779B97F4A7C15ull >> 40) & (TRACE_CAP - 1);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (TRACE_CAP - 1)) {
        if (trace_tab[i].mem == m) return &trace_tab[i];
        if (!trace_tab[i].mem) { trace_tab[i].mem = m; return &trace_tab[i]; }
    }
    return NULL;
}
void vk_trace_event(NvnTexture *t, uint64_t key, NvnTexture *const *srcs, int nsrcs, const char *fmt, ...) {
    if (!vk_trace_tex(t)) return;
    pthread_mutex_lock(&trace_lock);
    TraceHist *h = trace_entry(t);
    uint64_t frame = vk_frame_counter();
    if (h && !(key && h->n && h->last_frame == frame && h->last_key == key)) {
        h->last_frame = frame;
        h->last_key = key;
        char *e = h->ev[h->n++ % TRACE_EVENTS];
        int o = snprintf(e, sizeof h->ev[0], "frame %llu: #%llu f%#x ", (unsigned long long)frame,
                         (unsigned long long)t->debug_id, t->format);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e + o, sizeof h->ev[0] - (size_t)o, fmt, ap);
        va_end(ap);        for (int i = 0; i < nsrcs; i++) {
            bool have = !srcs[i] || trace_mem(srcs[i]) == trace_mem(t);
            for (unsigned k = 0; k < TRACE_SRCS && !have; k++) have = h->src[k] == srcs[i];
            if (!have) h->src[h->nsrc++ % TRACE_SRCS] = srcs[i];
        }
    }
    pthread_mutex_unlock(&trace_lock);
}
static void trace_dump(NvnTexture *t, int depth, NvnTexture **done, int *ndone) {  // (lock held)
    for (int i = 0; i < *ndone; i++) if (trace_mem(done[i]) == trace_mem(t)) return;
    if (*ndone < 64) done[(*ndone)++] = t;
    TraceHist *h = trace_entry(t);
    unsigned n = h ? h->n : 0, first = n > TRACE_EVENTS ? n - TRACE_EVENTS : 0;
    LOG("[trace] %*s#%llu %ux%u f%#x flags %#x target %u levels %u at %llx: last %u of %u events\n", depth * 2, "",
        (unsigned long long)t->debug_id, t->width, t->height, t->format, t->flags, t->target, t->levels,
        (unsigned long long)t->addr, n - first, n);
    for (unsigned j = first; j < n; j++) LOG("[trace] %*s  %s\n", depth * 2, "", h->ev[j % TRACE_EVENTS]);
    if (depth < 4 && h)
        for (unsigned k = 0; k < TRACE_SRCS && k < h->nsrc; k++) trace_dump(h->src[k], depth + 1, done, ndone);
}
void vk_trace_dump(NvnTexture *t) {
    if (!vk_trace_tex(t)) return;
    NvnTexture *done[64];
    int ndone = 0;
    pthread_mutex_lock(&trace_lock);
    trace_dump(t, 0, done, &ndone);
    pthread_mutex_unlock(&trace_lock);
    // and what they hold now: build/trace_<n>_<id>.png (the copied texture first, then its sources in dump order)
    extern void renderer_save_texture_tex(TexBackend *b, const char *path);
    static int saves;
    for (int i = 0; i < ndone && i < 6 && saves < 4; i++) {
        if (!done[i]->backend) continue;
        char path[96];
        snprintf(path, sizeof path, "build/trace_%d_%llu.png", saves * 10 + i, (unsigned long long)done[i]->debug_id);
        renderer_save_texture_tex(done[i]->backend, path);
    }
    saves++;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (!for_render && !b->gpu_owned && vk_trace_tex(t))  // (CPU-owned: guest memory is uploaded when it changed)
        vk_trace_event(t, 1, NULL, 0, "use: CPU-owned, uploaded %d dynamic %d", b->uploaded, b->dynamic);
    if (b->pending_rgba) upload_pending(b);
    if (b->glyph_gen && b->glyph_gen != glyphs_generation()) upload_levels(t, b, 1);  // (button icons: other device)
    // Levels nvnTextureWriteTexels filled go up now, also into an image the GPU draws into: BOTW writes level 0 of its
    // 32x32x32 noise that way and then renders the smaller levels from it (the noise stayed black: GPU-owned images
    // never read guest memory again).
    uint32_t written = __atomic_exchange_n(&t->cpu_levels, 0, __ATOMIC_ACQ_REL);
    if (written) {
        if (b->gpu_owned) upload_levels(t, b, written);
        else upload_texture(t, b, true);
    }
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b, false);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend(t)->gpu_owned = true; }

// Guest memory -> image for every level, whoever owns the image (exec.c: linear render targets the CPU wrote).
void vk_texture_reload(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    upload_levels(t, b, ~0u);
}
uint64_t vk_hash_bytes(const void *p, uint64_t n) { return hash_full(p, n); }

bool renderer_texture_scalable(NvnTexture *t) { return tex_backend(t)->scalable; }

// Render at the output scale from now on: a target drawn into together with scaled ones (its contents so far, kept
// at the guest size, are dropped: the pass about to run writes it).
void renderer_make_scalable(NvnTexture *t) { vk_make_scalable_like(t, NULL); }

// ... at the scale of `like` (another attachment of the same pass; NULL: the scene's)
void vk_make_scalable_like(NvnTexture *t, NvnTexture *like) {
    TexBackend *b = tex_backend(t);
    uint8_t g = like ? tex_backend(like)->rs_group : RS_SCENE;
    if (b->rs_group || !g || (t->target != 1 && t->target != 4) || t->samples > 1) return;  // (a group stays)
    b->rs_group = g;
    destroy_image(b);
    create_image(t, b);
}

uint8_t vk_texture_scale_group(NvnTexture *t) { return tex_backend(t)->rs_group; }
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

static void evict_idle_images(void) {
    for (int i = g_nidle - 1; i >= 0; i--) {
        TexBackend *b = g_idle[i];
        if (b->gpu_owned || b->kept || b->pending_rgba) continue;
        // The caller drained the GPU; these unreferenced images can be rebuilt from guest memory.
        vk_view_gen++;
        for (int j = 0; j < VIEW_TAB; j++)
            if (view_tab[j].b == b) {
                vkDestroyImageView(g_vk.dev, view_tab[j].view, NULL);
                view_tab[j].b = NULL;
                view_tab[j].view = VK_NULL_HANDLE;
            }
        keyed_remove(b);
        idle_remove(b);
        images_remove(b);
        if (b->image) vmaDestroyImage(g_vk.vma, b->image, b->mem);
        free(b->pending_rgba);
        free(b);
    }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// The component mapping a sampled colour texture is read with: its view's NVN swizzle (or the texture's), and alpha 1
// for formats that have none.
VkComponentMapping vk_sample_swizzle(NvnTexture *t, const NvnTextureView *v) {
    VkComponentMapping sw = IDENTITY;
    const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
    uint8_t c[4];
    for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
    if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
        if (fmt_of(t->format).alpha_one)
            for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
        // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
        sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                  (VkComponentSwizzle)(c[3] + 1)};
    } else if (fmt_of(t->format).alpha_one) {
        sw.a = VK_COMPONENT_SWIZZLE_ONE;
    }
    return sw;
}

static bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

// Attachment view for a draw into the blocks of BC image b, a texel a 4x4 block in format fmt (the game's view: RG32
// for 8-byte blocks): a view of b->block, made for it, that vk_block_flush copies into b after the pass.
static VkImageView block_view(TexBackend *b, VkFormat fmt) {
    TexBackend *s = b->block;
    if (s && s->format != fmt) {
        destroy_image(s);
        free(s);
        s = b->block = NULL;
    }
    if (!s) {
        if (!format_supports(fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return VK_NULL_HANDLE;
        s = calloc(1, sizeof *s);
        s->format = fmt;
        s->width = (b->width + 3) / 4, s->height = (b->height + 3) / 4;
        s->depth = s->layers = s->levels = s->samples = 1;
        s->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        s->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                                .extent = {s->width, s->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &s->image, &s->mem, NULL) != VK_SUCCESS) {
            free(s);
            return VK_NULL_HANDLE;
        }
        vk_image_init_layout(vk_upload_cmd(), s->image, VK_IMAGE_ASPECT_COLOR_BIT);
        b->block = s;
    }
    b->block_pending = true;
    return vk_image_view(s, VK_IMAGE_VIEW_TYPE_2D, fmt, 0, 1, 0, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
}

// A texel of b->block is a block of b: an image copy between the two formats moves texels into blocks as they are
void vk_block_flush(VkCommandBuffer cb, TexBackend *b) {
    TexBackend *s = b->block;
    if (!s) return;
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {s->width, s->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
}

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render && fmt != b->format && is_bc(b->format) && !is_bc(fmt) && lv0 == 0 && b->layers == 1 && b->depth == 1)
        return block_view(b, fmt);  // (drawn into through a view of its blocks)
    if (render) {
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
            // One slice of one level (the view's layer is the slice), as a 2D view of the 2D-array-compatible image.
            // BOTW renders the levels of its 3D noise and an 8x8x8 light volume slice by slice.
            uint32_t z = v && (v->setmask & 2) ? v->min_layer : 0, d = b->depth >> lv0 ? b->depth >> lv0 : 1;
            if (!b->slices_renderable || z >= d) return VK_NULL_HANDLE;
            return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, z, 1, IDENTITY, b->aspect);
        }
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) sw = vk_sample_swizzle(t, v);
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    // a cube read as a cube array of one (BOTW's 64x64 R11G11B10 light probes): it was black
    if (want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && (have == VK_IMAGE_VIEW_TYPE_CUBE || have == VK_IMAGE_VIEW_TYPE_2D_ARRAY) &&
        b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, b->layers / 6 * 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// Video > Anisotropic filtering (g_settings.anisotropy): 0 keeps the game's own max anisotropy (the console's look);
// 2x .. 16x override it on texture samplers that filter linearly between mip levels or within one (ground, walls,
// paths at a distance). Left alone: point-filtered samplers (pixel-exact data, some UI), samplers without mipmaps (the
// UI, render targets read back), ones clamped to one mip level (BOTW reads its bloom and blur chains that way) and
// depth-comparison ones (shadow maps). And whatever the sampler (aniso_ok false, exec.c texture_info): textures with
// one mip level - BOTW reads its render targets with samplers allowing the whole mip chain, and filtered that way
// they put stepped light outlines along edges (around Link's bow and quiver most) - and the blended draws:
// particles, effects, decals, whose quads' square edges the wider footprint brought out (floating dust as small
// squares). Both the worse the higher the setting.
static float sampler_aniso(const NvnSamplerDesc *d, bool aniso_ok) {
    float game = d->max_aniso > 1 ? fminf(d->max_aniso, 16) : 1;
    int a = g_settings.anisotropy;
    if (a <= 0 || a > 4 || !g_vk.feat_aniso || !aniso_ok) return game;
    if (d->compare_mode || d->mag_filter != 1 || (d->min_filter != 3 && d->min_filter != 5)) return game;
    if (d->lod_max - d->lod_min < 0.5f) return game;
    return fminf((float)(1 << a), g_vk.props.limits.maxSamplerAnisotropy);
}

// One VkSampler per distinct setting (pool entries repeat the same few hundred), so a change of the anisotropy makes
// new ones without destroying samplers that submitted work may still use.
static struct { NvnSamplerDesc key; VkSampler s; } *g_samplers;
static int g_nsamplers, g_cap_samplers;
static pthread_mutex_t g_sampler_lock = PTHREAD_MUTEX_INITIALIZER;

static VkSampler sampler_create(const NvnSamplerDesc *d, float aniso);

VkSampler vk_sampler(NvnSamplerDesc *d, bool aniso_ok) {
    float an = sampler_aniso(d, aniso_ok);
    void **slot = aniso_ok ? &d->backend : &d->backend_plain;
    float *slot_aniso = aniso_ok ? &d->backend_aniso : &d->backend_plain_aniso;
    if (*slot && *slot_aniso == an) return (VkSampler)*slot;
    NvnSamplerDesc key = *d;
    key.backend = key.backend_plain = NULL;
    key.backend_aniso = an, key.backend_plain_aniso = 0;
    pthread_mutex_lock(&g_sampler_lock);
    VkSampler s = VK_NULL_HANDLE;
    for (int i = 0; i < g_nsamplers && !s; i++)
        if (!memcmp(&g_samplers[i].key, &key, sizeof key)) s = g_samplers[i].s;
    if (!s) {
        s = sampler_create(d, an);
        if (g_nsamplers == g_cap_samplers) {
            g_cap_samplers = g_cap_samplers ? g_cap_samplers * 2 : 256;
            g_samplers = realloc(g_samplers, g_cap_samplers * sizeof *g_samplers);
        }
        g_samplers[g_nsamplers].key = key;
        g_samplers[g_nsamplers++].s = s;
        static int log = -1;
        if (log < 0) log = getenv("SAVAGEWIND_LOG_SAMPLERS") != NULL;
        if (log) LOG("[sampler] #%d min %u mag %u wrap %u %u cmp %u lod %.2f..%.2f bias %.2f aniso game %.0f -> %.0f\n",
                     g_nsamplers, d->min_filter, d->mag_filter, d->wrap[0], d->wrap[1], d->compare_mode, d->lod_min,
                     d->lod_max, d->lod_bias, d->max_aniso, an);
    }
    pthread_mutex_unlock(&g_sampler_lock);
    *slot = (void *)s;
    *slot_aniso = an;
    return s;
}

static VkSampler sampler_create(const NvnSamplerDesc *d, float aniso) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = aniso; }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("SAVAGEWIND_LOD_MIN") ? (float)atof(getenv("SAVAGEWIND_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("SAVAGEWIND_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("SAVAGEWIND_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
