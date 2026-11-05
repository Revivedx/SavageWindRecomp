// Headless, game-free regression for shader access beyond maxStorageBufferRange.
// cc -std=c11 -O2 -Wall -Wextra -Werror tools/test_vk_heap_range.c -lvulkan -lshaderc_shared
// Default checks legal windowed SSBO access; --physical checks bounded physical pointers.
// --oversized reproduces the invalid whole-heap descriptor (an expected validation error).
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r = (x); if (r) { fprintf(stderr, "%s: %d\n", #x, r); exit(2); } } while (0)
static VkDevice dev;
static uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return i;
    fprintf(stderr, "no coherent host memory\n"); exit(2);
}
static VkShaderModule shader(const char *src) {
    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, src, strlen(src), shaderc_compute_shader, "heap-range", "main", o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) { fprintf(stderr, "%s\n", shaderc_result_get_error_message(r)); exit(2); }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaderc_result_get_length(r), .pCode = (const uint32_t *)shaderc_result_get_bytes(r)};
    VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    shaderc_result_release(r); shaderc_compile_options_release(o); shaderc_compiler_release(c); return m;
}
int main(int argc, char **argv) {
    bool oversized = argc == 2 && !strcmp(argv[1], "--oversized");
    bool physical = argc == 2 && !strcmp(argv[1], "--physical");
    if (argc > 2 || (argc == 2 && !oversized && !physical)) return 2;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; VkPhysicalDevice phys; CHECK(vkEnumeratePhysicalDevices(inst, &n, &phys));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    VkDeviceSize limit = props.limits.maxStorageBufferRange;
    printf("device %s maxStorageBufferRange %llu\n", props.deviceName, (unsigned long long)limit);
    if (limit > (256u << 20)) { puts("SKIP: fixture targets devices with <=256 MiB SSBO ranges"); vkDestroyInstance(inst, NULL); return 77; }
    VkDeviceSize offset = (2560ull << 20) + props.limits.minStorageBufferOffsetAlignment, size = 3120ull << 20;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq, sizeof *q); vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, q);
    uint32_t family; for (family = 0; family < nq && !(q[family].queueFlags & VK_QUEUE_COMPUTE_BIT); family++) {} free(q);
    if (family == nq) return 2;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.robustBufferAccess = VK_TRUE, .shaderInt64 = physical};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = physical};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev)); VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | (physical ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateFlagsInfo af = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = physical ? &af : NULL, .allocationSize = req.size, .memoryTypeIndex = memory_type(phys, req.memoryTypeBits)};
    VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &ai, NULL, &mem)); CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t *mapped; CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    mapped[offset / 4] = 0x12345678; mapped[offset / 4 + 1] = 0;
    if (physical) {
        VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
        uint64_t info[] = {vkGetBufferDeviceAddress(dev, &address), size};
        memcpy(mapped, info, sizeof info);
    }
    VkDescriptorType dtype = physical ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorCount = 1, .descriptorType = dtype, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &sl));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl};
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    char src[2048]; snprintf(src, sizeof src, "#version 450\nlayout(local_size_x=1) in; layout(std430,set=0,binding=0) buffer B { uint w[]; }; void main(){ w[%llu]=w[%llu]; }", (unsigned long long)(oversized ? offset / 4 + 1 : 1), (unsigned long long)(oversized ? offset / 4 : 0));
    if (physical) snprintf(src, sizeof src,
        "#version 460\n#extension GL_EXT_buffer_reference : require\n#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
        "layout(local_size_x=1) in; layout(buffer_reference,std430,buffer_reference_align=4) buffer GMem {uint w[];};"
        "layout(std140,set=0,binding=0) uniform Info {uint64_t addr; uint64_t size;};"
        "uint load_word(uint64_t at) {return addr!=0ul && size>=4ul && at<=size-4ul ? GMem(addr).w[uint(at>>2)] : 0u;}"
        "void store_word(uint64_t at,uint x) {if(addr!=0ul && size>=4ul && at<=size-4ul) GMem(addr).w[uint(at>>2)]=x;}"
        "void main(){store_word(%lluul,load_word(%lluul)); store_word(16ul,load_word(size));"
        "store_word(20ul,load_word(size-1ul)); store_word(size,42u);}",
        (unsigned long long)(offset+4), (unsigned long long)offset);
    VkShaderModule cs = shader(src);
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main"}};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
    VkDescriptorPoolSize ps = {dtype, 1};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &sl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo bi = {buf, oversized || physical ? 0 : offset, physical ? 16 : oversized ? size : 4096};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = dtype, .pBufferInfo = &bi};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo poolci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &poolci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL); vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb)); VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    uint32_t got = mapped[offset / 4 + 1]; printf("%s: %#x (expected 0x12345678)\n", physical ? "bounded physical pointer" : oversized ? "oversized descriptor (invalid API use)" : "bounded window", got);
    bool bounds_ok = !physical || (mapped[4] == 0 && mapped[5] == 0);
    if (physical) printf("physical out-of-bounds reads zero: %s\n", bounds_ok ? "PASS" : "FAIL");
    vkDestroyCommandPool(dev, pool, NULL); vkDestroyDescriptorPool(dev, dp, NULL); vkDestroyPipeline(dev, pipeline, NULL); vkDestroyShaderModule(dev, cs, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL); vkDestroyDescriptorSetLayout(dev, sl, NULL); vkUnmapMemory(dev, mem); vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL); return got == 0x12345678 && bounds_ok ? 0 : 1;
}
