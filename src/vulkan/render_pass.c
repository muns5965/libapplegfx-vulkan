/* SPDX-License-Identifier: MIT */
#include "render_pass.h"
#include "instance.h"
#include "command.h"
#include "common/log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Cache limits: render passes 256, framebuffers 1024 (both overridable once per device for tests: LAGFX_RENDER_CACHE_MAX_RP / _MAX_FB). Entries are never evicted: a framebuffer may be
 * referenced by a recorded, not yet submitted command buffer; they leave only through render_forget_view / shutdown. A full cache is a clean error, not an eviction. */
#define RP_MAX_DEFAULT 256u
#define FB_MAX_DEFAULT 1024u
#define MAX_ATT (LAGFX_RENDER_MAX_COLOR + 1u)   /* colours + depth/stencil */

/* One attachment of a pass, as far as the pass object cares. All 32-bit members, no padding: keys are compared with memcmp after memset(0). */
typedef struct { VkFormat fmt; VkSampleCountFlagBits samples; VkImageLayout layout; VkAttachmentLoadOp load, sload; VkAttachmentStoreOp store, sstore; } akey_t;
typedef struct { uint32_t ncolor, has_ds; akey_t a[MAX_ATT]; } rpkey_t;

typedef struct { rpkey_t key; VkRenderPass rp; } rp_ent_t;
typedef struct { VkRenderPass rp; uint32_t nview; VkImageView view[MAX_ATT]; uint32_t w, h; VkFramebuffer fb; } fb_ent_t;
typedef struct { VkImageView view; VkImage image; VkDeviceMemory memory; } defer_ent_t;

struct lagfx_render_cache {
    pthread_mutex_t mu;
    rp_ent_t *rp; unsigned n_rp, cap_rp, max_rp;
    fb_ent_t *fb; unsigned n_fb, cap_fb, max_fb;
    defer_ent_t *def; unsigned n_def, cap_def;
    unsigned fail_waitidle; bool fail_lost;   /* test seam */
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

static bool fmt_has_stencil(VkFormat f) {
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static bool fmt_has_depth(VkFormat f) { return f != VK_FORMAT_S8_UINT; }   /* only meaningful for a depth/stencil format */

static VkSampleCountFlagBits samples_of(const lagfx_render_attachment_t *a) { return a->samples ? a->samples : VK_SAMPLE_COUNT_1_BIT; }

static bool desc_valid(const lagfx_render_desc_t *d, bool need_views) {
    if (!d || d->color_count > LAGFX_RENDER_MAX_COLOR) return false;
    if (need_views && (!d->width || !d->height)) return false;
    uint32_t used = d->has_depth_stencil ? 1u : 0u;
    for (uint32_t i = 0; i < d->color_count; i++) {
        if (d->color[i].format == VK_FORMAT_UNDEFINED) {   /* an unused slot (format UNDEFINED, no view): it keeps its place, so the shader's output locations do not move */
            if (need_views && d->color[i].view != VK_NULL_HANDLE) return false;
            continue;
        }
        used++;
        if (need_views && d->color[i].view == VK_NULL_HANDLE) return false;
    }
    if (used == 0u) return false;   /* a pass needs at least one real attachment */
    if (d->has_depth_stencil) {
        if (d->depth_stencil.format == VK_FORMAT_UNDEFINED) return false;
        if (need_views && d->depth_stencil.view == VK_NULL_HANDLE) return false;
    }
    return true;
}

/* compat: the LOAD/STORE variant used for pipeline creation (compatibility ignores ops and layouts). */
static void make_key(rpkey_t *k, const lagfx_render_desc_t *d, bool compat) {
    memset(k, 0, sizeof *k);
    k->ncolor = d->color_count;
    for (uint32_t i = 0; i < d->color_count; i++) {
        const lagfx_render_attachment_t *a = &d->color[i];
        if (a->format == VK_FORMAT_UNDEFINED) continue;   /* unused slot: all-zero entry, only its position is part of the key */
        k->a[i] = (akey_t){ a->format, samples_of(a), a->layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->layout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,   /* never UNDEFINED: not valid for an attachment reference or a LOAD */
                            compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->load, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                            compat ? VK_ATTACHMENT_STORE_OP_STORE : a->store, VK_ATTACHMENT_STORE_OP_DONT_CARE };
    }
    if (d->has_depth_stencil) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        k->has_ds = 1;
        k->a[d->color_count] = (akey_t){ a->format, samples_of(a), a->layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->load, compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->stencil_load,
                                         compat ? VK_ATTACHMENT_STORE_OP_STORE : a->store, compat ? VK_ATTACHMENT_STORE_OP_STORE : a->stencil_store };
    }
}

static VkRenderPass rp_locked(struct lagfx_vk_state *vk, struct lagfx_render_cache *c, const rpkey_t *k) {
    for (unsigned i = 0; i < c->n_rp; i++) if (!memcmp(&c->rp[i].key, k, sizeof *k)) return c->rp[i].rp;
    if (c->n_rp >= c->max_rp) { LAGFX_ERR("render_pass: cache full (%u render passes)", c->n_rp); return VK_NULL_HANDLE; }
    if (c->n_rp == c->cap_rp) {
        unsigned nc = c->cap_rp ? c->cap_rp * 2u : 8u; if (nc > c->max_rp) nc = c->max_rp;
        rp_ent_t *n = realloc(c->rp, nc * sizeof *n); if (!n) return VK_NULL_HANDLE;
        c->rp = n; c->cap_rp = nc;
    }
    VkAttachmentDescription ad[MAX_ATT]; VkAttachmentReference cref[LAGFX_RENDER_MAX_COLOR]; VkAttachmentReference dref = { 0 };
    uint32_t na = 0;   /* real attachments only: an unused colour slot is VK_ATTACHMENT_UNUSED in the subpass and has no attachment description */
    for (uint32_t i = 0; i < k->ncolor + k->has_ds; i++) {
        const akey_t *a = &k->a[i];
        const bool is_color = i < k->ncolor;
        if (is_color && a->fmt == VK_FORMAT_UNDEFINED) { cref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }; continue; }
        ad[na] = (VkAttachmentDescription){ .format = a->fmt, .samples = a->samples, .loadOp = a->load, .storeOp = a->store,
                                            .stencilLoadOp = a->sload, .stencilStoreOp = a->sstore,   /* part of the cache key: exactly what the caller asked for */
                                            .initialLayout = a->layout, .finalLayout = a->layout };
        if (is_color) cref[i] = (VkAttachmentReference){ na, a->layout };
        else dref = (VkAttachmentReference){ na, a->layout };
        na++;
    }
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor, .pColorAttachments = k->ncolor ? cref : NULL,
                                .pDepthStencilAttachment = k->has_ds ? &dref : NULL };
    /* External dependencies: the caller's barrier covers the layout change; these order the attachment writes against what comes before / after the pass. */
    VkPipelineStageFlags ds_st = k->has_ds ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : 0;
    VkAccessFlags ds_ac = k->has_ds ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0;
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | ds_st,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | (ds_ac & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | ds_ac, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (ds_ac & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | ds_ac, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
    };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = ad,
                                  .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult vr = vkCreateRenderPass(vk->device, &ci, NULL, &rp);
    if (vr != VK_SUCCESS) { LAGFX_ERR("render_pass: vkCreateRenderPass failed (%d)", (int)vr); return VK_NULL_HANDLE; }
    c->rp[c->n_rp].key = *k; c->rp[c->n_rp].rp = rp; c->n_rp++;
    return rp;
}

