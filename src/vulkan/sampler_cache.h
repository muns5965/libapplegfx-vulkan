/*
 * libapplegfx-vulkan — VkSampler cache keyed by sampler state
 * src/vulkan/sampler_cache.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. One VkSampler per distinct state; identical states share
 * the handle. The cache owns the samplers (destroyed with the cache).
 */

#ifndef LIBAPPLEGFX_VULKAN_SAMPLER_CACHE_H
#define LIBAPPLEGFX_VULKAN_SAMPLER_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "air2spv/constexpr_sampler.h"

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

#define LAGFX_SAMPLER_CACHE_MAX 32u

typedef struct {
    VkFilter             mag_filter, min_filter;
    VkSamplerMipmapMode  mip_mode;
    VkSamplerAddressMode addr_u, addr_v, addr_w;
    float                lod_min, lod_max;     /* lod_max >= 1000 is canonicalised to VK_LOD_CLAMP_NONE */
    float                max_anisotropy;       /* <= 1 : anisotropy disabled */
    bool                 compare_enable;
    VkCompareOp          compare_op;
    VkBorderColor        border;
    bool                 unnormalized;
} lagfx_sampler_state_t;

typedef struct {
    VkDevice              device;
    bool                  have_anisotropy;     /* device feature samplerAnisotropy enabled */
    uint32_t              count;
    lagfx_sampler_state_t state[LAGFX_SAMPLER_CACHE_MAX];
    VkSampler             sampler[LAGFX_SAMPLER_CACHE_MAX];
} lagfx_sampler_cache_t;

/* The state of the library's shared default sampler: linear/linear,
 * mip nearest, clamp-to-edge on all axes, lod [0, none), no anisotropy,
 * no compare, normalized coordinates. */
lagfx_sampler_state_t lagfx_sampler_state_default(void);

/* State for a constexpr sampler: the default with the fields that are
 * evidenced in the AIR encoding applied (currently only the LOD clamps;
 * see constexpr_sampler.h). An invalid/NULL decode yields the default. */
lagfx_sampler_state_t lagfx_sampler_state_from_constexpr(const lagfx_constexpr_sampler_t *cs);

void lagfx_sampler_cache_init(lagfx_sampler_cache_t *c, VkDevice device, bool have_anisotropy);
void lagfx_sampler_cache_destroy(lagfx_sampler_cache_t *c);

/* Return the cached sampler for `st` (creating it on first use), or
 * VK_NULL_HANDLE if creation failed / the cache is full. */
VkSampler lagfx_sampler_cache_get(lagfx_sampler_cache_t *c, const lagfx_sampler_state_t *st);
uint32_t  lagfx_sampler_cache_count(const lagfx_sampler_cache_t *c);

#endif /* LAGFX_HAVE_VULKAN */
#endif
