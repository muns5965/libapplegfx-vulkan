/*
 * libapplegfx-vulkan — i1 / <N x i1> constants and air.any / air.all, executed on lavapipe
 * tests/air2spv-i1-const-lavapipe.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Regression guard for two translator bugs found in the real dump shader TsdrA2Xhfcu_Idst, whose state machine is selected by
 * `air.any.v4i1(<4 x i1> zeroinitializer)`:
 *   1. i1 / <N x i1> constants (CST_CODE_NULL / INTEGER / AGGREGATE of i1) were never bound (air_type_requires_extra_cap said "i1 needs a cap"), so the operand of
 *      air.any/air.all became OpUndef -> the branch it controls was driver dependent.
 *   2. air.any.* was emitted as OpAll (the name test looked at the wrong character).
 * Fixture tests/fixtures/i1const_fragment.air.bc (source: i1const_fragment.ll, assembled with llvm-as; hand-written IR, NOT Apple output) computes
 *     r = (any(<4 x i1> zeroinitializer) & true) ? 1 : 0                  -> 0     (scalar i1 constant, null vector operand)
 *     g = phi(all(<4 x i1> <1,1,1,1>) ? 1 : 0, via branches)             -> 1     (aggregate operand controlling a BRANCH)
 *     b = any(<4 x i1> <0,0,1,0>) ? 1 : 0                                 -> 1     (any != all)
 *     a = all(<2 x i1> <1,0>) ? 0 : 1                                     -> 1
 * so every pixel must be exactly (0, 255, 255, 255). The translated module must also contain no OpUndef of a bool type.
 * Exit 77 (skip) when there is no Vulkan device.
 */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

/* vertex shader: full-screen triangle from gl_VertexIndex, outputs vec4 at Locations 0 and 1 (glslangValidator -V of the GLSL in the commit message of this test) */
static const uint32_t VERT_SPV[] = {
0x07230203,0x00010000,0x0008000b,0x0000002e,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0009000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x0000001d,0x0000002b,
0x0000002d,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
0x00030005,0x00000009,0x00000070,0x00060005,0x0000000c,0x565f6c67,0x65747265,0x646e4978,
0x00007865,0x00060005,0x0000001b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,
0x0000001b,0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00070006,0x0000001b,0x00000001,
0x505f6c67,0x746e696f,0x657a6953,0x00000000,0x00070006,0x0000001b,0x00000002,0x435f6c67,
0x4470696c,0x61747369,0x0065636e,0x00070006,0x0000001b,0x00000003,0x435f6c67,0x446c6c75,
0x61747369,0x0065636e,0x00030005,0x0000001d,0x00000000,0x00030005,0x0000002b,0x0000306f,
0x00030005,0x0000002d,0x0000316f,0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00030047,
0x0000001b,0x00000002,0x00050048,0x0000001b,0x00000000,0x0000000b,0x00000000,0x00050048,
0x0000001b,0x00000001,0x0000000b,0x00000001,0x00050048,0x0000001b,0x00000002,0x0000000b,
0x00000003,0x00050048,0x0000001b,0x00000003,0x0000000b,0x00000004,0x00040047,0x0000002b,
0x0000001e,0x00000000,0x00040047,0x0000002d,0x0000001e,0x00000001,0x00020013,0x00000002,
0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,
0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,0x0000000a,0x0004003b,0x0000000b,
0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,0x00000001,0x0004002b,0x0000000a,
0x00000010,0x00000002,0x00040017,0x00000017,0x00000006,0x00000004,0x00040015,0x00000018,
0x00000020,0x00000000,0x0004002b,0x00000018,0x00000019,0x00000001,0x0004001c,0x0000001a,
0x00000006,0x00000019,0x0006001e,0x0000001b,0x00000017,0x00000006,0x0000001a,0x0000001a,
0x00040020,0x0000001c,0x00000003,0x0000001b,0x0004003b,0x0000001c,0x0000001d,0x00000003,
0x0004002b,0x0000000a,0x0000001e,0x00000000,0x0004002b,0x00000006,0x00000020,0x40000000,
0x0004002b,0x00000006,0x00000022,0x3f800000,0x0004002b,0x00000006,0x00000025,0x00000000,
0x00040020,0x00000029,0x00000003,0x00000017,0x0004003b,0x00000029,0x0000002b,0x00000003,
0x0007002c,0x00000017,0x0000002c,0x00000025,0x00000025,0x00000025,0x00000025,0x0004003b,
0x00000029,0x0000002d,0x00000003,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003d,0x0000000a,
0x0000000d,0x0000000c,0x000500c4,0x0000000a,0x0000000f,0x0000000d,0x0000000e,0x000500c7,
0x0000000a,0x00000011,0x0000000f,0x00000010,0x0004006f,0x00000006,0x00000012,0x00000011,
0x0004003d,0x0000000a,0x00000013,0x0000000c,0x000500c7,0x0000000a,0x00000014,0x00000013,
0x00000010,0x0004006f,0x00000006,0x00000015,0x00000014,0x00050050,0x00000007,0x00000016,
0x00000012,0x00000015,0x0003003e,0x00000009,0x00000016,0x0004003d,0x00000007,0x0000001f,
0x00000009,0x0005008e,0x00000007,0x00000021,0x0000001f,0x00000020,0x00050050,0x00000007,
0x00000023,0x00000022,0x00000022,0x00050083,0x00000007,0x00000024,0x00000021,0x00000023,
0x00050051,0x00000006,0x00000026,0x00000024,0x00000000,0x00050051,0x00000006,0x00000027,
0x00000024,0x00000001,0x00070050,0x00000017,0x00000028,0x00000026,0x00000027,0x00000025,
0x00000022,0x00050041,0x00000029,0x0000002a,0x0000001d,0x0000001e,0x0003003e,0x0000002a,
0x00000028,0x0003003e,0x0000002b,0x0000002c,0x0003003e,0x0000002d,0x0000002c,0x000100fd,
0x00010038
};

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

