// Dear ImGui's Vulkan backend on our device (start screen and in-game menu; both draw inside the window image's
// dynamic rendering begun by vk_frame_begin).
#include <vector>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "vk.h"
#include "record.h"
}

// Per ImGui context (the start screen and the in-game menu each have one, and the start screen comes back after an
// in-place reboot while the menu's context lives on): the backend keeps its data in the current context.
extern "C" bool vk_imgui_backend_init(void) {
    if (ImGui::GetIO().BackendRendererUserData) return true;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_vk.inst;
    ii.PhysicalDevice = g_vk.phys;
    ii.Device = g_vk.dev;
    ii.QueueFamily = g_vk.qfam;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = 2;
    // vertex buffers rotate per image: cover frames in flight (twice: frame generation draws the menu on both frames)
    ii.ImageCount = 2 * (g_vk.sc_count > 3 ? g_vk.sc_count : 3) + 1;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = g_vk.pcache;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk.sc_format;
    bool ok = ImGui_ImplVulkan_Init(&ii);
    if (!ok) LOG("[vk] ImGui Vulkan backend init failed\n");
    return ok;
}

// The current context's fonts were made again: its font texture goes once no submitted frame can still read it, and
// the next NewFrame makes the new one.
extern "C" void vk_imgui_fonts_changed(void) {
    if (!ImGui::GetIO().BackendRendererUserData) return;
    uint64_t pending = vk_pending_seq();
    if (pending > 1) vk_wait_seq(pending - 1);
    ImGui_ImplVulkan_DestroyFontsTexture();
}

extern "C" void vk_imgui_backend_shutdown(void) {
    if (!ImGui::GetIO().BackendRendererUserData) return;
    vk_present_idle();
    rec_drain();
    vkDeviceWaitIdle(g_vk.dev);
    ImGui_ImplVulkan_Shutdown();
}

// A static RGBA8 image for ImGui (the start screen's picture, runtime/ui/splash.mm). Returns the ImTextureID; 0 when it
// couldn't be made. vk_imgui_texture_free gives it back (before the backend shuts down: its descriptor is the backend's).
struct ImGuiTex { uint64_t id; VkImage img; VkImageView view; VmaAllocation mem; };
static std::vector<ImGuiTex> g_textures;

extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return 0;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSpan s = vk_transient_copy(rgba, (uint64_t)w * h * 4, 16);
    VkBufferImageCopy rg = {};
    rg.bufferOffset = s.off;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    rec_CmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &view) != VK_SUCCESS) return 0;
    uint64_t id = (uint64_t)ImGui_ImplVulkan_AddTexture(vk_default_sampler(true), view, VK_IMAGE_LAYOUT_GENERAL);
    if (!id) { vkDestroyImageView(g_vk.dev, view, NULL); return 0; }  // (no descriptor left: rare, the image is left)
    g_textures.push_back({id, img, view, mem});
    return id;
}

extern "C" void vk_imgui_texture_free(uint64_t id) {
    for (size_t i = 0; i < g_textures.size(); i++) {
        if (g_textures[i].id != id) continue;
        rec_drain();
        vkDeviceWaitIdle(g_vk.dev);  // (a frame in flight may still draw it)
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
        vkDestroyImageView(g_vk.dev, g_textures[i].view, NULL);
        vmaDestroyImage(g_vk.vma, g_textures[i].img, g_textures[i].mem);
        g_textures.erase(g_textures.begin() + i);
        return;
    }
}
