/* libapplegfx-vulkan — ARM64 exec: guest-visible multisample path (see arm64_msaa.h). SPDX-License-Identifier: MIT */
#include "arm64_msaa.h"
#include "../common/le.h"
#include "../common/log.h"
#include "../vulkan/msaa_host.h"
#include "../vulkan/render_pass.h"

#ifdef LAGFX_HAVE_VULKAN
#include <stdio.h>
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
    const VkImageUsageFlags opt_ms = ds ? 0u : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);   /* texelFetch of a sample; input attachment = per-sample framebuffer fetch (spv_msfetch.c); no transfers (a multisampled image cannot take part in a buffer copy) */
    const VkImageUsageFlags opt_1x = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ds ? 0u : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT));
    lagfx_msaa_plan_t pl;
    const lagfx_msaa_mode_t mode = lagfx_msaa_mode();
    memset(cu, 0, sizeof *cu);
    const int rc = lagfx_msaa_plan_mode(be, o->format, o->width, o->height, o->samples_req, req, opt_ms, opt_1x, mode, &pl);
    if (rc != LAGFX_MSAA_OK) {
        MS(p).ms_tex_refused++;
        if (rc == LAGFX_MSAA_EXACT_REFUSED) {   /* exact mode: the guest's count is not offered exactly: the texture does not exist (never approximated) */
            static unsigned logged;
            MS(p).ms_exact_refused++;
            if (logged++ < 8u) LAGFX_WARN("g7-gpu-msaa: mode=exact refused %ux multisample texture fmt=%d %ux%u (not offered exactly; LAGFX_MSAA_COMPAT=1 would clamp / fall back to 1x, NOT exact MSAA)", (unsigned)o->samples_req, (int)o->format, (unsigned)o->width, (unsigned)o->height);
        } else p->diag2.exec.fmt.refused[LAGFX_FMTCAP_NO_REQUIRED]++;
        return false;
    }
    cu->usage = pl.usage, cu->dropped = pl.dropped, cu->why = LAGFX_FMTCAP_OK;
    *ns = pl.samples;
    o->samples = (uint8_t)pl.samples;
    if (pl.samples > 1u) MS(p).ms_images_created++;
    if (pl.clamped && pl.samples > 1u) MS(p).ms_compat_clamped++;   /* only reachable in compat mode */
    if (pl.fallback_1x) MS(p).ms_compat_1x++;                       /* only reachable in compat mode */
    if ((pl.clamped && pl.samples > 1u) || pl.fallback_1x) {
        static unsigned logged;
        if (logged++ < 8u) LAGFX_WARN("g7-gpu-msaa: mode=compat %ux multisample texture granted %ux (NOT exact MSAA)", (unsigned)o->samples_req, (unsigned)pl.samples);
    }
    return true;
}

bool lagfx_a64_msaa_attach_ok(lagfx_protocol_t *p, const lagfx_a64_obj_t *t) {
    if (!t || t->samples_req <= 1u || lagfx_msaa_mode() == LAGFX_MSAA_MODE_COMPAT) return true;
    if (t->samples == t->samples_req) return true;
    MS(p).ms_pass_refused++;
    return false;
}

