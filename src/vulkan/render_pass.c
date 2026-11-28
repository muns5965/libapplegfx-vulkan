/* SPDX-License-Identifier: MIT */
#include "render_pass.h"
#include "instance.h"
#include "common/log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Cache limits: render passes 256, framebuffers 1024 (both overridable once per device for tests: LAGFX_RENDER_CACHE_MAX_RP / _MAX_FB). Entries are never evicted: a framebuffer may be
 * referenced by a recorded, not yet submitted command buffer; they leave only through render_forget_view / shutdown. A full cache is a clean error, not an eviction. */
#define RP_MAX_DEFAULT 256u
#define FB_MAX_DEFAULT 1024u

typedef struct { VkFormat fmt; VkImageLayout layout; VkAttachmentLoadOp load; VkAttachmentStoreOp store; VkRenderPass rp; } rp_ent_t;
typedef struct { VkRenderPass rp; VkImageView view; uint32_t w, h; VkFramebuffer fb; } fb_ent_t;

struct lagfx_render_cache {
    pthread_mutex_t mu;
    rp_ent_t *rp; unsigned n_rp, cap_rp, max_rp;
    fb_ent_t *fb; unsigned n_fb, cap_fb, max_fb;
};

static unsigned env_limit(const char *name, unsigned def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end; unsigned long n = strtoul(v, &end, 10);
    return (*end || n == 0 || n > 1000000ul) ? def : (unsigned)n;
}

/* Chosen once, when the device is created (lagfx_vk_init), and never changed afterwards: pipelines, begin and end must all see the same backend. */
lagfx_render_backend_t lagfx_vk_render_backend_select(bool have_dynamic_rendering) {
    const char *f = getenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    if (f && !strcmp(f, "1")) return LAGFX_RENDER_LEGACY;
    return have_dynamic_rendering ? LAGFX_RENDER_DYNAMIC : LAGFX_RENDER_LEGACY;
}

lagfx_render_backend_t lagfx_vk_render_backend(const struct lagfx_vk_state *vk) {
    if (vk && vk->render_backend == LAGFX_RENDER_LEGACY) return LAGFX_RENDER_LEGACY;
    return vk && vk->have_dynamic_rendering ? LAGFX_RENDER_DYNAMIC : LAGFX_RENDER_LEGACY;
}

static struct lagfx_render_cache *cache_get(struct lagfx_vk_state *vk) {
    if (!vk->render_cache) {
        struct lagfx_render_cache *c = calloc(1, sizeof *c);
        if (!c) return NULL;
        pthread_mutex_init(&c->mu, NULL);
        c->max_rp = env_limit("LAGFX_RENDER_CACHE_MAX_RP", RP_MAX_DEFAULT);
        c->max_fb = env_limit("LAGFX_RENDER_CACHE_MAX_FB", FB_MAX_DEFAULT);
        vk->render_cache = c;   /* a state is used from one thread at a time until the first begin; the mutex guards the rest */
    }
    return vk->render_cache;
}

static VkRenderPass rp_locked(struct lagfx_vk_state *vk, struct lagfx_render_cache *c, VkFormat fmt, VkImageLayout layout, VkAttachmentLoadOp load, VkAttachmentStoreOp store) {
    for (unsigned i = 0; i < c->n_rp; i++) if (c->rp[i].fmt == fmt && c->rp[i].layout == layout && c->rp[i].load == load && c->rp[i].store == store) return c->rp[i].rp;
    if (c->n_rp >= c->max_rp) { LAGFX_ERR("render_pass: cache full (%u render passes)", c->n_rp); return VK_NULL_HANDLE; }
    if (c->n_rp == c->cap_rp) {
        unsigned nc = c->cap_rp ? c->cap_rp * 2u : 8u; if (nc > c->max_rp) nc = c->max_rp;
        rp_ent_t *n = realloc(c->rp, nc * sizeof *n); if (!n) return VK_NULL_HANDLE;
        c->rp = n; c->cap_rp = nc;
    }
    VkAttachmentDescription ad = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = load, .storeOp = store,   /* part of the cache key: exactly what the caller asked for */
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
    c->rp[c->n_rp++] = (rp_ent_t){ fmt, layout, load, store, rp };
    return rp;
}

VkRenderPass lagfx_vk_render_pass_get(struct lagfx_vk_state *vk, VkFormat format, VkImageLayout layout) {
    if (!vk || vk->device == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    struct lagfx_render_cache *c = cache_get(vk);
    if (!c) return VK_NULL_HANDLE;
    pthread_mutex_lock(&c->mu);
    VkRenderPass rp = rp_locked(vk, c, format, layout, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);   /* pipelines only need a COMPATIBLE pass: format + samples */
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
    VkRenderPass rp = rp_locked(vk, rc, c->format, c->layout, c->load, c->store);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (rp != VK_NULL_HANDLE) {
        for (unsigned i = 0; i < rc->n_fb; i++)
            if (rc->fb[i].rp == rp && rc->fb[i].view == c->view && rc->fb[i].w == c->width && rc->fb[i].h == c->height) { fb = rc->fb[i].fb; break; }
        if (fb == VK_NULL_HANDLE) {
            if (rc->n_fb >= rc->max_fb) { LAGFX_ERR("render_begin: framebuffer cache full (%u): a view is not being forgotten", rc->n_fb); goto fb_done; }
            if (rc->n_fb == rc->cap_fb) {
                unsigned nc = rc->cap_fb ? rc->cap_fb * 2u : 16u; if (nc > rc->max_fb) nc = rc->max_fb;
                fb_ent_t *n = realloc(rc->fb, nc * sizeof *n); if (!n) goto fb_done;
                rc->fb = n; rc->cap_fb = nc;
            }
            VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &c->view,
                                            .width = c->width, .height = c->height, .layers = 1 };
            VkResult vr = vkCreateFramebuffer(vk->device, &fci, NULL, &fb);
            if (vr != VK_SUCCESS) { LAGFX_ERR("render_begin: vkCreateFramebuffer failed (%d)", (int)vr); fb = VK_NULL_HANDLE; }
            else rc->fb[rc->n_fb++] = (fb_ent_t){ rp, c->view, c->width, c->height, fb };
        }
    }
fb_done:
    pthread_mutex_unlock(&rc->mu);
    if (fb == VK_NULL_HANDLE) return LAGFX_ERR_BACKEND;
    VkClearValue cv = { .color = c->clear };
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                 .renderArea = { { 0, 0 }, { c->width, c->height } },
                                 .clearValueCount = c->load == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u : 0u, .pClearValues = &cv };
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
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
    bool idle = false, failed = false;
    for (unsigned i = 0; i < c->n_fb;) {
        if (c->fb[i].view == view) {
            if (!idle && !failed) {
                if (vkDeviceWaitIdle(vk->device) == VK_SUCCESS) idle = true;
                else { failed = true; LAGFX_ERR("render_forget_view: vkDeviceWaitIdle failed; the framebuffers of this view are KEPT (leaked until shutdown), not destroyed under possibly running work"); }
            }
            if (failed) { i++; continue; }
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
    free(c->rp); free(c->fb);
    free(c);
    vk->render_cache = NULL;
}
