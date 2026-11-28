/* SPDX-License-Identifier: MIT */
/* General render description (render_pass.c) + the converted draw path, on lavapipe, once per backend (dynamic rendering, and the legacy VkRenderPass backend forced with
 * LAGFX_VK_FORCE_LEGACY_RENDERPASS=1):
 *   A. two colour attachments + a D32 depth attachment in ONE pass: each colour target keeps ITS OWN clear value (clearValue index == attachment index);
 *   B. a real triangle through lagfx_pipeline_build (render_pass from lagfx_pipeline_render_desc / lagfx_vk_render_pass_compat on the legacy backend) and
 *      lagfx_vk_draw_record_and_submit_bound: the covered pixel has the shader colour, an uncovered pixel keeps what the target held (LOAD);
 *   C. legacy backend only: vkDeviceWaitIdle failure injected into render_forget_view -> the target's view/image/memory are DEFERRED (not destroyed), VK_ERROR_DEVICE_LOST counts
 *      as idle and the target is destroyed; shutdown after a deferral is clean;
 *   D. shapes the legacy backend does not express (UNDEFINED colour slot, no_color, dyn_state) are refused by lagfx_pipeline_render_desc, not built for another pass.
 * NOT verified: a real Vulkan 1.1 driver (Adreno), depth CONTENT (the depth image is only attached and cleared), the validation layer only where the test is registered with LAGFX_VK_VALIDATION=1 (then: messenger active, positive control delivered, 0 messages before it). Exit 77 = no usable device. */
#define _GNU_SOURCE
#include "vulkan/instance.h"
#include "vulkan/render_pass.h"
#include "vulkan/render_target.h"
#include "vulkan/command.h"
#include "vulkan/draw_record.h"
#include "vulkan/pipeline_build.h"
#include "validation-control.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "arm64-exec-run-shaders.h"   /* k_vs_spv (vec2 pos at location 0), k_fs_spv (constant red) */

static int g_fail;
#include "legacy-fetch-shaders.h"
#include "dsres-shaders.h"
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); g_fail++; } } while (0)

#ifdef LAGFX_HAVE_VULKAN
static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* Record with `fn`, submit, wait; every return value is checked. */
static bool one_shot(struct lagfx_vk_state *vk, bool (*fn)(struct lagfx_vk_state *, VkCommandBuffer, void *), void *arg) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (lagfx_vk_cmdbuf_alloc(vk, &cb) != LAGFX_OK) return false;
    bool ok = false; VkFence f = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) goto out;
    if (!fn(vk, cb, arg)) { vkEndCommandBuffer(cb); goto out; }
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) goto out;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(vk->device, &fci, NULL, &f) != VK_SUCCESS) goto out;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (vkQueueSubmit(vk->graphics_queue, 1, &si, f) != VK_SUCCESS) goto out;
    ok = vkWaitForFences(vk->device, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    if (!ok) {   /* the submission is accepted and may still run: the fence and command buffer are released only once the device is idle; if that cannot be shown they are leaked */
        if (vkDeviceWaitIdle(vk->device) != VK_SUCCESS) { printf("one_shot: fence wait failed and the device is not idle: command buffer and fence leaked on purpose\n"); return false; }
    }
out:
    if (f) vkDestroyFence(vk->device, f, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);
    return ok;
}

typedef struct { lagfx_vk_render_target_t *c0, *c1; VkImage dimg; VkImageView dview; uint32_t w, h; } mrt_t;

static bool rec_mrt(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    mrt_t *m = arg;
    lagfx_vk_cmd_rt_to_color(cb, m->c0);
    lagfx_vk_cmd_rt_to_color(cb, m->c1);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = m->dimg, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = m->w; d.height = m->h; d.color_count = 2;
    const VkClearColorValue red = { .float32 = { 1, 0, 0, 1 } }, green = { .float32 = { 0, 1, 0, 1 } };
    lagfx_vk_render_target_t *c[2] = { m->c0, m->c1 };
    for (int i = 0; i < 2; i++)
        d.color[i] = (lagfx_render_attachment_t){ .view = c[i]->view, .format = c[i]->format, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                  .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = i ? green : red } };
    d.has_depth_stencil = true;
    d.depth_stencil = (lagfx_render_attachment_t){ .view = m->dview, .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clear = { .depthStencil = { 1.0f, 0 } } };
    if (lagfx_vk_render_begin_desc(vk, cb, &d) != LAGFX_OK) return false;
    lagfx_vk_render_end(vk, cb);
    return true;
}

typedef struct { lagfx_vk_render_target_t *c0, *c2; uint32_t w, h; uint32_t fetch_mask; } gap_t;
static bool rec_gap(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    gap_t *g = arg;
    lagfx_vk_cmd_rt_to_color(cb, g->c0);
    lagfx_vk_cmd_rt_to_color(cb, g->c2);
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = g->w; d.height = g->h; d.color_count = 3; d.fetch_mask = g->fetch_mask;
    const VkClearColorValue red = { .float32 = { 1, 0, 0, 1 } }, green = { .float32 = { 0, 1, 0, 1 } };
    d.color[0] = (lagfx_render_attachment_t){ .view = g->c0->view, .format = g->c0->format, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = red } };
    /* slot 1: unused (format UNDEFINED, no view) */
    d.color[2] = (lagfx_render_attachment_t){ .view = g->c2->view, .format = g->c2->format, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = green } };
    if (lagfx_vk_render_begin_desc(vk, cb, &d) != LAGFX_OK) return false;
    lagfx_vk_render_end(vk, cb);
    return true;
}

static bool px_is(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt, uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r) {
    uint8_t px[16 * 16 * 4]; size_t stride = 0;
    if (lagfx_vk_render_target_readback(vk, rt, px, sizeof px, &stride) != LAGFX_OK) return false;
    const uint8_t *p = px + y * stride + x * 4;
    return p[0] == b && p[1] == g && p[2] == r;   /* BGRA */
}

static bool rec_clear(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    void **a = arg; const float *col = a[1];
    return lagfx_vk_render_clear_color(vk, cb, a[0], col) == LAGFX_OK;
}

typedef struct { lagfx_vk_render_target_t *rt; VkPipeline pipe; VkBuffer vb; } tri_t;

static bool rec_load_pass(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    lagfx_vk_render_target_t *rt = arg;
    lagfx_vk_cmd_rt_to_color(cb, rt);
    lagfx_render_color_t c = { .view = rt->view, .format = rt->format, .width = rt->width, .height = rt->height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               .load = VK_ATTACHMENT_LOAD_OP_LOAD, .store = VK_ATTACHMENT_STORE_OP_STORE };
    if (lagfx_vk_render_begin(vk, cb, &c) != LAGFX_OK) return false;
    lagfx_vk_render_end(vk, cb);
    return true;
}


