/* SPDX-License-Identifier: MIT */
#include "vertex_fetch.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t kind; int32_t arg; int64_t off; int32_t bytes; } val_t;   /* kind 1 = pointer into buffer arg `arg` at byte `off`; 2 = value loaded from there */
enum { V_NONE = 0, V_PTR = 1, V_LOADED = 2 };

static uint64_t np2(uint64_t v) { uint64_t p = 1; while (p < v) p <<= 1; return p; }

/* LLVM store size / ABI alignment of an AIR type; false = not modelled. */
static bool type_size(const lagfx_air_type_t *ts, uint32_t nt, uint32_t t, int depth, uint64_t *store, uint64_t *align) {
    if (t >= nt || depth > 6) return false;
    const lagfx_air_type_t *y = &ts[t];
    switch (y->kind) {
    case LAGFX_AIR_TYPE_INTEGER: { uint64_t b = y->num_op ? y->op[0] : 0; if (!b) return false; *store = (b + 7) / 8; *align = np2(*store); return true; }
    case LAGFX_AIR_TYPE_FLOAT: *store = *align = 4; return true;
    case LAGFX_AIR_TYPE_DOUBLE: case LAGFX_AIR_TYPE_POINTER: *store = *align = 8; return true;
    case LAGFX_AIR_TYPE_HALF: *store = *align = 2; return true;
    case LAGFX_AIR_TYPE_VECTOR: {
        uint64_t es, ea;
        if (y->num_op < 2 || !type_size(ts, nt, y->op[1], depth + 1, &es, &ea)) return false;
        *store = es * y->op[0]; *align = np2(*store); return true;   /* datalayout v96:128 etc.: vectors align to their size rounded up to a power of two */
    }
    case LAGFX_AIR_TYPE_ARRAY: {
        uint64_t es, ea;
        if (y->num_op < 2 || !type_size(ts, nt, y->op[1], depth + 1, &es, &ea)) return false;
        *store = y->op[0] * ((es + ea - 1) / ea * ea); *align = ea; return true;
    }
    case LAGFX_AIR_TYPE_STRUCT_ANON: case LAGFX_AIR_TYPE_STRUCT_NAMED: {
        bool packed = y->num_op && y->op[0]; uint64_t off = 0, al = 1;
        for (uint32_t k = 1; k < y->num_op; k++) {
            uint64_t s, a;
            if (!type_size(ts, nt, y->op[k], depth + 1, &s, &a)) return false;
            if (packed) a = 1;
            off = (off + a - 1) / a * a + s; if (a > al) al = a;
        }
        *store = (off + al - 1) / al * al; *align = al; return true;
    }
    default: return false;
    }
}
static bool alloc_size(const lagfx_air_type_t *ts, uint32_t nt, uint32_t t, uint64_t *sz) {
    uint64_t s, a; if (!type_size(ts, nt, t, 0, &s, &a)) return false; *sz = (s + a - 1) / a * a; return true;
}
static bool field_offset(const lagfx_air_type_t *ts, uint32_t nt, uint32_t st, uint32_t field, uint64_t *off) {
    const lagfx_air_type_t *y = &ts[st]; bool packed = y->op[0]; uint64_t o = 0;
    for (uint32_t k = 1; k < y->num_op; k++) {
        uint64_t s, a;
        if (!type_size(ts, nt, y->op[k], 1, &s, &a)) return false;
        if (packed) a = 1;
        o = (o + a - 1) / a * a;
        if (k - 1 == field) { *off = o; return true; }
        o += s;
    }
    return false;
}

