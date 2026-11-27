/* Sampler state and 3D textures, executed on a Vulkan 1.3 device (lavapipe) and compared with an INDEPENDENT CPU reference (a straight transcription of the Vulkan
 * spec's texel addressing and filtering, written without looking at the translator or the state mapping):
 *
 *   A  constexpr samplers (X06): hand-written AIR fragment functions (tests/fixtures/cxs_<word>.air.bc, made by gen-sampler-fixtures.sh, NOT Apple output) sample a
 *      4x4 RGBA32F texture at fragment position - 1.75 texels (1.5 texels below / above the edges) through every combination of address mode (clamp to zero,
 *      clamp to edge, repeat, mirrored repeat, clamp to border) x nearest / linear, with normalized and with PIXEL (unnormalized, bit 15) coordinates. The test goes
 *      the production way: translate -> reflect the constexpr sampler binding + word (lagfx_spv_reflect_constexpr_samplers) -> lagfx_sampler_state_from_constexpr_norm
 *      -> lagfx_sampler_cache_get. A pixel sampler must come out as a NORMALIZED VkSampler plus an OpImageQuerySizeLod division in the shader (checked).
 *   B  runtime sampler descriptors (O02): the serialized Metal sampler state (lagfx_a64_sampler_decode) -> VkSamplerCreateInfo, all address modes, both filters,
 *      mirror-clamp-to-edge, filters that differ, and the unnormalized-coordinates case (explicit-LOD variant of the shader, as the exec selects it).
 *   C  texture3d (X05): air.sample_texture_3d -> Dim3D image + vec3 coordinate, sampled from a real 3D VkImage / 3D view: nearest at texel centres (exact texels) and
 *      trilinear along a diagonal (z interpolation, which a 2D slice cannot produce).
 *
 *   D  texture2d_array (mip/layer views): air.sample_texture_2d_array -> arrayed Dim2D image + (u, v, layer) coordinate, sampled from a real 2D_ARRAY VkImage with 4 layers x
 *      4 mip levels (every texel encodes layer, level, x, y): explicit level = column, layer = row; the constexpr PIXEL sampler variant; negative controls: the module that
 *      does not mark its texture argument as an array (layer 0 of a plain 2D view, the index dropped), a one-layer array view (indices clamp to layer 0), a level-0-only view
 *      (LOD clamps to the view). The translated modules also go through spirv-val when it is installed. The AIR spelling is INFER: no fixture / capture holds an array sample.
 *
 * Meaning of the constexpr word fields is the external, device-UNVERIFIED layout of air2spv/constexpr_sampler.h (INFER); this test checks the pipeline is
 * consistent with it, not that Apple's hardware agrees. Exit 77 (skip) without a Vulkan 1.3 device.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/constexpr_sampler.h"
#include "air/bitcode_reader.h"
#include "protocol/arm64_sampler.h"
#include "vulkan/sampler_cache.h"

#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP(...) do { printf("SKIP: "); printf(__VA_ARGS__); printf("\n"); return 77; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

#include "air2spv-run-vert.h"   /* VERT_SPV: full-screen triangle, Location 0 = fragment position of an 8 x 8 target */

#define W 8
#define H 8

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q;
static bool g_have_mirror_clamp;
static lagfx_sampler_cache_t g_cache;

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

/* ---------------------------------------------------------------- reference sampler (Vulkan spec, texel addressing + filtering) */
enum { AM_ZERO = 0, AM_EDGE, AM_REPEAT, AM_MIRROR, AM_BORDER, AM_MIRROR_CLAMP };   /* constexpr word numbering 0..4; 5 = Metal mirror clamp to edge (runtime sampler only) */
typedef struct { int n[3]; const float *t; } tex_t;   /* n texels per axis, rgba float */
static int resolve(int i, int n, int am) {   /* -1 = border (transparent black) */
    switch (am) {
    case AM_EDGE: return i < 0 ? 0 : i >= n ? n - 1 : i;
    case AM_REPEAT: return ((i % n) + n) % n;
    case AM_MIRROR: { int m = ((i % (2 * n)) + 2 * n) % (2 * n); return m < n ? m : 2 * n - 1 - m; }
    case AM_MIRROR_CLAMP: { int m = i < 0 ? -1 - i : i; return m >= n ? n - 1 : m; }
    default: return (i < 0 || i >= n) ? -1 : i;   /* zero / border */
    }
}
static void fetch(const tex_t *T, const int am[3], int x, int y, int z, float o[4]) {
    int rx = resolve(x, T->n[0], am[0]), ry = resolve(y, T->n[1], am[1]), rz = resolve(z, T->n[2], am[2]);
    if (rx < 0 || ry < 0 || rz < 0) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    memcpy(o, T->t + (((size_t)rz * T->n[1] + ry) * T->n[0] + rx) * 4, 16);
}
/* t* = coordinates in TEXELS; dims = 2 or 3 (a 2D texture has n[2] = 1 and z fixed at 0) */
static void ref_sample(const tex_t *T, const int am[3], int linear, float tx, float ty, float tz, float o[4]) {
    if (!linear) { fetch(T, am, (int)floorf(tx), (int)floorf(ty), (int)floorf(tz), o); return; }
    float ux = tx - 0.5f, uy = ty - 0.5f, uz = T->n[2] > 1 ? tz - 0.5f : 0.0f;
    int x0 = (int)floorf(ux), y0 = (int)floorf(uy), z0 = (int)floorf(uz);
    float fx = ux - (float)x0, fy = uy - (float)y0, fz = uz - (float)z0;
    int nz = T->n[2] > 1 ? 2 : 1;
    for (int c = 0; c < 4; c++) o[c] = 0.0f;
    for (int dz = 0; dz < nz; dz++) for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
        float t[4]; fetch(T, am, x0 + dx, y0 + dy, z0 + dz, t);
        float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (nz == 2 ? (dz ? fz : 1 - fz) : 1.0f);
        for (int c = 0; c < 4; c++) o[c] += w * t[c];
    }
}