void lagfx_a64_msaa_note_missing(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t ref) {
    if (ref && lagfx_a64_res_fail_why(x, ref) == TEXWHY_C_MSAA) MS(p).ms_pass_refused++;
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

/* Why the resolve texture `rref` cannot be the resolve target of multisampled attachment `t` (NULL = it can; *out = the object). Shared by the pass gate and lagfx_a64_msaa_color. */
static const char *resolve_why(lagfx_a64_rexec_t *x, const lagfx_a64_obj_t *t, uint32_t rref, bool sub_nonzero, const lagfx_a64_obj_t *const attached[4], lagfx_a64_obj_t **out) {
    lagfx_a64_obj_t *r = rref ? lagfx_a64_res_object(x, rref) : NULL;
    *out = r;
    if (!rref) return "resolve_texture_none";
    if (!r) return "resolve_texture_missing";
    if (sub_nonzero) return "resolve_subresource";
    if (!same_shape(t, r)) return "resolve_shape_mismatch";
    if (!(r->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return "resolve_not_attachable";
    for (uint32_t q = 0; q < 4u; q++) if (attached[q] == r) return "resolve_is_attachment";
    return NULL;
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
        const char *why = resolve_why(x, t, resolve_ref, resolve_sub_nonzero, attached, &r);
        if (!why && fallback && !((t->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (r->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))) why = "copy_usage";   /* the 1x copy needs the transfer usages */
        if (why) r = NULL, ms->fail = why;
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
    if (slot >= 4u || !lagfx_a64_res_target_begin(x, r, &rv, &w, &h)) { MS(p).resolve_no_target++; ca->storeOp = VK_ATTACHMENT_STORE_OP_STORE; ms->fail = "resolve_target_begin"; return false; }
    ca->resolveMode = lagfx_msaa_resolve_mode(t->format), ca->resolveImageView = rv, ca->resolveImageLayout = lagfx_a64_res_target_layout(x, r);   /* the layout target_begin put the resolve target in */
    ms->res[slot] = r, ms->nres++;
    MS(p).resolves++;
    return true;
}


/* ---- exact-mode whole-pass rejection ---- */
static char g_last_reject[96];
const char *lagfx_a64_msaa_last_reject(void) { return g_last_reject; }

void lagfx_a64_msaa_reject(lagfx_protocol_t *p, const lagfx_a64_msaa_veto_t *v) {
    static unsigned logged;
    MS(p).ms_pass_refused++;
    snprintf(g_last_reject, sizeof g_last_reject, "%s: %s", v->what, v->why);
    if (logged++ < 16u) LAGFX_WARN("g7-gpu-msaa: mode=exact whole pass REJECTED (no draw, no partial rendering): %s ref=%u (LAGFX_MSAA_COMPAT=1 would skip just that attachment, NOT exact MSAA)", g_last_reject, (unsigned)v->ref);
}

static bool veto_set(lagfx_a64_msaa_veto_t *v, const char *what, int idx, const char *why, uint32_t ref) {
    if (idx >= 0) snprintf(v->what, sizeof v->what, "%s%d", what, idx); else snprintf(v->what, sizeof v->what, "%s", what);
    v->why = why, v->ref = ref;
    return false;
}

static bool attach_ok_quiet(const lagfx_a64_obj_t *t) { return !t || t->samples_req <= 1u || t->samples == t->samples_req; }

/* The object a pass names for `ref`, with the reason it cannot be an attachment of an exact multisample pass (NULL = usable). */
static const char *named_why(lagfx_a64_rexec_t *x, uint32_t ref, lagfx_a64_obj_t *o) {
    if (!o) return lagfx_a64_res_fail_why(x, ref) == TEXWHY_C_MSAA ? "multisample_texture_refused" : "texture_missing";
    if (o->kind != LAGFX_A64_TEXTURE) return "not_a_texture";
    if (!attach_ok_quiet(o)) return "host_sample_count_differs";
    return NULL;
}

static lagfx_msaa_dsres_caps_t ds_caps(const struct lagfx_vk_state *vk) {
    lagfx_msaa_dsres_caps_t c; memset(&c, 0, sizeof c);
    if (vk && vk->have_ds_resolve) c.probed = true, c.depth_modes = vk->dsres_depth_modes, c.stencil_modes = vk->dsres_stencil_modes, c.independent_none = vk->dsres_independent_none, c.independent = vk->dsres_independent;
    return c;
}

const char *lagfx_a64_msaa_ds_plan(lagfx_a64_rexec_t *x, const lagfx_render_pass_full_desc_t *d, const lagfx_a64_obj_t *dso, int *want, lagfx_a64_obj_t **rt, uint32_t *dm, uint32_t *sm) {
    *want = 0, *rt = NULL, *dm = *sm = 0;
    if (!dso || lagfx_a64_msaa_obj_samples(dso) <= 1u) return NULL;
    const bool hd = d->has_depth && d->depth_texture_ref && (dso->ds_aspects & 1u);
    const bool hs = d->has_stencil && (dso->ds_aspects & 2u) && d->stencil_texture_ref == (hd ? d->depth_texture_ref : d->stencil_texture_ref);
    const uint32_t da = hd ? d->depth_store_action : 0u, sa = hs ? d->stencil_store_action : 0u;
    const bool rd = hd && (da == 2u || da == 3u), rs = hs && (sa == 2u || sa == 3u);
    if ((hd && da > 3u) || (hs && sa > 3u)) { *want = 1; return "depth_stencil_store_action_unknown"; }
    if (!rd && !rs) return NULL;
    *want = 1;
    const uint32_t rref = rd ? d->depth_resolve_texture_ref : d->stencil_resolve_texture_ref;
    if (rd && rs && d->depth_resolve_texture_ref != d->stencil_resolve_texture_ref) return "depth_stencil_resolve_split";
    if (!rref) return "resolve_texture_none";
    lagfx_a64_obj_t *r = lagfx_a64_res_object(x, rref);
    if (!r) return "resolve_texture_missing";
    if (r->kind != LAGFX_A64_TEXTURE || !r->ds_aspects) return "resolve_not_depth_stencil";
    if (r == dso) return "resolve_is_attachment";
    if (r->format != dso->format || r->width != dso->width || r->height != dso->height || lagfx_a64_msaa_obj_samples(r) != 1u) return "resolve_shape_mismatch";
    if (!(r->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return "resolve_not_attachable";
    const lagfx_msaa_dsres_caps_t caps = ds_caps(lagfx_a64_res_vk(x));
    VkResolveModeFlagBits m1 = 0, m2 = 0;
    if (lagfx_msaa_dsres_choose(&caps, rd, d->depth_resolve_filter, rs, d->stencil_resolve_filter, &m1, &m2) != LAGFX_MSAA_DSRES_OK) return "resolve_mode_unsupported";
    if (!lagfx_vk_ds_resolve_ok(lagfx_a64_res_vk(x), dso->format, rd ? m1 : 0, rs ? m2 : 0)) return "resolve_mode_unsupported";
    *rt = r, *dm = rd ? m1 : 0, *sm = rs ? m2 : 0;
    return NULL;
}

bool lagfx_a64_msaa_ds_resolve(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms, const lagfx_render_pass_full_desc_t *d, lagfx_a64_obj_t *dso, VkRenderingAttachmentInfo *dai, VkRenderingAttachmentInfo *sai) {
    int want; lagfx_a64_obj_t *r; uint32_t dm, sm;
    const char *why = lagfx_a64_msaa_ds_plan(x, d, dso, &want, &r, &dm, &sm);
    if (!want) return false;
    if (why) { MS(p).ds_resolve_refused++; MS(p).ds_resolve_unsupported++; ms->fail = why; return false; }
    VkImageView rv; uint32_t w, h;
    if (!lagfx_a64_res_ds_begin(x, r, &rv, &w, &h)) { MS(p).ds_resolve_refused++; ms->fail = "resolve_target_begin"; return false; }
    /* the plan already ran the shared chooser; the shared attachment filler sets the modes (a mode of 0 = aspect not resolved leaves its info untouched) */
    const lagfx_msaa_dsres_caps_t caps = ds_caps(lagfx_a64_res_vk(x));
    const bool rd = dm != 0, rs = sm != 0;
    if (lagfx_msaa_dsres_attachments(&caps, rd, d->depth_resolve_filter, rs, d->stencil_resolve_filter, rv, VK_IMAGE_LAYOUT_GENERAL, dai, sai, &MS(p)) != LAGFX_MSAA_DSRES_OK) {
        lagfx_a64_res_ds_end(x, r); ms->fail = "resolve_mode_unsupported"; return false;
    }
    ms->ds_res = r; MS(p).ds_resolves++;
    return true;
}

bool lagfx_a64_msaa_pass_gate(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, const lagfx_a64_msaa_pass_t *ms, const lagfx_render_pass_full_desc_t *d, bool ds_ok, lagfx_a64_msaa_look_t *look, bool *strict, lagfx_a64_msaa_veto_t *v) {
    lagfx_a64_obj_t **co = look->c, *dobj = NULL;
    const uint32_t nc = d->color_attachment_count < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS ? d->color_attachment_count : LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS;
    const uint32_t dref = d->has_depth ? d->depth_texture_ref : 0u, sref = d->has_stencil ? d->stencil_texture_ref : 0u, dsref = dref ? dref : sref;
    memset(v, 0, sizeof *v), memset(look, 0, sizeof *look);
    *strict = false;
    bool involved = false;
    for (uint32_t k = 0; k < nc; k++) {   /* the one lookup of the colour objects (the caller reuses look->c) */
        const uint64_t u0 = lagfx_a64_res_stats(p)->linear_unsupported;
        co[k] = lagfx_a64_res_object(x, d->colors[k].texture_ref);
        look->lin[k] = lagfx_a64_res_stats(p)->linear_unsupported != u0;
        involved |= co[k] ? co[k]->samples_req > 1u : lagfx_a64_res_fail_why(x, d->colors[k].texture_ref) == TEXWHY_C_MSAA;
    }
    if (dsref && ds_ok) {
        dobj = look->ds = lagfx_a64_res_object(x, dsref), look->ds_looked = true;
        involved |= dobj ? dobj->samples_req > 1u : lagfx_a64_res_fail_why(x, dsref) == TEXWHY_C_MSAA;
    }
    if (lagfx_msaa_mode() == LAGFX_MSAA_MODE_COMPAT) return true;
    if (!involved) return true;   /* no multisampled attachment named: the pass is not subject to the whole-pass rule */
    *strict = true;
    uint32_t samples = 0, w0 = 0, h0 = 0;
    const lagfx_a64_obj_t *attached[4] = { 0 };
    for (uint32_t k = 0; k < nc; k++) {
        const uint32_t slot = d->colors[k].slot, ref = d->colors[k].texture_ref;
        const char *why;
        if (slot >= 4u) return veto_set(v, "colour", (int)slot, "slot_out_of_range", ref);
        why = named_why(x, ref, co[k]);
        if (why) return veto_set(v, "colour", (int)slot, why, ref);
        if (!(co[k]->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return veto_set(v, "colour", (int)slot, "not_attachable", ref);
        if (co[k]->img_type != VK_IMAGE_TYPE_2D) return veto_set(v, "colour", (int)slot, "not_2d", ref);
        for (uint32_t q = 0; q < 4u; q++) if (attached[q] == co[k]) return veto_set(v, "colour", (int)slot, "duplicate_attachment", ref);
        attached[slot] = co[k];
        const uint32_t ns = lagfx_a64_msaa_obj_samples(co[k]);
        if (!samples) samples = ns, w0 = co[k]->width, h0 = co[k]->height;
        else if (ns != samples) return veto_set(v, "colour", (int)slot, "mixed_sample_counts", ref);
        else if (co[k]->width != w0 || co[k]->height != h0) return veto_set(v, "colour", (int)slot, "extent_mismatch", ref);
    }
    if (dsref) {
        if (!ds_ok) return veto_set(v, "depth", -1, "no_depth_stencil_support", dsref);
        const char *why = named_why(x, dsref, dobj);
        if (why) return veto_set(v, dref ? "depth" : "stencil", -1, why, dsref);
        const char *what = dref ? "depth" : "stencil";
        if (!dobj->ds_aspects || (dref && !(dobj->ds_aspects & 1u)) || (!dref && !(dobj->ds_aspects & 2u))) return veto_set(v, what, -1, "not_depth_stencil_texture", dsref);
        if (dref && sref && sref != dref) return veto_set(v, "stencil", -1, "stencil_in_other_texture", sref);
        if (dref && d->depth_level) return veto_set(v, what, -1, "depth_level", dsref);
        for (uint32_t q = 0; q < 4u; q++) if (attached[q] == dobj) return veto_set(v, what, -1, "duplicate_attachment", dsref);
        const uint32_t ns = lagfx_a64_msaa_obj_samples(dobj);
        if (samples && ns != samples) return veto_set(v, what, -1, "mixed_sample_counts", dsref);
        if (samples && (dobj->width < w0 || dobj->height < h0)) return veto_set(v, what, -1, "extent_smaller_than_colour", dsref);
        samples = ns;
        if (ns > 1u) {   /* depth / stencil resolve: planned by the same function the pass uses (a refusal is a whole-pass refusal in exact mode) */
            int want; lagfx_a64_obj_t *rt; uint32_t dm, sm;
            const char *dwhy = lagfx_a64_msaa_ds_plan(x, d, dobj, &want, &rt, &dm, &sm);
            if (dwhy) { MS(p).ds_resolve_refused++; return veto_set(v, what, -1, dwhy, dsref); }
        }
    }
    /* resolve targets the store actions require (deferred tags 0x66 / 0x96 override the descriptor, as lagfx_a64_msaa_color does) */
    for (uint32_t k = 0; k < nc; k++) {
        const uint32_t slot = d->colors[k].slot;
        if (lagfx_a64_msaa_obj_samples(co[k]) <= 1u) continue;
        uint32_t act = d->colors[k].store_action, rref = d->colors[k].resolve_texture_ref;
        bool sub = d->colors[k].resolve_level || d->colors[k].resolve_slice;
        if (slot < 8u && ms->st_set[slot]) act = ms->st_act[slot];
        if (slot < 8u && ms->res_set[slot]) rref = ms->res_ref[slot], sub = false;
        if (act != 2u && act != 3u) continue;
        lagfx_a64_obj_t *r;
        const char *why = resolve_why(x, co[k], rref, sub, attached, &r);
        if (why) { MS(p).resolve_no_target++; return veto_set(v, "resolve", (int)slot, why, rref); }
    }
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
    if (ms->ds_res) lagfx_a64_res_ds_end(x, ms->ds_res);
    lagfx_a64_msaa_pass_reset(ms);
}

void lagfx_a64_msaa_pass_abort(lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms) {
    for (uint32_t i = 0; i < 4u; i++) if (ms->res[i] && !ms->copy_src[i]) lagfx_a64_res_target_end(x, ms->res[i]);
    if (ms->ds_res) lagfx_a64_res_ds_end(x, ms->ds_res);
    lagfx_a64_msaa_pass_reset(ms);
}
#endif
