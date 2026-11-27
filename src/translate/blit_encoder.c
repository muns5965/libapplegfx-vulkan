/*
 * libapplegfx-vulkan — Blit-encoder Vulkan translation layer (M5 phase 1)
 * src/translate/blit_encoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implements real Vulkan translation for critical blit opcodes that are
 * blocking M5 stage 20%. This is the minimum viable set to produce visible
 * pixels: FillTextureWithColor (clear) and CopyFromTextureToTexture (surface copies).
 *
 * Implementation notes:
 *   - Uses staging buffers for host-visible access (matches display.c patterns)
 *   - Fails gracefully on missing resources (fail-open like render encoder)
 *   - Minimal validation; production will need tighter error handling
 *
 * Phase 1 target (2026-05-07): Get something visible on screen.
 * Future phases: Complete remaining blit ops, optimize paths.
 */

#include "blit_encoder.h"

#include "../common/log.h"
#include "../device.h"
#include "../display.h"
#include "../vulkan/command.h"
#include "../vulkan/format_caps.h"
#include "../vulkan/iosurface.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* ================================================================
 * Helper lookup — find memory type index (inlined from display.c pattern)
 * ================================================================ */
#ifdef LAGFX_HAVE_VULKAN

static uint32_t blit_find_memory_type(VkPhysicalDevice phys,
                                      uint32_t typeBits,
                                      VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

/* ================================================================
 * Internal helpers — staging buffer management
 * ================================================================ */

static VkBuffer create_staging_buffer(struct lagfx_vk_state *vk_impl,
                                      size_t size) {
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult vr = vkCreateBuffer(vk_impl->device, &bci, NULL, &buf);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: create staging buffer failed (%zu bytes, %d)",
                   size, (int)vr);
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(vk_impl->device, buf, &mem_reqs);

    VkMemoryAllocateInfo mai = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = blit_find_memory_type(vk_impl->phys_device,
                                                    mem_reqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };

    VkDeviceMemory mem = VK_NULL_HANDLE;
    vr = vkAllocateMemory(vk_impl->device, &mai, NULL, &mem);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: allocate staging memory failed (%d)", (int)vr);
        vkDestroyBuffer(vk_impl->device, buf, NULL);
        return VK_NULL_HANDLE;
    }

    vr = vkBindBufferMemory(vk_impl->device, buf, mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: bind staging memory failed (%d)", (int)vr);
        vkFreeMemory(vk_impl->device, mem, NULL);
        vkDestroyBuffer(vk_impl->device, buf, NULL);
        return VK_NULL_HANDLE;
    }

    return buf;
}

static void destroy_staging_buffer(struct lagfx_vk_state *vk_impl,
                                   VkBuffer buf,
                                   VkDeviceMemory mem) {
    if (buf != VK_NULL_HANDLE) vkDestroyBuffer(vk_impl->device, buf, NULL);
    if (mem != VK_NULL_HANDLE) vkFreeMemory(vk_impl->device, mem, NULL);
}

#endif /* LAGFX_HAVE_VULKAN */

/* ================================================================
 * 0x141 FillTextureWithColor — region fill that keeps the rest of the image
 *
 * Payload (blit_inner_ops.c op_fill_texture_with_color; x86-era guess, NOT verified against a guest capture [I]; the ARM64 wire layout of tag 321 is 100 B total and is not decoded):
 *   +0x00 u32 texture_ref   +0x04 u32 level   +0x08 u32 slice   +0x0c MTLOrigin (3 x u32)   +0x18 MTLSize (3 x u32)   +0x24 4 x f64 colour (r g b a)   +0x44 u32 MTLPixelFormat
 * (the body is declared 92 B: bytes +0x48..+0x5b are not decoded [I]).
 *
 * Behaviour (the spec text each rule rests on is [V], Metal's own behaviour where Metal is silent is [N]):
 *   - the image's REAL current layout (*layout, kept by the owner: lagfx_vk_iosurface_t::layout) is the barrier's old layout; afterwards the image is back in that layout.
 *     UNDEFINED / PREINITIALIZED (no content yet) is moved to GENERAL (the only layout every later use accepts). Never UNDEFINED over live contents (that discards them).
 *   - a region covering the whole subresource (level extent; for a 3D image all depth planes): vkCmdClearColorImage on exactly {level, layer} (colour aspect).
 *   - a smaller region: a temporary image of the region's size and the SAME format is cleared (same clear conversion as the whole-subresource path: lagfx_fmtcap_clear_color)
 *     and copied into the region with vkCmdCopyImage; every texel outside the region is untouched. Why this path: it needs no per-format byte packing (packed 10-bit / 2-bit
 *     formats, half floats, integer formats and sRGB all convert exactly as for the full clear), and vkCmdClearAttachments would need a colour-attachment usage the image may not have.
 *     Cost: one small image per partial fill (command is synchronous: fence wait, as before).
 *   - [N] out-of-range level / slice / region (x + w > level width ... ) : Metal validation rejects it and defines no result; this function REFUSES (LAGFX_ERR_INVALID_ARG, nothing
 *     recorded, counter fill_oob / fill_bad_subres) instead of clipping, because a clipped fill would apply a different rectangle than the guest asked for.
 *   - a zero width / height (/ depth of a 3D image) is an empty region: success, nothing recorded, counter fill_empty. A 2D image takes depth 0 as 1 [N].
 *   - depth / stencil, block-compressed and multi-planar formats cannot be colour-cleared (VUID-vkCmdClearColorImage-image-00007 / -aspectMask): refused, counter fill_format.
 * The target needs TRANSFER_DST usage (and TRANSFER_SRC is needed on the temporary image only; the format must allow both for the partial path, else fill_format).
 * ================================================================ */

