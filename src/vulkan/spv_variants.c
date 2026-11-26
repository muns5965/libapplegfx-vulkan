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

int lagfx_spv_variant_undef_composites(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    if (out) *out = NULL;
    if (!walk_ok(in, words)) return -1;
    uint32_t bound = in[3];
    uint8_t *comp = calloc((size_t)bound + 1, 1);
    if (!comp) return -1;
    for (size_t w = 5; w < words; w += in[w] >> 16) {
        uint32_t op = in[w] & 0xffffu;
        if ((op == OP_TYPE_VECTOR || op == OP_TYPE_MATRIX || op == OP_TYPE_ARRAY || op == OP_TYPE_STRUCT) && in[w + 1] < bound) comp[in[w + 1]] = 1;
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
    if (!changes) { free(b.w); free(hoist.w); if (info && cap) snprintf(info, cap, "no OpUndef of a composite type"); return 0; }
    if (hoist.n) {   /* insert before the first OpFunction of the NEW buffer */
        size_t at = first_function(b.w, b.n);
        uint32_t *nw = malloc((b.n + hoist.n) * 4);
        if (!nw) { free(b.w); free(hoist.w); return -1; }
        memcpy(nw, b.w, at * 4), memcpy(nw + at, hoist.w, hoist.n * 4), memcpy(nw + at + hoist.n, b.w + at, (b.n - at) * 4);
        free(b.w);
        b.w = nw, b.n += hoist.n;
    }
    free(hoist.w);
    *out = b.w, *out_words = b.n;
    note(info, cap, "%u OpUndef of a composite type replaced by OpConstantNull (%u of them hoisted out of a function)", (unsigned)changes, 0);
    return changes;
}
