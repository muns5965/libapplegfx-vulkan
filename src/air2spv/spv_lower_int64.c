/* SPDX-License-Identifier: MIT */
#include "spv_lower_int64.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_UNDEF = 1, OP_NAME = 5, OP_CAPABILITY = 17, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_POINTER = 32, OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24, OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30,
    OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44, OP_CONSTANT_NULL = 46, OP_SPEC_CONSTANT = 50, OP_FUNCTION_PARAMETER = 55, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62,
    OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67, OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70,
    OP_VECTOR_EXTRACT_DYNAMIC = 77, OP_VECTOR_INSERT_DYNAMIC = 78, OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81, OP_COMPOSITE_INSERT = 82, OP_COPY_OBJECT = 83, OP_U_CONVERT = 113, OP_S_CONVERT = 114,
    OP_S_NEGATE = 126, OP_I_ADD = 128, OP_I_SUB = 130, OP_I_MUL = 132, OP_U_MUL_EXTENDED = 151, OP_SELECT = 169, OP_PHI = 245, OP_RETURN_VALUE = 254,
    OP_LOGICAL_OR = 166, OP_LOGICAL_AND = 167, OP_I_EQUAL = 170, OP_I_NOT_EQUAL = 171, OP_UGT = 172, OP_SGT = 173, OP_UGE = 174, OP_SGE = 175, OP_ULT = 176, OP_SLT = 177, OP_ULE = 178, OP_SLE = 179,
    OP_SHR_LOGICAL = 194, OP_SHR_ARITH = 195, OP_SHL = 196, OP_BIT_OR = 197, OP_BIT_XOR = 198, OP_BIT_AND = 199, OP_NOT = 200,
};

typedef struct { uint32_t *w; size_t n, cap; bool oom; } wbuf_t;
static void wput(wbuf_t *o, uint32_t v) {
    if (o->oom) return;
    if (o->n == o->cap) { size_t nc = o->cap ? o->cap * 2 : 4096; uint32_t *p = realloc(o->w, nc * 4); if (!p) { o->oom = true; return; } o->w = p; o->cap = nc; }
    o->w[o->n++] = v;
}
static void wins(wbuf_t *o, uint32_t op, const uint32_t *a, uint32_t na) { wput(o, ((na + 1u) << 16) | op); for (uint32_t i = 0; i < na; i++) wput(o, a[i]); }

typedef struct {
    const uint32_t *w; size_t nw;
    uint32_t bound;
    uint32_t *ty;          /* result type id of each id (0 = none / a type) */
    uint8_t *isint;        /* OpTypeInt width/8 class: 1 = 64-bit int, 2 = 32-bit unsigned/signed int, 3 = narrower int */
    uint32_t *width;       /* int type widths */
    uint32_t I64, U32, BOOL, V2, X, STR, c0, c1, c31, c32, c63;
    wbuf_t o; uint32_t next;
    char *why; size_t why_cap; bool failed;
} L;

static void fail(L *l, const char *fmt, uint32_t a) { if (!l->failed && l->why && l->why_cap) snprintf(l->why, l->why_cap, fmt, a); l->failed = true; }

/* copy one instruction verbatim, but where the 64-bit type id was merged into an EXISTING uvec2 type (l->X), point type-id operands at that type */
static void copy_rm(L *l, size_t i, uint32_t wc, uint32_t op) {
    uint32_t first = 0, last = 0;   /* word range (instruction-relative) holding type ids */
    switch (op) {
    case OP_TYPE_POINTER: first = last = 3; break;
    case OP_TYPE_ARRAY: case 29: first = last = 2; break;
    case OP_TYPE_STRUCT: case 33: first = 2; last = wc - 1; break;
    default: if (op >= 43u || op == OP_UNDEF) { first = last = 1; } break;   /* result type of value-producing instructions */
    }
    for (uint32_t k = 0; k < wc; k++) {
        uint32_t v = l->w[i + k];
        if (l->X && k >= first && first && k <= last && v == l->I64) v = l->X;
        wput(&l->o, v);
    }
}
static uint32_t nid(L *l) { return l->next++; }
static bool is64(const L *l, uint32_t id) { return id < l->bound && l->ty[id] == l->I64 && l->I64; }
/* emit a result-producing instruction and return its id */
static uint32_t E(L *l, uint32_t op, uint32_t ty, uint32_t a, uint32_t b, int nargs) {
    uint32_t r = nid(l), v[4] = { ty, r, a, b }; wins(&l->o, op, v, 2u + (uint32_t)nargs); return r;
}
static uint32_t Ex(L *l, uint32_t v, uint32_t idx) { uint32_t r = nid(l), a[4] = { l->U32, r, v, idx }; wins(&l->o, OP_COMPOSITE_EXTRACT, a, 4); return r; }
static uint32_t Mk(L *l, uint32_t lo, uint32_t hi, uint32_t res) { uint32_t r = res ? res : nid(l), a[4] = { l->V2, r, lo, hi }; wins(&l->o, OP_COMPOSITE_CONSTRUCT, a, 4); return r; }
static uint32_t Sel(L *l, uint32_t cond, uint32_t a, uint32_t b) { uint32_t r = nid(l), x[5] = { l->U32, r, cond, a, b }; wins(&l->o, OP_SELECT, x, 5); return r; }
static uint32_t U(L *l, uint32_t op, uint32_t a, uint32_t b) { return E(l, op, l->U32, a, b, 2); }
static uint32_t B(L *l, uint32_t op, uint32_t a, uint32_t b) { return E(l, op, l->BOOL, a, b, 2); }

