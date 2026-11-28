#ifdef LAGFX_HAVE_VULKAN
/* libapplegfx-vulkan — common Vulkan compute execution base (see compute_exec.h) */
#include "compute_exec.h"
#include "air2spv/spv_reflect.h"
#include <stdlib.h>
#include <string.h>

struct lagfx_cx_pipe {
    VkDevice dev;
    VkShaderModule cs; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool pool;
    lagfx_cx_bind_t bind[LAGFX_CX_MAX_BIND]; uint32_t nbind, push_bytes, nsets;
    bool have_lim; uint32_t max_groups[3], max_push; VkDeviceSize ssbo_align, ubo_align;
};

void lagfx_cx_pipe_destroy(lagfx_cx_pipe_t *p) {
    if (!p) return;
    if (p->pool) vkDestroyDescriptorPool(p->dev, p->pool, NULL);
    if (p->pipe) vkDestroyPipeline(p->dev, p->pipe, NULL);
    if (p->pl) vkDestroyPipelineLayout(p->dev, p->pl, NULL);
    if (p->dsl) vkDestroyDescriptorSetLayout(p->dev, p->dsl, NULL);
    if (p->cs) vkDestroyShaderModule(p->dev, p->cs, NULL);
    free(p);
}

lagfx_cx_pipe_t *lagfx_cx_pipe_create(VkDevice dev, const uint32_t *spv, size_t spv_bytes, const char *entry, const lagfx_cx_bind_t *bind, uint32_t nbind, uint32_t push_bytes) {
    return lagfx_cx_pipe_create_ex(dev, spv, spv_bytes, entry, bind, nbind, push_bytes, NULL, 0, NULL);
}

lagfx_cx_pipe_t *lagfx_cx_pipe_create_ex(VkDevice dev, const uint32_t *spv, size_t spv_bytes, const char *entry, const lagfx_cx_bind_t *bind, uint32_t nbind, uint32_t push_bytes,
                                         const uint32_t (*spec)[2], uint32_t nspec, const VkPhysicalDeviceLimits *lim) {
    if (!dev || !spv || !spv_bytes || (spv_bytes & 3u) || nbind > LAGFX_CX_MAX_BIND || (nbind && !bind) || (push_bytes & 3u) || push_bytes > 128u) return NULL;
    for (uint32_t i = 0; i < nbind; i++) {   /* only the descriptor types record() knows how to write */
        const VkDescriptorType t = bind[i].type;
        if (t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && t != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && t != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            t != VK_DESCRIPTOR_TYPE_SAMPLER && t != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) return NULL;
    }
    for (uint32_t i = 0; i < nbind; i++) for (uint32_t j = i + 1; j < nbind; j++) if (bind[i].binding == bind[j].binding) return NULL;   /* one descriptor per binding */
    if ((nspec && !spec) || nspec > 64u) return NULL;
    for (uint32_t i = 0; i < nspec; i++) for (uint32_t j = i + 1; j < nspec; j++) if (spec[i][0] == spec[j][0]) return NULL;   /* one value per SpecId */
    {   /* every threadgroup argument of the kernel needs an explicit length */
        lagfx_spv_tgarg_t tg[32]; const size_t ntg = lagfx_spv_reflect_threadgroup_args((const uint8_t *)spv, spv_bytes, tg, 32);
        if (ntg > 32u) return NULL;
        for (size_t t = 0; t < ntg; t++) { bool given = false; for (uint32_t i = 0; i < nspec; i++) if (spec[i][0] == tg[t].spec_id) given = true; if (!given) return NULL; }
        if (lim) {   /* shared memory and workgroup size against the device */
            uint64_t bytes = 0;
            if (!lagfx_spv_reflect_workgroup_bytes((const uint8_t *)spv, spv_bytes, spec, nspec, &bytes) || bytes > lim->maxComputeSharedMemorySize) return NULL;
            uint64_t wg[3] = { 1, 1, 1 }; bool any = false;
            for (uint32_t i = 0; i < nspec; i++) if (spec[i][0] < 3u) { wg[spec[i][0]] = spec[i][1]; any = true; }
            if (any) {
                if (wg[0] > lim->maxComputeWorkGroupSize[0] || wg[1] > lim->maxComputeWorkGroupSize[1] || wg[2] > lim->maxComputeWorkGroupSize[2] || wg[0] * wg[1] * wg[2] > lim->maxComputeWorkGroupInvocations) return NULL;
            }
        }
    }
    lagfx_cx_pipe_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->dev = dev, p->nbind = nbind, p->push_bytes = push_bytes;
    if (nbind) memcpy(p->bind, bind, nbind * sizeof *bind);
    VkDescriptorSetLayoutBinding lb[LAGFX_CX_MAX_BIND]; VkDescriptorPoolSize ps[LAGFX_CX_MAX_BIND]; uint32_t nps = 0;
    for (uint32_t i = 0; i < nbind; i++) {
        lb[i] = (VkDescriptorSetLayoutBinding){ bind[i].binding, bind[i].type, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        uint32_t k = 0; while (k < nps && ps[k].type != bind[i].type) k++;
        if (k == nps) ps[nps++] = (VkDescriptorPoolSize){ bind[i].type, 0 };
        ps[k].descriptorCount += LAGFX_CX_SET_POOL;
    }
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbind, .pBindings = lb };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_bytes, .pCode = spv };
    if (vkCreateDescriptorSetLayout(dev, &dli, NULL, &p->dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p->dsl, .pushConstantRangeCount = push_bytes ? 1u : 0u, .pPushConstantRanges = push_bytes ? &pcr : NULL };
    if (vkCreatePipelineLayout(dev, &pli, NULL, &p->pl) != VK_SUCCESS) goto fail;
    if (vkCreateShaderModule(dev, &ci, NULL, &p->cs) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = p->pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = p->cs, .pName = entry ? entry : "main" } };
    VkSpecializationMapEntry sme[64]; uint32_t sdata[64]; VkSpecializationInfo sinfo;
    if (nspec) {
        for (uint32_t i = 0; i < nspec; i++) { sme[i] = (VkSpecializationMapEntry){ spec[i][0], i * 4u, 4u }; sdata[i] = spec[i][1]; }
        sinfo = (VkSpecializationInfo){ nspec, sme, nspec * 4u, sdata };
        cpi.stage.pSpecializationInfo = &sinfo;
    }
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p->pipe) != VK_SUCCESS) goto fail;
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = LAGFX_CX_SET_POOL, .poolSizeCount = nps, .pPoolSizes = nps ? ps : NULL };
    if (nbind && vkCreateDescriptorPool(dev, &dpi, NULL, &p->pool) != VK_SUCCESS) goto fail;
    if (lim) lagfx_cx_set_limits(p, lim);
    return p;