/* ---------------------------------------------------------------- Vulkan helpers */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } vtex_t;
static int make_tex(const tex_t *T, int is3d, vtex_t *out) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = { (uint32_t)T->n[0], (uint32_t)T->n[1], (uint32_t)T->n[2] }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(g_dev, &ici, NULL, &out->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, out->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(g_dev, &mai, NULL, &out->mem)); VK(vkBindImageMemory(g_dev, out->img, out->mem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->img, .viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(g_dev, &vci, NULL, &out->view));
    size_t bytes = (size_t)T->n[0] * T->n[1] * T->n[2] * 16;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer sb; VK(vkCreateBuffer(g_dev, &bci, NULL, &sb)); vkGetBufferMemoryRequirements(g_dev, sb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory smem; VK(vkAllocateMemory(g_dev, &mai, NULL, &smem)); VK(vkBindBufferMemory(g_dev, sb, smem, 0));
    void *p; VK(vkMapMemory(g_dev, smem, 0, VK_WHOLE_SIZE, 0, &p)); memcpy(p, T->t, bytes); vkUnmapMemory(g_dev, smem);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool; VK(vkCreateCommandPool(g_dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(g_dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = out->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)T->n[0], (uint32_t)T->n[1], (uint32_t)T->n[2] } };
    vkCmdCopyBufferToImage(cmd, sb, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    vkDestroyCommandPool(g_dev, pool, NULL); vkDestroyBuffer(g_dev, sb, NULL); vkFreeMemory(g_dev, smem, NULL);
    return 0;
}
static void free_tex(vtex_t *t) { vkDestroyImageView(g_dev, t->view, NULL); vkDestroyImage(g_dev, t->img, NULL); vkFreeMemory(g_dev, t->mem, NULL); }

/* How a sampler binding of the fragment module is fed. */
typedef struct { VkSampler runtime; int unnormalized_expected; } feed_t;

/* Renders the 8 x 8 RGBA32F target with the fragment module `fspv`; the image binding gets `view`, a constexpr sampler binding the sampler of its decoded word (production
 * mapping), any other sampler binding `runtime` (or the default state when NULL). out = W * H * 4 floats. */
static int render(const uint8_t *fspv, size_t fsz, VkImageView view, VkSampler runtime, float *out) {
    lagfx_spv_binding_t rb[16]; size_t nb = lagfx_spv_reflect_bindings(fspv, fsz, rb, 16);
    CHECK(nb >= 1 && nb <= 16, "reflect: %zu bindings", nb);
    lagfx_spv_cxs_t cx[16]; size_t ncx = lagfx_spv_reflect_constexpr_samplers(fspv, fsz, cx, 16);
    lagfx_sampler_state_t dst = lagfx_sampler_state_default();
    VkSampler defs = lagfx_sampler_cache_get(&g_cache, &dst);
    CHECK(defs, "default sampler");
    VkDescriptorSetLayoutBinding lb[16]; VkDescriptorImageInfo dii[16]; VkWriteDescriptorSet wr[16];
    for (size_t i = 0; i < nb; i++) {
        bool img = rb[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE;
        CHECK(img || rb[i].kind == LAGFX_SPV_BINDING_SAMPLER, "unexpected binding kind %d", (int)rb[i].kind);
        lb[i] = (VkDescriptorSetLayoutBinding){ .binding = rb[i].binding, .descriptorType = img ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        VkSampler s = defs;
        if (!img) {
            size_t q = 0; while (q < ncx && cx[q].binding != rb[i].binding) q++;
            if (q < ncx) {
                lagfx_constexpr_sampler_t cs; CHECK(lagfx_constexpr_sampler_decode(cx[q].raw, &cs), "cxs word 0x%016llx does not decode", (unsigned long long)cx[q].raw);
                lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs);
                CHECK(!st.unnormalized, "a constexpr sampler must map to a normalized VkSampler (the shader converts pixel coordinates)");
                s = lagfx_sampler_cache_get(&g_cache, &st); CHECK(s, "cxs sampler");
            } else if (runtime) s = runtime;
        }
        dii[i] = img ? (VkDescriptorImageInfo){ .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } : (VkDescriptorImageInfo){ .sampler = s };
        wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = rb[i].binding, .descriptorCount = 1, .descriptorType = lb[i].descriptorType, .pImageInfo = &dii[i] };
    }
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nb, .pBindings = lb };
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(g_dev, &dlci, NULL, &dsl));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 12 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    VkDescriptorPool dpool; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dpool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; VK(vkAllocateDescriptorSets(g_dev, &dai, &dset));
    for (size_t i = 0; i < nb; i++) wr[i].dstSet = dset;
    vkUpdateDescriptorSets(g_dev, (uint32_t)nb, wr, 0, NULL);
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage rimg; VK(vkCreateImage(g_dev, &imi, NULL, &rimg));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, rimg, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory rmem; VK(vkAllocateMemory(g_dev, &mai, NULL, &rmem)); VK(vkBindImageMemory(g_dev, rimg, rmem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rimg, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView rview; VK(vkCreateImageView(g_dev, &vci, NULL, &rview));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; VK(vkCreateBuffer(g_dev, &bci, NULL, &buf)); vkGetBufferMemoryRequirements(g_dev, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; VK(vkAllocateMemory(g_dev, &mai, NULL, &bmem)); VK(vkBindBufferMemory(g_dev, buf, bmem, 0));
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fsz, .pCode = (const uint32_t *)(const void *)fspv };
    VkShaderModule vs, fs; VK(vkCreateShaderModule(g_dev, &smv, NULL, &vs)); VK(vkCreateShaderModule(g_dev, &smf, NULL, &fs));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &plci, NULL, &pl));
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
    VkPipeline pipe; VK(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool; VK(vkCreateCommandPool(g_dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(g_dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier c1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = rimg,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &c1);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = rview, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { -7.0f, -7.0f, -7.0f, -7.0f } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier c2 = c1; c2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; c2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    c2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; c2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &c2);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    float *px; VK(vkMapMemory(g_dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    memcpy(out, px, W * H * 16);
    vkUnmapMemory(g_dev, bmem);
    vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL); vkDestroyDescriptorPool(g_dev, dpool, NULL); vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
    vkDestroyShaderModule(g_dev, vs, NULL); vkDestroyShaderModule(g_dev, fs, NULL); vkDestroyCommandPool(g_dev, pool, NULL);
    vkDestroyBuffer(g_dev, buf, NULL); vkFreeMemory(g_dev, bmem, NULL); vkDestroyImageView(g_dev, rview, NULL); vkDestroyImage(g_dev, rimg, NULL); vkFreeMemory(g_dev, rmem, NULL);
    return 0;
}

static int translate(const char *fixture, uint8_t **spv, size_t *sz) {
    size_t bn = 0; uint8_t *bc = slurp(fixture, &bn);
    CHECK(bc, "fixture %s not found (run tests/fixtures/gen-sampler-fixtures.sh)", fixture);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", fixture);
    CHECK(lagfx_air2spv_translate_module(m, spv, sz) == LAGFX_OK && *spv, "%s: translate", fixture);
    lagfx_air_module_free(m); free(bc);
    return 0;
}
static bool spv_has_op(const uint8_t *spv, size_t sz, uint32_t op) {
    const uint32_t *w = (const uint32_t *)(const void *)spv;
    for (size_t i = 5; i < sz / 4;) { uint32_t o = w[i] & 0xffffu, wc = w[i] >> 16; if (!wc) break; if (o == op) return true; i += wc; }
    return false;
}
#define OP_IMAGE_QUERY_SIZE_LOD 103u

static int compare(const char *what, const float *got, const float *want, float tol) {
    int bad = 0;
    for (int i = 0; i < W * H; i++) {
        int ok = 1; for (int c = 0; c < 4; c++) if (!(fabsf(got[i * 4 + c] - want[i * 4 + c]) <= tol)) ok = 0;
        if (!ok) { if (!bad) printf("%s: first bad pixel (%d,%d): got (%g %g %g %g) want (%g %g %g %g)\n", what, i % W, i / W, got[i*4], got[i*4+1], got[i*4+2], got[i*4+3], want[i*4], want[i*4+1], want[i*4+2], want[i*4+3]); bad++; }
    }
    CHECK(!bad, "%s: %d of %d pixels differ from the reference", what, bad, W * H);
    return 0;
}

/* ---------------------------------------------------------------- test data */
static float g_t2[4 * 4 * 4], g_t3[4 * 4 * 4 * 4];
static tex_t g_T2 = { { 4, 4, 1 }, g_t2 }, g_T3 = { { 4, 4, 4 }, g_t3 };
static void init_tex(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        float *t = g_t2 + (y * 4 + x) * 4; t[0] = (1 + x) / 8.0f; t[1] = (1 + y) / 8.0f; t[2] = (x + 4 * y + 1) / 17.0f; t[3] = 1.0f;
    }
    for (int z = 0; z < 4; z++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        float *t = g_t3 + ((z * 4 + y) * 4 + x) * 4; t[0] = (1 + x) / 8.0f; t[1] = (1 + y) / 8.0f; t[2] = (1 + z) / 8.0f; t[3] = (1 + x + y + z) / 16.0f;
    }
}
/* the 2D fixtures sample at (fragment position - 1.75) texels; a pixel (x, y) has position x + 0.5 */
static void ref2d(const int am[3], int linear, float *want) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) ref_sample(&g_T2, am, linear, x + 0.5f - 1.75f, y + 0.5f - 1.75f, 0.0f, want + (y * W + x) * 4);
}

