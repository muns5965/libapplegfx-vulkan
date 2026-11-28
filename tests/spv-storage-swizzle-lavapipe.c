/* Red/blue swap of storage-image bindings (spv_storage_swizzle), EXECUTED on a Vulkan device (lavapipe), SPDX-License-Identifier: MIT
 *
 * The S8 logs show one refused storage-image format: B8G8R8A8_UNORM (no STORAGE_IMAGE feature on Adreno 730), while R8G8B8A8_UNORM is storable and a BGRA8 image can be viewed as RGBA8. This test
 * checks the SHADER half of that substitution: a kernel reads the logical texel of image 1 and writes (read.r, 0, 0, 1) to image 0, both R8G8B8A8 images whose BYTES are meant to be BGRA-ordered.
 *   - unswizzled: the bytes come out as RGBA-ordered (the kernel treats byte 0 as red) = what a BGRA image would NOT hold
 *   - swizzled (pass applied to both bindings): source bytes (B,G,R,A) = (10,20,30,255) read as logical red 30 and the output bytes are (0,0,30,255): byte 2 is red, as in a native BGRA8 image
 * Also: a storage image used any other way (passed to a function / texel pointer) is refused by the pass; a module without the pair is returned untouched (rc 0).
 * NOT tested: the image-creation / view half (mutable format + extended usage) - that needs the device. Exit 77 without a Vulkan device. */
#include "air2spv/spv_storage_swizzle.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

static const uint32_t k_spv[] = {
0x07230203, 0x00010300, 0x00070000, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
0x00000031, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005, 0x00000001, 0x6e69616d,
0x00000000, 0x00000002, 0x00060010, 0x00000001, 0x00000011, 0x00000008, 0x00000008, 0x00000001,
0x00040047, 0x00000002, 0x0000000b, 0x0000001c, 0x00040047, 0x00000003, 0x00000022, 0x00000000,
0x00040047, 0x00000003, 0x00000021, 0x00000000, 0x00040047, 0x00000004, 0x00000022, 0x00000000,
0x00040047, 0x00000004, 0x00000021, 0x00000001, 0x00020013, 0x00000005, 0x00030021, 0x00000006,
0x00000005, 0x00030016, 0x00000007, 0x00000020, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
0x00040015, 0x00000009, 0x00000020, 0x00000001, 0x00040017, 0x0000000a, 0x00000007, 0x00000004,
0x00040017, 0x0000000b, 0x00000008, 0x00000003, 0x00040017, 0x0000000c, 0x00000009, 0x00000002,
0x00090019, 0x0000000d, 0x00000007, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002,
0x00000004, 0x00040020, 0x0000000e, 0x00000000, 0x0000000d, 0x0004003b, 0x0000000e, 0x00000003,
0x00000000, 0x0004003b, 0x0000000e, 0x00000004, 0x00000000, 0x00040020, 0x0000000f, 0x00000001,
0x0000000b, 0x0004003b, 0x0000000f, 0x00000002, 0x00000001, 0x0004002b, 0x00000007, 0x00000010,
0x00000000, 0x0004002b, 0x00000007, 0x00000011, 0x3f800000, 0x00050036, 0x00000005, 0x00000001,
0x00000000, 0x00000006, 0x000200f8, 0x00000012, 0x0004003d, 0x0000000b, 0x00000013, 0x00000002,
0x00050051, 0x00000008, 0x00000014, 0x00000013, 0x00000000, 0x00050051, 0x00000008, 0x00000015,
0x00000013, 0x00000001, 0x0004007c, 0x00000009, 0x00000016, 0x00000014, 0x0004007c, 0x00000009,
0x00000017, 0x00000015, 0x00050050, 0x0000000c, 0x00000018, 0x00000016, 0x00000017, 0x0004003d,
0x0000000d, 0x00000019, 0x00000004, 0x00050062, 0x0000000a, 0x0000001a, 0x00000019, 0x00000018,
0x00050051, 0x00000007, 0x0000001b, 0x0000001a, 0x00000000, 0x00070050, 0x0000000a, 0x0000001c,
0x0000001b, 0x00000010, 0x00000010, 0x00000011, 0x0004003d, 0x0000000d, 0x0000001d, 0x00000003,
0x00040063, 0x0000001d, 0x00000018, 0x0000001c, 0x000100fd, 0x00010038
};

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
#define W 8u
typedef struct { VkImage im; VkDeviceMemory mem; VkImageView v; uint8_t *p; uint32_t pitch; } img_t;

