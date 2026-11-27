/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — framebuffer fetch (input attachment) in a MULTISAMPLED pass
 * src/air2spv/spv_msfetch.c
 *
 * The translator declares a framebuffer-fetch input as a single-sampled SubpassData image read with a plain OpImageRead. Vulkan requires the shader's image type to be multisampled
 * when the attachment is, and the read to name the sample. Metal's fetch in a multisampled pass reads the SAMPLE the fragment invocation is shading, so:
 *   - every OpTypeImage with Dim = SubpassData gets MS = 1,
 *   - every plain OpImageRead of such an image gets the Sample image operand = the invocation's SampleId (a new BuiltIn SampleId Input variable, Flat; reading it makes the
 *     fragment shader run per sample, which is exactly Metal's per-sample shading for a shader that fetches),
 *   - OpCapability SampleRateShading, and the new variable joins the entry point's interface.
 * A module without a SubpassData image is not touched (0). A SubpassData read of any other form (image operands already present) is not rewritten: -1, the caller refuses the draw.
 */
#include "air2spv/spv_reflect.h"

#include <stdlib.h>
#include <string.h>

enum { O_CAPABILITY = 17, O_ENTRY_POINT = 15, O_TYPE_INT = 21, O_TYPE_IMAGE = 25, O_TYPE_POINTER = 32, O_FUNCTION = 54, O_VARIABLE = 59, O_LOAD = 61, O_DECORATE = 71, O_IMAGE_READ = 98 };
#define SPV_MAGIC 0x07230203u

