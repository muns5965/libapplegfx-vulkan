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
 * An existing 32-bit int type, Input pointer type, SampleId variable and SampleRateShading capability are reused (no duplicate declaration); the variable joins only FRAGMENT entry points.
 * The output size is computed exactly from what is inserted (see `extra`). A module without a SubpassData image is not touched (0). A SubpassData read of any other form (image operands already present) is not rewritten: -1, the caller refuses the draw.
 */
#include "air2spv/spv_reflect.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { O_CAPABILITY = 17, O_ENTRY_POINT = 15, O_TYPE_INT = 21, O_TYPE_IMAGE = 25, O_TYPE_POINTER = 32, O_FUNCTION = 54, O_VARIABLE = 59, O_LOAD = 61, O_DECORATE = 71, O_IMAGE_READ = 98 };
#define SPV_MAGIC 0x07230203u

#define FREE3 do { free(w); free(is_sub); free(ld_sub); free(ptr_to); } while (0)

/* does the interface list of the OpEntryPoint at w[i..i+wc) already name `id`? (the name literal is skipped first: it ends in the word holding a zero byte) */
static int ep_has_iface(const uint32_t *w, size_t i, uint32_t wc, uint32_t id) {
    size_t j = i + 3;
    while (j < i + wc) { const uint32_t x = w[j++]; if (!(x & 0xFFu) || !(x & 0xFF00u) || !(x & 0xFF0000u) || !(x & 0xFF000000u)) break; }
    for (; j < i + wc; j++) if (w[j] == id) return 1;
    return 0;
}

/* TEST SEAM (negative control): when set, every read names the constant sample 0 instead of the invocation's SampleId (what a broken rewrite would do); tests assert that they then FAIL. */
int lagfx_spv_msfetch_test_force_sample0;

