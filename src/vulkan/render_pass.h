/* SPDX-License-Identifier: MIT */
/* Common single-colour-attachment rendering interface with two backends:
 *   LAGFX_RENDER_DYNAMIC  vkCmdBeginRendering / vkCmdEndRendering (Vulkan 1.3 or VK_KHR_dynamic_rendering) -- the default wherever the device has it;
 *   LAGFX_RENDER_LEGACY   VkRenderPass + VkFramebuffer + vkCmdBeginRenderPass / vkCmdEndRenderPass (Vulkan 1.0), for a device without dynamic rendering.
 * LAGFX_VK_FORCE_LEGACY_RENDERPASS=1 selects LEGACY on a device that has both (test seam: exercises the legacy path on lavapipe / the S26).
 *
 * The caller transitions the image to `layout` (COLOR_ATTACHMENT_OPTIMAL) BEFORE render_begin, exactly as for dynamic rendering; a legacy pass keeps that layout
 * (initial == final), so the two backends are interchangeable at a call site. Pipelines used inside a legacy pass must be created against
 * lagfx_vk_render_pass_get() for the same colour format (render-pass COMPATIBILITY only depends on format / sample count, not on load/store ops).
 *
 * NOT here yet: depth/stencil, multiple attachments, MSAA resolve, input attachments (framebuffer fetch). Call sites are converted one at a time; a call site that
 * has not been converted must not run on a LEGACY device. */
#ifndef LAGFX_VULKAN_RENDER_PASS_H
#define LAGFX_VULKAN_RENDER_PASS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "libapplegfx-vulkan.h"

struct lagfx_vk_state;

typedef enum { LAGFX_RENDER_DYNAMIC = 0, LAGFX_RENDER_LEGACY = 1 } lagfx_render_backend_t;

typedef struct {
    VkImageView            view;
    VkFormat               format;
    uint32_t               width, height;
    VkAttachmentLoadOp     load;     /* CLEAR / LOAD / DONT_CARE */
    VkAttachmentStoreOp    store;
    VkClearColorValue      clear;    /* used when load == CLEAR */
    VkImageLayout          layout;   /* the layout the image is in during the pass (COLOR_ATTACHMENT_OPTIMAL) */
} lagfx_render_color_t;

lagfx_render_backend_t lagfx_vk_render_backend(const struct lagfx_vk_state *vk);

/* The (cached) VkRenderPass for one colour format; NULL handle on failure. Used for pipeline creation and by render_begin. */
VkRenderPass lagfx_vk_render_pass_get(struct lagfx_vk_state *vk, VkFormat format, VkImageLayout layout);

lagfx_status_t lagfx_vk_render_begin(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_color_t *c);
void           lagfx_vk_render_end(struct lagfx_vk_state *vk, VkCommandBuffer cmd);

/* Drop the cached framebuffers that use this view. Call BEFORE destroying an image view that was rendered to through the legacy backend (waits for the device
 * to be idle only if a framebuffer is actually destroyed). */
void lagfx_vk_render_forget_view(struct lagfx_vk_state *vk, VkImageView view);

void lagfx_vk_render_pass_shutdown(struct lagfx_vk_state *vk);

#endif
