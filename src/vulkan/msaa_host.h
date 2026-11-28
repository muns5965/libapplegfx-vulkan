/*
 * libapplegfx-vulkan — host-side multisample machinery that is NOT reachable from the guest yet: multisample texel fetch, sample id / sample mask, alpha-to-coverage, depth / stencil resolve.
 * src/vulkan/msaa_host.h
 *
 * SPDX-License-Identifier: MIT
 *
 * STATUS (per item): implemented + build-verified + lavapipe-tested (tests/vulkan-msaa-host-unit.c) = YES; wired to the guest protocol / the AIR translator = NO; device-verified = NO.
 * WHY unwired: the AIR spelling of texture2d_ms reads, get_num_samples, [[sample_id]] / [[sample_mask]] is not in any fixture or dump in hand (src/air2spv/msaa_air_scan.h), the
 * pipeline descriptor's alpha-to-coverage / sample-shading fields are not decoded, and the depth / stencil resolve filter fields of the pass descriptor are not exercised by any capture.
 * Nothing here guesses a name or a wire field: the SPIR-V generators take the semantics as parameters, the entry points are marked UNWIRED, and the exact-mode refusal counters stay in place
 * (sampled_ms_refused, air_ms_ops, air_sample_builtins, ds_resolve_unsupported) so a guest run that needs them is still counted, not approximated.
 *
 * Official basis: SPIR-V specification (OpTypeImage MS operand, OpImageFetch + ImageOperands Sample, OpImageQuerySamples (capability ImageQuery), BuiltIn SampleId (capability
 * SampleRateShading) / SampleMask, OpExecutionMode OriginUpperLeft); Vulkan specification (VkPipelineMultisampleStateCreateInfo: sampleShadingEnable / minSampleShading (feature
 * sampleRateShading), alphaToCoverageEnable; "Multisample Coverage" alpha-to-coverage rules; VkPhysicalDeviceDepthStencilResolveProperties, VkRenderingAttachmentInfo::resolveMode);
 * Metal: MTLMultisampleDepthResolveFilter, MTLMultisampleStencilResolveFilter, MTLRenderPipelineDescriptor.alphaToCoverageEnabled. No GPL / LGPL code is a basis.
 */
#ifndef LAGFX_VULKAN_MSAA_HOST_H
#define LAGFX_VULKAN_MSAA_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "msaa_stats.h"

/* ---- (a) multisample texel fetch ---------------------------------------------------------------------------------------------------------------------------------------- */

/* A compute shader (SPIR-V 1.4, Vulkan 1.1 environment + ImageQuery) that reads EVERY sample of a multisampled 2D image with OpImageFetch + the Sample image operand:
 *   set 0 binding 0  texture2DMS (OpTypeImage ... Dim 2D, Depth 0, Arrayed 0, MS 1, Sampled 1, Unknown), sampled type float (uint_texels = false) or uint (true)
 *   set 0 binding 1  storage buffer, vec4 / uvec4 array: element (y * width + x) * samples + s holds sample s of pixel (x, y); element width * height * samples holds the
 *                    OpImageQuerySamples result (as float / uint) in .x
 *   one invocation per pixel (LocalSize 1 1 1; dispatch width x height x 1).
 * `samples` is the number of fetches emitted (unrolled, 1..16). Returns a malloc'd module (caller frees), NULL on failure. */
uint32_t *lagfx_msaa_spv_fetch_cs(uint32_t width, uint32_t height, uint32_t samples, bool uint_texels, size_t *bytes);

/* A multisampled image the shaders may read: the whole-image 2D view (never an array view, never a 3D view) of an image created with SAMPLED usage and more than one sample.
 * UNWIRED entry point (the exec mirror still refuses sampled multisample textures: sampled_ms_refused). Counts ms_fetch_views / ms_fetch_refused in `st` (may be NULL).
 * false (nothing created) when samples <= 1, the usage lacks SAMPLED, or vkCreateImageView fails. The descriptor type of such a view is VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE like any other. */
bool lagfx_msaa_ms_sampled_view(VkDevice dev, VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkSampleCountFlagBits samples, VkImageUsageFlags usage, lagfx_msaa_stats_t *st, VkImageView *out);

/* ---- (b) sample id / sample mask --------------------------------------------------------------------------------------------------------------------------------------- */
typedef enum {
    LAGFX_MSAA_FRAG_SAMPLE_ID = 1,        /* capability SampleRateShading; colour = (float(SampleId + 1) / 8, 0, 0, 1): every sample gets its own value. Needs sample shading (minSampleShading 1.0) in the pipeline */
    LAGFX_MSAA_FRAG_SAMPLE_MASK_OUT,      /* writes the SampleMask output (word 0 = `mask`); colour = (1, 0, 0, 1): a sample is written only when covered AND its mask bit is set */
    LAGFX_MSAA_FRAG_SAMPLE_MASK_IN,       /* reads the SampleMask input (word 0); colour = (float(mask), 0, 0, 1) with mask = the covered samples of the pixel (no sample shading) */
    LAGFX_MSAA_FRAG_ALPHA                 /* colour = (1, 0, 0, alpha): constant alpha for alpha-to-coverage */
} lagfx_msaa_frag_kind_t;
/* Fragment shader of the kinds above (location 0 vec4 output, OriginUpperLeft). `mask` is used by MASK_OUT, `alpha` by ALPHA. malloc'd, NULL on failure. */
uint32_t *lagfx_msaa_spv_frag(lagfx_msaa_frag_kind_t kind, uint32_t mask, float alpha, size_t *bytes);

