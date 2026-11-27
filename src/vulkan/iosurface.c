/*
 * libapplegfx-vulkan — Vulkan-backed IOSurface create/destroy (Phase 4)
 * src/vulkan/iosurface.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "iosurface.h"
#include "instance.h"
#include "command.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

static uint32_t find_memory_type(VkPhysicalDevice phys,
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

/* MTLPixelFormat (public Metal enum; numeric values as in Apple's Metal headers, checked against the objc2-metal generated bindings of MTLPixelFormat.h) -> the
 * exactly equivalent VkFormat. Every Vulkan equivalent below is the one MoltenVK (Apache-2.0, MVKPixelFormats.mm) pairs with that MTLPixelFormat. The uncompressed
 * colour rows are the same set as k_fmt[] in src/protocol/arm64_resources.c (the arm64 texture mapper); unmapped on purpose: 253 / 255 / 260 stencil and depth-stencil formats (this
 * function's depth caller passes a depth attachment format; 255 is also macOS-only in Metal and optional in Vulkan: the arm64 mapper has the fallback), 261/262 stencil views, the 16-bit packed (40-43), 91/94 and
 * XR formats and every compressed format. Formats that occur in the device dumps (runs 231501 / 230109): 1, 30, 70, 80, 115. An unknown value is reported and
 * substituted by BGRA8, as before. */
static const struct { uint32_t mtl; VkFormat vk; } k_mtl_to_vk[] = {
    {10, VK_FORMAT_R8_UNORM}, {11, VK_FORMAT_R8_SRGB}, {12, VK_FORMAT_R8_SNORM}, {13, VK_FORMAT_R8_UINT}, {14, VK_FORMAT_R8_SINT},
    {20, VK_FORMAT_R16_UNORM}, {22, VK_FORMAT_R16_SNORM}, {23, VK_FORMAT_R16_UINT}, {24, VK_FORMAT_R16_SINT}, {25, VK_FORMAT_R16_SFLOAT},   /* R16Float is 25; Depth16Unorm is 250 */
    {30, VK_FORMAT_R8G8_UNORM}, {31, VK_FORMAT_R8G8_SRGB}, {32, VK_FORMAT_R8G8_SNORM}, {33, VK_FORMAT_R8G8_UINT}, {34, VK_FORMAT_R8G8_SINT},
    {53, VK_FORMAT_R32_UINT}, {54, VK_FORMAT_R32_SINT}, {55, VK_FORMAT_R32_SFLOAT},
    {60, VK_FORMAT_R16G16_UNORM}, {62, VK_FORMAT_R16G16_SNORM}, {63, VK_FORMAT_R16G16_UINT}, {64, VK_FORMAT_R16G16_SINT}, {65, VK_FORMAT_R16G16_SFLOAT},
    {70, VK_FORMAT_R8G8B8A8_UNORM}, {71, VK_FORMAT_R8G8B8A8_SRGB}, {72, VK_FORMAT_R8G8B8A8_SNORM}, {73, VK_FORMAT_R8G8B8A8_UINT}, {74, VK_FORMAT_R8G8B8A8_SINT},
    {80, VK_FORMAT_B8G8R8A8_UNORM}, {81, VK_FORMAT_B8G8R8A8_SRGB},
    {90, VK_FORMAT_A2B10G10R10_UNORM_PACK32}, {92, VK_FORMAT_B10G11R11_UFLOAT_PACK32}, {93, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},
    {103, VK_FORMAT_R32G32_UINT}, {104, VK_FORMAT_R32G32_SINT}, {105, VK_FORMAT_R32G32_SFLOAT},
    {110, VK_FORMAT_R16G16B16A16_UNORM}, {112, VK_FORMAT_R16G16B16A16_SNORM}, {113, VK_FORMAT_R16G16B16A16_UINT}, {114, VK_FORMAT_R16G16B16A16_SINT}, {115, VK_FORMAT_R16G16B16A16_SFLOAT},
    {123, VK_FORMAT_R32G32B32A32_UINT}, {124, VK_FORMAT_R32G32B32A32_SINT}, {125, VK_FORMAT_R32G32B32A32_SFLOAT},
    {250, VK_FORMAT_D16_UNORM}, {252, VK_FORMAT_D32_SFLOAT},
};

VkFormat lagfx_metal_pixel_format_to_vk(uint32_t pixel_format) {
    if (pixel_format == 1u) return VK_FORMAT_R8_UNORM;   /* MTLPixelFormatA8Unorm — glyph-atlas coverage bytes (stored as one byte, as before) */
    for (size_t i = 0; i < sizeof k_mtl_to_vk / sizeof *k_mtl_to_vk; i++)
        if (k_mtl_to_vk[i].mtl == pixel_format) return k_mtl_to_vk[i].vk;
    LAGFX_WARN("iosurface: unknown Metal pixel format %u, "
                "falling back to BGRA8", (unsigned)pixel_format);
    return VK_FORMAT_B8G8R8A8_UNORM;
}

static VkFormat metal_pixel_format_to_vk(uint32_t pixel_format) {
    return lagfx_metal_pixel_format_to_vk(pixel_format);
}

