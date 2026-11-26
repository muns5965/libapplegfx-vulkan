/*
 * libapplegfx-vulkan — OpUndef usage report for a translated SPIR-V module
 * src/air2spv/spv_undef_report.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The translator substitutes OpUndef for operands it cannot resolve. An OpUndef that is never consumed is harmless; one that reaches a branch condition / switch selector /
 * OpSelect condition makes the result driver dependent (found in TsdrA2Xhfcu_Idst: `air.any.v4i1(<4 x i1> zeroinitializer)` -> OpUndef -> state machine). This walks the finished
 * module and classifies what every OpUndef reaches.
 *
 * Analysis: a flow-INSENSITIVE MAY-be-undefined taint with one bit per vector lane / struct member (a `poison` base of an insertelement chain, or an undef shuffle lane, is
 * thereby NOT reported as long as only the defined lanes are consumed). Memory is coarse: an OpStore of a value with an undefined lane taints the whole pointer, and an OpLoad of
 * a tainted pointer is wholly tainted (a variable that is undefined on one path only is reported: that is the hazard). Sinks that write memory are only counted for non-Function
 * storage classes. This over-approximates (never misses a use of an OpUndef through the opcodes handled; unknown opcodes propagate "any operand tainted -> whole result tainted").
 */
#include "air2spv/translate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_UNDEF = 1, OP_EXTINST = 12, OP_TYPE_VECTOR = 23, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_CONSTNULL = 46, OP_CONSTCOMPOSITE = 44, OP_FUNCTION = 54, OP_FUNCPARAM = 55,
       OP_FUNCEND = 56, OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_COPYMEM = 63, OP_VECEXTRACTDYN = 77, OP_VECINSERTDYN = 78, OP_VECSHUF = 79, OP_COMPCONSTRUCT = 80,
       OP_COMPEXTRACT = 81, OP_COMPINSERT = 82, OP_IMAGEWRITE = 99, OP_SELECT = 169, OP_CTRLBARRIER = 224, OP_MEMBARRIER = 225, OP_ATOMICSTORE = 228, OP_PHI = 245,
       OP_LOOPMERGE = 246, OP_SELMERGE = 247, OP_LABEL = 248, OP_BRANCH = 249, OP_BRCOND = 250, OP_SWITCH = 251, OP_KILL = 252, OP_RETURN = 253, OP_RETVALUE = 254,
       OP_UNREACHABLE = 255 };
enum { SC_FUNCTION = 7 };

typedef struct {
    uint32_t bound;
    uint32_t *mask;      /* per id: bit k = component k may be undefined (scalars: bit 0; pointers: bit 0 = pointee may be undefined) */
    uint32_t *type_of;   /* result type id of the instruction defining the id */
    uint32_t *ncomp;     /* per type id: vector lanes / struct members (capped at 32), 1 otherwise */
    uint32_t *pn;        /* per pointer id: component count of the stored values (0 = unknown / partial writes: loads are wholly tainted) */
    uint32_t *why;       /* debug: first tainted operand that tainted the id */
    uint32_t *base;      /* per OpAccessChain result: its base pointer */
    uint32_t *sclass;    /* per type id: storage class of an OpTypePointer, else 0xffffffff */
    int64_t  *cval;      /* per id: OpConstant scalar value (low 32 bits sign-extended) */
    uint8_t  *iscval;
    uint8_t  *isund, *used;
} an_t;

static uint32_t full_mask(uint32_t n) { return n >= 32u ? 0xffffffffu : ((1u << n) - 1u); }
static uint32_t m_of(const an_t *a, uint32_t id) { return id < a->bound ? a->mask[id] : 0u; }
static uint32_t nc_of_type(const an_t *a, uint32_t ty) { return ty < a->bound && a->ncomp[ty] ? a->ncomp[ty] : 1u; }
static uint32_t nc_of_val(const an_t *a, uint32_t id) { return id < a->bound ? nc_of_type(a, a->type_of[id]) : 1u; }

static bool no_result(uint32_t op) {
    switch (op) {
        case OP_STORE: case OP_COPYMEM: case OP_IMAGEWRITE: case OP_ATOMICSTORE: case OP_CTRLBARRIER: case OP_MEMBARRIER: case OP_LOOPMERGE: case OP_SELMERGE:
        case OP_BRANCH: case OP_BRCOND: case OP_SWITCH: case OP_RETURN: case OP_RETVALUE: case OP_KILL: case OP_UNREACHABLE: case OP_FUNCEND: case OP_LABEL: return true;
        default: return false;
    }
}