/* ---------------------------------------------------------------- A: constexpr samplers */
static uint64_t cxs_word(int addr, int filt, int pixel) {   /* layout of air2spv/constexpr_sampler.h (INFER) */
    return 0x807bff0000080000ull | (uint64_t)(addr | addr << 3 | addr << 6) | (uint64_t)filt << 9 | (uint64_t)filt << 11 | (uint64_t)pixel << 15;
}
static int test_constexpr(vtex_t *t2d) {
    static const struct { int addr, filt, pixel; } cs[] = {
        { 1, 0, 0 }, { 1, 1, 0 }, { 2, 0, 0 }, { 2, 1, 0 }, { 3, 0, 0 }, { 3, 1, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 4, 0, 0 }, { 4, 1, 0 },
        { 0, 0, 1 }, { 0, 1, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 4, 0, 1 }, { 4, 1, 1 }, { 2, 0, 1 }, { 3, 1, 1 },
    };
    static const char *an[] = { "clamp_to_zero", "clamp_to_edge", "repeat", "mirrored_repeat", "clamp_to_border" };
    for (size_t k = 0; k < sizeof cs / sizeof cs[0]; k++) {
        uint64_t word = cxs_word(cs[k].addr, cs[k].filt, cs[k].pixel);
        char name[96], what[160]; snprintf(name, sizeof name, "cxs_%016llx.air.bc", (unsigned long long)word);
        snprintf(what, sizeof what, "constexpr %s %s %s", cs[k].pixel ? "PIXEL" : "normalized", an[cs[k].addr], cs[k].filt ? "linear" : "nearest");
        uint8_t *spv; size_t sz; if (translate(name, &spv, &sz)) return 1;
        lagfx_spv_cxs_t cx[4]; size_t ncx = lagfx_spv_reflect_constexpr_samplers(spv, sz, cx, 4);
        CHECK(ncx == 1 && cx[0].raw == word, "%s: reflected %zu constexpr samplers (word 0x%016llx)", what, ncx, ncx ? (unsigned long long)cx[0].raw : 0ull);
        /* the pixel case is converted IN the shader (explicit division by the image size), the normalized one is not */
        CHECK(spv_has_op(spv, sz, OP_IMAGE_QUERY_SIZE_LOD) == (cs[k].pixel != 0), "%s: OpImageQuerySizeLod %s", what, cs[k].pixel ? "missing" : "unexpected");
        int am[3] = { cs[k].addr, cs[k].addr, cs[k].addr };
        float want[W * H * 4], got[W * H * 4]; ref2d(am, cs[k].filt, want);
        if (render(spv, sz, t2d->view, NULL, got)) return 1;
        if (compare(what, got, want, 0.012f)) return 1;
        printf("PASS: %s: all %d pixels match the reference\n", what, W * H);
        free(spv);
    }
    return 0;
}

