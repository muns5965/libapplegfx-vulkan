/* S06 robustness: air.discard_fragment inside nested loops (several discards), around an OpPhi merge, and followed by side effects (device-buffer store, image store, atomic),
 * executed on a Vulkan 1.3 device (lavapipe) and compared with a CPU reference. The translator output is run twice: OpKill in a split block (default) and
 * OpDemoteToHelperInvocation (when the device enables shaderDemoteToHelperInvocation).
 *
 * Side effects after a discard (what is verified here and what is only quoted):
 *   [V: SPV_EXT_demote_to_helper_invocation, OpDemoteToHelperInvocation] "Any stores to memory after this instruction are suppressed and the fragment does not write outputs to the
 *     framebuffer." "Unlike the OpKill instruction, this does not necessarily terminate the invocation."  (The extension text does not mention atomics.)
 *   [V: Vulkan spec, chapter Shaders, "Helper Invocations"] "Stores and atomics performed by helper invocations must not have any effect on memory" (the text excepts the Function,
 *     Private and Output storage classes), so a demoted invocation's later atomics are no-ops too.
 *   [V: Vulkan spec, same chapter] an OpKill may be implemented as OpTerminateInvocation OR as OpDemoteToHelperInvocation; OpTerminateInvocation terminates the invocation: nothing after it executes.
 *   => for the memory contents the three flavours agree (nothing after the discard reaches memory); they differ in derivatives of the neighbouring lanes (test_discard_derivatives in
 *   arm64-render-compat-unit.c) and in whether code after the discard still runs (invisible when its stores are suppressed).
 * No translator fixture stores to a device buffer: llvm-as (LLVM 18) writes only opaque pointers and frag_resource_kind classifies a buffer argument by its typed pointee (Apple's AIR is typed), so
 * a hand-written `ptr addrspace(1)` buffer argument is taken for a texture; the buffer stores run from the GLSL below.
 * The translator has NO atomic instruction (no air.atomic.* / atomicrmw), so the atomic case runs hand-written GLSL compiled to the three discard flavours
 * (tests/fixtures/discard_atomic.frag, gen-discard-atomic-spv.sh): that checks the DEVICE, not the translator.
 * Fixtures: tests/fixtures/gen-discard-robust-fixtures.sh (HAND-WRITTEN IR, spelling air.discard_fragment is [I]). Exit 77 (skip) without a Vulkan 1.3 device.
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

#include "air2spv-run-vert.h"   /* VERT_SPV: full-screen triangle, Location 0 = fragment position of an 8 x 8 target */
#include "air2spv-discard-atomic-spv.h"

enum { RES_NONE = 0, RES_SSBO, RES_IMAGE };
typedef struct { float col[8 * 8 * 4]; uint32_t buf[64 + 1]; float img[8 * 8 * 4]; } result_t;
static const float CLEAR[4] = { 0.0f, 0.0f, 0.0f, 0.25f };

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static int g_have_demote;

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
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}
static int make_image(VkImageUsageFlags usage, VkImage *img, VkImageView *view) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = { 8, 8, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(g_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory m; VK(vkAllocateMemory(g_dev, &mai, NULL, &m)); VK(vkBindImageMemory(g_dev, *img, m, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(g_dev, &vci, NULL, view));
    return 0;
}
static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, void **map) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VK(vkCreateBuffer(g_dev, &bci, NULL, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, *b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(mai.memoryTypeIndex != 0xffffffffu, "no host visible memory");
    VkDeviceMemory m; VK(vkAllocateMemory(g_dev, &mai, NULL, &m)); VK(vkBindBufferMemory(g_dev, *b, m, 0));
    VK(vkMapMemory(g_dev, m, 0, VK_WHOLE_SIZE, 0, map));
    return 0;
}

/* Draws one full-screen triangle with the fragment shader `fspv` into a cleared 8 x 8 RGBA32F target, with resource `res` at set 0 binding 0. */
static int run(const uint32_t *fspv, size_t fbytes, int res, result_t *out) {
    VkDevice dev = g_dev; memset(out, 0, sizeof *out);
    VkImage img, rimg = VK_NULL_HANDLE; VkImageView view, rview = VK_NULL_HANDLE;
    if (make_image(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &img, &view)) return 1;
    if (res == RES_IMAGE && make_image(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &rimg, &rview)) return 1;
    VkBuffer rb, cb_, ib; float *colmap, *imgmap; uint32_t *bufmap;
    if (make_buffer(sizeof out->col, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &cb_, (void **)&colmap)) return 1;
    if (make_buffer(sizeof out->img, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &ib, (void **)&imgmap)) return 1;
    if (make_buffer(sizeof out->buf, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &rb, (void **)&bufmap)) return 1;
    memset(bufmap, 0, sizeof out->buf);
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkDescriptorPool dpool = VK_NULL_HANDLE; VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if (res != RES_NONE) {
        VkDescriptorType dt = res == RES_SSBO ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkDescriptorSetLayoutBinding lb = { .binding = 0, .descriptorType = dt, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
        VK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
        VkDescriptorPoolSize ps = { dt, 1 };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
        VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        VK(vkAllocateDescriptorSets(dev, &dai, &dset));
        VkDescriptorBufferInfo dbi = { .buffer = rb, .range = VK_WHOLE_SIZE };
        VkDescriptorImageInfo dii = { .imageView = rview, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = dt, .pBufferInfo = &dbi, .pImageInfo = &dii };
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
        plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    }
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fbytes, .pCode = fspv };
    VkShaderModule vs, fs; VK(vkCreateShaderModule(dev, &smv, NULL, &vs)); VK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 8, 8, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 8, 8 } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl };
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
    if (res == RES_IMAGE) {   /* the storage image starts as zeros, in GENERAL */
        VkImageMemoryBarrier t = b1; t.image = rimg; t.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; t.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &t);
        VkClearColorValue z = { .float32 = { 0, 0, 0, 0 } }; VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
        t.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; t.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT; t.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; t.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &t);
    }
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { CLEAR[0], CLEAR[1], CLEAR[2], CLEAR[3] } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 8, 8 } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (res != RES_NONE) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 8, 8, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cb_, 1, &bic);
    if (res == RES_IMAGE) {
        VkImageMemoryBarrier t = b1; t.image = rimg; t.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; t.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; t.oldLayout = VK_IMAGE_LAYOUT_GENERAL; t.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &t);
        vkCmdCopyImageToBuffer(cmd, rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ib, 1, &bic);
    }
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    memcpy(out->col, colmap, sizeof out->col); memcpy(out->buf, bufmap, sizeof out->buf);
    if (res == RES_IMAGE) memcpy(out->img, imgmap, sizeof out->img);
    vkDestroyPipeline(dev, pipe, NULL); vkDestroyPipelineLayout(dev, pl, NULL);
    if (dpool) vkDestroyDescriptorPool(dev, dpool, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL); vkDestroyCommandPool(dev, pool, NULL);
    return 0;
}

