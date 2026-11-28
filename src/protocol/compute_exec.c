#ifdef LAGFX_HAVE_VULKAN
/* libapplegfx-vulkan — common Vulkan compute execution base (see compute_exec.h) */
#include "compute_exec.h"
#include <stdlib.h>
#include <string.h>

struct lagfx_cx_pipe {
    VkDevice dev;
    VkShaderModule cs; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool pool;
    lagfx_cx_bind_t bind[LAGFX_CX_MAX_BIND]; uint32_t nbind, push_bytes, nsets;
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
    if (!dev || !spv || !spv_bytes || (spv_bytes & 3u) || nbind > LAGFX_CX_MAX_BIND || (nbind && !bind) || (push_bytes & 3u) || push_bytes > 128u) return NULL;
    for (uint32_t i = 0; i < nbind; i++) for (uint32_t j = i + 1; j < nbind; j++) if (bind[i].binding == bind[j].binding) return NULL;   /* one descriptor per binding */
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
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p->pipe) != VK_SUCCESS) goto fail;
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = LAGFX_CX_SET_POOL, .poolSizeCount = nps, .pPoolSizes = nps ? ps : NULL };
    if (nbind && vkCreateDescriptorPool(dev, &dpi, NULL, &p->pool) != VK_SUCCESS) goto fail;
    return p;
fail:
    lagfx_cx_pipe_destroy(p);
    return NULL;
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
