/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — per-use rewrite of image samples that go through an unnormalized-coordinates sampler
 * src/air2spv/spv_unnorm.c
 *
 * Copyright © 2026 Matthew Jackson
 *
 * Vulkan (VUID-vkCmdDraw-None-08609 .. 08611, text as shipped in the Khronos validation layer used by our tests): a sampler created with unnormalizedCoordinates must not
 *   - sample a 3D / cube / 1D-array / 2D-array view,
 *   - be used by OpImageSample* / OpImageSparseSample* with ImplicitLod, Dref or Proj in the name,
 *   - be used by an OpImageSample* that carries a LOD bias or any offset.
 * Metal's coord::pixel sampler (MSL 4.1 table 2.7 "mip_filter ... If none, ... all samples are read from level 0"; coord::pixel requires mip_filter none) samples level 0 only,
 * so for such a sampler a bias / LOD / gradient / min-lod operand has no effect and the explicit-LOD-0 form is the same sample. A constant (or any integer) texel offset
 * is a shift of the texel coordinate, and pixel coordinates are texel coordinates, so the offset can be added to the coordinate (exact in Vulkan's addressing: the offset is
 * added to the integer texel coordinates, floor(u + k) = floor(u) + k, the filter weights are unchanged).
 *
 * The relationship that decides all of this is the OpSampledImage: one sample instruction belongs to ONE (image, sampler) pair. This module follows
 * sample -> OpSampledImage -> OpLoad -> OpVariable (-> Binding) and rewrites only the samples whose sampler variable is one of the unnormalized bindings of THIS draw.
 * Everything else (normalized samplers, fetch / read, other bindings) stays byte for byte as translated.
 */
#include "air2spv/spv_reflect.h"
#include "common/le.h"

#include <stdlib.h>
#include <string.h>

#define SPV_MAGIC_ 0x07230203u
enum {
    O_EXTINST = 12, O_TYPE_IMAGE = 25, O_TYPE_SAMPLER = 26, O_TYPE_SAMPLED_IMAGE = 27, O_TYPE_POINTER = 32, O_TYPE_FLOAT = 22, O_TYPE_VECTOR = 23, O_CONSTANT = 43,
    O_FUNCTION = 54, O_VARIABLE = 59, O_LOAD = 61, O_ACCESS_CHAIN = 65, O_IN_BOUNDS_ACCESS_CHAIN = 66, O_DECORATE = 71, O_SAMPLED_IMAGE = 86,
    O_SAMPLE_IMPLICIT = 87, O_SAMPLE_EXPLICIT = 88, O_SAMPLE_DREF_IMPLICIT = 89, O_SAMPLE_PROJ_DREF_EXPLICIT = 94, O_IMAGE_GATHER = 96, O_IMAGE_DREF_GATHER = 97,
    O_IMAGE_QUERY_LOD = 105, O_CONVERT_S_TO_F = 111, O_FADD = 129, O_SPARSE_FIRST = 305, O_SPARSE_LAST = 316
};
enum { IM_BIAS = 0x1, IM_LOD = 0x2, IM_GRAD = 0x4, IM_CONSTOFF = 0x8, IM_OFFSET = 0x10, IM_CONSTOFFS = 0x20, IM_SAMPLE = 0x40, IM_MINLOD = 0x80 };

/* Opcodes whose words 1 / 2 are (result type, result id): used only to learn the type of a coordinate. */
static int has_ty_res(uint32_t op) {
    if (op == 1u || op == O_EXTINST || (op >= 43u && op <= 52u) || op == 54u || op == 55u || op == 57u || op == 59u || op == 61u || (op >= 65u && op <= 70u)) return 1;
    if (op >= 77u && op <= 98u) return 1;
    if (op >= 100u && op <= 245u) return !(op == 218u || op == 219u || op == 220u || op == 221u || op == 224u || op == 225u || op == 228u);
    return op >= 305u && op <= 320u;
}

typedef struct { uint32_t w1, w2, w3, w4, w5, w6, w7; } img_t;   /* OpTypeImage operands */