/* ---- C3: multisampled colour attachments with resolve: partial resolve, gap + resolve, resolve + depth (legacy: pResolveAttachments; dynamic: resolveImageView) ---- */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } ms_img_t;
static bool ms_img_make(struct lagfx_vk_state *vk, VkFormat fmt, VkSampleCountFlagBits sm, VkImageUsageFlags usage, VkImageAspectFlags asp, ms_img_t *o) {
    memset(o, 0, sizeof *o);
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { 16, 16, 1 }, .mipLevels = 1, .arrayLayers = 1,
                             .samples = sm, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vk->device, &ii, NULL, &o->img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk->device, o->img, &mr);
    uint32_t mt = memtype(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt == UINT32_MAX) mt = memtype(vk->phys_device, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { asp, 0, 1, 0, 1 } };
    return mt != UINT32_MAX && vkAllocateMemory(vk->device, &ai, NULL, &o->mem) == VK_SUCCESS && vkBindImageMemory(vk->device, o->img, o->mem, 0) == VK_SUCCESS
        && vkCreateImageView(vk->device, &vi, NULL, &o->view) == VK_SUCCESS;
}
static void ms_img_free(struct lagfx_vk_state *vk, ms_img_t *o) {
    if (o->view) vkDestroyImageView(vk->device, o->view, NULL);
    if (o->img) vkDestroyImage(vk->device, o->img, NULL);
    if (o->mem) vkFreeMemory(vk->device, o->mem, NULL);
}
typedef struct { ms_img_t *m0, *m2, *dp; lagfx_vk_render_target_t *r0, *r2; bool res2, gap; VkResolveModeFlagBits mode0; uint32_t fetch0; } msc_t;
static bool rec_msc(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    msc_t *g = arg;
    VkImageMemoryBarrier b[3]; uint32_t nb = 0;
    ms_img_t *im[3] = { g->m0, g->m2, g->dp };
    for (int i = 0; i < 3; i++) if (im[i]) {
        const bool d = i == 2;
        b[nb++] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = d ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = d ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i]->img,
            .subresourceRange = { d ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, nb, b);
    lagfx_vk_cmd_rt_to_color(cb, g->r0);
    if (g->res2) lagfx_vk_cmd_rt_to_color(cb, g->r2);
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = 16; d.height = 16; d.color_count = g->gap ? 3 : 2;
    const uint32_t s2 = g->gap ? 2 : 1;
    const VkClearColorValue red = { .float32 = { 1, 0, 0, 1 } }, green = { .float32 = { 0, 1, 0, 1 } };
    d.color[0] = (lagfx_render_attachment_t){ .view = g->m0->view, .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_4_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_DONT_CARE, .clear = { .color = red },
        .resolve_view = g->r0->view, .resolve_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .resolve_mode = g->mode0 ? g->mode0 : VK_RESOLVE_MODE_AVERAGE_BIT };
    if (g->fetch0) d.fetch_mask = g->fetch0, d.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;   /* multisampled input attachment named on slot 0 */
    d.color[s2] = (lagfx_render_attachment_t){ .view = g->m2->view, .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_4_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = g->res2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = green } };
    if (g->res2) { d.color[s2].resolve_view = g->r2->view; d.color[s2].resolve_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; d.color[s2].resolve_mode = VK_RESOLVE_MODE_AVERAGE_BIT; }
    if (g->dp) { d.has_depth_stencil = true;
        d.depth_stencil = (lagfx_render_attachment_t){ .view = g->dp->view, .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_4_BIT, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, .clear = { .depthStencil = { 1.0f, 0 } } }; }
    if (lagfx_vk_render_begin_desc(vk, cb, &d) != LAGFX_OK) return false;
    lagfx_vk_render_end(vk, cb);
    return true;
}


/* ---- C4 (legacy only): input attachment = colour slot 0 of the one subpass (fetch_mask), a descriptor of type INPUT_ATTACHMENT, a same-pixel barrier between two draws ---- */
typedef struct { ms_img_t *a; lagfx_vk_render_target_t *b; VkPipeline pipe; VkPipelineLayout pl; VkDescriptorSet set; } fetch_t;
static bool rec_fetch(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    fetch_t *g = arg;
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g->a->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    lagfx_vk_cmd_rt_to_color(cb, g->b);
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = 16; d.height = 16; d.color_count = 2; d.fetch_mask = 1u;
    const VkClearColorValue red = { .float32 = { 1, 0, 0, 1 } }, zero = { .float32 = { 0, 0, 0, 0 } };
    d.color[0] = (lagfx_render_attachment_t){ .view = g->a->view, .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_GENERAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = red } };
    d.color[1] = (lagfx_render_attachment_t){ .view = g->b->view, .format = g->b->format, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE, .clear = { .color = zero } };
    if (lagfx_vk_render_begin_desc(vk, cb, &d) != LAGFX_OK) return false;
    VkViewport vp = { 0, 0, 16, 16, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 16, 16 } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &g->set, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);   /* draw 1: reads the clear colour (red) of slot 0, writes blue into slot 0 */
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);   /* draw 2: reads what draw 1 wrote (blue) */
    lagfx_vk_render_end(vk, cb);
    return true;
}
static void test_legacy_fetch(struct lagfx_vk_state *vk, const char *bn) {
    char m[160];
    VkDevice dev = vk->device;
    ms_img_t a; lagfx_vk_render_target_t b; memset(&b, 0, sizeof b);
    int ok = ms_img_make(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &a)
          && lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &b) == LAGFX_OK;
    snprintf(m, sizeof m, "legacy fetch targets (%s)", bn); CHECK(ok, m);
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
    fetch_t g = { &a, &b, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    if (ok) {
        VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1 };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1 };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
        VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_fbf_spv, .pCode = k_vs_fbf_spv };
        VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_fbf_spv, .pCode = k_fs_fbf_spv };
        ok = vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS;
        pli.pSetLayouts = &dsl;
        ok = ok && vkCreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS && vkCreateDescriptorPool(dev, &dpi, NULL, &pool) == VK_SUCCESS
                && vkCreateShaderModule(dev, &vsi, NULL, &vs) == VK_SUCCESS && vkCreateShaderModule(dev, &fsi, NULL, &fs) == VK_SUCCESS;
        if (ok) {
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
            ok = vkAllocateDescriptorSets(dev, &ai, &g.set) == VK_SUCCESS;
        }
        if (ok) {
            VkDescriptorImageInfo ii = { VK_NULL_HANDLE, a.view, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .pImageInfo = &ii };
            vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
            lagfx_render_desc_t cd; memset(&cd, 0, sizeof cd);   /* the pass the pipeline is created against: same attachments and the same fetch mask as the pass it is used in */
            cd.color_count = 2; cd.fetch_mask = 1u;
            cd.color[0].format = VK_FORMAT_B8G8R8A8_UNORM; cd.color[0].samples = VK_SAMPLE_COUNT_1_BIT; cd.color[1] = cd.color[0];
            VkRenderPass rp = lagfx_vk_render_pass_compat(vk, &cd);
            VkPipelineShaderStageCreateInfo st[2] = {
                { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
                { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
            VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
            VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
            VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
            VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
            VkPipelineColorBlendAttachmentState cba[2] = { { .colorWriteMask = 0xF }, { .colorWriteMask = 0xF } };
            VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 2, .pAttachments = cba };
            VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
            VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = pl, .renderPass = rp, .subpass = 0 };
            ok = rp != VK_NULL_HANDLE && vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &pipe) == VK_SUCCESS;
        }
        snprintf(m, sizeof m, "legacy fetch pipeline / descriptor objects (%s)", bn); CHECK(ok, m);
    }
    if (ok) {
        g.pipe = pipe; g.pl = pl;
        snprintf(m, sizeof m, "legacy input attachment: pass with fetch_mask records and runs (%s)", bn); CHECK(one_shot(vk, rec_fetch, &g), m);
        /* slot 1 = what draw 2 read from slot 0 = what draw 1 wrote there: blue (BGRA b=255 g=0 r=0). A broken read gives black (0,0,0) or draw 1's red clear. */
        snprintf(m, sizeof m, "legacy input attachment: second draw read the first draw's write (blue), not the clear colour (%s)", bn); CHECK(px_is(vk, &b, 8, 8, 255, 0, 0), m);
    }
    if (pipe) vkDestroyPipeline(dev, pipe, NULL);
    if (fs) vkDestroyShaderModule(dev, fs, NULL);
    if (vs) vkDestroyShaderModule(dev, vs, NULL);
    if (pool) vkDestroyDescriptorPool(dev, pool, NULL);
    if (pl) vkDestroyPipelineLayout(dev, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    lagfx_vk_render_target_destroy(vk, &b);
    ms_img_free(vk, &a);
}