/* a 64-bit shift by `amt` (the low word of the 64-bit amount); kind: OP_SHL / OP_SHR_LOGICAL / OP_SHR_ARITH */
static void shift64(L *l, uint32_t kind, uint32_t res, uint32_t a, uint32_t amt_lo) {
    uint32_t lo = Ex(l, a, 0), hi = Ex(l, a, 1);
    uint32_t n = U(l, OP_BIT_AND, amt_lo, l->c63), n31 = U(l, OP_BIT_AND, n, l->c31);
    uint32_t lt32 = B(l, OP_ULT, n, l->c32), z = B(l, OP_I_EQUAL, n31, l->c0);
    uint32_t inv = U(l, OP_BIT_AND, U(l, OP_I_SUB, l->c32, n31), l->c31);       /* 32 - n31, 0 when n31 == 0 (then masked out by z) */
    uint32_t rlo, rhi;
    if (kind == OP_SHL) {
        uint32_t lo_s = U(l, OP_SHL, lo, n31);
        uint32_t carry = Sel(l, z, l->c0, U(l, OP_SHR_LOGICAL, lo, inv));
        uint32_t hi_a = U(l, OP_BIT_OR, U(l, OP_SHL, hi, n31), carry);
        rhi = Sel(l, lt32, hi_a, lo_s);
        rlo = Sel(l, lt32, lo_s, l->c0);
    } else {
        const bool arith = kind == OP_SHR_ARITH;
        uint32_t hi_s = U(l, arith ? OP_SHR_ARITH : OP_SHR_LOGICAL, hi, n31);
        uint32_t carry = Sel(l, z, l->c0, U(l, OP_SHL, hi, inv));
        uint32_t lo_a = U(l, OP_BIT_OR, U(l, OP_SHR_LOGICAL, lo, n31), carry);
        rlo = Sel(l, lt32, lo_a, hi_s);
        rhi = Sel(l, lt32, hi_s, arith ? U(l, OP_SHR_ARITH, hi, l->c31) : l->c0);
    }
    Mk(l, rlo, rhi, res);
}

static uint32_t nid_all_ones(L *l) { return E(l, OP_NOT, l->U32, l->c0, 0, 1); }

static uint32_t cmp64(L *l, uint32_t op, uint32_t a, uint32_t b) {
    uint32_t al = Ex(l, a, 0), ah = Ex(l, a, 1), bl = Ex(l, b, 0), bh = Ex(l, b, 1);
    if (op == OP_I_EQUAL) return B(l, OP_LOGICAL_AND, B(l, OP_I_EQUAL, al, bl), B(l, OP_I_EQUAL, ah, bh));
    if (op == OP_I_NOT_EQUAL) return B(l, OP_LOGICAL_OR, B(l, OP_I_NOT_EQUAL, al, bl), B(l, OP_I_NOT_EQUAL, ah, bh));
    uint32_t hi_strict, lo_op;
    switch (op) {
    case OP_ULT: hi_strict = OP_ULT; lo_op = OP_ULT; break;   case OP_SLT: hi_strict = OP_SLT; lo_op = OP_ULT; break;
    case OP_ULE: hi_strict = OP_ULT; lo_op = OP_ULE; break;   case OP_SLE: hi_strict = OP_SLT; lo_op = OP_ULE; break;
    case OP_UGT: hi_strict = OP_UGT; lo_op = OP_UGT; break;   case OP_SGT: hi_strict = OP_SGT; lo_op = OP_UGT; break;
    case OP_UGE: hi_strict = OP_UGT; lo_op = OP_UGE; break;   default: hi_strict = OP_SGT; lo_op = OP_UGE; break;   /* OP_SGE */
    }
    uint32_t hs = B(l, hi_strict, ah, bh), he = B(l, OP_I_EQUAL, ah, bh), lc = B(l, lo_op, al, bl);
    return B(l, OP_LOGICAL_OR, hs, B(l, OP_LOGICAL_AND, he, lc));
}