#ifdef LAGFX_HAVE_VULKAN

static lagfx_blit_fill_stats_t g_fill_stats;
void lagfx_translate_blit_fill_stats_get(lagfx_blit_fill_stats_t *out) { if (out) *out = g_fill_stats; }
void lagfx_translate_blit_fill_stats_reset(void) { memset(&g_fill_stats, 0, sizeof g_fill_stats); }

static bool fill_format_clearable(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return false;
    if (f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT) return false;      /* depth / stencil */
    if (f == VK_FORMAT_S8_UINT) return false;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return false;   /* BC, ETC2, EAC, ASTC LDR */
    if ((uint32_t)f >= 1000054000u && (uint32_t)f <= 1000054007u) return false;            /* PVRTC */
    if ((uint32_t)f >= 1000066000u && (uint32_t)f <= 1000066013u) return false;            /* ASTC HDR */
    if ((uint32_t)f >= 1000156000u && (uint32_t)f <= 1000156033u) return false;            /* multi-planar / Y'CbCr */
    return true;
}

static void fill_barrier(VkCommandBuffer cb, VkImage img, uint32_t level, uint32_t layer, VkImageLayout from, VkImageLayout to,
                         VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1 },
    };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

lagfx_status_t lagfx_translate_blit_fill_image_color(void *vk, const lagfx_blit_fill_target_t *t, VkImageLayout *layout,
                                                     uint32_t level, uint32_t slice, const MTLOrigin *origin, const MTLSize *size, const double color[4]) {
    struct lagfx_vk_state *vk_impl = (struct lagfx_vk_state *)vk;
    if (!vk || !t || !layout || !origin || !size || !color || t->image == VK_NULL_HANDLE) return LAGFX_ERR_INVALID_ARG;
    const bool is3d = t->type == VK_IMAGE_TYPE_3D;
    const uint32_t mips = t->mip_levels ? t->mip_levels : 1u, layers = t->array_layers ? t->array_layers : 1u;

    if (!fill_format_clearable(t->format)) { g_fill_stats.fill_format++; LAGFX_WARN("blit_fill_texture_color: format %d cannot be colour-cleared: refused", (int)t->format); return LAGFX_ERR_INVALID_ARG; }
    if (level >= mips || slice >= (is3d ? 1u : layers)) {
        g_fill_stats.fill_bad_subres++;
        LAGFX_WARN("blit_fill_texture_color: level %u / slice %u outside the image (%u levels, %u layers): refused", level, slice, mips, is3d ? 1u : layers);
        return LAGFX_ERR_INVALID_ARG;
    }
    uint32_t lw = t->width >> level, lh = t->height >> level, ld = is3d ? (t->depth >> level) : 1u;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (!ld) ld = 1;

    uint32_t rd = size->depth;
    if (!is3d && rd == 0) rd = 1;   /* [N] a 2D image has no depth: 0 is read as 1 */
    if (size->width == 0 || size->height == 0 || rd == 0) {
        g_fill_stats.fill_empty++;
        LAGFX_WARN("blit_fill_texture_color: empty region (%ux%ux%u)", size->width, size->height, size->depth);
        return LAGFX_OK;
    }
    if ((uint64_t)origin->x + size->width > lw || (uint64_t)origin->y + size->height > lh || (uint64_t)origin->z + rd > ld) {
        g_fill_stats.fill_oob++;
        LAGFX_WARN("blit_fill_texture_color: region origin (%u,%u,%u) size %ux%ux%u exceeds level %u extent %ux%ux%u: refused",
                   origin->x, origin->y, origin->z, size->width, size->height, rd, level, lw, lh, ld);
        return LAGFX_ERR_INVALID_ARG;
    }
    const bool full = origin->x == 0 && origin->y == 0 && origin->z == 0 && size->width == lw && size->height == lh && rd == ld;

    VkClearColorValue cv = {0};
    lagfx_fmtcap_clear_color(t->format, color, &cv);   /* the one documented conversion (integer formats use the int32 / uint32 member) */

    /* temporary same-format image for a partial fill */
    VkImage tmp = VK_NULL_HANDLE; VkDeviceMemory tmem = VK_NULL_HANDLE;
    if (!full) {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = t->format,
            .extent = { size->width, size->height, rd }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImageFormatProperties ifp;
        if (vkGetPhysicalDeviceImageFormatProperties(vk_impl->phys_device, ici.format, ici.imageType, ici.tiling, ici.usage, 0, &ifp) != VK_SUCCESS
            || ifp.maxExtent.width < ici.extent.width || ifp.maxExtent.height < ici.extent.height || ifp.maxExtent.depth < ici.extent.depth) {
            g_fill_stats.fill_format++;
            LAGFX_WARN("blit_fill_texture_color: format %d cannot hold a %ux%ux%u transfer image for a partial fill: refused", (int)t->format, size->width, size->height, rd);
            return LAGFX_ERR_INVALID_ARG;
        }
        if (vkCreateImage(vk_impl->device, &ici, NULL, &tmp) != VK_SUCCESS) { g_fill_stats.fill_failed++; return LAGFX_ERR_BACKEND; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk_impl->device, tmp, &mr);
        uint32_t mt = blit_find_memory_type(vk_impl->phys_device, mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
        if (mt == UINT32_MAX || vkAllocateMemory(vk_impl->device, &mai, NULL, &tmem) != VK_SUCCESS || vkBindImageMemory(vk_impl->device, tmp, tmem, 0) != VK_SUCCESS) {
            if (tmem != VK_NULL_HANDLE) vkFreeMemory(vk_impl->device, tmem, NULL);
            vkDestroyImage(vk_impl->device, tmp, NULL);
            g_fill_stats.fill_failed++;
            return LAGFX_ERR_BACKEND;
        }
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) goto cleanup_tmp;

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_fill_texture_color: vkBeginCommandBuffer failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        st = LAGFX_ERR_BACKEND;
        goto cleanup_tmp;
    }

    /* the real layout; an image with no content yet has nothing to preserve and ends in GENERAL */
    const VkImageLayout cur = *layout;
    const bool no_content = cur == VK_IMAGE_LAYOUT_UNDEFINED || cur == VK_IMAGE_LAYOUT_PREINITIALIZED;
    const VkImageLayout back = no_content ? VK_IMAGE_LAYOUT_GENERAL : cur;
    const VkImageLayout from = no_content ? VK_IMAGE_LAYOUT_UNDEFINED : cur;
    const VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, slice, 1 };
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags rw = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    if (full) {
        fill_barrier(cb, t->image, level, slice, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, no_content ? 0 : VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, all, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
        fill_barrier(cb, t->image, level, slice, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, back, VK_ACCESS_TRANSFER_WRITE_BIT, rw, VK_PIPELINE_STAGE_TRANSFER_BIT, all);
    } else {
        const VkImageSubresourceRange tsr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        fill_barrier(cb, tmp, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &tsr);
        fill_barrier(cb, tmp, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        fill_barrier(cb, t->image, level, slice, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, no_content ? 0 : VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, all, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, slice, 1 },
                           .dstOffset = { (int32_t)origin->x, (int32_t)origin->y, (int32_t)origin->z }, .extent = { size->width, size->height, rd } };
        vkCmdCopyImage(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        fill_barrier(cb, t->image, level, slice, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, back, VK_ACCESS_TRANSFER_WRITE_BIT, rw, VK_PIPELINE_STAGE_TRANSFER_BIT, all);
    }

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_fill_texture_color: vkEndCommandBuffer failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        st = LAGFX_ERR_BACKEND;
        goto cleanup_tmp;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk_impl->device, &fci, NULL, &fence);
    if (vr == VK_SUCCESS) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        vr = vkQueueSubmit(vk_impl->graphics_queue, 1, &si, fence);
        if (vr == VK_SUCCESS) vkWaitForFences(vk_impl->device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk_impl->device, fence, NULL);
    }
    lagfx_vk_cmdbuf_free(vk, cb);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_fill_texture_color: fence / submit failed (%d)", (int)vr);
        st = LAGFX_ERR_BACKEND;
        goto cleanup_tmp;
    }
    *layout = back;   /* only now: the recorded work ran */
    if (full) g_fill_stats.fill_full++; else g_fill_stats.fill_partial++;
    st = LAGFX_OK;