/* ---- C5: depth / stencil resolve (dynamic: resolveImageView; legacy: vkCreateRenderPass2 + VkSubpassDescriptionDepthStencilResolve). A 4x depth image is cleared to 1.0, a triangle at z = 0.25 covers the lower-left half
 * (its diagonal edge crosses pixels), then the pass resolves into a 1x image that is read back: pixels fully inside = 0.25, fully outside = 1.0, edge pixels = 0.25 for MIN and 1.0 for MAX; SAMPLE_ZERO is one of the two. ---- */
typedef struct { ms_img_t *ms, *res; VkFormat fmt; VkResolveModeFlagBits dmode, smode; VkPipeline pipe; VkImageAspectFlags asp; VkBuffer buf; uint32_t stencil_clear; } dsr_t;
static bool rec_dsres(struct lagfx_vk_state *vk, VkCommandBuffer cb, void *arg) {
    dsr_t *g = arg;
    VkImageMemoryBarrier b[2];
    ms_img_t *im[2] = { g->ms, g->res };
    for (int i = 0; i < 2; i++)
        b[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = im[i]->img, .subresourceRange = { g->asp, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 2, b);
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = 16; d.height = 16; d.has_depth_stencil = true;
    d.depth_stencil = (lagfx_render_attachment_t){ .view = g->ms->view, .format = g->fmt, .samples = VK_SAMPLE_COUNT_4_BIT, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR, .stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clear = { .depthStencil = { 1.0f, g->stencil_clear } },
        .resolve_view = g->res->view, .resolve_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .resolve_mode = g->dmode, .stencil_resolve_mode = g->smode };
    if (lagfx_vk_render_begin_desc(vk, cb, &d) != LAGFX_OK) return false;
    VkViewport vp = { 0, 0, 16, 16, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 16, 16 } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe);
    vkCmdDraw(cb, 3, 1, 0, 0);
    lagfx_vk_render_end(vk, cb);
    VkImageMemoryBarrier tb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g->res->img, .subresourceRange = { g->asp, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
    VkBufferImageCopy cp[2]; uint32_t ncp = 0;
    if (g->asp & VK_IMAGE_ASPECT_DEPTH_BIT) cp[ncp++] = (VkBufferImageCopy){ .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { 16, 16, 1 } };
    if (g->asp & VK_IMAGE_ASPECT_STENCIL_BIT) cp[ncp++] = (VkBufferImageCopy){ .bufferOffset = 2048, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { 16, 16, 1 } };
    vkCmdCopyImageToBuffer(cb, g->res->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->buf, ncp, cp);
    return true;
}
static void test_ds_resolve(struct lagfx_vk_state *vk, const char *bn, bool combined) {
    char m[200]; VkDevice dev = vk->device;
    const VkFormat fmt = combined ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
    const VkImageAspectFlags asp = combined ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageFormatProperties fp;
    if (vkGetPhysicalDeviceImageFormatProperties(vk->phys_device, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &fp) != VK_SUCCESS || !(fp.sampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
        printf("SKIP C5 (%s, %s): 4x depth image not offered\n", bn, combined ? "D32S8" : "D32"); return;
    }
    printf("NOTE C5 (%s): device depth modes 0x%x stencil modes 0x%x independent_none=%d independent=%d\n", bn, (unsigned)vk->dsres_depth_modes, (unsigned)vk->dsres_stencil_modes, (int)vk->dsres_independent_none, (int)vk->dsres_independent);
    snprintf(m, sizeof m, "depth/stencil resolve available on the device (%s)", bn); CHECK(vk->have_ds_resolve && vk->create_renderpass2, m);
    if (!vk->have_ds_resolve) return;
    float out[3][256]; unsigned nmodes = 0; VkResolveModeFlagBits mode_of[3];
    const VkResolveModeFlagBits want[3] = { VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, VK_RESOLVE_MODE_MIN_BIT, VK_RESOLVE_MODE_MAX_BIT };
    VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_dsres_spv, .pCode = k_vs_dsres_spv };
    VkShaderModule vs = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int ok = vkCreateShaderModule(dev, &vsi, NULL, &vs) == VK_SUCCESS && vkCreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS;
    if (ok) {
        lagfx_render_desc_t cd; memset(&cd, 0, sizeof cd);
        cd.has_depth_stencil = true; cd.depth_stencil.format = fmt; cd.depth_stencil.samples = VK_SAMPLE_COUNT_4_BIT;
        VkRenderPass rp = lagfx_vk_render_pass_compat(vk, &cd);   /* the pass the pipeline is created against: 4x depth(/stencil), no resolve (compatible with the resolving pass) */
        VkPipelineShaderStageCreateInfo st = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT };
        VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
        VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = fmt, .stencilAttachmentFormat = combined ? fmt : VK_FORMAT_UNDEFINED };
        const bool dynamic = lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC;
        VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = dynamic ? &ri : NULL, .stageCount = 1, .pStages = &st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cbs, .pDynamicState = &ds, .layout = pl, .renderPass = dynamic ? VK_NULL_HANDLE : rp, .subpass = 0 };
        ok = (dynamic || rp != VK_NULL_HANDLE) && vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &pipe) == VK_SUCCESS;
    }
    snprintf(m, sizeof m, "depth/stencil resolve pipeline (%s, %s)", bn, combined ? "D32S8" : "D32"); CHECK(ok, m);
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory bmem = VK_NULL_HANDLE; void *map = NULL;
    if (ok) {
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VkMemoryRequirements mr;
        ok = vkCreateBuffer(dev, &bi, NULL, &buf) == VK_SUCCESS;
        if (ok) { vkGetBufferMemoryRequirements(dev, buf, &mr); const uint32_t mt = memtype(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
            ok = mt != UINT32_MAX && vkAllocateMemory(dev, &ai, NULL, &bmem) == VK_SUCCESS && vkBindBufferMemory(dev, buf, bmem, 0) == VK_SUCCESS && vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS; }
    }
    for (int k = 0; ok && k < 3; k++) {
        if (!lagfx_vk_ds_resolve_ok(vk, fmt, want[k], combined ? want[k] : 0)) continue;   /* a mode the device does not list is covered by the refusal checks below */
        ms_img_t ms, res;
        const VkImageUsageFlags u = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        int ok2 = ms_img_make(vk, fmt, VK_SAMPLE_COUNT_4_BIT, u, asp, &ms) && ms_img_make(vk, fmt, VK_SAMPLE_COUNT_1_BIT, u | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, asp, &res);
        snprintf(m, sizeof m, "depth/stencil resolve images (%s)", bn); CHECK(ok2, m);
        if (ok2) {
            memset(map, 0xAB, 4096);
            dsr_t g = { &ms, &res, fmt, want[k], combined ? want[k] : 0, pipe, asp, buf, 7u };
            snprintf(m, sizeof m, "depth/stencil resolve mode 0x%x %s: pass records and runs (%s)", (unsigned)want[k], combined ? "D32S8" : "D32", bn); CHECK(one_shot(vk, rec_dsres, &g), m);
            memcpy(out[nmodes], map, 256 * sizeof(float)); mode_of[nmodes++] = want[k];
            if (combined) { const uint8_t *sp = (const uint8_t *)map + 2048; unsigned bad = 0; for (int i = 0; i < 256; i++) bad += sp[i] != 7u;
                snprintf(m, sizeof m, "stencil aspect resolved (SAMPLE_ZERO of the cleared 7) in all 256 pixels, %u wrong (%s)", bad, bn); CHECK(bad == 0, m); }
        }
        ms_img_free(vk, &res); ms_img_free(vk, &ms);
    }
    float *mn = NULL, *mx = NULL, *sz = NULL;
    for (unsigned i = 0; i < nmodes; i++) { if (mode_of[i] == VK_RESOLVE_MODE_MIN_BIT) mn = out[i]; if (mode_of[i] == VK_RESOLVE_MODE_MAX_BIT) mx = out[i]; if (mode_of[i] == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) sz = out[i]; }
    snprintf(m, sizeof m, "depth resolve: at least SAMPLE_ZERO ran (%s)", bn); CHECK(sz != NULL, m);
    if (sz) { unsigned inside = 0, outside = 0, other = 0; for (int i = 0; i < 256; i++) { if (sz[i] == 0.25f) inside++; else if (sz[i] == 1.0f) outside++; else other++; }
        snprintf(m, sizeof m, "depth SAMPLE_ZERO: %u inside (0.25), %u outside (1.0), %u other (%s)", inside, outside, other, bn); CHECK(other == 0 && inside > 60 && outside > 60, m); }
    if (mn && mx) {
        unsigned edge = 0, bad = 0, inside = 0, outside = 0;
        for (int i = 0; i < 256; i++) {
            if (mn[i] == 0.25f && mx[i] == 1.0f) edge++;
            else if (mn[i] == 0.25f && mx[i] == 0.25f) inside++;
            else if (mn[i] == 1.0f && mx[i] == 1.0f) outside++;
            else bad++;
            if (sz && !(sz[i] == mn[i] || sz[i] == mx[i])) bad++;
        }
        snprintf(m, sizeof m, "depth MIN vs MAX: %u edge pixels (min 0.25, max 1.0), %u inside, %u outside, %u inconsistent (%s)", edge, inside, outside, bad, bn); CHECK(bad == 0 && edge >= 8 && inside > 40 && outside > 40, m);
    } else printf("NOTE C5 (%s): device offers %s%s%s -- MIN/MAX comparison not run\n", bn, mn ? "MIN " : "", mx ? "MAX " : "", (mn || mx) ? "" : "neither");
    /* refusals: a mode the device does not list, a resolve view on a single-sample attachment, a missing stencil mode on D32S8 without independent resolve */
    { ms_img_t ms, res; int ok2 = ms_img_make(vk, fmt, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, asp, &ms) && ms_img_make(vk, fmt, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, asp, &res);
      if (ok2) {
        dsr_t g = { &ms, &res, fmt, (VkResolveModeFlagBits)0x40000000, combined ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : 0, pipe, asp, buf, 0 };
        snprintf(m, sizeof m, "direct begin_desc: a depth resolve mode no device lists is refused (%s)", bn); CHECK(!one_shot(vk, rec_dsres, &g), m);
        g.dmode = 0; g.smode = 0;
        snprintf(m, sizeof m, "direct begin_desc: a resolve view with no resolve mode is refused (%s)", bn); CHECK(!one_shot(vk, rec_dsres, &g), m);
      }
      ms_img_free(vk, &res); ms_img_free(vk, &ms); }
    if (map) vkUnmapMemory(dev, bmem);
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (bmem) vkFreeMemory(dev, bmem, NULL);
    if (pipe) vkDestroyPipeline(dev, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(dev, pl, NULL);
    if (vs) vkDestroyShaderModule(dev, vs, NULL);
}

static void run_backend(int legacy) {
    const char *bn = legacy ? "legacy" : "dynamic";
    char m[160];
    if (legacy) setenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS", "1", 1); else unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    struct lagfx_vk_state *vk = NULL; lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { snprintf(m, sizeof m, "init (%s)", bn); CHECK(0, m); return; }
    CHECK(lagfx_vk_render_backend(vk) == (legacy ? LAGFX_RENDER_LEGACY : LAGFX_RENDER_DYNAMIC), "backend as requested");

    /* A. two colours + depth in one pass */
    {
        lagfx_vk_render_target_t c0, c1; memset(&c0, 0, sizeof c0); memset(&c1, 0, sizeof c1);
        int ok = lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &c0) == LAGFX_OK && lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &c1) == LAGFX_OK;
        VkImage di = VK_NULL_HANDLE; VkDeviceMemory dm = VK_NULL_HANDLE; VkImageView dv = VK_NULL_HANDLE;
        if (ok) {
            VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .extent = { 16, 16, 1 }, .mipLevels = 1, .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
            ok = vkCreateImage(vk->device, &ii, NULL, &di) == VK_SUCCESS;
            if (ok) {
                VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk->device, di, &mr);
                uint32_t mt = memtype(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt == UINT32_MAX) mt = memtype(vk->phys_device, mr.memoryTypeBits, 0);
                VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
                VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = di, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
                ok = mt != UINT32_MAX && vkAllocateMemory(vk->device, &ai, NULL, &dm) == VK_SUCCESS && vkBindImageMemory(vk->device, di, dm, 0) == VK_SUCCESS && vkCreateImageView(vk->device, &vi, NULL, &dv) == VK_SUCCESS;
            }
        }
        snprintf(m, sizeof m, "targets + depth image (%s)", bn); CHECK(ok, m);
        if (ok) {
            mrt_t mr = { &c0, &c1, di, dv, 16u, 16u };
            snprintf(m, sizeof m, "2 colours + depth pass records and runs (%s)", bn); CHECK(one_shot(vk, rec_mrt, &mr), m);
            snprintf(m, sizeof m, "colour 0 holds ITS clear (red) (%s)", bn); CHECK(px_is(vk, &c0, 8, 8, 0, 0, 255), m);
            snprintf(m, sizeof m, "colour 1 holds ITS clear (green) (%s)", bn); CHECK(px_is(vk, &c1, 8, 8, 0, 255, 0), m);
        }
        /* the framebuffer references the colour views and the depth view: forget them all before they go (waits idle: the pass has finished) */
        if (dv) { lagfx_forget_t f = lagfx_vk_render_forget_view(vk, dv); CHECK(f == LAGFX_FORGET_OK, "forget depth view: not deferred"); vkDestroyImageView(vk->device, dv, NULL); }
        if (di) vkDestroyImage(vk->device, di, NULL);
        if (dm) vkFreeMemory(vk->device, dm, NULL);
        lagfx_vk_render_target_destroy(vk, &c1); lagfx_vk_render_target_destroy(vk, &c0);
    }

    /* B. a triangle through pipeline_build + draw_record */
    {
        lagfx_vk_render_target_t rt; memset(&rt, 0, sizeof rt);
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
        VkBuffer vb = VK_NULL_HANDLE; VkDeviceMemory vbm = VK_NULL_HANDLE; void *map = NULL;
        VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
        VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_spv, .pCode = k_fs_spv };
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        int ok = lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK
              && vkCreateShaderModule(vk->device, &vsi, NULL, &vs) == VK_SUCCESS && vkCreateShaderModule(vk->device, &fsi, NULL, &fs) == VK_SUCCESS
              && vkCreateBuffer(vk->device, &bi, NULL, &vb) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk->device, vb, &mr);
            uint32_t mt = memtype(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
            ok = mt != UINT32_MAX && vkAllocateMemory(vk->device, &ai, NULL, &vbm) == VK_SUCCESS && vkBindBufferMemory(vk->device, vb, vbm, 0) == VK_SUCCESS && vkMapMemory(vk->device, vbm, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
        }
        if (ok) {
            const float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };   /* covers the middle, not the corners, whichever way y points */
            memcpy(map, tri, sizeof tri);
            lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
            d.vertex_shader = vs; d.fragment_shader = fs; d.layout = vk->empty_layout; d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
            d.n_vtx_inputs = 1; d.vtx_in_loc[0] = 0; d.vtx_in_comp[0] = 2; d.vtx_stride = 8;
            if (legacy) {
                lagfx_render_desc_t rd; VkRenderPass rp = VK_NULL_HANDLE;
                CHECK(lagfx_pipeline_render_desc(&d, &rd), "render desc for a plain one-colour pipeline");
                CHECK(rd.color_count == 1 && rd.color[0].format == VK_FORMAT_B8G8R8A8_UNORM && !rd.has_depth_stencil, "render desc: one BGRA8 colour, no depth");
                rp = lagfx_vk_render_pass_compat(vk, &rd);
                CHECK(rp != VK_NULL_HANDLE, "compatible render pass for the pipeline");
                d.render_pass = (uint64_t)(uintptr_t)rp;
            }
            snprintf(m, sizeof m, "triangle pipeline builds (%s)", bn);
            ok = lagfx_pipeline_build(vk->device, &d, &pipe) == LAGFX_OK && pipe != VK_NULL_HANDLE; CHECK(ok, m);
        }
        if (ok) {
            const float blue[4] = { 0, 0, 1, 1 };
            void *a[2] = { &rt, (void *)blue };
            CHECK(one_shot(vk, rec_clear, a), "target cleared to blue");
            lagfx_status_t st = lagfx_vk_draw_record_and_submit_bound(vk, pipe, VK_NULL_HANDLE, VK_NULL_HANDLE, &rt, false, 3, 1, 0, 0, 0, vb, NULL);
            snprintf(m, sizeof m, "draw_record_and_submit_bound OK (%s)", bn); CHECK(st == LAGFX_OK, m);
            snprintf(m, sizeof m, "covered pixel has the shader colour (red) (%s)", bn); CHECK(px_is(vk, &rt, 8, 8, 0, 0, 255), m);
            snprintf(m, sizeof m, "uncovered corner keeps the previous contents (blue: LOAD) (%s)", bn); CHECK(px_is(vk, &rt, 0, 0, 255, 0, 0), m);
        }
        if (pipe) vkDestroyPipeline(vk->device, pipe, NULL);
        if (vb) vkDestroyBuffer(vk->device, vb, NULL);
        if (vbm) vkFreeMemory(vk->device, vbm, NULL);
        if (vs) vkDestroyShaderModule(vk->device, vs, NULL);
        if (fs) vkDestroyShaderModule(vk->device, fs, NULL);
        lagfx_vk_render_target_destroy(vk, &rt);
    }

    /* C. WaitIdle failure injection (the legacy backend is the one that caches framebuffers) */
    if (legacy) {
        for (int lost = 0; lost < 2; lost++) {
            lagfx_vk_render_target_t rt; memset(&rt, 0, sizeof rt);
            int ok = lagfx_vk_render_target_create(vk, 8u, 8u, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK && one_shot(vk, rec_load_pass, &rt);
            CHECK(ok, "target with a cached framebuffer");
            if (!ok) { lagfx_vk_render_target_destroy(vk, &rt); continue; }
            const unsigned before = lagfx_vk_render_deferred_count(vk);
            lagfx_vk_render_test_fail_waitidle(vk, 1, lost != 0);
            VkImageView v = rt.view;
            lagfx_vk_render_target_destroy(vk, &rt);
            if (!lost) {
                CHECK(lagfx_vk_render_deferred_count(vk) == before + 1, "WaitIdle failed: view + image + memory deferred, not destroyed");
                CHECK(rt.view == VK_NULL_HANDLE && rt.image == VK_NULL_HANDLE, "the target struct no longer owns them");
                (void)v;
            } else {
                CHECK(lagfx_vk_render_deferred_count(vk) == before, "DEVICE_LOST counts as idle: the target is destroyed, nothing deferred");
                CHECK(vk->device_lost, "DEVICE_LOST is remembered (sticky)");
                lagfx_vk_render_target_t r3; memset(&r3, 0, sizeof r3);
                lagfx_render_color_t cc = { .view = (VkImageView)(uintptr_t)1, .format = VK_FORMAT_B8G8R8A8_UNORM, .width = 8, .height = 8, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .load = VK_ATTACHMENT_LOAD_OP_LOAD, .store = VK_ATTACHMENT_STORE_OP_STORE };
                CHECK(lagfx_vk_render_begin(vk, (VkCommandBuffer)(uintptr_t)1, &cc) == LAGFX_ERR_BACKEND, "after DEVICE_LOST: render_begin refuses new work");
                CHECK(lagfx_vk_draw_record_and_submit_bound(vk, (VkPipeline)(uintptr_t)1, VK_NULL_HANDLE, VK_NULL_HANDLE, &r3, false, 3, 1, 0, 0, 0, VK_NULL_HANDLE, NULL) == LAGFX_ERR_BACKEND, "after DEVICE_LOST: draw_record refuses new work");
                vk->device_lost = false;   /* the injected loss was not real: the device itself is fine, the rest of the test goes on */
            }
        }
        /* a second use of the same state after a deferral keeps working */
        lagfx_vk_render_target_t rt2; memset(&rt2, 0, sizeof rt2);
        CHECK(lagfx_vk_render_target_create(vk, 8u, 8u, VK_FORMAT_B8G8R8A8_UNORM, &rt2) == LAGFX_OK && one_shot(vk, rec_load_pass, &rt2), "device still usable after a deferral");
        lagfx_vk_render_target_destroy(vk, &rt2);
    }

    /* C2. a pass with an unused colour slot between two real ones: slot 2 keeps its index (the shader's output location), pixels land on the right targets */
    {
        lagfx_vk_render_target_t c0, c2; memset(&c0, 0, sizeof c0); memset(&c2, 0, sizeof c2);
        int ok = lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &c0) == LAGFX_OK && lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &c2) == LAGFX_OK;
        snprintf(m, sizeof m, "gap pass targets (%s)", bn); CHECK(ok, m);
        if (ok) {
            gap_t g = { &c0, &c2, 16u, 16u, 0 };
            snprintf(m, sizeof m, "colour 0 + unused slot 1 + colour 2: pass records and runs (%s)", bn); CHECK(one_shot(vk, rec_gap, &g), m);
            g.fetch_mask = 1u << 1;   /* the unused slot 1 named as an input attachment: the legacy backend refuses it, the dynamic backend ignores the mask (it must never be constrained by it) */
            { const bool acc = one_shot(vk, rec_gap, &g);
              snprintf(m, sizeof m, "fetch_mask on an unused slot: legacy refuses, dynamic ignores (%s)", bn); CHECK(legacy ? !acc : acc, m); }
            g.fetch_mask = 0;
            snprintf(m, sizeof m, "slot 0 holds red (%s)", bn); CHECK(px_is(vk, &c0, 8, 8, 0, 0, 255), m);
            snprintf(m, sizeof m, "slot 2 holds green, not shifted onto slot 1 (%s)", bn); CHECK(px_is(vk, &c2, 8, 8, 0, 255, 0), m);
            VkRenderPass a, b2; lagfx_render_desc_t d1; memset(&d1, 0, sizeof d1);
            d1.color_count = 3; d1.color[0].format = VK_FORMAT_B8G8R8A8_UNORM; d1.color[2].format = VK_FORMAT_B8G8R8A8_UNORM;
            lagfx_render_desc_t d2 = d1; d2.color[1].format = d2.color[2].format; d2.color[2].format = VK_FORMAT_UNDEFINED;
            a = lagfx_vk_render_pass_compat(vk, &d1); b2 = lagfx_vk_render_pass_compat(vk, &d2);
            snprintf(m, sizeof m, "the position of the gap is part of the pass identity (%s)", bn); CHECK(a != VK_NULL_HANDLE && b2 != VK_NULL_HANDLE && a != b2, m);
        }
        lagfx_vk_render_target_destroy(vk, &c2); lagfx_vk_render_target_destroy(vk, &c0);
    }


    /* C3. multisampled colour + resolve combinations. All clear-only passes, so each resolved pixel is exactly the clear colour. */
    {
        VkImageFormatProperties fp;
        const bool ms_ok = vkGetPhysicalDeviceImageFormatProperties(vk->phys_device, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &fp) == VK_SUCCESS && (fp.sampleCounts & VK_SAMPLE_COUNT_4_BIT);
        if (!ms_ok) printf("SKIP C3 (%s): 4x not offered for BGRA8\n", bn);
        else for (int variant = 0; variant < 4; variant++) {
            /* 0: partial resolve (slot 0 resolves, slot 1 stored only); 1: gap + resolve in both real slots; 2: partial resolve + depth; 3: gap + partial resolve + depth */
            const bool gap = variant == 1 || variant == 3, dep = variant >= 2, res2 = variant == 1;
            ms_img_t m0, m2, dp; memset(&dp, 0, sizeof dp);
            lagfx_vk_render_target_t r0, r2; memset(&r0, 0, sizeof r0); memset(&r2, 0, sizeof r2);
            const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            int ok = ms_img_make(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, &m0) && ms_img_make(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, &m2)
                && lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &r0) == LAGFX_OK && lagfx_vk_render_target_create(vk, 16u, 16u, VK_FORMAT_B8G8R8A8_UNORM, &r2) == LAGFX_OK;
            if (ok && dep) ok = ms_img_make(vk, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &dp);
            snprintf(m, sizeof m, "msaa resolve combo %d targets (%s)", variant, bn); CHECK(ok, m);
            if (ok) {
                msc_t g = { &m0, &m2, dep ? &dp : NULL, &r0, &r2, res2, gap, 0 };
                snprintf(m, sizeof m, "msaa combo %d (gap=%d depth=%d resolve2=%d): pass records and runs (%s)", variant, gap, dep, res2, bn); CHECK(one_shot(vk, rec_msc, &g), m);
                snprintf(m, sizeof m, "msaa combo %d: slot 0 resolved to red (%s)", variant, bn); CHECK(px_is(vk, &r0, 8, 8, 0, 0, 255), m);
                if (variant == 0 && legacy) {   /* the descriptor entry point itself must refuse a mode the legacy implicit resolve does not perform (BGRA8 = average). Not run on the dynamic backend: SAMPLE_ZERO on a normalised colour attachment is itself invalid there (VUID-VkRenderingAttachmentInfo-imageView-06129) */
                    g.mode0 = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                    const bool accepted = one_shot(vk, rec_msc, &g);
                    snprintf(m, sizeof m, "msaa combo: direct begin_desc with resolve mode SAMPLE_ZERO on BGRA8 is refused on legacy (%s)", bn);
                    CHECK(!accepted, m);
                    g.mode0 = 0;
                }
                if (variant == 0 && legacy) {   /* direct begin_desc: a multisampled fetch slot is refused when the per-sample read path is not available (no sampleRateShading), for every caller */
                    const bool srs = vk->have_sample_rate_shading;
                    vk->have_sample_rate_shading = false; g.fetch0 = 1u;
                    const bool accepted = one_shot(vk, rec_msc, &g);
                    vk->have_sample_rate_shading = srs; g.fetch0 = 0;
                    snprintf(m, sizeof m, "msaa fetch: direct begin_desc with a multisampled fetch slot and no sampleRateShading is refused on legacy (%s)", bn);
                    CHECK(!accepted, m);
                }
                if (res2) { snprintf(m, sizeof m, "msaa combo %d: the second real slot resolved to green, not shifted (%s)", variant, bn); CHECK(px_is(vk, &r2, 8, 8, 0, 255, 0), m); }
            }
            lagfx_vk_render_target_destroy(vk, &r2); lagfx_vk_render_target_destroy(vk, &r0);
            ms_img_free(vk, &dp); ms_img_free(vk, &m2); ms_img_free(vk, &m0);
        }
    }

    if (legacy) test_legacy_fetch(vk, bn);
    test_ds_resolve(vk, bn, false);
    test_ds_resolve(vk, bn, true);

    /* D. shapes the legacy backend does not express are refused */
    {
        lagfx_pipeline_desc_t d; lagfx_render_desc_t rd; memset(&d, 0, sizeof d);
        d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
        CHECK(lagfx_pipeline_render_desc(&d, &rd), "plain pipeline: expressible");
        d.no_color = 1; CHECK(!lagfx_pipeline_render_desc(&d, &rd), "no_color without depth/stencil: refused");
        d.dyn_state = 1; d.ds_depth_format = VK_FORMAT_D32_SFLOAT; CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.color_count == 0 && rd.has_depth_stencil, "no_color + depth format: depth-only description");
        d.no_color = 0; d.dyn_state = 0; d.ds_depth_format = VK_FORMAT_UNDEFINED;
        d.dyn_state = 1; CHECK(lagfx_pipeline_render_desc(&d, &rd) && !rd.has_depth_stencil, "dyn_state without depth/stencil formats: colour only");
        d.ds_depth_format = VK_FORMAT_D32_SFLOAT; CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.has_depth_stencil && rd.depth_stencil.format == VK_FORMAT_D32_SFLOAT, "dyn_state + depth format: depth attachment in the description");
        d.ds_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT; CHECK(!lagfx_pipeline_render_desc(&d, &rd), "different depth and stencil formats: refused");
        d.ds_depth_format = VK_FORMAT_UNDEFINED; CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.depth_stencil.format == VK_FORMAT_D24_UNORM_S8_UINT, "stencil format only: that format is the attachment");
        d.ds_stencil_format = VK_FORMAT_UNDEFINED; d.dyn_state = 0;
        d.n_color = 2; d.color_formats[0] = VK_FORMAT_B8G8R8A8_UNORM; d.color_formats[1] = VK_FORMAT_UNDEFINED;
        CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.color_count == 2 && rd.color[0].format == VK_FORMAT_B8G8R8A8_UNORM && rd.color[1].format == VK_FORMAT_UNDEFINED, "a gap after slot 0: expressed as an unused slot 1");
        d.color_formats[0] = VK_FORMAT_UNDEFINED; d.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM;
        CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.color_count == 2 && rd.color[0].format == VK_FORMAT_UNDEFINED && rd.color[1].format == VK_FORMAT_R8G8B8A8_UNORM, "a gap before slot 1: slot 1 keeps its index");
        d.color_formats[1] = VK_FORMAT_UNDEFINED;
        { lagfx_legacy_refuse_t why = LAGFX_LR_NONE; CHECK(!lagfx_pipeline_render_desc_why(&d, &rd, &why) && why == LAGFX_LR_NO_COLOR, "no colour slot with a format: refused, reason NO_COLOR"); }
        d.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM; CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.color_count == 2, "slot 0 gap + slot 1: expressible");
        d.color_formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
        { lagfx_legacy_refuse_t why = LAGFX_LR_NONE; d.no_color = 1; d.dyn_state = 0;
          CHECK(!lagfx_pipeline_render_desc_why(&d, &rd, &why) && why == LAGFX_LR_NO_DS, "no_color without depth/stencil: reason NO_DS");
          d.no_color = 0; d.dyn_state = 1; d.ds_depth_format = VK_FORMAT_D32_SFLOAT; d.ds_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
          CHECK(!lagfx_pipeline_render_desc_why(&d, &rd, &why) && why == LAGFX_LR_DS_MIXED, "different depth and stencil formats: reason DS_MIXED"); }
    }
    CHECK(lagfx_legacy_resolve_mode_ok(VK_FORMAT_B8G8R8A8_UNORM, VK_RESOLVE_MODE_AVERAGE_BIT) && lagfx_legacy_resolve_mode_ok(VK_FORMAT_R16G16B16A16_SFLOAT, VK_RESOLVE_MODE_AVERAGE_BIT)
          && lagfx_legacy_resolve_mode_ok(VK_FORMAT_R32_UINT, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) && lagfx_legacy_resolve_mode_ok(VK_FORMAT_R8G8B8A8_SINT, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
          && !lagfx_legacy_resolve_mode_ok(VK_FORMAT_B8G8R8A8_UNORM, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) && !lagfx_legacy_resolve_mode_ok(VK_FORMAT_R32_UINT, VK_RESOLVE_MODE_AVERAGE_BIT)
          && !lagfx_legacy_resolve_mode_ok(VK_FORMAT_B8G8R8A8_UNORM, 0) && !lagfx_legacy_resolve_mode_ok(VK_FORMAT_B8G8R8A8_UNORM, VK_RESOLVE_MODE_MIN_BIT),
          "legacy resolve mode: only the mode the implicit resolve performs is accepted (float/normalised average, integer sample 0)");
    { snprintf(m, sizeof m, "validation: messenger live, control delivered, 0 messages before it (%s)", bn); CHECK(lagfx_test_validation_expect(vk, bn) == 0, m); }
    const uint64_t msgs_before_shutdown = lagfx_vk_validation_msg_count;
    lagfx_vk_shutdown(vk);   /* after a deferral: the deferred view/image/memory go here, after the framebuffers */
    if (getenv("LAGFX_VK_VALIDATION") && getenv("LAGFX_VK_VALIDATION")[0] == '1') { snprintf(m, sizeof m, "validation: no message during shutdown / cache teardown (%s)", bn); CHECK(lagfx_vk_validation_msg_count == msgs_before_shutdown, m); }
}

