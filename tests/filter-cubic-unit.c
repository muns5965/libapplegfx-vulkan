/*
 * Opt-in cubic filtering: the pure selection logic (src/vulkan/filter_cubic.c). No device: the exec-level fallbacks run in arm64-exec-texview-unit.c.
 * What this does NOT show: that Metal would sample bicubically, which kernel, or what the Adreno driver's VK_FILTER_CUBIC_EXT result looks like.
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef LAGFX_HAVE_VULKAN
#include "vulkan/filter_cubic.h"

int main(void) {
#ifdef VK_FILTER_CUBIC_EXT
    assert(LAGFX_FILTER_CUBIC == VK_FILTER_CUBIC_EXT);
#endif
#ifdef VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT
    assert(LAGFX_FMT_FEATURE_FILTER_CUBIC == (uint32_t)VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT);
#endif
    assert(LAGFX_FMT_FEATURE_FILTER_CUBIC == 0x2000u && LAGFX_FILTER_CUBIC == (VkFilter)1000015000);

    /* the env switch: exactly "1" */
    unsetenv("LAGFX_CUBIC"); assert(!lagfx_cubic_optin());
    setenv("LAGFX_CUBIC", "0", 1); assert(!lagfx_cubic_optin());
    setenv("LAGFX_CUBIC", "yes", 1); assert(!lagfx_cubic_optin());
    setenv("LAGFX_CUBIC", "1", 1); assert(lagfx_cubic_optin());
    unsetenv("LAGFX_CUBIC");

    const uint32_t cub = LAGFX_FMT_FEATURE_FILTER_CUBIC, other = 0x1u | 0x1000u;   /* sampled | linear filter, no cubic */
    const lagfx_cubic_ctx_t on = { true, true }, noext = { true, false }, off = { false, true }, off2 = { false, false };
    /* the decision table, in the order of the enum */
    assert(lagfx_cubic_decide(&on, cub, true, true) == LAGFX_CUBIC_USE);
    assert(lagfx_cubic_decide(&on, cub | other, true, true) == LAGFX_CUBIC_USE);
    assert(lagfx_cubic_decide(&off, cub, true, true) == LAGFX_CUBIC_NO_OPTIN);     /* the default: never cubic, even with everything available */
    assert(lagfx_cubic_decide(&off2, 0, false, false) == LAGFX_CUBIC_NO_OPTIN);
    assert(lagfx_cubic_decide(NULL, cub, true, true) == LAGFX_CUBIC_NO_OPTIN);
    assert(lagfx_cubic_decide(&noext, cub, true, true) == LAGFX_CUBIC_NO_EXT);     /* capability absent -> linear + counter */
    assert(lagfx_cubic_decide(&noext, 0, false, false) == LAGFX_CUBIC_NO_EXT);
    assert(lagfx_cubic_decide(&on, other, true, true) == LAGFX_CUBIC_NO_FORMAT);
    assert(lagfx_cubic_decide(&on, 0, true, true) == LAGFX_CUBIC_NO_FORMAT);
    assert(lagfx_cubic_decide(&on, cub, false, true) == LAGFX_CUBIC_NO_VIEWTYPE);
    assert(lagfx_cubic_decide(&on, other, false, true) == LAGFX_CUBIC_NO_FORMAT);   /* the first failing reason wins */
    assert(lagfx_cubic_decide(&on, cub, true, false) == LAGFX_CUBIC_NO_COMBO);
    assert(lagfx_cubic_decide(&on, cub, false, false) == LAGFX_CUBIC_NO_VIEWTYPE);
    /* the two formats the Adreno 840 log reports with SAMPLED_IMAGE_FILTER_CUBIC=1 are a property of that device, not something this unit can show (the test injects bits) */

    /* the create info: only the requested filters change */
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .minFilter = VK_FILTER_LINEAR, .magFilter = VK_FILTER_LINEAR, .maxLod = 7.f, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR };
    VkSamplerCreateInfo c2 = ci;
    lagfx_cubic_apply(&c2, false, false); assert(!memcmp(&c2, &ci, sizeof ci));
    lagfx_cubic_apply(&c2, true, false); assert(c2.minFilter == LAGFX_FILTER_CUBIC && c2.magFilter == VK_FILTER_LINEAR && c2.maxLod == 7.f && c2.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    c2 = ci; lagfx_cubic_apply(&c2, false, true); assert(c2.minFilter == VK_FILTER_LINEAR && c2.magFilter == LAGFX_FILTER_CUBIC);
    c2 = ci; lagfx_cubic_apply(&c2, true, true); assert(c2.minFilter == LAGFX_FILTER_CUBIC && c2.magFilter == LAGFX_FILTER_CUBIC);

    for (int w = 0; w < LAGFX_CUBIC_NR; w++) assert(strcmp(lagfx_cubic_why_name((lagfx_cubic_why_t)w), "?") != 0);
    assert(!strcmp(lagfx_cubic_why_name(LAGFX_CUBIC_NR), "?"));
    puts("PASS: cubic opt-in decision table, env switch, sampler create-info mapping");
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