/* ---- CPU references: pixel (x, y), position (x + 0.5, y + 0.5) ---- */
static void put(float *p, float r, float g, float b, float a) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
static void ref_clear(float *p) { memcpy(p, CLEAR, sizeof CLEAR); }
typedef struct { const char *fixture; int res; void (*color)(int x, int y, float *p); void (*mem)(int x, int y, uint32_t *buf, float *img); } case_t;

static void col_nested(int x, int y, float *p) {   /* discards: outer i == 2 && px < 2 (x < 2), inner i == 1 && j == 1 && px > 6 (x > 5), after the loops py < 1 (y == 0); survivors: 3 x 3 iterations -> 9 / 16 */
    if (x < 2 || x > 5 || y == 0) ref_clear(p); else put(p, 9.0f / 16.0f, 0, 0, 1);
}
static void col_diamond(int x, int y, float *p) { if (x < 4 && y < 4) ref_clear(p); else put(p, x < 4 ? 0.25f : 0.5f, 0, 0, 1); }
static void col_x4(int x, int y, float *p) { (void)y; if (x < 4) ref_clear(p); else put(p, 1, 0, 0, 1); }
static void mem_img(int x, int y, uint32_t *buf, float *img) { (void)buf; if (x >= 4) put(img + (y * 8 + x) * 4, 1, 0.5f, 0.25f, 1); }
static const case_t cases[] = {
    { "spec_discard_r_nested.air.bc",   RES_NONE,  col_nested,  NULL },
    { "spec_discard_r_diamond.air.bc",  RES_NONE,  col_diamond, NULL },
    { "spec_discard_r_imgstore.air.bc", RES_IMAGE, col_x4,      mem_img },
};

static int compare(const char *label, const case_t *cs, const result_t *r) {
    int bad = 0;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        float want[4]; cs->color(x, y, want);
        const float *got = r->col + (y * 8 + x) * 4;
        for (int c = 0; c < 4; c++) if (fabsf(got[c] - want[c]) > 1.0f / 1024.0f) { if (!bad) printf("%s: first bad colour (%d,%d): got (%g %g %g %g) want (%g %g %g %g)\n", label, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]); bad++; break; }
    }
    uint32_t wbuf[65]; float wimg[8 * 8 * 4]; memset(wbuf, 0, sizeof wbuf); memset(wimg, 0, sizeof wimg);
    if (cs->mem) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) cs->mem(x, y, wbuf, wimg);
    for (int i = 0; i < 64; i++) if (r->buf[i] != wbuf[i]) { if (!bad) printf("%s: first bad buffer word %d: got %08x want %08x (a store after the discard reached memory, or a survivor's store is missing)\n", label, i, r->buf[i], wbuf[i]); bad++; break; }
    for (int i = 0; i < 8 * 8 * 4; i++) if (fabsf(r->img[i] - wimg[i]) > 1.0f / 1024.0f) { if (!bad) printf("%s: first bad image float %d: got %g want %g\n", label, i, r->img[i], wimg[i]); bad++; break; }
    return bad;
}

