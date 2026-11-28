/* Real-draw equivalence of the fallback pipeline: the same triangle is drawn with (A) the pipeline built from the original modules and (B) the pipeline the
 * fallback produces when the original build is reported failed (hook lagfx_pipeline_test_fail_original) - unused capability declarations stripped from both
 * stages. The read-back images must be byte-identical and not just the clear colour. Both render paths are run: the LEGACY VkRenderPass path (the S8 path: no
 * dynamicRendering) and, when the device has it, dynamic rendering. Also: the result of a failed build survives the matrix / reduce diagnostics (they build
 * other pipelines through the same function). lavapipe only; the Adreno -13 itself is NOT reproduced here.
 * SPDX-License-Identifier: MIT */
#include "device.h"
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"
#include <vulkan/vulkan.h>
#include "arm64-exec-run-shaders.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define W 16
#define H 16
#define MEM 4096

static uint8_t g_mem[MEM];
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM) return false; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }

static struct lagfx_vk_state *g_vk;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_vk->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    assert(!"no memory type"); return 0;
}
static void mkbuf(VkDeviceSize n, VkBufferUsageFlags u, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u };
    assert(vkCreateBuffer(g_vk->device, &bi, NULL, b) == VK_SUCCESS);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk->device, *b, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    assert(vkAllocateMemory(g_vk->device, &ai, NULL, m) == VK_SUCCESS);
    assert(vkBindBufferMemory(g_vk->device, *b, *m, 0) == VK_SUCCESS);
    assert(vkMapMemory(g_vk->device, *m, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS);
}

/* Draw the triangle with `pipe` (legacy render pass `rp` when legacy, dynamic rendering otherwise) and read the W x H BGRA8 image back into out[]. */
static void draw(VkPipeline pipe, VkRenderPass rp, bool legacy, uint32_t *out) {
    VkDevice dev = g_vk->device;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; assert(vkCreateImage(dev, &ii, NULL, &img) == VK_SUCCESS);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory im; assert(vkAllocateMemory(dev, &ai, NULL, &im) == VK_SUCCESS); assert(vkBindImageMemory(dev, img, im, 0) == VK_SUCCESS);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; assert(vkCreateImageView(dev, &vi, NULL, &view) == VK_SUCCESS);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (legacy) { VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
                  assert(vkCreateFramebuffer(dev, &fi, NULL, &fb) == VK_SUCCESS); }
    VkBuffer vb, rb; VkDeviceMemory vm, rm; void *vmap, *rmap;
    mkbuf(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vm, &vmap);
    mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rm, &rmap);
    const float tri[6] = { -0.6f, -0.6f, 0.6f, -0.6f, 0.0f, 0.7f };   /* partial coverage: some pixels drawn, some left at the clear colour */
    memcpy(vmap, tri, sizeof tri); memset(rmap, 0xCD, W * H * 4);
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; assert(vkAllocateCommandBuffers(dev, &ca, &cb) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    assert(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
    const VkClearValue clr = { .color = { .float32 = { 0.f, 0.f, 1.f, 1.f } } };   /* blue; the shader writes red */
    VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (legacy) {
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clr };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    } else {
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = sr };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");
        if (!begin) begin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR");
        assert(begin);
        VkRenderingAttachmentInfo ca2 = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = clr };
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca2 };
        begin(cb, &ri);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (legacy) vkCmdEndRenderPass(cb);
    else {
        PFN_vkCmdEndRendering end = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
        if (!end) end = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR");
        assert(end); end(cb);
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = sr };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &cp);
    assert(vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    assert(vkQueueSubmit(g_vk->graphics_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(g_vk->graphics_queue) == VK_SUCCESS);
    memcpy(out, rmap, W * H * 4);
    vkFreeCommandBuffers(dev, g_vk->cmd_pool, 1, &cb);
    vkDestroyBuffer(dev, vb, NULL); vkFreeMemory(dev, vm, NULL); vkDestroyBuffer(dev, rb, NULL); vkFreeMemory(dev, rm, NULL);
    if (fb) vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyImageView(dev, view, NULL); vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, im, NULL);
}

