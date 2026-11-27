/*
 * libapplegfx-vulkan — ARM64 exec: serialized sampler -> VkSamplerCreateInfo. See arm64_sampler.h for the layout.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_sampler.h"

#ifdef LAGFX_HAVE_VULKAN
#include "../common/le.h"
#include <string.h>

static VkSamplerAddressMode vk_addr(uint32_t m, bool mirror_clamp) {
    switch (m) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

uint32_t lagfx_a64_sampler_decode(const uint8_t *d, uint32_t n, bool have_aniso, float max_aniso, VkSamplerCreateInfo *ci) {
    return lagfx_a64_sampler_decode_ex(d, n, have_aniso, max_aniso, false, ci);
}

uint32_t lagfx_a64_sampler_decode_ex(const uint8_t *d, uint32_t n, bool have_aniso, float max_aniso, bool have_mirror_clamp, VkSamplerCreateInfo *ci) {
    uint32_t flags = 0;
    uint32_t st = lagfx_le32(d + 12);
    uint32_t lo = n >= 24u ? lagfx_le32(d + 20) : 0, hi = n >= 28u ? lagfx_le32(d + 24) : 0x7f7fffffu;
    float lod_min, lod_max;
    memcpy(&lod_min, &lo, 4), memcpy(&lod_max, &hi, 4);
    if (!(lod_min >= 0.f)) lod_min = 0.f;                        /* also NaN */
    if (!(lod_max >= lod_min)) lod_max = 32.f, lod_min = 0.f;    /* an inverted range is a guest error: the defaults */
    if (lod_max > VK_LOD_CLAMP_NONE) lod_max = VK_LOD_CLAMP_NONE;   /* the FLT_MAX default */
    uint32_t mip = (st >> 24) & 3u, am[3] = { (st >> 8) & 7u, (st >> 12) & 7u, (st >> 16) & 7u }, bc = (st >> 20) & 3u, aniso = (st >> 26) & 31u, cmp = (st >> 5) & 7u;
    bool border = am[0] == 5u || am[1] == 5u || am[2] == 5u;
    *ci = (VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .minFilter = (st & 1u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .magFilter = ((st >> 2) & 1u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip == 2u ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_addr(am[0], have_mirror_clamp), .addressModeV = vk_addr(am[1], have_mirror_clamp), .addressModeW = vk_addr(am[2], have_mirror_clamp),
        .minLod = mip ? lod_min : 0.f, .maxLod = mip ? lod_max : 0.25f,
        .borderColor = !border ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : bc == 2u ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : bc == 1u ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    if (have_aniso && aniso > 1u && max_aniso >= 1.f) {
        ci->anisotropyEnable = VK_TRUE, ci->maxAnisotropy = (float)aniso < max_aniso ? (float)aniso : max_aniso;
        flags |= LAGFX_A64_SAMP_ANISO_APPLIED;
    }
    if (!(st & 0x80000000u)) {   /* unnormalized coordinates (seen on a real device: two of the six samplers of a capture): Vulkan's restrictions are applied to the state */
        ci->unnormalizedCoordinates = VK_TRUE;
        ci->magFilter = ci->minFilter;                                  /* the two filters must be equal (the minification one is kept: INFER, real device samplers had equal filters) */
        ci->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, ci->minLod = ci->maxLod = 0.f;
        if (ci->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ci->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   /* only the clamping modes exist (Vulkan: U and V must be edge or border) */
        if (ci->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ci->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci->addressModeW = ci->addressModeU;                            /* unnormalized images are 1D / 2D: R is unused; keep it a legal value */
        ci->anisotropyEnable = VK_FALSE, ci->maxAnisotropy = 0.f;
        flags &= ~LAGFX_A64_SAMP_ANISO_APPLIED;
        flags |= LAGFX_A64_SAMP_UNNORM_APPLIED;
        mip = 0;                                                        /* no LOD at all: the clamps of a LOD sampler bind do not apply */
    }
    if (mip) flags |= LAGFX_A64_SAMP_MIPMAPPED;
    if (cmp) flags |= LAGFX_A64_SAMP_COMPARE_IGNORED;
    if (!have_mirror_clamp && (am[0] == 1u || am[1] == 1u || am[2] == 1u)) flags |= LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX;
    if (st & 0x10u) flags |= LAGFX_A64_SAMP_LODAVG_IGNORED;
    if (am[0] > 5u || am[1] > 5u || am[2] > 5u) flags |= LAGFX_A64_SAMP_ADDR_UNKNOWN;   /* the mapping above is unchanged: 6 / 7 stay clamp-to-edge */
    if ((st >> 24 & 3u) == 3u) flags |= LAGFX_A64_SAMP_MIP_UNKNOWN;
    return flags;
}
#endif