/* generic result mask of an instruction that has [type][result] then id operands in words [lo, hi) */
static uint32_t generic_mask(const an_t *a, const uint32_t *x, uint32_t lo, uint32_t hi, uint32_t rty, bool *any) {
    uint32_t nR = nc_of_type(a, rty), m = 0u; bool t = false, lanewise = nR > 1u;
    for (uint32_t k = lo; k < hi; k++) {
        uint32_t om = m_of(a, x[k]); if (!om) continue;
        t = true;
        if (nc_of_val(a, x[k]) == nR) m |= om; else lanewise = false;
    }
    *any = t;
    if (!t) return 0u;
    return lanewise ? (m ? m : full_mask(nR)) : full_mask(nR);
}

void lagfx_spv_undef_analyze(const void *spv, size_t bytes, lagfx_spv_undef_report_t *out) {
    memset(out, 0, sizeof *out);
    const uint32_t *w = (const uint32_t *)spv; size_t nw = bytes / 4u;
    if (nw < 6u || w[0] != 0x07230203u) return;
    an_t a; memset(&a, 0, sizeof a); a.bound = w[3];
    if (a.bound == 0u || a.bound > (1u << 24)) return;
    uint32_t b = a.bound;
    a.mask = (uint32_t *)calloc(b, 4); a.type_of = (uint32_t *)calloc(b, 4); a.ncomp = (uint32_t *)calloc(b, 4); a.sclass = (uint32_t *)malloc((size_t)b * 4); a.pn = (uint32_t *)calloc(b, 4); a.base = (uint32_t *)calloc(b, 4); a.why = (uint32_t *)calloc(b, 4);
    a.cval = (int64_t *)calloc(b, 8); a.iscval = (uint8_t *)calloc(b, 1); a.isund = (uint8_t *)calloc(b, 1); a.used = (uint8_t *)calloc(b, 1);
    if (!a.mask || !a.type_of || !a.ncomp || !a.sclass || !a.pn || !a.base || !a.why || !a.cval || !a.iscval || !a.isund || !a.used) goto done;
    memset(a.sclass, 0xff, (size_t)b * 4);
    size_t body = 0;
    /* pass A: types, constants, undefs (module scope, definition order) */
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc || i + wc > nw) break;
        const uint32_t *x = w + i;
        if (op == OP_FUNCTION) { body = i; break; }
        if (op == OP_TYPE_VECTOR && wc >= 4 && x[1] < b) a.ncomp[x[1]] = x[3];
        else if (op == OP_TYPE_STRUCT && wc >= 2 && x[1] < b) a.ncomp[x[1]] = wc - 2u > 32u ? 32u : wc - 2u;
        else if (op == OP_TYPE_POINTER && wc >= 4 && x[1] < b) a.sclass[x[1]] = x[2];
        else if (op == OP_CONSTANT && wc >= 4 && x[2] < b) { a.cval[x[2]] = (int64_t)(int32_t)x[3]; a.iscval[x[2]] = 1; a.type_of[x[2]] = x[1]; }
        else if (op == OP_CONSTNULL && wc >= 3 && x[2] < b) { a.cval[x[2]] = 0; a.iscval[x[2]] = 1; a.type_of[x[2]] = x[1]; }
        else if (op == OP_UNDEF && wc >= 3 && x[2] < b) {
            a.isund[x[2]] = 1; a.type_of[x[2]] = x[1]; a.mask[x[2]] = full_mask(nc_of_type(&a, x[1])); out->undef_defs++;
        } else if (op == OP_CONSTCOMPOSITE && wc >= 3 && x[2] < b) {
            a.type_of[x[2]] = x[1]; uint32_t m = 0u, bit = 0u;
            for (uint32_t k = 3; k < wc && bit < 32u; k++, bit++) { if (x[k] < b && a.isund[x[k]]) a.used[x[k]] = 1; if (m_of(&a, x[k])) m |= 1u << bit; }
            a.mask[x[2]] = m;
        } else if (wc >= 3 && x[2] < b && op >= 41u && op <= 52u) a.type_of[x[2]] = x[1];
        i += wc;
    }
    /* pass B: function bodies, iterated to a fixpoint (phis / variables) */
    for (int pass = 0; pass < 8; pass++) {
        bool changed = false;
        for (size_t i = body ? body : nw; i < nw;) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc || i + wc > nw) break;
            const uint32_t *x = w + i;
            if (pass == 0) for (uint32_t k = 1; k < wc; k++) if (x[k] < b && a.isund[x[k]] && op != OP_UNDEF) a.used[x[k]] = 1;   /* over-approximates literal collisions: only an upper bound of `used` */
            if (op == OP_STORE && wc >= 3) {
                uint32_t vm = m_of(&a, x[2]), p = x[1];
                if (p < b) {
                    uint32_t nv = nc_of_val(&a, x[2]);
                    if (a.base[p]) {   /* partial write through an access chain: the base variable becomes wholly (maybe) undefined */
                        uint32_t bp = a.base[p]; if (vm && bp < b && (a.mask[bp] != 0xffffffffu || a.pn[bp])) { a.mask[bp] = 0xffffffffu; a.pn[bp] = 0; changed = true; }
                    } else if (vm) {
                        uint32_t nmk = a.mask[p] | vm; if (a.pn[p] && a.pn[p] != nv) { nmk = 0xffffffffu; a.pn[p] = 0; }
                        else if (!a.mask[p]) a.pn[p] = nv;
                        if (nmk != a.mask[p]) { a.mask[p] = nmk; changed = true; if (!a.why[p]) a.why[p] = x[2]; }
                    }
                }
            } else if ((op == OP_FUNCPARAM || op == OP_VARIABLE) && wc >= 3 && x[2] < b) {
                a.type_of[x[2]] = x[1];
            } else if (!no_result(op) && op != OP_FUNCTION && op != OP_FUNCPARAM && wc >= 3 && x[2] < b) {
                uint32_t res = x[2], rty = x[1], nR = nc_of_type(&a, rty), nm = 0u; bool any = false;
                a.type_of[res] = rty;
                switch (op) {
                    case OP_COMPEXTRACT: {   /* [type][res][obj][literal indices...] */
                        uint32_t om = m_of(&a, x[3]);
                        nm = (wc >= 5 && om & (x[4] < 32u ? 1u << x[4] : 0xffffffffu)) ? full_mask(nR) : 0u;
                        break;
                    }
                    case OP_COMPINSERT: {    /* [type][res][obj][composite][literal indices...] */
                        uint32_t cm = m_of(&a, x[4]), om = m_of(&a, x[3]);
                        if (wc == 6 && x[5] < 32u) nm = (cm & ~(1u << x[5])) | (om ? 1u << x[5] : 0u);
                        else nm = cm | (om ? full_mask(nR) : 0u);
                        break;
                    }
                    case OP_VECINSERTDYN: {  /* [type][res][vector][component][index] */
                        uint32_t vm = m_of(&a, x[3]), cm = m_of(&a, x[4]);
                        if (x[5] < b && a.iscval[x[5]] && a.cval[x[5]] >= 0 && a.cval[x[5]] < 32) nm = (vm & ~(1u << a.cval[x[5]])) | (cm ? 1u << a.cval[x[5]] : 0u);
                        else nm = vm | (cm || m_of(&a, x[5]) ? full_mask(nR) : 0u);
                        break;
                    }
                    case OP_VECEXTRACTDYN: { /* [type][res][vector][index] */
                        uint32_t vm = m_of(&a, x[3]);
                        if (x[4] < b && a.iscval[x[4]] && a.cval[x[4]] >= 0 && a.cval[x[4]] < 32) nm = (vm >> a.cval[x[4]]) & 1u;
                        else nm = (vm || m_of(&a, x[4])) ? 1u : 0u;
                        break;
                    }
                    case OP_VECSHUF: {       /* [type][res][v1][v2][literal lanes...] */
                        uint32_t n1 = nc_of_val(&a, x[3]), m1 = m_of(&a, x[3]), m2 = m_of(&a, x[4]);
                        for (uint32_t k = 5; k < wc && k - 5u < 32u; k++) {
                            uint32_t sel = x[k]; bool u;
                            if (sel == 0xffffffffu) u = true;                               /* undefined lane of the shuffle itself */
                            else if (sel < n1) u = (m1 >> sel) & 1u; else u = ((m2 >> (sel - n1)) & 1u) != 0u;
                            if (u) nm |= 1u << (k - 5u);
                        }
                        break;
                    }
                    case OP_COMPCONSTRUCT: { /* vector of scalars / vectors, or struct / array of members */
                        uint32_t bit = 0u;
                        for (uint32_t k = 3; k < wc && bit < 32u; k++) {
                            uint32_t om = m_of(&a, x[k]), no = nc_of_val(&a, x[k]);
                            if (a.type_of[res] && a.ncomp[rty] && nR >= 1u && no > 1u && nR != wc - 3u) { for (uint32_t l = 0; l < no && bit < 32u; l++, bit++) if ((om >> l) & 1u) nm |= 1u << bit; }
                            else { if (om) nm |= 1u << bit; bit++; }
                        }
                        break;
                    }
                    case OP_LOAD: {
                        uint32_t pm = m_of(&a, x[3]);
                        nm = !pm ? 0u : (x[3] < b && a.pn[x[3]] == nR && nR > 0u ? (pm & full_mask(nR)) : full_mask(nR));
                        break;
                    }
                    case 65: case 66: if (wc >= 4 && x[3] < b) { uint32_t bp = a.base[x[3]] ? a.base[x[3]] : x[3]; a.base[res] = bp; }   /* OpAccessChain / OpInBoundsAccessChain */
                        /* fall through: the chain pointer itself is tainted like its base */
                        nm = m_of(&a, x[3]) ? 1u : 0u; break;
                    case OP_PHI: for (uint32_t k = 3; k + 1 < wc; k += 2) nm |= m_of(&a, x[k]); break;
                    case OP_SELECT: {
                        uint32_t vm = m_of(&a, x[4]) | m_of(&a, x[5]);
                        bool lane = nc_of_val(&a, x[4]) == nR;
                        nm = m_of(&a, x[3]) ? full_mask(nR) : (lane ? vm : (vm ? full_mask(nR) : 0u));
                        break;
                    }
                    case OP_EXTINST: nm = generic_mask(&a, x, 5, wc, rty, &any); break;
                    default: nm = generic_mask(&a, x, 3, wc, rty, &any); break;
                }
                if (nm & ~a.mask[res]) {
                    a.mask[res] |= nm; changed = true;
                    if (!a.why[res]) for (uint32_t k = 3; k < wc; k++) if (op != OP_PHI || (k & 1u)) { if (m_of(&a, x[k])) { a.why[res] = x[k]; break; } }
                }
            }
            i += wc;
        }
        if (!changed) break;
    }
    /* sinks (LAGFX_UNDEF_DEBUG=1 prints the ids) */
    const bool dbg = getenv("LAGFX_UNDEF_DEBUG") != NULL;
    if (dbg) {   /* taint chain of every first sink: id <- operand <- ... <- OpUndef */
        for (size_t i = body ? body : nw; i < nw;) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc || i + wc > nw) break;
            if ((op == OP_SWITCH || op == OP_BRCOND) && m_of(&a, w[i + 1])) {
                fprintf(stderr, "undef-report: chain of control id:"); uint32_t id = w[i + 1];
                for (int hop = 0; hop < 40 && id && id < b; hop++) { fprintf(stderr, " %%%u", id); if (a.isund[id]) { fprintf(stderr, "(OpUndef)"); break; } id = a.why[id]; }
                fprintf(stderr, "\n");
            }
            i += wc;
        }
    }
    for (size_t i = body ? body : nw; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc || i + wc > nw) break;
        const uint32_t *x = w + i;
        switch (op) {
            case OP_BRCOND: if (wc >= 2 && m_of(&a, x[1])) { out->reach_branch++; if (dbg) fprintf(stderr, "undef-report: OpBranchConditional cond %%%u\n", x[1]); } break;
            case OP_SWITCH: if (wc >= 2 && m_of(&a, x[1])) { out->reach_switch++; if (dbg) fprintf(stderr, "undef-report: OpSwitch selector %%%u\n", x[1]); } break;
            case OP_SELECT: if (wc >= 4 && m_of(&a, x[3])) { out->reach_select_cond++; if (dbg) fprintf(stderr, "undef-report: OpSelect %%%u cond %%%u\n", x[2], x[3]); } break;
            case OP_STORE: if (wc >= 3 && m_of(&a, x[2]) && x[1] < b) {
                uint32_t pt = a.type_of[x[1]]; uint32_t sc = pt < b ? a.sclass[pt] : 0xffffffffu;
                if (sc != SC_FUNCTION) { out->reach_store++; if (dbg) fprintf(stderr, "undef-report: OpStore ptr %%%u value %%%u\n", x[1], x[2]); }
            } break;
            case OP_RETVALUE: if (wc >= 2 && m_of(&a, x[1])) { out->reach_store++; if (dbg) fprintf(stderr, "undef-report: OpReturnValue value %%%u\n", x[1]); } break;
            case OP_IMAGEWRITE: if (wc >= 4 && m_of(&a, x[3])) { out->reach_store++; if (dbg) fprintf(stderr, "undef-report: OpImageWrite texel %%%u\n", x[3]); } break;
            default: break;
        }
        i += wc;
    }
    for (uint32_t id = 1; id < b; id++) if (a.isund[id] && a.used[id]) out->undef_used++;
done:
    free(a.mask); free(a.type_of); free(a.ncomp); free(a.sclass); free(a.pn); free(a.base); free(a.why); free(a.cval); free(a.iscval); free(a.isund); free(a.used);
}