typedef struct {
    uint32_t bound;
    uint32_t *res_type;      /* id -> result type id (0 = unknown) */
    uint32_t *bind;          /* id -> Binding + 1 (0 = none) */
    uint32_t *ld_ptr;        /* OpLoad result id -> pointer id */
    uint32_t *ac_base;       /* OpAccessChain result id -> base id */
    uint32_t *simg_img;      /* OpSampledImage result id -> image id */
    uint32_t *simg_smp;      /* OpSampledImage result id -> sampler id */
    uint8_t  *is_var;
    img_t    *img;           /* type id -> OpTypeImage operands (w3 = dim) ; valid when img_ok */
    uint8_t  *img_ok;
    uint32_t *simg_t_img;    /* OpTypeSampledImage id -> image type id */
    uint32_t *vec_comp, *vec_n;
    uint8_t  *is_f32;
} tab_t;

static void tab_free(tab_t *t) {
    free(t->res_type); free(t->bind); free(t->ld_ptr); free(t->ac_base); free(t->simg_img); free(t->simg_smp); free(t->is_var);
    free(t->img); free(t->img_ok); free(t->simg_t_img); free(t->vec_comp); free(t->vec_n); free(t->is_f32);
    memset(t, 0, sizeof *t);
}

static int tab_alloc(tab_t *t, uint32_t bound) {
    memset(t, 0, sizeof *t);
    t->bound = bound;
    t->res_type = calloc(bound, 4); t->bind = calloc(bound, 4); t->ld_ptr = calloc(bound, 4); t->ac_base = calloc(bound, 4); t->simg_img = calloc(bound, 4);
    t->simg_smp = calloc(bound, 4); t->is_var = calloc(bound, 1); t->img = calloc(bound, sizeof(img_t)); t->img_ok = calloc(bound, 1);
    t->simg_t_img = calloc(bound, 4); t->vec_comp = calloc(bound, 4); t->vec_n = calloc(bound, 4); t->is_f32 = calloc(bound, 1);
    if (!t->res_type || !t->bind || !t->ld_ptr || !t->ac_base || !t->simg_img || !t->simg_smp || !t->is_var || !t->img || !t->img_ok || !t->simg_t_img || !t->vec_comp || !t->vec_n || !t->is_f32) { tab_free(t); return 0; }
    return 1;
}

/* The binding number of the sampler variable behind a sampler value id, or -1 when it cannot be traced (function parameter, phi, select, ...). */
static int64_t sampler_binding(const tab_t *t, uint32_t smp) {
    if (smp == 0u || smp >= t->bound) return -1;
    uint32_t p = t->ld_ptr[smp];
    if (!p) return -1;
    for (int hop = 0; hop < 4 && p < t->bound && !t->is_var[p]; hop++) p = t->ac_base[p];   /* access chain into a binding array */
    if (p == 0u || p >= t->bound || !t->is_var[p] || !t->bind[p]) return -1;
    return (int64_t)t->bind[p] - 1;
}

static uint32_t imgop_ids(uint32_t mask) {   /* number of id operands that follow the mask, -1 as UINT32_MAX when unknown bits are set */
    if (mask & ~0xffu) return 0xffffffffu;
    return (uint32_t)((mask & IM_BIAS) ? 1 : 0) + ((mask & IM_LOD) ? 1 : 0) + ((mask & IM_GRAD) ? 2 : 0) + ((mask & IM_CONSTOFF) ? 1 : 0) + ((mask & IM_OFFSET) ? 1 : 0) +
           ((mask & IM_CONSTOFFS) ? 1 : 0) + ((mask & IM_SAMPLE) ? 1 : 0) + ((mask & IM_MINLOD) ? 1 : 0);
}

