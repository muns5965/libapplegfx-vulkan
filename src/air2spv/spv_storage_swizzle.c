/* SPDX-License-Identifier: MIT */
#include "spv_storage_swizzle.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_NAME = 5, OP_ENTRY_POINT = 15, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_INT = 21, OP_TYPE_IMAGE = 25, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_LOAD = 61, OP_IMAGE_READ = 98, OP_IMAGE_WRITE = 99,
       OP_VECTOR_SHUFFLE = 79, OP_DECORATE = 71, OP_DEC_SET = 34, OP_DEC_BINDING = 33 };

static void fail(char *why, size_t cap, const char *m) { if (why && cap) snprintf(why, cap, "%s", m); }

int lagfx_spv_swizzle_storage_rb(const uint8_t *in, size_t in_len, const uint32_t (*pairs)[2], size_t n, uint8_t **out, size_t *out_len, char *why, size_t why_cap) {
    if (why && why_cap) why[0] = 0;
    if (!in || in_len < 20u || (in_len & 3u) || !out || !out_len || !pairs || !n) return -1;
    const uint32_t *w = (const uint32_t *)(const void *)in; const size_t nw = in_len / 4u;
    if (w[0] != 0x07230203u) return -1;
    const uint32_t bound = w[3]; if (!bound || bound > (1u << 24)) return -1;
    uint32_t *set = calloc(bound, 4), *bind = calloc(bound, 4); uint8_t *hasset = calloc(bound, 1), *hasbind = calloc(bound, 1), *sel = calloc(bound, 1), *selvar = calloc(bound, 1);
    uint32_t *ptr_pointee = calloc(bound, 4), *var_type = calloc(bound, 4), *img_sampled = calloc(bound, 4), *img_is_storage = calloc(bound, 4), *vec_comp = calloc(bound, 4), *vec_n = calloc(bound, 4), *load_var = calloc(bound, 4);
    int rc = -1; uint32_t *o = NULL; size_t on = 0, ocap = 0;
    if (!set || !bind || !hasset || !hasbind || !sel || !selvar || !ptr_pointee || !var_type || !img_sampled || !img_is_storage || !vec_comp || !vec_n || !load_var) goto done;
    /* pass 1: decorations, types, variables */
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc || i + wc > nw) goto done; const uint32_t *a = &w[i + 1];
        if (op == OP_DECORATE && wc >= 4 && a[0] < bound) { if (a[1] == OP_DEC_SET) { set[a[0]] = a[2]; hasset[a[0]] = 1; } else if (a[1] == OP_DEC_BINDING) { bind[a[0]] = a[2]; hasbind[a[0]] = 1; } }
        else if (op == OP_TYPE_IMAGE && wc >= 9 && a[0] < bound) { img_sampled[a[0]] = a[1]; img_is_storage[a[0]] = (a[6] == 2u) ? 1u : 0u; }
        else if (op == OP_TYPE_VECTOR && wc >= 4 && a[0] < bound) { vec_comp[a[0]] = a[1]; vec_n[a[0]] = a[2]; }
        else if (op == OP_TYPE_POINTER && wc >= 4 && a[0] < bound) ptr_pointee[a[0]] = a[2];
        else if (op == OP_VARIABLE && wc >= 4 && a[1] < bound) var_type[a[1]] = a[0];
        i += wc;
    }
    bool any = false;
    for (uint32_t v = 1; v < bound; v++) {
        if (!var_type[v] || !hasset[v] || !hasbind[v]) continue;
        uint32_t pt = var_type[v] < bound ? ptr_pointee[var_type[v]] : 0u; if (!pt || pt >= bound || !img_is_storage[pt]) continue;
        for (size_t k = 0; k < n; k++) if (pairs[k][0] == set[v] && pairs[k][1] == bind[v]) { selvar[v] = 1; any = true; }
    }
    if (!any) { rc = 0; goto done; }
    /* pass 2: loads of selected variables; every other use of a selected variable or of such a loaded image is refused */
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; const uint32_t *a = &w[i + 1];
        if (op == OP_LOAD && wc >= 4 && a[2] < bound && selvar[a[2]]) { sel[a[1]] = 1; load_var[a[1]] = a[2]; }
        i += wc;
    }
    bool in_fn = false;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; const uint32_t *a = &w[i + 1];
        if (op == 54u) in_fn = true;
        /* only instructions inside functions can use an image / variable; literal-carrying ones (constants, extract / insert / shuffle indices) are skipped so a literal equal to an id is no false hit */
        if (!in_fn || (op >= 43u && op <= 52u) || op == 79u || op == 81u || op == 82u) { i += wc; continue; }
        const bool deco = op == OP_DECORATE || op == OP_NAME || op == 72u /* MemberDecorate */ || op == 6u /* MemberName */ || op == OP_ENTRY_POINT || op == OP_VARIABLE;
        if (!deco) for (uint32_t k = 0; k < wc - 1u; k++) {
            uint32_t id = a[k]; if (id >= bound) continue;
            if (selvar[id] && !(op == OP_LOAD && k == 2u)) { fail(why, why_cap, "a selected storage image variable is used other than by a direct load"); goto done; }
            if (sel[id]) {
                const bool ok = (op == OP_LOAD && k == 1u) || ((op == OP_IMAGE_READ || op == OP_IMAGE_WRITE) && k == (op == OP_IMAGE_READ ? 2u : 0u));
                if (!ok) { fail(why, why_cap, "a selected storage image is used other than by OpImageRead / OpImageWrite (function argument, texel pointer, query)"); goto done; }
            }
        }
        i += wc;
    }
    /* pass 3: rewrite */