lagfx_status_t lagfx_vk_iosurface_create(struct lagfx_vk_state *vk,
                                          uint32_t width, uint32_t height,
                                          uint32_t pixel_format,
                                          lagfx_vk_iosurface_t **out) {
    if (!vk || !out || width == 0u || height == 0u) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        LAGFX_ERR("iosurface_create: vk state not initialized");
        return LAGFX_ERR_BACKEND;
    }

    lagfx_vk_iosurface_t *ios = calloc(1, sizeof(*ios));
    if (!ios) return LAGFX_ERR_OUT_OF_MEMORY;

    VkFormat fmt = metal_pixel_format_to_vk(pixel_format);

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { width, height, 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = vkCreateImage(vk->device, &ici, NULL, &ios->image);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkCreateImage failed (VkResult=%d)", (int)vr);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, ios->image, &req);

    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("iosurface_create: no matching memory type (typeBits=0x%x)",
                   req.memoryTypeBits);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &ios->memory);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkAllocateMemory failed (VkResult=%d)", (int)vr);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindImageMemory(vk->device, ios->image, ios->memory, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkBindImageMemory failed (VkResult=%d)", (int)vr);
        vkFreeMemory(vk->device, ios->memory, NULL);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = ios->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        /* A8/R8 glyph atlases carry COVERAGE in the single channel; the panel
         * shaders sample .a (and some .r). Broadcast r → (r,r,r,r) so both
         * conventions see the coverage byte instead of alpha=1/rgb=0. */
        .components = (fmt == VK_FORMAT_R8_UNORM)
            ? (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                    VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R }
            : (VkComponentMapping){ 0 },
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vr = vkCreateImageView(vk->device, &vci, NULL, &ios->view);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkCreateImageView failed (VkResult=%d)", (int)vr);
        vkFreeMemory(vk->device, ios->memory, NULL);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    ios->width  = width;
    ios->height = height;
    ios->format = fmt;
    ios->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ios->refcount = 1u;

    /* M2: clear the new IOSurface to a color + transition to SHADER_READ_ONLY
     * so it is immediately SAMPLEABLE. Without this every IOSurface stays
     * UNDEFINED, the texture-bind path treats it as unbacked, and every
     * texture-sampling compositor pipeline is skipped → black. A cleared,
     * sampleable surface lets the composite chain DRAW (sampling real content)
     * — non-black pixels from the real translated shaders. The cleared colour
     * is the placeholder until per-pipeline RT compositing fills surfaces with
     * actual rendered layers. Gated by LAGFX_CLEAR_IOSURF (off → no behaviour
     * change). Best-effort: on any Vulkan failure, leave layout UNDEFINED. */
    if (getenv("LAGFX_CLEAR_IOSURF") && vk->graphics_queue != VK_NULL_HANDLE) {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK) {
            VkCommandBufferBeginInfo bi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            VkImageSubresourceRange range = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1u, .layerCount = 1u,
            };
            if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
                VkImageMemoryBarrier to_dst = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = ios->image, .subresourceRange = range,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                };
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, NULL, 0, NULL, 1, &to_dst);
                VkClearColorValue cc = { .float32 = { 0.20f, 0.22f, 0.28f, 1.0f } };
                vkCmdClearColorImage(cb, ios->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &cc, 1, &range);
                VkImageMemoryBarrier to_ro = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = ios->image, .subresourceRange = range,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                };
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, NULL, 0, NULL, 1, &to_ro);
                if (vkEndCommandBuffer(cb) == VK_SUCCESS) {
                    VkFence fence = VK_NULL_HANDLE;
                    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                    vkCreateFence(vk->device, &fci, NULL, &fence);
                    VkSubmitInfo si = {
                        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb,
                    };
                    if (vkQueueSubmit(vk->graphics_queue, 1, &si, fence) == VK_SUCCESS) {
                        vkWaitForFences(vk->device, 1, &fence, VK_TRUE,
                                        1000ull * 1000ull * 1000ull);
                        ios->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        LAGFX_LOG("iosurface_create: %ux%u cleared + SHADER_READ_ONLY (sampleable)",
                                  width, height);
                    }
                    if (fence != VK_NULL_HANDLE) vkDestroyFence(vk->device, fence, NULL);
                }
            }
            lagfx_vk_cmdbuf_free(vk, cb);
        }
    }

    LAGFX_LOG("iosurface_create: %ux%u fmt=%d image=%p view=%p mem=%p refcount=1",
              width, height, (int)fmt,
              (void *)ios->image, (void *)ios->view, (void *)ios->memory);

    *out = ios;
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_iosurface_upload_pixels(struct lagfx_vk_state *vk,
                                                lagfx_vk_iosurface_t *ios,
                                                const uint8_t *data,
                                                size_t data_len) {
    if (!vk || !ios || !data || data_len == 0u) return LAGFX_ERR_INVALID_ARG;
    if (vk->graphics_queue == VK_NULL_HANDLE || ios->image == VK_NULL_HANDLE)
        return LAGFX_ERR_BACKEND;

    uint32_t texel_bytes = 4u;
    switch (ios->format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: texel_bytes = 8u; break;
    case VK_FORMAT_D16_UNORM:           texel_bytes = 2u; break;
    case VK_FORMAT_R8_UNORM:            texel_bytes = 1u; break;
    default:                            texel_bytes = 4u; break;
    }
    const VkDeviceSize img_bytes = (VkDeviceSize)ios->width * ios->height * texel_bytes;
    VkDeviceSize copy_bytes = data_len < img_bytes ? (VkDeviceSize)data_len : img_bytes;

    /* Host-visible staging buffer sized to the whole image (zero-padded). */
    VkBuffer       sb = VK_NULL_HANDLE;
    VkDeviceMemory sm = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = img_bytes,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(vk->device, &bci, NULL, &sb) != VK_SUCCESS)
        return LAGFX_ERR_BACKEND;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, sb, &req);
    uint32_t mt = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) { vkDestroyBuffer(vk->device, sb, NULL); return LAGFX_ERR_BACKEND; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(vk->device, &mai, NULL, &sm) != VK_SUCCESS) {
        vkDestroyBuffer(vk->device, sb, NULL); return LAGFX_ERR_BACKEND;
    }
    vkBindBufferMemory(vk->device, sb, sm, 0);
    void *map = NULL;
    if (vkMapMemory(vk->device, sm, 0, img_bytes, 0, &map) != VK_SUCCESS) {
        vkFreeMemory(vk->device, sm, NULL); vkDestroyBuffer(vk->device, sb, NULL);
        return LAGFX_ERR_BACKEND;
    }
    memset(map, 0, (size_t)img_bytes);
    memcpy(map, data, (size_t)copy_bytes);
    vkUnmapMemory(vk->device, sm);

    lagfx_status_t result = LAGFX_ERR_BACKEND;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK) {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1u, .layerCount = 1u,
        };
        if (vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS) {
            VkImageMemoryBarrier to_dst = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = ios->image, .subresourceRange = range,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
            VkBufferImageCopy region = {
                .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1u },
                .imageExtent = { ios->width, ios->height, 1u },
            };
            vkCmdCopyBufferToImage(cb, sb, ios->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            VkImageMemoryBarrier to_ro = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = ios->image, .subresourceRange = range,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_ro);
            if (vkEndCommandBuffer(cb) == VK_SUCCESS) {
                VkFence fence = VK_NULL_HANDLE;
                VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                vkCreateFence(vk->device, &fci, NULL, &fence);
                VkSubmitInfo si = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1, .pCommandBuffers = &cb,
                };
                if (vkQueueSubmit(vk->graphics_queue, 1, &si, fence) == VK_SUCCESS) {
                    vkWaitForFences(vk->device, 1, &fence, VK_TRUE, 1000ull*1000ull*1000ull);
                    ios->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    result = LAGFX_OK;
                    LAGFX_LOG("iosurface_upload: %ux%u uploaded %llu/%llu bytes → SHADER_READ_ONLY",
                              ios->width, ios->height, (unsigned long long)copy_bytes,
                              (unsigned long long)img_bytes);
                }
                if (fence != VK_NULL_HANDLE) vkDestroyFence(vk->device, fence, NULL);
            }
        }
        lagfx_vk_cmdbuf_free(vk, cb);
    }
    vkFreeMemory(vk->device, sm, NULL);
    vkDestroyBuffer(vk->device, sb, NULL);
    return result;
}