int lagfx_spv_unnorm_rewrite(const uint8_t *spv, size_t spv_len, const uint32_t *bindings, uint32_t nb, int convert_unresolved,
                             uint8_t **out, size_t *out_len, lagfx_spv_unnorm_report_t *rep) {
    lagfx_spv_unnorm_report_t R; memset(&R, 0, sizeof R);
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (rep) *rep = R;
    if (!spv || spv_len < 20u || (spv_len & 3u) || lagfx_le32(spv) != SPV_MAGIC_) return -1;
    size_t nwords = spv_len / 4u;
    uint32_t bound = lagfx_le32(spv + 12u);
    if (bound == 0u || bound > (16u << 20)) return -1;
    if (!nb || !bindings) return 0;

    tab_t T;
    if (!tab_alloc(&T, bound)) return -1;
    size_t first_fn = 0; uint32_t f32 = 0, zero_c = 0;
    /* ---- pass 1: tables ---- */
    for (size_t i = 5u; i < nwords;) {
        uint32_t h = lagfx_le32(spv + i * 4u), wc = h >> 16, op = h & 0xFFFFu;
#define W(k) lagfx_le32(spv + (i + (k)) * 4u)
        if (wc == 0u || i + wc > nwords) { tab_free(&T); return -1; }
        if (has_ty_res(op) && wc >= 3u && W(2) < bound) T.res_type[W(2)] = W(1);
        switch (op) {
        case O_TYPE_FLOAT: if (wc >= 3u && W(1) < bound && W(2) == 32u) { T.is_f32[W(1)] = 1; if (!f32) f32 = W(1); } break;
        case O_TYPE_VECTOR: if (wc >= 4u && W(1) < bound) T.vec_comp[W(1)] = W(2), T.vec_n[W(1)] = W(3); break;
        case O_CONSTANT: if (wc == 4u && f32 && W(1) == f32 && W(3) == 0u && !zero_c) zero_c = W(2); break;
        case O_TYPE_IMAGE: if (wc >= 8u && W(1) < bound) { T.img[W(1)] = (img_t){ W(1), W(2), W(3), W(4), W(5), W(6), W(7) }; T.img_ok[W(1)] = 1; } break;
        case O_TYPE_SAMPLED_IMAGE: if (wc >= 3u && W(1) < bound) T.simg_t_img[W(1)] = W(2); break;
        case O_FUNCTION: if (!first_fn) first_fn = i; break;
        case O_VARIABLE: if (wc >= 4u && W(2) < bound) T.is_var[W(2)] = 1; break;
        case O_LOAD: if (wc >= 4u && W(2) < bound) T.ld_ptr[W(2)] = W(3); break;
        case O_ACCESS_CHAIN: case O_IN_BOUNDS_ACCESS_CHAIN: if (wc >= 4u && W(2) < bound) T.ac_base[W(2)] = W(3); break;
        case O_SAMPLED_IMAGE: if (wc >= 5u && W(2) < bound) T.simg_img[W(2)] = W(3), T.simg_smp[W(2)] = W(4); break;
        case O_DECORATE: if (wc >= 4u && W(1) < bound && W(2) == 33u) T.bind[W(1)] = W(3) + 1u; break;   /* Binding */
        default: break;
        }
        i += wc;
    }
    if (!first_fn) { tab_free(&T); return 0; }

    /* ---- pass 2: classify every sample, size the output ---- */
    size_t extra = 0; uint32_t n_conv = 0;
    int unsupported = 0;
    for (size_t i = 5u; i < nwords;) {
        uint32_t h = lagfx_le32(spv + i * 4u), wc = h >> 16, op = h & 0xFFFFu;
        int is_sample = (op >= O_SAMPLE_IMPLICIT && op <= O_SAMPLE_PROJ_DREF_EXPLICIT), is_gather = op == O_IMAGE_GATHER || op == O_IMAGE_DREF_GATHER,
            is_sparse = op >= O_SPARSE_FIRST && op <= O_SPARSE_LAST, is_qlod = op == O_IMAGE_QUERY_LOD;
        if ((is_sample || is_gather || is_sparse || is_qlod) && wc >= 5u) {
            R.samples_total++;
            uint32_t simg = W(3);
            int64_t b = -1; int resolved = 0;
            if (is_qlod) simg = W(3);
            if (simg < bound && T.simg_smp[simg]) { b = sampler_binding(&T, T.simg_smp[simg]); resolved = b >= 0; }
            int hit = 0;
            if (resolved) { for (uint32_t k = 0; k < nb; k++) if ((int64_t)bindings[k] == b) hit = 1; }
            else { R.uses_unresolved++; hit = convert_unresolved && (op == O_SAMPLE_IMPLICIT || op == O_SAMPLE_EXPLICIT); }
            if (hit) {
                if (resolved) R.uses_unnorm++;
                /* the image type behind the sampled image */
                uint32_t sty = simg < bound ? T.res_type[simg] : 0u, ity = sty && sty < bound ? T.simg_t_img[sty] : 0u;
                int img_known = ity && ity < bound && T.img_ok[ity];
                /* Dim: 0 = 1D, 1 = 2D, 2 = 3D, 3 = Cube, 4 = Rect, 5 = Buffer; Vulkan: only 1D / 2D views, not arrayed, and (here) not multisampled. An image type that cannot be found is not judged. */
                int dim_ok = !img_known || ((T.img[ity].w3 == 0u || T.img[ity].w3 == 1u) && T.img[ity].w5 == 0u && T.img[ity].w6 == 0u);
                if (is_sparse) { R.unsup_sparse++; unsupported = 1; }
                else if (is_qlod) { R.unsup_other++; unsupported = 1; }
                else if (!dim_ok) { R.unsup_image_dim++; unsupported = 1; }
                else if (is_gather) { R.gather_kept++; }
                else if (op >= O_SAMPLE_DREF_IMPLICIT && op <= O_SAMPLE_PROJ_DREF_EXPLICIT) {
                    if (op == 89u || op == 90u) R.unsup_dref++; else R.unsup_proj++;
                    unsupported = 1;
                } else {   /* 87 / 88 */
                    uint32_t mask = wc > 5u ? W(5) : 0u, nid = imgop_ids(mask);
                    if (nid == 0xffffffffu || wc != (wc > 5u ? 6u : 5u) + nid) { R.unsup_other++; unsupported = 1; }
                    else if (mask & (IM_CONSTOFFS | IM_SAMPLE)) { R.unsup_other++; unsupported = 1; }
                    else {
                        uint32_t c = W(4), cty = c < bound ? T.res_type[c] : 0u;
                        int need_off = (mask & (IM_CONSTOFF | IM_OFFSET)) != 0;
                        int coord_ok = !need_off || (cty && cty < bound && (T.is_f32[cty] || (T.vec_comp[cty] && T.vec_comp[cty] < bound && T.is_f32[T.vec_comp[cty]])));
                        if (!coord_ok) { R.unsup_other++; unsupported = 1; }
                        else {
                            if (op == O_SAMPLE_IMPLICIT) { R.implicit_to_lod0++; if (mask & IM_BIAS) R.bias_dropped++; if (mask & IM_MINLOD) R.minlod_dropped++; }
                            else { R.explicit_kept++; if (mask & IM_GRAD) R.grad_kept++; }
                            if (need_off) R.offset_folded++;
                            if (op == O_SAMPLE_IMPLICIT || need_off) {   /* an ExplicitLod without offset is already valid: left as it is */
                                n_conv++;
                                extra += 12u;   /* worst case: ConvertSToF (4) + FAdd (5) + a longer sample (<= 2) */
                            }
                        }
                    }
                }
            }
        }
        i += wc;
    }
    if (unsupported) { if (rep) *rep = R; tab_free(&T); return -1; }
    if (!n_conv) { if (rep) *rep = R; tab_free(&T); return 0; }
    if (!out) { if (rep) *rep = R; tab_free(&T); return 1; }   /* analysis only */
    if (!f32) { R.unsup_other++; if (rep) *rep = R; tab_free(&T); return -1; }

    /* ---- pass 3: emit ---- */
    uint32_t next = bound, zid = zero_c ? zero_c : next++;
    size_t cap = nwords + extra + 8u;
    uint32_t *o = malloc(cap * 4u);
    if (!o) { tab_free(&T); return -1; }
    size_t w = 0;
#define EMIT(v) do { o[w++] = (v); } while (0)
    for (size_t i = 0; i < 5u; i++) o[w++] = lagfx_le32(spv + i * 4u);
    for (size_t i = 5u; i < nwords;) {
        uint32_t h = lagfx_le32(spv + i * 4u), wc = h >> 16, op = h & 0xFFFFu;
        if (i == first_fn && !zero_c) { EMIT(4u << 16 | O_CONSTANT); EMIT(f32); EMIT(zid); EMIT(0u); }
        int conv = 0;
        if ((op == O_SAMPLE_IMPLICIT || op == O_SAMPLE_EXPLICIT) && wc >= 5u) {
            uint32_t simg = W(3);
            if (simg < bound && T.simg_smp[simg]) {
                int64_t b = sampler_binding(&T, T.simg_smp[simg]);
                if (b >= 0) { for (uint32_t k = 0; k < nb; k++) if ((int64_t)bindings[k] == b) conv = 1; }
                else conv = convert_unresolved;
            } else conv = convert_unresolved;
        }
        if (conv && op == O_SAMPLE_EXPLICIT && !((wc > 5u ? W(5) : 0u) & (IM_CONSTOFF | IM_OFFSET))) conv = 0;
        if (!conv) { for (uint32_t k = 0; k < wc; k++) EMIT(lagfx_le32(spv + (i + k) * 4u)); i += wc; continue; }
        uint32_t mask = wc > 5u ? W(5) : 0u, c = W(4), cty = T.res_type[c < bound ? c : 0u];
        /* operand ids by bit */
        uint32_t id_bias = 0, id_lod = 0, id_g0 = 0, id_g1 = 0, id_coff = 0, id_off = 0, id_minlod = 0, p = 6u;
        if (mask & IM_BIAS) id_bias = W(p++);
        if (mask & IM_LOD) id_lod = W(p++);
        if (mask & IM_GRAD) id_g0 = W(p++), id_g1 = W(p++);
        if (mask & IM_CONSTOFF) id_coff = W(p++);
        if (mask & IM_OFFSET) id_off = W(p++);
        if (mask & IM_MINLOD) id_minlod = W(p++);
        (void)id_bias; (void)id_minlod;
        uint32_t offv = id_coff ? id_coff : id_off;
        if (offv) {
            uint32_t cv = next++, c2 = next++;
            EMIT(4u << 16 | O_CONVERT_S_TO_F); EMIT(cty); EMIT(cv); EMIT(offv);
            EMIT(5u << 16 | O_FADD); EMIT(cty); EMIT(c2); EMIT(c); EMIT(cv);
            c = c2;
        }
        uint32_t nm, nops[6]; uint32_t nn = 0;
        if (op == O_SAMPLE_IMPLICIT) { nm = IM_LOD; nops[nn++] = zid; }
        else {
            nm = mask & ~(uint32_t)(IM_CONSTOFF | IM_OFFSET);
            if (nm & IM_BIAS) nops[nn++] = id_bias;   /* cannot occur with ExplicitLod, kept for completeness */
            if (nm & IM_LOD) nops[nn++] = id_lod;
            if (nm & IM_GRAD) nops[nn++] = id_g0, nops[nn++] = id_g1;
            if (nm & IM_MINLOD) nops[nn++] = id_minlod;
        }
        uint32_t nwc = 5u + (nm ? 1u + nn : 0u);
        EMIT(nwc << 16 | O_SAMPLE_EXPLICIT); EMIT(W(1)); EMIT(W(2)); EMIT(W(3)); EMIT(c);
        if (nm) { EMIT(nm); for (uint32_t k = 0; k < nn; k++) EMIT(nops[k]); }
        i += wc;
    }
#undef EMIT
    o[3] = next;
    R.module_changed = 1;
    if (rep) *rep = R;
    tab_free(&T);
    *out = (uint8_t *)o;
    {   /* the words were written in host order: store little endian */
        for (size_t k = 0; k < w; k++) lagfx_put_le32((uint8_t *)o + k * 4u, o[k]);
    }
    *out_len = w * 4u;
    return 1;
#undef W
}