cleanup_tmp:
    if (tmp != VK_NULL_HANDLE) vkDestroyImage(vk_impl->device, tmp, NULL);
    if (tmem != VK_NULL_HANDLE) vkFreeMemory(vk_impl->device, tmem, NULL);
    if (st != LAGFX_OK) g_fill_stats.fill_failed++;
    return st;
}

lagfx_status_t lagfx_translate_blit_fill_texture_color(void *vk,
                                                       void *texture_ref,
                                                       uint32_t level,
                                                       uint32_t slice,
                                                       const MTLOrigin *origin,
                                                       const MTLSize *size,
                                                       const double color[4],
                                                       uint32_t pixel_format) {
    (void)pixel_format;   /* the target's own VkFormat decides the conversion; the payload's MTLPixelFormat is [I] not cross-checked */
    if (!vk || !texture_ref || !origin || !size || !color) return LAGFX_ERR_INVALID_ARG;
    lagfx_vk_iosurface_t *tex = (lagfx_vk_iosurface_t *)texture_ref;
    if (tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_fill_texture_color: invalid texture handle");
        return LAGFX_ERR_INVALID_ARG;
    }
    /* an IOSurface-backed image is 2D with one level and one layer: level / slice != 0 are refused by the worker */
    lagfx_blit_fill_target_t t = { .image = tex->image, .format = tex->format, .type = VK_IMAGE_TYPE_2D, .width = tex->width, .height = tex->height, .depth = 1, .mip_levels = 1, .array_layers = 1 };
    return lagfx_translate_blit_fill_image_color(vk, &t, &tex->layout, level, slice, origin, size, color);
}