VkRenderPass lagfx_vk_render_pass_compat(struct lagfx_vk_state *vk, const lagfx_render_desc_t *d) {
    if (!vk || vk->device == VK_NULL_HANDLE || !desc_valid(d, false)) return VK_NULL_HANDLE;
    struct lagfx_render_cache *c = cache_get(vk);
    if (!c) return VK_NULL_HANDLE;
    rpkey_t k; make_key(&k, d, true);
    pthread_mutex_lock(&c->mu);
    VkRenderPass rp = rp_locked(vk, c, &k);
    pthread_mutex_unlock(&c->mu);
    return rp;
}

VkRenderPass lagfx_vk_render_pass_get(struct lagfx_vk_state *vk, VkFormat format, VkImageLayout layout) {
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.color_count = 1; d.color[0].format = format; d.color[0].layout = layout; d.color[0].samples = VK_SAMPLE_COUNT_1_BIT;
    return lagfx_vk_render_pass_compat(vk, &d);
}

static lagfx_status_t begin_dynamic(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_desc_t *d) {
    if (!vk->have_dynamic_rendering) return LAGFX_ERR_BACKEND;
    VkRenderingAttachmentInfo col[LAGFX_RENDER_MAX_COLOR], dep = { 0 }, ste = { 0 };
    for (uint32_t i = 0; i < d->color_count; i++) {
        const lagfx_render_attachment_t *a = &d->color[i];
        if (a->format == VK_FORMAT_UNDEFINED) { col[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = VK_NULL_HANDLE,
                                                                                   .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE }; continue; }
        col[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view, .imageLayout = a->layout,
                                              .loadOp = a->load, .storeOp = a->store, .clearValue = a->clear };
    }
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { d->width, d->height } }, .layerCount = 1,
                           .colorAttachmentCount = d->color_count, .pColorAttachments = d->color_count ? col : NULL };
    if (d->has_depth_stencil) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        if (fmt_has_depth(a->format)) {
            dep = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view, .imageLayout = a->layout,
                                               .loadOp = a->load, .storeOp = a->store, .clearValue = a->clear };
            ri.pDepthAttachment = &dep;
        }
        if (fmt_has_stencil(a->format)) {
            ste = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view, .imageLayout = a->layout,
                                               .loadOp = a->stencil_load, .storeOp = a->stencil_store, .clearValue = a->clear };
            ri.pStencilAttachment = &ste;
        }
    }
    vkCmdBeginRendering(cmd, &ri);
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_render_begin_desc(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_desc_t *d) {
    if (!vk || cmd == VK_NULL_HANDLE || !desc_valid(d, true)) return LAGFX_ERR_INVALID_ARG;
    if (lagfx_vk_device_lost(vk)) { LAGFX_ERR("render_begin: device lost earlier; no new work"); return LAGFX_ERR_BACKEND; }
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) return begin_dynamic(vk, cmd, d);

    struct lagfx_render_cache *rc = cache_get(vk);
    if (!rc) return LAGFX_ERR_BACKEND;
    uint32_t na = 0;   /* real attachments, in the order the render pass numbers them (unused colour slots take no entry) */
    VkImageView views[MAX_ATT]; VkClearValue cv[MAX_ATT]; bool any_clear = false;
    for (uint32_t i = 0; i < d->color_count; i++) {
        if (d->color[i].format == VK_FORMAT_UNDEFINED) continue;
        views[na] = d->color[i].view; cv[na] = d->color[i].clear; na++;
        if (d->color[i].load == VK_ATTACHMENT_LOAD_OP_CLEAR) any_clear = true;
    }
    if (d->has_depth_stencil) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        views[na] = a->view; cv[na] = a->clear; na++;
        if (a->load == VK_ATTACHMENT_LOAD_OP_CLEAR || a->stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) any_clear = true;
    }
    rpkey_t k; make_key(&k, d, false);
    pthread_mutex_lock(&rc->mu);
    VkRenderPass rp = rp_locked(vk, rc, &k);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (rp != VK_NULL_HANDLE) {
        for (unsigned i = 0; i < rc->n_fb && fb == VK_NULL_HANDLE; i++) {
            const fb_ent_t *e = &rc->fb[i];
            if (e->rp == rp && e->nview == na && e->w == d->width && e->h == d->height && !memcmp(e->view, views, na * sizeof views[0])) fb = e->fb;
        }
        if (fb == VK_NULL_HANDLE) {
            if (rc->n_fb >= rc->max_fb) { LAGFX_ERR("render_begin: framebuffer cache full (%u): a view is not being forgotten", rc->n_fb); goto fb_done; }
            if (rc->n_fb == rc->cap_fb) {
                unsigned nc = rc->cap_fb ? rc->cap_fb * 2u : 16u; if (nc > rc->max_fb) nc = rc->max_fb;
                fb_ent_t *n = realloc(rc->fb, nc * sizeof *n); if (!n) goto fb_done;
                rc->fb = n; rc->cap_fb = nc;
            }
            VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = na, .pAttachments = views,
                                            .width = d->width, .height = d->height, .layers = 1 };
            VkResult vr = vkCreateFramebuffer(vk->device, &fci, NULL, &fb);
            if (vr != VK_SUCCESS) { LAGFX_ERR("render_begin: vkCreateFramebuffer failed (%d)", (int)vr); fb = VK_NULL_HANDLE; }
            else {
                fb_ent_t *e = &rc->fb[rc->n_fb++];
                memset(e, 0, sizeof *e);
                e->rp = rp; e->nview = na; e->w = d->width; e->h = d->height; e->fb = fb;
                memcpy(e->view, views, na * sizeof views[0]);
            }
        }
    }
