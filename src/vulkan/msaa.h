/*
 * libapplegfx-vulkan — Vulkan multisample colour path: sample-count choice, multisampled target + resolve target, Metal store action -> dynamic-rendering attachment.
 * src/vulkan/msaa.h
 *
 * SPDX-License-Identifier: MIT
 *
 * STATUS: Vulkan-side machinery behind an explicit internal API, exercised by tests/vulkan-msaa-unit.c. It is NOT wired to the PVG guest protocol: the dumps in hand
 * (/tmp/vm2/dump, /tmp/x231501/dump) hold no raw pass descriptor or texture descriptor of a multisampled pass (see docs/metal-vulkan-compatibility.md, "MSAA evidence").
 * The arm64 exec mirror therefore still refuses a texture whose descriptor says samples > 1 and counts it (diag2.exec.fmt.msaa_refused); nothing here guesses a wire field.
 * Whoever wires it must first read the sample count / store action / resolve texture from a real capture.
 *
 * Official basis. Vulkan: VkRenderingAttachmentInfo::resolveMode / resolveImageView / resolveImageLayout (dynamic rendering), "resolveMode must be a mode the format
 * supports; for an integer format it must be VK_RESOLVE_MODE_SAMPLE_ZERO_BIT"; the pipeline's rasterizationSamples must equal the sample count of the attachments it draws
 * to; a resolve attachment is single-sample and has the format of the attachment it resolves. Metal: MTLStoreAction (MTLStoreActionDontCare 0, Store 1, MultisampleResolve 2,
 * StoreAndMultisampleResolve 3, Unknown 4, CustomSampleDepthStore 5), MTLRenderPassColorAttachmentDescriptor.resolveTexture, MTLTextureDescriptor.sampleCount,
 * -[MTLDevice supportsTextureSampleCount:].
 */
#ifndef LAGFX_VULKAN_MSAA_H
#define LAGFX_VULKAN_MSAA_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "format_caps.h"

struct lagfx_vk_state;

/* Pick the sample count: the largest power of two <= `requested` (1..16) that is in `supported` (a VkSampleCountFlags mask). Always at least 1. *clamped (optional) = the
 * result differs from the request (the caller counts it: never a silent downgrade). requested 0 is treated as 1. */
VkSampleCountFlagBits lagfx_msaa_choose(VkSampleCountFlags supported, uint32_t requested, bool *clamped);

/* Metal MTLStoreAction of a colour attachment -> Vulkan. Returns false for a value this table does not know (4 Unknown, 5 CustomSampleDepthStore, anything else): the caller
 * must count it and keep the safe default (STORE, no resolve). *resolve = a resolve is requested; it needs a resolve attachment: without one (`have_resolve_target` false) a
 * resolve action cannot be honoured, which is reported as false too (counted by the caller) and the multisampled content is stored instead. */
bool lagfx_msaa_store_map(uint32_t mtl_store_action, bool have_resolve_target, VkAttachmentStoreOp *store, bool *resolve);

/* VK_RESOLVE_MODE_* for a colour format: SAMPLE_ZERO for the integer formats (required by Vulkan), AVERAGE for the others (the Metal resolve of a float / normalized format
 * averages the samples). */
VkResolveModeFlagBits lagfx_msaa_resolve_mode(VkFormat fmt);

typedef struct {
    VkFormat fmt;
    uint32_t w, h;
    VkSampleCountFlagBits samples;      /* granted (may be lower than requested) */
    uint32_t requested;                 /* what the caller asked for */
    VkImage ms_image, resolve_image;    /* resolve_image is VK_NULL_HANDLE when no resolve target was asked for or samples == 1 */
    VkImageView ms_view, resolve_view;
    VkDeviceMemory ms_mem, resolve_mem;
    VkImageUsageFlags ms_usage, resolve_usage;
    bool clamped;                       /* samples < requested */
} lagfx_msaa_target_t;

enum { LAGFX_MSAA_OK = 0, LAGFX_MSAA_NO_FORMAT, LAGFX_MSAA_VK_FAIL };

/* Create the multisampled colour image (+ view) and, when want_resolve and the granted count is > 1, the single-sample resolve image (+ view; COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST |
 * SAMPLED where the format offers them). Capability comes from `be` (production backend: lagfx_fmtcap_device_backend; tests inject one). When the device cannot render to the
 * format at all, or not with any sample count > 1 for the requested usage, the result is the single-sample image with t->samples == 1 and t->clamped set (the caller counts it);
 * only a format that cannot be a colour attachment at all returns LAGFX_MSAA_NO_FORMAT. */
int lagfx_msaa_target_create(struct lagfx_vk_state *vk, const lagfx_fmtcap_backend_t *be, VkFormat fmt, uint32_t w, uint32_t h, uint32_t requested_samples, bool want_resolve, lagfx_msaa_target_t *t);
void lagfx_msaa_target_destroy(struct lagfx_vk_state *vk, lagfx_msaa_target_t *t);

/* Fill one colour VkRenderingAttachmentInfo for `t`: view = the multisampled view; with a resolve: resolveMode / resolveImageView / resolveImageLayout
 * (COLOR_ATTACHMENT_OPTIMAL). storeOp comes from the Metal store action through lagfx_msaa_store_map. Returns that function's result (false = unknown action, the safe
 * default STORE without resolve was used). */
bool lagfx_msaa_color_attachment(const lagfx_msaa_target_t *t, VkAttachmentLoadOp load, const VkClearColorValue *clear, uint32_t mtl_store_action, VkRenderingAttachmentInfo *out);

/* The device-info MSAASamples value (guest supportsRasterSampleCount:N = value >= N): `advertised` lowered to the largest count the backend reports for the colour formats of the
 * mirror (B8G8R8A8_UNORM and R16G16B16A16_SFLOAT as colour attachments; the smaller of the two maxima), never below 1. *clamped |= the value changed. */
uint32_t lagfx_msaa_devinfo_max(const lagfx_fmtcap_backend_t *be, uint32_t advertised, bool *clamped);

#endif