int lagfx_spv_msfetch_rewrite(const uint8_t *spv, size_t spv_len, uint8_t **out, size_t *out_len, uint32_t *nreads) {
    if (nreads) *nreads = 0;
    if (!spv || spv_len < 20u || (spv_len & 3u)) return -1;
    const size_t nw = spv_len / 4u;
    uint32_t *w = malloc(spv_len);
    if (!w) return -1;
    memcpy(w, spv, spv_len);
    if (w[0] != SPV_MAGIC) { free(w); return -1; }
    const uint32_t bound = w[3];
    uint8_t *is_sub = calloc(bound + 1u, 1), *ld_sub = calloc(bound + 1u, 1);   /* image type ids with Dim = SubpassData; OpLoad results of such an image */
    if (!is_sub || !ld_sub) { free(w); free(is_sub); free(ld_sub); return -1; }
    size_t i = 5, first_ann = 0, first_fn = 0, last_cap = 0;
    uint32_t i32_ty = 0, nsub = 0;
    for (; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || i + wc > nw) { free(w); free(is_sub); free(ld_sub); return -1; }
        if (op == O_CAPABILITY) last_cap = i + wc;
        if (op == O_DECORATE && !first_ann) first_ann = i;
        if (op == O_TYPE_INT && wc == 4 && w[i + 2] == 32u && !i32_ty) i32_ty = w[i + 1];   /* any 32-bit int: SampleId is a 32-bit integer */
        if (op == O_TYPE_IMAGE && wc >= 9 && w[i + 3] == 6u /* SubpassData */ && w[i + 1] <= bound) { is_sub[w[i + 1]] = 1; nsub++; }
        if (op == O_FUNCTION) { first_fn = i; break; }
        i += wc;
    }
    if (!nsub) { free(w); free(is_sub); free(ld_sub); return 0; }
    if (!first_fn) { free(w); free(is_sub); free(ld_sub); return -1; }
    /* pass 1 over the functions: OpLoad of a SubpassData image, and the reads that use them */
    uint32_t reads = 0;
    for (i = first_fn; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || i + wc > nw) { free(w); free(is_sub); free(ld_sub); return -1; }
        if (op == O_LOAD && wc >= 4 && w[i + 1] <= bound && is_sub[w[i + 1]] && w[i + 2] <= bound) ld_sub[w[i + 2]] = 1;
        if (op == O_IMAGE_READ && wc >= 5 && w[i + 4] <= bound && ld_sub[w[i + 3]]) {
            if (wc != 5) { free(w); free(is_sub); free(ld_sub); return -1; }   /* image operands already there: not a form the translator emits */
            reads++;
        }
        i += wc;
    }
    if (!reads) { free(w); free(is_sub); free(ld_sub); return 0; }
    /* new ids: [int type if absent] [ptr type] [var] + one id per read (the loaded sample id) */
    uint32_t nid = bound;
    const uint32_t ty_int = i32_ty ? i32_ty : nid++, ty_ptr = nid++, var = nid++;
    const uint32_t first_sid = nid; nid += reads;
    size_t cap = nw + 16u + 3u * (size_t)reads + 32u;
    uint32_t *o = malloc(cap * 4u);
    if (!o) { free(w); free(is_sub); free(ld_sub); return -1; }
    size_t n = 0;
    for (size_t k = 0; k < 5; k++) o[n++] = w[k];
    o[3] = nid;   /* the new bound */
    uint32_t sid = first_sid;
    for (i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (i == last_cap) o[n++] = (2u << 16) | O_CAPABILITY, o[n++] = 35u;   /* SampleRateShading, after the last existing capability */
        if (first_ann && i == first_ann) {   /* annotations: BuiltIn SampleId + Flat on the new variable (first decoration instruction) */
            o[n++] = (4u << 16) | O_DECORATE, o[n++] = var, o[n++] = 11u /* BuiltIn */, o[n++] = 18u /* SampleId */;
            o[n++] = (3u << 16) | O_DECORATE, o[n++] = var, o[n++] = 14u /* Flat */;
        }
        if (i == first_fn) {   /* global declarations before the first function */
            if (!i32_ty) o[n++] = (4u << 16) | O_TYPE_INT, o[n++] = ty_int, o[n++] = 32u, o[n++] = 1u;
            o[n++] = (4u << 16) | O_TYPE_POINTER, o[n++] = ty_ptr, o[n++] = 1u /* Input */, o[n++] = ty_int;
            o[n++] = (4u << 16) | O_VARIABLE, o[n++] = ty_ptr, o[n++] = var, o[n++] = 1u /* Input */;
        }
        if (op == O_ENTRY_POINT) {   /* the new Input variable joins the interface (required from SPIR-V 1.4 on, allowed before) */
            o[n] = ((wc + 1u) << 16) | O_ENTRY_POINT;
            memcpy(&o[n + 1], &w[i + 1], (wc - 1u) * 4u);
            o[n + wc] = var;
            n += wc + 1u;
            i += wc;
            continue;
        }
        if (op == O_TYPE_IMAGE && wc >= 9 && w[i + 3] == 6u && w[i + 6] == 0u) {   /* MS = 1 */
            memcpy(&o[n], &w[i], wc * 4u); o[n + 6] = 1u; n += wc; i += wc; continue;
        }
        if (op == O_IMAGE_READ && wc == 5 && w[i + 3] <= bound && ld_sub[w[i + 3]]) {
            const uint32_t id = sid++;
            o[n++] = (4u << 16) | O_LOAD, o[n++] = ty_int, o[n++] = id, o[n++] = var;   /* %id = OpLoad int %SampleId */
            o[n++] = (7u << 16) | O_IMAGE_READ, o[n++] = w[i + 1], o[n++] = w[i + 2], o[n++] = w[i + 3], o[n++] = w[i + 4], o[n++] = 0x40u /* Sample */, o[n++] = id;
            i += wc; continue;
        }
        memcpy(&o[n], &w[i], wc * 4u); n += wc; i += wc;
    }
    free(w); free(is_sub); free(ld_sub);
    if (!first_ann) { free(o); return -1; }   /* no annotation section to put the decoration in: the translator always emits one for a fetch module */
    *out = (uint8_t *)o; *out_len = n * 4u;
    if (nreads) *nreads = reads;
    return 1;
}
