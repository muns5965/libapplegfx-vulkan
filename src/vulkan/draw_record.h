/*
 * libapplegfx-vulkan — Vulkan draw command recording (Step 4)
 * src/vulkan/draw_record.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Encapsulates the begin/bind/draw/end/submit bundle for one-shot
 * draw execution. Called by compute_inner_ops.c when a draw fires.
 */

#ifndef LIBAPPLEGFX_VULKAN_DRAW_RECORD_H
#define LIBAPPLEGFX_VULKAN_DRAW_RECORD_H

#include "libapplegfx-vulkan.h"
#include "render_target.h"  /* lagfx_vk_render_target_t */

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#ifdef LAGFX_HAVE_VULKAN
/* Guest-declared per-draw viewport + scissor rects, framebuffer coords
 * (top-left origin, matching both Metal and Vulkan). A zero width disables
 * that rect (the draw falls back to the full render target). The rects are
 * clamped to the RT bounds at record time. */
typedef struct {
    uint32_t vp_x, vp_y, vp_w, vp_h;
    uint32_t sc_x, sc_y, sc_w, sc_h;
    float    vp_znear, vp_zfar;     /* depth range of the viewport, used only when vp_z_set (zero-initialised regions keep 0..1) */
    bool     vp_z_set;
    /* The arm64 executor path (arm64_exec_run.c): Metal's MTLViewport (doubles) and MTLScissorRect (integers) exactly as the guest recorded them, used INSTEAD of the integer
     * fields above when `metal` is set (the older compute-handler path keeps the integer behaviour). Unset (guest recorded none) = Metal's default, the whole render target.
     * A viewport may be fractional, begin at a negative origin and extend past the target (Vulkan allows that inside viewportBoundsRange: VkViewport [V]); the scissor is clamped
     * to the target. lim_* are the device's maxViewportDimensions[0..1] minimum and viewportBoundsRange (0 = unknown: no limit check). */
    bool     metal, m_vp_set, m_sc_set;
    double   m_vp[4];       /* originX, originY, width, height */
    uint64_t m_sc[4];       /* x, y, width, height */
    float    lim_dim, lim_lo, lim_hi;
} lagfx_draw_region_t;

/* Translate a `metal` region for a render target of rt_w x rt_h to the Vulkan viewport + scissor (Y flip by a negative height unless yflip is false). Returns false when the
 * guest viewport cannot be expressed in Vulkan (width or height not > 0, NaN / infinite, or outside the device limits): the viewport is then 1x1 and the scissor EMPTY, so the draw
 * rasterizes nothing (a degenerate Metal viewport covers no pixel [I]). *vp / *sc are always filled. */
bool lagfx_vk_region_to_vk(const lagfx_draw_region_t *region, uint32_t rt_w, uint32_t rt_h, bool yflip, VkViewport *vp, VkRect2D *sc);

/* Record and submit a single draw command into a one-shot command buffer.
 *
 * Parameters:
 *   vk            — lagfx_vk_state with cmd_pool, graphics_queue, device
 *   pipeline      — VkPipeline already built by Step 3
 *   rt            — render target whose image is the destination; used for
 *                   layout transitions and dynamic rendering attachment
 *   indexed       — true for indexed draw (vkCmdDrawIndexed), false for
 *                   unindexed (vkCmdDraw)
 *   vertex_count  — count for vkCmdDraw, or index_count for vkCmdDrawIndexed
 *   instance_count — always 1 for current step (non-instanced path)
 *   first_vertex  — base VertexIndex for vkCmdDraw
 *   first_instance — always 0 for current step
 *   index_buffer_ref — ignored when indexed=false; will be used in future
 *                      stages to bind the actual VkBuffer
 *
 * Returns LAGFX_OK on success, LAGFX_ERR_BACKEND on Vulkan failure.
 *
 * Anti-fab citations:
 *   - vkAllocateCommandBuffers: triangle-lavapipe-e2e.c:476-480
 *   - vkBeginCommandBuffer (ONE_TIME_SUBMIT): triangle-lavapipe-e2e.c:483-487
 *   - vkCmdBeginRendering (dynamic rendering): display.c:519-530, render_target.c:364-369
 *   - vkCmdBindPipeline: triangle-lavapipe-e2e.c:500
 *   - vkCmdDraw: triangle-lavapipe-e2e.c:501 (unindexed), display.c:527 (via cursor)
 *   - vkCmdEndRendering: display.c:528, render_target.c:369
 *   - vkQueueSubmit + vkWaitForFences: triangle-lavapipe-e2e.c:504-513
 */
lagfx_status_t lagfx_vk_draw_record_and_submit(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref);

/* Stage 85b — same, but binds `desc_set` (set 0) via `pipe_layout` before the
 * draw, for translated resource-using pipelines. Pass VK_NULL_HANDLE for both
 * to behave like the plain entry above. */
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
    VkBuffer vertex_buffer,    /* VK_NULL_HANDLE = no vertex-input binding */
    const lagfx_draw_region_t *region); /* NULL = full-RT viewport+scissor */

/* Building blocks of the one-shot draw above, usable when the caller records several draws into one command buffer (ARM64 exec path). Both
 * are the exact code the one-shot path runs. rt_to_color: barrier moving rt->layout to COLOR_ATTACHMENT_OPTIMAL (outside a render pass),
 * updates rt->layout. set_region: vkCmdSetViewport/Scissor for rt, region may be NULL (full target), Y-flipped unless LAGFX_DISABLE_YFLIP. */
void lagfx_vk_cmd_rt_to_color(VkCommandBuffer cb, lagfx_vk_render_target_t *rt);
bool lagfx_vk_cmd_set_region(VkCommandBuffer cb, const lagfx_vk_render_target_t *rt, const lagfx_draw_region_t *region);
#endif /* LAGFX_HAVE_VULKAN */

#endif /* LIBAPPLEGFX_VULKAN_DRAW_RECORD_H */