int lagfx_spv_msfetch_rewrite(const uint8_t *spv, size_t spv_len, uint8_t **out, size_t *out_len, uint32_t *nreads) {
    if (nreads) *nreads = 0;
    if (!spv || spv_len < 20u || (spv_len & 3u) || spv_len > (SIZE_MAX / 8u)) return -1;
    const size_t nw = spv_len / 4u;
    uint32_t *w = malloc(spv_len);
    if (!w) return -1;
    memcpy(w, spv, spv_len);
    if (w[0] != SPV_MAGIC) { free(w); return -1; }
    const uint32_t bound = w[3];
    if (bound == 0u || bound > 0x3FFFFFFFu) { free(w); return -1; }
    uint8_t *is_sub = calloc((size_t)bound + 1u, 1), *ld_sub = calloc((size_t)bound + 1u, 1);   /* image type ids with Dim = SubpassData; OpLoad results of such an image */
    uint32_t *ptr_to = calloc((size_t)bound + 1u, 4);   /* OpTypePointer result id -> pointee (Input storage only) */
    if (!is_sub || !ld_sub || !ptr_to) { FREE3; return -1; }
    size_t i = 5, first_ann = 0, first_fn = 0, last_cap = 0;
    uint32_t i32_ty = 0, ptr_ty = 0, nsub = 0, nfrag = 0, exist_var = 0, exist_var_ty = 0;
    int has_srs = 0;
    for (; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || i + wc > nw) { FREE3; return -1; }
        if (op == O_CAPABILITY && wc == 2) { last_cap = i + wc; if (w[i + 1] == 35u) has_srs = 1; }
        if (op == O_DECORATE && !first_ann) first_ann = i;
        if (op == O_DECORATE && wc == 4 && w[i + 2] == 11u && w[i + 3] == 18u) exist_var = w[i + 1];   /* BuiltIn SampleId already declared */
        if (op == O_TYPE_INT && wc == 4 && w[i + 2] == 32u && !i32_ty && w[i + 1] <= bound) i32_ty = w[i + 1];   /* any 32-bit int: SampleId is a 32-bit integer */
        if (op == O_TYPE_POINTER && wc == 4 && w[i + 2] == 1u /* Input */ && w[i + 1] <= bound) ptr_to[w[i + 1]] = w[i + 3];
        if (op == O_VARIABLE && wc >= 4 && exist_var && w[i + 2] == exist_var && w[i + 1] <= bound) exist_var_ty = w[i + 1];
        if (op == O_TYPE_IMAGE && wc >= 9 && w[i + 3] == 6u /* SubpassData */ && w[i + 1] <= bound) { is_sub[w[i + 1]] = 1; nsub++; }
        if (op == O_ENTRY_POINT && wc >= 4 && w[i + 1] == 4u /* Fragment */) nfrag++;
        if (op == O_FUNCTION) { first_fn = i; break; }
        i += wc;
    }
    if (!nsub) { FREE3; return 0; }
    if (!first_fn || !nfrag) { FREE3; return -1; }
    uint32_t ty_int_exist = 0;
    if (exist_var) {   /* reuse the module's own SampleId variable: its pointee must be a 32-bit int */
        ty_int_exist = exist_var_ty && exist_var_ty <= bound ? ptr_to[exist_var_ty] : 0;
        if (!ty_int_exist) { FREE3; return -1; }
    }
    for (uint32_t t = 1; t <= bound && i32_ty && !ptr_ty && !exist_var; t++) if (ptr_to[t] == i32_ty) ptr_ty = t;   /* an existing Input pointer to the int type */
    /* pass 1 over the functions: OpLoad of a SubpassData image, and the reads that use them */
    uint32_t reads = 0;
    for (i = first_fn; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!wc || i + wc > nw) { FREE3; return -1; }
        if (op == O_LOAD && wc >= 4 && w[i + 1] <= bound && is_sub[w[i + 1]] && w[i + 2] <= bound) ld_sub[w[i + 2]] = 1;
        if (op == O_IMAGE_READ && wc >= 5 && w[i + 3] <= bound && w[i + 4] <= bound && ld_sub[w[i + 3]]) {
            if (wc != 5) { FREE3; return -1; }   /* image operands already there: not a form the translator emits */
            reads++;
        }
        i += wc;
    }
    if (!reads) { FREE3; return 0; }
    /* new ids: [int type if absent] [ptr type if absent] [var if absent] + one id per read (the loaded sample id) */
    uint32_t nid = bound;
    const int new_int = !i32_ty && !exist_var, new_ptr = !ptr_ty && !exist_var, new_var = !exist_var;
    const uint32_t ty_int = exist_var ? ty_int_exist : i32_ty ? i32_ty : (nid++, nid - 1u);
    const uint32_t ty_ptr = ptr_ty ? ptr_ty : new_ptr ? nid++ : 0u;
    const uint32_t var = exist_var ? exist_var : nid++;
    const int f0 = lagfx_spv_msfetch_test_force_sample0 != 0;
    const uint32_t c0 = f0 ? nid++ : 0u;
    const uint32_t first_sid = nid;
    if (reads > 0x3FFFFFFFu - nid) { FREE3; return -1; }
    nid += reads;
    /* EXACT growth: capability 2 + decorations 7 + int type 4 + pointer type 4 + variable 4 (each only when new) + one interface word per fragment entry point + 6 per read (OpLoad 4 + OpImageRead 7 - 5) */
    const size_t extra = (has_srs ? 0u : 2u) + (new_var ? 7u : 0u) + (new_int ? 4u : 0u) + (new_ptr ? 4u : 0u) + (new_var ? 4u : 0u) + (size_t)nfrag + 6u * (size_t)reads + (f0 ? 4u : 0u);
    const size_t cap = nw + extra;
    uint32_t *o = malloc(cap * 4u);
    if (!o) { FREE3; return -1; }
    size_t n = 0;
    for (size_t k = 0; k < 5; k++) o[n++] = w[k];
    o[3] = nid;   /* the new bound */
    uint32_t sid = first_sid;
    for (i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (i == last_cap && !has_srs) o[n++] = (2u << 16) | O_CAPABILITY, o[n++] = 35u;   /* SampleRateShading, after the last existing capability */
        if (new_var && first_ann && i == first_ann) {   /* annotations: BuiltIn SampleId + Flat on the new variable (first decoration instruction) */
            o[n++] = (4u << 16) | O_DECORATE, o[n++] = var, o[n++] = 11u /* BuiltIn */, o[n++] = 18u /* SampleId */;
            o[n++] = (3u << 16) | O_DECORATE, o[n++] = var, o[n++] = 14u /* Flat */;
        }
        if (i == first_fn && new_var) {   /* global declarations before the first function */
            if (new_int) o[n++] = (4u << 16) | O_TYPE_INT, o[n++] = ty_int, o[n++] = 32u, o[n++] = 1u;
            if (new_ptr) o[n++] = (4u << 16) | O_TYPE_POINTER, o[n++] = ty_ptr, o[n++] = 1u /* Input */, o[n++] = ty_int;
            o[n++] = (4u << 16) | O_VARIABLE, o[n++] = ty_ptr, o[n++] = var, o[n++] = 1u /* Input */;
        }
        if (i == first_fn && f0) o[n++] = (4u << 16) | 43u /* OpConstant */, o[n++] = ty_int, o[n++] = c0, o[n++] = 0u;   /* test seam: the constant sample 0, after the int type it uses */
        if (op == O_ENTRY_POINT && wc >= 4 && w[i + 1] == 4u && !ep_has_iface(w, i, wc, var)) {   /* the Input variable joins the interface of the FRAGMENT entry points (required from SPIR-V 1.4 on, allowed before) */
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
        if (op == O_IMAGE_READ && wc == 5 && w[i + 3] <= bound && w[i + 4] <= bound && ld_sub[w[i + 3]]) {   /* the SAME predicate as the counting pass: the buffer holds exactly what that pass counted */
            const uint32_t id = sid++;
            o[n++] = (4u << 16) | O_LOAD, o[n++] = ty_int, o[n++] = id, o[n++] = var;   /* %id = OpLoad int %SampleId */
            o[n++] = (7u << 16) | O_IMAGE_READ, o[n++] = w[i + 1], o[n++] = w[i + 2], o[n++] = w[i + 3], o[n++] = w[i + 4], o[n++] = 0x40u /* Sample */, o[n++] = f0 ? c0 : id;
            i += wc; continue;
        }
        memcpy(&o[n], &w[i], wc * 4u); n += wc; i += wc;
    }
    FREE3;
    if (n > cap || (new_var && !first_ann)) { free(o); return -1; }   /* n > cap cannot happen (exact bound; the ASan test pins it); no annotation section to hold the decoration: the translator always emits one for a fetch module */
    *out = (uint8_t *)o; *out_len = n * 4u;
    if (nreads) *nreads = reads;
    return 1;
}