static bool is_ptr_or_func_type_op(uint32_t op) { return (op >= 19 && op <= 39); }   /* OpTypeVoid .. OpTypeForwardPointer region: the declarations themselves */

int lagfx_spv_lower_int64(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len, char *why, size_t why_cap) {
    if (why && why_cap) why[0] = 0;
    if (!in || in_len < 20u || (in_len & 3u) || ((const uint32_t *)(const void *)in)[0] != 0x07230203u || !out || !out_len) return -1;
    L l; memset(&l, 0, sizeof l);
    l.w = (const uint32_t *)(const void *)in; l.nw = in_len / 4u; l.bound = l.w[3]; l.why = why; l.why_cap = why_cap;
    if (l.bound == 0 || l.bound > (1u << 24)) return -1;
    l.ty = calloc(l.bound, 4); l.isint = calloc(l.bound, 1); l.width = calloc(l.bound, 4);
    int rc = -1;
    if (!l.ty || !l.isint || !l.width) goto done;

    /* pass 1: types of ids, the 64-bit / 32-bit / bool type ids, the first 64-bit declaration */
    size_t first64 = (size_t)-1; int n64 = 0; bool cap_int64 = false;
    for (size_t i = 5; i < l.nw;) {
        uint32_t wc = l.w[i] >> 16, op = l.w[i] & 0xffffu; if (!wc || i + wc > l.nw) goto done;
        const uint32_t *o = &l.w[i + 1];
        if (op == OP_TYPE_INT && wc >= 4) {
            l.isint[o[0]] = 1; l.width[o[0]] = o[1];
            if (o[1] == 64u) { n64++; if (!l.I64) { l.I64 = o[0]; first64 = i; } }
            else if (o[1] == 32u && o[2] == 0u && !l.U32) l.U32 = o[0];
        } else if (op == OP_TYPE_VECTOR && wc >= 4 && l.U32 && o[1] == l.U32 && o[2] == 2u && !l.X) { l.X = o[0];
        } else if (op == OP_TYPE_BOOL && !l.BOOL) l.BOOL = o[0];
        else if (op == OP_CAPABILITY && o[0] == 11u) cap_int64 = true;
        /* result type / result id of the value-producing instructions: [type, id] for most; a few have no type */
        if (op >= 19 && op <= 39) { /* type declarations: no result type */ }
        else if (wc >= 3 && op != OP_NAME && op != OP_STORE && !(op >= 71 && op <= 75) && op != OP_CAPABILITY && op != 14 /* MemoryModel */ && op != 15 /* EntryPoint */ && op != 16 /* ExecutionMode */) {
            if (o[1] < l.bound && o[0] < l.bound && (op == OP_FUNCTION_PARAMETER || op == OP_UNDEF || (op >= 43 && op <= 52) || op == OP_VARIABLE || op == OP_LOAD || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN ||
                op == OP_PTR_ACCESS_CHAIN || op == OP_IN_BOUNDS_PTR_ACCESS_CHAIN || (op >= 79 && op <= 83) || (op >= 109 && op <= 142) || (op >= 164 && op <= 200) || op == OP_PHI || op == OP_FUNCTION_CALL || op == 54 /* OpFunction */))
                l.ty[o[1]] = o[0];
        }
        i += wc;
    }
    if (!l.I64) { rc = 0; goto done; }
    if (n64 != 1) { fail(&l, "%u distinct 64-bit integer types (only one, unsigned, is lowered)", (uint32_t)n64); goto done; }
    (void)cap_int64;

    /* pass 2: rewrite */
    l.next = l.bound;
    for (size_t i = 0; i < 5; i++) wput(&l.o, l.w[i]);
    size_t iu = (size_t)-1, ib = (size_t)-1, ix = (size_t)-1;   /* positions of the original 32-bit / bool declarations */
    for (size_t i = 5; i < l.nw;) { uint32_t wc = l.w[i] >> 16, op = l.w[i] & 0xffffu; const uint32_t *o = &l.w[i + 1];
        if (op == OP_TYPE_INT && o[0] == l.U32) iu = i; else if (op == OP_TYPE_VECTOR && l.X && o[0] == l.X) ix = i; else if (op == OP_TYPE_BOOL && o[0] == l.BOOL) ib = i; i += wc; }
    const bool hoist_x = l.X && ix > first64;
    const bool hoist_u = l.U32 && iu > first64, hoist_b = l.BOOL && ib > first64;
    if (!l.U32) l.U32 = (l.next++);
    if (!l.BOOL) l.BOOL = (l.next++);
    const bool need_u = !(l.U32 < l.bound), need_b = !(l.BOOL < l.bound);
    l.V2 = nid(&l); l.STR = nid(&l); l.c0 = nid(&l); l.c1 = nid(&l); l.c31 = nid(&l); l.c32 = nid(&l); l.c63 = nid(&l);
    for (size_t i = 5; i < l.nw && !l.failed && !l.o.oom;) {
        const uint32_t wc = l.w[i] >> 16, op = l.w[i] & 0xffffu; const uint32_t *o = &l.w[i + 1];
        if (op == OP_CAPABILITY && o[0] == 11u) { i += wc; continue; }     /* Int64 no longer needed (checked at the end: nothing 64-bit is left) */
        if (i == first64) {   /* the 64-bit declaration: hoist the 32-bit / bool types here, then uvec2 in its place, then the helper types and constants */
            if (need_u || hoist_u) { uint32_t a[3] = { l.U32, 32u, 0u }; wins(&l.o, OP_TYPE_INT, a, 3); }
            if (need_b || hoist_b) { uint32_t a[1] = { l.BOOL }; wins(&l.o, OP_TYPE_BOOL, a, 1); }
            if (!l.X) { uint32_t v2[3] = { l.I64, l.U32, 2u }; wins(&l.o, OP_TYPE_VECTOR, v2, 3); }   /* the 64-bit type id now names uvec2: every user of it follows */
            else if (hoist_x) { uint32_t v2[3] = { l.X, l.U32, 2u }; wins(&l.o, OP_TYPE_VECTOR, v2, 3); }         /* an existing uvec2 type is hoisted here instead (no duplicate declaration) */
            uint32_t st[3] = { l.STR, l.U32, l.U32 }; wins(&l.o, OP_TYPE_STRUCT, st, 3);
            const uint32_t cv[5] = { 0, 1, 31, 32, 63 }; const uint32_t ci[5] = { l.c0, l.c1, l.c31, l.c32, l.c63 };
            for (int k = 0; k < 5; k++) { uint32_t c[3] = { l.U32, ci[k], cv[k] }; wins(&l.o, OP_CONSTANT, c, 3); }
            l.V2 = l.X ? l.X : l.I64;
            i += wc; continue;
        }
        if ((hoist_u && i == iu) || (hoist_b && i == ib)) { i += wc; continue; }
        if (hoist_x && op == OP_TYPE_VECTOR && o[0] == l.X) { i += wc; continue; }   /* declared at the hoist point */
        if (l.X && (op == OP_NAME || op == 6u || op == 71u || op == 72u) && o[0] == l.I64) { i += wc; continue; }   /* declared at the hoist point instead */
        const uint32_t rty = (op >= 19 && op <= 39) ? 0u : (wc >= 3 && o[0] < l.bound ? o[0] : 0u);
        const bool res64 = rty && rty == l.I64 && op != OP_TYPE_INT;
        switch (op) {
        case OP_TYPE_VECTOR: case OP_TYPE_MATRIX:
            if (o[1] == l.I64) { fail(&l, "vector of 64-bit integers (id %u)", o[0]); break; }
            copy_rm(&l, i, wc, op); break;
        case OP_SPEC_CONSTANT:
            if (res64) { fail(&l, "64-bit specialization constant (id %u)", o[1]); break; }
            copy_rm(&l, i, wc, op); break;
        case OP_CONSTANT:
            if (res64) {
                uint32_t lo = nid(&l), hi = nid(&l); uint32_t a[3] = { l.U32, lo, o[2] }, b[3] = { l.U32, hi, wc > 4 ? o[3] : 0u };
                wins(&l.o, OP_CONSTANT, a, 3); wins(&l.o, OP_CONSTANT, b, 3);
                uint32_t cc[4] = { l.V2, o[1], lo, hi }; wins(&l.o, OP_CONSTANT_COMPOSITE, cc, 4);
            } else copy_rm(&l, i, wc, op);
            break;
        case OP_I_ADD: case OP_I_SUB: case OP_I_MUL: if (!res64) goto generic;
            {   const uint32_t a = o[2], b = o[3], r = o[1];
                uint32_t al = Ex(&l, a, 0), ah = Ex(&l, a, 1), bl = Ex(&l, b, 0), bh = Ex(&l, b, 1);
                if (op == OP_I_ADD) {
                    uint32_t lo = U(&l, OP_I_ADD, al, bl), cy = Sel(&l, B(&l, OP_ULT, lo, al), l.c1, l.c0);
                    Mk(&l, lo, U(&l, OP_I_ADD, U(&l, OP_I_ADD, ah, bh), cy), r);
                } else if (op == OP_I_SUB) {
                    uint32_t lo = U(&l, OP_I_SUB, al, bl), br = Sel(&l, B(&l, OP_ULT, al, bl), l.c1, l.c0);
                    Mk(&l, lo, U(&l, OP_I_SUB, U(&l, OP_I_SUB, ah, bh), br), r);
                } else {
                    uint32_t ext = E(&l, OP_U_MUL_EXTENDED, l.STR, al, bl, 2), lo = nid(&l), h0 = nid(&l);
                    { uint32_t x[4] = { l.U32, lo, ext, 0 }; wins(&l.o, OP_COMPOSITE_EXTRACT, x, 4); }
                    { uint32_t x[4] = { l.U32, h0, ext, 1 }; wins(&l.o, OP_COMPOSITE_EXTRACT, x, 4); }
                    Mk(&l, lo, U(&l, OP_I_ADD, U(&l, OP_I_ADD, h0, U(&l, OP_I_MUL, al, bh)), U(&l, OP_I_MUL, ah, bl)), r);
                }
            }
            break;
        case OP_S_NEGATE: if (!res64) goto generic;
            { uint32_t al = Ex(&l, o[2], 0), ah = Ex(&l, o[2], 1);
              uint32_t lo = U(&l, OP_I_SUB, l.c0, al), br = Sel(&l, B(&l, OP_ULT, l.c0, al), l.c1, l.c0);
              Mk(&l, lo, U(&l, OP_I_SUB, U(&l, OP_I_SUB, l.c0, ah), br), o[1]); }
            break;
        case OP_SHL: case OP_SHR_LOGICAL: case OP_SHR_ARITH: if (!res64) goto generic;
            { uint32_t amt = is64(&l, o[3]) ? Ex(&l, o[3], 0) : o[3];
              if (!is64(&l, o[3]) && l.isint[l.ty[o[3]]] && l.width[l.ty[o[3]]] != 32u) { fail(&l, "64-bit shift by a non-32-bit amount (id %u)", o[1]); break; }
              shift64(&l, op, o[1], o[2], amt); }
            break;
        case OP_I_EQUAL: case OP_I_NOT_EQUAL: case OP_UGT: case OP_SGT: case OP_UGE: case OP_SGE: case OP_ULT: case OP_SLT: case OP_ULE: case OP_SLE:
            if (!is64(&l, o[2])) goto generic;
            { uint32_t r = cmp64(&l, op, o[2], o[3]); uint32_t x[3] = { l.BOOL, o[1], r }; wins(&l.o, OP_COPY_OBJECT, x, 3); }
            break;
        case OP_SELECT: if (!res64) goto generic;
            { uint32_t lo = Sel(&l, o[2], Ex(&l, o[3], 0), Ex(&l, o[4], 0)), hi = Sel(&l, o[2], Ex(&l, o[3], 1), Ex(&l, o[4], 1)); Mk(&l, lo, hi, o[1]); }
            break;
        case OP_U_CONVERT: case OP_S_CONVERT:
            if (res64) {   /* to 64 bits */
                const uint32_t src = o[2], sw = l.isint[l.ty[src]] ? l.width[l.ty[src]] : 0u;
                if (l.BOOL && l.ty[src] == l.BOOL) {   /* the translator emits zext/sext of an i1 as a conversion of a bool: lower it as a select */
                    uint32_t v = Sel(&l, src, op == OP_U_CONVERT ? l.c1 : nid_all_ones(&l), l.c0);
                    Mk(&l, v, op == OP_U_CONVERT ? l.c0 : v, o[1]); break;
                }
                if (is64(&l, src) || !sw || sw > 32u) { fail(&l, "unsupported 64-bit conversion source (id %u)", o[1]); break; }
                uint32_t v = src;
                if (sw < 32u) v = E(&l, op, l.U32, src, 0, 1);
                Mk(&l, v, op == OP_U_CONVERT ? l.c0 : U(&l, OP_SHR_ARITH, v, l.c31), o[1]);
            } else if (is64(&l, o[2])) {   /* from 64 bits: the low word */
                uint32_t lo = Ex(&l, o[2], 0);
                if (rty == l.U32) { uint32_t x[3] = { rty, o[1], lo }; wins(&l.o, OP_COPY_OBJECT, x, 3); }
                else { uint32_t x[3] = { rty, o[1], lo }; wins(&l.o, OP_U_CONVERT, x, 3); }
            } else goto generic;
            break;
        case OP_VECTOR_EXTRACT_DYNAMIC: case OP_VECTOR_INSERT_DYNAMIC: {   /* a 64-bit dynamic index: the same high-word guard as access chains (out of range stays out of range) */
            uint32_t nw3[8]; const uint32_t ix = op == OP_VECTOR_EXTRACT_DYNAMIC ? 4u : 5u;
            if (wc > 8u || wc <= ix || res64) { fail(&l, "unsupported 64-bit integer operation (SPIR-V opcode %u)", op); break; }
            memcpy(nw3, &l.w[i], wc * 4u);
            if (is64(&l, nw3[ix])) { uint32_t idx = nw3[ix], lo = Ex(&l, idx, 0), hi = Ex(&l, idx, 1); nw3[ix] = Sel(&l, B(&l, OP_I_EQUAL, hi, l.c0), lo, nid_all_ones(&l)); }
            for (uint32_t k = 0; k < wc; k++) wput(&l.o, nw3[k]);
            break;
        }
        case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: case OP_PTR_ACCESS_CHAIN: case OP_IN_BOUNDS_PTR_ACCESS_CHAIN: {
            uint32_t nw2[32]; if (wc > 32u) { fail(&l, "access chain too long (id %u)", o[1]); break; }
            memcpy(nw2, &l.w[i], wc * 4u);
            for (uint32_t k = 4; k < wc; k++) if (is64(&l, nw2[k])) {
                /* a 64-bit index with a non-zero high word is out of range for any buffer (< 2^32 bytes); keeping only the low word would wrap it onto a DIFFERENT valid element.
                 * So it becomes 0xFFFFFFFF (still out of range: the driver's robust-access rules apply, as for the original index). */
                uint32_t idx = nw2[k], lo = Ex(&l, idx, 0), hi = Ex(&l, idx, 1);
                nw2[k] = Sel(&l, B(&l, OP_I_EQUAL, hi, l.c0), lo, nid_all_ones(&l));
            }
            if (l.X && nw2[1] == l.I64) nw2[1] = l.X;
            for (uint32_t k = 0; k < wc; k++) wput(&l.o, nw2[k]);
            break;
        }
        case OP_PHI: case OP_LOAD: case OP_STORE: case OP_COPY_OBJECT: case OP_UNDEF: case OP_CONSTANT_NULL: case OP_VARIABLE: case OP_FUNCTION_PARAMETER: case OP_FUNCTION_CALL: case OP_RETURN_VALUE:
        case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR: case OP_NOT: case OP_COMPOSITE_CONSTRUCT: case OP_COMPOSITE_EXTRACT: case OP_COMPOSITE_INSERT: case 54: case 56 /* FunctionEnd */:
            copy_rm(&l, i, wc, op);   /* the type id already means uvec2 / the layout is unchanged */
            break;
        default: generic: {
            bool touches = res64;
            if (!is_ptr_or_func_type_op(op) && op >= 19u && !(op >= 71u && op <= 75u)) for (uint32_t k = 1; k < wc && !touches; k++) if (is64(&l, l.w[i + k])) touches = true;
            if (touches) { fail(&l, "unsupported 64-bit integer operation (SPIR-V opcode %u)", op); break; }
            copy_rm(&l, i, wc, op);
        }
        }
        i += wc;
    }
    if (l.failed || l.o.oom) goto done;
    l.o.w[3] = l.next;
    *out = (uint8_t *)l.o.w; *out_len = l.o.n * 4u; l.o.w = NULL; rc = 1;
done:
    free(l.ty); free(l.isint); free(l.width); free(l.o.w);
    return rc;
}
