/* SPDX-License-Identifier: MIT */
#include "spv_variants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint32_t lagfx_stub_vs_spv[67] = {
    0x07230203,0x00010600,0x00070000,0x0000000c,0x00000000,0x00020011,0x00000001,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000000,0x00000001,0x6e69616d,0x00000000,0x00000002,0x00040047,0x00000002,0x0000000b,0x00000000,
    0x00020013,0x00000003,0x00030021,0x00000004,0x00000003,0x00030016,0x00000005,0x00000020,0x00040017,0x00000006,0x00000005,0x00000004,0x00040020,0x00000007,0x00000003,0x00000006,0x0004003b,0x00000007,0x00000002,0x00000003,
    0x0004002b,0x00000005,0x00000008,0x00000000,0x0004002b,0x00000005,0x00000009,0x3f800000,0x0007002c,0x00000006,0x0000000a,0x00000008,0x00000008,0x00000008,0x00000009,0x00050036,0x00000003,0x00000001,0x00000000,0x00000004,
    0x000200f8,0x0000000b,0x0003003e,0x00000002,0x0000000a,0x000100fd,0x00010038 };
const uint32_t lagfx_stub_fs_spv[32] = {
    0x07230203,0x00010600,0x00070000,0x00000005,0x00000000,0x00020011,0x00000001,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000004,0x00000001,0x6e69616d,0x00000000,0x00030010,0x00000001,0x00000007,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00050036,0x00000002,0x00000001,0x00000000,0x00000003,0x000200f8,0x00000004,0x000100fd,0x00010038 };

#define OP_UNDEF 1u
#define OP_CAPABILITY 17u
#define OP_TYPE_INT 21u
#define OP_TYPE_FLOAT 22u
#define OP_TYPE_VECTOR 23u
#define OP_TYPE_MATRIX 24u
#define OP_TYPE_ARRAY 28u
#define OP_TYPE_STRUCT 30u
#define OP_TYPE_POINTER 32u
#define OP_CONST_NULL 46u
#define OP_FUNCTION 54u
#define OP_FUNCTION_END 56u
#define OP_VEC_EXTRACT_DYN 77u
#define OP_VEC_INSERT_DYN 78u
#define OP_UCONVERT 113u

typedef struct { uint32_t *w; size_t n, cap; } wbuf_t;
static int wput(wbuf_t *b, const uint32_t *src, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint32_t *p = realloc(b->w, c * 4);
        if (!p) return -1;
        b->w = p, b->cap = c;
    }
    memcpy(b->w + b->n, src, n * 4);
    b->n += n;
    return 0;
}
static void note(char *info, size_t cap, const char *fmt, unsigned a, unsigned b) { if (info && cap) snprintf(info, cap, fmt, a, b); }

/* instruction walk; returns 0 on a malformed module */
static int walk_ok(const uint32_t *in, size_t words) {
    if (words < 5 || in[0] != 0x07230203u) return 0;
    for (size_t w = 5; w < words;) { uint32_t wc = in[w] >> 16; if (!wc || w + wc > words) return 0; w += wc; }
    return 1;
}
/* word index of the first OpFunction (end of the module-scope declarations), or `words` */
static size_t first_function(const uint32_t *in, size_t words) {
    for (size_t w = 5; w < words; w += in[w] >> 16) if ((in[w] & 0xffffu) == OP_FUNCTION) return w;
    return words;
}
/* Is the result of this opcode typed (word1 = result type, word2 = result id)? Only used to find the TYPE of an id that is an integer index, so a coarse rule is enough. */
static int typed(uint32_t op) {
    if (op == 1 || op == 12 || (op >= 41 && op <= 55 && op != 53) || op == 57 || op == 59 || op == 61 || (op >= 65 && op <= 70)) return 1;
    if (op >= 77 && op <= 217) return 1;
    if ((op >= 222 && op <= 223) || op == 226 || op == 227 || (op >= 229 && op <= 242) || op == 245) return 1;
    return 0;
}

