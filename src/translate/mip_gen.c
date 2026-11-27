/* libapplegfx-vulkan — standalone mip-chain generation recorder. SPDX-License-Identifier: MIT. See mip_gen.h. */
#include "translate/mip_gen.h"
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN
static uint64_t g_stats[LAGFX_MIPGEN_REASON_COUNT];

void lagfx_mipgen_stats_get(uint64_t out[LAGFX_MIPGEN_REASON_COUNT]) { memcpy(out, g_stats, sizeof g_stats); }
void lagfx_mipgen_stats_reset(void) { memset(g_stats, 0, sizeof g_stats); }
const char *lagfx_mipgen_status_name(lagfx_mipgen_status_t s) {
    static const char *n[] = { "ok", "nothing_to_do", "args", "type", "multisample", "depth_stencil", "compressed", "usage", "features" };
    return (unsigned)s < LAGFX_MIPGEN_REASON_COUNT ? n[s] : "?";
}

static bool is_depth_stencil(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default: return false;
    }
}
/* block-compressed formats occupy the contiguous enum ranges BC1..ASTC (core 1.0: 131..184) plus the extension ranges PVRTC / ASTC-HDR */
static bool is_compressed(VkFormat f) {
    if ((int)f >= (int)VK_FORMAT_BC1_RGB_UNORM_BLOCK && (int)f <= (int)VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return true;
    if ((int)f >= (int)VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && (int)f <= (int)VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG) return true;
    if ((int)f >= (int)VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && (int)f <= (int)VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK) return true;
    return false;
}
static uint32_t half(uint32_t v) { return v > 1u ? v >> 1 : 1u; }

static lagfx_mipgen_status_t note(lagfx_mipgen_status_t s) { g_stats[s]++; return s; }

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout a, VkImageLayout b, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                    uint32_t level, uint32_t nlevels, uint32_t layers) {
    VkImageMemoryBarrier br = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = a, .newLayout = b,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, nlevels, 0, layers } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &br);
}

lagfx_mipgen_status_t lagfx_mipgen_record(VkCommandBuffer cb, VkPhysicalDevice phys, VkImage img, const lagfx_mipgen_image_t *in, VkImageLayout layout) {
    if (!cb || !phys || !img || !in || !in->width || !in->height || !in->mip_levels || !in->array_layers || layout == VK_IMAGE_LAYOUT_UNDEFINED) return note(LAGFX_MIPGEN_REFUSED_ARGS);
    uint32_t chain = 1; for (uint32_t m = in->width > in->height ? in->width : in->height; m > 1; m >>= 1) chain++;
    if (in->mip_levels > chain) return note(LAGFX_MIPGEN_REFUSED_ARGS);
    if (in->type != VK_IMAGE_TYPE_2D) return note(LAGFX_MIPGEN_REFUSED_TYPE);
    if (in->samples != VK_SAMPLE_COUNT_1_BIT) return note(LAGFX_MIPGEN_REFUSED_MULTISAMPLE);
    if (is_depth_stencil(in->format)) return note(LAGFX_MIPGEN_REFUSED_DEPTH_STENCIL);
    if (is_compressed(in->format)) return note(LAGFX_MIPGEN_REFUSED_COMPRESSED);
    if (in->mip_levels < 2) return note(LAGFX_MIPGEN_NOTHING_TO_DO);
    const VkImageUsageFlags need_use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((in->usage & need_use) != need_use) return note(LAGFX_MIPGEN_REFUSED_USAGE);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, in->format, &fp);
    const VkFormatFeatureFlags need_f = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need_f) != need_f) return note(LAGFX_MIPGEN_REFUSED_FEATURES);

    const uint32_t n = in->mip_levels, L = in->array_layers;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags RW = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier(cb, img, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, RW, VK_ACCESS_TRANSFER_READ_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, L);
    barrier(cb, img, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RW, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, n - 1, L);
    uint32_t sw = in->width, sh = in->height;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t dw = half(sw), dh = half(sh);
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, L }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, L },     .dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } } };
        vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        if (i + 1 < n)   /* the level just written becomes the next source: write -> read */
            barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, i, 1, L);
        sw = dw, sh = dh;
    }
    /* levels 0..n-2 are TRANSFER_SRC, level n-1 TRANSFER_DST: back to the entry layout */
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, RW, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL, 0, n - 1, L);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT, RW, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL, n - 1, 1, L);
    return note(LAGFX_MIPGEN_OK);
}
#else
typedef int lagfx_mipgen_unused_t;
#endif
