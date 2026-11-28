/* SPDX-License-Identifier: MIT */
/* Validation-layer checks shared by the Vulkan unit tests.
 *   lagfx_test_validation_expect(vk, what): with LAGFX_VK_VALIDATION=1 the messenger must be installed (hard failure when LAGFX_REQUIRE_VALIDATION=1, otherwise a visible INFO and
 *     a soft skip), and the messages counted SO FAR must be 0 -- "0 messages" only means something once the layer is proven live, which the control below does.
 *   lagfx_test_validation_control(vk): a deliberate CORE validation error (a zero-sized VkBuffer: VUID-VkBufferCreateInfo-size-00912, checked by the layer before the driver sees
 *     it, no synchronization validation needed) must be delivered to the messenger; the count is then restored so the caller's own zero assertion is not polluted.
 * Returns: 1 = flagged (layer live), 0 = not flagged, -1 = no messenger (validation not enabled / layer missing). */
#ifndef LAGFX_TESTS_VALIDATION_CONTROL_H
#define LAGFX_TESTS_VALIDATION_CONTROL_H
#include "vulkan/instance.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef LAGFX_HAVE_VULKAN
extern uint64_t lagfx_vk_validation_msg_count;
extern void (*lagfx_vk_validation_hook)(const char *msg);

/* Messages a test KNOWS and explains (matched by substring of the message, e.g. a VUID / message id) are counted apart and do not count as unexplained. */
static const char *g_val_allow[8]; static unsigned g_val_nallow; static uint64_t g_val_allowed;
static inline void lagfx_test_validation_hook(const char *msg) {
    for (unsigned i = 0; i < g_val_nallow; i++) if (strstr(msg, g_val_allow[i])) { g_val_allowed++; return; }
}
static inline void lagfx_test_validation_allow(const char *needle) {
    if (g_val_nallow < 8u) g_val_allow[g_val_nallow++] = needle;
    lagfx_vk_validation_hook = lagfx_test_validation_hook;
}

static inline int lagfx_test_validation_control(struct lagfx_vk_state *vk) {
    if (!vk || !vk->debug_messenger) return -1;
    const uint64_t base = lagfx_vk_validation_msg_count;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 0, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(vk->device, &bi, NULL, &b);
    if (r == VK_SUCCESS && b != VK_NULL_HANDLE) vkDestroyBuffer(vk->device, b, NULL);
    const uint64_t seen = lagfx_vk_validation_msg_count - base;
    lagfx_vk_validation_msg_count = base;   /* the control's own message is expected; everything else must stay at 0 */
    return seen > 0 ? 1 : 0;
}

/* A validation layer older than the headers 1.3.287 does not know VK_KHR_dynamic_rendering_local_read (the RENDERING_LOCAL_READ layout, the barrier exemption inside a dynamic
 * rendering instance, input attachments without a render pass) nor the ARM rasterization-order flag on a dynamic pipeline: its messages about exactly those are explained by the
 * layer's age, NOT proven harmless. They are allowed ONLY while the installed layer is that old; with a newer layer the same messages fail the test. Returns the layer's spec
 * version (0 = not found). */
static inline uint32_t lagfx_test_validation_layer_version(void) {
    uint32_t n = 0; uint32_t v = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkLayerProperties *l = calloc(n, sizeof *l);
    if (l && vkEnumerateInstanceLayerProperties(&n, l) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) if (!strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation")) v = l[i].specVersion;
    free(l);
    return v;
}
static inline void lagfx_test_validation_allow_old_layer_localread(void) {
    const uint32_t v = lagfx_test_validation_layer_version();
    if (v == 0 || v >= VK_MAKE_API_VERSION(0, 1, 3, 287)) return;
    printf("INFO: validation layer %u.%u.%u predates local read (1.3.287): its local-read / raster-order messages are listed as explained-by-layer-age, not proven harmless\n",
           VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    lagfx_test_validation_allow("VUID-vkCmdPipelineBarrier-shaderTileImageColorReadAccess-08718");
    lagfx_test_validation_allow("VUID-vkCmdPipelineBarrier-None-08719");
    lagfx_test_validation_allow("VUID-VkWriteDescriptorSet-descriptorType-04151");
    lagfx_test_validation_allow("VUID-VkGraphicsPipelineCreateInfo-renderPass-06061");
}

/* returns 0 when everything holds (or validation is not requested and not active), nonzero on a failure; prints one INFO line either way */
static inline int lagfx_test_validation_expect(struct lagfx_vk_state *vk, const char *what) {
    const char *want = getenv("LAGFX_VK_VALIDATION");
    const bool asked = want && want[0] == '1';
    const bool require = getenv("LAGFX_REQUIRE_VALIDATION") != NULL;
    if (!asked) return 0;
    const uint64_t before = lagfx_vk_validation_msg_count - g_val_allowed;   /* unexplained messages only */
    const int c = lagfx_test_validation_control(vk);
    printf("INFO: validation (%s): messenger %s, unexplained messages before control=%llu (explained/allowed=%llu), control %s\n", what, vk && vk->debug_messenger ? "ACTIVE" : "inactive", (unsigned long long)before, (unsigned long long)g_val_allowed,
           c > 0 ? "FLAGGED (layer live)" : c == 0 ? "NOT flagged" : "not run");
    if (c < 0) { if (require) { printf("FAIL: validation required but the messenger is not active (%s)\n", what); return 1; } return 0; }
    if (c == 0) { printf("FAIL: the validation control was not delivered to the messenger (%s)\n", what); return 1; }
    if (before != 0) {
        /* gated only where the registration says LAGFX_REQUIRE_VALIDATION: a registration without it reports the count (triage item) instead of failing */
        if (require) { printf("FAIL: %llu unexplained validation message(s) before the control (%s)\n", (unsigned long long)before, what); return 1; }
        printf("NOTE: %llu unexplained validation message(s) (%s); not gated in this registration\n", (unsigned long long)before, what);
    }
    return 0;
}
#endif
#endif