/* ---------------------------------------------------------------- B: runtime sampler descriptors */
/* Metal serialized sampler state (src/protocol/arm64_sampler.h): [0] min [2] mag filter, [10:8] [14:12] [18:16] address S/T/R, [25:24] mip, [31] normalized */
static uint32_t msl_state(int minf, int magf, int addr, int normalized) { return (uint32_t)minf | (uint32_t)magf << 2 | (uint32_t)addr << 8 | (uint32_t)addr << 12 | (uint32_t)addr << 16 | (uint32_t)(normalized ? 1u : 0u) << 31; }
static VkSampler make_runtime_sampler(uint32_t state, uint32_t *flags) {
    uint8_t d[36]; memset(d, 0, sizeof d);
    uint32_t tag = 3, size = 36, lo = 0, hi = 0x7f7fffffu;   /* lod clamps: 0 .. FLT_MAX */
    memcpy(d, &tag, 4); memcpy(d + 4, &size, 4); memcpy(d + 12, &state, 4); memcpy(d + 20, &lo, 4); memcpy(d + 24, &hi, 4);
    VkSamplerCreateInfo ci; *flags = lagfx_a64_sampler_decode_ex(d, sizeof d, false, 1.0f, g_have_mirror_clamp, &ci);
    VkSampler s = VK_NULL_HANDLE;
    return vkCreateSampler(g_dev, &ci, NULL, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}
static int test_runtime(vtex_t *t2d) {
    uint8_t *spv_n, *spv_p; size_t sz_n, sz_p;
    if (translate("smp_norm.air.bc", &spv_n, &sz_n) || translate("smp_pix.air.bc", &spv_p, &sz_p)) return 1;
    uint8_t *lod0 = NULL; size_t lod0_sz = 0;
    CHECK(lagfx_spv_explicit_lod0(spv_p, sz_p, &lod0, &lod0_sz) == 1, "explicit LOD 0 variant of the texel-coordinate shader");
    static const char *mn[] = { "clamp_to_edge", "mirror_clamp_to_edge", "repeat", "mirrored_repeat", "clamp_to_zero", "clamp_to_border" };
    static const int mam[] = { AM_EDGE, AM_MIRROR_CLAMP, AM_REPEAT, AM_MIRROR, AM_ZERO, AM_BORDER };
    for (int m = 0; m < 6; m++) for (int f = 0; f < 2; f++) {
        char what[128]; snprintf(what, sizeof what, "runtime sampler %s %s", mn[m], f ? "linear" : "nearest");
        uint32_t fl; VkSampler s = make_runtime_sampler(msl_state(f, f, m, 1), &fl); CHECK(s, "%s: vkCreateSampler", what);
        if (m == 1 && !g_have_mirror_clamp) { printf("SKIP: %s (the device lacks samplerMirrorClampToEdge)\n", what); vkDestroySampler(g_dev, s, NULL); continue; }
        CHECK(!(fl & LAGFX_A64_SAMP_UNNORM_APPLIED) && ((fl & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX) == 0), "%s: flags 0x%x", what, fl);
        int am[3] = { mam[m], mam[m], mam[m] };
        float want[W * H * 4], got[W * H * 4]; ref2d(am, f, want);
        if (render(spv_n, sz_n, t2d->view, s, got)) return 1;
        if (compare(what, got, want, 0.012f)) return 1;
        printf("PASS: %s: all %d pixels match the reference\n", what, W * H);
        vkDestroySampler(g_dev, s, NULL);
    }
    {   /* min and mag filter differ (normalized): the exec binds both; the shader samples 1 texel per pixel, lambda = 0, which Vulkan magnifies (Vulkan semantics, not Metal) */
        char what[] = "runtime sampler min=nearest mag=linear (normalized, 1 texel per pixel: lambda 0 magnifies)";
        uint32_t fl; VkSampler s = make_runtime_sampler(msl_state(0, 1, 0, 1), &fl); CHECK(s, "vkCreateSampler");
        int am[3] = { AM_EDGE, AM_EDGE, AM_EDGE };
        float want[W * H * 4], got[W * H * 4]; ref2d(am, 1, want);
        if (render(spv_n, sz_n, t2d->view, s, got) || compare(what, got, want, 0.012f)) return 1;
        printf("PASS: %s: all %d pixels match the reference (magnification filter)\n", what, W * H);
        vkDestroySampler(g_dev, s, NULL);
    }
    /* unnormalized coordinates: Vulkan's restrictions (equal filters, edge / border addressing, level 0) applied by the decoder, explicit-LOD shader variant */
    static const struct { int addr, minf, magf; } un[] = { { 0, 0, 0 }, { 0, 1, 1 }, { 4, 0, 0 }, { 4, 1, 1 }, { 5, 1, 1 }, { 0, 0, 1 }, { 0, 1, 0 } };   /* the last two: unequal filters, Vulkan needs equal ones: the minification one is kept (INFER) */
    for (size_t k = 0; k < sizeof un / sizeof un[0]; k++) {
        char what[128]; snprintf(what, sizeof what, "runtime sampler UNNORMALIZED %s min=%s mag=%s", mn[un[k].addr], un[k].minf ? "linear" : "nearest", un[k].magf ? "linear" : "nearest");
        uint32_t fl; VkSampler s = make_runtime_sampler(msl_state(un[k].minf, un[k].magf, un[k].addr, 0), &fl); CHECK(s, "%s: vkCreateSampler", what);
        CHECK(fl & LAGFX_A64_SAMP_UNNORM_APPLIED, "%s: UNNORM flag missing", what);
        int am[3] = { mam[un[k].addr], mam[un[k].addr], mam[un[k].addr] };
        float want[W * H * 4], got[W * H * 4]; ref2d(am, un[k].minf, want);
        if (render(lod0, lod0_sz, t2d->view, s, got) || compare(what, got, want, 0.012f)) return 1;
        printf("PASS: %s: all %d pixels match the reference\n", what, W * H);
        vkDestroySampler(g_dev, s, NULL);
    }
    {   /* flags: compare / lodAverage / anisotropy are not mapped and say so */
        uint32_t fl; VkSampler s = make_runtime_sampler(msl_state(1, 1, 0, 1) | 0x10u, &fl);   /* bit 4 lodAverage */
        CHECK(fl & LAGFX_A64_SAMP_LODAVG_IGNORED, "lodAverage not reported (flags 0x%x)", fl);
        vkDestroySampler(g_dev, s, NULL);
        s = make_runtime_sampler(msl_state(1, 1, 0, 1) | 3u << 5, &fl);   /* compare function 3 */
        CHECK(fl & LAGFX_A64_SAMP_COMPARE_IGNORED, "compare not reported (flags 0x%x)", fl);
        vkDestroySampler(g_dev, s, NULL);
    }
    free(lod0); free(spv_n); free(spv_p);
    return 0;
}

/* ---------------------------------------------------------------- C: texture3d */
static int test_3d(void) {
    vtex_t t3; if (make_tex(&g_T3, 1, &t3)) return 1;
    uint8_t *spv; size_t sz;
    int am[3] = { AM_EDGE, AM_EDGE, AM_EDGE };
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    for (int f = 0; f < 2; f++) {
        sci.magFilter = sci.minFilter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSampler s; VK(vkCreateSampler(g_dev, &sci, NULL, &s));
        for (int fx = 0; fx < 2; fx++) {
            const char *fixture = fx ? "tex3d_diag.air.bc" : "tex3d_centers.air.bc";
            if (translate(fixture, &spv, &sz)) return 1;
            lagfx_spv_binding_t rb[8]; size_t nb = lagfx_spv_reflect_bindings(spv, sz, rb, 8);
            int dim = 0; for (size_t i = 0; i < nb; i++) if (rb[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE) dim = rb[i].image_dim;
            CHECK(dim == 2, "%s: the texture3d binding reflects Dim %d, want 2 (Dim3D)", fixture, dim);
            float want[W * H * 4], got[W * H * 4];
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                float px = x + 0.5f, py = y + 0.5f, w = fx ? (px + py) / 16.0f : 0.375f;
                ref_sample(&g_T3, am, f, px / 8.0f * 4.0f, py / 8.0f * 4.0f, w * 4.0f, want + (y * W + x) * 4);
            }
            if (render(spv, sz, t3.view, s, got)) return 1;
            char what[96]; snprintf(what, sizeof what, "texture3d %s %s", fx ? "diagonal (z interpolation)" : "texel rows at slice 1", f ? "trilinear" : "nearest");
            if (compare(what, got, want, 0.012f)) return 1;
            if (!fx && !f) {   /* the exact-texel claim: pixel (x, y) is texel (x >> 1, y >> 1, 1) of the pattern */
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                    const float *tx = g_t3 + ((1 * 4 + (y >> 1)) * 4 + (x >> 1)) * 4;
                    CHECK(fabsf(got[(y * W + x) * 4] - tx[0]) < 1e-6f && fabsf(got[(y * W + x) * 4 + 1] - tx[1]) < 1e-6f && fabsf(got[(y * W + x) * 4 + 2] - tx[2]) < 1e-6f && fabsf(got[(y * W + x) * 4 + 3] - tx[3]) < 1e-6f,
                          "texel (%d,%d,1) not read back exactly", x >> 1, y >> 1);
                }
            }
            printf("PASS: %s: all %d pixels match the reference\n", what, W * H);
            if (fx && f) {   /* the z interpolation must matter: a 2D slice (z dropped) would give a different image */
                float slice[W * H * 4], diff = 0.0f;
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) ref_sample(&g_T3, am, 1, (x + 0.5f) / 2.0f, (y + 0.5f) / 2.0f, 0.5f, slice + (y * W + x) * 4);
                for (int i = 0; i < W * H * 4; i++) diff = fmaxf(diff, fabsf(slice[i] - got[i]));
                CHECK(diff > 0.05f, "the diagonal does not distinguish a 3D sample from a 2D slice (max diff %g)", diff);
            }
            free(spv);
        }
        vkDestroySampler(g_dev, s, NULL);
    }
    free_tex(&t3);
    return 0;
}


