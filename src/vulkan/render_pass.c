/* SPDX-License-Identifier: MIT */
#include "vulkan/msaa.h"
#include "render_pass.h"
#include "instance.h"
#include "command.h"
#include "common/log.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* Cache limits: render passes 256, framebuffers 1024 (both overridable once per device for tests: LAGFX_RENDER_CACHE_MAX_RP / _MAX_FB). Entries are never evicted: a framebuffer may be
 * referenced by a recorded, not yet submitted command buffer; they leave only through render_forget_view / shutdown. A full cache is a clean error, not an eviction. */
#define RP_MAX_DEFAULT 256u
#define FB_MAX_DEFAULT 1024u
#define MAX_ATT (LAGFX_RENDER_MAX_COLOR + 1u + LAGFX_RENDER_MAX_COLOR)   /* colours + depth/stencil + resolve targets */
#define MAX_REAL (LAGFX_RENDER_MAX_COLOR + 1u)   /* colours + depth/stencil (the slots of the key) */

/* One attachment of a pass, as far as the pass object cares. All 32-bit members, no padding: keys are compared with memcmp after memset(0). */
typedef struct { VkFormat fmt; VkSampleCountFlagBits samples; VkImageLayout layout; VkAttachmentLoadOp load, sload; VkAttachmentStoreOp store, sstore; VkImageLayout rlayout; VkResolveModeFlagBits dmode, smode; } akey_t;   /* rlayout != 0: resolved into a 1x view of the same format */
typedef struct { uint32_t ncolor, has_ds; akey_t a[MAX_REAL]; uint32_t fetch_mask; } rpkey_t;

typedef struct { rpkey_t key; VkRenderPass rp; } rp_ent_t;
typedef struct { VkRenderPass rp; uint32_t nview; VkImageView view[MAX_ATT]; uint32_t w, h; VkFramebuffer fb; } fb_ent_t;
typedef struct { VkImageView view; VkImage image; VkDeviceMemory memory; } defer_ent_t;

struct lagfx_render_cache {
    pthread_mutex_t mu;
    rp_ent_t *rp; unsigned n_rp, cap_rp, max_rp;
    fb_ent_t *fb; unsigned n_fb, cap_fb, max_fb;
    defer_ent_t *def; unsigned n_def, cap_def;
    unsigned fail_waitidle; bool fail_lost;   /* test seam */
    lagfx_render_cache_stats_t st;            /* framebuffer cache counters (legacy backend) */
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

bool lagfx_vk_ds_resolve_ok(const struct lagfx_vk_state *vk, VkFormat fmt, VkResolveModeFlagBits dm, VkResolveModeFlagBits sm) {
    if (!vk || !vk->have_ds_resolve) return false;
    const bool hd = fmt_has_depth(fmt), hs = fmt_has_stencil(fmt);
    if ((!hd && dm) || (!hs && sm) || (!dm && !sm)) return false;
    if (dm && ((vk->dsres_depth_modes & dm) != dm || (dm & (dm - 1u)))) return false;      /* one mode, in the supported set */
    if (sm && ((vk->dsres_stencil_modes & sm) != sm || (sm & (sm - 1u)))) return false;
    if (hd && hs && dm != sm) {   /* both aspects, different modes (NONE counts as a mode): independentResolve, or independentResolveNone when one of them is NONE */
        if (!(dm && sm) ? !(vk->dsres_independent_none || vk->dsres_independent) : !vk->dsres_independent) return false;
    }
    return true;
}

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
        if (d->color[i].resolve_view != VK_NULL_HANDLE && samples_of(&d->color[i]) == VK_SAMPLE_COUNT_1_BIT) return false;   /* nothing to resolve from a single-sample image */
    }
    if (used == 0u) return false;   /* a pass needs at least one real attachment */

    if (d->has_depth_stencil) {
        if (d->depth_stencil.format == VK_FORMAT_UNDEFINED) return false;
        if (d->depth_stencil.resolve_view != VK_NULL_HANDLE && samples_of(&d->depth_stencil) == VK_SAMPLE_COUNT_1_BIT) return false;   /* nothing to resolve from a single-sample image */
        if (need_views && d->depth_stencil.view == VK_NULL_HANDLE) return false;
    }
    return true;
}