int lagfx_spv_variant_index32(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint32_t *tyof = calloc((size_t)bound + 1, 4), *width = calloc((size_t)bound + 1, 4);
    if (!tyof || !width) { free(tyof); free(width); return -1; }
    uint32_t u32 = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t op = in[w] & 0xffffu, wc = in[w] >> 16;
        if (op == OP_TYPE_INT && wc >= 4 && in[w + 1] < bound) { width[in[w + 1]] = in[w + 2]; if (in[w + 2] == 32 && in[w + 3] == 0 && !u32) u32 = in[w + 1]; }
        else if (typed(op) && wc >= 3 && in[w + 2] < bound) tyof[in[w + 2]] = in[w + 1];
    }
    int changes = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t op = in[w] & 0xffffu;
        size_t ix = op == OP_VEC_EXTRACT_DYN ? w + 4 : op == OP_VEC_INSERT_DYN ? w + 5 : 0;   /* extract: type res vec index; insert: type res vec component index */
        if (ix && (in[w] >> 16) > (ix - w) && in[ix] < bound && tyof[in[ix]] < bound && width[tyof[in[ix]]] == 64) changes++;
    }
    int rc = changes;
    if (!changes) { note(info, cap, "no OpVectorExtractDynamic / OpVectorInsertDynamic with a 64-bit index (%u)", 0, 0); goto done; }
    {
        wbuf_t b = { 0 };
        uint32_t next = bound, newty = 0;
        size_t ff = first_function(in, words);
        if (wput(&b, in, 5) < 0) { rc = -1; goto done; }
        for (size_t w = 5; w < words;) {
            uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
            if (w == ff && !u32) { newty = next++; u32 = newty; uint32_t t[4] = { (21u) | (4u << 16), newty, 32, 0 }; if (wput(&b, t, 4) < 0) { rc = -1; break; } }
            size_t ix = op == OP_VEC_EXTRACT_DYN ? w + 4 : op == OP_VEC_INSERT_DYN ? w + 5 : 0;
            if (ix && wc > ix - w && in[ix] < bound && tyof[in[ix]] < bound && width[tyof[in[ix]]] == 64) {
                uint32_t nid = next++, cv[4] = { OP_UCONVERT | (4u << 16), u32, nid, in[ix] };
                if (wput(&b, cv, 4) < 0) { rc = -1; break; }
                uint32_t tmp[16];
                if (wc > 16) { rc = -1; break; }
                memcpy(tmp, in + w, wc * 4u), tmp[ix - w] = nid;
                if (wput(&b, tmp, wc) < 0) { rc = -1; break; }
            } else if (wput(&b, in + w, wc) < 0) { rc = -1; break; }
            w += wc;
        }
        if (rc < 0) { free(b.w); goto done; }
        b.w[3] = next;
        *out = b.w, *out_words = b.n;
        note(info, cap, "%u 64-bit dynamic index(es) converted to 32 bit (%u new ids)", (unsigned)changes, (unsigned)(next - bound));
    }
done:
    free(tyof); free(width);
    return rc;
}

static int cap_is(uint32_t c, const uint32_t *list, size_t n) { for (size_t i = 0; i < n; i++) if (list[i] == c) return 1; return 0; }

int lagfx_spv_variant_strip_caps(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    int has8 = 0, has16 = 0, hasf16 = 0, has64 = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t op = in[w] & 0xffffu;
        if (op == OP_TYPE_INT) { uint32_t b = in[w + 2]; has8 |= b == 8; has16 |= b == 16; has64 |= b == 64; }
        else if (op == OP_TYPE_FLOAT && in[w + 2] == 16) hasf16 = 1;
    }
    static const uint32_t c8[] = { 39, 4448, 4449, 4450 }, c16[] = { 22 }, cf16[] = { 9 }, c16s[] = { 4433, 4434, 4435, 4436 }, c64[] = { 11, 12 };
    /* the 16-bit STORAGE capabilities serve both Int16 and Float16 data: dropped only when neither is used */
    wbuf_t b = { 0 };
    int removed = 0;
    if (wput(&b, in, 5) < 0) return -1;
    for (size_t w = 5; w < words;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu, c = op == OP_CAPABILITY ? in[w + 1] : 0;
        int drop = op == OP_CAPABILITY && ((!has8 && cap_is(c, c8, 4)) || (!has16 && cap_is(c, c16, 1)) || (!hasf16 && cap_is(c, cf16, 1)) || (!has16 && !hasf16 && cap_is(c, c16s, 4)) || (!has64 && cap_is(c, c64, 2)));
        if (drop) removed++; else if (wput(&b, in + w, wc) < 0) { free(b.w); return -1; }
        w += wc;
    }
    if (!removed) {
        free(b.w);
        if (info && cap) snprintf(info, cap, "nothing to strip; in use: %s%s%s%s", has8 ? "Int8 " : "", has16 ? "Int16 " : "", hasf16 ? "Float16 " : "", has64 ? "Int64" : "");
        return 0;
    }
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "%d unused capability declaration(s) dropped; still in use: %s%s%s%s", removed, has8 ? "Int8 " : "", has16 ? "Int16 " : "", hasf16 ? "Float16 " : "", has64 ? "Int64" : "(none)");
    return removed;
}

