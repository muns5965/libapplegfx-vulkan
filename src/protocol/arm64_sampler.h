/*
 * libapplegfx-vulkan — ARM64 exec: serialized sampler (object payload tag 3) -> VkSamplerCreateInfo.
 * src/protocol/arm64_sampler.h
 *
 * Payload (36 bytes allocated, the first 28 written): {u32 tag = 3, u32 size, u32 ref, u32 state, u8 flags (+3 bytes unwritten), f32 lod min, f32 lod max,
 * 8 bytes never written}. state bit fields:
 *   [0] min filter  [2] mag filter  [4] lodAverage  [7:5] compare function  [10:8] / [14:12] / [18:16] address mode S / T / R  [21:20] border colour
 *   [25:24] mip filter (0 not mip mapped, 1 nearest, 2 linear)  [30:26] max anisotropy (1..16, verbatim)  [31] normalized coordinates (1 = normalized)
 * MTLSamplerAddressMode: 0 clamp to edge, 1 mirror clamp to edge, 2 repeat, 3 mirror repeat, 4 clamp to zero, 5 clamp to border colour.
 * MTLSamplerBorderColor: 0 transparent black, 1 opaque black, 2 opaque white. MTLCompareFunction 0..7 = VkCompareOp order.
 * Source of the bit assignments: the independent derivation in medfa12/apple-silicon-anyos-project (pvg-vulkan-wire ops/sampler.rs), which agrees with
 * the previous reading of pvg_metal.m makeSerialized in this file.
 *
 * Mapped: filters, mip filter, address modes, border colour (only when an axis clamps to the border colour; clamp to zero is transparent black), LOD clamps
 * (the FLT_MAX default becomes VK_LOD_CLAMP_NONE; a descriptor that is not mip mapped samples level 0 only), anisotropy (when the device enables the feature,
 * clamped to its limit), unnormalized coordinates (with Vulkan's restrictions: equal filters, level 0 only, clamp addressing on S / T, no anisotropy; caveat:
 * Vulkan formally allows such a sampler only with explicit-LOD sampling instructions and the translator emits implicit-LOD ones, which drivers in practice
 * accept). Not mapped (reported in the returned flags): the compare function (the translator emits no depth-compare sampling, and a compare sampler used by a
 * plain sample is invalid Vulkan), lodAverage, and mirror clamp to edge (needs VK_KHR_sampler_mirror_clamp_to_edge: mirror repeat is used).
 */
#ifndef LAGFX_PROTOCOL_ARM64_SAMPLER_H
#define LAGFX_PROTOCOL_ARM64_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

enum {
    LAGFX_A64_SAMP_MIPMAPPED          = 1u,   /* the descriptor is mip mapped (LOD clamps apply) */
    LAGFX_A64_SAMP_COMPARE_IGNORED    = 2u,   /* compare function != never was not mapped */
    LAGFX_A64_SAMP_UNNORM_APPLIED     = 4u,   /* unnormalized coordinates: mapped with Vulkan's restrictions (equal filters, no mips, clamp addressing, no anisotropy) */
    LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX = 8u,  /* mirror clamp to edge was mapped to mirror repeat */
    LAGFX_A64_SAMP_ANISO_APPLIED      = 16u,
    LAGFX_A64_SAMP_LODAVG_IGNORED     = 32u,  /* lodAverage (state bit 4) was set: not mapped */
    LAGFX_A64_SAMP_ADDR_UNKNOWN       = 64u,  /* an address mode field was 6 or 7 (undefined): mapped to clamp-to-edge (compat fallback, counted) */
    LAGFX_A64_SAMP_MIP_UNKNOWN        = 128u, /* the mip filter field was 3 (undefined): treated as nearest-with-LOD (compat fallback, counted) */
};

/* d = the first n >= 20 bytes of the payload (a record that ends before the LOD clamps gets the descriptor defaults 0 / FLT_MAX). Pure: no Vulkan calls.
 * have_aniso / max_aniso describe the device (samplerAnisotropy enabled, limits.maxSamplerAnisotropy). Returns LAGFX_A64_SAMP_* flags. */
uint32_t lagfx_a64_sampler_decode(const uint8_t *d, uint32_t n, bool have_aniso, float max_aniso, VkSamplerCreateInfo *ci);
/* The same with have_mirror_clamp = the device enables samplerMirrorClampToEdge (Vulkan 1.2): MTLSamplerAddressModeMirrorClampToEdge then maps to
 * VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE exactly (no LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX); without it, mirrored repeat as before. */
uint32_t lagfx_a64_sampler_decode_ex(const uint8_t *d, uint32_t n, bool have_aniso, float max_aniso, bool have_mirror_clamp, VkSamplerCreateInfo *ci);
#endif

#endif
