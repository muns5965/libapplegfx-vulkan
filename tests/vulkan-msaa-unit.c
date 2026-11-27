/*
 * libapplegfx-vulkan — Vulkan multisample colour path (src/vulkan/msaa.{h,c}); lavapipe, NOT a prediction for Adreno.
 * tests/vulkan-msaa-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * (A) pure: sample-count choice, Metal store action table, resolve mode by format class.
 * (B) real device: a red triangle with a diagonal edge is drawn into a 1x / 2x / 4x target through lagfx_pipeline_build (desc.raster_samples) and the attachment info of
 *     lagfx_msaa_color_attachment, resolved by the store action, and read back. Checks: interior is exactly red; at 2x / 4x the edge pixels hold coverage in steps of 1/N
 *     (a value that cannot occur at 1x), at 1x only 0 or 255; store action 2 (resolve, DontCare) and 3 (store + resolve) both resolve; store action 1 without a resolve target
 *     leaves the resolve image untouched (the "not resolved" negative control); a pipeline whose sample count differs from the attachments' is rejected by the validation layer
 *     (negative control for raster_samples being really wired).
 * (C) injected devices (also the device-info MSAASamples clamp helper): no 4x / no 2x -> clamped (counted by the target flag) to the largest supported; no colour attachment -> NO_FORMAT.
 * Skips (exit 77) when no Vulkan device is usable. The pipeline-sample negative control needs LAGFX_VK_VALIDATION=1 and is skipped otherwise.
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/msaa.h"
#include "vulkan/pipeline_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
extern uint64_t lagfx_vk_validation_msg_count;

#include "arm64-exec-run-shaders.h"

static int part_a(void) {
    bool c;
    const VkSampleCountFlags all = 1 | 2 | 4 | 8;
    CHECK(lagfx_msaa_choose(all, 4, &c) == 4 && !c, "4 of 1|2|4|8");
    CHECK(lagfx_msaa_choose(1 | 2, 4, &c) == 2 && c, "4 on a device with 1|2 -> 2, clamped");
    CHECK(lagfx_msaa_choose(1, 4, &c) == 1 && c, "4 on a 1-only device -> 1, clamped");
    CHECK(lagfx_msaa_choose(1 | 4, 2, &c) == 1 && c, "2 requested, only 1|4 -> 1 (never rounds UP)");
    CHECK(lagfx_msaa_choose(all, 3, &c) == 2 && c, "3 -> 2 (largest power of two <= 3)");
    CHECK(lagfx_msaa_choose(all, 0, &c) == 1 && !c, "0 -> 1");
    CHECK(lagfx_msaa_choose(0, 1, &c) == 1 && !c, "nothing reported, 1 requested -> 1");
    VkAttachmentStoreOp so; bool rs;
    CHECK(lagfx_msaa_store_map(0, false, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_DONT_CARE && !rs, "DontCare");
    CHECK(lagfx_msaa_store_map(1, false, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_STORE && !rs, "Store");
    CHECK(lagfx_msaa_store_map(2, true, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_DONT_CARE && rs, "MultisampleResolve");
    CHECK(lagfx_msaa_store_map(3, true, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_STORE && rs, "StoreAndMultisampleResolve");
    CHECK(!lagfx_msaa_store_map(2, false, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_STORE && !rs, "resolve without a target: reported + safe default");
    CHECK(!lagfx_msaa_store_map(4, true, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_STORE && !rs, "Unknown(4): reported + safe default");
    CHECK(!lagfx_msaa_store_map(5, true, &so, &rs) && !rs, "CustomSampleDepthStore(5): reported");
    CHECK(!lagfx_msaa_store_map(77, true, &so, &rs) && !rs, "garbage: reported");
    CHECK(lagfx_msaa_resolve_mode(VK_FORMAT_B8G8R8A8_UNORM) == VK_RESOLVE_MODE_AVERAGE_BIT && lagfx_msaa_resolve_mode(VK_FORMAT_R16G16B16A16_SFLOAT) == VK_RESOLVE_MODE_AVERAGE_BIT, "float classes average");
    CHECK(lagfx_msaa_resolve_mode(VK_FORMAT_R32_UINT) == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT && lagfx_msaa_resolve_mode(VK_FORMAT_R8G8B8A8_SINT) == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, "integer classes sample zero");
    puts("PASS (A): sample-count choice, store-action table, resolve mode");
    return 0;
}

#define W 16u
#define H 16u
typedef struct { uint32_t px[W * H]; } img_t;

/* Render the triangle into `t` with a pipeline of `pipe_samples` (0 = t->samples) and the Metal store action; read back the resolve image (or the single-sample target). */
static int render(struct lagfx_vk_state *vk, lagfx_msaa_target_t *t, uint32_t pipe_samples, uint32_t store_action, uint32_t resolve_init, img_t *out, bool *resolved_known) {
    VkDevice dev = vk->device;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &vs) == VK_SUCCESS, "vs");
    ci.codeSize = sizeof k_fs_spv, ci.pCode = k_fs_spv;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &fs) == VK_SUCCESS, "fs");
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = (uint32_t)t->fmt;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    d.blend_set = 1, d.color_write_mask = 0xf, d.blend_src_rgb = d.blend_src_a = VK_BLEND_FACTOR_ONE, d.blend_dst_rgb = d.blend_dst_a = VK_BLEND_FACTOR_ZERO;
    d.raster_samples = (uint8_t)(pipe_samples ? pipe_samples : (uint32_t)t->samples);
    VkPipeline pipe;
    CHECK(lagfx_pipeline_build(dev, &d, &pipe) == LAGFX_OK, "pipeline build samples=%u", d.raster_samples);
    /* vertex buffer: a triangle with a slanted edge across the pixel grid */
    const float v[6] = { -1.0f, -1.0f, 1.0f, -0.35f, -0.35f, 1.0f };
    VkBuffer vb, rb; VkDeviceMemory vm, rm;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof v, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bi, NULL, &vb) == VK_SUCCESS, "vb");
    bi.size = W * H * 4, bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateBuffer(dev, &bi, NULL, &rb) == VK_SUCCESS, "rb");
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
    VkBuffer bufs[2] = { vb, rb }; VkDeviceMemory *mems[2] = { &vm, &rm };
    for (int i = 0; i < 2; i++) {
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, bufs[i], &mr);
        uint32_t mt = UINT32_MAX;
        for (uint32_t k = 0; k < mp.memoryTypeCount; k++) if ((mr.memoryTypeBits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = k; break; }
        CHECK(mt != UINT32_MAX, "host memory");
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
        CHECK(vkAllocateMemory(dev, &ai, NULL, mems[i]) == VK_SUCCESS && vkBindBufferMemory(dev, bufs[i], *mems[i], 0) == VK_SUCCESS, "mem");
    }
    void *p; vkMapMemory(dev, vm, 0, sizeof v, 0, &p); memcpy(p, v, sizeof v); vkUnmapMemory(dev, vm);

    VkCommandBuffer cb; CHECK(lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK, "cb");
    VkCommandBufferBeginInfo bg = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bg);
    VkImage final = t->resolve_image ? t->resolve_image : t->ms_image;
    VkImageMemoryBarrier b[2]; uint32_t nb = 0;
    VkImage imgs[2] = { t->ms_image, t->resolve_image };
    for (int i = 0; i < 2; i++) if (imgs[i]) b[nb++] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = i == 1 ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = imgs[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, nb, b);
    if (t->resolve_image) {   /* a known content in the resolve image: lets "not resolved" be told from "resolved to zero" */
        VkClearColorValue cc = { .uint32 = { resolve_init, resolve_init, resolve_init, resolve_init } };
        VkClearColorValue cf = { .float32 = { resolve_init / 255.f, resolve_init / 255.f, resolve_init / 255.f, resolve_init / 255.f } };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        (void)cc; vkCmdClearColorImage(cb, t->resolve_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cf, 1, &rg);
        VkImageMemoryBarrier rb2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t->resolve_image, .subresourceRange = rg };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &rb2);
    }
    VkClearColorValue clr = { .float32 = { 0, 0, 0, 0 } };
    VkRenderingAttachmentInfo ca;
    const bool known = lagfx_msaa_color_attachment(t, VK_ATTACHMENT_LOAD_OP_CLEAR, &clr, store_action, &ca);
    if (resolved_known) *resolved_known = known;
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    VkImageMemoryBarrier tb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = final, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, final, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &cp);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; vkCreateFence(dev, &fi, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(lagfx_vk_submit_checked(vk, &si, fence) == LAGFX_OK && lagfx_vk_wait_bounded(vk, fence, 0) == LAGFX_VK_WAIT_OK, "submit");
    vkMapMemory(dev, rm, 0, W * H * 4, 0, &p); memcpy(out->px, p, W * H * 4); vkUnmapMemory(dev, rm);
    vkDestroyFence(dev, fence, NULL); lagfx_vk_cmdbuf_free(vk, cb);
    vkDestroyBuffer(dev, vb, NULL); vkDestroyBuffer(dev, rb, NULL); vkFreeMemory(dev, vm, NULL); vkFreeMemory(dev, rm, NULL);
    vkDestroyPipeline(dev, pipe, NULL); vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    return 0;
}