#define PUT(v) do { if (on == ocap) { size_t nc = ocap ? ocap * 2 : 8192; uint32_t *p = realloc(o, nc * 4); if (!p) goto done; o = p; ocap = nc; } o[on++] = (v); } while (0)
    uint32_t next = bound; int rewritten = 0;
    for (size_t i = 0; i < 5; i++) PUT(w[i]);
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; const uint32_t *a = &w[i + 1];
        if (op == OP_IMAGE_WRITE && wc >= 4 && a[0] < bound && sel[a[0]]) {
            uint32_t vi = load_var[a[0]], pt = ptr_pointee[var_type[vi]], comp = img_sampled[pt], vt = 0;
            for (uint32_t t = 1; t < bound; t++) if (vec_comp[t] == comp && vec_n[t] == 4u) { vt = t; break; }
            if (!vt) { fail(why, why_cap, "no 4-component vector type for the storage image texel"); goto done; }
            uint32_t r = next++;
            PUT((9u << 16) | OP_VECTOR_SHUFFLE); PUT(vt); PUT(r); PUT(a[2]); PUT(a[2]); PUT(2); PUT(1); PUT(0); PUT(3);   /* 9 words: op, type, result, 2 vectors, 4 components */
            PUT(((uint32_t)wc << 16) | OP_IMAGE_WRITE); PUT(a[0]); PUT(a[1]); PUT(r); for (uint32_t k = 3; k < wc - 1u; k++) PUT(a[k]);
            rewritten++;
        } else if (op == OP_IMAGE_READ && wc >= 5 && a[2] < bound && sel[a[2]]) {
            uint32_t tmp = next++;
            PUT(w[i]); PUT(a[0]); PUT(tmp); for (uint32_t k = 2; k < wc - 1u; k++) PUT(a[k]);
            PUT((9u << 16) | OP_VECTOR_SHUFFLE); PUT(a[0]); PUT(a[1]); PUT(tmp); PUT(tmp); PUT(2); PUT(1); PUT(0); PUT(3);
            rewritten++;
        } else for (uint32_t k = 0; k < wc; k++) PUT(w[i + k]);
        i += wc;
    }
    if (!rewritten) { rc = 0; goto done; }
    o[3] = next; *out = (uint8_t *)o; *out_len = on * 4u; o = NULL; rc = 1;
done:
    free(set); free(bind); free(hasset); free(hasbind); free(sel); free(selvar); free(ptr_pointee); free(var_type); free(img_sampled); free(img_is_storage); free(vec_comp); free(vec_n); free(load_var); free(o);
    return rc;
}