/* kinds of OpUndef result type that undef_rewrite() converts: bit 0 = vector / matrix / array, bit 1 = struct, bit 2 = pointer */
enum { UK_ARRAYLIKE = 1, UK_STRUCT = 2, UK_POINTER = 4 };
static int undef_rewrite(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap, unsigned kinds, const char *what) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint8_t *comp = calloc((size_t)bound + 1, 1);
    if (!comp) return -1;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t op = in[w] & 0xffffu;
        if ((in[w] >> 16) < 2u || in[w + 1] >= bound) continue;
        if ((op == OP_TYPE_VECTOR || op == OP_TYPE_MATRIX || op == OP_TYPE_ARRAY) && (kinds & UK_ARRAYLIKE)) comp[in[w + 1]] = 1;
        else if (op == OP_TYPE_STRUCT && (kinds & UK_STRUCT)) comp[in[w + 1]] = 1;
        else if (op == OP_TYPE_POINTER && (kinds & UK_POINTER)) comp[in[w + 1]] = 1;
    }
    size_t ff = first_function(in, words);
    int changes = 0, inside = 0;
    wbuf_t b = { 0 }, hoist = { 0 };
    int rc = 0;
    if (wput(&b, in, 5) < 0) { free(comp); return -1; }
    for (size_t w = 5; w < words && rc >= 0;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        if (op == OP_FUNCTION) inside = 1;
        int hit = op == OP_UNDEF && wc == 3 && in[w + 1] < bound && comp[in[w + 1]];
        if (hit) {
            changes++;
            uint32_t n[3] = { OP_CONST_NULL | (3u << 16), in[w + 1], in[w + 2] };
            if (!inside || w < ff) rc = wput(&b, n, 3); else rc = wput(&hoist, n, 3);   /* in a function: hoisted to the module scope (a constant instruction cannot live in a block) */
        } else rc = wput(&b, in + w, wc);
        if (op == OP_FUNCTION_END) inside = 0;
        w += wc;
    }
    free(comp);
    if (rc < 0) { free(b.w); free(hoist.w); return -1; }
    if (!changes) { free(b.w); free(hoist.w); if (info && cap) snprintf(info, cap, "no OpUndef of %s", what); return 0; }
    if (hoist.n) {   /* insert before the first OpFunction of the NEW buffer */
        size_t at = first_function(b.w, b.n);
        uint32_t *nw = malloc((b.n + hoist.n) * 4);
        if (!nw) { free(b.w); free(hoist.w); return -1; }
        memcpy(nw, b.w, at * 4), memcpy(nw + at, hoist.w, hoist.n * 4), memcpy(nw + at + hoist.n, b.w + at, (b.n - at) * 4);
        free(b.w);
        b.w = nw, b.n += hoist.n;
    }
    size_t hoist_n = hoist.n;
    free(hoist.w);
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "%u OpUndef of %s replaced by OpConstantNull (%u of them hoisted out of a function)", (unsigned)changes, what, (unsigned)(hoist_n / 3u));
    return changes;
}

/* (g) every composite type */
int lagfx_spv_variant_undef_composites(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    return undef_rewrite(in, words, out, out_words, info, cap, UK_ARRAYLIKE | UK_STRUCT, "a composite type");
}
/* (h) ONLY the struct- and pointer-typed ones (the vector / scalar ones are left alone) */
int lagfx_spv_variant_undef_struct_ptr(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    return undef_rewrite(in, words, out, out_words, info, cap, UK_STRUCT | UK_POINTER, "a struct or pointer type");
}

/* (j) ONLY the struct-typed ones (no pointer): with (h) and (i) this separates "pointer-typed OpUndef" from "struct-typed OpUndef" */
int lagfx_spv_variant_undef_struct(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    return undef_rewrite(in, words, out, out_words, info, cap, UK_STRUCT, "a struct type");
}

/* (i) drops every OpUndef whose id no other instruction mentions (any word equal to the id counts as a mention, so a literal that collides keeps the OpUndef: conservative).
 * Value-preserving: nothing consumed those values. */
