/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — what the device can do with a VkFormat: image usage, blending, sample counts, numeric class.
 * src/vulkan/format_caps.h
 *
 * Source of truth (Khronos Vulkan specification): vkGetPhysicalDeviceFormatProperties2 (VkFormatProperties2::formatProperties.optimalTilingFeatures) and
 * vkGetPhysicalDeviceImageFormatProperties2 (VkPhysicalDeviceImageFormatInfo2 -> VkImageFormatProperties2: maxExtent / maxMipLevels / maxArrayLayers / sampleCounts, or
 * VK_ERROR_FORMAT_NOT_SUPPORTED for that exact {format, type, tiling, usage, flags}). Both are queried through a small backend table so a test can inject a device that
 * says "no" (limits it does not have on lavapipe) — the production backend is a thin wrapper of the two calls above on the VkPhysicalDevice.
 *
 * Why not the one-call feature check the mirror used before: a VkImage is created with ONE usage mask, and a mask is valid only if the combination is (a format that
 * supports STORAGE_IMAGE and COLOR_ATTACHMENT separately can still refuse both in one image, Vulkan 12.3 "Image Creation Limits" + VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251
 * family). lagfx_fmtcap_image_usage() therefore asks for the whole mask, and on VK_ERROR_FORMAT_NOT_SUPPORTED removes OPTIONAL usages one at a time in a fixed order
 * (STORAGE, INPUT_ATTACHMENT, COLOR_ATTACHMENT, SAMPLED) and says which it removed, instead of failing the texture or omitting a bit silently.
 *
 * Pure of protocol state: the callers (arm64_resources.c, arm64_exec_run.c) count what comes back.
 */
#ifndef LIBAPPLEGFX_VULKAN_FORMAT_CAPS_H
#define LIBAPPLEGFX_VULKAN_FORMAT_CAPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#include <stdbool.h>
#include <vulkan/vulkan.h>

typedef struct lagfx_fmtcap_backend {
    void *ctx;
    void (*format_props2)(void *ctx, VkFormat f, VkFormatProperties2 *out);
    VkResult (*image_format_props2)(void *ctx, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out);
} lagfx_fmtcap_backend_t;

/* The production backend over `pd` (storage must outlive the returned pointer). */
const lagfx_fmtcap_backend_t *lagfx_fmtcap_device_backend(VkPhysicalDevice pd, lagfx_fmtcap_backend_t *storage);

/* TEST HOOK: when non-NULL it replaces the production backend inside the library (the exec mirror asks lagfx_fmtcap_active()). Plain global, set by tests only, never from env or input. */
extern const lagfx_fmtcap_backend_t *lagfx_fmtcap_test_backend;
const lagfx_fmtcap_backend_t *lagfx_fmtcap_active(VkPhysicalDevice pd, lagfx_fmtcap_backend_t *storage);

enum {   /* lagfx_fmtcap_usage_t::why */
    LAGFX_FMTCAP_OK = 0,
    LAGFX_FMTCAP_NO_REQUIRED,   /* a required usage is not offered by the format (feature bit missing or the combination is refused) */
    LAGFX_FMTCAP_EXTENT,        /* the width / height / depth exceeds VkImageFormatProperties2::maxExtent */
    LAGFX_FMTCAP_MIPS,          /* mip levels above maxMipLevels */
    LAGFX_FMTCAP_LAYERS,        /* array layers above maxArrayLayers */
    LAGFX_FMTCAP_SAMPLES,       /* the sample count is not in sampleCounts */
    LAGFX_FMTCAP_WHY_N
};

typedef struct {
    VkImageUsageFlags usage;      /* the mask the image may be created with (0 when why != OK) */
    VkImageUsageFlags dropped;    /* OPTIONAL usages that were removed (feature bit missing, or the combination refused) */
    VkSampleCountFlags sample_counts;   /* sampleCounts reported for the granted mask (0 when never answered) */
    int why;                      /* LAGFX_FMTCAP_* */
    uint32_t queries;             /* image-format queries issued */
} lagfx_fmtcap_usage_t;

const char *lagfx_fmtcap_why_name(int why);

/* The usage mask for a VkImage {format, type, optimal tiling, extent, mips, layers, samples}. `required` must all be granted; `optional` bits are granted when the format
 * offers them AND the combined mask is accepted. Returns true (out->why == OK) when an image can be made. COLOR_ATTACHMENT dropped -> INPUT_ATTACHMENT dropped too (the
 * input attachment of this library only ever reads a colour attachment). */
bool lagfx_fmtcap_image_usage(const lagfx_fmtcap_backend_t *be, VkFormat fmt, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers,
                              VkSampleCountFlagBits samples, VkImageUsageFlags required, VkImageUsageFlags optional, lagfx_fmtcap_usage_t *out);

/* optimalTilingFeatures of `fmt` (0 when the backend is NULL). */
VkFormatFeatureFlags lagfx_fmtcap_features(const lagfx_fmtcap_backend_t *be, VkFormat fmt);

/* Blending into an attachment of this format: VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT (VUID-vkCmdDraw-blendEnable-04727: blendEnable must be VK_FALSE for an
 * attachment whose format does not support it). */
bool lagfx_fmtcap_blend_ok(const lagfx_fmtcap_backend_t *be, VkFormat fmt);

/* Sample counts (VkSampleCountFlags) a 2D optimal image of `fmt` with `usage` can have, 0 = none / not supported. 1 is always included when the image is possible at all. */
VkSampleCountFlags lagfx_fmtcap_sample_counts(const lagfx_fmtcap_backend_t *be, VkFormat fmt, VkImageUsageFlags usage);

/* The largest of {4, 2, 1} contained in `mask` (0 when none): the value of the guest's "MSAASamples" device-info field. */
uint32_t lagfx_fmtcap_max_samples(VkSampleCountFlags mask);

enum { LAGFX_FMTCAP_CLASS_FLOAT = 0, LAGFX_FMTCAP_CLASS_UINT, LAGFX_FMTCAP_CLASS_SINT, LAGFX_FMTCAP_CLASS_N };
/* Numeric class of a colour format (UINT / SINT formats are read by integer sampled types, all others by float ones: VUID-vkCmdDraw-format-07753 family). Depth / stencil: FLOAT. */
int lagfx_fmtcap_class(VkFormat fmt);
const char *lagfx_fmtcap_class_name(int cls);

/* One line per format of `list` (a library format table), then a summary; every emit is <= 400 chars. Counts formats lacking each capability the library needs (all
 * out parameters optional). Returns the number of lines. */
typedef void (*lagfx_fmtcap_emit_fn)(void *user, const char *line);
typedef struct { uint32_t formats, no_transfer, no_sampled, no_color_att, no_blend, no_storage, no_input, no_msaa4, no_msaa2; } lagfx_fmtcap_summary_t;
size_t lagfx_fmtcap_report(const lagfx_fmtcap_backend_t *be, const VkFormat *list, const uint32_t *mtl, size_t n, lagfx_fmtcap_emit_fn emit, void *user, lagfx_fmtcap_summary_t *sum);

#endif /* LAGFX_HAVE_VULKAN */
#endif /* LIBAPPLEGFX_VULKAN_FORMAT_CAPS_H */
