/* SPDX-License-Identifier: MIT */
/* Common rendering interface with two backends:
 *   LAGFX_RENDER_DYNAMIC  vkCmdBeginRendering / vkCmdEndRendering (Vulkan 1.3 or VK_KHR_dynamic_rendering) -- the default wherever the device has it;
 *   LAGFX_RENDER_LEGACY   VkRenderPass + VkFramebuffer + vkCmdBeginRenderPass / vkCmdEndRenderPass (Vulkan 1.0), for a device without dynamic rendering.
 * LAGFX_VK_FORCE_LEGACY_RENDERPASS=1 selects LEGACY on a device that has both (test seam: exercises the legacy path on lavapipe / the S26).
 *
 * The general entry point is lagfx_render_desc_t: up to LAGFX_RENDER_MAX_COLOR colour attachments plus an optional depth/stencil attachment, each with its own sample count.
 * The single-colour lagfx_render_color_t functions are thin wrappers over it.
 *
 * The caller transitions every image to its attachment layout BEFORE render_begin, exactly as for dynamic rendering; a legacy pass keeps that layout
 * (initial == final), so the two backends are interchangeable at a call site. Pipelines used inside a legacy pass must be created against
 * lagfx_vk_render_pass_compat() for the same attachment formats / sample counts (render-pass COMPATIBILITY does not depend on load/store ops or layouts).
 *
 * An unused colour slot (format VK_FORMAT_UNDEFINED, no view) is kept in place (VK_ATTACHMENT_UNUSED in the subpass, no framebuffer view), so fragment output locations do not move.
 *
 * MSAA: a colour attachment with samples > 1 may carry a resolve view (see lagfx_render_attachment_t); pipelines are created against the compatible pass WITHOUT resolve (render-pass
 * compatibility ignores resolve attachments when both passes have a single subpass). Depth/stencil resolve is not here.
 *
 * NOT here yet: depth/stencil resolve, input attachments (framebuffer fetch), multiple subpasses. Call sites are converted one at a time; a call site that has not been converted must not run on a LEGACY device. */
#ifndef LAGFX_VULKAN_RENDER_PASS_H
#define LAGFX_VULKAN_RENDER_PASS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "libapplegfx-vulkan.h"

struct lagfx_vk_state;

#define LAGFX_RENDER_MAX_COLOR 8u

typedef enum { LAGFX_RENDER_DYNAMIC = 0, LAGFX_RENDER_LEGACY = 1 } lagfx_render_backend_t;

/* Why the legacy backend refused a pipeline shape or a pass (diagnostic counters, one per cause). */
typedef enum {
    LAGFX_LR_NONE = 0,
    LAGFX_LR_ARGS,        /* attachment arguments inconsistent (view without format, format without view, count out of range) */
    LAGFX_LR_MSAA,        /* sample count > 1: no resolve yet */
    LAGFX_LR_RESOLVE,     /* a colour or depth/stencil resolve attachment was asked for */
    LAGFX_LR_SLOT,        /* an unused colour slot is described inconsistently */
    LAGFX_LR_DS,          /* depth/stencil attachment without format / view, or depth and stencil on different views */
    LAGFX_LR_NO_COLOR,    /* pipeline: no colour slot has a format */
    LAGFX_LR_DS_MIXED,    /* pipeline: depth and stencil formats differ */
    LAGFX_LR_NO_DS,       /* pipeline: no_color without a depth/stencil format */
    LAGFX_LR_PASS,        /* pipeline: the compatible VkRenderPass could not be created (cache full / driver failure) */
    LAGFX_LR_BEGIN,       /* pass: render pass / framebuffer could not be created, or the device is lost */
    LAGFX_LR__COUNT
} lagfx_legacy_refuse_t;

typedef struct {
    VkImageView            view;
    VkFormat               format;
    VkSampleCountFlagBits  samples;        /* 0 is treated as 1 */
    VkImageLayout          layout;         /* the layout the image is in during the pass (COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL) */
    VkAttachmentLoadOp     load;           /* colour, or the depth aspect */
    VkAttachmentStoreOp    store;
    VkAttachmentLoadOp     stencil_load;   /* depth/stencil attachment only; ignored for colour */
    VkAttachmentStoreOp    stencil_store;
    VkClearValue           clear;          /* used when load (or stencil_load) == CLEAR: .color for colour, .depthStencil for depth/stencil */
    /* COLOUR attachments with samples > 1 only: a single-sample view the pass resolves into at its end (legacy: pResolveAttachments of the one subpass; dynamic: resolveImageView).
     * NULL = no resolve. The resolve image has the same format; resolve_layout is its layout during the pass (0 = COLOR_ATTACHMENT_OPTIMAL). resolve_mode (dynamic backend only; the
     * legacy resolve is implicit: averaged for float / normalised formats, sample 0 for integer formats, as VK_RESOLVE_MODE_AVERAGE / _SAMPLE_ZERO) is 0 = AVERAGE. */
    VkImageView            resolve_view;
    VkImageLayout          resolve_layout;
    VkResolveModeFlagBits  resolve_mode;
} lagfx_render_attachment_t;

