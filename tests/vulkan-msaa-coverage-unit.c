/*
 * libapplegfx-vulkan — multisample coverage, resolve and MS texel fetch against a CPU reference (lavapipe + validation layer; NOT a prediction for Adreno).
 * tests/vulkan-msaa-coverage-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * One triangle that covers some pixels partly is drawn at 4x (2x where the device reports it) into BGRA8 and RGBA16F targets created through lagfx_msaa_plan /
 * lagfx_msaa_image_create (the same functions the guest path of src/protocol/arm64_msaa.c uses), then
 *   A. read back sample by sample with OpImageFetch + the Sample operand on a texture2DMS (compute shader, textureSamples() too) and compared EXACTLY with the CPU reference
 *      (Vulkan standard sample locations, edge functions in double precision; a sample on an edge would make the reference ambiguous: the geometry is chosen so none does);
 *   B. resolved by the dynamic-rendering resolve attachment (store action 2 "MultisampleResolve" and 3 "StoreAndMultisampleResolve") and by vkCmdResolveImage after a plain Store,
 *      compared with the CPU average of the reference samples.
 * Tolerances (documented here because they are the only non-exact comparisons): per-sample fetch: exact (UNORM 0 / 255 and half 0 / 1 survive the format unchanged). Resolved
 * BGRA8: |value - round(255 * covered / N)| <= 1 (the average is taken in float and rounded once to UNORM; implementations may differ in the last bit). Resolved RGBA16F:
 * |value - covered / N| <= 1e-3 (k / 4 and k / 2 are exact in half; the slack is for implementations that average in another order). The "resolve by store action" and the
 * "vkCmdResolveImage" results are compared with each other with the same tolerance.
 * Negative controls: a reference built with the triangle shifted by one pixel must NOT match (the test can fail); Store without resolve leaves a sentinel resolve target
 * untouched; the 1x render holds only 0 / 255 coverage; a pipeline whose sample count differs from the attachments' is flagged by the validation layer.
 * Skips (exit 77) without a usable Vulkan device. 2x is skipped, explicitly, where the device does not report it (lavapipe: 1 and 4).
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/msaa.h"
#include "vulkan/pipeline_build.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
extern uint64_t lagfx_vk_validation_msg_count;

#include "arm64-exec-run-shaders.h"
#include "arm64-msaa-shaders.h"

#define W 16u
#define H 16u
#define MAXS 8u

/* triangle in NDC (y down in framebuffer space, no flip: this test builds its own pipeline) */
static const float TRI[6] = { -0.90f, -0.80f, 0.85f, -0.30f, -0.20f, 0.90f };

/* Vulkan standard sample locations (spec "Multisample Coverage": standardSampleLocations), relative to the pixel's upper-left corner, y down */
static const double LOC2[2][2] = { { 0.75, 0.75 }, { 0.25, 0.25 } };
static const double LOC4[4][2] = { { 0.375, 0.125 }, { 0.875, 0.375 }, { 0.125, 0.625 }, { 0.625, 0.875 } };
static const double LOC1[1][2] = { { 0.5, 0.5 } };