static const lagfx_air_metadata_t *md_rec(const lagfx_air_module_t *m, uint32_t id) {
    uint32_t nms = 0, nmd = 0; (void)lagfx_air_module_metadata_strings(m, &nms);
    const lagfx_air_metadata_t *mds = lagfx_air_module_metadata(m, &nmd);
    return (mds && id >= nms && id - nms < nmd) ? &mds[id - nms] : NULL;
}
static int64_t md_int(const lagfx_air_module_t *m, const lagfx_air_metadata_t *v) {
    if (!v || v->kind != LAGFX_AIR_MD_VALUE || v->num_operands < 2) return -1;
    uint32_t base = lagfx_air_module_num_globalvars(m), nf = 0, nc = 0;
    (void)lagfx_air_module_functions(m, &nf); base += nf;
    const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &nc);
    uint32_t id = v->operands[1];
    if (!cs || id < base || id - base >= nc) return -1;
    return cs[id - base].kind == LAGFX_AIR_CONST_INTEGER ? cs[id - base].payload.i64 : cs[id - base].kind == LAGFX_AIR_CONST_NULL ? 0 : -1;
}
/* Metal slot (air.location_index) of the entry argument node whose first operand is `arg`; -1 = not found. */
static int32_t arg_buffer_slot(const lagfx_air_module_t *m, uint32_t arg) {
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named || named->num_operands < 1) return -1;
    const lagfx_air_metadata_t *ep = md_rec(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3 || !ep->operands[2]) return -1;
    const lagfx_air_metadata_t *args = md_rec(m, ep->operands[2] - 1u);
    uint32_t nms = 0; const char *const *strs = lagfx_air_module_metadata_strings(m, &nms);
    if (!args || args->kind != LAGFX_AIR_MD_NODE || !strs) return -1;
    for (uint32_t a = 0; a < args->num_operands; a++) {
        const lagfx_air_metadata_t *n = args->operands[a] ? md_rec(m, args->operands[a] - 1u) : NULL;
        if (!n || n->kind != LAGFX_AIR_MD_NODE || n->num_operands < 2 || !n->operands[0]) continue;
        if (md_int(m, md_rec(m, n->operands[0] - 1u)) != (int64_t)arg) continue;
        bool buf = false; int64_t loc = -1;
        for (uint32_t j = 1; j < n->num_operands; j++) {
            uint32_t o = n->operands[j]; if (!o || o - 1u >= nms) continue;
            const char *s = strs[o - 1u];
            if (!strcmp(s, "air.buffer")) buf = true;
            else if (!strcmp(s, "air.location_index") && j + 1 < n->num_operands && n->operands[j + 1]) loc = md_int(m, md_rec(m, n->operands[j + 1] - 1u));
        }
        return (buf && loc >= 0 && loc < 0x7fff) ? (int32_t)loc : -1;
    }
    return -1;
}

static bool const_int(const lagfx_air_module_t *m, const lagfx_air_function_body_t *body, uint32_t id, uint32_t mod_base, uint32_t mod_end, uint32_t local_base, uint32_t nlocal, int64_t *v) {
    const lagfx_air_constant_t *c = NULL;
    if (id >= mod_base && id < mod_end) { uint32_t n = 0; const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &n); if (id - mod_base < n) c = &cs[id - mod_base]; }
    else if (id >= local_base && id - local_base < nlocal) { uint32_t n = 0; const lagfx_air_constant_t *cs = lagfx_air_function_body_local_constants(body, &n); if (id - local_base < n) c = &cs[id - local_base]; }
    if (!c) return false;
    if (c->kind == LAGFX_AIR_CONST_NULL) { *v = 0; return true; }
    if (c->kind == LAGFX_AIR_CONST_INTEGER) { *v = c->payload.i64; return true; }
    return false;
}

