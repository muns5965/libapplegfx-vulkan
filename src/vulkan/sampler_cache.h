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

/* 0: real bicubic sampling does not exist; a bicubic request (filter 2) is mapped to plain LINEAR and counted (cxs_state_builds_bicubic_as_linear). Flip only when real
 * bicubic sampling exists. Tests include this header and use the same macro. */
#ifndef LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED
#define LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED 0
#endif

/* Process-wide fidelity counters of the constexpr-sampler mapping (and of the runtime sampler's out-of-range enum values). Each cxs_state_builds_* counter counts sampler-STATE BUILDS
 * (one per constexpr bind / descriptor build, plus test calls) in which the condition held. None of them is a count of guest sampler requests or of samples; the log line is printed once per distinct word. The names say what the host did, not what
 * Metal does: cxs_state_builds_bicubic_as_linear counts state builds with a bicubic filter value that were sampled with plain LINEAR; while LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED is 0 real bicubic filtering is not implemented. */
typedef struct {
    uint64_t cxs_state_builds_bicubic_as_linear;      /* mag or min filter 2 (bicubic) mapped to VK_FILTER_LINEAR (approximation) */
    uint64_t cxs_state_builds_clamp_zero_as_border;   /* address 0 (clamp_to_zero) mapped to clamp-to-border + transparent black (approximation) */
    uint64_t cxs_state_builds_addr_outside_table;       /* address value 5..7 (outside our INFER table 0..4): fell back to clamp-to-edge */
    uint64_t cxs_state_builds_filter_outside_table;     /* mag/min filter value 3+ (outside our INFER table 0..2): fell back to LINEAR (as before) */
    uint64_t cxs_state_builds_mip_outside_table;        /* mip filter value 3+ (outside our INFER table 0..2): fell back to nearest (as before) */
    uint64_t cxs_state_builds_compare_field_not8;        /* compare function field != 8: not mapped. 8 appears in all 4 captured words; its meaning ("no compare") is unverified */
    uint64_t cxs_testpath_state_builds_unnorm_illegal_addr;    /* TEST-ONLY path (lagfx_sampler_state_from_constexpr): pixel word with repeat / mirrored repeat on S or T, no Vulkan unnormalized sampler (the state stays normalized).
                                                       * It can NOT fire in production: from_constexpr_norm forces normalized = true, so cxs_note is always called with unnorm_illegal = false. A zero here says nothing about pixel words met in a real run. */
    uint64_t cxs_distinct_words_logged;           /* DISTINCT words that produced a log line (capped at SD_LOGGED_MAX = 64); not a count of words seen */
    uint64_t cxs_words_log_suppressed;   /* state builds whose word was not logged because the 64-word table was full (counting itself is never capped); the same word may be counted more than once */
    uint64_t rt_sampler_objs_addr_out_of_range;            /* runtime sampler OBJECT creations (not draws) with an address field 6 or 7: outside the public Metal enum range, mapped to clamp-to-edge. Same event as state.h ext_samp_addr_unknown (that pair is what diag.c prints) */
    uint64_t rt_sampler_objs_filter_out_of_range;         /* runtime sampler OBJECT creations whose min or mag filter field (2 bits, INFER) was 2 or 3: outside MTLSamplerMinMagFilter 0..1, mapped to LINEAR. Same event as ext_samp_filter_unknown */
    uint64_t rt_sampler_objs_mip_out_of_range;             /* runtime sampler OBJECT creations with mip field 3: outside the public Metal enum range. Same event as ext_samp_mip_unknown */
} lagfx_sampler_diag_t;

void lagfx_sampler_diag_get(lagfx_sampler_diag_t *out);        /* snapshot (zeros without Vulkan) */
void lagfx_sampler_diag_reset(void);                           /* tests only: zero the counters and forget the logged words */
void lagfx_sampler_diag_rt_count(bool addr_unknown, bool mip_unknown, bool filter_unknown, uint32_t state_word);   /* the runtime sampler: count + log once per distinct state word */

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

#define LAGFX_SAMPLER_CACHE_MAX 32u

typedef enum { LAGFX_CXS_EXACT = 0, LAGFX_CXS_APPROX = 1, LAGFX_CXS_UNSUPPORTED = 2 } lagfx_cxs_fid_t;

/* THE mapper of the constexpr address-mode field (INFER numbering, see constexpr_sampler.h; NOT the runtime / MTLSamplerAddressMode numbering):
 * 0 clamp_to_zero -> CLAMP_TO_BORDER (APPROX), 1 clamp_to_edge, 2 repeat, 3 mirrored_repeat, 4 clamp_to_border (transparent black) -> EXACT;
 * 5..7 -> CLAMP_TO_EDGE, UNSUPPORTED (never wraps onto another mode). Shared by from_constexpr and from_constexpr_norm (the production path). */
lagfx_cxs_fid_t lagfx_cxs_map_address(uint32_t v, VkSamplerAddressMode *out);
/* Filter field: 0 nearest, 1 linear EXACT; 2 bicubic -> LINEAR APPROX; 3.. -> LINEAR UNSUPPORTED. */
lagfx_cxs_fid_t lagfx_cxs_map_filter(uint32_t v, VkFilter *out);
/* Mip filter field: 0 none (NEAREST mode, level 0 only), 1 nearest, 2 linear EXACT; 3.. -> NEAREST UNSUPPORTED. */
lagfx_cxs_fid_t lagfx_cxs_map_mip(uint32_t v, VkSamplerMipmapMode *out);
/* Vulkan: unnormalized coordinates need clamp-to-edge or clamp-to-border on U and V. */
bool lagfx_cxs_address_unnorm_legal(VkSamplerAddressMode m);

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

/* State for a constexpr sampler: address modes, filters, mip filter, LOD clamps, anisotropy from the decoded word (field meaning INFER, see constexpr_sampler.h);
 * bicubic and the compare function are not mapped. A pixel (unnormalized) word becomes an unnormalized VkSampler only when Vulkan's restrictions hold (kept for
 * callers that sample with explicit LOD 0); the translator-wired path uses lagfx_sampler_state_from_constexpr_norm instead. An invalid/NULL decode yields the default. */
lagfx_sampler_state_t lagfx_sampler_state_from_constexpr(const lagfx_constexpr_sampler_t *cs);

/* The host sampler of a constexpr sampler whose coordinates the SHADER converts (the translator divides the texel coordinates of a pixel / unnormalized constexpr
 * sampler by the image size): always a NORMALIZED VkSampler. Address modes, filters, anisotropy follow from_constexpr; for a pixel sampler the mip filter / LOD range
 * collapse to level 0 (INFER: Metal coord::pixel samples level 0 only) and repeat / mirrored repeat stay what the word says (Metal forbids them with pixel
 * coordinates; with the shader-side division they have the normalized meaning). An invalid / NULL decode yields the default. */
lagfx_sampler_state_t lagfx_sampler_state_from_constexpr_norm(const lagfx_constexpr_sampler_t *cs);

void lagfx_sampler_cache_init(lagfx_sampler_cache_t *c, VkDevice device, bool have_anisotropy);
void lagfx_sampler_cache_destroy(lagfx_sampler_cache_t *c);

/* Return the cached sampler for `st` (creating it on first use), or
 * VK_NULL_HANDLE if creation failed / the cache is full. */
VkSampler lagfx_sampler_cache_get(lagfx_sampler_cache_t *c, const lagfx_sampler_state_t *st);
uint32_t  lagfx_sampler_cache_count(const lagfx_sampler_cache_t *c);

#endif /* LAGFX_HAVE_VULKAN */
#endif
