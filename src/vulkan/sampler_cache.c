/*
 * libapplegfx-vulkan — VkSampler cache keyed by sampler state
 * src/vulkan/sampler_cache.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "vulkan/sampler_cache.h"
#include "common/log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---- fidelity counters (always built; zeros without Vulkan) ---- */
static lagfx_sampler_diag_t g_sd;
static pthread_mutex_t g_sd_lock = PTHREAD_MUTEX_INITIALIZER;
#define SD_LOGGED_MAX 64u
static uint64_t g_sd_cxs_seen[SD_LOGGED_MAX]; static uint32_t g_sd_cxs_n;
static uint32_t g_sd_rt_seen[SD_LOGGED_MAX];  static uint32_t g_sd_rt_n;
#define SD_ADD(f, n) __atomic_fetch_add(&g_sd.f, (uint64_t)(n), __ATOMIC_RELAXED)

void lagfx_sampler_diag_get(lagfx_sampler_diag_t *out) {
    if (!out) return;
    uint64_t *d = (uint64_t *)out; const uint64_t *s = (const uint64_t *)&g_sd;
    for (size_t i = 0; i < sizeof(*out) / sizeof(uint64_t); i++) d[i] = __atomic_load_n(&s[i], __ATOMIC_RELAXED);
}

void lagfx_sampler_diag_reset(void) {
    pthread_mutex_lock(&g_sd_lock);
    memset(&g_sd, 0, sizeof g_sd);
    g_sd_cxs_n = g_sd_rt_n = 0;
    pthread_mutex_unlock(&g_sd_lock);
}

/* 1 = first time (log it), 0 = already logged, -1 = new word but the table is full (not logged). */
static int sd_first_cxs(uint64_t w) {
    int first = 0;
    pthread_mutex_lock(&g_sd_lock);
    uint32_t i = 0;
    while (i < g_sd_cxs_n && g_sd_cxs_seen[i] != w) i++;
    if (i == g_sd_cxs_n) { if (g_sd_cxs_n < SD_LOGGED_MAX) g_sd_cxs_seen[g_sd_cxs_n++] = w, first = 1; else first = -1; }
    pthread_mutex_unlock(&g_sd_lock);
    return first;
}

void lagfx_sampler_diag_rt_count(bool addr_unknown, bool mip_unknown, bool filter_unknown, uint32_t st) {
    if (!addr_unknown && !mip_unknown && !filter_unknown) return;
    if (addr_unknown) SD_ADD(rt_sampler_objs_addr_out_of_range, 1);
    if (mip_unknown) SD_ADD(rt_sampler_objs_mip_out_of_range, 1);
    if (filter_unknown) SD_ADD(rt_sampler_objs_filter_out_of_range, 1);
    bool first = false;
    pthread_mutex_lock(&g_sd_lock);
    uint32_t i = 0;
    while (i < g_sd_rt_n && g_sd_rt_seen[i] != st) i++;
    if (i == g_sd_rt_n && g_sd_rt_n < SD_LOGGED_MAX) g_sd_rt_seen[g_sd_rt_n++] = st, first = true;
    pthread_mutex_unlock(&g_sd_lock);
    if (first)
        LAGFX_WARN("sampler(runtime) state=0x%08x: value outside the public Metal enum range%s%s%s%s%s -> address 6/7 = clamp-to-edge, mip 3 = nearest, min/mag filter 2/3 = linear (counted, logged once per state word)",
                   st, addr_unknown ? " address_mode(6|7)" : "", addr_unknown && mip_unknown ? "," : "", mip_unknown ? " mip_filter(3)" : "", (addr_unknown || mip_unknown) && filter_unknown ? "," : "", filter_unknown ? " min/mag_filter(2|3)" : "");
}

#ifdef LAGFX_HAVE_VULKAN

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

lagfx_cxs_fid_t lagfx_cxs_map_address(uint32_t v, VkSamplerAddressMode *out) {
    switch (v) {
    case 0: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return LAGFX_CXS_APPROX;   /* clamp_to_zero: transparent-black border (alpha-less formats: INFER) */
    case 1: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return LAGFX_CXS_EXACT;
    case 2: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT; return LAGFX_CXS_EXACT;
    case 3: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return LAGFX_CXS_EXACT;
    case 4: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return LAGFX_CXS_EXACT;   /* colour fixed to transparent black */
    default: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return LAGFX_CXS_UNSUPPORTED;
    }
}

lagfx_cxs_fid_t lagfx_cxs_map_filter(uint32_t v, VkFilter *out) {
    *out = v ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    return v <= 1u ? LAGFX_CXS_EXACT : v == 2u ? LAGFX_CXS_APPROX : LAGFX_CXS_UNSUPPORTED;
}

lagfx_cxs_fid_t lagfx_cxs_map_mip(uint32_t v, VkSamplerMipmapMode *out) {
    *out = v == 2u ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return v <= 2u ? LAGFX_CXS_EXACT : LAGFX_CXS_UNSUPPORTED;
}

bool lagfx_cxs_address_unnorm_legal(VkSamplerAddressMode m) {
    return m == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || m == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
}

