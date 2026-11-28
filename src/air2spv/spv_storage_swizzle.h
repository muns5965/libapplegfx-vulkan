/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan - red/blue swap of chosen storage-image bindings (a SPIR-V -> SPIR-V pass)
 *
 * Why: Adreno 730 has no STORAGE_IMAGE feature for B8G8R8A8_UNORM (the only storage-image format the S8 logs refused: VkFormat 44), but it has it for R8G8B8A8_UNORM, and a B8G8R8A8 image can be
 * viewed as R8G8B8A8 (same texel size class; mutable format + extended usage). Memory byte order is what the rest of the pipeline sees (sampling, copies, the screen), so a shader that writes the
 * LOGICAL texel (r, g, b, a) through an R8G8B8A8 view must write (b, g, r, a): the stored bytes are then exactly what a native BGRA8 storage image would hold. Reads are swapped back.
 *
 * The pass rewrites OpImageWrite texels and OpImageRead results of the listed (descriptor set, binding) pairs with OpVectorShuffle 2 1 0 3. It is conservative: if such a storage image is used
 * in any way other than a direct OpLoad -> OpImageRead / OpImageWrite (passed to a function, indexed array of images, texel pointer for atomics, non-4-component texel) it returns -1 and the
 * caller must not use the surrogate for that shader.
 */
#ifndef LAGFX_AIR2SPV_SPV_STORAGE_SWIZZLE_H
#define LAGFX_AIR2SPV_SPV_STORAGE_SWIZZLE_H
#include <stddef.h>
#include <stdint.h>

/* pairs: n (set, binding) pairs. Returns 1 and a malloc'ed module (*out, *out_len) when at least one access was rewritten, 0 (nothing allocated) when none of the pairs is used by the module,
 * -1 when it cannot be done safely (reason in `why`, may be NULL) or on allocation failure. */
int lagfx_spv_swizzle_storage_rb(const uint8_t *in, size_t in_len, const uint32_t (*pairs)[2], size_t n, uint8_t **out, size_t *out_len, char *why, size_t why_cap);
#endif
