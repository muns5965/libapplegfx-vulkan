/* air.sample_texture_2d option operands (census X04), executed on a Vulkan 1.3 device (lavapipe): an 8x8 RGBA8 texture with 4 mip levels whose texels encode
 * (x, y, level) is sampled by hand-written AIR fixtures tests/fixtures/tex_run_*.air.bc (llvm-as; HAND-WRITTEN IR, NOT Apple output) and every pixel of an 8x8
 * RGBA32F target is compared with the texel the Metal semantics select (nearest filter, nearest mip, clamp to edge):
 *   plain / bias 0                -> level 0 (the screen footprint is exactly one texel per pixel)
 *   explicit level 0 / 1 / 2      -> that level (constant and shader-computed variable)
 *   bias 1.0 (constant, variable) -> level 1 (implicit LOD 0 + bias 1)
 * Exit 77 (skip) when there is no Vulkan 1.3 device.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP(...) do { printf("SKIP: "); printf(__VA_ARGS__); printf("\n"); return 77; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

/* vertex shader (glslangValidator -V of the GLSL below): full-screen triangle from gl_VertexIndex; Location 0 = (fragment x + 0.5, fragment y + 0.5, 0, 1) for an
 * 8 x 8 target (the fixtures read it as the AIR position argument).
#version 450
layout(location = 0) out vec4 o0;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = p * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    o0 = vec4((ndc.x * 0.5 + 0.5) * 8.0, (ndc.y * 0.5 + 0.5) * 8.0, 0.0, 1.0);
}
 */
static const uint32_t VERT_SPV[] = {
0x07230203,0x00010000,0x0008000b,0x0000003d,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x00000024,0x0000002d,
0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,
0x00000009,0x00000070,0x00060005,0x0000000c,0x565f6c67,0x65747265,0x646e4978,0x00007865,
0x00030005,0x00000017,0x0063646e,0x00060005,0x00000022,0x505f6c67,0x65567265,0x78657472,
0x00000000,0x00060006,0x00000022,0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00070006,
0x00000022,0x00000001,0x505f6c67,0x746e696f,0x657a6953,0x00000000,0x00070006,0x00000022,
0x00000002,0x435f6c67,0x4470696c,0x61747369,0x0065636e,0x00070006,0x00000022,0x00000003,
0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,0x00000024,0x00000000,0x00030005,
0x0000002d,0x0000306f,0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00030047,0x00000022,
0x00000002,0x00050048,0x00000022,0x00000000,0x0000000b,0x00000000,0x00050048,0x00000022,
0x00000001,0x0000000b,0x00000001,0x00050048,0x00000022,0x00000002,0x0000000b,0x00000003,
0x00050048,0x00000022,0x00000003,0x0000000b,0x00000004,0x00040047,0x0000002d,0x0000001e,
0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,
0x00000007,0x00040015,0x0000000a,0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,
0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,
0x00000001,0x0004002b,0x0000000a,0x00000010,0x00000002,0x0004002b,0x00000006,0x00000019,
0x40000000,0x0004002b,0x00000006,0x0000001b,0x3f800000,0x00040017,0x0000001e,0x00000006,
0x00000004,0x00040015,0x0000001f,0x00000020,0x00000000,0x0004002b,0x0000001f,0x00000020,
0x00000001,0x0004001c,0x00000021,0x00000006,0x00000020,0x0006001e,0x00000022,0x0000001e,
0x00000006,0x00000021,0x00000021,0x00040020,0x00000023,0x00000003,0x00000022,0x0004003b,
0x00000023,0x00000024,0x00000003,0x0004002b,0x0000000a,0x00000025,0x00000000,0x0004002b,
0x00000006,0x00000027,0x00000000,0x00040020,0x0000002b,0x00000003,0x0000001e,0x0004003b,
0x0000002b,0x0000002d,0x00000003,0x0004002b,0x0000001f,0x0000002e,0x00000000,0x00040020,
0x0000002f,0x00000007,0x00000006,0x0004002b,0x00000006,0x00000032,0x3f000000,0x0004002b,
0x00000006,0x00000035,0x41000000,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003b,0x00000008,
0x00000017,0x00000007,0x0004003d,0x0000000a,0x0000000d,0x0000000c,0x000500c4,0x0000000a,
0x0000000f,0x0000000d,0x0000000e,0x000500c7,0x0000000a,0x00000011,0x0000000f,0x00000010,
0x0004006f,0x00000006,0x00000012,0x00000011,0x0004003d,0x0000000a,0x00000013,0x0000000c,
0x000500c7,0x0000000a,0x00000014,0x00000013,0x00000010,0x0004006f,0x00000006,0x00000015,
0x00000014,0x00050050,0x00000007,0x00000016,0x00000012,0x00000015,0x0003003e,0x00000009,
0x00000016,0x0004003d,0x00000007,0x00000018,0x00000009,0x0005008e,0x00000007,0x0000001a,
0x00000018,0x00000019,0x00050050,0x00000007,0x0000001c,0x0000001b,0x0000001b,0x00050083,
0x00000007,0x0000001d,0x0000001a,0x0000001c,0x0003003e,0x00000017,0x0000001d,0x0004003d,
0x00000007,0x00000026,0x00000017,0x00050051,0x00000006,0x00000028,0x00000026,0x00000000,
0x00050051,0x00000006,0x00000029,0x00000026,0x00000001,0x00070050,0x0000001e,0x0000002a,
0x00000028,0x00000029,0x00000027,0x0000001b,0x00050041,0x0000002b,0x0000002c,0x00000024,
0x00000025,0x0003003e,0x0000002c,0x0000002a,0x00050041,0x0000002f,0x00000030,0x00000017,
0x0000002e,0x0004003d,0x00000006,0x00000031,0x00000030,0x00050085,0x00000006,0x00000033,
0x00000031,0x00000032,0x00050081,0x00000006,0x00000034,0x00000033,0x00000032,0x00050085,
0x00000006,0x00000036,0x00000034,0x00000035,0x00050041,0x0000002f,0x00000037,0x00000017,
0x00000020,0x0004003d,0x00000006,0x00000038,0x00000037,0x00050085,0x00000006,0x00000039,
0x00000038,0x00000032,0x00050081,0x00000006,0x0000003a,0x00000039,0x00000032,0x00050085,
0x00000006,0x0000003b,0x0000003a,0x00000035,0x00070050,0x0000001e,0x0000003c,0x00000036,
0x0000003b,0x00000027,0x0000001b,0x0003003e,0x0000002d,0x0000003c,0x000100fd,0x00010038
};