/* ---------------------------------------------------------------- D: texture2d_array */
#define AL 4   /* layers */
#define AM 4   /* mip levels of the 8 x 8 texture */
static float g_arr[AL][AM][8 * 8 * 4];   /* [layer][level] texels (s = 8 >> level): R = (1 + x) / 16, G = (1 + y) / 16, B = (1 + level) / 8, A = (1 + layer) / 8 */
static void init_arr(void) {
    for (int l = 0; l < AL; l++) for (int m = 0; m < AM; m++) {
        int s = 8 >> m;
        for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) { float *t = g_arr[l][m] + (y * s + x) * 4; t[0] = (1 + x) / 16.0f; t[1] = (1 + y) / 16.0f; t[2] = (1 + m) / 8.0f; t[3] = (1 + l) / 8.0f; }
    }
}
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView varr, v2d; } atex_t;
static int make_arr_tex(atex_t *out) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = { 8, 8, 1 }, .mipLevels = AM, .arrayLayers = AL,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(g_dev, &ici, NULL, &out->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, out->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(g_dev, &mai, NULL, &out->mem)); VK(vkBindImageMemory(g_dev, out->img, out->mem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->img, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, AM, 0, AL } };
    VK(vkCreateImageView(g_dev, &vci, NULL, &out->varr));
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.subresourceRange.layerCount = 1;
    VK(vkCreateImageView(g_dev, &vci, NULL, &out->v2d));
    size_t per_layer = 0, offs[AM];
    for (int m = 0; m < AM; m++) { offs[m] = per_layer; per_layer += (size_t)(8 >> m) * (8 >> m) * 16; }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = per_layer * AL, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer sb; VK(vkCreateBuffer(g_dev, &bci, NULL, &sb)); vkGetBufferMemoryRequirements(g_dev, sb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory smem; VK(vkAllocateMemory(g_dev, &mai, NULL, &smem)); VK(vkBindBufferMemory(g_dev, sb, smem, 0));
    uint8_t *p; VK(vkMapMemory(g_dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    for (int l = 0; l < AL; l++) for (int m = 0; m < AM; m++) memcpy(p + per_layer * l + offs[m], g_arr[l][m], (size_t)(8 >> m) * (8 >> m) * 16);
    vkUnmapMemory(g_dev, smem);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool; VK(vkCreateCommandPool(g_dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(g_dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = out->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, AM, 0, AL } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    for (int l = 0; l < AL; l++) for (int m = 0; m < AM; m++) {
        VkBufferImageCopy bic = { .bufferOffset = per_layer * l + offs[m], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, (uint32_t)l, 1 }, .imageExtent = { (uint32_t)(8 >> m), (uint32_t)(8 >> m), 1 } };
        vkCmdCopyBufferToImage(cmd, sb, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    }
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    vkDestroyCommandPool(g_dev, pool, NULL); vkDestroyBuffer(g_dev, sb, NULL); vkFreeMemory(g_dev, smem, NULL);
    return 0;
}

/* CPU model: pixel (x, y) reads level x / 2 of layer y / 2 (explicit LOD = column, array index = row), nearest, clamp to edge; `layer_of` / `level_of` pick what the module is expected to
 * select (a control may drop the layer). The texel is looked up with the independent reference sampler on that level's own texture. */
static void ref_arr(int level_fixed, int layer_fixed, float *want) {
    int am[3] = { AM_EDGE, AM_EDGE, AM_EDGE };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int lvl = level_fixed >= 0 ? level_fixed : x >> 1, lay = layer_fixed >= 0 ? layer_fixed : y >> 1;
        int s = 8 >> lvl; tex_t T = { { s, s, 1 }, g_arr[lay][lvl] };
        ref_sample(&T, am, 0, (x + 0.5f) / 8.0f * (float)s, (y + 0.5f) / 8.0f * (float)s, 0.0f, want + (y * W + x) * 4);
    }
}
static int spirv_valid(const uint8_t *spv, size_t sz, const char *what) {
    if (system("spirv-val --version >/dev/null 2>&1") != 0) return 0;   /* tool absent: the validation layer at pipeline creation is the check */
    char path[64] = "/tmp/lagfx-arr-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0, "mkstemp");
    FILE *f = fdopen(fd, "wb"); fwrite(spv, 1, sz, f); fclose(f);
    char cmd[160]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s >/dev/null 2>&1", path);
    int rc = system(cmd); remove(path);
    CHECK(rc == 0, "%s: spirv-val rejects the translated module", what);
    return 0;
}
static int test_array(void) {
    atex_t at; init_arr(); if (make_arr_tex(&at)) return 1;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = (float)(AM - 1) };
    VkSampler s; VK(vkCreateSampler(g_dev, &sci, NULL, &s));
    float want[W * H * 4], got[W * H * 4];
    uint8_t *spv; size_t sz;
    /* 1. explicit level = column, array index = row: sixteen (layer, level) pairs, each read exactly */
    if (translate("arr_lod.air.bc", &spv, &sz)) return 1;
    {
        lagfx_spv_binding_t rb[8]; size_t nb = lagfx_spv_reflect_bindings(spv, sz, rb, 8);
        int dim = -1, arrd = -1; for (size_t i = 0; i < nb; i++) if (rb[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE) dim = rb[i].image_dim, arrd = rb[i].image_arrayed;
        CHECK(dim == 1 && arrd == 1, "arr_lod: the texture2d_array binding reflects Dim %d arrayed %d, want 1 / 1", dim, arrd);
    }
    if (spirv_valid(spv, sz, "arr_lod")) return 1;
    ref_arr(-1, -1, want);
    if (render(spv, sz, at.varr, s, got) || compare("texture2d_array: level = column, layer = row", got, want, 1e-6f)) return 1;
    {   /* the sixteen cells really are sixteen different texels */
        int distinct = 0; for (int a = 0; a < 16; a++) { int dup = 0; for (int b = 0; b < a; b++) if (!memcmp(got + ((a / 4 * 2) * W + a % 4 * 2) * 4, got + ((b / 4 * 2) * W + b % 4 * 2) * 4, 16)) dup = 1; distinct += !dup; }
        CHECK(distinct == 16, "the (layer, level) cells are not all distinct (%d)", distinct);
    }
    printf("PASS: texture2d_array explicit level = column, layer = row: all %d pixels match (16 distinct (layer, level) texels)\n", W * H);
    /* negative control: a layer-0 view of the same image does not give the array result -- the array shader on a layer-0 2D_ARRAY view of one layer clamps every row to layer 0 */
    {
        VkImageView one; VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = at.img, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, AM, 0, 1 } };
        VK(vkCreateImageView(g_dev, &vci, NULL, &one));
        float w0[W * H * 4]; ref_arr(-1, 0, w0);
        if (render(spv, sz, one, s, got) || compare("texture2d_array on a one-layer array view (layer index clamped to 0)", got, w0, 1e-6f)) return 1;
        float diff = 0; for (int i = 0; i < W * H * 4; i++) diff = fmaxf(diff, fabsf(w0[i] - want[i]));
        CHECK(diff > 0.1f, "negative control does not differ from the full array (diff %g)", diff);
        vkDestroyImageView(g_dev, one, NULL);
        printf("PASS: negative control: a one-layer array view clamps every row to layer 0 (differs from the 4-layer result by %.2f)\n", diff);
    }
    /* negative control: the same image through a view with ONE level: LOD clamped to level 0 for every column */
    {
        VkImageView lv0; VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = at.img, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, AL } };
        VK(vkCreateImageView(g_dev, &vci, NULL, &lv0));
        float w1[W * H * 4]; ref_arr(0, -1, w1);
        if (render(spv, sz, lv0, s, got) || compare("texture2d_array on a level-0-only view (LOD clamped to the view)", got, w1, 1e-6f)) return 1;
        vkDestroyImageView(g_dev, lv0, NULL);
        printf("PASS: negative control: a level-0-only view reads level 0 in every column\n");
    }
    free(spv);
    /* 2. constexpr PIXEL sampler on an array texture: coordinate texels / size of level 0 (the size query of an arrayed image has 3 components) */
    if (translate("arr_cxs_pix.air.bc", &spv, &sz)) return 1;
    CHECK(spv_has_op(spv, sz, OP_IMAGE_QUERY_SIZE_LOD), "arr_cxs_pix: no OpImageQuerySizeLod");
    if (spirv_valid(spv, sz, "arr_cxs_pix")) return 1;
    ref_arr(0, -1, want);
    if (render(spv, sz, at.varr, s, got) || compare("texture2d_array constexpr PIXEL sampler, level 0, layer = row", got, want, 1e-6f)) return 1;
    printf("PASS: texture2d_array with a constexpr pixel sampler: size query shuffled to (w, h), layer = row, all %d pixels match\n", W * H);
    free(spv);
    /* 3. control: the module whose texture argument is NOT marked as an array: the index is dropped (layer 0), a plain 2D view of layer 0 is what it needs */
    if (translate("arr_plainarg.air.bc", &spv, &sz)) return 1;
    {
        lagfx_spv_binding_t rb[8]; size_t nb = lagfx_spv_reflect_bindings(spv, sz, rb, 8);
        int arrd = -1; for (size_t i = 0; i < nb; i++) if (rb[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE) arrd = rb[i].image_arrayed;
        CHECK(arrd == 0, "arr_plainarg: binding reflects arrayed %d, want 0 (the metadata does not name an array texture)", arrd);
    }
    if (spirv_valid(spv, sz, "arr_plainarg")) return 1;
    ref_arr(-1, 0, want);
    if (render(spv, sz, at.v2d, s, got) || compare("array call on a texture2d argument: layer index dropped, layer 0", got, want, 1e-6f)) return 1;
    printf("PASS: array call without array argument metadata falls back to layer 0 of a plain 2D image (the translator logs a warning)\n");
    free(spv);
    vkDestroySampler(g_dev, s, NULL);
    return 0;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sampler-lavapipe", .apiVersion = VK_API_VERSION_1_3 };
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
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) SKIP("RGBA32F is not linearly filterable");
    VkPhysicalDeviceVulkan12Features p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 pf2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &p12 };
    vkGetPhysicalDeviceFeatures2(pd, &pf2);
    g_have_mirror_clamp = p12.samplerMirrorClampToEdge == VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .samplerMirrorClampToEdge = p12.samplerMirrorClampToEdge };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    VkDevice dev; VK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    g_pd = pd; g_dev = dev; g_q = q;
    lagfx_sampler_cache_init(&g_cache, dev, false);
    init_tex();
    vtex_t t2d; if (make_tex(&g_T2, 0, &t2d)) return 1;
    int rc = test_constexpr(&t2d);
    if (!rc) rc = test_runtime(&t2d);
    if (!rc) rc = test_3d();
    if (!rc) rc = test_array();
    if (rc) return rc;
    printf("PASS air2spv-sampler-lavapipe on %s (mirror clamp to edge %s)\n", pp.deviceName, g_have_mirror_clamp ? "exact" : "not available");
    return 0;
}
