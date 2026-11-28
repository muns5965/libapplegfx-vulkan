/* SPDX-License-Identifier: MIT */
/* General render description (render_pass.c) + the converted draw path, on lavapipe, once per backend (dynamic rendering, and the legacy VkRenderPass backend forced with
 * LAGFX_VK_FORCE_LEGACY_RENDERPASS=1):
 *   A. two colour attachments + a D32 depth attachment in ONE pass: each colour target keeps ITS OWN clear value (clearValue index == attachment index);
 *   B. a real triangle through lagfx_pipeline_build (render_pass from lagfx_pipeline_render_desc / lagfx_vk_render_pass_compat on the legacy backend) and
 *      lagfx_vk_draw_record_and_submit_bound: the covered pixel has the shader colour, an uncovered pixel keeps what the target held (LOAD);
 *   C. legacy backend only: vkDeviceWaitIdle failure injected into render_forget_view -> the target's view/image/memory are DEFERRED (not destroyed), VK_ERROR_DEVICE_LOST counts
 *      as idle and the target is destroyed; shutdown after a deferral is clean;
 *   D. shapes the legacy backend does not express (UNDEFINED colour slot, no_color, dyn_state) are refused by lagfx_pipeline_render_desc, not built for another pass.
 * NOT verified: a real Vulkan 1.1 driver (Adreno), depth CONTENT (the depth image is only attached and cleared), the validation layer (not enabled here). Exit 77 = no usable device. */
#define _GNU_SOURCE
#include "vulkan/instance.h"
#include "vulkan/render_pass.h"
#include "vulkan/render_target.h"
#include "vulkan/command.h"
#include "vulkan/draw_record.h"
#include "vulkan/pipeline_build.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "arm64-exec-run-shaders.h"   /* k_vs_spv (vec2 pos at location 0), k_fs_spv (constant red) */

static int g_fail;
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
        CHECK(!lagfx_pipeline_render_desc(&d, &rd), "an UNDEFINED colour slot: refused");
        d.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM; CHECK(lagfx_pipeline_render_desc(&d, &rd) && rd.color_count == 2, "two defined colour slots: expressible");
    }
    lagfx_vk_shutdown(vk);   /* after a deferral: the deferred view/image/memory go here, after the framebuffers */
}
#endif

int main(void) {
#ifndef LAGFX_HAVE_VULKAN
    printf("no Vulkan build: skip\n"); return 77;
#else
    struct lagfx_vk_state *vk = NULL; lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    unsetenv("LAGFX_VK_FORCE_LEGACY_FEATURES"); unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    lagfx_vk_shutdown(vk);
    run_backend(0);
    run_backend(1);
    unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    printf(g_fail ? "FAILED: %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
#endif
}