void lagfx_vk_iosurface_retain(lagfx_vk_iosurface_t *ios) {
    if (!ios) return;
    /* BQL-serialised in steady-state; non-atomic is correct here.
     * If we ever drain on a thread outside BQL, lift to _Atomic. */
    ios->refcount += 1u;
    LAGFX_TRACE("iosurface_retain: %ux%u → refcount=%u",
                ios->width, ios->height, ios->refcount);
}

void lagfx_vk_iosurface_release(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios) {
    if (!ios) return;
    if (ios->refcount == 0u) {
        LAGFX_WARN("iosurface_release: refcount already 0 — double release? "
                   "%ux%u fmt=%d", ios->width, ios->height, (int)ios->format);
        return;
    }
    ios->refcount -= 1u;
    LAGFX_TRACE("iosurface_release: %ux%u → refcount=%u",
                ios->width, ios->height, ios->refcount);
    if (ios->refcount == 0u) {
        lagfx_vk_iosurface_destroy(vk, ios);
    }
}

void lagfx_vk_iosurface_destroy(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios) {
    if (!vk || !ios) return;
    if (vk->device == VK_NULL_HANDLE) {
        free(ios);
        return;
    }
    if (ios->view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, ios->view, NULL);
    }
    if (ios->image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, ios->image, NULL);
    }
    if (ios->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, ios->memory, NULL);
    }
    LAGFX_LOG("iosurface_destroy: %ux%u fmt=%d (was refcount=%u)",
              ios->width, ios->height, (int)ios->format, ios->refcount);
    free(ios);
}

#endif
