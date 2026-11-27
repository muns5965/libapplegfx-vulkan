/* libapplegfx-vulkan — ARM64 exec: guest-visible multisample path (see arm64_msaa.h). SPDX-License-Identifier: MIT */
#include "arm64_msaa.h"
#include "../common/le.h"

#ifdef LAGFX_HAVE_VULKAN
#include <string.h>

#define MS(p) ((p)->diag2.exec.msaa)

int lagfx_a64_msaa_tex_gate(lagfx_protocol_t *p, uint32_t mtype, uint32_t samples, uint32_t mips, uint32_t layers, uint32_t *req) {
    *req = 1;
    if (samples <= 1u && mtype != LAGFX_A64_MTL_TEX_2D_MS && mtype != LAGFX_A64_MTL_TEX_2D_MS_ARRAY) return LAGFX_A64_MSAA_PLAIN;
    if (mtype == LAGFX_A64_MTL_TEX_2D_MS && samples > 1u && samples <= 16u && mips <= 1u && layers <= 1u) { *req = samples; return LAGFX_A64_MSAA_YES; }
    /* every other combination is not a texture Metal can create: sampleCount > 1 needs the 2D multisample type, that type needs sampleCount > 1, one mip, one layer; the multisample
     * array type has no mirror (counted, never guessed) */
    p->diag2.exec.fmt.msaa_refused++;
    MS(p).ms_tex_bad_shape++;
    return LAGFX_A64_MSAA_REFUSE;
}

bool lagfx_a64_msaa_plan_image(lagfx_protocol_t *p, const lagfx_fmtcap_backend_t *be, lagfx_a64_obj_t *o, VkSampleCountFlagBits *ns, lagfx_fmtcap_usage_t *cu) {
    const bool ds = o->ds_aspects != 0;
    const VkImageUsageFlags req = ds ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkImageUsageFlags opt_ms = ds ? 0u : VK_IMAGE_USAGE_SAMPLED_BIT;   /* texelFetch of a sample; no transfers (a multisampled image cannot take part in a buffer copy) */
    const VkImageUsageFlags opt_1x = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ds ? 0u : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT));
    lagfx_msaa_plan_t pl;
    memset(cu, 0, sizeof *cu);
    if (lagfx_msaa_plan(be, o->format, o->width, o->height, o->samples_req, req, opt_ms, opt_1x, &pl) != LAGFX_MSAA_OK) {
        MS(p).ms_tex_refused++;
        p->diag2.exec.fmt.refused[LAGFX_FMTCAP_NO_REQUIRED]++;
        return false;
    }
    cu->usage = pl.usage, cu->dropped = pl.dropped, cu->why = LAGFX_FMTCAP_OK;
    *ns = pl.samples;
    o->samples = (uint8_t)pl.samples;
    if (pl.samples > 1u) { MS(p).ms_images_created++; if (pl.clamped) MS(p).ms_images_clamped++; }
    else MS(p).ms_fallback_1x++;
    return true;
}

void lagfx_a64_msaa_pass_reset(lagfx_a64_msaa_pass_t *ms) { memset(ms, 0, sizeof *ms); }

void lagfx_a64_msaa_prescan(lagfx_protocol_t *p, lagfx_a64_msaa_pass_t *ms, const uint8_t *recs, uint64_t lim) {
    (void)p;
    uint64_t q = 0; uint32_t guard = 0;
    while (q + 8u <= lim && guard++ < 4096u) {
        const uint32_t tag = lagfx_le32(recs + q), sz = lagfx_le32(recs + q + 4);
        if (sz < 8u || q + sz > lim) break;
        if (tag == 26u) break;
        const uint8_t *rec = recs + q + 8u; const uint32_t n = sz - 8u;
        if (tag == 102u && n >= 8u) {   /* SetColorStoreAction {u32 action, u32 index} */
            const uint32_t idx = lagfx_le32(rec + 4);
            if (idx < 8u) ms->st_set[idx] = true, ms->st_act[idx] = lagfx_le32(rec);
        } else if (tag == 150u && n >= 8u) {   /* SetColorResolveTexture {u32 ref, u32 slot, ...} */
            const uint32_t idx = lagfx_le32(rec + 4);
            if (idx < 8u) ms->res_set[idx] = true, ms->res_ref[idx] = lagfx_le32(rec);
        }
        q += sz;
    }
}

