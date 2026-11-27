/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — descriptor-set + pipeline layout from SPIR-V reflection
 * src/vulkan/descriptor_layout.c
 *
 * Copyright © 2026 Matthew Jackson
 */
#include "descriptor_layout.h"

#ifdef LAGFX_HAVE_VULKAN

#include "air2spv/binding_alloc.h"
#include "air2spv/spv_reflect.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

static VkDescriptorType kind_to_vk(lagfx_spv_binding_kind_t k) {
    switch (k) {
        case LAGFX_SPV_BINDING_SAMPLED_IMAGE:  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case LAGFX_SPV_BINDING_SAMPLER:        return VK_DESCRIPTOR_TYPE_SAMPLER;
        case LAGFX_SPV_BINDING_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case LAGFX_SPV_BINDING_INPUT_ATTACHMENT: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case LAGFX_SPV_BINDING_STORAGE_IMAGE:  return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        default:                               return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

static VkShaderStageFlags stages_to_vk(uint32_t st, VkDescriptorType t) {
    VkShaderStageFlags f = 0;
    if (st & LAGFX_BA_STAGE_VERTEX) f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (st & LAGFX_BA_STAGE_FRAGMENT) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (st & LAGFX_BA_STAGE_COMPUTE) f |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) f = VK_SHADER_STAGE_FRAGMENT_BIT;   /* VUID-VkDescriptorSetLayoutBinding-descriptorType-01510 */
    return f;
}

lagfx_status_t lagfx_layout_derive(const uint8_t *const *spv_blobs, const size_t *spv_lens, uint32_t n_stages, VkDescriptorSetLayoutBinding *binds, uint32_t cap,
                                   uint32_t *n_out, lagfx_layout_info_t *info) {
    if (n_out) *n_out = 0;
    if (info) memset(info, 0, sizeof *info);
    if (!spv_blobs || !spv_lens || !binds || !n_out) return LAGFX_ERR_INVALID_ARG;
    lagfx_ba_t *ba = malloc(sizeof *ba);
    if (!ba) return LAGFX_ERR_OUT_OF_MEMORY;
    lagfx_ba_init(ba);
    lagfx_ba_merge_modules(ba, spv_blobs, spv_lens, n_stages);
    uint32_t n = 0, other = 0;
    for (uint32_t i = 0; i < ba->n; i++) {
        const lagfx_ba_entry_t *e = &ba->e[i];
        VkDescriptorType vt = kind_to_vk(e->kind);
        if (vt == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        if (e->set != 0u) {
            /* Multi-set shaders aren't emitted by our translator; skip (counted, reported) so we don't silently build a wrong layout. */
            other++;
            continue;
        }
        if (n >= cap) { ba->noverflow++; continue; }
        binds[n++] = (VkDescriptorSetLayoutBinding){
            .binding = e->binding,
            .descriptorType = vt,
            .descriptorCount = 1u,
            .stageFlags = stages_to_vk(e->stages, vt),
            .pImmutableSamplers = NULL,
        };
    }
    char text[1024];
    text[0] = 0;
    if (!lagfx_ba_ok(ba)) lagfx_ba_describe(ba, text, sizeof text);
    if (info) {
        info->key = lagfx_ba_key(ba);
        info->n_conflict = ba->nconflict;
        info->n_overflow = ba->noverflow;
        info->n_other_sets = other;
        lagfx_ba_describe(ba, info->report, sizeof info->report);
    }
    if (other) LAGFX_WARN("descriptor_layout: %u binding(s) outside set 0 skipped (only set 0 is supported)", other);
    lagfx_status_t st = LAGFX_OK;
    if (!lagfx_ba_ok(ba)) {
        LAGFX_ERR("descriptor_layout: the binding table is invalid (%u conflict(s), %u overflow(s)); no layout is built:\n%s", ba->nconflict, ba->noverflow, text);
        st = LAGFX_ERR_INTERNAL;
    } else *n_out = n;
    free(ba);
    return st;
}

uint32_t lagfx_layout_bindings_from_spv(const uint8_t *const *spv_blobs, const size_t *spv_lens, uint32_t n_stages, VkDescriptorSetLayoutBinding *binds) {
    uint32_t n = 0;
    if (lagfx_layout_derive(spv_blobs, spv_lens, n_stages, binds, LAGFX_LAYOUT_MAX_BINDINGS, &n, NULL) != LAGFX_OK) return 0;
    return n;
}

lagfx_status_t
lagfx_build_pipeline_layout_from_spv(VkDevice device,
                                     const uint8_t *const *spv_blobs,
                                     const size_t *spv_lens,
                                     uint32_t n_stages,
                                     VkDescriptorSetLayout *out_dsl,
                                     VkPipelineLayout *out_pl) {
    if (!device || !spv_blobs || !spv_lens || !out_dsl || !out_pl)
        return LAGFX_ERR_INVALID_ARG;
    *out_dsl = VK_NULL_HANDLE;
    *out_pl = VK_NULL_HANDLE;

    VkDescriptorSetLayoutBinding binds[LAGFX_LAYOUT_MAX_BINDINGS];
    uint32_t n_binds = 0;
    lagfx_status_t dst = lagfx_layout_derive(spv_blobs, spv_lens, n_stages, binds, LAGFX_LAYOUT_MAX_BINDINGS, &n_binds, NULL);
    if (dst != LAGFX_OK) return dst;   /* a conflicting / oversized table: no layout (the reason is in the log) */

    if (n_binds == 0u) {
        /* No descriptors — caller uses the device empty layout. */
        return LAGFX_OK;
    }

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = n_binds,
        .pBindings = binds,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl) != VK_SUCCESS) {
        LAGFX_ERR("descriptor_layout: vkCreateDescriptorSetLayout failed (%u bindings)",
                  n_binds);
        return LAGFX_ERR_PROTOCOL;
    }

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &plci, NULL, &pl) != VK_SUCCESS) {
        LAGFX_ERR("descriptor_layout: vkCreatePipelineLayout failed");
        vkDestroyDescriptorSetLayout(device, dsl, NULL);
        return LAGFX_ERR_PROTOCOL;
    }

    *out_dsl = dsl;
    *out_pl = pl;
    LAGFX_LOG("descriptor_layout: built set-0 layout with %u binding(s)", n_binds);
    return LAGFX_OK;
}

#endif /* LAGFX_HAVE_VULKAN */