int lagfx_spv_variant_drop_dead_undef(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint8_t *ref = calloc((size_t)bound + 1, 1);
    if (!ref) return -1;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        for (uint32_t k = 1; k < wc; k++) if (in[w + k] < bound && !(op == OP_UNDEF && k == 2)) ref[in[w + k]] = 1;
    }
    wbuf_t b = { 0 };
    int dropped = 0, rc = wput(&b, in, 5);
    for (size_t w = 5; w < words && rc >= 0;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        if (op == OP_UNDEF && wc == 3 && in[w + 2] < bound && !ref[in[w + 2]]) dropped++;
        else rc = wput(&b, in + w, wc);
        w += wc;
    }
    free(ref);
    if (rc < 0) { free(b.w); return -1; }
    if (!dropped) { free(b.w); if (info && cap) snprintf(info, cap, "no unreferenced OpUndef"); return 0; }
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "%d unreferenced OpUndef dropped", dropped);
    return dropped;
}

/* ---- rows of the -13 matrix (src/vulkan/pipeline_matrix.c) ---- */
#define OP_ENTRY_POINT 15u
#define OP_VARIABLE 59u
#define OP_SELECT 169u
#define OP_DECORATE 71u
#define OP_RETURN_VALUE 254u
#define OP_COPY_LOGICAL 400u
#define OP_PTR_EQUAL 401u
#define OP_PTR_NOT_EQUAL 402u
#define OP_PTR_DIFF 403u
#define OP_DECORATE_STRING 5632u
#define OP_MEMBER_DECORATE_STRING 5633u
#define OP_EXTENSION 10u
#define OP_EXECUTION_MODE 16u
#define DEC_BINDING 33u
#define DEC_DESCRIPTOR_SET 34u

/* (M9k) ONLY the OpReturnValue operands that are an OpUndef become a fresh OpConstantNull of the same type (hoisted to the module scope); the OpUndef itself and every other
 * use of it stay. CHANGES MEANING for a returned undef (undefined -> zero). */
int lagfx_spv_variant_undef_returns(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint32_t *undef_ty = calloc((size_t)bound + 1, 4);
    if (!undef_ty) return -1;
    for (size_t w = 5; w < words; w += in[w] >> 16)
        if ((in[w] & 0xffffu) == OP_UNDEF && (in[w] >> 16) == 3 && in[w + 2] < bound) undef_ty[in[w + 2]] = in[w + 1];
    uint32_t *nullid = calloc((size_t)bound + 1, 4);   /* per type id */
    if (!nullid) { free(undef_ty); return -1; }
    wbuf_t b = { 0 }, hoist = { 0 };
    int rc = wput(&b, in, 5), changes = 0;
    uint32_t next = bound;
    for (size_t w = 5; w < words && rc >= 0;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        if (op == OP_RETURN_VALUE && wc == 2 && in[w + 1] < bound && undef_ty[in[w + 1]]) {
            uint32_t ty = undef_ty[in[w + 1]];
            if (!nullid[ty]) { nullid[ty] = next++; uint32_t n[3] = { OP_CONST_NULL | (3u << 16), ty, nullid[ty] }; rc = wput(&hoist, n, 3); }
            uint32_t r[2] = { OP_RETURN_VALUE | (2u << 16), nullid[ty] };
            if (rc >= 0) rc = wput(&b, r, 2);
            changes++;
        } else rc = wput(&b, in + w, wc);
        w += wc;
    }
    free(undef_ty); free(nullid);
    if (rc < 0) { free(b.w); free(hoist.w); return -1; }
    if (!changes) { free(b.w); free(hoist.w); if (info && cap) snprintf(info, cap, "no OpReturnValue of an OpUndef"); return 0; }
    size_t at = first_function(b.w, b.n);
    uint32_t *nw = malloc((b.n + hoist.n) * 4);
    if (!nw) { free(b.w); free(hoist.w); return -1; }
    memcpy(nw, b.w, at * 4), memcpy(nw + at, hoist.w, hoist.n * 4), memcpy(nw + at + hoist.n, b.w + at, (b.n - at) * 4);
    nw[3] = next;
    size_t nn = b.n + hoist.n;
    free(b.w); free(hoist.w);
    *out = nw, *out_words = nn;
    if (info && cap) snprintf(info, cap, "%d OpReturnValue(OpUndef) operand(s) -> OpConstantNull (%u new null constant(s))", changes, (unsigned)(next - bound));
    return changes;
}