/* Legacy-only: the input-attachment slots (fetch_mask) must be real, single-sample colour attachments in GENERAL. Kept out of desc_valid so the dynamic backend, which ignores the mask,
 * is never constrained by it. */
static bool fetch_valid(const lagfx_render_desc_t *d, bool real_layouts) {
    for (uint32_t i = 0; i < LAGFX_RENDER_MAX_COLOR; i++) {   /* input-attachment slots (legacy): real colour attachments in GENERAL */
        if (!(d->fetch_mask & (1u << i))) continue;
        if (i >= d->color_count || d->color[i].format == VK_FORMAT_UNDEFINED ) return false;   /* multisampled fetch slots are allowed here (subpassInputMS); the CALLER gates them on lagfx_vk_fetch_ms_capable */
        if (real_layouts && d->color[i].layout != VK_IMAGE_LAYOUT_UNDEFINED && d->color[i].layout != VK_IMAGE_LAYOUT_GENERAL) return false;   /* a compatible pass for pipeline creation names no real layout */
    }
    if (d->fetch_mask >> LAGFX_RENDER_MAX_COLOR) return false;
    return true;
}

/* compat: the LOAD/STORE variant used for pipeline creation (compatibility ignores ops and layouts). */
static void make_key(rpkey_t *k, const lagfx_render_desc_t *d, bool compat) {
    memset(k, 0, sizeof *k);
    k->ncolor = d->color_count;
    k->fetch_mask = d->fetch_mask;
    for (uint32_t i = 0; i < d->color_count; i++) {
        const lagfx_render_attachment_t *a = &d->color[i];
        if (a->format == VK_FORMAT_UNDEFINED) continue;   /* unused slot: all-zero entry, only its position is part of the key */
        k->a[i] = (akey_t){ a->format, samples_of(a), (d->fetch_mask & (1u << i)) ? VK_IMAGE_LAYOUT_GENERAL : a->layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->layout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,   /* never UNDEFINED: not valid for an attachment reference or a LOAD */
                            compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->load, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                            compat ? VK_ATTACHMENT_STORE_OP_STORE : a->store, VK_ATTACHMENT_STORE_OP_DONT_CARE, 0, 0, 0 };
        if (!compat && a->resolve_view != VK_NULL_HANDLE) k->a[i].rlayout = a->resolve_layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->resolve_layout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (d->has_depth_stencil) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        k->has_ds = 1;
        k->a[d->color_count] = (akey_t){ a->format, samples_of(a), a->layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->load, compat ? VK_ATTACHMENT_LOAD_OP_LOAD : a->stencil_load,
                                         compat ? VK_ATTACHMENT_STORE_OP_STORE : a->store, compat ? VK_ATTACHMENT_STORE_OP_STORE : a->stencil_store, 0, 0, 0 };
        if (!compat && a->resolve_view != VK_NULL_HANDLE) {   /* depth / stencil resolve: the resolve attachment's layout and the two modes are part of the pass */
            k->a[d->color_count].rlayout = a->resolve_layout != VK_IMAGE_LAYOUT_UNDEFINED ? a->resolve_layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            k->a[d->color_count].dmode = a->resolve_mode, k->a[d->color_count].smode = a->stencil_resolve_mode;
        }
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
    VkAttachmentDescription ad[MAX_ATT]; VkAttachmentReference cref[LAGFX_RENDER_MAX_COLOR], rref[LAGFX_RENDER_MAX_COLOR]; VkAttachmentReference dref = { 0 };
    bool any_resolve = false;
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
    for (uint32_t i = 0; i < k->ncolor; i++) {   /* resolve targets come after every real attachment (the framebuffer lists their views in the same order) */
        rref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        const akey_t *a = &k->a[i];
        if (a->fmt == VK_FORMAT_UNDEFINED || a->rlayout == 0) continue;
        ad[na] = (VkAttachmentDescription){ .format = a->fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                            .initialLayout = (VkImageLayout)a->rlayout, .finalLayout = (VkImageLayout)a->rlayout };
        rref[i] = (VkAttachmentReference){ na, (VkImageLayout)a->rlayout };
        na++; any_resolve = true;
    }
    VkAttachmentReference dsres_ref = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };   /* depth / stencil resolve target: the very last attachment (the framebuffer lists its view last) */
    const bool ds_resolve = k->has_ds && k->a[k->ncolor].rlayout != 0;
    if (ds_resolve) {
        const akey_t *a = &k->a[k->ncolor];
        ad[na] = (VkAttachmentDescription){ .format = a->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
                                            .loadOp = a->dmode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,   /* an aspect that is not resolved (mode NONE) keeps its contents */
                                            .stencilLoadOp = a->smode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .initialLayout = (VkImageLayout)a->rlayout, .finalLayout = (VkImageLayout)a->rlayout };
        dsres_ref = (VkAttachmentReference){ na, (VkImageLayout)a->rlayout };
        na++;
    }
    VkAttachmentReference iref[LAGFX_RENDER_MAX_COLOR]; uint32_t nin = 0;   /* input attachment k = colour slot k (legacy framebuffer fetch); holes stay UNUSED */
    for (uint32_t i = 0; i < k->ncolor; i++) {
        iref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        if (!(k->fetch_mask & (1u << i)) || cref[i].attachment == VK_ATTACHMENT_UNUSED) continue;
        iref[i] = (VkAttachmentReference){ cref[i].attachment, VK_IMAGE_LAYOUT_GENERAL };
        nin = i + 1u;
    }
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor, .pColorAttachments = k->ncolor ? cref : NULL,
                                .pResolveAttachments = any_resolve ? rref : NULL, .pDepthStencilAttachment = k->has_ds ? &dref : NULL,
                                .inputAttachmentCount = nin, .pInputAttachments = nin ? iref : NULL };
    /* External dependencies: the caller's barrier covers the layout change; these order the attachment writes against what comes before / after the pass. */
    VkPipelineStageFlags ds_st = k->has_ds ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : 0;
    VkAccessFlags ds_ac = k->has_ds ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0;
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | ds_st,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st | (nin ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0u),
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | (ds_ac & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (nin ? VK_ACCESS_INPUT_ATTACHMENT_READ_BIT : 0u) | ds_ac, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | ds_st,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (ds_ac & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | ds_ac, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT },
    };
    VkSubpassDependency full[3]; memcpy(full, dep, sizeof dep);
    if (nin) full[2] = (VkSubpassDependency){ .srcSubpass = 0, .dstSubpass = 0,   /* same-pixel read of an earlier draw's colour write inside the pass (the barrier lagfx records between draws) */
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = ad,
                                  .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = nin ? 3u : 2u, .pDependencies = full };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult vr;
    if (!ds_resolve) vr = vkCreateRenderPass(vk->device, &ci, NULL, &rp);
    else {
        /* depth / stencil resolve exists only in VkRenderPassCreateInfo2 (VkSubpassDescriptionDepthStencilResolve): convert the one-subpass pass built above. Only passes that need it take this path. */
        if (!vk->have_ds_resolve || !vk->create_renderpass2) { LAGFX_ERR("render_pass: depth/stencil resolve requested but the device has no vkCreateRenderPass2 + VK_KHR_depth_stencil_resolve"); return VK_NULL_HANDLE; }
        VkAttachmentDescription2 ad2[MAX_ATT]; VkAttachmentReference2 cref2[LAGFX_RENDER_MAX_COLOR], rref2[LAGFX_RENDER_MAX_COLOR], iref2[LAGFX_RENDER_MAX_COLOR], dref2 = { 0 }, dsres2 = { 0 };
        for (uint32_t i = 0; i < na; i++)
            ad2[i] = (VkAttachmentDescription2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = ad[i].format, .samples = ad[i].samples, .loadOp = ad[i].loadOp, .storeOp = ad[i].storeOp,
                                                 .stencilLoadOp = ad[i].stencilLoadOp, .stencilStoreOp = ad[i].stencilStoreOp, .initialLayout = ad[i].initialLayout, .finalLayout = ad[i].finalLayout };
        for (uint32_t i = 0; i < k->ncolor; i++) {
            cref2[i] = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = cref[i].attachment, .layout = cref[i].layout, .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
            rref2[i] = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = rref[i].attachment, .layout = rref[i].layout, .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
            iref2[i] = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = iref[i].attachment, .layout = iref[i].layout, .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };   /* v2 requires the aspect of an input attachment */
        }
        dref2 = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = dref.attachment, .layout = dref.layout, .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT };
        dsres2 = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = dsres_ref.attachment, .layout = dsres_ref.layout, .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT };
        VkSubpassDescriptionDepthStencilResolve dsr = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
            .depthResolveMode = k->a[k->ncolor].dmode ? k->a[k->ncolor].dmode : VK_RESOLVE_MODE_NONE, .stencilResolveMode = k->a[k->ncolor].smode ? k->a[k->ncolor].smode : VK_RESOLVE_MODE_NONE,
            .pDepthStencilResolveAttachment = &dsres2 };
        VkSubpassDescription2 sp2 = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, .pNext = &dsr, .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      .inputAttachmentCount = nin, .pInputAttachments = nin ? iref2 : NULL, .colorAttachmentCount = k->ncolor, .pColorAttachments = k->ncolor ? cref2 : NULL,
                                      .pResolveAttachments = any_resolve ? rref2 : NULL, .pDepthStencilAttachment = &dref2 };
        VkSubpassDependency2 dep2[3];
        for (uint32_t i = 0; i < ci.dependencyCount; i++)
            dep2[i] = (VkSubpassDependency2){ .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, .srcSubpass = full[i].srcSubpass, .dstSubpass = full[i].dstSubpass, .srcStageMask = full[i].srcStageMask,
                                              .dstStageMask = full[i].dstStageMask, .srcAccessMask = full[i].srcAccessMask, .dstAccessMask = full[i].dstAccessMask, .dependencyFlags = full[i].dependencyFlags };
        /* the resolve happens at the end of the subpass (colour-attachment-output stage for the colour resolve, the late fragment-test stage for depth/stencil): the existing subpass->external dependency already covers both stages and the attachments' writes */
        VkRenderPassCreateInfo2 ci2 = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, .attachmentCount = na, .pAttachments = ad2, .subpassCount = 1, .pSubpasses = &sp2,
                                        .dependencyCount = ci.dependencyCount, .pDependencies = dep2 };
        vr = vk->create_renderpass2(vk->device, &ci2, NULL, &rp);
    }
    if (vr != VK_SUCCESS) { LAGFX_ERR("render_pass: %s failed (%d)", ds_resolve ? "vkCreateRenderPass2" : "vkCreateRenderPass", (int)vr); return VK_NULL_HANDLE; }
    c->rp[c->n_rp].key = *k; c->rp[c->n_rp].rp = rp; c->n_rp++;
    return rp;
}