/* ---- (c) alpha-to-coverage / sample shading plumbing ---------------------------------------------------------------------------------------------------------------------- */
/* TEST / INJECTION HOOK (UNWIRED: the pipeline descriptor's alpha-to-coverage and sample-shading bits are not decoded [N]): when non-NULL, the arm64 executor applies it to every
 * pipeline it builds (src/protocol/arm64_exec_run.c pipeline_for). A plain global set by tests only, never from env or guest data. */
typedef struct { uint8_t alpha_to_coverage; uint8_t sample_shading; uint32_t min_sample_shading_permille; } lagfx_msaa_inject_t;   /* min_sample_shading in 1/1000 */
extern const lagfx_msaa_inject_t *lagfx_msaa_inject;

/* ---- (d) depth / stencil resolve ----------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct {
    bool probed;                       /* the device answered VkPhysicalDeviceDepthStencilResolveProperties (core 1.2 or VK_KHR_depth_stencil_resolve) */
    VkResolveModeFlags depth_modes, stencil_modes;   /* supportedDepthResolveModes / supportedStencilResolveModes */
    bool independent_none, independent;              /* independentResolveNone / independentResolve */
} lagfx_msaa_dsres_caps_t;
/* Queries the physical device (vkGetPhysicalDeviceProperties2). Returns caps->probed. Core 1.2, or 1.1 with VK_KHR_depth_stencil_resolve advertised (otherwise false: not probed, nothing guessed). */
bool lagfx_msaa_dsres_probe(VkPhysicalDevice pd, lagfx_msaa_dsres_caps_t *caps);

/* Metal filters (numbering of the MTL enums, [I] for the guest wire): depth 0 Sample0, 1 Min, 2 Max; stencil 0 Sample0, 1 DepthResolvedSample. */
enum { LAGFX_MSAA_DEPTH_FILTER_SAMPLE0 = 0, LAGFX_MSAA_DEPTH_FILTER_MIN = 1, LAGFX_MSAA_DEPTH_FILTER_MAX = 2, LAGFX_MSAA_STENCIL_FILTER_SAMPLE0 = 0, LAGFX_MSAA_STENCIL_FILTER_DEPTH_RESOLVED = 1 };
enum { LAGFX_MSAA_DSRES_OK = 0, LAGFX_MSAA_DSRES_NOT_PROBED, LAGFX_MSAA_DSRES_UNKNOWN_FILTER, LAGFX_MSAA_DSRES_MODE_UNSUPPORTED, LAGFX_MSAA_DSRES_INDEPENDENT_UNSUPPORTED, LAGFX_MSAA_DSRES_NO_ASPECT };
const char *lagfx_msaa_dsres_why_name(int why);
/* Chooses the resolve modes for a depth and / or stencil attachment that is resolved: depth filter -> SAMPLE_ZERO / MIN / MAX; stencil filter Sample0 -> SAMPLE_ZERO, DepthResolvedSample ->
 * the same mode as depth when that mode is also a supported stencil mode (Vulkan has no "the sample the depth resolve picked" for MIN / MAX of another aspect: refused, never approximated).
 * A mode the device does not list, or two different modes on one image without independentResolve, is refused (LAGFX_MSAA_DSRES_*), not replaced by another mode. *dmode / *smode = the chosen
 * modes (0 for an aspect that is not resolved). */
int lagfx_msaa_dsres_choose(const lagfx_msaa_dsres_caps_t *caps, bool depth, uint32_t depth_filter, bool stencil, uint32_t stencil_filter, VkResolveModeFlagBits *dmode, VkResolveModeFlagBits *smode);
/* Fills the resolve half of a depth and / or stencil VkRenderingAttachmentInfo (the caller set view / load / store / clear): resolveMode, resolveImageView, resolveImageLayout. Counts
 * ds_resolve_refused in `st` on a refusal. UNWIRED into the pass path (the pass still rejects a depth / stencil resolve in exact mode: ds_resolve_unsupported). */
int lagfx_msaa_dsres_attachments(const lagfx_msaa_dsres_caps_t *caps, bool depth, uint32_t depth_filter, bool stencil, uint32_t stencil_filter, VkImageView resolve_view, VkImageLayout resolve_layout,
                                 VkRenderingAttachmentInfo *dai, VkRenderingAttachmentInfo *sai, lagfx_msaa_stats_t *st);

#endif