/* (M2) SPIR-V 1.4 -> 1.3 for a module that uses no 1.4-only feature: the version word, and the OpEntryPoint interface lists, which in 1.4+ name EVERY global variable but in
 * 1.3 may only name Input / Output ones (the others are dropped from the list). Refused (0, reason in info) for a module that is not exactly 1.4 or that uses OpCopyLogical, OpPtrEqual /
 * NotEqual / Diff, OpSelect of a struct / array, OpDecorateString (no extension) or a float-controls execution mode (no extension). */
int lagfx_spv_variant_downgrade13(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    if (in[1] != 0x00010400u) { if (info && cap) snprintf(info, cap, "module is SPIR-V %u.%u, not 1.4: nothing to downgrade", (in[1] >> 16) & 0xffu, (in[1] >> 8) & 0xffu); return 0; }
    uint32_t bound = in[3];
    uint8_t *agg = calloc((size_t)bound + 1, 1);
    uint8_t *sclass = calloc((size_t)bound + 1, 1);   /* storage class + 1 of each OpVariable id */
    uint32_t *tyof = calloc((size_t)bound + 1, 4), *vecn = calloc((size_t)bound + 1, 4);   /* result type of an id; component count of a vector type id */
    if (!agg || !sclass || !tyof || !vecn) { free(agg); free(sclass); free(tyof); free(vecn); return -1; }
    int fc_ext = 0, has_hlsl_ext = 0, rc = 0;
    uint32_t booltype = 0, boolvec[5] = { 0 };
    uint8_t needvec[5] = { 0 };
    unsigned n_copylogical = 0, n_ptrcmp = 0, n_selagg = 0, n_decstr = 0, n_fc_mode = 0, n_selvec = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        if ((op == 30u || op == 28u) && wc >= 2 && in[w + 1] < bound) agg[in[w + 1]] = 1;   /* OpTypeStruct / OpTypeArray */
        else if (op == 20u && wc >= 2 && !booltype) booltype = in[w + 1];                    /* OpTypeBool */
        else if (op == OP_TYPE_VECTOR && wc >= 4 && in[w + 1] < bound) { vecn[in[w + 1]] = in[w + 3]; if (booltype && in[w + 2] == booltype && in[w + 3] >= 2 && in[w + 3] <= 4) boolvec[in[w + 3]] = in[w + 1]; }
        else if (op == OP_VARIABLE && wc >= 4 && in[w + 2] < bound) { sclass[in[w + 2]] = (uint8_t)(in[w + 3] + 1u); tyof[in[w + 2]] = in[w + 1]; }
        else if (op == OP_EXTENSION) {
            const char *e = (const char *)(in + w + 1);
            if (!strncmp(e, "SPV_KHR_float_controls", 22)) fc_ext = 1;
            if (!strncmp(e, "SPV_GOOGLE_hlsl_functionality1", 30) || !strncmp(e, "SPV_GOOGLE_user_type", 20)) has_hlsl_ext = 1;
        } else if (op == OP_COPY_LOGICAL) n_copylogical++;
        else if (op == OP_PTR_EQUAL || op == OP_PTR_NOT_EQUAL || op == OP_PTR_DIFF) n_ptrcmp++;
        else if (op == OP_SELECT && wc == 6 && in[w + 1] < bound && agg[in[w + 1]]) n_selagg++;
        else if (op == OP_DECORATE_STRING || op == OP_MEMBER_DECORATE_STRING) n_decstr++;
        else if (op == OP_EXECUTION_MODE && wc >= 3 && in[w + 2] >= 4459u && in[w + 2] <= 4463u) n_fc_mode++;
        if (typed(op) && wc >= 3 && in[w + 2] < bound) tyof[in[w + 2]] = in[w + 1];
        if (op == OP_SELECT && wc == 6 && in[w + 1] < bound && vecn[in[w + 1]] && in[w + 3] < bound && booltype && tyof[in[w + 3]] == booltype) { n_selvec++; if (vecn[in[w + 1]] <= 4) needvec[vecn[in[w + 1]]] = 1; }   /* scalar condition, vector result: 1.4 only */
    }
    if (n_copylogical || n_ptrcmp || n_selagg || (n_decstr && !has_hlsl_ext) || (n_fc_mode && !fc_ext)) {
        if (info && cap) snprintf(info, cap, "uses 1.4-only features: OpCopyLogical x%u, OpPtrEqual/NotEqual/Diff x%u, OpSelect of struct/array x%u, OpDecorateString x%u, float-controls modes x%u", n_copylogical, n_ptrcmp, n_selagg, n_decstr, n_fc_mode);
        free(agg); free(sclass); free(tyof); free(vecn);
        return 0;
    }
    /* a scalar-condition OpSelect of a vector (1.4 only) becomes OpCompositeConstruct of a bool vector + the vector-condition OpSelect (valid in 1.3); needs bool-vector types, added at the module scope when absent */
    wbuf_t b = { 0 }, newtypes = { 0 };
    uint32_t next = bound;
    if (n_selvec) {
        for (uint32_t n = 2; n <= 4; n++) if (needvec[n] && !boolvec[n]) { boolvec[n] = next++; uint32_t t[4] = { OP_TYPE_VECTOR | (4u << 16), boolvec[n], booltype, n }; if (wput(&newtypes, t, 4) < 0) rc = -1; }
    }
    if (rc >= 0) rc = wput(&b, in, 5);
    int dropped = 0;
    if (rc >= 0) b.w[1] = 0x00010300u;
    for (size_t w = 5; w < words && rc >= 0;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        if (op == OP_ENTRY_POINT && wc >= 4) {
            size_t k = w + 3;   /* the name: a nul-terminated string in whole words */
            while (k < w + wc) { const char *p = (const char *)(in + k); k++; if (!p[0] || !p[1] || !p[2] || !p[3]) break; }
            uint32_t *tmp = malloc((size_t)wc * 4);
            if (!tmp) { rc = -1; break; }
            size_t o = k - w;
            memcpy(tmp, in + w, o * 4);
            for (size_t i = k; i < w + wc; i++) {
                uint32_t id = in[i];
                uint32_t sc = id < bound ? sclass[id] : 0u;   /* storage class + 1 */
                if (sc && sc != 2u /* Input */ && sc != 4u /* Output */) { dropped++; continue; }
                tmp[o++] = id;
            }
            tmp[0] = OP_ENTRY_POINT | ((uint32_t)o << 16);
            rc = wput(&b, tmp, o);
            free(tmp);
        } else if (op == OP_SELECT && wc == 6 && in[w + 1] < bound && vecn[in[w + 1]] && in[w + 3] < bound && booltype && tyof[in[w + 3]] == booltype) {
            uint32_t n = vecn[in[w + 1]], cv = next++;
            uint32_t cc[7] = { 80u /* OpCompositeConstruct */ | ((3u + n) << 16), boolvec[n], cv, in[w + 3], in[w + 3], in[w + 3], in[w + 3] };
            rc = wput(&b, cc, 3u + n);
            uint32_t sel[6] = { OP_SELECT | (6u << 16), in[w + 1], in[w + 2], cv, in[w + 4], in[w + 5] };
            if (rc >= 0) rc = wput(&b, sel, 6);
        } else rc = wput(&b, in + w, wc);
        w += wc;
    }
    free(agg); free(sclass); free(tyof); free(vecn);
    if (rc >= 0 && newtypes.n) {
        size_t at = first_function(b.w, b.n);
        uint32_t *nw = malloc((b.n + newtypes.n) * 4);
        if (!nw) rc = -1;
        else { memcpy(nw, b.w, at * 4), memcpy(nw + at, newtypes.w, newtypes.n * 4), memcpy(nw + at + newtypes.n, b.w + at, (b.n - at) * 4); free(b.w); b.w = nw, b.n += newtypes.n; }
    }
    free(newtypes.w);
    if (rc < 0) { free(b.w); return -1; }
    b.w[3] = next;
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "SPIR-V 1.4 -> 1.3, %d non-Input/Output id(s) dropped from the OpEntryPoint interface list(s), %u scalar-condition OpSelect of a vector rewritten with a bool-vector condition", dropped, n_selvec);
    return 1 + dropped + (int)n_selvec;
}