/* Count the approximations / unsupported values of one state build and log once per distinct word. */
static void cxs_note(const lagfx_constexpr_sampler_t *cs, bool unnorm_illegal) {
    VkSamplerAddressMode am; VkFilter f; VkSamplerMipmapMode mm;
    bool clampz = false, addr_unsup = false, bic = false, filt_unsup = false, mip_unsup, cmp = cs->compare_func != 8u;
    for (int i = 0; i < 3; i++) {
        lagfx_cxs_fid_t r = lagfx_cxs_map_address(cs->addr[i], &am);
        if (r == LAGFX_CXS_UNSUPPORTED) addr_unsup = true;
        else if (cs->addr[i] == 0u) clampz = true;
    }
    lagfx_cxs_fid_t a = lagfx_cxs_map_filter(cs->mag_filter, &f), b = lagfx_cxs_map_filter(cs->min_filter, &f);
    bic = a == LAGFX_CXS_APPROX || b == LAGFX_CXS_APPROX;
    filt_unsup = a == LAGFX_CXS_UNSUPPORTED || b == LAGFX_CXS_UNSUPPORTED;
    mip_unsup = lagfx_cxs_map_mip(cs->mip_filter, &mm) == LAGFX_CXS_UNSUPPORTED;
    if (!(clampz || addr_unsup || bic || filt_unsup || mip_unsup || cmp || unnorm_illegal)) return;
    if (bic) SD_ADD(cxs_state_builds_bicubic_as_linear, 1);
    if (clampz) SD_ADD(cxs_state_builds_clamp_zero_as_border, 1);
    if (addr_unsup) SD_ADD(cxs_state_builds_addr_outside_table, 1);
    if (filt_unsup) SD_ADD(cxs_state_builds_filter_outside_table, 1);
    if (mip_unsup) SD_ADD(cxs_state_builds_mip_outside_table, 1);
    if (cmp) SD_ADD(cxs_state_builds_compare_field_not8, 1);
    if (unnorm_illegal) SD_ADD(cxs_testpath_state_builds_unnorm_illegal_addr, 1);
    int fl = sd_first_cxs(cs->raw);
    if (fl < 0) SD_ADD(cxs_words_log_suppressed, 1);
    if (fl <= 0) return;
    SD_ADD(cxs_distinct_words_logged, 1);
    char n[256]; int o = 0;
#define NOTE(...) do { if (o < (int)sizeof n) o += snprintf(n + o, sizeof n - (size_t)o, __VA_ARGS__); } while (0)
    if (addr_unsup) NOTE(" addr_outside_table[%u,%u,%u]->clamp_to_edge", cs->addr[0], cs->addr[1], cs->addr[2]);
    if (filt_unsup) NOTE(" filter_outside_table[mag=%u,min=%u]->linear", cs->mag_filter, cs->min_filter);
    if (mip_unsup) NOTE(" mip_outside_table[%u]->nearest", cs->mip_filter);
    if (unnorm_illegal) NOTE(" unnorm_illegal_addr[%u,%u]->kept_normalized", cs->addr[0], cs->addr[1]);
#if LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED
    if (bic) NOTE(" bicubic_as_linear[mag=%u,min=%u](compat mapping)", cs->mag_filter, cs->min_filter);
#else
    if (bic) NOTE(" bicubic_as_linear[mag=%u,min=%u](NOT real bicubic)", cs->mag_filter, cs->min_filter);
#endif
    if (clampz) NOTE(" clamp_zero_as_border");
    if (cmp) NOTE(" compare_ignored[%u]", cs->compare_func);
#undef NOTE
    if (addr_unsup || filt_unsup || mip_unsup || unnorm_illegal)
        LAGFX_WARN("cxs word=0x%016llx fidelity=OUTSIDE_TABLE:%s", (unsigned long long)cs->raw, n);
    else
        LAGFX_LOG("cxs word=0x%016llx fidelity=APPROX:%s", (unsigned long long)cs->raw, n);
}

lagfx_sampler_state_t lagfx_sampler_state_from_constexpr(const lagfx_constexpr_sampler_t *cs) {
    lagfx_sampler_state_t s = lagfx_sampler_state_default();
    if (cs && cs->valid) {
        bool unnorm_illegal = false;
        (void)lagfx_cxs_map_address(cs->addr[0], &s.addr_u); (void)lagfx_cxs_map_address(cs->addr[1], &s.addr_v); (void)lagfx_cxs_map_address(cs->addr[2], &s.addr_w);
        (void)lagfx_cxs_map_filter(cs->mag_filter, &s.mag_filter);   /* bicubic (2) -> LINEAR: counted approximation, not an implementation of bicubic */
        (void)lagfx_cxs_map_filter(cs->min_filter, &s.min_filter);
        (void)lagfx_cxs_map_mip(cs->mip_filter, &s.mip_mode);
        s.lod_min = cs->lod_min;
        s.lod_max = cs->mip_filter ? cs->lod_max : 0.0f;    /* mip filter none: base level only */
        s.max_anisotropy = (float)cs->max_aniso;
        s.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (!cs->normalized) {
            /* Vulkan unnormalized coordinates: edge/border addressing only, no mips,
             * no anisotropy. Applied only when those hold; otherwise the state stays
             * normalized and the caller must handle it (not silently remapped). */
            bool am_ok = lagfx_cxs_address_unnorm_legal(s.addr_u) && lagfx_cxs_address_unnorm_legal(s.addr_v);   /* decided on the MAPPED Vulkan modes, never on raw words */
            unnorm_illegal = !am_ok;
            if (am_ok && cs->mip_filter == 0u && cs->max_aniso == 1u) {
                s.unnormalized = true; s.lod_min = 0.0f; s.lod_max = 0.0f;
                s.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                s.addr_w = s.addr_u;
                s.mag_filter = s.min_filter;   /* Vulkan: the two filters must be equal (the minification one is kept: INFER, as arm64_sampler.c) */
            }
        }
        cxs_note(cs, unnorm_illegal);
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