static bool same_shape(const lagfx_a64_obj_t *t, const lagfx_a64_obj_t *r) {
    return r->kind == LAGFX_A64_TEXTURE && !r->ds_aspects && r->img_type == VK_IMAGE_TYPE_2D && r->samples_req <= 1u && r != t && r->format == t->format && r->width == t->width && r->height == t->height;
}

bool lagfx_a64_msaa_color(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms, uint32_t slot, uint32_t cidx, lagfx_a64_obj_t *t, uint32_t store_action, uint32_t resolve_ref,
                          bool resolve_sub_nonzero, const lagfx_a64_obj_t *const attached[4], VkRenderingAttachmentInfo *ca) {
    const bool fallback = t->samples <= 1u && t->samples_req > 1u;   /* the guest texture is multisampled, the host image is not */
    uint32_t act = store_action, rref = resolve_ref;
    bool sub = resolve_sub_nonzero;
    if (cidx < 8u && ms->st_set[cidx]) act = ms->st_act[cidx];
    if (cidx < 8u && ms->res_set[cidx]) rref = ms->res_ref[cidx], sub = false;
    /* a resolve store action on a plain single-sample texture is invalid Metal: stored (the caller's default), counted */
    if (t->samples_req <= 1u) {
        if (act == 2u || act == 3u) MS(p).resolve_on_1x++;
        return false;
    }
    if (cidx < 8u) MS(p).deferred_store_tags += (ms->st_set[cidx] ? 1u : 0u) + (ms->res_set[cidx] ? 1u : 0u);
    if (!fallback) MS(p).ms_attach++;
    store_action = act, resolve_ref = rref, resolve_sub_nonzero = sub;
    const bool wants_resolve = store_action == 2u || store_action == 3u;
    lagfx_a64_obj_t *r = NULL;
    if (wants_resolve) {
        r = resolve_ref ? lagfx_a64_res_object(x, resolve_ref) : NULL;
        bool ok = r && !resolve_sub_nonzero && same_shape(t, r) && (r->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        for (uint32_t q = 0; ok && q < 4u; q++) if (attached[q] == r) ok = false;
        if (ok && fallback) ok = (t->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (r->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);   /* the 1x copy needs the transfer usages */
        if (!ok) r = NULL;
    }
    VkAttachmentStoreOp so; bool res;
    const bool known = lagfx_msaa_store_map(store_action, r != NULL, &so, &res);
    if (!known) {
        if (wants_resolve) MS(p).resolve_no_target++; else MS(p).store_unknown++;
        ca->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        return false;
    }
    ca->storeOp = so;
    if (!res) return false;
    if (fallback) {   /* a one-sample "resolve" is a copy after the pass; the attachment must be stored for it */
        ca->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (slot < 4u && !ms->copy_src[slot]) { ms->copy_src[slot] = t, ms->res[slot] = r; ms->nres++; MS(p).resolves++, MS(p).resolves_by_copy++; return true; }
        return false;
    }
    VkImageView rv; uint32_t w, h;
    if (slot >= 4u || !lagfx_a64_res_target_begin(x, r, &rv, &w, &h)) { MS(p).resolve_no_target++; ca->storeOp = VK_ATTACHMENT_STORE_OP_STORE; return false; }
    ca->resolveMode = lagfx_msaa_resolve_mode(t->format), ca->resolveImageView = rv, ca->resolveImageLayout = ca->imageLayout;
    ms->res[slot] = r, ms->nres++;
    MS(p).resolves++;
    return true;
}

static void barrier_mem(VkCommandBuffer cb, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &mb, 0, NULL, 0, NULL);
}

void lagfx_a64_msaa_pass_end(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms) {
    (void)p;
    VkCommandBuffer cb = lagfx_a64_res_cmdbuf(x);
    for (uint32_t i = 0; i < 4u; i++) {
        lagfx_a64_obj_t *r = ms->res[i];
        if (!r) continue;
        if (ms->copy_src[i]) {   /* both images are in GENERAL (target_end of the attachments ran): copy, with the access ordering around it */
            lagfx_a64_obj_t *s = ms->copy_src[i];
            barrier_mem(cb, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { s->width, s->height, 1 } };
            vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, r->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            barrier_mem(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        } else lagfx_a64_res_target_end(x, r);
    }
    lagfx_a64_msaa_pass_reset(ms);
}

void lagfx_a64_msaa_pass_abort(lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms) {
    for (uint32_t i = 0; i < 4u; i++) if (ms->res[i] && !ms->copy_src[i]) lagfx_a64_res_target_end(x, ms->res[i]);
    lagfx_a64_msaa_pass_reset(ms);
}
#endif