/* ================================================================
 * 0x12f CopyFromTextureToTexture — vkCmdCopyImage
 *
 *   +0x00  u32 src_texture_ref      (resource registry index)
 *   +0x08  u32 dst_texture_ref      (resource registry index)
 *   +0x10  MTLOrigin src_origin     (8 bytes: x, y, z)
 *   +0x1c  MTLSize src_size         (12 bytes: width, height, depth)
 *   +0x28  u32 dst_slice            (4 bytes)
 *   +0x2c  u32 dst_level            (4 bytes)
*   = 88 B total
 * ================================================================ */

lagfx_status_t lagfx_translate_blit_copy_texture_to_texture(void *vk,
                                                            void *src_texture_ref,
                                                            void *dst_texture_ref,
                                                            const MTLOrigin *src_origin,
                                                            const MTLSize *src_size,
                                                            uint32_t dst_slice,
                                                            uint32_t dst_level) {
#endif /* LAGFX_HAVE_VULKAN */

#ifdef LAGFX_HAVE_VULKAN
    struct lagfx_vk_state *vk_impl = (struct lagfx_vk_state *)vk;

    if (!vk || !src_texture_ref || !dst_texture_ref
        || !src_origin || !src_size) {
        return LAGFX_ERR_INVALID_ARG;
    }

    if (src_size->width == 0 || src_size->height == 0) {
        LAGFX_WARN("blit_copy_texture_to_texture: empty region");
        return LAGFX_OK;
    }

    lagfx_vk_iosurface_t *src_tex = (lagfx_vk_iosurface_t *)src_texture_ref;
    lagfx_vk_iosurface_t *dst_tex = (lagfx_vk_iosurface_t *)dst_texture_ref;

    if (!src_tex || src_tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_copy_texture_to_texture: invalid source texture");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!dst_tex || dst_tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_copy_texture_to_texture: invalid dest texture");
        return LAGFX_ERR_INVALID_ARG;
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_copy_texture_to_texture: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Transition src to transfer source layout */
    VkImageMemoryBarrier src_barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = src_tex->image,
        .subresourceRange = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount   = 1,
            .baseArrayLayer = 0,
            .layerCount   = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &src_barrier);

    /* Transition dst to transfer dest layout */
    VkImageMemoryBarrier dst_barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = dst_tex->image,
        .subresourceRange = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = dst_level,
            .levelCount   = 1,
            .baseArrayLayer = dst_slice,
            .layerCount   = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &dst_barrier);

    /* Issue copy command */
    VkImageCopy region = {
        .srcSubresource = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel    = 0,
            .baseArrayLayer = 0,
            .layerCount   = 1,
        },
        .srcOffset = {
            .x = (int32_t)src_origin->x,
            .y = (int32_t)src_origin->y,
            .z = (int32_t)src_origin->z,
        },
        .dstSubresource = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel    = dst_level,
            .baseArrayLayer = dst_slice,
            .layerCount   = 1,
        },
        .dstOffset = {0, 0, 0},
        .extent = {
            .width  = src_size->width,
            .height = src_size->height,
            .depth  = src_size->depth,
        },
    };

    vkCmdCopyImage(cb, src_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dst_tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);

    /* Restore layouts */
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, NULL, 0, NULL, 1, &src_barrier);

    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, NULL, 0, NULL, 1, &dst_barrier);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_copy_texture_to_texture: vkEndCommandBuffer failed (%d)",
                   (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Submit and wait */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    vr = vkCreateFence(vk_impl->device, &fci, NULL, &fence);
    if (vr == VK_SUCCESS) {
        VkSubmitInfo si = {
            .sType  = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        vkQueueSubmit(vk_impl->graphics_queue, 1, &si, fence);
        vkWaitForFences(vk_impl->device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk_impl->device, fence, NULL);
    } else {
        LAGFX_WARN("blit_copy_texture_to_texture: create fence failed (%d)", (int)vr);
    }

    lagfx_vk_cmdbuf_free(vk, cb);
    return LAGFX_OK;
}


/* ================================================================
 * ARM64 exec recorders (tags 300-303, 306): record only, see blit_encoder.h
 * ================================================================ */
void lagfx_translate_blit_cmd_buffer_image(VkCommandBuffer cb, VkBuffer buf, VkImage img, const VkBufferImageCopy *r, bool buffer_to_image) {
    if (buffer_to_image) vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, r);
    else vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, r);
}
void lagfx_translate_blit_cmd_buffer_buffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, const VkBufferCopy *r) { vkCmdCopyBuffer(cb, src, dst, 1, r); }
void lagfx_translate_blit_cmd_image_image(VkCommandBuffer cb, VkImage src, VkImage dst, const VkImageCopy *r) {
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, r);
}
void lagfx_translate_blit_cmd_fill_buffer(VkCommandBuffer cb, VkBuffer buf, uint64_t offset, uint64_t size, uint8_t value) {
    vkCmdFillBuffer(cb, buf, offset, size, (uint32_t)value * 0x01010101u);
}