/* (M3b / M4a) input-attachment renumbering, read from the module itself. A variable with an InputAttachmentIndex k is an input attachment.
 *   mode 0 (IAFIRST): input attachments get Binding = k (set unchanged); every other descriptor variable of set 0 is shifted up by `nia`.
 *   mode 1 (IASET1):  input attachments move to DescriptorSet 1, Binding = k; nothing else changes.
 * `nia` = the number of input attachments of the FRAGMENT module (the vertex module has none but its bindings shift all the same). The pipeline layout must be renumbered the same way. */
#define DEC_INPUT_ATTACHMENT_INDEX 43u
int lagfx_spv_variant_ia_rebind(const uint32_t *in, size_t words, unsigned mode, uint32_t nia, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint32_t *iai = calloc((size_t)bound + 1, 4);   /* InputAttachmentIndex + 1 */
    uint8_t *has_b = calloc((size_t)bound + 1, 1);
    if (!iai || !has_b) { free(iai); free(has_b); return -1; }
    unsigned n_ia = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        if ((in[w] & 0xffffu) != OP_DECORATE || (in[w] >> 16) < 4 || in[w + 1] >= bound) continue;
        if (in[w + 2] == DEC_INPUT_ATTACHMENT_INDEX) { iai[in[w + 1]] = in[w + 3] + 1u; n_ia++; }
        else if (in[w + 2] == DEC_BINDING) has_b[in[w + 1]] = 1;
    }
    wbuf_t b = { 0 };
    int rc = wput(&b, in, 5), changes = 0;
    for (size_t w = 5; w < words && rc >= 0;) {
        uint32_t wc = in[w] >> 16, op = in[w] & 0xffffu;
        uint32_t tmp[4];
        if (op == OP_DECORATE && wc == 4 && in[w + 1] < bound && has_b[in[w + 1]] && (in[w + 2] == DEC_DESCRIPTOR_SET || in[w + 2] == DEC_BINDING)) {
            memcpy(tmp, in + w, 16);
            uint32_t k = iai[in[w + 1]];
            if (mode == 0) { if (in[w + 2] == DEC_BINDING) tmp[3] = k ? k - 1u : in[w + 3] + nia; }
            else if (k) tmp[3] = in[w + 2] == DEC_DESCRIPTOR_SET ? 1u : k - 1u;
            if (tmp[3] != in[w + 3]) changes++;
            rc = wput(&b, tmp, 4);
        } else rc = wput(&b, in + w, wc);
        w += wc;
    }
    free(iai); free(has_b);
    if (rc < 0) { free(b.w); return -1; }
    if (!changes) { free(b.w); if (info && cap) snprintf(info, cap, "no descriptor variable to renumber"); return 0; }
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "%d descriptor decoration(s) renumbered (%u input attachment variable(s))", changes, n_ia);
    return changes;
}

