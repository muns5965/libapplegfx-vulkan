/*
 * libapplegfx-vulkan — VkSampler cache by state (unit, lavapipe)
 * tests/vulkan-sampler-cache-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Distinct sampler states must give distinct VkSamplers, equal states one
 * shared VkSampler, and every decoded constexpr word seen in the captured
 * modules (which differ only in undecoded bits) the default sampler.
 */

#include "libapplegfx-vulkan.h"
#include <stdio.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { fprintf(stderr, "built without Vulkan; skipping\n"); return 77; }
#else

#include "vulkan/sampler_cache.h"
#include <stdlib.h>
#include <string.h>

void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no Vulkan instance; skipping\n"); return 77; }
    uint32_t np = 0;
    vkEnumeratePhysicalDevices(inst, &np, NULL);
    if (!np) { vkDestroyInstance(inst, NULL); fprintf(stderr, "no physical device; skipping\n"); return 77; }
    VkPhysicalDevice pds[8]; if (np > 8) np = 8;
    vkEnumeratePhysicalDevices(inst, &np, pds);
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < np; i++) {   /* prefer a software ICD (lavapipe) */
        VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; }
    }
    VkPhysicalDeviceFeatures pf; vkGetPhysicalDeviceFeatures(pd, &pf);
    VkPhysicalDeviceFeatures ef = { .samplerAnisotropy = pf.samplerAnisotropy };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .pEnabledFeatures = &ef };
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { vkDestroyInstance(inst, NULL); fprintf(stderr, "no device; skipping\n"); return 77; }

    lagfx_sampler_cache_t c;
    lagfx_sampler_cache_init(&c, dev, pf.samplerAnisotropy == VK_TRUE);

    lagfx_sampler_state_t def = lagfx_sampler_state_default();
    VkSampler sdef = lagfx_sampler_cache_get(&c, &def);
    CHECK(sdef != VK_NULL_HANDLE);
    CHECK(lagfx_sampler_cache_get(&c, &def) == sdef);
    CHECK(lagfx_sampler_cache_count(&c) == 1);

    /* The four captured words decode (external layout) to four distinct states:
     * pixel+linear+zero, pixel+nearest+zero, normalized nearest repeat, normalized
     * linear clamp-to-edge. The same word twice -> the same VkSampler. */
    const uint64_t words[4] = { 0x807bff0000088a00ull, 0x807bff0000088000ull,
                                0x807bff0000080092ull, 0x807bff0000080a49ull };
    VkSampler ws[4];
    for (int i = 0; i < 4; i++) {
        lagfx_constexpr_sampler_t cs;
        CHECK(lagfx_constexpr_sampler_decode(words[i], &cs));
        lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr(&cs);
        CHECK(st.unnormalized == (i < 2));
        ws[i] = lagfx_sampler_cache_get(&c, &st);
        CHECK(ws[i] != VK_NULL_HANDLE);
        CHECK(lagfx_sampler_cache_get(&c, &st) == ws[i]);
        for (int j = 0; j < i; j++) CHECK(ws[i] != ws[j]);
    }
    /* Undecodable word / NULL -> the default state. */
    lagfx_constexpr_sampler_t bad;
    CHECK(!lagfx_constexpr_sampler_decode(0x0ull, &bad));
    lagfx_sampler_state_t sb = lagfx_sampler_state_from_constexpr(&bad);
    CHECK(lagfx_sampler_cache_get(&c, &sb) == sdef);
    lagfx_sampler_state_t sn = lagfx_sampler_state_from_constexpr(NULL);
    CHECK(lagfx_sampler_cache_get(&c, &sn) == sdef);
    CHECK(lagfx_sampler_cache_count(&c) == 5);

    /* A decoded, non-default LOD clamp is a distinct state. */
    lagfx_constexpr_sampler_t cs2;
    CHECK(lagfx_constexpr_sampler_decode(0x8000000000000000ull | (0x4400ull << 40) | (0x3c00ull << 24) | (1ull << 13), &cs2));  /* [1, 4], mip nearest */
    lagfx_sampler_state_t sl = lagfx_sampler_state_from_constexpr(&cs2);
    CHECK(sl.lod_min == 1.0f && sl.lod_max == 4.0f);

    /* Distinct states -> distinct samplers; repeated -> same handle. */
    lagfx_sampler_state_t v[8];
    for (int i = 0; i < 8; i++) v[i] = def;
    v[0] = sl;
    v[1].mag_filter = VK_FILTER_NEAREST;
    v[2].addr_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    v[3].mip_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    v[4].compare_enable = true; v[4].compare_op = VK_COMPARE_OP_LESS;
    v[5].addr_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; v[5].border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    v[6].lod_max = 3.0f;
    v[7].unnormalized = true; v[7].lod_max = 0.0f; v[7].mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSampler h[8];
    for (int i = 0; i < 8; i++) {
        h[i] = lagfx_sampler_cache_get(&c, &v[i]);
        CHECK(h[i] != VK_NULL_HANDLE);
        CHECK(h[i] != sdef);
        for (int j = 0; j < i; j++) CHECK(h[i] != h[j]);
    }
    CHECK(lagfx_sampler_cache_count(&c) == 13);
    for (int i = 0; i < 8; i++) CHECK(lagfx_sampler_cache_get(&c, &v[i]) == h[i]);
    CHECK(lagfx_sampler_cache_count(&c) == 13);

    /* lod_max >= 1000 is the same state as "no clamp". */
    lagfx_sampler_state_t big = def; big.lod_max = 65504.0f;
    CHECK(lagfx_sampler_cache_get(&c, &big) == sdef);
    /* Anisotropy only when the device supports it; otherwise it folds to a
     * plain sampler of the same state minus aniso (still created). */
    lagfx_sampler_state_t an = def; an.max_anisotropy = 8.0f;
    VkSampler sa = lagfx_sampler_cache_get(&c, &an);
    CHECK(sa != VK_NULL_HANDLE && sa != sdef);

    /* The cache is bounded: once full a new state returns VK_NULL_HANDLE
     * and the existing entries still resolve. */
    for (uint32_t i = 0; lagfx_sampler_cache_count(&c) < LAGFX_SAMPLER_CACHE_MAX; i++) {
        lagfx_sampler_state_t t = def; t.lod_max = 10.0f + (float)i;
        CHECK(lagfx_sampler_cache_get(&c, &t) != VK_NULL_HANDLE);
    }
    lagfx_sampler_state_t extra = def; extra.lod_max = 500.0f;
    CHECK(lagfx_sampler_cache_get(&c, &extra) == VK_NULL_HANDLE);
    CHECK(lagfx_sampler_cache_get(&c, &def) == sdef);

    lagfx_sampler_cache_destroy(&c);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    printf("vulkan sampler cache unit: ok\n");
    return 0;
}
#endif