#endif /* LAGFX_HAVE_VULKAN */

#ifndef LAGFX_HAVE_VULKAN
/* Non-Vulkan host stubs. blit_opcodes.c calls these unconditionally;
 * the real implementations live above inside LAGFX_HAVE_VULKAN. On
 * non-Vulkan hosts (macOS CI runner, etc.) we satisfy the linker with
 * stubs that return NOT_SUPPORTED so the library can build + run the
 * non-Vulkan stub init path. */

lagfx_status_t lagfx_translate_blit_fill_texture_color(void *vk,
                                                       void *texture_ref,
                                                       uint32_t level,
                                                       uint32_t slice,
                                                       const MTLOrigin *origin,
                                                       const MTLSize *size,
                                                       const double color[4],
                                                       uint32_t pixel_format) {
    (void)vk; (void)texture_ref; (void)level; (void)slice;
    (void)origin; (void)size; (void)color; (void)pixel_format;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_blit_copy_texture_to_texture(void *vk,
                                                            void *src_texture_ref,
                                                            void *dst_texture_ref,
                                                            const MTLOrigin *src_origin,
                                                            const MTLSize *src_size,
                                                            uint32_t dst_slice,
                                                            uint32_t dst_level) {
    (void)vk; (void)src_texture_ref; (void)dst_texture_ref;
    (void)src_origin; (void)src_size; (void)dst_slice; (void)dst_level;
    return LAGFX_ERR_BACKEND;
}
#endif /* !LAGFX_HAVE_VULKAN */