fb_done:
    pthread_mutex_unlock(&rc->mu);
    if (fb == VK_NULL_HANDLE) return LAGFX_ERR_BACKEND;
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                 .renderArea = { { 0, 0 }, { d->width, d->height } },
                                 .clearValueCount = any_clear ? na : 0u, .pClearValues = cv };   /* indexed by attachment; unused entries are ignored */
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_render_begin(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_color_t *c) {
    if (!c) return LAGFX_ERR_INVALID_ARG;
    lagfx_render_desc_t d; memset(&d, 0, sizeof d);
    d.width = c->width; d.height = c->height; d.color_count = 1;
    d.color[0] = (lagfx_render_attachment_t){ .view = c->view, .format = c->format, .samples = VK_SAMPLE_COUNT_1_BIT, .layout = c->layout,
                                              .load = c->load, .store = c->store, .clear = { .color = c->clear } };
    return lagfx_vk_render_begin_desc(vk, cmd, &d);
}

void lagfx_vk_render_end(struct lagfx_vk_state *vk, VkCommandBuffer cmd) {
    if (!vk || cmd == VK_NULL_HANDLE) return;
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) vkCmdEndRendering(cmd);
    else vkCmdEndRenderPass(cmd);
}

lagfx_forget_t lagfx_vk_render_forget_view(struct lagfx_vk_state *vk, VkImageView view) {
    if (!vk || !vk->render_cache || view == VK_NULL_HANDLE) return LAGFX_FORGET_OK;
    struct lagfx_render_cache *c = vk->render_cache;
    pthread_mutex_lock(&c->mu);
    bool idle = false; lagfx_forget_t out = LAGFX_FORGET_OK;
    for (unsigned i = 0; i < c->n_fb;) {
        bool uses = false;
        for (uint32_t j = 0; j < c->fb[i].nview; j++) if (c->fb[i].view[j] == view) uses = true;
        if (!uses) { i++; continue; }
        if (!idle && out == LAGFX_FORGET_OK) {
            VkResult vr;
            if (c->fail_waitidle) { c->fail_waitidle--; vr = c->fail_lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY; }
            else vr = vkDeviceWaitIdle(vk->device);
            if (vr == VK_SUCCESS) idle = true;
            else if (vr == VK_ERROR_DEVICE_LOST) { idle = true; lagfx_vk_mark_device_lost(vk); LAGFX_ERR("render_forget_view: device lost; the objects are destroyed (nothing can still run), but no new work is accepted from now on"); }
            else { out = LAGFX_FORGET_DEFERRED; LAGFX_ERR("render_forget_view: vkDeviceWaitIdle failed (%d); framebuffers and the view's resources are deferred to shutdown", (int)vr); }
        }
        if (out == LAGFX_FORGET_DEFERRED) { i++; continue; }
        vkDestroyFramebuffer(vk->device, c->fb[i].fb, NULL);
        c->fb[i] = c->fb[--c->n_fb];
    }
    pthread_mutex_unlock(&c->mu);
    return out;
}