static double edge_fn(double ax, double ay, double bx, double by, double px, double py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
/* 1 covered, 0 not, -1 ambiguous (sample within 1e-6 of an edge: the top-left rule would decide, the geometry must avoid it) */
static int sample_covered(const float *tri, double dx, double dy, unsigned px, unsigned py, double sx, double sy) {
    double x[3], y[3];
    for (int i = 0; i < 3; i++) x[i] = ((double)tri[2 * i] + 1.0) * 0.5 * W + dx, y[i] = ((double)tri[2 * i + 1] + 1.0) * 0.5 * H + dy;
    const double X = px + sx, Y = py + sy;
    double e0 = edge_fn(x[0], y[0], x[1], y[1], X, Y), e1 = edge_fn(x[1], y[1], x[2], y[2], X, Y), e2 = edge_fn(x[2], y[2], x[0], y[0], X, Y);
    if (fabs(e0) < 1e-6 || fabs(e1) < 1e-6 || fabs(e2) < 1e-6) return -1;
    return (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
}
static const double (*locs(unsigned n))[2] { return n == 4 ? LOC4 : n == 2 ? LOC2 : LOC1; }

static float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

typedef struct {
    uint8_t res[W * H * 8];          /* resolved image bytes (tight) */
    float samples[W * H * MAXS][4];  /* fetched samples (RGBA) */
    uint32_t nsamples;               /* textureSamples() */
    bool have_res, have_samples;
} out_t;

enum { M_STORE_FETCH_RESOLVE_CMD, M_ACTION2, M_ACTION3_FETCH, M_STORE_ONLY_SENTINEL };

static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static int hostbuf(struct lagfx_vk_state *vk, VkDeviceSize sz, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(vk->device, &bi, NULL, b) == VK_SUCCESS, "buffer");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk->device, *b, &mr);
    uint32_t mt = memtype(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mt != UINT32_MAX, "host memory");
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    CHECK(vkAllocateMemory(vk->device, &ai, NULL, m) == VK_SUCCESS && vkBindBufferMemory(vk->device, *b, *m, 0) == VK_SUCCESS && vkMapMemory(vk->device, *m, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS, "bind");
    memset(*map, 0, (size_t)sz);
    return 0;
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One case: format fmt, N samples, mode. pipe_samples = the pipeline's rasterization samples (0 = N). */
static int run_case(struct lagfx_vk_state *vk, const lagfx_fmtcap_backend_t *be, VkFormat fmt, unsigned N, int mode, unsigned pipe_samples, const float *tri, out_t *out, int *plan_sampled) {
    VkDevice dev = vk->device;
    memset(out, 0, sizeof *out);
    /* the same plan the guest path makes: colour attachment required, SAMPLED for the texel fetch, transfers only for the 1x fallback */
    lagfx_msaa_plan_t pl;
    CHECK(lagfx_msaa_plan(be, fmt, W, H, N, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &pl) == LAGFX_MSAA_OK, "plan");
    CHECK(pl.samples == N && !pl.clamped && !pl.fallback_1x, "plan grants %u of %u", (unsigned)pl.samples, N);
    if (plan_sampled) *plan_sampled = (pl.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    VkImage ms, rs = VK_NULL_HANDLE; VkDeviceMemory msm, rsm = VK_NULL_HANDLE; VkImageView msv, rsv = VK_NULL_HANDLE, msv_s = VK_NULL_HANDLE;
    CHECK(lagfx_msaa_image_create(vk, fmt, W, H, pl.samples, pl.usage, VK_IMAGE_ASPECT_COLOR_BIT, &ms, &msm, &msv), "ms image");
    const bool need_res = mode != M_ACTION3_FETCH || true;
    if (need_res) {
        const VkImageUsageFlags ru = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(lagfx_msaa_image_create(vk, fmt, W, H, VK_SAMPLE_COUNT_1_BIT, ru, VK_IMAGE_ASPECT_COLOR_BIT, &rs, &rsm, &rsv), "resolve image");
    }
    const bool do_fetch = N > 1 && (mode == M_ACTION3_FETCH || mode == M_STORE_FETCH_RESOLVE_CMD) && (pl.usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    if (do_fetch) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ms, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &vi, NULL, &msv_s) == VK_SUCCESS, "sampled view");
    }
    /* pipeline */
    VkShaderModule vs, fs, cs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
    CHECK(vkCreateShaderModule(dev, &sci, NULL, &vs) == VK_SUCCESS, "vs");
    sci.codeSize = sizeof k_fs_spv, sci.pCode = k_fs_spv;
    CHECK(vkCreateShaderModule(dev, &sci, NULL, &fs) == VK_SUCCESS, "fs");
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = (uint32_t)fmt;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    d.blend_set = 1, d.color_write_mask = 0xf, d.blend_src_rgb = d.blend_src_a = VK_BLEND_FACTOR_ONE, d.blend_dst_rgb = d.blend_dst_a = VK_BLEND_FACTOR_ZERO;
    d.raster_samples = (uint8_t)(pipe_samples ? pipe_samples : N);
    VkPipeline pipe;
    CHECK(lagfx_pipeline_build(dev, &d, &pipe) == LAGFX_OK, "pipeline build");
    /* compute (texel fetch) */
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout cpl = VK_NULL_HANDLE; VkPipeline cpipe = VK_NULL_HANDLE; VkDescriptorPool dpool = VK_NULL_HANDLE; VkDescriptorSet dset = VK_NULL_HANDLE;
    VkBuffer ob = VK_NULL_HANDLE, cb_buf = VK_NULL_HANDLE; VkDeviceMemory om = VK_NULL_HANDLE, cm = VK_NULL_HANDLE; void *omap = NULL, *cmap = NULL;
    if (do_fetch) {
        sci.codeSize = sizeof k_cs_ms_fetch_spv, sci.pCode = k_cs_ms_fetch_spv;
        CHECK(vkCreateShaderModule(dev, &sci, NULL, &cs) == VK_SUCCESS, "cs");
        VkDescriptorSetLayoutBinding lb[3] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                               { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = lb };
        CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
        CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &cpl) == VK_SUCCESS, "cpl");
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" }, .layout = cpl };
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe) == VK_SUCCESS, "compute pipeline");
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
        CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool) == VK_SUCCESS, "dpool");
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        CHECK(vkAllocateDescriptorSets(dev, &dai, &dset) == VK_SUCCESS, "dset");
        if (hostbuf(vk, sizeof out->samples, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &ob, &om, &omap)) return 1;
        if (hostbuf(vk, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &cb_buf, &cm, &cmap)) return 1;
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, msv_s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo b0 = { ob, 0, VK_WHOLE_SIZE }, b1 = { cb_buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr[3] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
                                       { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0 },
                                       { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1 } };
        vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);
    }
    /* vertex + readback buffers */
    VkBuffer vb, rb; VkDeviceMemory vm, rm; void *vmap, *rmap;
    if (hostbuf(vk, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vm, &vmap)) return 1;
    memcpy(vmap, tri, 6 * sizeof(float));
    if (hostbuf(vk, W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rm, &rmap)) return 1;

    VkCommandBuffer cb; CHECK(lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK, "cb");
    VkCommandBufferBeginInfo bg = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bg);
    img_barrier(cb, ms, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    {   /* resolve target: a sentinel (0.3 everywhere) so "not resolved" differs from "resolved to 0" */
        img_barrier(cb, rs, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cc = { .float32 = { 0.3f, 0.3f, 0.3f, 0.3f } };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, rs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
        const bool dyn_res = mode == M_ACTION2 || mode == M_ACTION3_FETCH;
        if (dyn_res) img_barrier(cb, rs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    const uint32_t action = mode == M_ACTION2 ? 2u : mode == M_ACTION3_FETCH ? 3u : 1u;
    VkAttachmentStoreOp so; bool res;
    CHECK(lagfx_msaa_store_map(action, true, &so, &res), "store map");
    VkClearColorValue clr = { .float32 = { 0, 0, 0, 0 } };
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = msv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = so,
        .clearValue = { .color = clr } };
    if (res && N > 1) ca.resolveMode = lagfx_msaa_resolve_mode(fmt), ca.resolveImageView = rsv, ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    const bool one_x = N == 1;   /* 1x: nothing to resolve, read the rendered image itself */
    VkImage final_img = one_x ? ms : rs;
    const bool dyn_res = mode == M_ACTION2 || mode == M_ACTION3_FETCH || one_x;
    VkImageLayout final_layout = dyn_res ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (mode == M_STORE_FETCH_RESOLVE_CMD && N > 1) {   /* resolve after the pass with vkCmdResolveImage */
        img_barrier(cb, ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        /* the image has no TRANSFER_SRC usage (a multisampled image is made for attachment + sampling only): vkCmdResolveImage needs it, so this path is only taken on an image
         * created for it: see below, the test re-creates the MS image with the extra usage when the format allows */
    }
    if (do_fetch) {   /* make the MS image readable by the compute shader */
        if (mode == M_STORE_FETCH_RESOLVE_CMD && N > 1) { /* layout is TRANSFER_SRC from the barrier above */ img_barrier(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); }
        else img_barrier(cb, ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 1, &dset, 0, NULL);
        vkCmdDispatch(cb, W, H, 1);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    img_barrier(cb, final_img, final_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dyn_res ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                dyn_res ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, final_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &cp);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; vkCreateFence(dev, &fi, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(lagfx_vk_submit_checked(vk, &si, fence) == LAGFX_OK && lagfx_vk_wait_bounded(vk, fence, 0) == LAGFX_VK_WAIT_OK, "submit");
    memcpy(out->res, rmap, W * H * (fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u)); out->have_res = true;
    if (do_fetch) { memcpy(out->samples, omap, sizeof out->samples); out->nsamples = *(uint32_t *)cmap; out->have_samples = true; }
    vkDestroyFence(dev, fence, NULL); lagfx_vk_cmdbuf_free(vk, cb);
    vkDestroyBuffer(dev, vb, NULL); vkFreeMemory(dev, vm, NULL); vkDestroyBuffer(dev, rb, NULL); vkFreeMemory(dev, rm, NULL);
    if (do_fetch) { vkDestroyPipeline(dev, cpipe, NULL); vkDestroyPipelineLayout(dev, cpl, NULL); vkDestroyDescriptorPool(dev, dpool, NULL); vkDestroyDescriptorSetLayout(dev, dsl, NULL); vkDestroyShaderModule(dev, cs, NULL);
                    vkDestroyBuffer(dev, ob, NULL); vkFreeMemory(dev, om, NULL); vkDestroyBuffer(dev, cb_buf, NULL); vkFreeMemory(dev, cm, NULL); vkDestroyImageView(dev, msv_s, NULL); }
    vkDestroyPipeline(dev, pipe, NULL); vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyImageView(dev, msv, NULL); vkDestroyImage(dev, ms, NULL); vkFreeMemory(dev, msm, NULL);
    vkDestroyImageView(dev, rsv, NULL); vkDestroyImage(dev, rs, NULL); vkFreeMemory(dev, rsm, NULL);
    return 0;
}

/* reference: covered samples of every pixel (and ambiguity check) */
static int reference(const float *tri, unsigned N, double dx, double dy, uint8_t cov[W * H][MAXS], unsigned *edge_px, unsigned *full_px) {
    const double (*L)[2] = locs(N);
    *edge_px = *full_px = 0;
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        unsigned c = 0;
        for (unsigned s = 0; s < N; s++) {
            int r = sample_covered(tri, dx, dy, x, y, L[s][0], L[s][1]);
            if (r < 0) return -1;
            cov[y * W + x][s] = (uint8_t)r, c += (unsigned)r;
        }
        if (c == N) (*full_px)++; else if (c) (*edge_px)++;
    }
    return 0;
}

static int check_case(struct lagfx_vk_state *vk, const lagfx_fmtcap_backend_t *be, VkFormat fmt, unsigned N) {
    const char *fn = fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? "RGBA16F" : "BGRA8";
    static uint8_t cov[W * H][MAXS], cov_shift[W * H][MAXS];
    unsigned edge, full, e2, f2;
    CHECK(reference(TRI, N, 0, 0, cov, &edge, &full) == 0, "reference ambiguous: a sample lies on an edge");
    CHECK(edge >= 6 && full >= 40, "%s %ux: the triangle must cover pixels partly (%u) and fully (%u)", fn, N, edge, full);
    CHECK(reference(TRI, N, 1.0, 0, cov_shift, &e2, &f2) == 0, "shifted reference ambiguous");
    static out_t o;
    int plan_sampled = 0;
    /* A. no resolve at all, sample-by-sample fetch (action 3 keeps the MS content too) + the dynamic-rendering resolve of the same render */
    CHECK(run_case(vk, be, fmt, N, M_ACTION3_FETCH, 0, TRI, &o, &plan_sampled) == 0, "%s %ux action 3 render", fn, N);
    CHECK(plan_sampled && o.have_samples, "%s %ux: the plan granted SAMPLED on the multisampled image", fn, N);
    CHECK(o.nsamples == N, "%s %ux: textureSamples() = %u", fn, N, o.nsamples);
    unsigned bad = 0, bad_shift = 0, n_cov = 0;
    for (unsigned p = 0; p < W * H; p++) for (unsigned s = 0; s < N; s++) {
        const float *v = o.samples[p * MAXS + s];
        const bool is_red = v[0] == 1.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 1.0f, is_clear = v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f;
        if (!(is_red || is_clear)) { bad++; continue; }
        if (is_red != (cov[p][s] != 0)) bad++;
        if (is_red != (cov_shift[p][s] != 0)) bad_shift++;
        n_cov += is_red;
    }
    CHECK(bad == 0, "%s %ux: %u fetched samples differ from the CPU reference", fn, N, bad);
    CHECK(bad_shift > 4, "negative control: the reference shifted by a pixel must NOT match (differing samples %u)", bad_shift);
    /* B1: the resolved image of the action 3 render vs the CPU average */
    const bool f16 = fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    unsigned worst = 0; double worstf = 0;
    for (unsigned p = 0; p < W * H; p++) {
        unsigned c = 0; for (unsigned s = 0; s < N; s++) c += cov[p][s];
        if (f16) {
            const uint16_t *h = (const uint16_t *)(o.res + p * 8);
            const double want = (double)c / N, got = half_to_float(h[0]);
            if (fabs(got - want) > worstf) worstf = fabs(got - want);
            CHECK(fabs(got - want) <= 1e-3 && half_to_float(h[1]) == 0.0f && half_to_float(h[2]) == 0.0f && fabs((double)half_to_float(h[3]) - want) <= 1e-3, "%s %ux pixel %u resolved (%g, %g) want %g", fn, N, p, got, (double)half_to_float(h[3]), want);
        } else {
            const uint8_t *b = o.res + p * 4;   /* B G R A */
            const int want = (int)lround(255.0 * c / N);
            const unsigned dr = (unsigned)abs((int)b[2] - want), da = (unsigned)abs((int)b[3] - want);
            if (dr > worst) worst = dr;
            CHECK(dr <= 1 && da <= 1 && b[0] == 0 && b[1] == 0, "%s %ux pixel %u resolved r=%u a=%u want %d", fn, N, p, b[2], b[3], want);
        }
    }
    const unsigned full_cnt = n_cov;
    /* B2: store action 2 (resolve only, MS content not kept): same resolved image */
    static out_t o2;
    CHECK(run_case(vk, be, fmt, N, M_ACTION2, 0, TRI, &o2, NULL) == 0, "%s %ux action 2 render", fn, N);
    CHECK(memcmp(o.res, o2.res, W * H * (f16 ? 8u : 4u)) == 0, "%s %ux: store action 2 and 3 resolve to the same image", fn, N);
    /* B3: Store, then vkCmdResolveImage after the pass: needs TRANSFER_SRC on the MS image -> asserted in the dedicated part below */
    printf("PASS %s %ux: per-sample fetch == CPU reference (%u samples covered of %u), resolved image within tolerance (worst %s %.4g), action 2 == action 3, shifted-reference control differs in %u samples\n",
           fn, N, full_cnt, W * H * N, f16 ? "abs" : "UNORM steps", f16 ? worstf : (double)worst, bad_shift);
    return 0;
}

/* vkCmdResolveImage after a plain Store: the MS image needs TRANSFER_SRC (usage the guest plan does NOT offer: offered here only to prove the outside-the-pass path) */
static int check_resolve_cmd(struct lagfx_vk_state *vk, VkFormat fmt, unsigned N) {
    VkDevice dev = vk->device;
    lagfx_fmtcap_backend_t st; const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_device_backend(vk->phys_device, &st);
    lagfx_msaa_plan_t pl;
    if (lagfx_msaa_plan(be, fmt, W, H, N, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 0, &pl) != LAGFX_MSAA_OK || pl.samples != N) { printf("SKIP vkCmdResolveImage %ux: the device refuses TRANSFER_SRC on a multisampled colour image\n", N); return 0; }
    static out_t viaact, viacmd;
    CHECK(run_case(vk, be, fmt, N, M_ACTION2, 0, TRI, &viaact, NULL) == 0, "reference render");
    /* build the vkCmdResolveImage variant by hand */
    VkImage ms, rs; VkDeviceMemory msm, rsm; VkImageView msv, rsv;
    CHECK(lagfx_msaa_image_create(vk, fmt, W, H, pl.samples, pl.usage, VK_IMAGE_ASPECT_COLOR_BIT, &ms, &msm, &msv), "ms");
    CHECK(lagfx_msaa_image_create(vk, fmt, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &rs, &rsm, &rsv), "rs");
    VkShaderModule vs, fs; VkShaderModuleCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
    CHECK(vkCreateShaderModule(dev, &sci, NULL, &vs) == VK_SUCCESS, "vs"); sci.codeSize = sizeof k_fs_spv, sci.pCode = k_fs_spv; CHECK(vkCreateShaderModule(dev, &sci, NULL, &fs) == VK_SUCCESS, "fs");
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = (uint32_t)fmt, d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    d.blend_set = 1, d.color_write_mask = 0xf, d.blend_src_rgb = d.blend_src_a = VK_BLEND_FACTOR_ONE, d.blend_dst_rgb = d.blend_dst_a = VK_BLEND_FACTOR_ZERO, d.raster_samples = (uint8_t)N;
    VkPipeline pipe; CHECK(lagfx_pipeline_build(dev, &d, &pipe) == LAGFX_OK, "pipe");
    VkBuffer vb, rb; VkDeviceMemory vm, rm; void *vmap, *rmap;
    if (hostbuf(vk, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vm, &vmap) || hostbuf(vk, W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rm, &rmap)) return 1;
    memcpy(vmap, TRI, sizeof TRI);
    VkCommandBuffer cb; CHECK(lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK, "cb");
    VkCommandBufferBeginInfo bg = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bg);
    img_barrier(cb, ms, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkClearColorValue clr = { .float32 = { 0, 0, 0, 0 } };
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = msv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = clr } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cb, &ri); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } }; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off); vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRendering(cb);
    img_barrier(cb, ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    img_barrier(cb, rs, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(lagfx_msaa_cmd_resolve_image(cb, fmt, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, W, H), "record resolve");
    img_barrier(cb, rs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, rs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &cp);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; vkCreateFence(dev, &fi, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(lagfx_vk_submit_checked(vk, &si, fence) == LAGFX_OK && lagfx_vk_wait_bounded(vk, fence, 0) == LAGFX_VK_WAIT_OK, "submit");
    const bool f16 = fmt == VK_FORMAT_R16G16B16A16_SFLOAT; const unsigned bpp = f16 ? 8u : 4u;
    memcpy(viacmd.res, rmap, W * H * bpp);
    unsigned worst = 0;
    for (unsigned i = 0; i < W * H; i++) {
        if (f16) { const uint16_t *a = (const uint16_t *)(viaact.res + i * 8), *b = (const uint16_t *)(viacmd.res + i * 8); CHECK(fabsf(half_to_float(a[0]) - half_to_float(b[0])) <= 1e-3f && fabsf(half_to_float(a[3]) - half_to_float(b[3])) <= 1e-3f, "pixel %u differs between the in-pass resolve and vkCmdResolveImage", i); }
        else { const uint8_t *a = viaact.res + i * 4, *b = viacmd.res + i * 4; for (int c = 0; c < 4; c++) { unsigned dd = (unsigned)abs((int)a[c] - (int)b[c]); if (dd > worst) worst = dd; CHECK(dd <= 1, "pixel %u channel %d: in-pass resolve %u vs vkCmdResolveImage %u", i, c, a[c], b[c]); } }
    }
    printf("PASS %s %ux: vkCmdResolveImage after a plain Store == the in-pass resolve attachment (worst difference %u step)\n", f16 ? "RGBA16F" : "BGRA8", N, worst);
    vkDestroyFence(dev, fence, NULL); lagfx_vk_cmdbuf_free(vk, cb); vkDestroyBuffer(dev, vb, NULL); vkFreeMemory(dev, vm, NULL); vkDestroyBuffer(dev, rb, NULL); vkFreeMemory(dev, rm, NULL);
    vkDestroyPipeline(dev, pipe, NULL); vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyImageView(dev, msv, NULL); vkDestroyImage(dev, ms, NULL); vkFreeMemory(dev, msm, NULL); vkDestroyImageView(dev, rsv, NULL); vkDestroyImage(dev, rs, NULL); vkFreeMemory(dev, rsm, NULL);
    return 0;
}

/* injected capability answers: the plan's downgrade is explicit (clamped / fallback_1x), never silent */
typedef struct { VkFormatFeatureFlags feat; VkSampleCountFlags samples; } model_t;
static void m_fp(void *c, VkFormat f, VkFormatProperties2 *o) { (void)f; model_t *m = c; memset(&o->formatProperties, 0, sizeof o->formatProperties); o->formatProperties.optimalTilingFeatures = m->feat; }
static VkResult m_ifp(void *c, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *o) {
    model_t *m = c; (void)in;
    memset(&o->imageFormatProperties, 0, sizeof o->imageFormatProperties);
    o->imageFormatProperties.maxExtent = (VkExtent3D){ 16384, 16384, 1 }, o->imageFormatProperties.maxMipLevels = 15, o->imageFormatProperties.maxArrayLayers = 256, o->imageFormatProperties.sampleCounts = m->samples;
    return VK_SUCCESS;
}
static int check_plan_injected(void) {
    const VkFormatFeatureFlags f = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    model_t all = { f, 1 | 2 | 4 | 8 }, no4 = { f, 1 | 2 }, only1 = { f, 1 }, noatt = { f & ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 1 | 2 | 4 }, no8 = { f, 1 | 2 | 4 };
    lagfx_fmtcap_backend_t ba = { &all, m_fp, m_ifp }, b4 = { &no4, m_fp, m_ifp }, b1 = { &only1, m_fp, m_ifp }, bn = { &noatt, m_fp, m_ifp }, b8 = { &no8, m_fp, m_ifp };
    lagfx_msaa_plan_t p;
    const VkImageUsageFlags ms = VK_IMAGE_USAGE_SAMPLED_BIT, x1 = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormat fmts[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
    for (int i = 0; i < 2; i++) {
        const VkFormat F = fmts[i];
        CHECK(lagfx_msaa_plan(&ba, F, W, H, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 4 && !p.clamped && !p.fallback_1x && (p.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "4x granted");
        CHECK(lagfx_msaa_plan(&ba, F, W, H, 2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 2 && !p.clamped, "2x granted");
        CHECK(lagfx_msaa_plan(&ba, F, W, H, 8, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 8 && !p.clamped, "8x granted when reported");
        CHECK(lagfx_msaa_plan(&b8, F, W, H, 8, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 4 && p.clamped && !p.fallback_1x, "8x asked, 4x max: clamped to 4");
        CHECK(lagfx_msaa_plan(&b4, F, W, H, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 2 && p.clamped && !p.fallback_1x && p.requested == 4, "no 4x: clamped to 2");
        CHECK(lagfx_msaa_plan(&b1, F, W, H, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 1 && p.fallback_1x && !p.clamped && (p.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT), "1-only: explicit fallback_1x, with the transfer usages of the 1x set");
        CHECK(lagfx_msaa_plan(&b1, F, W, H, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 1 && !p.fallback_1x && !p.clamped, "1 asked, 1 granted: neither flag");
        CHECK(lagfx_msaa_plan(&b4, F, W, H, 3, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 2 && p.clamped, "3 asked: largest power of two below it");
        CHECK(lagfx_msaa_plan(&bn, F, W, H, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_NO_FORMAT, "no colour attachment feature: no image at all");
        CHECK(lagfx_msaa_plan(&ba, F, 32768, H, 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms, x1, &p) == LAGFX_MSAA_NO_FORMAT, "extent beyond maxExtent: refused, not silently shrunk");
    }
    puts("PASS plan with injected devices (BGRA8 + RGBA16F): 8 -> 4 clamped, no 4x -> 2 clamped, 1-only -> explicit 1x fallback with transfer usages, no attachment feature / oversized -> refused");
    return 0;
}

/* EXACT policy of the planner (lagfx_msaa_plan_mode): the requested count or a refusal, never a clamp / 1x fallback. 2x is only checked against INJECTED devices (lavapipe has 1|4). */
static int check_plan_exact_injected(void) {
    const VkFormatFeatureFlags f = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    model_t all = { f, 1 | 2 | 4 | 8 }, no4 = { f, 1 | 2 }, only1 = { f, 1 }, one4 = { f, 1 | 4 }, noatt = { f & ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 1 | 2 | 4 };
    lagfx_fmtcap_backend_t ba = { &all, m_fp, m_ifp }, b4 = { &no4, m_fp, m_ifp }, b1 = { &only1, m_fp, m_ifp }, b14 = { &one4, m_fp, m_ifp }, bn = { &noatt, m_fp, m_ifp };
    lagfx_msaa_plan_t p;
    const VkImageUsageFlags C = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ms = VK_IMAGE_USAGE_SAMPLED_BIT, x1 = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const lagfx_msaa_mode_t E = LAGFX_MSAA_MODE_EXACT, K = LAGFX_MSAA_MODE_COMPAT;
    const VkFormat fmts[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
    for (int i = 0; i < 2; i++) {
        const VkFormat F = fmts[i];
        CHECK(lagfx_msaa_plan_mode(&ba, F, W, H, 4, C, ms, x1, E, &p) == LAGFX_MSAA_OK && p.samples == 4 && !p.clamped && !p.fallback_1x, "exact 4x on a 1|2|4|8 device");
        CHECK(lagfx_msaa_plan_mode(&ba, F, W, H, 2, C, ms, x1, E, &p) == LAGFX_MSAA_OK && p.samples == 2 && !p.clamped && !p.fallback_1x, "exact 2x on a device that reports 2 (injected)");
        CHECK(lagfx_msaa_plan_mode(&ba, F, W, H, 8, C, ms, x1, E, &p) == LAGFX_MSAA_OK && p.samples == 8, "exact 8x on a device that reports 8");
        CHECK(lagfx_msaa_plan_mode(&b14, F, W, H, 2, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED && p.samples == 0, "exact 2x on a 1|4 device (lavapipe-like): refused, not 4x, not 1x");
        CHECK(lagfx_msaa_plan_mode(&b14, F, W, H, 8, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 8x on a 1|4 device: refused, not clamped to 4");
        CHECK(lagfx_msaa_plan_mode(&b4, F, W, H, 4, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 4x on a 1|2 device: refused, not clamped to 2");
        CHECK(lagfx_msaa_plan_mode(&b1, F, W, H, 4, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 4x on a 1-only device: refused, no 1x fallback");
        CHECK(lagfx_msaa_plan_mode(&ba, F, W, H, 3, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 3 samples: not a power of two, refused (compat would take 2)");
        CHECK(lagfx_msaa_plan_mode(&ba, F, W, H, 32, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 32 samples: refused");
        CHECK(lagfx_msaa_plan_mode(&b1, F, W, H, 1, C, ms, x1, E, &p) == LAGFX_MSAA_OK && p.samples == 1 && !p.fallback_1x && !p.clamped, "exact 1 sample is a plain image");
        CHECK(lagfx_msaa_plan_mode(&b1, F, W, H, 0, C, ms, x1, E, &p) == LAGFX_MSAA_OK && p.samples == 1, "exact 0 treated as 1");
        CHECK(lagfx_msaa_plan_mode(&bn, F, W, H, 4, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 4x without the attachment feature: refused");
        CHECK(lagfx_msaa_plan_mode(&ba, F, 32768, H, 4, C, ms, x1, E, &p) == LAGFX_MSAA_EXACT_REFUSED, "exact 4x beyond maxExtent: refused, not shrunk");
        /* the same inputs in compat mode keep the old downgrade behaviour (and lagfx_msaa_plan IS the compat planner) */
        CHECK(lagfx_msaa_plan_mode(&b14, F, W, H, 2, C, ms, x1, K, &p) == LAGFX_MSAA_OK && p.samples == 1 && p.fallback_1x, "compat 2x on 1|4: 1x fallback (counted by the caller)");
        CHECK(lagfx_msaa_plan_mode(&b14, F, W, H, 8, C, ms, x1, K, &p) == LAGFX_MSAA_OK && p.samples == 4 && p.clamped, "compat 8x on 1|4: clamped to 4");
        CHECK(lagfx_msaa_plan(&b14, F, W, H, 8, C, ms, x1, &p) == LAGFX_MSAA_OK && p.samples == 4 && p.clamped, "lagfx_msaa_plan is the compat planner");
    }
    puts("PASS exact planner (injected devices, BGRA8 + RGBA16F): requested count or EXACT_REFUSED (2x on 1|4, 8x on 1|4, 4x on 1|2, 1-only, 3 / 32 samples, no attachment feature, oversized); compat keeps clamp / 1x fallback");
    return 0;
}

int main(void) {
    int rc = check_plan_injected();
    if (rc) return rc;
    rc = check_plan_exact_injected();
    if (rc) return rc;
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { puts("no usable Vulkan device: skip"); return 77; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(vk->phys_device, &pr);
    printf("device: %s  standardSampleLocations=%u framebufferColorSampleCounts=0x%x sampledImageColorSampleCounts=0x%x\n", pr.deviceName, (unsigned)pr.limits.standardSampleLocations,
           (unsigned)pr.limits.framebufferColorSampleCounts, (unsigned)pr.limits.sampledImageColorSampleCounts);
    if (!pr.limits.standardSampleLocations) { puts("SKIP: the device does not use the standard sample locations; the exact CPU reference does not apply"); return 77; }
    lagfx_fmtcap_backend_t st; const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_device_backend(vk->phys_device, &st);
    const VkFormat fmts[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
    uint64_t v0 = lagfx_vk_validation_msg_count;
    for (int i = 0; i < 2; i++) {
        VkSampleCountFlags sc = lagfx_fmtcap_sample_counts(be, fmts[i], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        printf("format %d colour+sampled sample counts: 0x%x\n", (int)fmts[i], (unsigned)sc);
        for (unsigned n = 2; n <= 4; n *= 2) {
            if (!(sc & n)) { printf("SKIP %s %ux: the device does not report it (lavapipe: 1 and 4)\n", i ? "RGBA16F" : "BGRA8", n); continue; }
            if ((rc = check_case(vk, be, fmts[i], n))) return rc;
            if ((rc = check_resolve_cmd(vk, fmts[i], n))) return rc;
        }
    }
    /* 1x control: no sample is partly covered (only 0 / 255 in the image) */
    {
        static out_t o1; CHECK(run_case(vk, be, VK_FORMAT_B8G8R8A8_UNORM, 1, M_ACTION2, 0, TRI, &o1, NULL) == 0, "1x render");
        unsigned part = 0, full = 0;
        for (unsigned p = 0; p < W * H; p++) { const uint8_t a = o1.res[p * 4 + 3]; if (a == 255) full++; else if (a) part++; }
        CHECK(part == 0 && full > 40, "1x control: partly covered pixels %u (want 0), fully covered %u", part, full);
        puts("PASS 1x control: only 0 / 255 coverage (no partial pixel without MSAA)");
    }
    if (lagfx_vk_validation_msg_count != v0) { printf("FAIL: %llu validation message(s) during the positive cases\n", (unsigned long long)(lagfx_vk_validation_msg_count - v0)); return 1; }
    puts("PASS: no validation message in any positive case");
    /* negative control: a 1-sample pipeline on a 4-sample attachment is reported by the validation layer */
    if ((lagfx_fmtcap_sample_counts(be, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) & 4) && getenv("LAGFX_VK_VALIDATION") && vk->debug_messenger) {
        static out_t on; uint64_t b = lagfx_vk_validation_msg_count;
        CHECK(run_case(vk, be, VK_FORMAT_B8G8R8A8_UNORM, 4, M_ACTION2, 1, TRI, &on, NULL) == 0, "mismatched render");
        CHECK(lagfx_vk_validation_msg_count > b, "a 1-sample pipeline on a 4-sample attachment must be flagged by the validation layer");
        puts("PASS: negative control: pipeline sample count != attachment sample count is flagged by the validation layer");
    } else puts("SKIP: validation negative control (needs LAGFX_VK_VALIDATION=1 and 4x)");
    return 0;
}