fail:
    lagfx_cx_pipe_destroy(p);
    return NULL;
}

void lagfx_cx_set_limits(lagfx_cx_pipe_t *p, const VkPhysicalDeviceLimits *lim) {
    if (!p || !lim) return;
    p->have_lim = true;
    for (int i = 0; i < 3; i++) p->max_groups[i] = lim->maxComputeWorkGroupCount[i];
    p->max_push = lim->maxPushConstantsSize, p->ssbo_align = lim->minStorageBufferOffsetAlignment, p->ubo_align = lim->minUniformBufferOffsetAlignment;
}

void lagfx_cx_recycle(lagfx_cx_pipe_t *p) {
    if (!p) return;
    if (p->pool) vkResetDescriptorPool(p->dev, p->pool, 0);
    p->nsets = 0;
}

bool lagfx_cx_record(lagfx_cx_pipe_t *p, VkCommandBuffer cb, const lagfx_cx_res_t *res, uint32_t nres, const void *push, uint32_t push_bytes, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!p || !cb || !gx || !gy || !gz || push_bytes != p->push_bytes || (push_bytes && !push) || nres != p->nbind || (nres && !res)) return false;
    /* every layout binding present exactly once, matched by number */
    const lagfx_cx_res_t *by[LAGFX_CX_MAX_BIND] = { 0 };
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t k = 0; while (k < p->nbind && p->bind[k].binding != res[i].binding) k++;
        if (k == p->nbind || by[k]) return false;
        by[k] = &res[i];
    }
    if (p->have_lim && (gx > p->max_groups[0] || gy > p->max_groups[1] || gz > p->max_groups[2] || push_bytes > p->max_push)) return false;
    for (uint32_t k = 0; k < p->nbind; k++) {   /* resource sanity before anything is allocated or recorded */
        const lagfx_cx_res_t *r = by[k];
        switch (p->bind[k].type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            const VkDeviceSize al = p->bind[k].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? p->ssbo_align : p->ubo_align;
            if (!r->buffer || (p->have_lim && al && (r->offset % al))) return false;
            break; }
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: if (!r->view) return false; break;
        case VK_DESCRIPTOR_TYPE_SAMPLER: if (!r->sampler) return false; break;
        default: if (!r->view || !r->sampler) return false; break;   /* combined image sampler */
        }
    }
    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (p->nbind) {
        if (p->nsets >= LAGFX_CX_SET_POOL) return false;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl };
        if (vkAllocateDescriptorSets(p->dev, &ai, &ds) != VK_SUCCESS) return false;
        p->nsets++;
        VkWriteDescriptorSet w[LAGFX_CX_MAX_BIND]; VkDescriptorBufferInfo bi[LAGFX_CX_MAX_BIND]; VkDescriptorImageInfo ii[LAGFX_CX_MAX_BIND];
        for (uint32_t k = 0; k < p->nbind; k++) {
            const lagfx_cx_res_t *r = by[k];
            w[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = p->bind[k].binding, .descriptorCount = 1, .descriptorType = p->bind[k].type };
            switch (p->bind[k].type) {
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                bi[k] = (VkDescriptorBufferInfo){ r->buffer, r->offset, r->range ? r->range : VK_WHOLE_SIZE }; w[k].pBufferInfo = &bi[k]; break;
            default:   /* storage image, sampled image, sampler, combined image sampler */
                ii[k] = (VkDescriptorImageInfo){ r->sampler, r->view, r->layout }; w[k].pImageInfo = &ii[k]; break;
            }
        }
        vkUpdateDescriptorSets(p->dev, p->nbind, w, 0, NULL);
    }
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    if (ds) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pl, 0, 1, &ds, 0, NULL);
    if (push_bytes) vkCmdPushConstants(cb, p->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    vkCmdDispatch(cb, gx, gy, gz);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return true;
}

bool lagfx_cx_threads_to_groups(const uint32_t threads[3], const uint32_t tg[3], uint32_t groups[3], bool *nonuniform) {
    bool nu = false;
    for (int i = 0; i < 3; i++) {
        if (!threads[i] || !tg[i]) return false;
        groups[i] = (uint32_t)(((uint64_t)threads[i] + tg[i] - 1u) / tg[i]);
        nu |= (threads[i] % tg[i]) != 0u;
    }
    if (nonuniform) *nonuniform = nu;
    return true;
}
#endif /* LAGFX_HAVE_VULKAN */