static uint32_t mtype(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return i;
    return ~0u;
}
static int mkimg(img_t *o) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_STORAGE_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED };
    VK(vkCreateImage(g_dev, &ci, NULL, &o->im));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, o->im, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mtype(rq.memoryTypeBits) };
    CHECK(ai.memoryTypeIndex != ~0u, "no host-visible memory for a linear image");
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->mem)); VK(vkBindImageMemory(g_dev, o->im, o->mem, 0));
    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(g_dev, o->im, &sr, &sl); o->pitch = (uint32_t)sl.rowPitch;
    void *p; VK(vkMapMemory(g_dev, o->mem, sl.offset, VK_WHOLE_SIZE, 0, &p)); o->p = p; memset(p, 0, (size_t)(o->pitch * W));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(g_dev, &vi, NULL, &o->v));
    return 0;
}

static int run(const uint8_t *spv, size_t n, img_t *dst, img_t *src) {
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, dst->v, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, src->v, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[0] },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1] } };
    vkUpdateDescriptorSets(g_dev, 2, wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    VkImageMemoryBarrier ib[2]; img_t *im[2] = { dst, src };
    for (int k = 0; k < 2; k++) ib[k] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[k]->im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, ib);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(g_q, 1, &sub, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    vkDestroyCommandPool(g_dev, cp, NULL); vkDestroyDescriptorPool(g_dev, dp, NULL); vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL); vkDestroyDescriptorSetLayout(g_dev, dl, NULL); vkDestroyShaderModule(g_dev, sm, NULL);
    return 0;
}

static int fill_and_run(const uint8_t *spv, size_t n, uint8_t out[4]) {
    img_t d, s; if (mkimg(&d) || mkimg(&s)) return 1;
    for (uint32_t y = 0; y < W; y++) for (uint32_t x = 0; x < W; x++) { uint8_t *t = s.p + y * s.pitch + x * 4u; t[0] = 10; t[1] = 20; t[2] = 30; t[3] = 255; }   /* BYTES of a BGRA texel: B=10 G=20 R=30 A=255 */
    if (run(spv, n, &d, &s)) return 1;
    for (uint32_t y = 0; y < W; y++) for (uint32_t x = 0; x < W; x++) { const uint8_t *t = d.p + y * d.pitch + x * 4u; if (memcmp(t, d.p, 4)) { printf("FAIL texel (%u,%u) differs from texel (0,0)\n", x, y); return 1; } }
    memcpy(out, d.p, 4); return 0;
}


