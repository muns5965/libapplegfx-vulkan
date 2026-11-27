/*
 * libapplegfx-vulkan — standalone mip-chain generation recorder (Metal generateMipmaps equivalent)
 * src/translate/mip_gen.h
 *
 * SPDX-License-Identifier: MIT
 *
 * Implemented from the Vulkan specification only (vkCmdBlitImage, VkFormatFeatureFlagBits, image layouts / barriers). NOT connected to the ARM64 blit
 * dispatcher: the wire payload of tag 307 is not established by this repository (docs/g7-blit-mip.md).
 */
#ifndef LIBAPPLEGFX_TRANSLATE_MIP_GEN_H
#define LIBAPPLEGFX_TRANSLATE_MIP_GEN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

typedef enum {
    LAGFX_MIPGEN_OK = 0,
    LAGFX_MIPGEN_NOTHING_TO_DO,       /* one level only (or zero layers): nothing recorded, not an error */
    LAGFX_MIPGEN_REFUSED_ARGS,        /* null handle / zero extent / mip count above the chain length of the extent */
    LAGFX_MIPGEN_REFUSED_TYPE,        /* not a 2D image (3D chains are not generated here) */
    LAGFX_MIPGEN_REFUSED_MULTISAMPLE,
    LAGFX_MIPGEN_REFUSED_DEPTH_STENCIL,
    LAGFX_MIPGEN_REFUSED_COMPRESSED,
    LAGFX_MIPGEN_REFUSED_USAGE,       /* image lacks TRANSFER_SRC and/or TRANSFER_DST */
    LAGFX_MIPGEN_REFUSED_FEATURES,    /* format lacks BLIT_SRC / BLIT_DST / SAMPLED_IMAGE_FILTER_LINEAR (optimal tiling) */
    LAGFX_MIPGEN_REASON_COUNT
} lagfx_mipgen_status_t;

typedef struct { VkFormat format; uint32_t width, height, mip_levels, array_layers; VkSampleCountFlagBits samples; VkImageType type; VkImageUsageFlags usage; } lagfx_mipgen_image_t;

/* Process-wide counters, one per status (BQL-serialised like the blit path). */
void lagfx_mipgen_stats_get(uint64_t out[LAGFX_MIPGEN_REASON_COUNT]);
void lagfx_mipgen_stats_reset(void);
const char *lagfx_mipgen_status_name(lagfx_mipgen_status_t s);

/* Validates, and only if everything is acceptable records into `cb` (nothing is recorded on a refusal): level 0 is the source of the chain; level i+1 is
 * a linear-filter vkCmdBlitImage of level i, extent max(1, dim >> 1), for every array layer, with a transfer write->read barrier between levels. `layout` is the
 * layout every subresource is in on entry and is restored at the end (levels 0..n-1, all layers); access is restored as a general memory read|write barrier.
 * `layout` must not be UNDEFINED (the level 0 contents would be discarded). */
lagfx_mipgen_status_t lagfx_mipgen_record(VkCommandBuffer cb, VkPhysicalDevice phys, VkImage img, const lagfx_mipgen_image_t *info, VkImageLayout layout);
#endif

#endif