bool lagfx_vk_render_defer_target(struct lagfx_vk_state *vk, VkImageView view, VkImage image, VkDeviceMemory memory) {
    struct lagfx_render_cache *c = vk ? cache_get(vk) : NULL;
    if (!c) return false;
    pthread_mutex_lock(&c->mu);
    bool ok = true;
    if (c->n_def == c->cap_def) {
        unsigned nc = c->cap_def ? c->cap_def * 2u : 8u;
        defer_ent_t *n = realloc(c->def, nc * sizeof *n);
        if (n) { c->def = n; c->cap_def = nc; } else ok = false;
    }
    if (ok) c->def[c->n_def++] = (defer_ent_t){ view, image, memory };
    pthread_mutex_unlock(&c->mu);
    if (!ok) LAGFX_ERR("render_defer_target: out of memory; the resources are leaked");
    return ok;
}

unsigned lagfx_vk_render_deferred_count(const struct lagfx_vk_state *vk) { return vk && vk->render_cache ? vk->render_cache->n_def : 0u; }

void lagfx_vk_render_test_fail_waitidle(struct lagfx_vk_state *vk, unsigned n, bool lost) {
    struct lagfx_render_cache *c = vk ? cache_get(vk) : NULL;
    if (c) { c->fail_waitidle = n; c->fail_lost = lost; }
}

void lagfx_vk_render_pass_shutdown(struct lagfx_vk_state *vk) {
    if (!vk || !vk->render_cache) return;
    struct lagfx_render_cache *c = vk->render_cache;
    if (vk->device != VK_NULL_HANDLE) {
        if (c->n_fb || c->n_def) (void)vkDeviceWaitIdle(vk->device);   /* the device is going away; a failure here changes nothing */
        for (unsigned i = 0; i < c->n_fb; i++) vkDestroyFramebuffer(vk->device, c->fb[i].fb, NULL);
        for (unsigned i = 0; i < c->n_rp; i++) vkDestroyRenderPass(vk->device, c->rp[i].rp, NULL);
        for (unsigned i = 0; i < c->n_def; i++) {   /* after the framebuffers that reference the views */
            if (c->def[i].view != VK_NULL_HANDLE) vkDestroyImageView(vk->device, c->def[i].view, NULL);
            if (c->def[i].image != VK_NULL_HANDLE) vkDestroyImage(vk->device, c->def[i].image, NULL);
            if (c->def[i].memory != VK_NULL_HANDLE) vkFreeMemory(vk->device, c->def[i].memory, NULL);
        }
    }
    pthread_mutex_destroy(&c->mu);
    free(c->rp); free(c->fb); free(c->def);
    free(c);
    vk->render_cache = NULL;
}