static void run_path(bool legacy) {
    VkDevice dev = g_vk->device;
    /* modules with unused capability declarations the fallback can strip (Float16 in the VS, Int16 in the FS; nothing in the program uses them) */
    uint32_t cvs[sizeof k_vs_spv / 4 + 2], cfs[sizeof k_fs_spv / 4 + 2];
    memcpy(cvs, k_vs_spv, 5 * 4); cvs[5] = 0x00020011u, cvs[6] = 9u; memcpy(cvs + 7, k_vs_spv + 5, sizeof k_vs_spv - 5 * 4);
    memcpy(cfs, k_fs_spv, 5 * 4); cfs[5] = 0x00020011u, cfs[6] = 22u; memcpy(cfs + 7, k_fs_spv + 5, sizeof k_fs_spv - 5 * 4);
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof cvs, .pCode = cvs };
    assert(vkCreateShaderModule(dev, &ci, NULL, &vs) == VK_SUCCESS);
    ci.codeSize = sizeof cfs, ci.pCode = cfs; assert(vkCreateShaderModule(dev, &ci, NULL, &fs) == VK_SUCCESS);
    VkRenderPass rp = VK_NULL_HANDLE;
    if (legacy) {
        VkAttachmentDescription ad = { .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
        VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp };
        assert(vkCreateRenderPass(dev, &rpi, NULL, &rp) == VK_SUCCESS);
    }
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = g_vk->empty_layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    d.vertex_spv = cvs, d.vertex_spv_bytes = sizeof cvs, d.fragment_spv = cfs, d.fragment_spv_bytes = sizeof cfs, d.caps_enabled = g_vk->caps_enabled;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    d.vertex_entry_point = d.fragment_entry_point = "main";
    if (legacy) d.render_pass = (uint64_t)(uintptr_t)rp, d.subpass = 0;

    VkPipeline pa = VK_NULL_HANDLE, pb = VK_NULL_HANDLE; uint32_t var = 99;
    lagfx_pipeline_test_fail_original = 0;
    assert(lagfx_pipeline_build_fallback(dev, &d, &pa, &var) == LAGFX_OK && pa && var == 0);       /* A: the original modules */
    lagfx_pipeline_test_fail_original = 1; var = 99;
    assert(lagfx_pipeline_build_fallback(dev, &d, &pb, &var) == LAGFX_OK && pb && var == 1);       /* B: the fallback's stripped modules */
    lagfx_pipeline_test_fail_original = 0;
    assert(pa != pb);

    uint32_t ia[W * H], ib[W * H];
    draw(pa, rp, legacy, ia); draw(pb, rp, legacy, ib);
    int red = 0, blue = 0;
    for (int i = 0; i < W * H; i++) { if (ia[i] == 0xFFFF0000u) red++; else if (ia[i] == 0xFF0000FFu) blue++; }
    assert(red > 0 && blue > 0 && red + blue == W * H);              /* really drawn (red triangle) over the blue clear: not an all-clear image */
    assert(memcmp(ia, ib, sizeof ia) == 0);                           /* the fallback pipeline renders every pixel identically */
    printf("PASS: fallback pipeline draws pixel-identical (%s path): %d triangle pixels, %d clear pixels\n", legacy ? "legacy VkRenderPass" : "dynamic rendering", red, blue);

    vkDestroyPipeline(dev, pa, NULL); vkDestroyPipeline(dev, pb, NULL);
    if (rp) vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
}

int main(void) {
    setenv("LAGFX_PIPELINE_MATRIX", "1", 1);   /* the diagnostics that build other pipelines through lagfx_pipeline_build are ON for the whole run */
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    dd.shell.opaque = (void *)0xfeedu; dd.shell.create_task = cb_create_task; dd.shell.destroy_task = cb_destroy_task; dd.shell.map_memory = cb_map; dd.shell.unmap_memory = cb_unmap;
    dd.shell.read_memory = rd; dd.shell.write_memory = wr; dd.shell.raise_interrupt = irq; dd.thread_count = 1;
    char *err = NULL; lagfx_device_t *dv = lagfx_device_new(&dd, &err);
    if (!dv || !dv->vk || !dv->vk->initialized) { puts("SKIP: no Vulkan device"); return 77; }
    g_vk = dv->vk;
    run_path(true);                                   /* the S8 path */
    if (g_vk->have_dynamic_rendering) run_path(false); else puts("NOTE: device without dynamic rendering: only the legacy path ran");

    /* guard: after a failed build with the matrix diagnostics on, the thread's last result is the failure. NOT discriminating: with the restore line removed this still passes (no matrix row
     * ends on a success for this failure on lavapipe), so the restore itself is verified by reading the code only. */
    {   lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
        VkShaderModule vs, fs; VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
        assert(vkCreateShaderModule(g_vk->device, &ci, NULL, &vs) == VK_SUCCESS); ci.codeSize = sizeof k_fs_spv, ci.pCode = k_fs_spv; assert(vkCreateShaderModule(g_vk->device, &ci, NULL, &fs) == VK_SUCCESS);
        d.vertex_shader = vs, d.fragment_shader = fs, d.layout = g_vk->empty_layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
        d.vertex_spv = k_vs_spv, d.vertex_spv_bytes = sizeof k_vs_spv, d.fragment_spv = k_fs_spv, d.fragment_spv_bytes = sizeof k_fs_spv, d.caps_enabled = g_vk->caps_enabled;
        d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8; d.vertex_entry_point = "main"; d.fragment_entry_point = "nonexistent";   /* only the FS entry is wrong: the matrix row with a stub FS BUILDS (g_last_vr = VK_SUCCESS inside the diagnostics) */
        VkPipeline p = VK_NULL_HANDLE;
        assert(lagfx_pipeline_build(g_vk->device, &d, &p) == LAGFX_ERR_BACKEND && !p);
        assert(lagfx_pipeline_build_last_result() != VK_SUCCESS);
        vkDestroyShaderModule(g_vk->device, vs, NULL); vkDestroyShaderModule(g_vk->device, fs, NULL);
        printf("PASS: guard only: last result after a failed build with matrix on = %d (does not prove the restore)\n", lagfx_pipeline_build_last_result());
    }
    lagfx_device_free(dv);
    return 0;
}