static int run_fixture(const case_t *cs, int demote) {
    size_t bn = 0; uint8_t *bc = slurp(cs->fixture, &bn);
    CHECK(bc, "fixture %s not found", cs->fixture);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", cs->fixture);
    uint8_t *spv = NULL; size_t sz = 0;
    lagfx_air2spv_set_discard_demote(demote != 0);
    lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sz);
    lagfx_air2spv_set_discard_demote(false);
    CHECK(st == LAGFX_OK && spv, "%s: translate (%s)", cs->fixture, demote ? "demote" : "kill");
    if (getenv("DISCARD_ROBUST_DUMP")) { char p[600]; snprintf(p, sizeof p, "%s/%s.%s.spv", getenv("DISCARD_ROBUST_DUMP"), cs->fixture, demote ? "demote" : "kill"); FILE *f = fopen(p, "wb"); if (f) { fwrite(spv, 1, sz, f); fclose(f); } }
    static result_t r;
    char label[160]; snprintf(label, sizeof label, "%s [%s]", cs->fixture, demote ? "demote" : "kill");
    if (run((const uint32_t *)(const void *)spv, sz, cs->res, &r)) return 1;
    int bad = compare(label, cs, &r);
    CHECK(!bad, "%s: %d mismatching items against the CPU reference", label, bad);
    printf("PASS: %s matches the CPU reference (colour%s)\n", label, cs->res == RES_IMAGE ? " + storage image" : "");
    free(spv); lagfx_air_module_free(m); free(bc);
    return 0;
}

static int run_atomic(const char *name, const uint32_t *spv, size_t bytes) {
    static result_t r;
    if (run(spv, bytes, RES_SSBO, &r)) return 1;
    int bad = 0;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        float want[4]; col_x4(x, y, want);
        for (int c = 0; c < 4; c++) if (fabsf(r.col[(y * 8 + x) * 4 + c] - want[c]) > 1.0f / 1024.0f) bad++;
        if (r.buf[1 + y * 8 + x] != (x >= 4 ? 7u : 0u)) bad++;
    }
    CHECK(r.buf[0] == 32u, "%s: atomicAdd counter = %u, want 32 (32 surviving invocations; a discarded lane's atomic must not reach memory)", name, r.buf[0]);
    CHECK(!bad, "%s: %d mismatching colour / buffer items", name, bad);
    printf("PASS: %s: atomicAdd + store after the discard: counter = 32, stores only from the 32 surviving pixels\n", name);
    return 0;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "discard-robust", .apiVersion = VK_API_VERSION_1_3 };
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
    VkPhysicalDeviceVulkan13Features have13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 have2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have13 };
    vkGetPhysicalDeviceFeatures2(pd, &have2);
    if (!have2.features.fragmentStoresAndAtomics || !have2.features.shaderStorageImageWriteWithoutFormat || !have13.shaderTerminateInvocation) SKIP("device lacks fragmentStoresAndAtomics / shaderStorageImageWriteWithoutFormat / shaderTerminateInvocation");
    g_have_demote = have13.shaderDemoteToHelperInvocation != 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderTerminateInvocation = VK_TRUE, .shaderDemoteToHelperInvocation = (VkBool32)g_have_demote };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    f2.features.fragmentStoresAndAtomics = VK_TRUE; f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    VkDevice dev; VK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    g_pd = pd; g_dev = dev; g_q = q;
    int n = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++)
        for (int mode = 0; mode < 2; mode++) {
            if (mode && !g_have_demote) { printf("note: device does not enable shaderDemoteToHelperInvocation: demote mode not run for %s\n", cases[k].fixture); continue; }
            if (run_fixture(&cases[k], mode)) return 1;
            n++;
        }
    if (run_atomic("OpKill", DISCARD_ATOMIC_KILL_SPV, sizeof DISCARD_ATOMIC_KILL_SPV)) return 1;
    if (run_atomic("OpTerminateInvocation", DISCARD_ATOMIC_TERM_SPV, sizeof DISCARD_ATOMIC_TERM_SPV)) return 1;
    if (g_have_demote && run_atomic("OpDemoteToHelperInvocation", DISCARD_ATOMIC_DEMOTE_SPV, sizeof DISCARD_ATOMIC_DEMOTE_SPV)) return 1;
    printf("PASS air2spv-discard-robust-lavapipe (%d translator runs + atomics) on %s\n", n, pp.deviceName);
    return 0;
}