/* number of OpUndef whose result type is OpTypeBool / vector of bool */
static int count_bool_undef(const uint32_t *w, size_t nw) {
    int n = 0; size_t i = 5;
    while (i < nw) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc) break;
        if (op == 1 /* OpUndef */) {
            uint32_t ty = w[i + 1]; size_t j = 5;
            while (j < nw) {
                uint32_t wc2 = w[j] >> 16, op2 = w[j] & 0xffffu; if (!wc2) break;
                if (op2 == 20 /* OpTypeBool */ && w[j + 1] == ty) { n++; break; }
                if (op2 == 23 /* OpTypeVector */ && w[j + 1] == ty) { size_t k = 5; while (k < nw) { uint32_t wc3 = w[k] >> 16, op3 = w[k] & 0xffffu; if (!wc3) break; if (op3 == 20 && w[k + 1] == w[j + 2]) { n++; break; } k += wc3; } break; }
                j += wc2;
            }
        }
        i += wc;
    }
    return n;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}

int main(void) {
    size_t bn = 0; uint8_t *bc = slurp("i1const_fragment.air.bc", &bn);
    CHECK(bc, "fixture i1const_fragment.air.bc not found");
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "module_open");
    uint8_t *spv = NULL; size_t sz = 0;
    CHECK(lagfx_air2spv_translate_module(m, &spv, &sz) == LAGFX_OK && spv, "translate");
    CHECK(count_bool_undef((const uint32_t *)(const void *)spv, sz / 4u) == 0, "an i1 / <N x i1> operand was translated to OpUndef");

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "i1-const", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("SKIP: no Vulkan ICD\n"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) { printf("SKIP: no Vulkan device\n"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3) { printf("SKIP: device is not Vulkan 1.3 (dynamic rendering)\n"); return 77; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    VkDevice dev; VK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    enum { W = 16, H = 16 };
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; VK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; VK(vkAllocateMemory(dev, &mai, NULL, &imem)); VK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; VK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; VK(vkCreateBuffer(dev, &bci, NULL, &buf)); vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mai.memoryTypeIndex != 0xffffffffu, "no host visible memory");
    VkDeviceMemory bmem; VK(vkAllocateMemory(dev, &mai, NULL, &bmem)); VK(vkBindBufferMemory(dev, buf, bmem, 0));

    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule vs, fs; VK(vkCreateShaderModule(dev, &smv, NULL, &vs)); VK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
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
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
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
    uint8_t *px; VK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    int bad = 0;
    for (int i = 0; i < W * H; i++) if (px[i * 4] != 0 || px[i * 4 + 1] != 255 || px[i * 4 + 2] != 255 || px[i * 4 + 3] != 255) { if (!bad) printf("first bad pixel %d: %02x%02x%02x%02x\n", i, px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]); bad++; }
    CHECK(!bad, "%d of %d pixels are not (0,255,255,255)", bad, W * H);
    printf("PASS: i1 constants + air.any/air.all: all %d pixels == (0,255,255,255) on %s\n", W * H, pp.deviceName);
    return 0;
}