typedef struct {
    uint32_t                   width, height;
    uint32_t                   color_count;                       /* 0 .. LAGFX_RENDER_MAX_COLOR; a slot with format UNDEFINED is unused */
    lagfx_render_attachment_t  color[LAGFX_RENDER_MAX_COLOR];
    bool                       has_depth_stencil;                 /* depth, stencil or both, decided by the format */
    lagfx_render_attachment_t  depth_stencil;
} lagfx_render_desc_t;

typedef struct {
    VkImageView            view;
    VkFormat               format;
    uint32_t               width, height;
    VkAttachmentLoadOp     load;     /* CLEAR / LOAD / DONT_CARE */
    VkAttachmentStoreOp    store;
    VkClearColorValue      clear;    /* used when load == CLEAR */
    VkImageLayout          layout;   /* the layout the image is in during the pass (COLOR_ATTACHMENT_OPTIMAL) */
} lagfx_render_color_t;

/* Outcome of lagfx_vk_render_forget_view(). DEFERRED: the device could not be proven idle, so the cached framebuffers of the view were KEPT; the caller must
 * not destroy the view (or the image / memory behind it) -- hand them to lagfx_vk_render_defer_target(). */
typedef enum { LAGFX_FORGET_OK = 0, LAGFX_FORGET_DEFERRED = 1 } lagfx_forget_t;

/* The backend of a device: fixed at lagfx_vk_init (LAGFX_VK_FORCE_LEGACY_RENDERPASS is read there, once); changing the variable later has no effect on an existing device. */
lagfx_render_backend_t lagfx_vk_render_backend_select(bool have_dynamic_rendering);
lagfx_render_backend_t lagfx_vk_render_backend(const struct lagfx_vk_state *vk);
/* A legacy render pass resolves implicitly: average for float / normalised colour formats, sample 0 for integer ones. A requested mode is honoured only when it is that one (same mapping as lagfx_msaa_resolve_mode). */
bool lagfx_legacy_resolve_mode_ok(VkFormat fmt, VkResolveModeFlagBits requested);

/* The (cached) VkRenderPass COMPATIBLE with a description (formats / sample counts / count; load/store/layout come from the pass actually begun); NULL handle on failure. */
VkRenderPass lagfx_vk_render_pass_compat(struct lagfx_vk_state *vk, const lagfx_render_desc_t *d);
/* Single-colour convenience: the compatible pass for one colour format. */
VkRenderPass lagfx_vk_render_pass_get(struct lagfx_vk_state *vk, VkFormat format, VkImageLayout layout);

lagfx_status_t lagfx_vk_render_begin_desc(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_desc_t *d);
lagfx_status_t lagfx_vk_render_begin(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_color_t *c);
void           lagfx_vk_render_end(struct lagfx_vk_state *vk, VkCommandBuffer cmd);

/* Drop the cached framebuffers that use this view. Call BEFORE destroying an image view that was rendered to through the legacy backend. Waits for the device to be
 * idle only if a framebuffer is actually destroyed. VK_ERROR_DEVICE_LOST counts as idle (no work can still be running on a lost device); any other wait failure
 * returns LAGFX_FORGET_DEFERRED and keeps the framebuffers. */
lagfx_forget_t lagfx_vk_render_forget_view(struct lagfx_vk_state *vk, VkImageView view);

/* Park a view / image / memory (any may be NULL) whose framebuffers could not be released; they are destroyed at shutdown, after the framebuffers. false: out of memory (the
 * handles are then leaked, never destroyed). */
bool lagfx_vk_render_defer_target(struct lagfx_vk_state *vk, VkImageView view, VkImage image, VkDeviceMemory memory);
unsigned lagfx_vk_render_deferred_count(const struct lagfx_vk_state *vk);

/* Test seam: make the next `n` vkDeviceWaitIdle calls made by forget_view report failure (VK_ERROR_OUT_OF_HOST_MEMORY), or one DEVICE_LOST when `lost`. */
void lagfx_vk_render_test_fail_waitidle(struct lagfx_vk_state *vk, unsigned n, bool lost);

void lagfx_vk_render_pass_shutdown(struct lagfx_vk_state *vk);

#endif