/* E. DEVICE_LOST is seen in ONE place (lagfx_vk_queue_submit / lagfx_vk_wait_fences): after it, no submit of ANY kind reaches the driver (readback, submit_empty, submit_checked,
 * a raw empty submit with a fence that would signal), objects can still be destroyed. The loss is INJECTED (the submit seam), not a real device loss. */
static void test_lost_layer(void) {
    unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS"); unsetenv("LAGFX_VK_FORCE_LEGACY_FEATURES");
    struct lagfx_vk_state *vk = NULL; lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { CHECK(0, "init (lost layer)"); return; }
    lagfx_vk_render_target_t rt; memset(&rt, 0, sizeof rt);
    CHECK(lagfx_vk_render_target_create(vk, 8u, 8u, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK, "target for the lost-layer test");
    uint8_t px[8 * 8 * 4]; size_t stride = 0;
    CHECK(lagfx_vk_render_target_readback(vk, &rt, px, sizeof px, &stride) == LAGFX_OK, "before the loss: readback works");
    CHECK(lagfx_vk_submit_empty(vk) == LAGFX_OK && !lagfx_vk_device_lost(vk), "before the loss: submit_empty works, flag clear");

    lagfx_vk_test_inject_submit_result((int)VK_ERROR_DEVICE_LOST);
    CHECK(lagfx_vk_submit_empty(vk) != LAGFX_OK, "injected DEVICE_LOST: submit_empty fails");
    CHECK(lagfx_vk_device_lost(vk), "the one submit layer set the sticky flag");
    lagfx_vk_test_inject_submit_result(0x7fffffff);   /* the seam is off again: only the sticky flag keeps the work out */

    CHECK(lagfx_vk_render_target_readback(vk, &rt, px, sizeof px, &stride) != LAGFX_OK, "after the loss: readback submits nothing");
    CHECK(lagfx_vk_submit_empty(vk) != LAGFX_OK, "after the loss: submit_empty submits nothing");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    CHECK(lagfx_vk_submit_checked(vk, &si, VK_NULL_HANDLE) == LAGFX_ERR_BACKEND, "after the loss: submit_checked refuses");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f = VK_NULL_HANDLE;
    CHECK(vkCreateFence(vk->device, &fci, NULL, &f) == VK_SUCCESS, "fence");
    CHECK(lagfx_vk_queue_submit(vk, &si, f) == VK_ERROR_DEVICE_LOST, "after the loss: the common submit returns DEVICE_LOST");
    CHECK(vkGetFenceStatus(vk->device, f) == VK_NOT_READY, "after the loss: the empty submit never reached the driver (its fence did not signal)");
    vkDestroyFence(vk->device, f, NULL);
    lagfx_vk_render_target_destroy(vk, &rt);   /* destroying stays allowed */
    lagfx_vk_shutdown(vk);
}
#endif

int main(void) {
#ifndef LAGFX_HAVE_VULKAN
    printf("no Vulkan build: skip\n"); return 77;
#else
    struct lagfx_vk_state *vk = NULL; lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (getenv("LAGFX_VK_TEST_API_MINOR")) {   /* simulated Vulkan 1.1 (instance.c test knob): only the depth/stencil resolve through the pre-1.2 path (VK_KHR_depth_stencil_resolve + vkCreateRenderPass2KHR), legacy backend */
        setenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS", "1", 1); setenv("LAGFX_VK_LEGACY_RENDERPASS_GPU", "1", 1);
        if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
        CHECK(VK_VERSION_MAJOR(vk->phys_props.apiVersion) == 1 && VK_VERSION_MINOR(vk->phys_props.apiVersion) < 2, "simulated device reports below Vulkan 1.2");
        CHECK(lagfx_vk_render_backend(vk) == LAGFX_RENDER_LEGACY, "legacy backend");
        test_ds_resolve(vk, "legacy, simulated 1.1 (KHR entry points)", false);
        test_ds_resolve(vk, "legacy, simulated 1.1 (KHR entry points)", true);
        CHECK(lagfx_test_validation_expect(vk, "simulated 1.1") == 0, "validation: control delivered, 0 messages before it (simulated 1.1)");
        lagfx_vk_shutdown(vk);
        printf(g_fail ? "FAILED: %d\n" : "OK\n", g_fail);
        return g_fail ? 1 : 0;
    }
    unsetenv("LAGFX_VK_FORCE_LEGACY_FEATURES"); unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    lagfx_vk_shutdown(vk);
    run_backend(0);
    run_backend(1);
    test_lost_layer();
    unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    printf(g_fail ? "FAILED: %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
#endif
}