/* (n) D2 candidate: GLSL.std.450 FMin / FMax / FClamp (37 / 40 / 43) -> NMin / NMax / NClamp (79 / 80 / 81) in every OpExtInst of the module's "GLSL.std.450" import. Same operand layout, no new
 * capability. CHANGES MEANING only for a NaN operand (FMin / FMax / FClamp: undefined which operand is the result; NMin / NMax / NClamp: the other operand is the result). */
int lagfx_spv_variant_nanmax(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t glsl = 0;
    for (size_t w = 5; w < words; w += in[w] >> 16)
        if ((in[w] & 0xffffu) == 11u && (in[w] >> 16) >= 3 && !strncmp((const char *)&in[w + 2], "GLSL.std.450", 13)) glsl = in[w + 1];   /* OpExtInstImport: result id, name */
    if (!glsl) { if (info && cap) snprintf(info, cap, "no GLSL.std.450 import"); return 0; }
    wbuf_t b = { 0 };
    int rc = wput(&b, in, words) , n37 = 0, n40 = 0, n43 = 0;
    if (rc < 0) return -1;
    for (size_t w = 5; w < words; w += b.w[w] >> 16) {
        if ((b.w[w] & 0xffffu) != 12u || (b.w[w] >> 16) < 6 || b.w[w + 3] != glsl) continue;   /* OpExtInst: type, result, set, instruction, operands */
        uint32_t *ins = &b.w[w + 4];
        if (*ins == 37u) *ins = 79u, n37++; else if (*ins == 40u) *ins = 80u, n40++; else if (*ins == 43u) *ins = 81u, n43++;
    }
    const int changes = n37 + n40 + n43;
    if (!changes) { free(b.w); if (info && cap) snprintf(info, cap, "no FMin / FMax / FClamp"); return 0; }
    *out = b.w, *out_words = b.n;
    if (info && cap) snprintf(info, cap, "FMin->NMin %d, FMax->NMax %d, FClamp->NClamp %d", n37, n40, n43);
    return changes;
}
