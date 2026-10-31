// Headless GPU regression: an independently compiled fragment library must read set 1's dynamic UBO,
// even though set 0 (not known to that library) also has dynamic UBOs. No game data is needed.
// Build with: cc tools/test_vk_gpl_dynamic.c -o test_vk_gpl_dynamic -lvulkan -lshaderc_shared
// Default tests the workaround and monolithic control. --dynamic-fragment also tests the broken path.
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { VkResult r = (expr); if (r != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #expr, r); exit(2); } } while (0)
static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static uint32_t family;
static VkPipelineCache cache;
static VkShaderModule vs, fs;
static VkCommandPool pool;
static VkDescriptorPool dpool;
static VkBuffer buf;
static VkDeviceMemory mem;
static void *mapped;
static VkImage image;
static VkImageView view;
static VkDeviceSize alignment;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "no memory type %#x\n", flags); exit(2);
}
static VkShaderModule shader(const char *src, shaderc_shader_kind kind) {
    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, src, strlen(src), kind, "fixture", "main", o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        fprintf(stderr, "%s\n", shaderc_result_get_error_message(r)); exit(2);
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaderc_result_get_length(r), .pCode = (const uint32_t *)shaderc_result_get_bytes(r)};
    VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    shaderc_result_release(r); shaderc_compile_options_release(o); shaderc_compiler_release(c);
    return m;
}
static VkPipelineLayout layout(VkDescriptorSetLayout a, VkDescriptorSetLayout b, int n, bool independent) {
    VkDescriptorSetLayout sets[] = {a, b};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .flags = independent ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0, .setLayoutCount = n, .pSetLayouts = sets};
    VkPipelineLayout p; CHECK(vkCreatePipelineLayout(dev, &ci, NULL, &p)); return p;
}
static VkPipeline pipeline(VkGraphicsPipelineCreateInfo *ci, uint32_t flags) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = ci->pNext, .flags = flags};
    if (flags) { ci->pNext = &li; ci->flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR; }
    ci->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    VkPipeline p; CHECK(vkCreateGraphicsPipelines(dev, cache, 1, ci, NULL, &p)); return p;
}
static int draw(bool libraries, bool dynamic_fragment) {
    VkDescriptorSetLayout sl[2];
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutBinding b = {.binding = 3, .descriptorCount = 1,
            .descriptorType = i == 0 || dynamic_fragment ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT};
        VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        CHECK(vkCreateDescriptorSetLayout(dev, &ci, NULL, &sl[i]));
    }
    VkPipelineLayout full = layout(sl[0], sl[1], 2, libraries);
    VkPipelineLayout vl = libraries ? layout(sl[0], VK_NULL_HANDLE, 1, true) : full;
    VkPipelineLayout fl = libraries ? layout(VK_NULL_HANDLE, sl[1], 2, true) : full;
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, 1, 1, 0, 1}; VkRect2D sc = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkPipeline p, libs[4];
    if (libraries) {
        VkGraphicsPipelineCreateInfo c0 = {.pVertexInputState = &vi, .pInputAssemblyState = &ia};
        libs[0] = pipeline(&c0, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
        VkGraphicsPipelineCreateInfo c1 = {.stageCount = 1, .pStages = stages, .pViewportState = &viewport, .pRasterizationState = &rs, .layout = vl};
        libs[1] = pipeline(&c1, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
        VkGraphicsPipelineCreateInfo c2 = {.stageCount = 1, .pStages = stages + 1, .pMultisampleState = &ms, .pDepthStencilState = &ds, .layout = fl};
        libs[2] = pipeline(&c2, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
        VkGraphicsPipelineCreateInfo c3 = {.pNext = &ri, .pMultisampleState = &ms, .pColorBlendState = &blend};
        libs[3] = pipeline(&c3, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT);
        VkPipelineLibraryCreateInfoKHR link = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = libs};
        VkGraphicsPipelineCreateInfo ci = {.pNext = &link, .layout = full}; p = pipeline(&ci, 0);
    } else {
        VkGraphicsPipelineCreateInfo ci = {.pNext = &ri, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &viewport, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pDepthStencilState = &ds, .pColorBlendState = &blend, .layout = full}; p = pipeline(&ci, 0);
    }
    VkDescriptorSetAllocateInfo sai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 2, .pSetLayouts = sl};
    VkDescriptorSet sets[2]; CHECK(vkAllocateDescriptorSets(dev, &sai, sets));
    for (int i = 0; i < 2; i++) {
        bool dynamic = i == 0 || dynamic_fragment;
        VkDescriptorBufferInfo bi = {buf, dynamic ? 0 : alignment * 2, 16};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 3,
            .descriptorCount = 1, .descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &begin));
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo render = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &render); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    uint32_t offsets[] = {alignment, alignment * 2};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, full, 0, 2, sets, dynamic_fragment ? 2 : 1, offsets);
    vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRendering(cb);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL; ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy copy = {.bufferOffset = alignment * 3, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &copy);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    float *result = (float *)((char *)mapped + alignment * 3);
    printf("%s, fragment %s: %.9g %.9g %.9g %.9g (expected 7 11 13 17)\n", libraries ? "independent GPL" : "whole", dynamic_fragment ? "dynamic" : "static", result[0], result[1], result[2], result[3]);
    int bad = result[0] != 7 || result[1] != 11 || result[2] != 13 || result[3] != 17;
    vkDestroyPipeline(dev, p, NULL);
    if (libraries) { for (int i = 0; i < 4; i++) vkDestroyPipeline(dev, libs[i], NULL); vkDestroyPipelineLayout(dev, vl, NULL); vkDestroyPipelineLayout(dev, fl, NULL); }
    vkDestroyPipelineLayout(dev, full, NULL);
    for (int i = 0; i < 2; i++) vkDestroyDescriptorSetLayout(dev, sl[i], NULL);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    return bad;
}
int main(int argc, char **argv) {
    bool reproduce = argc == 2 && !strcmp(argv[1], "--dynamic-fragment");
    if (argc > 2 || (argc == 2 && !reproduce)) { fprintf(stderr, "usage: %s [--dynamic-fragment]\n", argv[0]); return 2; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t n = 1; CHECK(vkEnumeratePhysicalDevices(instance, &n, &phys));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props); printf("device: %s\n", props.deviceName);
    alignment = props.limits.minUniformBufferOffsetAlignment; if (alignment < 256) alignment = 256;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL); VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, q);
    for (family = 0; family < nq && !(q[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++) {} free(q);
    if (family == nq) return 2;
    float priority = 1; VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &gpl, .dynamicRendering = VK_TRUE};
    const char *ext[] = {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev)); vkGetDeviceQueue(dev, family, 0, &queue);
    VkPipelineCacheCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; CHECK(vkCreatePipelineCache(dev, &pci, NULL, &cache));
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family}; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 2, .pPoolSizes = sizes}; CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = alignment * 4, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT}; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &alloc, NULL, &mem)); CHECK(vkBindBufferMemory(dev, buf, mem, 0)); CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
    memset(mapped, 0, alignment * 4);
    float position[] = {1, 0, 0, 0}, color[] = {7, 11, 13, 17};
    memcpy((char *)mapped + alignment, position, 16); memcpy((char *)mapped + alignment * 2, color, 16);
    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}; CHECK(vkCreateImage(dev, &imci, NULL, &image));
    vkGetImageMemoryRequirements(dev, image, &req); alloc.allocationSize = req.size; alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    VkDeviceMemory imem; CHECK(vkAllocateMemory(dev, &alloc, NULL, &imem)); CHECK(vkBindImageMemory(dev, image, imem, 0));
    VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}; CHECK(vkCreateImageView(dev, &ivci, NULL, &view));
    vs = shader("#version 450\nlayout(set=0,binding=3) uniform V { vec4 p; }; void main(){ vec2 v=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4((v*2-1)*p.x,0,1); }", shaderc_vertex_shader);
    fs = shader("#version 450\nlayout(set=1,binding=3) uniform F { vec4 color; }; layout(location=0) out vec4 o; void main(){ o=color; }", shaderc_fragment_shader);
    int failures = draw(false, true) + draw(true, false);
    if (reproduce) failures += draw(true, true);
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyImageView(dev, view, NULL); vkDestroyImage(dev, image, NULL); vkFreeMemory(dev, imem, NULL);
    vkUnmapMemory(dev, mem); vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL); vkDestroyCommandPool(dev, pool, NULL); vkDestroyPipelineCache(dev, cache, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(instance, NULL);
    return failures ? 1 : 0;
}