#define TEXW 8
#define LEVELS 4

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}

/* texel (x, y) of level L: R = 20 + 25x, G = 20 + 25y, B = 40 + 60L, A = 255 */
static void texel(int L, int x, int y, float o[4]) { o[0] = (float)(20 + 25 * x) / 255.0f; o[1] = (float)(20 + 25 * y) / 255.0f; o[2] = (float)(40 + 60 * L) / 255.0f; o[3] = 1.0f; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void ref_l0(int x, int y, float o[4]) { texel(0, x, y, o); }
static void ref_l1(int x, int y, float o[4]) { texel(1, x >> 1, y >> 1, o); }
static void ref_l2(int x, int y, float o[4]) { texel(2, x >> 2, y >> 2, o); }

typedef struct { const char *fixture; void (*ref)(int x, int y, float o[4]); } case_t;
static const case_t cases[] = {
    { "tex_run_plain.air.bc",  ref_l0 },
    { "tex_run_level0.air.bc", ref_l0 },
    { "tex_run_level1.air.bc", ref_l1 },
    { "tex_run_level2.air.bc", ref_l2 },
    { "tex_run_bias1.air.bc",  ref_l1 },
    { "tex_run_biasvar.air.bc", ref_l1 },
};

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q;
static VkImage g_timg; static VkImageView g_tview; static VkSampler g_samp;

static int make_texture(void) {
    VkDevice dev = g_dev; VkPhysicalDevice pd = g_pd;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { TEXW, TEXW, 1 }, .mipLevels = LEVELS, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ici, NULL, &g_timg));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, g_timg, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; VK(vkAllocateMemory(dev, &mai, NULL, &imem)); VK(vkBindImageMemory(dev, g_timg, imem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_timg, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 } };
    VK(vkCreateImageView(dev, &vci, NULL, &g_tview));
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .minLod = 0.0f, .maxLod = (float)(LEVELS - 1) };
    VK(vkCreateSampler(dev, &sci, NULL, &g_samp));
    /* staging buffer: levels packed back to back */
    uint8_t data[(TEXW * TEXW + 16 + 4 + 1) * 4]; size_t off = 0, offs[LEVELS];
    for (int L = 0; L < LEVELS; L++) {
        int s = TEXW >> L; offs[L] = off;
        for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) { uint8_t *p = data + off + ((size_t)y * s + x) * 4; p[0] = (uint8_t)(20 + 25 * x); p[1] = (uint8_t)(20 + 25 * y); p[2] = (uint8_t)(40 + 60 * L); p[3] = 255; }
        off += (size_t)s * s * 4;
    }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = off, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer sb; VK(vkCreateBuffer(dev, &bci, NULL, &sb)); vkGetBufferMemoryRequirements(dev, sb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mai.memoryTypeIndex != 0xffffffffu, "no host visible memory");
    VkDeviceMemory smem; VK(vkAllocateMemory(dev, &mai, NULL, &smem)); VK(vkBindBufferMemory(dev, sb, smem, 0));
    void *p; VK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &p)); memcpy(p, data, off); vkUnmapMemory(dev, smem);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool; VK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_timg, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    for (int L = 0; L < LEVELS; L++) {
        VkBufferImageCopy bic = { .bufferOffset = offs[L], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)L, 0, 1 }, .imageExtent = { (uint32_t)(TEXW >> L), (uint32_t)(TEXW >> L), 1 } };
        vkCmdCopyBufferToImage(cmd, sb, g_timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    }
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    return 0;
}