/* ---- the image half: B8G8R8A8 images created MUTABLE_FORMAT | EXTENDED_USAGE with STORAGE, R8G8B8A8 STORAGE views (the arm64_resources.c surrogate), swizzled kernel, bytes read back by a copy ---- */
typedef struct { VkImage im; VkDeviceMemory mem; VkImageView v; } simg_t;
static uint32_t dtype(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return 0;
}
static int mksimg(simg_t *o) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(g_dev, &ci, NULL, &o->im));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, o->im, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = dtype(rq.memoryTypeBits) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->mem)); VK(vkBindImageMemory(g_dev, o->im, o->mem, 0));
    VkImageViewUsageCreateInfo vu = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_STORAGE_BIT };
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &vu, .image = o->im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(g_dev, &vi, NULL, &o->v));
    return 0;
}
static int surrogate_run(const uint8_t *spv, size_t n, uint8_t out[4]) {
    simg_t d, s; if (mksimg(&d) || mksimg(&s)) return 1;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * W * 4u, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer bu, bd; VK(vkCreateBuffer(g_dev, &bci, NULL, &bu)); VK(vkCreateBuffer(g_dev, &bci, NULL, &bd));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, bu, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size * 2u, .memoryTypeIndex = mtype(rq.memoryTypeBits) };
    VkDeviceMemory bm; VK(vkAllocateMemory(g_dev, &ai, NULL, &bm)); VK(vkBindBufferMemory(g_dev, bu, bm, 0)); VK(vkBindBufferMemory(g_dev, bd, bm, rq.size));
    uint8_t *p; VK(vkMapMemory(g_dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    memset(p, 0, (size_t)rq.size * 2u); for (uint32_t i = 0; i < W * W; i++) { p[i * 4] = 10; p[i * 4 + 1] = 20; p[i * 4 + 2] = 30; p[i * 4 + 3] = 255; }   /* BGRA bytes: B=10 G=20 R=30 A=255 */
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, d.v, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, s.v, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[0] },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1] } };
    vkUpdateDescriptorSets(g_dev, 2, wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    simg_t *im[2] = { &d, &s }; VkImageMemoryBarrier ib[2];
    for (int k = 0; k < 2; k++) ib[k] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[k]->im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, ib);
    VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, W, 1 } };
    vkCmdCopyBufferToImage(cb, bu, s.im, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);   /* the guest's BGRA bytes go in through the BGRA image, as the mirror upload does */
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb2, 0, NULL, 0, NULL);
    vkCmdCopyImageToBuffer(cb, d.im, VK_IMAGE_LAYOUT_GENERAL, bd, 1, &rg);
    VkMemoryBarrier mb3 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb3, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(g_q, 1, &sub, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    const uint8_t *q = p + rq.size;
    for (uint32_t i = 0; i < W * W; i++) if (memcmp(q + i * 4, q, 4)) { printf("FAIL texel %u differs\n", i); return 1; }
    memcpy(out, q, 4);
    return 0;
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "spv-storage-swizzle", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("SKIP: no Vulkan instance"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL); if (!nd) { puts("SKIP: no Vulkan device"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds); g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);
    float prio = 1.0f; uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    g_qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qf = i; break; }
    free(qp); CHECK(g_qf != ~0u, "no compute queue");
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures en = { 0 }; VkPhysicalDeviceFeatures sup; vkGetPhysicalDeviceFeatures(g_pd, &sup); en.shaderStorageImageExtendedFormats = sup.shaderStorageImageExtendedFormats;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &en };
    VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev)); vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    const uint8_t *spv = (const uint8_t *)(const void *)k_spv; const size_t sn = sizeof k_spv;

    uint8_t plain[4]; if (fill_and_run(spv, sn, plain)) return 1;
    CHECK(plain[0] == 10 && plain[1] == 0 && plain[2] == 0 && plain[3] == 255, "unswizzled kernel: bytes %u %u %u %u (expected 10 0 0 255: byte 0 treated as red)", plain[0], plain[1], plain[2], plain[3]);
    printf("PASS: unswizzled: the kernel treats byte 0 as red (bytes 10 0 0 255) - not what a BGRA8 image would hold\n");

    const uint32_t both[2][2] = { { 0, 0 }, { 0, 1 } }, only_dst[1][2] = { { 0, 0 } }, none[1][2] = { { 3, 7 } };
    uint8_t *sw = NULL; size_t swn = 0; char why[160];
    CHECK(lagfx_spv_swizzle_storage_rb(spv, sn, both, 2, &sw, &swn, why, sizeof why) == 1 && sw, "swizzle both bindings: %s", why);
    uint8_t got[4]; if (fill_and_run(sw, swn, got)) return 1;
    CHECK(got[0] == 0 && got[1] == 0 && got[2] == 30 && got[3] == 255, "swizzled kernel: bytes %u %u %u %u (expected 0 0 30 255: red in byte 2)", got[0], got[1], got[2], got[3]);
    printf("PASS: swizzled (read + write): source bytes (B,G,R,A)=(10,20,30,255) read as red 30; output bytes (0,0,30,255), red in byte 2 as in a BGRA8 image\n");
    free(sw);

    sw = NULL; CHECK(lagfx_spv_swizzle_storage_rb(spv, sn, only_dst, 1, &sw, &swn, why, sizeof why) == 1 && sw, "swizzle destination only");
    if (fill_and_run(sw, swn, got)) return 1;
    CHECK(got[0] == 0 && got[1] == 0 && got[2] == 10 && got[3] == 255, "destination-only: bytes %u %u %u %u (expected 0 0 10 255)", got[0], got[1], got[2], got[3]);
    printf("PASS: only the listed binding is swapped (destination only: bytes 0 0 10 255)\n");
    free(sw);

    sw = NULL; CHECK(lagfx_spv_swizzle_storage_rb(spv, sn, none, 1, &sw, &swn, why, sizeof why) == 0 && !sw, "a pair the module does not use must return 0");
    printf("PASS: a binding the module does not use leaves it untouched (rc 0)\n");
    sw = NULL; CHECK(lagfx_spv_swizzle_storage_rb(spv, sn, both, 2, &sw, &swn, why, sizeof why) == 1 && sw, "swizzle for the image-half test");
    {   uint8_t sg[4]; if (surrogate_run(sw, swn, sg)) return 1;
        CHECK(sg[0] == 0 && sg[1] == 0 && sg[2] == 30 && sg[3] == 255, "BGRA8 image through an RGBA8 storage view: bytes %u %u %u %u (expected 0 0 30 255)", sg[0], sg[1], sg[2], sg[3]);
        printf("PASS: B8G8R8A8 images (mutable format + extended usage, STORAGE) with R8G8B8A8 storage views: uploaded BGRA bytes (10,20,30,255) read as red 30, result bytes (0,0,30,255) read back through the BGRA image\n"); }
    free(sw);
    vkDestroyDevice(g_dev, NULL);
    return 0;
}