size_t lagfx_air_vertex_fetch_inputs(const lagfx_air_module_t *m, uint32_t fn_idx, lagfx_vfetch_in_t *out, size_t cap) {
    if (!m || !out || !cap) return 0;
    uint32_t nf = 0, nt = 0, ncm = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    const lagfx_air_type_t *ts = lagfx_air_module_types(m, &nt);
    (void)lagfx_air_module_constants(m, &ncm);
    if (!fns || !ts || fn_idx >= nf) return 0;
    uint32_t ng = lagfx_air_module_num_globalvars(m);
    uint32_t cty = fns[fn_idx].type_index;
    if (cty >= nt || ts[cty].kind != LAGFX_AIR_TYPE_FUNCTION || ts[cty].num_op < 2) return 0;
    size_t callee_nargs = ts[cty].num_op - 2u;
    for (uint32_t w = 0; w < nf; w++) {
        if (w == fn_idx || fns[w].is_proto || !fns[w].body_offset) continue;
        const char *nm = lagfx_air_module_string(m, fns[w].name_offset);
        if (!nm || strcmp(nm, "air.vertexFetchFunction")) continue;
        uint32_t wty = fns[w].type_index;
        if (wty >= nt || ts[wty].kind != LAGFX_AIR_TYPE_FUNCTION) continue;
        uint32_t nargs = ts[wty].num_op > 2 ? ts[wty].num_op - 2u : 0u;
        lagfx_air_function_body_t *body = NULL;
        if (lagfx_air_function_body_open(m, w, &body) != LAGFX_OK || !body) continue;
        uint32_t ni = 0, nl = 0;
        const lagfx_air_inst_t *in = lagfx_air_function_body_instructions(body, &ni);
        (void)lagfx_air_function_body_local_constants(body, &nl);
        uint32_t mod_base = ng + nf, mod_end = mod_base + ncm, arg_base = mod_end, local_base = arg_base + nargs, inst_base = local_base + nl;
        val_t *vals = calloc((size_t)inst_base + ni + 2u, sizeof *vals);
        size_t result = 0;
        if (!vals) { lagfx_air_function_body_free(body); return 0; }
        for (uint32_t a = 0; a < nargs; a++) vals[arg_base + a] = (val_t){ V_PTR, (int32_t)a, 0, 0 };   /* only meaningful for pointer args; others never reach a load */
        uint32_t next = inst_base;
        for (uint32_t i = 0; i < ni && !result; i++) {
            const lagfx_air_inst_t *x = &in[i];
            bool produces = true;
            switch (x->code) {
            case LAGFX_AIR_INST_DECLAREBLOCKS: case LAGFX_AIR_INST_STORE: case LAGFX_AIR_INST_STORE_OLD: case LAGFX_AIR_INST_RET:
            case LAGFX_AIR_INST_BR: case LAGFX_AIR_INST_SWITCH: case LAGFX_AIR_INST_UNREACHABLE: produces = false; break;
            case LAGFX_AIR_INST_CALL: {   /* mirrors the translator's value numbering: a call takes a value id iff its explicit function type returns non-void */
                produces = false;
                if (x->num_ops >= 3 && (x->ops[1] & 0x8000)) {
                    uint32_t slot = 2u + ((x->ops[1] >> 17) & 1u);
                    if (x->num_ops > slot && x->ops[slot] < nt && ts[x->ops[slot]].kind == LAGFX_AIR_TYPE_FUNCTION && ts[x->ops[slot]].num_op >= 2) {
                        uint32_t rt = ts[x->ops[slot]].op[1]; produces = rt < nt && ts[rt].kind != LAGFX_AIR_TYPE_VOID;
                    }
                }
                break;
            }
            default: break;
            }
            #define REL(r) ((r) <= next ? next - (uint32_t)(r) : UINT32_MAX)
            if (x->code == LAGFX_AIR_INST_CAST && x->num_ops >= 3 && (x->ops[2] == 11 || x->ops[2] == 12)) {   /* bitcast / addrspacecast: same pointer */
                uint32_t s = REL(x->ops[0]); if (s < inst_base + ni && vals[s].kind == V_PTR) vals[next] = vals[s];
            } else if ((x->code == LAGFX_AIR_INST_GEP || x->code == LAGFX_AIR_INST_GEP_OLD) && x->num_ops >= 4) {
                uint32_t p = REL(x->ops[2]), st = (uint32_t)x->ops[1];
                if (p < inst_base + ni && vals[p].kind == V_PTR && vals[p].arg >= 0 && st < nt) {
                    int64_t off = vals[p].off; bool ok = true; uint32_t cur = st;
                    for (uint32_t k = 3; k < x->num_ops && ok; k++) {
                        uint32_t id = REL(x->ops[k]); int64_t c = 0;
                        bool isc = id != UINT32_MAX && const_int(m, body, id, mod_base, mod_end, local_base, nl, &c);
                        if (k == 3) {   /* element index of the pointee array: the vertex id (not constant) strides by sizeof(S) and adds nothing to the in-record offset */
                            if (isc) { uint64_t sz; if (alloc_size(ts, nt, cur, &sz)) off += c * (int64_t)sz; else ok = false; }
                            continue;
                        }
                        if (!isc || c < 0) { ok = false; break; }
                        const lagfx_air_type_t *y = &ts[cur];
                        if (y->kind == LAGFX_AIR_TYPE_ARRAY || y->kind == LAGFX_AIR_TYPE_VECTOR) {
                            uint64_t sz; if (y->num_op < 2 || !alloc_size(ts, nt, y->op[1], &sz)) { ok = false; break; }
                            off += c * (int64_t)sz; cur = y->op[1];
                        } else if (y->kind == LAGFX_AIR_TYPE_STRUCT_ANON || y->kind == LAGFX_AIR_TYPE_STRUCT_NAMED) {
                            uint64_t fo; if ((uint64_t)c + 1u >= y->num_op + 0u || !field_offset(ts, nt, cur, (uint32_t)c, &fo)) { ok = false; break; }
                            off += (int64_t)fo; cur = y->op[1 + c];
                        } else ok = false;
                    }
                    if (ok && off >= 0) vals[next] = (val_t){ V_PTR, vals[p].arg, off, 0 };
                }
            } else if (x->code == LAGFX_AIR_INST_LOAD && x->num_ops >= 2) {
                uint32_t p = REL(x->ops[0]); uint64_t s, a;
                if (p < inst_base + ni && vals[p].kind == V_PTR && type_size(ts, nt, (uint32_t)x->ops[1], 0, &s, &a))
                    vals[next] = (val_t){ V_LOADED, vals[p].arg, vals[p].off, (int32_t)s };
            } else if (x->code == LAGFX_AIR_INST_CALL && x->num_ops >= 3 && (x->ops[1] & 0x8000)) {
                uint32_t slot = 2u + (uint32_t)((x->ops[1] >> 17) & 1u) + 1u;
                uint32_t callee = x->num_ops > slot ? REL(x->ops[slot]) : UINT32_MAX;
                if (callee != UINT32_MAX && callee >= ng && callee - ng == fn_idx) {
                    for (size_t k = 0; k < cap && k < callee_nargs; k++) out[k] = (lagfx_vfetch_in_t){ -1, -1, -1, 0 };
                    for (size_t k = 0; k < cap && k < callee_nargs && slot + 1 + k < x->num_ops; k++) {
                        uint32_t v = REL(x->ops[slot + 1 + k]);
                        if (v != UINT32_MAX) {   /* an `undef` operand (module or function-local constant) */
                            const lagfx_air_constant_t *uc = NULL;
                            if (v >= mod_base && v < mod_end) { uint32_t n = 0; const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &n); if (v - mod_base < n) uc = &cs[v - mod_base]; }
                            else if (v >= local_base && v - local_base < nl) { uint32_t n = 0; const lagfx_air_constant_t *cs = lagfx_air_function_body_local_constants(body, &n); if (v - local_base < n) uc = &cs[v - local_base]; }
                            if (uc && uc->kind == LAGFX_AIR_CONST_UNDEF) { out[k].unfed = 1; continue; }
                        }
                        if (v < inst_base + ni && vals[v].kind == V_LOADED && vals[v].off <= 0x7fffffff)
                            out[k] = (lagfx_vfetch_in_t){ (int32_t)vals[v].off, vals[v].bytes, arg_buffer_slot(m, (uint32_t)vals[v].arg), 0 };
                    }
                    result = callee_nargs < cap ? callee_nargs : cap;
                }
            }
            #undef REL
            if (produces) next++;
        }
        free(vals);
        lagfx_air_function_body_free(body);
        if (result) return result;
    }
    return 0;
}