static int run_case(const case_t *cs) {
    size_t bn = 0; uint8_t *bc = slurp(cs->fixture, &bn);
    CHECK(bc, "fixture %s not found", cs->fixture);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", cs->fixture);
    uint8_t *spv = NULL; size_t sz = 0;
    CHECK(lagfx_air2spv_translate_module(m, &spv, &sz) == LAGFX_OK && spv, "%s: translate", cs->fixture);
    VkDevice dev = g_dev; VkPhysicalDevice pd = g_pd; VkQueue q = g_q;
    const int W = 8, H = 8;
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = { (uint32_t)W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; VK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; VK(vkAllocateMemory(dev, &mai, NULL, &imem)); VK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; VK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; VK(vkCreateBuffer(dev, &bci, NULL, &buf)); vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mai.memoryTypeIndex != 0xffffffffu, "no host visible memory");
    VkDeviceMemory bmem; VK(vkAllocateMemory(dev, &mai, NULL, &bmem)); VK(vkBindBufferMemory(dev, buf, bmem, 0));
    /* descriptors: set 0 = binding 0 sampled image (texture arg), bindings 1 and 2 samplers (the sampler arg and the translator's default sampler) */
    VkDescriptorSetLayoutBinding lb[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = lb };
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    VkDescriptorPool dpool; VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; VK(vkAllocateDescriptorSets(dev, &dai, &dset));
    VkDescriptorImageInfo dii[3] = { { .imageView = g_tview, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { .sampler = g_samp }, { .sampler = g_samp } };
    VkWriteDescriptorSet wr[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &dii[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &dii[2] } };
    vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule vs, fs; VK(vkCreateShaderModule(dev, &smv, NULL, &vs)); VK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe; VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool; VK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0.5f, 0.5f, 0.5f, 0.5f } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
    float *px; VK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float want[4]; cs->ref(x, y, want);
            const float *got = px + ((size_t)y * W + x) * 4;
            int ok = 1;
            for (int c = 0; c < 4; c++) if (fabsf(got[c] - want[c]) > 1.0f / 1024.0f) ok = 0;
            if (!ok) {
                if (!bad) printf("%s: first bad pixel (%d,%d): got (%g %g %g %g) want (%g %g %g %g)\n", cs->fixture, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                bad++;
            }
        }
    CHECK(!bad, "%s: %d of %d pixels differ from the reference", cs->fixture, bad, W * H);
    printf("PASS: %s: all %d pixels match the reference\n", cs->fixture, W * H);
    vkUnmapMemory(dev, bmem);
    vkDestroyPipeline(dev, pipe, NULL); vkDestroyPipelineLayout(dev, pl, NULL); vkDestroyDescriptorPool(dev, dpool, NULL); vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL); vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, bmem, NULL); vkDestroyImageView(dev, view, NULL); vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, imem, NULL);
    free(spv); lagfx_air_module_free(m); free(bc);
    return 0;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "texture-sample-options", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) SKIP("no Vulkan ICD");
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) SKIP("no Vulkan device");
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3) SKIP("device is not Vulkan 1.3 (dynamic rendering)");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    VkDevice dev; VK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    g_pd = pd; g_dev = dev; g_q = q;
    if (make_texture()) return 1;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++)
        if (run_case(&cases[k])) return 1;
    printf("PASS air2spv-texture-sample-lavapipe (%zu fixtures) on %s\n", sizeof cases / sizeof cases[0], pp.deviceName);
    return 0;
}
