/* SPDX-License-Identifier: MIT */
#include "render_pass.h"
#include "instance.h"
#include "common/log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RP_MAX 16
#define FB_MAX 64

typedef struct { VkFormat fmt; VkImageLayout layout; VkRenderPass rp; } rp_ent_t;
typedef struct { VkRenderPass rp; VkImageView view; uint32_t w, h; VkFramebuffer fb; } fb_ent_t;

struct lagfx_render_cache {
    pthread_mutex_t mu;
    rp_ent_t rp[RP_MAX]; unsigned n_rp;
    fb_ent_t fb[FB_MAX]; unsigned n_fb;
};

lagfx_render_backend_t lagfx_vk_render_backend(const struct lagfx_vk_state *vk) {
    const char *f = getenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    if (f && !strcmp(f, "1")) return LAGFX_RENDER_LEGACY;
    return vk && vk->have_dynamic_rendering ? LAGFX_RENDER_DYNAMIC : LAGFX_RENDER_LEGACY;
}

static struct lagfx_render_cache *cache_get(struct lagfx_vk_state *vk) {
    if (!vk->render_cache) {
        struct lagfx_render_cache *c = calloc(1, sizeof *c);
        if (!c) return NULL;
        pthread_mutex_init(&c->mu, NULL);
        vk->render_cache = c;   /* a state is used from one thread at a time until the first begin; the mutex guards the rest */
    }
    return vk->render_cache;
}

static VkRenderPass rp_locked(struct lagfx_vk_state *vk, struct lagfx_render_cache *c, VkFormat fmt, VkImageLayout layout) {
    for (unsigned i = 0; i < c->n_rp; i++) if (c->rp[i].fmt == fmt && c->rp[i].layout == layout) return c->rp[i].rp;
    if (c->n_rp >= RP_MAX) { LAGFX_ERR("render_pass: cache full (%u formats)", c->n_rp); return VK_NULL_HANDLE; }
    VkAttachmentDescription ad = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,   /* the real ops are per begin: see the note in begin */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = layout, .finalLayout = layout,
    };
    VkAttachmentReference ref = { .attachment = 0, .layout = layout };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    /* External dependencies: the caller's barrier covers the layout change; these order the attachment writes against what comes before / after the pass. */
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
    };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                  .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult vr = vkCreateRenderPass(vk->device, &ci, NULL, &rp);
    if (vr != VK_SUCCESS) { LAGFX_ERR("render_pass: vkCreateRenderPass failed (%d)", (int)vr); return VK_NULL_HANDLE; }
    c->rp[c->n_rp++] = (rp_ent_t){ fmt, layout, rp };
    return rp;
}

VkRenderPass lagfx_vk_render_pass_get(struct lagfx_vk_state *vk, VkFormat format, VkImageLayout layout) {
    if (!vk || vk->device == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    struct lagfx_render_cache *c = cache_get(vk);
    if (!c) return VK_NULL_HANDLE;
    pthread_mutex_lock(&c->mu);
    VkRenderPass rp = rp_locked(vk, c, format, layout);
    pthread_mutex_unlock(&c->mu);
    return rp;
}

lagfx_status_t lagfx_vk_render_begin(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_color_t *c) {
    if (!vk || !c || cmd == VK_NULL_HANDLE || c->view == VK_NULL_HANDLE || !c->width || !c->height) return LAGFX_ERR_INVALID_ARG;
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) {
        if (!vk->have_dynamic_rendering) return LAGFX_ERR_BACKEND;
        VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = c->view, .imageLayout = c->layout,
                                          .loadOp = c->load, .storeOp = c->store, .clearValue = { .color = c->clear } };
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { c->width, c->height } }, .layerCount = 1,
                               .colorAttachmentCount = 1, .pColorAttachments = &att };
        vkCmdBeginRendering(cmd, &ri);
        return LAGFX_OK;
    }
    struct lagfx_render_cache *rc = cache_get(vk);
    if (!rc) return LAGFX_ERR_BACKEND;
    pthread_mutex_lock(&rc->mu);
    VkRenderPass rp = rp_locked(vk, rc, c->format, c->layout);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (rp != VK_NULL_HANDLE) {
        for (unsigned i = 0; i < rc->n_fb; i++)
            if (rc->fb[i].rp == rp && rc->fb[i].view == c->view && rc->fb[i].w == c->width && rc->fb[i].h == c->height) { fb = rc->fb[i].fb; break; }
        if (fb == VK_NULL_HANDLE) {
            if (rc->n_fb >= FB_MAX) {   /* full: nothing recorded may still use them once the device is idle */
                vkDeviceWaitIdle(vk->device);
                for (unsigned i = 0; i < rc->n_fb; i++) vkDestroyFramebuffer(vk->device, rc->fb[i].fb, NULL);
                rc->n_fb = 0;
            }
            VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &c->view,
                                            .width = c->width, .height = c->height, .layers = 1 };
            VkResult vr = vkCreateFramebuffer(vk->device, &fci, NULL, &fb);
            if (vr != VK_SUCCESS) { LAGFX_ERR("render_begin: vkCreateFramebuffer failed (%d)", (int)vr); fb = VK_NULL_HANDLE; }
            else rc->fb[rc->n_fb++] = (fb_ent_t){ rp, c->view, c->width, c->height, fb };
        }
    }
    pthread_mutex_unlock(&rc->mu);
    if (fb == VK_NULL_HANDLE) return LAGFX_ERR_BACKEND;
    /* The cached pass loads and stores; a CLEAR is expressed with the clear value only when asked: the pass has loadOp LOAD, so clear explicitly inside it. */
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                 .renderArea = { { 0, 0 }, { c->width, c->height } } };
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (c->load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = { .color = c->clear } };
        VkClearRect cr = { .rect = { { 0, 0 }, { c->width, c->height } }, .baseArrayLayer = 0, .layerCount = 1 };
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    }
    return LAGFX_OK;
}

void lagfx_vk_render_end(struct lagfx_vk_state *vk, VkCommandBuffer cmd) {
    if (!vk || cmd == VK_NULL_HANDLE) return;
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) vkCmdEndRendering(cmd);
    else vkCmdEndRenderPass(cmd);
}

void lagfx_vk_render_forget_view(struct lagfx_vk_state *vk, VkImageView view) {
    if (!vk || !vk->render_cache || view == VK_NULL_HANDLE) return;
    struct lagfx_render_cache *c = vk->render_cache;
    pthread_mutex_lock(&c->mu);
    bool idle = false;
    for (unsigned i = 0; i < c->n_fb;) {
        if (c->fb[i].view == view) {
            if (!idle) { vkDeviceWaitIdle(vk->device); idle = true; }
            vkDestroyFramebuffer(vk->device, c->fb[i].fb, NULL);
            c->fb[i] = c->fb[--c->n_fb];
        } else i++;
    }
    pthread_mutex_unlock(&c->mu);
}

void lagfx_vk_render_pass_shutdown(struct lagfx_vk_state *vk) {
    if (!vk || !vk->render_cache) return;
    struct lagfx_render_cache *c = vk->render_cache;
    if (vk->device != VK_NULL_HANDLE) {
        for (unsigned i = 0; i < c->n_fb; i++) vkDestroyFramebuffer(vk->device, c->fb[i].fb, NULL);
        for (unsigned i = 0; i < c->n_rp; i++) vkDestroyRenderPass(vk->device, c->rp[i].rp, NULL);
    }
    pthread_mutex_destroy(&c->mu);
    free(c);
    vk->render_cache = NULL;
}
