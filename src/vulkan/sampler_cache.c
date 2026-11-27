/*
 * libapplegfx-vulkan — VkSampler cache keyed by sampler state
 * src/vulkan/sampler_cache.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "vulkan/sampler_cache.h"

#ifdef LAGFX_HAVE_VULKAN

#include <string.h>

lagfx_sampler_state_t lagfx_sampler_state_default(void) {
    lagfx_sampler_state_t s;
    memset(&s, 0, sizeof(s));
    s.mag_filter = VK_FILTER_LINEAR;
    s.min_filter = VK_FILTER_LINEAR;
    s.mip_mode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addr_u = s.addr_v = s.addr_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.lod_min = 0.0f;
    s.lod_max = VK_LOD_CLAMP_NONE;
    s.max_anisotropy = 1.0f;
    s.compare_op = VK_COMPARE_OP_NEVER;
    s.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return s;
}

static bool state_eq(const lagfx_sampler_state_t *a, const lagfx_sampler_state_t *b) {
    return a->mag_filter == b->mag_filter && a->min_filter == b->min_filter &&
           a->mip_mode == b->mip_mode && a->addr_u == b->addr_u && a->addr_v == b->addr_v &&
           a->addr_w == b->addr_w && a->lod_min == b->lod_min && a->lod_max == b->lod_max &&
           a->max_anisotropy == b->max_anisotropy && a->compare_enable == b->compare_enable &&
           a->compare_op == b->compare_op && a->border == b->border &&
           a->unnormalized == b->unnormalized;
}

/* Canonical form so that equivalent states compare equal. */
static void canon(lagfx_sampler_state_t *s) {
    if (s->lod_max >= 1000.0f) s->lod_max = VK_LOD_CLAMP_NONE;
    if (s->lod_min < 0.0f) s->lod_min = 0.0f;
    if (s->max_anisotropy < 1.0f) s->max_anisotropy = 1.0f;
    if (!s->compare_enable) s->compare_op = VK_COMPARE_OP_NEVER;
}

lagfx_sampler_state_t lagfx_sampler_state_from_constexpr(const lagfx_constexpr_sampler_t *cs) {
    lagfx_sampler_state_t s = lagfx_sampler_state_default();
    if (cs && cs->valid) {
        static const VkSamplerAddressMode am[5] = {
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,   /* 0 clamp_to_zero: transparent-black border (INFER) */
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };
        s.addr_u = am[cs->addr[0] % 5u]; s.addr_v = am[cs->addr[1] % 5u]; s.addr_w = am[cs->addr[2] % 5u];
        s.mag_filter = cs->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;   /* bicubic (2) not mapped */
        s.min_filter = cs->min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        s.mip_mode = cs->mip_filter == 2u ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.lod_min = cs->lod_min;
        s.lod_max = cs->mip_filter ? cs->lod_max : 0.0f;    /* mip filter none: base level only */
        s.max_anisotropy = (float)cs->max_aniso;
        s.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (!cs->normalized) {
            /* Vulkan unnormalized coordinates: edge/border addressing only, no mips,
             * no anisotropy. Applied only when those hold; otherwise the state stays
             * normalized and the caller must handle it (not silently remapped). */
            bool am_ok = true;
            for (int i = 0; i < 2; i++)
                if (cs->addr[i] == 2u || cs->addr[i] == 3u) am_ok = false;
            if (am_ok && cs->mip_filter == 0u && cs->max_aniso == 1u) {
                s.unnormalized = true; s.lod_min = 0.0f; s.lod_max = 0.0f;
                s.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                s.addr_w = s.addr_u;
                s.mag_filter = s.min_filter;   /* Vulkan: the two filters must be equal (the minification one is kept: INFER, as arm64_sampler.c) */
            }
        }
    }
    canon(&s);
    return s;
}

lagfx_sampler_state_t lagfx_sampler_state_from_constexpr_norm(const lagfx_constexpr_sampler_t *cs) {
    if (!cs || !cs->valid) return lagfx_sampler_state_default();
    lagfx_constexpr_sampler_t t = *cs;
    t.normalized = true;                    /* never Vulkan unnormalized coordinates: the shader converts them */
    lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr(&t);
    if (!cs->normalized) {
        s.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST, s.lod_min = 0.0f, s.lod_max = 0.0f, s.max_anisotropy = 1.0f;
        canon(&s);
    }
    return s;
}

void lagfx_sampler_cache_init(lagfx_sampler_cache_t *c, VkDevice device, bool have_anisotropy) {
    memset(c, 0, sizeof(*c));
    c->device = device;
    c->have_anisotropy = have_anisotropy;
}

void lagfx_sampler_cache_destroy(lagfx_sampler_cache_t *c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->count; i++)
        if (c->sampler[i] != VK_NULL_HANDLE) vkDestroySampler(c->device, c->sampler[i], NULL);
    c->count = 0;
}

uint32_t lagfx_sampler_cache_count(const lagfx_sampler_cache_t *c) { return c ? c->count : 0u; }

VkSampler lagfx_sampler_cache_get(lagfx_sampler_cache_t *c, const lagfx_sampler_state_t *st) {
    if (!c || !st || c->device == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    lagfx_sampler_state_t k = *st;
    canon(&k);
    for (uint32_t i = 0; i < c->count; i++)
        if (state_eq(&c->state[i], &k)) return c->sampler[i];
    if (c->count >= LAGFX_SAMPLER_CACHE_MAX) return VK_NULL_HANDLE;
    VkSamplerCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter     = k.mag_filter,
        .minFilter     = k.min_filter,
        .mipmapMode    = k.mip_mode,
        .addressModeU  = k.addr_u,
        .addressModeV  = k.addr_v,
        .addressModeW  = k.addr_w,
        .minLod        = k.lod_min,
        .maxLod        = k.lod_max,
        .anisotropyEnable = (c->have_anisotropy && k.max_anisotropy > 1.0f) ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = k.max_anisotropy,
        .compareEnable = k.compare_enable ? VK_TRUE : VK_FALSE,
        .compareOp     = k.compare_op,
        .borderColor   = k.border,
        .unnormalizedCoordinates = k.unnormalized ? VK_TRUE : VK_FALSE,
    };
    VkSampler vs = VK_NULL_HANDLE;
    if (vkCreateSampler(c->device, &ci, NULL, &vs) != VK_SUCCESS) return VK_NULL_HANDLE;
    c->state[c->count]   = k;
    c->sampler[c->count] = vs;
    c->count++;
    return vs;
}

#endif /* LAGFX_HAVE_VULKAN */