VkRenderPass lagfx_vk_render_pass_compat(struct lagfx_vk_state *vk, const lagfx_render_desc_t *d) {
    if (!vk || vk->device == VK_NULL_HANDLE || !desc_valid(d, false) || !fetch_valid(d, false)) return VK_NULL_HANDLE;
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
        if (a->resolve_view != VK_NULL_HANDLE) { col[i].resolveImageView = a->resolve_view; col[i].resolveImageLayout = a->resolve_layout ? a->resolve_layout : a->layout; col[i].resolveMode = a->resolve_mode ? a->resolve_mode : VK_RESOLVE_MODE_AVERAGE_BIT; }
    }
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { d->width, d->height } }, .layerCount = 1,
                           .colorAttachmentCount = d->color_count, .pColorAttachments = d->color_count ? col : NULL };
    if (d->has_depth_stencil) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        if (fmt_has_depth(a->format)) {
            dep = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view, .imageLayout = a->layout,
                                               .loadOp = a->load, .storeOp = a->store, .clearValue = a->clear };
            if (a->resolve_view != VK_NULL_HANDLE && a->resolve_mode) { dep.resolveMode = a->resolve_mode; dep.resolveImageView = a->resolve_view; dep.resolveImageLayout = a->resolve_layout ? a->resolve_layout : a->layout; }
            ri.pDepthAttachment = &dep;
        }
        if (fmt_has_stencil(a->format)) {
            ste = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view, .imageLayout = a->layout,
                                               .loadOp = a->stencil_load, .storeOp = a->stencil_store, .clearValue = a->clear };
            if (a->resolve_view != VK_NULL_HANDLE && a->stencil_resolve_mode) { ste.resolveMode = a->stencil_resolve_mode; ste.resolveImageView = a->resolve_view; ste.resolveImageLayout = a->resolve_layout ? a->resolve_layout : a->layout; }
            ri.pStencilAttachment = &ste;
        }
    }
    vkCmdBeginRendering(cmd, &ri);
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_render_begin_desc(struct lagfx_vk_state *vk, VkCommandBuffer cmd, const lagfx_render_desc_t *d) {
    if (!vk || cmd == VK_NULL_HANDLE || !desc_valid(d, true)) return LAGFX_ERR_INVALID_ARG;
    if (lagfx_vk_device_lost(vk)) { LAGFX_ERR("render_begin: device lost earlier; no new work"); return LAGFX_ERR_BACKEND; }
    if (d->has_depth_stencil && d->depth_stencil.resolve_view != VK_NULL_HANDLE) {
        const lagfx_render_attachment_t *a = &d->depth_stencil;
        if (!lagfx_vk_ds_resolve_ok(vk, a->format, fmt_has_depth(a->format) ? a->resolve_mode : 0, fmt_has_stencil(a->format) ? a->stencil_resolve_mode : 0)) {
            LAGFX_ERR("render_begin: depth/stencil resolve (depth mode 0x%x, stencil mode 0x%x) not supported by the device", (unsigned)a->resolve_mode, (unsigned)a->stencil_resolve_mode);
            return LAGFX_ERR_BACKEND;
        }
    }
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) return begin_dynamic(vk, cmd, d);

    if (!fetch_valid(d, true)) return LAGFX_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < d->color_count && i < LAGFX_RENDER_MAX_COLOR; i++)   /* a multisampled input attachment needs the per-sample read path (subpassInputMS + sample-rate shading pipelines): the descriptor entry point checks it for every caller */
        if ((d->fetch_mask & (1u << i)) && samples_of(&d->color[i]) != VK_SAMPLE_COUNT_1_BIT && !lagfx_vk_fetch_ms_capable(vk)) return LAGFX_ERR_INVALID_ARG;

    /* the legacy resolve is implicit: a resolve_mode (0 = average) it would not perform is refused here, for every caller of the descriptor entry point, not only the encoder */
    for (uint32_t i = 0; i < d->color_count; i++) {
        const lagfx_render_attachment_t *a = &d->color[i];
        if (a->resolve_view == VK_NULL_HANDLE) continue;
        const VkResolveModeFlagBits want = a->resolve_mode ? a->resolve_mode : VK_RESOLVE_MODE_AVERAGE_BIT;
        if (!lagfx_legacy_resolve_mode_ok(a->format, want)) {
            LAGFX_ERR("render_begin: legacy backend: colour %u asks resolve mode 0x%x, the implicit resolve of this format does not perform it", i, (unsigned)want);
            return LAGFX_ERR_BACKEND;
        }
    }

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
    for (uint32_t i = 0; i < d->color_count; i++) {   /* resolve targets: after the real attachments, in the order rp_locked numbers them */
        if (d->color[i].format == VK_FORMAT_UNDEFINED || d->color[i].resolve_view == VK_NULL_HANDLE) continue;
        views[na] = d->color[i].resolve_view; memset(&cv[na], 0, sizeof cv[na]); na++;
    }
    if (d->has_depth_stencil && d->depth_stencil.resolve_view != VK_NULL_HANDLE) { views[na] = d->depth_stencil.resolve_view; memset(&cv[na], 0, sizeof cv[na]); na++; }   /* last, as in rp_locked */
    rpkey_t k; make_key(&k, d, false);
    pthread_mutex_lock(&rc->mu);
    VkRenderPass rp = rp_locked(vk, rc, &k);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (rp != VK_NULL_HANDLE) {
        for (unsigned i = 0; i < rc->n_fb && fb == VK_NULL_HANDLE; i++) {
            const fb_ent_t *e = &rc->fb[i];
            if (e->rp == rp && e->nview == na && e->w == d->width && e->h == d->height && !memcmp(e->view, views, na * sizeof views[0])) { fb = e->fb; rc->st.fb_hits++; }
        }
        if (fb == VK_NULL_HANDLE) {
            if (rc->n_fb >= rc->max_fb) { rc->st.fb_full++; LAGFX_ERR("render_begin: framebuffer cache full (%u): a view is not being forgotten", rc->n_fb); goto fb_done; }
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
                rc->st.fb_created++; if (rc->n_fb > rc->st.fb_peak) rc->st.fb_peak = rc->n_fb;
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
            else {
                struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
                vr = vkDeviceWaitIdle(vk->device);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                uint64_t us = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000ll + (t1.tv_nsec - t0.tv_nsec) / 1000ll);
                c->st.forget_waits++; c->st.forget_wait_us += us; if (us > c->st.forget_wait_max_us) c->st.forget_wait_max_us = us;
            }
            if (vr == VK_SUCCESS) idle = true;
            else if (vr == VK_ERROR_DEVICE_LOST) { idle = true; lagfx_vk_mark_device_lost(vk); LAGFX_ERR("render_forget_view: device lost; the objects are destroyed (nothing can still run), but no new work is accepted from now on"); }
            else { out = LAGFX_FORGET_DEFERRED; c->st.forget_deferred++; LAGFX_ERR("render_forget_view: vkDeviceWaitIdle failed (%d); framebuffers and the view's resources are kept whole and retried later", (int)vr); }
        }
        if (out == LAGFX_FORGET_DEFERRED) { i++; continue; }
        vkDestroyFramebuffer(vk->device, c->fb[i].fb, NULL);
        c->st.fb_destroyed++;
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

