/*
 * libapplegfx-vulkan — Vulkan draw command recording (Step 4)
 * src/vulkan/draw_record.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implementation of lagfx_vk_draw_record_and_submit. Encapsulates the
 * one-shot draw path: allocate CB, begin, transition image layout,
 * begin rendering (render_pass.c: dynamic rendering, or a legacy VkRenderPass on devices without it), bind pipeline, record draw,
 * end rendering, end CB, submit, wait for fence, free CB.
 */

#include "draw_record.h"
#include "command.h"
#include "render_pass.h"
#include "common/log.h"
#include "common/perf.h"
#include <math.h>
#include <stdlib.h>

#ifdef LAGFX_HAVE_VULKAN

/* Record (into an already-begun cb, outside a render pass) the barrier that moves rt to COLOR_ATTACHMENT_OPTIMAL and update rt->layout.
 * Extracted unchanged from lagfx_vk_draw_record_and_submit_bound so the ARM64 exec path shares it. */
void lagfx_vk_cmd_rt_to_color(VkCommandBuffer cb, lagfx_vk_render_target_t *rt) {
    /* Transition render target to COLOR_ATTACHMENT_OPTIMAL for rendering */
    VkAccessFlags src_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (rt->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        /* B10: a prior draw left the RT in color-attachment layout — the
         * WAW hazard against that prior color write must be covered, or a
         * real GPU (lavapipe hides it) shows ordering artifacts. */
        src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = rt->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

lagfx_vp_status_t lagfx_vk_region_to_vk(const lagfx_draw_region_t *region, uint32_t rt_w, uint32_t rt_h, bool yflip, VkViewport *vp, VkRect2D *sc) {
    lagfx_vp_status_t st = LAGFX_VP_OK;
    bool ok;
    double x = 0.0, y = 0.0, w = (double)rt_w, h = (double)rt_h;
    if (region && region->m_vp_set) { x = region->m_vp[0], y = region->m_vp[1], w = region->m_vp[2], h = region->m_vp[3]; }
    if (!(w > 0.0) || !(h > 0.0)) st = LAGFX_VP_EMPTY_METAL;   /* zero / negative / NaN size: no pixel is inside the viewport [I] (Metal's behaviour for a negative size is [N]) */
    else {
        bool beyond = false;   /* a number Vulkan cannot hold in the VkViewport at all, or a device limit */
        if (!isfinite(w) || !isfinite(h) || !isfinite(x) || !isfinite(y)) st = LAGFX_VP_INEXPRESSIBLE;   /* NaN / infinite origin, infinite size: no defined Metal meaning either, nothing to be faithful to */
        else if (fabs(x) > 3e38 || fabs(y) > 3e38 || w > 3e38 || h > 3e38) beyond = true;   /* does not fit a float */
        else if (region && region->lim_dim > 0.0f) {
            const double lo = region->lim_lo, hi = region->lim_hi, md = region->lim_dim;
            if (w > md || h > md || x < lo || x + w > hi || y < lo || y + h > hi) beyond = true;   /* VUID-VkViewport-width-01771 / -01773 / -x-01774 / -x-01232 / -y-01775.. -y-01233 */
        }
        if (beyond) {
            /* Metal accepts this viewport, Vulkan cannot express it. Fragments only exist inside the viewport rectangle, so when that rectangle does not meet the render target nothing would be
             * drawn in Metal either: the (empty-scissor) skip is then EXACT. When it does meet the target the drawn pixels depend on the viewport TRANSFORM (x + w * (ndc + 1) / 2), and every
             * Vulkan-expressible viewport / scissor pair maps NDC differently, so no fallback is equivalent (docs/spec-audit.md S05): skipped and counted separately. */
            const bool meets = x < (double)rt_w && x + w > 0.0 && y < (double)rt_h && y + h > 0.0;
            st = meets ? LAGFX_VP_INEXPRESSIBLE : LAGFX_VP_EMPTY_METAL;
        }
    }
    ok = st == LAGFX_VP_OK;
    const float rx = ok ? (float)x : 0.0f, ry = ok ? (float)y : 0.0f, rw = ok ? (float)w : 1.0f, rh = ok ? (float)h : 1.0f;
    *vp = (VkViewport){ .x = rx, .y = yflip ? ry + rh : ry, .width = rw, .height = yflip ? -rh : rh, .minDepth = region && region->vp_z_set ? region->vp_znear : 0.0f, .maxDepth = region && region->vp_z_set ? region->vp_zfar : 1.0f };
    uint64_t sx = 0, sy = 0, sw = rt_w, sh = rt_h;
    if (region && region->m_sc_set) sx = region->m_sc[0], sy = region->m_sc[1], sw = region->m_sc[2], sh = region->m_sc[3];
    const uint64_t x1 = sx > UINT64_MAX - sw ? UINT64_MAX : sx + sw, y1 = sy > UINT64_MAX - sh ? UINT64_MAX : sy + sh;   /* far edge, saturating */
    const uint64_t cx0 = sx < rt_w ? sx : rt_w, cy0 = sy < rt_h ? sy : rt_h, cx1 = x1 < rt_w ? x1 : rt_w, cy1 = y1 < rt_h ? y1 : rt_h;   /* clamp to the target */
    *sc = (VkRect2D){ .offset = { (int32_t)cx0, (int32_t)cy0 }, .extent = { (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0) } };
    if (!ok) *sc = (VkRect2D){ .offset = { 0, 0 }, .extent = { 0, 0 } };
    return st;
}

/* Record dynamic viewport + scissor for rt (Y-flip, region clamp: see body). Extracted unchanged from the one-shot draw path; a `metal` region goes through
 * lagfx_vk_region_to_vk instead. Returns LAGFX_VP_OK except for a metal region whose viewport is not drawable (an empty scissor was recorded; see lagfx_vp_status_t). */
lagfx_vp_status_t lagfx_vk_cmd_set_region(VkCommandBuffer cb, const lagfx_vk_render_target_t *rt, const lagfx_draw_region_t *region) {
    if (region && region->metal) {
        VkViewport mvp; VkRect2D msc;
        const lagfx_vp_status_t ok = lagfx_vk_region_to_vk(region, rt->width, rt->height, getenv("LAGFX_DISABLE_YFLIP") == NULL, &mvp, &msc);
        vkCmdSetViewport(cb, 0, 1, &mvp);
        vkCmdSetScissor(cb, 0, 1, &msc);
        return ok;
    }
    /* Dynamic viewport/scissor sized to THIS render target (the pipeline no
     * longer bakes 1920×1080 — that mis-scaled the 1280×1024 per-pass draws).
     * Y-FLIP via negative height (Metal Y-up → Vulkan Y-down); kill-switch
     * LAGFX_DISABLE_YFLIP. */
    {
        bool yflip = (getenv("LAGFX_DISABLE_YFLIP") == NULL);
        /* Guest region (KICKOFF-shading-throughput): CA transforms vertices
         * for the DECLARED viewport rect (pixel->NDC matrix = 2/vp_w etc.)
         * and clips with the declared scissor. Mapping NDC onto the full RT
         * stretched small layers across the whole target, and the missing
         * scissor made full-target wash triangles shade EVERY RT pixel
         * (2M-8M px of the 73KB Xgc ubershader on lavapipe = the 20-30s
         * "runaway" draws). Clamp both rects to the RT; zero width = unset. */
        uint32_t vx = 0, vy = 0, vw = rt->width, vh = rt->height;
        uint32_t sx = 0, sy = 0, sw = rt->width, sh = rt->height;
        if (region && region->vp_w && region->vp_x < rt->width
            && region->vp_y < rt->height) {
            vx = region->vp_x; vy = region->vp_y;
            vw = region->vp_w; vh = region->vp_h;
            if (vx + vw > rt->width)  vw = rt->width - vx;
            if (vy + vh > rt->height) vh = rt->height - vy;
        }
        if (region && region->sc_w && region->sc_x < rt->width
            && region->sc_y < rt->height) {
            sx = region->sc_x; sy = region->sc_y;
            sw = region->sc_w; sh = region->sc_h;
            if (sx + sw > rt->width)  sw = rt->width - sx;
            if (sy + sh > rt->height) sh = rt->height - sy;
        }
        VkViewport dvp = {
            .x = (float)vx, .y = yflip ? (float)(vy + vh) : (float)vy,
            .width = (float)vw,
            .height = yflip ? -(float)vh : (float)vh,
            .minDepth = region && region->vp_z_set ? region->vp_znear : 0.0f, .maxDepth = region && region->vp_z_set ? region->vp_zfar : 1.0f,
        };
        VkRect2D dsc = { .offset = {(int32_t)sx, (int32_t)sy},
                         .extent = { sw, sh } };
        vkCmdSetViewport(cb, 0, 1, &dvp);
        vkCmdSetScissor(cb, 0, 1, &dsc);
    }
    return LAGFX_VP_OK;
}

/* Thin wrapper: the original substitute-path entry (no descriptor set). */
lagfx_status_t lagfx_vk_draw_record_and_submit(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref) {
    return lagfx_vk_draw_record_and_submit_bound(
        vk, pipeline, VK_NULL_HANDLE, VK_NULL_HANDLE, rt, indexed,
        vertex_count, instance_count, first_vertex, first_instance,
        index_buffer_ref, VK_NULL_HANDLE, NULL);
}

lagfx_status_t lagfx_vk_draw_record_and_submit_bound(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    VkPipelineLayout pipe_layout,
    VkDescriptorSet desc_set,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref,
    VkBuffer vertex_buffer,
    const lagfx_draw_region_t *region) {

    (void)index_buffer_ref;  /* TODO: bind actual VkBuffer in future stage */
    
    if (!vk || !vk->initialized || vk->device == VK_NULL_HANDLE
        || vk->graphics_queue == VK_NULL_HANDLE || vk->cmd_pool == VK_NULL_HANDLE) {
        LAGFX_ERR("draw_record_and_submit: Vulkan state not fully initialized");
        return LAGFX_ERR_BACKEND;
    }
    
    if (pipeline == VK_NULL_HANDLE || rt == NULL || rt->image == VK_NULL_HANDLE) {
        LAGFX_ERR("draw_record_and_submit: pipeline=%p rt=%p rt->image=%p",
                  (void *)pipeline, (void *)rt, (void *)rt->image);
        return LAGFX_ERR_INVALID_ARG;
    }
    
    /* Allocate one-shot command buffer */
    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }
    
    /* Begin recording (one-time submit pattern from triangle-lavapipe-e2e.c:483-487) */
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkBeginCommandBuffer failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* rt->layout describes what the GPU will see once this command buffer has run; it is rewritten by the barrier recorded below, so every failure before the
     * submit is accepted restores it (the barrier never executed). */
    const VkImageLayout prev_layout = rt->layout;
    lagfx_vk_cmd_rt_to_color(cb, rt);

    lagfx_render_color_t color_att = {
        .view = rt->view, .format = rt->format, .width = rt->width, .height = rt->height,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .load = VK_ATTACHMENT_LOAD_OP_LOAD,  /* preserve existing content */
        .store = VK_ATTACHMENT_STORE_OP_STORE,
    };

    /* M1 diagnostic (env-gated): clear to a distinctive teal before this draw.
     * If the isolated translated frame turns teal, the zero-resource-data draws
     * are DEGENERATE (positions collapse → nothing rasterizes) and a correct
     * clear/non-zero data would be visible — i.e. a non-black frame is reachable
     * within M1. If it stays black, the draws cover the RT with black geometry
     * and visible content genuinely needs M2 resource data. Decisive either way. */
    if (getenv("LAGFX_TEST_CLEAR") != NULL) {
        color_att.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.clear.float32[0] = 0.0f;
        color_att.clear.float32[1] = 0.6f;
        color_att.clear.float32[2] = 0.6f;
        color_att.clear.float32[3] = 1.0f;
    }

    st = lagfx_vk_render_begin(vk, cb, &color_att);
    if (st != LAGFX_OK) {
        LAGFX_ERR("draw_record_and_submit: render_begin failed (%d)", (int)st);
        rt->layout = prev_layout;
        lagfx_vk_cmdbuf_free(vk, cb);
        return st;
    }
    
    /* Bind the pipeline (pattern from triangle-lavapipe-e2e.c:500) */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    lagfx_vk_cmd_set_region(cb, rt, region);

    /* Stage 85b: bind the descriptor set for translated resource-using
     * pipelines (set 0 with the guest's buffers). NULL for the substitute path. */
    if (desc_set != VK_NULL_HANDLE && pipe_layout != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout, 0, 1, &desc_set, 0, NULL);
    }

    /* Vertex-input path: bind the guest's vertex buffer at binding 0 so the
     * pipeline's vertex attributes are fed real positions. The pipeline was
     * built with a matching non-empty vertex-input state (pipeline_build). */
    if (vertex_buffer != VK_NULL_HANDLE) {
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer, &voff);
    }

    /* Record draw command */
    if (indexed) {
        /* Indexed draw — vertex_count holds index_count per compute_inner_ops.c conventions */
        LAGFX_LOG("draw_record_and_submit: indexed draw indices=%u instances=%u baseVertex=%d",
                  vertex_count, instance_count, first_vertex);
        vkCmdDrawIndexed(cb, vertex_count, instance_count, 0, first_vertex, first_instance);
    } else {
        /* Unindexed draw */
        LAGFX_LOG("draw_record_and_submit: unindexed draw vertices=%u instances=%u baseVertex=%d",
                  vertex_count, instance_count, first_vertex);
        vkCmdDraw(cb, vertex_count, instance_count, first_vertex, first_instance);
    }
    
    /* End rendering */
    lagfx_vk_render_end(vk, cb);
    
    /* End command buffer (pattern from triangle-lavapipe-e2e.c:503) */
    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkEndCommandBuffer failed (%d)", (int)vr);
        rt->layout = prev_layout;
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Submit to queue with fence */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkCreateFence failed (%d)", (int)vr);
        rt->layout = prev_layout;
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    /* M3/perf: time the submit→fence-complete window — the per-draw
     * synchronous GPU round-trip (B3). At N draws/frame this is the prime
     * throughput suspect; aggregated per frame at readback. */
    uint64_t perf_t0 = getenv("LAGFX_PERF") ? lagfx_now_ns() : 0u;
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkQueueSubmit failed (%d)", (int)vr);
        rt->layout = prev_layout;   /* not accepted by the queue: the barrier never ran */
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Wait for completion. 30 s (was 1 s): with real rasterization the
     * big CA fixed-function draws legitimately shade for seconds on
     * lavapipe; at 1 s the common case became timeout → ERR + unbounded
     * vkDeviceWaitIdle, serializing the drain for minutes and stalling
     * the guest's GPU stack (KICKOFF-shading-throughput). Env-tunable:
     * LAGFX_DRAW_FENCE_MS. */
    uint64_t timeout_ns = 30ull * 1000ull * 1000ull * 1000ull;
    {
        const char *fm = getenv("LAGFX_DRAW_FENCE_MS");
        if (fm && fm[0]) {
            unsigned long ms = strtoul(fm, NULL, 10);
            if (ms) timeout_ns = (uint64_t)ms * 1000000ull;
        }
    }
    uint64_t fence_t0 = lagfx_now_ns();
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    {
        uint64_t waited_ms = (lagfx_now_ns() - fence_t0) / 1000000ull;
        vk->last_draw_wait_ms = (uint32_t)waited_ms;
        if (waited_ms > 1000u)
            LAGFX_LOG("draw_record_and_submit: SLOW draw %llums "
                      "(indexed=%d count=%u)",
                      (unsigned long long)waited_ms, indexed ? 1 : 0,
                      vertex_count);
    }
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkWaitForFences failed/timeout (%d)", (int)vr);
        /* The submission is STILL RUNNING on the lavapipe worker —
         * destroying the fence + freeing the command buffer here was a
         * use-after-free (SIGSEGV inside libvulkan_lvp at the next
         * present; live-observed when a long-running dispatched shader
         * exceeded the fence timeout). Block until the device is idle —
         * bounded now that dispatched control flow carries a hard
         * iteration cap — then clean up safely. */
        vkDeviceWaitIdle(vk->device);
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Cleanup */
    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    if (perf_t0) {
        vk->perf_frame_draw_ns += lagfx_now_ns() - perf_t0;
        vk->perf_frame_draws   += 1u;
    }

    LAGFX_LOG("draw_record_and_submit: draw completed successfully (indexed=%d count=%u)",
              indexed ? 1 : 0, vertex_count);
    return LAGFX_OK;
}

#endif /* LAGFX_HAVE_VULKAN */
/* No vulkan-disabled stub — the function declaration is also gated in
 * draw_record.h, so callers only see it when LAGFX_HAVE_VULKAN is on. */
