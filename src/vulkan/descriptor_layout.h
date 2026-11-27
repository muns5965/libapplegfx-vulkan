/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — descriptor-set + pipeline layout from SPIR-V reflection
 * src/vulkan/descriptor_layout.h
 *
 * Copyright © 2026 Matthew Jackson
 *
 * Builds a single-set (set 0) VkDescriptorSetLayout + VkPipelineLayout from
 * the descriptor bindings a translated SPIR-V shader stage declares (via
 * lagfx_spv_reflect_bindings + the shared binding table, air2spv/binding_alloc.h). The production substitute path uses the
 * device's empty layout because the triangle SPVs declare zero descriptors;
 * a real translated SkyLight pipeline with [[buffer]]/texture args declares
 * DescriptorSet-0 bindings and needs a matching layout or
 * vkCreateGraphicsPipelines fails. This is the host side of that match.
 */
#ifndef LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H
#define LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H

#ifdef LAGFX_HAVE_VULKAN

#include "libapplegfx-vulkan.h"
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Most set-0 bindings one layout holds (the size of every `binds` array handed to the functions below). */
#define LAGFX_LAYOUT_MAX_BINDINGS 128u

typedef struct {
    uint64_t key;                 /* lagfx_ba_key() of the merged table: changes whenever a binding, a descriptor type, a stage set or an input-attachment index changes */
    uint32_t n_conflict;          /* (set, binding) pairs holding two incompatible descriptors (kinds / input-attachment indices / image dimensionality) */
    uint32_t n_overflow;          /* descriptors that did not fit LAGFX_BA_MAX or `cap` */
    uint32_t n_other_sets;        /* bindings of sets other than 0 (not part of the layout: only set 0 is supported) */
    char     report[1024];        /* the binding table + every conflict, human readable */
} lagfx_layout_info_t;

/* The ONE derivation of the set-0 binding table of a pipeline: the stages' SPIR-V is reflected and merged through the shared allocator (air2spv/binding_alloc.h).
 *   - one VkDescriptorSetLayoutBinding per (set 0, binding), sorted by binding; descriptorCount 1;
 *   - stageFlags = exactly the stages that declare the binding (a binding both VS and FS declare with the SAME descriptor type is ONE binding with both stage bits;
 *     input attachments are FRAGMENT only);
 *   - a binding that two descriptors of different types (or input attachments of different index) share is an ERROR: LAGFX_ERR_INTERNAL, nothing is returned in
 *     binds (*n_out = 0), the conflict is in info->report and logged at error level. The old derivation kept the first variable of a binding and dropped the others
 *     silently, which no spirv-val run could notice;
 *   - more than `cap` bindings (or more descriptors than the table holds) is an ERROR too, never a truncation.
 * `binds` must hold `cap` entries (use LAGFX_LAYOUT_MAX_BINDINGS). `info` may be NULL. A module list with no descriptors is LAGFX_OK with *n_out = 0. */
lagfx_status_t lagfx_layout_derive(const uint8_t *const *spv_blobs, const size_t *spv_lens, uint32_t n_stages, VkDescriptorSetLayoutBinding *binds, uint32_t cap,
                                   uint32_t *n_out, lagfx_layout_info_t *info);

/* Build a set-0 descriptor-set layout + pipeline layout covering the union of descriptor bindings declared across `n_stages` translated SPIR-V blobs
 * (typically the vertex + fragment of one pipeline), from lagfx_layout_derive(). On success returns LAGFX_OK and writes the handles to *out_dsl / *out_pl. If the
 * shaders declare ZERO descriptors, both outputs are VK_NULL_HANDLE and the return is LAGFX_OK — the caller should fall back to the device's empty layout. A table
 * with a conflict / overflow returns LAGFX_ERR_INTERNAL and creates nothing.
 *
 * Binding type mapping: SAMPLED_IMAGE->VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SAMPLER->VK_DESCRIPTOR_TYPE_SAMPLER, STORAGE_BUFFER->VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
 * INPUT_ATTACHMENT->VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, STORAGE_IMAGE->VK_DESCRIPTOR_TYPE_STORAGE_IMAGE.
 *
 * Caller owns destruction: vkDestroyPipelineLayout(*out_pl) then vkDestroyDescriptorSetLayout(*out_dsl) when non-NULL. */
lagfx_status_t lagfx_build_pipeline_layout_from_spv(
    VkDevice device,
    const uint8_t *const *spv_blobs,
    const size_t *spv_lens,
    uint32_t n_stages,
    VkDescriptorSetLayout *out_dsl,
    VkPipelineLayout *out_pl);

/* The set-0 bindings lagfx_build_pipeline_layout_from_spv() would create (same derivation), without creating anything: `binds` must hold LAGFX_LAYOUT_MAX_BINDINGS
 * entries. Returns the count, 0 when there are none OR when the table is invalid (lagfx_layout_derive() tells which; the error is logged). */
uint32_t lagfx_layout_bindings_from_spv(const uint8_t *const *spv_blobs, const size_t *spv_lens, uint32_t n_stages, VkDescriptorSetLayoutBinding *binds);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_HAVE_VULKAN */
#endif /* LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H */