bool lagfx_legacy_resolve_mode_ok(VkFormat fmt, VkResolveModeFlagBits requested) {
    return requested != 0 && requested == lagfx_msaa_resolve_mode(fmt);
}

bool lagfx_vk_fetch_capable(const struct lagfx_vk_state *vk) {
    if (!vk) return false;
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) return vk->have_local_read;
    return vk->legacy_fetch;
}
VkImageLayout lagfx_vk_fetch_layout(const struct lagfx_vk_state *vk) {
    if (!lagfx_vk_fetch_capable(vk)) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC ? (VkImageLayout)1000232000 /* VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR */ : VK_IMAGE_LAYOUT_GENERAL;
}
bool lagfx_vk_raster_order(const struct lagfx_vk_state *vk) { return vk && vk->have_raster_order && lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC; }
bool lagfx_vk_fetch_ms_capable(const struct lagfx_vk_state *vk) {
    if (!vk) return false;
    if (lagfx_vk_render_backend(vk) == LAGFX_RENDER_DYNAMIC) return vk->have_local_read && vk->have_local_read_ms;
    return vk->legacy_fetch && vk->have_sample_rate_shading;   /* legacy: multisampled input attachment (subpassInputMS) read per sample = sample-rate shading pipeline */
}
uint32_t lagfx_legacy_fetch_mask(const lagfx_render_desc_t *d) {
    uint32_t m = 0;
    if (!d) return 0;
    for (uint32_t i = 0; i < d->color_count && i < 4u; i++)
        if (d->color[i].format != VK_FORMAT_UNDEFINED) m |= 1u << i;   /* every real slot; multisampled ones only take part through the caller's capability / usage gate */
    return m;
}

void lagfx_vk_render_cache_stats(struct lagfx_vk_state *vk, lagfx_render_cache_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!vk || !vk->render_cache) return;
    struct lagfx_render_cache *c = vk->render_cache;
    pthread_mutex_lock(&c->mu);
    *out = c->st; out->fb_live = c->n_fb; out->rp_live = c->n_rp;
    pthread_mutex_unlock(&c->mu);
}