/* B8G8R8A8 pixel: alpha = byte 3 */
static unsigned ncoverage(const img_t *im, unsigned *interior, unsigned *edge, unsigned *bad_step, unsigned steps) {
    *interior = *edge = *bad_step = 0;
    for (unsigned i = 0; i < W * H; i++) {
        unsigned a = im->px[i] >> 24;
        if (a == 255) (*interior)++;
        else if (a != 0) {
            (*edge)++;
            /* coverage c/steps of alpha 255 with a UNORM average: must sit within 1 of one of the k * 255 / steps values */
            bool ok = false;
            for (unsigned k = 1; k < steps; k++) { int ex = (int)((k * 255u + steps / 2) / steps); if ((int)a >= ex - 1 && (int)a <= ex + 1) ok = true; }
            if (!ok) (*bad_step)++;
        }
    }
    return *edge;
}

static int part_b(struct lagfx_vk_state *vk) {
    lagfx_fmtcap_backend_t st; const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_device_backend(vk->phys_device, &st);
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkSampleCountFlags sc = lagfx_fmtcap_sample_counts(be, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    printf("device colour sample counts for B8G8R8A8: 0x%x\n", (unsigned)sc);
    /* 1x reference: only 0 or 255 coverage */
    lagfx_msaa_target_t t1; CHECK(lagfx_msaa_target_create(vk, be, fmt, W, H, 1, true, &t1) == LAGFX_MSAA_OK && t1.samples == 1 && !t1.resolve_image && !t1.clamped, "1x target");
    static img_t i1, i2, i4, ineg, inr;
    bool known;
    if (render(vk, &t1, 0, 1, 0, &i1, &known)) return 1;
    unsigned in1, ed1, bs1; ncoverage(&i1, &in1, &ed1, &bs1, 2);
    CHECK(in1 > 40 && ed1 == 0, "1x: interior %u edge %u (an anti-aliased pixel at 1 sample)", in1, ed1);
    lagfx_msaa_target_destroy(vk, &t1);
    for (unsigned n = 2; n <= 4; n *= 2) {
        if (!(sc & n)) { printf("SKIP %ux: device does not report it\n", n); continue; }
        lagfx_msaa_target_t t; CHECK(lagfx_msaa_target_create(vk, be, fmt, W, H, n, true, &t) == LAGFX_MSAA_OK && t.samples == n && t.resolve_image && !t.clamped, "%ux target", n);
        img_t *im = n == 2 ? &i2 : &i4;
        for (int act = 2; act <= 3; act++) {
            CHECK(render(vk, &t, 0, (uint32_t)act, 77, im, &known) == 0 && known, "%ux render store action %d", n, act);
            unsigned in, ed, bs; ncoverage(im, &in, &ed, &bs, n);
            CHECK(in > 40 && ed >= 4 && bs == 0, "%ux store action %d: interior %u edge pixels %u off-step %u", n, act, in, ed, bs);
            /* the interior is exactly the triangle colour (opaque red: BGRA 0xffff0000) */
            unsigned red = 0; for (unsigned i = 0; i < W * H; i++) if (im->px[i] == 0xffff0000u) red++;
            CHECK(red == in, "%ux interior pixels are red (%u of %u)", n, red, in);
            /* all-uncovered pixels were CLEARED by the multisampled attachment's loadOp and resolved to 0, not left at the resolve image's initial 77 */
            CHECK(im->px[0] == 0u || im->px[W * H - 1] == 0u, "%ux a far corner resolved to the clear colour", n);
        }
        /* negative control: store action 1 (Store, no resolve) with a resolve image present: the attachment info carries no resolve, so the image keeps its initial 77 everywhere */
        CHECK(render(vk, &t, 0, 1, 77, &ineg, &known) == 0 && known, "store-only render");
        bool all77 = true; for (unsigned i = 0; i < W * H; i++) if (ineg.px[i] != 0x4d4d4d4du) all77 = false;
        CHECK(all77, "%ux Store without resolve left the resolve target untouched", n);
        /* unknown store action: reported, safe default (stored, not resolved) */
        CHECK(render(vk, &t, 0, 4, 77, &inr, &known) == 0 && !known, "unknown store action reported");
        lagfx_msaa_target_destroy(vk, &t);
        printf("PASS (B) %ux: edge pixels at coverage steps of 1/%u, interior red, store actions 2 and 3 resolve, Store alone does not, Unknown reported\n", n, n);
    }
    /* edge pixel exists only with multisampling: positive vs the 1x control above */
    CHECK(ed1 == 0, "control");
    /* sample-count mismatch negative control (validation layer needed) */
    if ((sc & 4) && getenv("LAGFX_VK_VALIDATION") && vk->debug_messenger) {
        lagfx_msaa_target_t t; CHECK(lagfx_msaa_target_create(vk, be, fmt, W, H, 4, true, &t) == LAGFX_MSAA_OK, "4x target");
        uint64_t v0 = lagfx_vk_validation_msg_count;
        if (render(vk, &t, 4, 3, 0, &i4, &known)) return 1;
        CHECK(lagfx_vk_validation_msg_count == v0, "matching pipeline and attachment samples: no validation message (%llu)", (unsigned long long)(lagfx_vk_validation_msg_count - v0));
        if (render(vk, &t, 1, 3, 0, &inr, &known)) return 1;   /* 1-sample pipeline into a 4-sample attachment */
        CHECK(lagfx_vk_validation_msg_count > v0, "a 1-sample pipeline on a 4-sample attachment must be reported by the validation layer");
        lagfx_msaa_target_destroy(vk, &t);
        puts("PASS (B): raster_samples is really wired: matching count validates clean, mismatching count is flagged by the validation layer");
    } else puts("SKIP: pipeline sample-count mismatch control (needs LAGFX_VK_VALIDATION=1 and a sample count 4 device)");
    return 0;
}

/* ---- injected devices ---- */
typedef struct { VkFormatFeatureFlags feat; VkSampleCountFlags samples; } model_t;
static void m_fp(void *c, VkFormat f, VkFormatProperties2 *o) { (void)f; model_t *m = c; memset(&o->formatProperties, 0, sizeof o->formatProperties); o->formatProperties.optimalTilingFeatures = m->feat; }
static VkResult m_ifp(void *c, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *o) {
    model_t *m = c; (void)in;
    memset(&o->imageFormatProperties, 0, sizeof o->imageFormatProperties);
    o->imageFormatProperties.maxExtent = (VkExtent3D){ 16384, 16384, 1 }, o->imageFormatProperties.maxMipLevels = 15, o->imageFormatProperties.maxArrayLayers = 256, o->imageFormatProperties.sampleCounts = m->samples;
    return VK_SUCCESS;
}
static int part_c(struct lagfx_vk_state *vk) {
    const VkFormatFeatureFlags f = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    lagfx_msaa_target_t t;
    model_t full = { f, 1 | 2 | 4 | 8 }, no4 = { f, 1 | 2 }, only1 = { f, 1 }, noatt = { f & ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 1 | 2 | 4 };
    lagfx_fmtcap_backend_t bf = { &full, m_fp, m_ifp }, b4 = { &no4, m_fp, m_ifp }, b1 = { &only1, m_fp, m_ifp }, bn = { &noatt, m_fp, m_ifp };
    /* the images are then really created on the REAL device; only the capability answer is injected, so the granted count must also be creatable there: lavapipe has 1|4 */
    VkSampleCountFlags real = lagfx_fmtcap_sample_counts(lagfx_fmtcap_device_backend(vk->phys_device, &(lagfx_fmtcap_backend_t){0}), VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (real & 4) {
        CHECK(lagfx_msaa_target_create(vk, &bf, VK_FORMAT_B8G8R8A8_UNORM, W, H, 4, true, &t) == LAGFX_MSAA_OK && t.samples == 4 && !t.clamped, "full device: 4x granted");
        lagfx_msaa_target_destroy(vk, &t);
    }
    if (real & 2) {
        CHECK(lagfx_msaa_target_create(vk, &b4, VK_FORMAT_B8G8R8A8_UNORM, W, H, 4, true, &t) == LAGFX_MSAA_OK && t.samples == 2 && t.clamped && t.requested == 4 && t.resolve_image, "no 4x: clamped to 2");
        lagfx_msaa_target_destroy(vk, &t);
    } else puts("SKIP: injected no-4x case creates a real 2x image, which this device does not offer (the choice itself is covered by part A)");
    CHECK(lagfx_msaa_target_create(vk, &b1, VK_FORMAT_B8G8R8A8_UNORM, W, H, 4, true, &t) == LAGFX_MSAA_OK && t.samples == 1 && t.clamped && !t.resolve_image && t.ms_image, "1-only device: single-sample image, clamped, no resolve image");
    lagfx_msaa_target_destroy(vk, &t);
    CHECK(lagfx_msaa_target_create(vk, &bn, VK_FORMAT_B8G8R8A8_UNORM, W, H, 4, true, &t) == LAGFX_MSAA_NO_FORMAT && !t.ms_image, "no colour attachment feature: NO_FORMAT, nothing created");
    { bool cl = false;
      CHECK(lagfx_msaa_devinfo_max(&bf, 4, &cl) == 4 && !cl, "devinfo: capable host keeps 4");
      CHECK(lagfx_msaa_devinfo_max(&b4, 4, &cl) == 2 && cl, "devinfo: host without 4x lowers to 2, counted");
      cl = false; CHECK(lagfx_msaa_devinfo_max(&b1, 4, &cl) == 1 && cl, "devinfo: 1-only host lowers to 1 (the supportsRasterSampleCount:1 gate stays open)");
      cl = false; CHECK(lagfx_msaa_devinfo_max(&bn, 4, &cl) == 1 && cl, "devinfo: a host with no colour-attachment feature still advertises 1, never 0");
      cl = false; CHECK(lagfx_msaa_devinfo_max(&b1, 1, &cl) == 1 && !cl, "devinfo: an advertised 1 is not changed");
      model_t mixed = { f, 1 | 2 | 4 }; lagfx_fmtcap_backend_t bm = { &mixed, m_fp, m_ifp }; cl = false;
      CHECK(lagfx_msaa_devinfo_max(&bm, 4, &cl) == 4 && !cl, "devinfo: 1|2|4 keeps 4"); }
    puts("PASS (C): injected devices: no 4x -> 2x clamped, 1-only -> single sample clamped without resolve image, no colour attachment -> NO_FORMAT");
    return 0;
}

int main(void) {
    int rc = part_a();
    if (rc) return rc;
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { puts("no usable Vulkan device: skip (B, C)"); return 77; }
    if ((rc = part_b(vk))) return rc;
    return part_c(vk);
}
