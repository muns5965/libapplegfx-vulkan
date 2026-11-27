/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — DIAGNOSTIC SPIR-V reduction rewrites (see spv_reduce.h). Pure C. */
#include "spv_reduce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPC(w) ((w) & 0xffffu)
#define WCN(w) ((w) >> 16)

typedef struct { uint32_t *w; size_t n, cap; } wb_t;
static int wb_reserve(wb_t *b, size_t add) {
    if (b->n + add <= b->cap) return 0;
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + add) c *= 2;
    uint32_t *p = realloc(b->w, c * 4);
    if (!p) return -1;
    b->w = p, b->cap = c;
    return 0;
}
static int wb_put(wb_t *b, const uint32_t *s, size_t n) {
    if (!n) return 0;
    if (wb_reserve(b, n)) return -1;
    memcpy(b->w + b->n, s, n * 4), b->n += n;
    return 0;
}
static int wb_inst(wb_t *b, uint32_t op, const uint32_t *ops, uint32_t nops) {
    if (wb_reserve(b, nops + 1u)) return -1;
    b->w[b->n++] = ((nops + 1u) << 16) | op;
    if (nops) memcpy(b->w + b->n, ops, nops * 4);
    b->n += nops;
    return 0;
}

/* 0: no result id, 1: result at word 1, 2: result type at word 1 and result id at word 2 */
static int res_pos(uint32_t op) {
    switch (op) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 10: case 14: case 15: case 16: case 17: case 39: case 56: case 62: case 63: case 64: case 71: case 72: case 74:
    case 99: case 218: case 219: case 220: case 221: case 224: case 225: case 228: case 246: case 247: case 249: case 250: case 251: case 252: case 253: case 254: case 255:
    case 256: case 317: case 330: case 332: case 4416: case 5380: case 5632: case 5633: return 0;
    case 7: case 11: case 73: case 248: return 1;
    default: return (op >= 19 && op <= 38) ? 1 : 2;
    }
}

typedef struct {
    const uint32_t *w; size_t n; uint32_t bound;
    size_t *off; uint32_t ni, first_fn;
    uint32_t *op_of, *ty_of, *undef_ord; size_t *defoff;
    uint8_t *ia_var;
    uint32_t nundef, entry_id;
    uint32_t nfn;
    struct { uint32_t id, i0, i1, ret; } fn[LAGFX_RED_MAXFN];
    uint32_t *fn_of_id;   /* id -> function index + 1 */
} mod_t;

static void mod_free(mod_t *m) { free(m->off); free(m->op_of); free(m->ty_of); free(m->undef_ord); free(m->defoff); free(m->ia_var); free(m->fn_of_id); memset(m, 0, sizeof *m); }

static int mod_open(mod_t *m, const uint32_t *w, size_t n) {
    memset(m, 0, sizeof *m);
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t ni = 0;
    for (size_t p = 5; p < n;) { uint32_t wc = WCN(w[p]); if (!wc || p + wc > n) return 0; p += wc; ni++; }
    m->w = w, m->n = n, m->bound = w[3], m->ni = ni, m->first_fn = ni;
    if (m->bound == 0 || m->bound > (1u << 24)) return 0;
    m->off = malloc((size_t)(ni + 1) * sizeof(size_t));
    m->op_of = calloc(m->bound + 1, 4), m->ty_of = calloc(m->bound + 1, 4), m->undef_ord = calloc(m->bound + 1, 4), m->defoff = calloc(m->bound + 1, sizeof(size_t));
    m->ia_var = calloc(m->bound + 1, 1), m->fn_of_id = calloc(m->bound + 1, 4);
    if (!m->off || !m->op_of || !m->ty_of || !m->undef_ord || !m->defoff || !m->ia_var || !m->fn_of_id) { mod_free(m); return -1; }
    uint32_t i = 0;
    int infn = 0;
    for (size_t p = 5; p < n; i++) {
        uint32_t op = OPC(w[p]), wc = WCN(w[p]);
        m->off[i] = p;
        int rp = res_pos(op);
        if (rp == 1 && wc >= 2 && w[p + 1] < m->bound) m->op_of[w[p + 1]] = op, m->defoff[w[p + 1]] = p;
        else if (rp == 2 && wc >= 3 && w[p + 2] < m->bound) m->op_of[w[p + 2]] = op, m->defoff[w[p + 2]] = p, m->ty_of[w[p + 2]] = w[p + 1];
        if (op == 1 && wc >= 3 && w[p + 2] < m->bound) m->undef_ord[w[p + 2]] = ++m->nundef;
        if (op == 71 && wc >= 4 && w[p + 2] == 43 && w[p + 1] < m->bound) m->ia_var[w[p + 1]] = 1;
        if (op == 15 && !m->entry_id && wc >= 3) m->entry_id = w[p + 2];
        if (op == 54) {
            if (m->first_fn == ni) m->first_fn = i;
            if (m->nfn >= LAGFX_RED_MAXFN) { mod_free(m); return 0; }
            m->fn[m->nfn].id = w[p + 2], m->fn[m->nfn].ret = w[p + 1], m->fn[m->nfn].i0 = i, m->fn[m->nfn].i1 = i;
            if (w[p + 2] < m->bound) m->fn_of_id[w[p + 2]] = m->nfn + 1;
            infn = 1;
        } else if (op == 56 && infn) { m->fn[m->nfn].i1 = i, m->nfn++, infn = 0; }
        p += wc;
    }
    m->off[ni] = n;
    if (infn) { mod_free(m); return 0; }
    return 1;
}
static inline uint32_t iop(const mod_t *m, uint32_t i) { return OPC(m->w[m->off[i]]); }

static int null_ok(const mod_t *m, uint32_t ty, int depth) {
    if (ty == 0 || ty >= m->bound || depth > 8) return 0;
    const uint32_t *p = m->w + m->defoff[ty];
    switch (m->op_of[ty]) {
    case 20: case 21: case 22: return 1;
    case 23: case 24: case 28: return null_ok(m, p[2], depth + 1);
    case 30: for (uint32_t k = 2; k < WCN(p[0]); k++) if (!null_ok(m, p[k], depth + 1)) return 0; return 1;
    default: return 0;
    }
}
static uint32_t type_class(const mod_t *m, uint32_t ty) {
    if (ty == 0 || ty >= m->bound) return 0;
    switch (m->op_of[ty]) {
    case 20: case 21: case 22: return LRD_UC_NUM;
    case 23: return type_class(m, m->w[m->defoff[ty] + 2]) == LRD_UC_NUM ? LRD_UC_NUM : LRD_UC_COMPOSITE;
    case 30: return LRD_UC_STRUCT;
    case 32: return LRD_UC_PTR;
    case 24: case 28: case 29: return LRD_UC_COMPOSITE;
    default: return 0;
    }
}

/* compile-time value of an integer index (OpConstant, possibly behind UConvert / SConvert / Bitcast) */
static int const_index(const mod_t *m, uint32_t id, uint64_t *v, int depth) {
    if (id == 0 || id >= m->bound || depth > 4) return 0;
    const uint32_t *p = m->w + m->defoff[id];
    switch (m->op_of[id]) {
    case 43: {
        uint32_t ty = m->ty_of[id];
        if (ty >= m->bound || m->op_of[ty] != 21) return 0;
        uint32_t wd = m->w[m->defoff[ty] + 2];
        if (wd == 64 && WCN(p[0]) >= 5) { *v = (uint64_t)p[3] | ((uint64_t)p[4] << 32); return 1; }
        *v = p[3];
        return wd <= 32;
    }
    case 46: *v = 0; return 1;
    case 113: case 114: case 124: return const_index(m, p[3], v, depth + 1);
    default: return 0;
    }
}

/* Is word `pos` of this instruction an id (as opposed to a literal)? Conservative: unknown positions count as ids (a false hit only makes a row n/a). */
static int is_id_pos(uint32_t op, uint32_t pos, uint32_t wc) {
    (void)wc;
    switch (op) {
    case 246: return pos < 3; case 247: return pos < 2; case 62: return pos < 3; case 61: return pos < 4; case 54: return pos != 3;
    case 43: return pos < 3; case 12: return pos != 4; case 81: return pos < 4; case 82: return pos < 5; case 79: return pos < 5; case 251: return pos < 3;
    case 59: return pos != 3; case 32: return pos != 2; case 25: return pos < 3; case 21: case 22: return 0; case 5: case 6: case 7: case 3: case 4: case 10: case 17: case 14: return 0;
    case 87: case 88: case 91: case 92: case 95: return pos != 5; case 89: case 90: case 93: case 94: case 96: return pos != 6; case 97: return pos != 7; case 98: return pos != 5;
    default: return 1;
    }
}

static int rp_has_ty(uint32_t op) { return res_pos(op) == 2; }

/* OpName of a translator constexpr-sampler variable ("lagfx.cxs.<16 hex>") */
static int is_cxs_name(const uint32_t *p) { return WCN(p[0]) >= 5 && !strncmp((const char *)(p + 2), "lagfx.cxs.", 10); }

/* ---- analysis ---- */
static int kclass(const mod_t *m, uint32_t ty) {
    if (ty >= m->bound) return 7;
    const uint32_t *p = m->w + m->defoff[ty];
    if (m->op_of[ty] == 22) return p[2] == 16 ? 0 : p[2] == 32 ? 1 : p[2] == 64 ? 2 : 7;
    if (m->op_of[ty] == 21) return p[2] == 8 ? 3 : p[2] == 16 ? 4 : p[2] == 32 ? 5 : p[2] == 64 ? 6 : 7;
    return 7;
}

int lagfx_red_analyze(const uint32_t *w, size_t words, lagfx_red_info_t *info) {
    mod_t m;
    memset(info, 0, sizeof *info);
    if (mod_open(&m, w, words) <= 0) return 0;
    info->ok = 1, info->version = w[1], info->bound = w[3], info->ninst = m.ni, info->words = (uint32_t)words, info->nfn = m.nfn;
    info->entry = 0;
    for (uint32_t k = 0; k < m.nfn; k++) if (m.fn[k].id == m.entry_id) info->entry = k;
    uint32_t *kuse = calloc(m.bound + 1, 4);
    if (!kuse) { mod_free(&m); return 0; }
    uint32_t *callee_n = calloc(m.nfn + 1, 4);
    for (uint32_t i = 0; i < m.ni; i++) {
        const uint32_t *p = m.w + m.off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
        if (op == 245) info->nphi++;
        if (op == 252 || op == 4416 || op == 5380) info->nkill++;
        if (i >= m.first_fn && (op == 197 || op == 198 || op == 199 || op == 200)) { info->nbit_total++; uint32_t bt = p[1] < m.bound ? p[1] : 0; if (bt && m.op_of[bt] == 23) bt = m.w[m.defoff[bt] + 2]; if (bt && bt < m.bound && m.op_of[bt] == 21 && m.w[m.defoff[bt] + 2] < 32) info->nbit_small++; }
        if (i >= m.first_fn && op == 61 && p[1] < m.bound && m.op_of[p[1]] == 30) info->nstruct_load++;
        if (i >= m.first_fn && op == 62 && p[2] < m.bound && m.ty_of[p[2]] < m.bound && m.op_of[m.ty_of[p[2]]] == 30) info->nstruct_store++;
        if (op == 63 || op == 64) info->ncopymem++;
        if (op == 59 && wc >= 4 && p[3] == 6 && p[1] < m.bound && m.op_of[p[1]] == 32) { uint32_t pt = m.w[m.defoff[p[1]] + 3]; if (pt < m.bound && m.op_of[pt] == 28) { uint32_t et = m.w[m.defoff[pt] + 2], ln = m.w[m.defoff[pt] + 3]; if (et < m.bound && m.op_of[et] == 23 && ln < m.bound && m.op_of[ln] == 43) { uint32_t n = m.w[m.defoff[ln] + 3]; info->nprivate_vecarr++; if (n > info->nprivate_arr_max) info->nprivate_arr_max = n; } } }
        if (op == 59 && wc >= 4) { info->nvar_total++; if (p[3] == 7) info->nfnvar++; }
        if (op == 62) info->nstore++;
        if (op == 61) info->nload++;
        if (op == 248) info->nlabel++;
        if (op == 115) info->nfconvert++;
        if (op == 67) info->nptrchain++;
        if (op == 28 && wc >= 4 && p[3] < m.bound && m.op_of[p[3]] == 43 && m.w[m.defoff[p[3]] + 3] >= 1024) info->nssbo_big++;
        if (i >= m.first_fn && rp_has_ty(op) && wc >= 3 && p[1] < m.bound && m.op_of[p[1]] == 21 && m.w[m.defoff[p[1]] + 2] == 64) {
            uint32_t q = 0;
            while (q < info->i64_total && q < 10 && info->i64_op[q].op != op) q++;
            if (q < 10) { info->i64_op[q].op = op, info->i64_op[q].n++; if (q == info->i64_total) info->i64_total++; }
        }
        if (op == 17 && (p[1] == 6022 || p[1] == 5357 || p[1] == 5358)) info->ncoopmat_cap++;
        if (op == 59 && wc >= 4 && m.ia_var[p[2]]) info->nia++;
        if (op == 1 && wc >= 3 && info->nundef < LAGFX_RED_MAXUNDEF && i < m.first_fn) {
            uint32_t k = info->nundef++;
            info->undef[k].id = p[2], info->undef[k].type = p[1], info->undef[k].cls = type_class(&m, p[1]);
        }
        if (i < m.first_fn) {
            if (op == 43 && wc >= 4 && p[2] < m.bound) { int c = kclass(&m, p[1]); info->k[c].unique++; }
            else if (op == 41 || op == 42) info->kbool++;
            else if (op == 44) info->kcomposite++;
            else if (op >= 48 && op <= 52) info->kspec++;
            else if (op == 46) info->knull++;
            else if (op == 1) info->kundef_global++;
        }
        if (i >= m.first_fn) {
            if (op == 251) info->nswitch++;
            if (op == 246) info->nloop++;
            if (op == 247) info->nselmerge++;
            if (op == 249 || op == 250) info->nbranch++;
            for (uint32_t q = 1; q < wc; q++) if (p[q] < m.bound && is_id_pos(op, q, wc) && (m.op_of[p[q]] == 43 || m.op_of[p[q]] == 44 || m.op_of[p[q]] == 41 || m.op_of[p[q]] == 42 || m.op_of[p[q]] == 46)) kuse[p[q]]++;
        }
    }
    for (uint32_t i = 0; i < m.first_fn; i++) {   /* Location inputs, the big array */
        const uint32_t *p = m.w + m.off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
        if (op == 59 && wc >= 4 && p[3] == 1 && info->nin < 32) {
            uint32_t id = p[2], loc = ~0u, comp = 0, flat = 0, bi = 0, bival = 0;
            for (uint32_t j = 0; j < m.first_fn; j++) { const uint32_t *d = m.w + m.off[j]; if (OPC(d[0]) == 71 && d[1] == id) { if (d[2] == 30) loc = d[3]; else if (d[2] == 24) comp = d[3]; else if (d[2] == 14) flat = 1; else if (d[2] == 11) bi = 1, bival = d[3]; } }
            if (bi && info->nblt < 8) { info->blt[info->nblt].id = id, info->blt[info->nblt].builtin = bival; info->nblt++; }
            if (loc != ~0u && !bi) {
                uint32_t pt = m.w[m.defoff[p[1]] + 3], nc = 1, base = pt;
                if (pt < m.bound && m.op_of[pt] == 23) nc = m.w[m.defoff[pt] + 3], base = m.w[m.defoff[pt] + 2];
                uint32_t k = info->nin++;
                info->in[k].id = id, info->in[k].loc = loc, info->in[k].comp = comp, info->in[k].ncomp = nc, info->in[k].flat = flat;
                if (base < m.bound && (m.op_of[base] == 21 || m.op_of[base] == 22)) info->in[k].width = m.w[m.defoff[base] + 2], info->in[k].kind = m.op_of[base] == 22 ? 0 : 1;
            }
        }
        if (op == 28 && wc >= 4 && !info->ssbo_arr_len && p[3] < m.bound && m.op_of[p[3]] == 43 && m.w[m.defoff[p[3]] + 3] >= 1024) {
            info->ssbo_arr_len = m.w[m.defoff[p[3]] + 3];
            for (uint32_t j = 0; j < m.first_fn; j++) {
                const uint32_t *d = m.w + m.off[j];
                if (OPC(d[0]) == 71 && d[1] == p[1] && d[2] == 6) info->ssbo_arr_stride = d[3];
                if (OPC(d[0]) == 30) for (uint32_t q = 2; q < WCN(d[0]); q++) if (d[q] == p[1])
                    for (uint32_t t = 0; t < m.first_fn; t++) { const uint32_t *e = m.w + m.off[t]; if (OPC(e[0]) == 72 && e[1] == d[1] && e[3] == 35 && e[2] == q - 2 && info->ssbo_noff < 8) info->ssbo_offsets[info->ssbo_noff++] = e[4]; }
            }
        }
    }
    for (uint32_t i = 0; i < m.ni && i < m.first_fn; i++) {
        const uint32_t *p = m.w + m.off[i];
        if (OPC(p[0]) == 43 && WCN(p[0]) >= 4) { info->k[kclass(&m, p[1])].uses += kuse[p[2]]; info->kuses_total += kuse[p[2]]; }
    }
    for (uint32_t i = 0; i < m.first_fn; i++) {   /* translator constexpr samplers: OpName "lagfx.cxs.<hex>" */
        const uint32_t *p = m.w + m.off[i];
        if (OPC(p[0]) != 5 || !is_cxs_name(p) || p[1] >= m.bound || m.op_of[p[1]] != 59 || info->ncxs >= 8) continue;
        uint32_t k = info->ncxs++, nuse = 0, bnd = ~0u;
        for (uint32_t j = 0; j < m.first_fn; j++) { const uint32_t *d = m.w + m.off[j]; if (OPC(d[0]) == 71 && d[1] == p[1] && d[2] == 33) bnd = d[3]; }
        for (uint32_t j = m.first_fn; j < m.ni; j++) { const uint32_t *d = m.w + m.off[j]; if (OPC(d[0]) == 61 && WCN(d[0]) >= 4 && d[3] == p[1]) nuse++; }
        info->cxs[k].id = p[1], info->cxs[k].binding = bnd, info->cxs[k].nuse = nuse;
    }
    /* distinct (type,value) among OpConstant: quadratic over a bounded prefix, exact below 8000 constants */
    {
        uint32_t cap = 0, *ids = malloc(sizeof(uint32_t) * (m.first_fn + 1));
        if (ids) {
            for (uint32_t i = 0; i < m.first_fn; i++) if (iop(&m, i) == 43) ids[cap++] = i;
            if (cap > 8000) cap = 8000;
            for (uint32_t a = 0; a < cap; a++) {
                const uint32_t *pa = m.w + m.off[ids[a]];
                int dup = 0;
                for (uint32_t b = 0; b < a && !dup; b++) {
                    const uint32_t *pb = m.w + m.off[ids[b]];
                    if (pa[0] == pb[0] && pa[1] == pb[1] && !memcmp(pa + 3, pb + 3, (WCN(pa[0]) - 3) * 4)) dup = 1;
                }
                if (!dup) info->k[kclass(&m, pa[1])].distinct_values++;
            }
            free(ids);
        }
    }
    for (uint32_t k = 0; k < m.nfn; k++) {
        info->fn[k].id = m.fn[k].id, info->fn[k].ninst = m.fn[k].i1 - m.fn[k].i0 + 1;
        info->fn[k].ret_op = m.fn[k].ret < m.bound ? m.op_of[m.fn[k].ret] : 0;
        for (uint32_t i = m.fn[k].i0; i <= m.fn[k].i1; i++) {
            const uint32_t *p = m.w + m.off[i];
            uint32_t op = OPC(p[0]);
            if (op == 248) info->fn[k].nblocks++;
            else if ((op >= 87 && op <= 97)) info->fn[k].nsamp++;
            else if (op == 98) info->fn[k].nread++;
            else if (op >= 207 && op <= 215) info->fn[k].nderiv++;
            else if (op == 246) info->fn[k].nloop++;
            else if (op == 251) info->fn[k].nswitch++;
            if (op == 77 || op == 78) {
                uint32_t vty = op == 77 ? m.ty_of[p[3]] : p[1], idx = op == 77 ? p[4] : p[5], q = 0, nc = 0, wd = 0, kd = 0;
                uint64_t cv;
                if (vty && vty < m.bound && m.op_of[vty] == 23) { const uint32_t *tp = m.w + m.defoff[vty]; nc = tp[3]; uint32_t et = tp[2]; if (et < m.bound) { wd = m.w[m.defoff[et] + 2]; kd = m.op_of[et] == 22 ? 0 : 1; } }
                info->dyn_total++, info->dyn_fn[k]++;
                if (const_index(&m, idx, &cv, 0)) info->dyn_const++; else info->dyn_truly++;
                if (const_index(&m, idx, &cv, 0)) info->dyn_idx_const++; else if (idx < m.bound && m.op_of[idx] == 61) info->dyn_idx_load++; else info->dyn_idx_other++;
                while (q < 8 && info->dyn_vec[q].n && !(info->dyn_vec[q].ncomp == nc && info->dyn_vec[q].width == wd && info->dyn_vec[q].kind == kd)) q++;
                if (q < 8) { info->dyn_vec[q].ncomp = nc, info->dyn_vec[q].width = wd, info->dyn_vec[q].kind = kd, info->dyn_vec[q].n++; }
            }
            else if (op == 57) { info->fn[k].ncall++; if (p[3] < m.bound && m.fn_of_id[p[3]]) callee_n[m.fn_of_id[p[3]] - 1]++; }
        }
    }
    info->entry_blocks = info->fn[info->entry].nblocks;
    for (uint32_t k = 0; k < m.nfn; k++) if (k != info->entry && !callee_n[k]) info->nfn_uncalled++;
    for (uint32_t u = 0; u < info->nundef; u++) {
        uint32_t uid = info->undef[u].id, n = 0;
        for (uint32_t i = 0; i < m.ni; i++) {
            const uint32_t *p = m.w + m.off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (op == 1) continue;
            for (uint32_t q = 1; q < wc; q++) if (p[q] == uid && is_id_pos(op, q, wc)) n++;
        }
        info->undef[u].nuse = n;
    }
    free(kuse), free(callee_n);
    mod_free(&m);
    return 1;
}

uint32_t lagfx_red_count_inst(const uint32_t *w, size_t words) {
    uint32_t n = 0;
    if (words < 5) return 0;
    for (size_t p = 5; p < words; n++) { uint32_t wc = WCN(w[p]); if (!wc) return n; p += wc; }
    return n;
}

/* ---- apply ---- */
typedef struct {
    mod_t m;
    const lagfx_red_recipe_t *r;
    wb_t gout, gtail, body;
    uint8_t *dead, *keep, *seen_hdr, *refd;
    uint32_t *nullmap, *hdr_merge, *cont_hdr;
    uint32_t next, f32zero, f32type, booltype, btrue, bfalse;
    uint32_t cut_idx;
    int in_body;
    uint32_t c_stub, c_undef, c_use, c_sw, c_swc, c_lp, c_smp, c_lod0, c_deriv, c_read, c_ia, c_pruned, c_min;
    uint8_t dynsel[LAGFX_RED_MAXFN], *arr_other, *arr_last, *arr_ok;
    uint32_t *lenc;
    struct { uint32_t ty; uint64_t val; uint32_t id; } ic[64];
    uint32_t nic, c_dynconst, c_dynsel, c_ssbo, c_input, c_cxs, c_minload, c_wire;
    uint8_t *insel, *cxs_var;
    uint32_t cxs_to;
    char why[160];
} ax_t;

static uint32_t newid(ax_t *a) { return a->next++; }
static wb_t *dst(ax_t *a) { return a->in_body ? &a->gtail : &a->gout; }

static uint32_t nullof(ax_t *a, uint32_t ty) {
    if (ty == 0 || ty >= a->m.bound) return 0;
    if (a->nullmap[ty]) return a->nullmap[ty];
    if (!null_ok(&a->m, ty, 0)) return 0;
    uint32_t id = newid(a), ops[2] = { ty, id };
    if (wb_inst(dst(a), 46, ops, 2)) return 0;
    a->nullmap[ty] = id;
    return id;
}
static uint32_t boolconst(ax_t *a, int val) {
    uint32_t *slot = val ? &a->btrue : &a->bfalse;
    if (*slot) return *slot;
    if (!a->booltype) { uint32_t id = newid(a); if (wb_inst(dst(a), 20, &id, 1)) return 0; a->booltype = id; }
    uint32_t id = newid(a), ops[2] = { a->booltype, id };
    if (wb_inst(dst(a), val ? 41 : 42, ops, 2)) return 0;
    *slot = id;
    return id;
}
static uint32_t floatzero(ax_t *a) {
    if (a->f32zero) return a->f32zero;
    if (!a->f32type) return 0;
    uint32_t id = newid(a), ops[3] = { a->f32type, id, 0 };
    if (wb_inst(dst(a), 43, ops, 3)) return 0;
    a->f32zero = id;
    return id;
}

static uint32_t intconst(ax_t *a, uint32_t ty, uint64_t v) {
    for (uint32_t i = 0; i < a->nic; i++) if (a->ic[i].ty == ty && a->ic[i].val == v) return a->ic[i].id;
    uint32_t wd = a->m.w[a->m.defoff[ty] + 2], id = newid(a), ops[4] = { ty, id, (uint32_t)v, (uint32_t)(v >> 32) };
    if (a->nic >= 64 || (wd != 32 && wd != 64)) return 0;
    if (wb_inst(dst(a), 43, ops, wd == 64 ? 4 : 3)) return 0;
    a->ic[a->nic].ty = ty, a->ic[a->nic].val = v, a->ic[a->nic].id = id, a->nic++;
    return id;
}
static uint32_t booltype_id(ax_t *a) {
    if (!a->booltype) { uint32_t id = newid(a); if (wb_inst(dst(a), 20, &id, 1)) return 0; a->booltype = id; }
    return a->booltype;
}

/* OpVectorExtractDynamic / InsertDynamic -> OpCompositeExtract / Insert (constant index) or an OpSelect chain. 1 = rewritten, 0 = left as is, -1 = error */
static int emit_dyn(ax_t *a, const uint32_t *p, int allow_select) {
    const mod_t *m = &a->m;
    uint32_t op = OPC(p[0]), vec = p[3], idx = op == 77 ? p[4] : p[5], vty = op == 77 ? (vec < m->bound ? m->ty_of[vec] : 0) : p[1];
    if (!vty || vty >= m->bound || m->op_of[vty] != 23) return 0;
    uint32_t nc = m->w[m->defoff[vty] + 3], ety = m->w[m->defoff[vty] + 2];
    uint64_t cv;
    if (const_index(m, idx, &cv, 0)) {
        if (cv >= nc) return 0;
        if (op == 77) { uint32_t o[4] = { p[1], p[2], vec, (uint32_t)cv }; return wb_inst(&a->body, 81, o, 4) ? -1 : 1; }
        uint32_t o[5] = { p[1], p[2], p[4], vec, (uint32_t)cv };
        return wb_inst(&a->body, 82, o, 5) ? -1 : 1;
    }
    if (!allow_select) return 0;
    uint32_t ity = idx < m->bound ? m->ty_of[idx] : 0, bt = booltype_id(a);
    if (!ity || ity >= m->bound || m->op_of[ity] != 21 || !bt || nc > 4) return 0;
    uint32_t sel[4], elem[4];
    for (uint32_t k = 0; k < nc; k++) {
        uint32_t kc = intconst(a, ity, k), c = newid(a), e = newid(a);
        if (!kc) return 0;
        uint32_t oc[4] = { bt, c, idx, kc };
        if (wb_inst(&a->body, 170, oc, 4)) return -1;
        uint32_t oe[4] = { ety, e, vec, k };
        if (wb_inst(&a->body, 81, oe, 4)) return -1;
        sel[k] = c, elem[k] = e;
    }
    if (op == 77) {
        uint32_t cur = elem[nc - 1];
        for (uint32_t k = nc - 1; k-- > 0;) {
            uint32_t r = k == 0 ? p[2] : newid(a), o[5] = { ety, r, sel[k], elem[k], cur };
            if (wb_inst(&a->body, 169, o, 5)) return -1;
            cur = r;
        }
        if (nc == 1) { uint32_t o[3] = { ety, p[2], elem[0] }; return wb_inst(&a->body, 83, o, 3) ? -1 : 1; }
        return 1;
    }
    uint32_t outc[4];
    for (uint32_t k = 0; k < nc; k++) { uint32_t r = newid(a), o[5] = { ety, r, sel[k], p[4], elem[k] }; if (wb_inst(&a->body, 169, o, 5)) return -1; outc[k] = r; }
    uint32_t cn[8] = { p[1], p[2], outc[0], outc[1], outc[2], outc[3] };
    return wb_inst(&a->body, 80, cn, 2 + nc) ? -1 : 1;
}

static int undef_wanted(const ax_t *a, uint32_t ord1, uint32_t ty) {
    uint32_t cls = type_class(&a->m, ty);
    if (cls & a->r->undef_class) return 1;
    return ord1 >= 1 && ord1 <= 32 && ((a->r->undef_sel >> (ord1 - 1)) & 1u);
}

/* id operand span of the instructions a use class applies to */
static uint32_t use_span(uint32_t op, uint32_t wc, uint32_t *lo, uint32_t *hi) {
    switch (op) {
    case 79: *lo = 3, *hi = 4; return LRD_UU_SHUF;
    case 78: *lo = 3, *hi = 5; return LRD_UU_INSDYN;
    case 77: *lo = 3, *hi = 4; return LRD_UU_EXTDYN;
    case 44: *lo = 3, *hi = wc - 1; return LRD_UU_CCOMP;
    case 80: *lo = 3, *hi = wc - 1; return LRD_UU_CCONS;
    case 82: *lo = 3, *hi = 4; return LRD_UU_CINS;
    case 62: *lo = 2, *hi = 2; return LRD_UU_STORE;
    case 57: *lo = 4, *hi = wc - 1; return LRD_UU_CALLARG;
    default: return 0;
    }
}

/* copies the instruction at p, replacing OpUndef operands (when the use class is selected); *did = number replaced */
static int emit_with_use_class(ax_t *a, wb_t *to, const uint32_t *p, uint32_t *did) {
    uint32_t op = OPC(p[0]), wc = WCN(p[0]), lo, hi;
    uint32_t cls = use_span(op, wc, &lo, &hi);
    *did = 0;
    if (!(cls & a->r->use_class) || hi >= wc || lo > hi) return wb_put(to, p, wc);
    int any = 0;
    for (uint32_t q = lo; q <= hi; q++) if (p[q] < a->m.bound && a->m.op_of[p[q]] == 1) any = 1;
    if (!any) return wb_put(to, p, wc);
    uint32_t *t = malloc((size_t)wc * 4);
    if (!t) return -1;
    memcpy(t, p, (size_t)wc * 4);
    for (uint32_t q = lo; q <= hi; q++) {
        uint32_t x = p[q];
        if (x < a->m.bound && a->m.op_of[x] == 1) { uint32_t nn = nullof(a, a->m.ty_of[x]); if (nn) t[q] = nn, (*did)++; }
    }
    int rc = wb_put(to, t, wc);
    free(t);
    return rc;
}

static void mark_dead_range(ax_t *a, uint32_t i0, uint32_t i1) {
    for (uint32_t i = i0; i <= i1 && i < a->m.ni; i++) {
        const uint32_t *p = a->m.w + a->m.off[i];
        int rp = res_pos(OPC(p[0]));
        if (rp && WCN(p[0]) > (uint32_t)rp && p[rp] < a->m.bound) a->dead[p[rp]] = 1;
    }
}

static uint32_t pick_switch_target(const mod_t *m, const uint32_t *p, int frequent) {
    uint32_t wc = WCN(p[0]), def = p[2];
    if (!frequent || wc <= 3) return def;
    uint32_t sty = p[1] < m->bound ? m->ty_of[p[1]] : 0, lw = 1;
    if (sty && sty < m->bound && m->op_of[sty] == 21 && m->w[m->defoff[sty] + 2] == 64) lw = 2;
    uint32_t best = def, bestn = 0;
    for (uint32_t q = 3; q + lw < wc; q += lw + 1) {
        uint32_t t = p[q + lw], n = 0;
        for (uint32_t r = 3; r + lw < wc; r += lw + 1) if (p[r + lw] == t) n++;
        if (n > bestn) bestn = n, best = t;
    }
    return best;
}

/* cut plan for the entry function: the instruction index of the first label at or after block `nblocks` (any nesting depth), or 0. The labels that the kept part still branches
 * to / declares as merge / continue targets stay as one-instruction blocks (OpReturn, or the back-edge for a continue target) so every kept reference resolves. */
static uint32_t plan_cut(ax_t *a, uint32_t k, uint32_t nblocks) {
    const mod_t *m = &a->m;
    uint32_t blocks = 0, cut = 0;
    for (uint32_t i = m->fn[k].i0; i <= m->fn[k].i1 && !cut; i++) if (iop(m, i) == 248) { if (blocks >= nblocks && blocks > 0) cut = i; blocks++; }
    if (!cut) return 0;
    uint32_t lab = 0;
    for (uint32_t i = m->fn[k].i0; i < cut; i++) {
        const uint32_t *p = m->w + m->off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
#define REF(x) do { if ((x) < m->bound) a->refd[x] = 1; } while (0)
        if (op == 248) lab = p[1];
        else if (op == 249) REF(p[1]);
        else if (op == 250) { REF(p[2]); REF(p[3]); }
        else if (op == 247) REF(p[1]);
        else if (op == 246) { REF(p[1]); REF(p[2]); if (p[2] < m->bound) a->cont_hdr[p[2]] = lab; }
        else if (op == 251) {
            uint32_t sty = p[1] < m->bound ? m->ty_of[p[1]] : 0, lw = (sty && sty < m->bound && m->op_of[sty] == 21 && m->w[m->defoff[sty] + 2] == 64) ? 2u : 1u;
            REF(p[2]);
            for (uint32_t q = 3; q + lw < wc; q += lw + 1) REF(p[q + lw]);
        }
#undef REF
    }
    return cut;
}

static int emit_cut_tail(ax_t *a, uint32_t k, uint32_t cut) {
    const mod_t *m = &a->m;
    uint32_t i1 = m->fn[k].i1;
    mark_dead_range(a, cut, i1 - 1);
    for (uint32_t j = cut; j < i1; j++) {
        const uint32_t *p = m->w + m->off[j];
        if (OPC(p[0]) != 248 || !(j == cut || a->refd[p[1]])) continue;
        a->dead[p[1]] = 0;
        if (wb_put(&a->body, p, WCN(p[0]))) return -1;
        if (a->cont_hdr[p[1]]) { uint32_t h = a->cont_hdr[p[1]]; if (wb_inst(&a->body, 249, &h, 1)) return -1; }
        else if (wb_inst(&a->body, 253, NULL, 0)) return -1;
    }
    return wb_inst(&a->body, 56, NULL, 0) ? -1 : 1;
}

static int emit_stub(ax_t *a, uint32_t k) {
    const mod_t *m = &a->m;
    uint32_t i0 = m->fn[k].i0, i1 = m->fn[k].i1, i = i0;
    const uint32_t *pf = m->w + m->off[i0];
    uint32_t rty = pf[1], rop = rty < m->bound ? m->op_of[rty] : 0, nn = 0;
    if (rop != 19) { nn = nullof(a, rty); if (!nn) return 0; }
    if (wb_put(&a->body, pf, WCN(pf[0]))) return -1;
    for (i = i0 + 1; i < i1 && iop(m, i) == 55; i++) if (wb_put(&a->body, m->w + m->off[i], WCN(m->w[m->off[i]]))) return -1;
    if (iop(m, i) != 248) return 0;
    if (wb_put(&a->body, m->w + m->off[i], WCN(m->w[m->off[i]]))) return -1;
    if (rop == 19) { if (wb_inst(&a->body, 253, NULL, 0)) return -1; }
    else if (wb_inst(&a->body, 254, &nn, 1)) return -1;
    if (wb_inst(&a->body, 56, NULL, 0)) return -1;
    mark_dead_range(a, i + 1, i1 - 1);
    a->c_stub++;
    return 1;
}

/* 0 = not an interface variable this code selects, 1 = Location input, 2 = BuiltIn input */
static int input_kind(const mod_t *m, uint32_t var) {
    int loc = 0, bi = 0;
    for (uint32_t j = 0; j < m->first_fn; j++) { const uint32_t *d = m->w + m->off[j]; if (OPC(d[0]) == 71 && d[1] == var) { if (d[2] == 30) loc = 1; if (d[2] == 11) bi = 1; } }
    return bi ? 2 : loc;
}
/* float32 scalar (1) or vector: *nc = component count, *sty = the scalar type; 0 = anything else */
static int f32_shape(const mod_t *m, uint32_t ty, uint32_t *nc, uint32_t *sty) {
    if (ty == 0 || ty >= m->bound) return 0;
    const uint32_t *p = m->w + m->defoff[ty];
    if (m->op_of[ty] == 22 && p[2] == 32) { *nc = 1, *sty = ty; return 1; }
    if (m->op_of[ty] == 23 && p[2] < m->bound && m->op_of[p[2]] == 22 && m->w[m->defoff[p[2]] + 2] == 32 && p[3] >= 2 && p[3] <= 4) { *nc = p[3], *sty = p[2]; return 1; }
    return 0;
}

static int emit_min_entry(ax_t *a, uint32_t k) {
    const mod_t *m = &a->m;
    uint32_t i0 = m->fn[k].i0, i1 = m->fn[k].i1, i = i0 + 1;
    const uint32_t *pf = m->w + m->off[i0];
    if (wb_put(&a->body, pf, WCN(pf[0]))) return -1;
    while (i < i1 && iop(m, i) != 248) { if (wb_put(&a->body, m->w + m->off[i], WCN(m->w[m->off[i]]))) return -1; i++; }
    if (iop(m, i) != 248) return 0;
    if (wb_put(&a->body, m->w + m->off[i], WCN(m->w[m->off[i]]))) return -1;
    uint32_t flat[32], nf = 0;
    if (a->r->bits & (LRD_MIN_LOADS | LRD_MIN_WIRE)) {   /* load the selected inputs (and, when wiring, spread their float32 lanes into flat[]) */
        uint32_t lo = 0, bo = 0;
        for (uint32_t g = 0; g < m->first_fn; g++) {
            const uint32_t *p = m->w + m->off[g];
            if (OPC(p[0]) != 59 || WCN(p[0]) < 4 || p[3] != 1 || a->dead[p[2]]) continue;
            int kd = input_kind(m, p[2]);
            if (!kd) continue;
            uint32_t ord = kd == 1 ? lo++ : bo++, sel = kd == 1 ? a->r->input_sel : a->r->blt_sel;
            if (ord >= 32 || !((sel >> ord) & 1u)) continue;
            uint32_t pt = m->w[m->defoff[p[1]] + 3], id = newid(a), ops[3] = { pt, id, p[2] };
            if (wb_inst(&a->body, 61, ops, 3)) return -1;
            a->c_minload++;
            uint32_t nc, sty;
            if (!(a->r->bits & LRD_MIN_WIRE) || !f32_shape(m, pt, &nc, &sty)) continue;
            if (nc == 1) { if (nf < 32) flat[nf++] = id; continue; }
            for (uint32_t c = 0; c < nc && nf < 32; c++) { uint32_t e = newid(a), eo[4] = { sty, e, id, c }; if (wb_inst(&a->body, 81, eo, 4)) return -1; flat[nf++] = e; }
        }
    }
    uint32_t oj = 0;
    for (uint32_t g = 0; g < m->first_fn; g++) {
        const uint32_t *p = m->w + m->off[g];
        if (OPC(p[0]) != 59 || p[3] != 3 /* Output */ || a->dead[p[2]]) continue;
        uint32_t pt = m->w[m->defoff[p[1]] + 3], nc, sty, val = 0;
        if (nf && (a->r->bits & LRD_MIN_WIRE) && f32_shape(m, pt, &nc, &sty)) {   /* sum of the chunks of nc loaded lanes (lane (chunk*nc+i+oj) mod nf) */
            uint32_t nch = (nf + nc - 1) / nc;
            for (uint32_t c = 0; c < nch; c++) {
                uint32_t ch;
                if (nc == 1) ch = flat[(c + oj) % nf];
                else {
                    uint32_t co[6] = { pt, newid(a) };
                    for (uint32_t t = 0; t < nc; t++) co[2 + t] = flat[(c * nc + t + oj) % nf];
                    if (wb_inst(&a->body, 80, co, 2 + nc)) return -1;
                    ch = co[1];
                }
                if (c == 0) val = ch;
                else { uint32_t fo[4] = { pt, newid(a), val, ch }; if (wb_inst(&a->body, 129, fo, 4)) return -1; val = fo[1]; }
            }
            a->c_wire++;
        } else val = nullof(a, pt);
        if (!val) return 0;
        uint32_t ops[2] = { p[2], val };
        if (wb_inst(&a->body, 62, ops, 2)) return -1;
        oj++;
    }
    if (wb_inst(&a->body, 253, NULL, 0) || wb_inst(&a->body, 56, NULL, 0)) return -1;
    mark_dead_range(a, i + 1, i1 - 1);
    a->c_min++;
    return 1;
}

static int emit_function(ax_t *a, uint32_t k) {
    const mod_t *m = &a->m;
    const lagfx_red_recipe_t *r = a->r;
    const int entry = m->fn[k].id == m->entry_id;
    const uint32_t i0 = m->fn[k].i0, i1 = m->fn[k].i1;
    const uint32_t B = r->bits;
    const int sw = entry ? !!(B & LRD_SW_E) : !!(B & LRD_SW_H), swc = entry ? !!(B & LRD_SWC_E) : !!(B & LRD_SWC_H);
    const int lp = entry ? !!(B & LRD_LP_E) : !!(B & LRD_LP_H), smpn = entry ? !!(B & LRD_SMP_NULL_E) : !!(B & LRD_SMP_NULL_H);
    if (!a->keep[k]) { mark_dead_range(a, i0, i1); a->c_pruned++; return 1; }
    if (entry && (B & LRD_MIN_ENTRY)) return emit_min_entry(a, k);
    if (!entry && r->stub[k]) return emit_stub(a, k);
    if (lp) for (uint32_t i = i0, lab = 0; i <= i1; i++) {   /* header -> merge map */
        const uint32_t *p = m->w + m->off[i];
        if (OPC(p[0]) == 248) lab = p[1];
        else if (OPC(p[0]) == 246 && lab < m->bound) a->hdr_merge[lab] = p[1];
    }
    for (uint32_t i = i0; i <= i1; i++) {
        const uint32_t *p = m->w + m->off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
        if (entry && a->cut_idx && i == a->cut_idx) return emit_cut_tail(a, k, i);
        if ((op == 77 || op == 78) && (B & (LRD_DYN_CONST | LRD_DYN_SELECT)) && a->dynsel[k]) {
            int e = emit_dyn(a, p, !!(B & LRD_DYN_SELECT));
            if (e < 0) return -1;
            if (e) { uint64_t cv; if (const_index(m, op == 77 ? p[4] : p[5], &cv, 0)) a->c_dynconst++; else a->c_dynsel++; continue; }
        }
        if (op == 61 && (B & LRD_CXS_FOLD) && wc >= 4 && p[3] < m->bound && a->cxs_var[p[3]]) {
            uint32_t t[8];
            if (wc > 8) return -1;
            memcpy(t, p, (size_t)wc * 4), t[3] = a->cxs_to;
            if (wb_put(&a->body, t, wc)) return -1;
            a->c_cxs++;
            continue;
        }
        if (op == 61 && (B & LRD_INPUT_CONST) && p[3] < m->bound && a->insel[p[3]]) {
            uint32_t nn = nullof(a, p[1]);
            if (nn) { uint32_t ops[3] = { p[1], p[2], nn }; if (wb_inst(&a->body, 83, ops, 3)) return -1; a->c_input++; continue; }
        }
        if (op == 61 && (B & LRD_IA_DROP) && p[3] < m->bound && m->ia_var[p[3]]) { a->dead[p[2]] = 1; a->c_ia++; continue; }
        if (((op >= 87 && op <= 97) && smpn) || (op == 98 && (B & (LRD_READ_NULL | LRD_IA_DROP))) || ((op >= 207 && op <= 215) && (B & LRD_DERIV_NULL))) {
            uint32_t nn = nullof(a, p[1]);
            if (nn) {
                uint32_t ops[3] = { p[1], p[2], nn };
                if (wb_inst(&a->body, 83, ops, 3)) return -1;
                if (op == 98) a->c_read++; else if (op >= 207) a->c_deriv++; else a->c_smp++;
                continue;
            }
        }
        if ((B & LRD_SMP_LOD0) && (op == 87 || op == 89 || op == 91 || op == 93)) {
            uint32_t base = (op == 89 || op == 93) ? 6 : 5, z = floatzero(a);
            if (z && wc >= base) {
                static const uint8_t nops[16] = { 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0 };   /* by bit: Bias Lod Grad(2) ConstOffset Offset ConstOffsets Sample MinLod ... (Grad = 2 operands, index 2) */
                static const uint8_t cnt[16] = { 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0 };
                (void)nops;
                uint32_t mask = wc > base ? p[base] : 0, tmp[64], nt = 0, q = base + 1;
                for (uint32_t h = 1; h < base; h++) tmp[nt++] = p[h];
                tmp[nt++] = (mask & ~(1u | 0x80u)) | 2u;
                int okk = 1;
                for (uint32_t b = 0; b < 16 && okk; b++) {
                    if (b == 1) tmp[nt++] = z;
                    if (!(mask & (1u << b))) continue;
                    uint32_t c = cnt[b];
                    if (q + c > wc || nt + c + 2 > 64) { okk = 0; break; }
                    if (b == 0 || b == 7) { q += c; continue; }
                    for (uint32_t t = 0; t < c; t++) tmp[nt++] = p[q++];
                }
                if (okk) { if (wb_inst(&a->body, op + 1, tmp, nt)) return -1; a->c_lod0++; continue; }
            }
        }
        if ((sw || swc) && op == 247 && i + 1 < i1 && iop(m, i + 1) == 251) {
            const uint32_t *ps = m->w + m->off[i + 1];
            uint32_t t = pick_switch_target(m, ps, swc);
            if (wb_inst(&a->body, 249, &t, 1)) return -1;
            if (swc && t != ps[2]) a->c_swc++; else a->c_sw++;
            i++;
            continue;
        }
        if (op == 248 && lp && p[1] < m->bound && a->hdr_merge[p[1]]) a->seen_hdr[p[1]] = 1;
        if (lp && op == 249 && p[1] < m->bound && a->hdr_merge[p[1]] && a->seen_hdr[p[1]]) {
            uint32_t f = boolconst(a, 0);
            if (f) { uint32_t ops[3] = { f, p[1], a->hdr_merge[p[1]] }; if (wb_inst(&a->body, 250, ops, 3)) return -1; a->c_lp++; continue; }
        }
        if (lp && op == 250 && wc >= 4) {
            int bh_t = p[2] < m->bound && a->hdr_merge[p[2]] && a->seen_hdr[p[2]], bh_f = p[3] < m->bound && a->hdr_merge[p[3]] && a->seen_hdr[p[3]];
            if (bh_t != bh_f) {
                uint32_t c = boolconst(a, bh_t ? 0 : 1);
                if (c) { uint32_t *t = malloc((size_t)wc * 4); if (!t) return -1; memcpy(t, p, (size_t)wc * 4); t[1] = c; int rc = wb_put(&a->body, t, wc); free(t); if (rc) return -1; a->c_lp++; continue; }
            }
        }
        if (op == 1 && i < m->ni && undef_wanted(a, m->undef_ord[p[2]], p[1])) {   /* function-level OpUndef */
            uint32_t nn = nullof(a, p[1]);
            if (nn) { uint32_t ops[3] = { p[1], p[2], nn }; if (wb_inst(&a->body, 83, ops, 3)) return -1; a->c_undef++; continue; }
        }
        if (r->use_class) { uint32_t did = 0; if (emit_with_use_class(a, &a->body, p, &did)) return -1; a->c_use += did; continue; }
        if (wb_put(&a->body, p, wc)) return -1;
    }
    return 1;
}

static int plan_keep(ax_t *a, char *why, size_t wcap) {
    const mod_t *m = &a->m;
    uint32_t B = a->r->bits, ek = 0;
    for (uint32_t k = 0; k < m->nfn; k++) if (m->fn[k].id == m->entry_id) ek = k;
    memset(a->keep, 1, m->nfn);
    a->cut_idx = 0;
    if (!(B & (LRD_CUT | LRD_MIN_ENTRY))) return 1;
    memset(a->keep, 0, m->nfn);
    a->keep[ek] = 1;
    if (B & LRD_MIN_ENTRY) return 1;
    a->cut_idx = plan_cut(a, ek, a->r->cut_blocks);
    if (!a->cut_idx) { snprintf(why, wcap, "the entry function has no block with index >= %u", a->r->cut_blocks); return 0; }
    uint8_t done[LAGFX_RED_MAXFN] = { 0 };
    for (uint32_t i = m->fn[ek].i0; i < a->cut_idx; i++) { const uint32_t *p = m->w + m->off[i]; if (OPC(p[0]) == 57 && p[3] < m->bound && m->fn_of_id[p[3]]) a->keep[m->fn_of_id[p[3]] - 1] = 1; }
    for (int again = 1; again;) {
        again = 0;
        for (uint32_t k = 0; k < m->nfn; k++) {
            if (!a->keep[k] || done[k] || k == ek) continue;
            done[k] = 1, again = 1;
            for (uint32_t i = m->fn[k].i0; i <= m->fn[k].i1; i++) { const uint32_t *p = m->w + m->off[i]; if (OPC(p[0]) == 57 && p[3] < m->bound && m->fn_of_id[p[3]]) a->keep[m->fn_of_id[p[3]] - 1] = 1; }
        }
    }
    return 1;
}

static int is_annot(uint32_t op) { return op == 5 || op == 6 || op == 71 || op == 72 || op == 332 || op == 5632 || op == 5633 || op == 73 || op == 74 || op == 75 || op == 76; }

/* ---- post passes: prune unreachable blocks / unused locals / unneeded memory accesses, on a finished module ---- */
typedef struct { uint32_t blk, blk_instr, blk_stub, var, wovar, wo_store, wo_chain, store, load, load_in, chain; } post_t;

static int is_chain_op(uint32_t op) { return op == 65 || op == 66 || op == 67 || op == 70; }

typedef struct {
    mod_t m;
    uint8_t *rm, *stubk, *dead, *readf, *fv, *stublab;
    uint32_t *uses, *parent, *stubh, *lab2blk;
} px_t;

static void px_free(px_t *x) { free(x->rm), free(x->stubk), free(x->dead), free(x->readf), free(x->fv), free(x->stublab), free(x->uses), free(x->parent), free(x->stubh), free(x->lab2blk); mod_free(&x->m); }

/* removes instruction i: its id operands lose a use, its result id is dead */
static void px_rm(px_t *x, uint32_t i) {
    const mod_t *m = &x->m;
    const uint32_t *p = m->w + m->off[i];
    uint32_t op = OPC(p[0]), wc = WCN(p[0]), rp = (uint32_t)res_pos(op);
    for (uint32_t t = 1; t < wc; t++) {
        if (t == rp || (rp == 2 && t == 1)) continue;
        if (is_id_pos(op, t, wc) && p[t] < m->bound && x->uses[p[t]]) x->uses[p[t]]--;
    }
    if (rp && wc > rp && p[rp] < m->bound) x->dead[p[rp]] = 1;
    x->rm[i] = 1;
}
static uint32_t px_root(const px_t *x, uint32_t id) { for (int d = 0; d < 64 && id < x->m.bound && x->parent[id]; d++) id = x->parent[id]; return id; }

/* 1 = changed (*out malloc'd), 2 = nothing to do, 0 = not applicable (why), -1 = OOM / malformed */
static int post_prune(const uint32_t *w, size_t n, uint32_t B, uint32_t **out, size_t *outn, post_t *ps, char *why, size_t wcap) {
    px_t x;
    memset(&x, 0, sizeof x);
    int mo = mod_open(&x.m, w, n), rc = -1;
    if (mo <= 0) { if (why && wcap) snprintf(why, wcap, "post pass: module not handled"); return mo < 0 ? -1 : 0; }
    const mod_t *m = &x.m;
    x.rm = calloc(m->ni + 1, 1), x.stubk = calloc(m->ni + 1, 1), x.stubh = calloc(m->ni + 1, 4);
    x.dead = calloc(m->bound + 1, 1), x.readf = calloc(m->bound + 1, 1), x.fv = calloc(m->bound + 1, 1), x.stublab = calloc(m->bound + 1, 1);
    x.uses = calloc(m->bound + 1, 4), x.parent = calloc(m->bound + 1, 4), x.lab2blk = calloc(m->bound + 1, 4);
    if (!x.rm || !x.stubk || !x.stubh || !x.dead || !x.readf || !x.fv || !x.stublab || !x.uses || !x.parent || !x.lab2blk) goto done;
    memset(ps, 0, sizeof *ps);
    if (B & LRD_PRUNE_BLK) {
        for (uint32_t k = 0; k < m->nfn; k++) {
            uint32_t i0 = m->fn[k].i0, i1 = m->fn[k].i1, nb = 0;
            for (uint32_t i = i0; i <= i1; i++) if (iop(m, i) == 248) nb++;
            if (nb < 2) continue;
            uint32_t *bf = malloc((size_t)nb * 4 * 4), *stack_ = NULL;
            if (!bf) goto done;
            uint32_t *be = bf + nb, *hdr = bf + 2 * nb;   /* first / last instruction index of block b, loop header label for a continue target */
            uint8_t *reach = calloc(nb, 1), *sref = calloc(nb, 1);
            stack_ = bf + 3 * nb;
            if (!reach || !sref) { free(reach), free(sref), free(bf); goto done; }
            uint32_t b = 0;
            for (uint32_t i = i0; i <= i1; i++) {
                if (iop(m, i) == 248) { bf[b] = i, x.lab2blk[m->w[m->off[i] + 1]] = b + 1; if (b) be[b - 1] = i - 1; b++; }
            }
            be[nb - 1] = i1 - 1;   /* the instruction before OpFunctionEnd */
            for (b = 0; b < nb; b++) hdr[b] = 0;
            uint32_t sp = 0;
            reach[0] = 1, stack_[sp++] = 0;
            while (sp) {
                uint32_t cur = stack_[--sp];
#define GO(lbl) do { uint32_t l_ = (lbl); if (l_ < m->bound && x.lab2blk[l_]) { uint32_t t_ = x.lab2blk[l_] - 1; if (!reach[t_]) { reach[t_] = 1; stack_[sp++] = t_; } } } while (0)
                const uint32_t *t = m->w + m->off[be[cur]];
                uint32_t top = OPC(t[0]), twc = WCN(t[0]);
                if (top == 249) GO(t[1]);
                else if (top == 250) { GO(t[2]); GO(t[3]); }
                else if (top == 251) {
                    uint32_t sty = t[1] < m->bound ? m->ty_of[t[1]] : 0, lw = (sty && sty < m->bound && m->op_of[sty] == 21 && m->w[m->defoff[sty] + 2] == 64) ? 2u : 1u;
                    GO(t[2]);
                    for (uint32_t q = 3; q + lw < twc; q += lw + 1) GO(t[q + lw]);
                }
#undef GO
            }
            /* structured-construct targets of the reachable blocks */
            for (b = 0; b < nb; b++) {
                if (!reach[b]) continue;
                uint32_t lab = m->w[m->off[bf[b]] + 1];
                for (uint32_t i = bf[b]; i <= be[b]; i++) {
                    const uint32_t *p = m->w + m->off[i];
                    uint32_t op = OPC(p[0]);
                    if (op == 247 && p[1] < m->bound && x.lab2blk[p[1]]) { if (!reach[x.lab2blk[p[1]] - 1]) sref[x.lab2blk[p[1]] - 1] |= 1; }
                    else if (op == 246) {
                        if (p[1] < m->bound && x.lab2blk[p[1]] && !reach[x.lab2blk[p[1]] - 1]) sref[x.lab2blk[p[1]] - 1] |= 1;
                        if (p[2] < m->bound && x.lab2blk[p[2]] && !reach[x.lab2blk[p[2]] - 1]) sref[x.lab2blk[p[2]] - 1] |= 2, hdr[x.lab2blk[p[2]] - 1] = lab;
                    }
                }
            }
            for (b = 0; b < nb; b++) {
                if (reach[b]) continue;
                uint32_t lab = m->w[m->off[bf[b]] + 1];
                if (sref[b]) {   /* stays as a one-instruction block */
                    x.stubk[bf[b]] = (sref[b] & 2) ? 2 : 1, x.stubh[bf[b]] = hdr[b], x.stublab[lab] = 1;
                    for (uint32_t i = bf[b] + 1; i <= be[b]; i++) { px_rm(&x, i); ps->blk_instr++; }
                    ps->blk_stub++;
                } else {
                    for (uint32_t i = bf[b]; i <= be[b]; i++) { px_rm(&x, i); ps->blk_instr++; }
                    ps->blk++;
                }
            }
            for (uint32_t i = i0; i <= i1; i++) if (iop(m, i) == 248) x.lab2blk[m->w[m->off[i] + 1]] = 0;
            free(reach), free(sref), free(bf);
        }
    }
    if (B & (LRD_PRUNE_VAR | LRD_PRUNE_WOVAR | LRD_MEM_STORE | LRD_MEM_LOAD | LRD_MEM_LOAD_IN)) {
        for (uint32_t i = m->first_fn; i < m->ni; i++) {   /* use counts and the read / write classification of every pointer */
            if (x.rm[i]) continue;
            const uint32_t *p = m->w + m->off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]), rp = (uint32_t)res_pos(op);
            for (uint32_t t = 1; t < wc; t++) {
                if (t == rp || (rp == 2 && t == 1)) continue;
                if (is_id_pos(op, t, wc) && p[t] < m->bound) x.uses[p[t]]++;
            }
            if (op == 59 && wc >= 4 && p[3] == 7 && p[2] < m->bound) x.fv[p[2]] = 1;
            if (op == 61 && wc >= 4) { if (p[3] < m->bound) x.readf[p[3]] = 1; }
            else if (op == 62 && wc >= 3) { if (p[2] < m->bound) x.readf[p[2]] = 1; }
            else if (op == 63 || op == 64) { if (wc >= 3 && p[2] < m->bound) x.readf[p[2]] = 1; }
            else if (is_chain_op(op) && wc >= 5 && p[2] < m->bound && p[3] < m->bound) x.parent[p[2]] = p[3];
            else for (uint32_t t = 1; t < wc; t++) { if (t == rp || (rp == 2 && t == 1)) continue; if (is_id_pos(op, t, wc) && p[t] < m->bound) x.readf[p[t]] = 1; }
        }
        for (uint32_t i = m->ni; i-- > m->first_fn;) {   /* a chain that is read makes its base read (bases are defined first: one backward pass) */
            if (x.rm[i]) continue;
            const uint32_t *p = m->w + m->off[i];
            if (is_chain_op(OPC(p[0])) && WCN(p[0]) >= 5 && p[2] < m->bound && p[3] < m->bound && x.readf[p[2]]) x.readf[p[3]] = 1;
        }
#define WO_VAR(id) ((id) < m->bound && x.fv[id] && !x.readf[id])
        for (uint32_t i = m->first_fn; i < m->ni; i++) {
            if (x.rm[i]) continue;
            const uint32_t *p = m->w + m->off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (op == 59 && wc >= 4 && (B & LRD_PRUNE_VAR) && p[3] == 7 && x.uses[p[2]] == 0) { px_rm(&x, i); ps->var++; continue; }
            if (op == 59 && wc >= 4 && (B & LRD_PRUNE_WOVAR) && p[3] == 7 && WO_VAR(p[2])) { px_rm(&x, i); ps->wovar++; continue; }
            if (is_chain_op(op) && wc >= 5 && (B & LRD_PRUNE_WOVAR) && WO_VAR(px_root(&x, p[3]))) { px_rm(&x, i); ps->wo_chain++; continue; }
            if ((op == 62 || op == 63 || op == 64) && wc >= 3 && (B & (LRD_PRUNE_WOVAR | LRD_MEM_STORE)) && WO_VAR(px_root(&x, p[1]))) {
                px_rm(&x, i);
                if (B & LRD_PRUNE_WOVAR) ps->wo_store++; else ps->store++;
                continue;
            }
        }
#undef WO_VAR
        if (B & (LRD_MEM_LOAD | LRD_MEM_LOAD_IN)) {
            for (int again = 1, iter = 0; again && iter < 256; iter++) {
                again = 0;
                for (uint32_t i = m->ni; i-- > m->first_fn;) {
                    if (x.rm[i]) continue;
                    const uint32_t *p = m->w + m->off[i];
                    uint32_t op = OPC(p[0]), wc = WCN(p[0]);
                    if (op == 61 && wc >= 4 && p[2] < m->bound && x.uses[p[2]] == 0 && !(wc > 4 && (p[4] & 1u))) {
                        uint32_t r = px_root(&x, p[3]);
                        int inp = r < m->bound && m->op_of[r] == 59 && m->w[m->defoff[r] + 3] == 1;
                        if (inp && !(B & LRD_MEM_LOAD_IN)) continue;
                        px_rm(&x, i), again = 1;
                        if (inp) ps->load_in++; else ps->load++;
                    } else if (is_chain_op(op) && wc >= 5 && p[2] < m->bound && x.uses[p[2]] == 0) { px_rm(&x, i), again = 1, ps->chain++; }
                }
            }
        }
    }
    if (!(ps->blk + ps->blk_stub + ps->var + ps->wovar + ps->wo_store + ps->wo_chain + ps->store + ps->load + ps->load_in + ps->chain)) { rc = 2; goto done; }
    for (uint32_t i = m->first_fn; i < m->ni; i++) {   /* no kept instruction may name a removed id; a phi must not name a stubbed predecessor */
        if (x.rm[i]) continue;
        const uint32_t *p = m->w + m->off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]), rp = (uint32_t)res_pos(op);
        for (uint32_t t = 1; t < wc; t++) {
            if (t == rp || (rp == 2 && t == 1)) continue;
            if (!is_id_pos(op, t, wc) || p[t] >= m->bound) continue;
            if (x.dead[p[t]] || (op == 245 && t >= 4 && ((t - 4) & 1u) == 0 && x.stublab[p[t]])) {
                if (why && wcap) snprintf(why, wcap, "post pass: removing code would leave instruction %u (op %u) using id %u: not applicable", i, op, p[t]);
                rc = 0;
                goto done;
            }
        }
    }
    {
        wb_t o = { 0 };
        if (wb_put(&o, w, 5)) { free(o.w); goto done; }
        for (uint32_t i = 0; i < m->ni; i++) {
            const uint32_t *p = m->w + m->off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (i < m->first_fn && is_annot(op) && wc >= 2 && p[1] < m->bound && x.dead[p[1]]) continue;
            if (i >= m->first_fn && x.rm[i]) continue;
            if (wb_put(&o, p, wc)) { free(o.w); goto done; }
            if (x.stubk[i]) { uint32_t hh = x.stubh[i]; if (x.stubk[i] == 2 ? wb_inst(&o, 249, &hh, 1) : wb_inst(&o, 255, NULL, 0)) { free(o.w); goto done; } }
        }
        *out = o.w, *outn = o.n;
        rc = 1;
    }
done:
    px_free(&x);
    return rc;
}

int lagfx_red_apply(const uint32_t *w, size_t words, const lagfx_red_recipe_t *r, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    ax_t *a = calloc(1, sizeof *a);
    int rc = -1;
    if (out) *out = NULL;
    if (info && cap) info[0] = 0;
    if (!a) return -1;
    int mo = mod_open(&a->m, w, words);
    if (mo <= 0) { free(a); if (info && cap) snprintf(info, cap, "module not handled (%s)", mo < 0 ? "out of memory" : "malformed or more than 128 functions"); return mo < 0 ? -1 : 0; }
    const mod_t *m = &a->m;
    a->r = r, a->next = m->bound;
    a->dead = calloc(m->bound + 1, 1), a->keep = calloc(m->nfn + 1, 1), a->seen_hdr = calloc(m->bound + 1, 1);
    a->nullmap = calloc(m->bound + 1, 4), a->hdr_merge = calloc(m->bound + 1, 4), a->cont_hdr = calloc(m->bound + 1, 4), a->refd = calloc(m->bound + 1, 1), a->lenc = calloc(m->bound + 1, 4);
    a->arr_other = calloc(m->bound + 1, 1), a->arr_last = calloc(m->bound + 1, 1), a->arr_ok = calloc(m->bound + 1, 1), a->insel = calloc(m->bound + 1, 1), a->cxs_var = calloc(m->bound + 1, 1);
    if (!a->dead || !a->keep || !a->seen_hdr || !a->nullmap || !a->hdr_merge || !a->cont_hdr || !a->refd || !a->lenc || !a->arr_other || !a->arr_last || !a->arr_ok || !a->insel || !a->cxs_var) goto done;
    if (m->entry_id == 0) { snprintf(a->why, sizeof a->why, "no entry point"); rc = 0; goto done; }
    if (!plan_keep(a, a->why, sizeof a->why)) { rc = 0; goto done; }
    const uint32_t B = r->bits;
    if (B & (LRD_SSBO_SHRINK | LRD_SSBO_RT)) {   /* which arrays are only ever the last member of a StorageBuffer struct */
        for (uint32_t i = 0; i < m->first_fn; i++) {
            const uint32_t *p = m->w + m->off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (op == 30) for (uint32_t q = 2; q < wc; q++) { if (p[q] < m->bound && m->op_of[p[q]] == 28) { if (q == wc - 1) a->arr_last[p[q]] = 1; else a->arr_other[p[q]] = 1; } }
            else if ((op == 28 || op == 29) && p[2] < m->bound) a->arr_other[p[2]] = 1;
            else if (op == 32 && p[3] < m->bound) a->arr_other[p[3]] |= (m->op_of[p[3]] == 28);
            else if (op == 32 && p[3] < m->bound && m->op_of[p[3]] == 30 && p[2] != 12) for (uint32_t t = 0; t < m->first_fn; t++) { (void)t; break; }
        }
    }
    if (B & LRD_INPUT_CONST) {   /* selected Input variables and the access chains rooted at them */
        uint32_t ord = 0;
        for (uint32_t i = 0; i < m->first_fn; i++) {
            const uint32_t *p = m->w + m->off[i];
            if (OPC(p[0]) != 59 || p[3] != 1) continue;
            int loc = 0, bi = 0;
            for (uint32_t j = 0; j < m->first_fn; j++) { const uint32_t *d = m->w + m->off[j]; if (OPC(d[0]) == 71 && d[1] == p[2]) { if (d[2] == 30) loc = 1; if (d[2] == 11) bi = 1; } }
            if (!loc || bi) continue;
            if (ord < 32 && ((r->input_sel >> ord) & 1u)) a->insel[p[2]] = 1;
            ord++;
        }
        for (uint32_t i = m->first_fn; i < m->ni; i++) { const uint32_t *p = m->w + m->off[i]; if ((OPC(p[0]) == 65 || OPC(p[0]) == 66 || OPC(p[0]) == 67) && p[3] < m->bound && a->insel[p[3]] && p[2] < m->bound) a->insel[p[2]] = 1; }
    }
    if (B & LRD_CXS_FOLD) {   /* the constexpr-sampler variables, and the sampler variable their loads are redirected to */
        uint32_t best = 0, bestb = 0, ptr = 0;
        for (uint32_t i = 0; i < m->first_fn; i++) { const uint32_t *p = m->w + m->off[i]; if (OPC(p[0]) == 5 && is_cxs_name(p) && p[1] < m->bound && m->op_of[p[1]] == 59) { a->cxs_var[p[1]] = 1; ptr = m->ty_of[p[1]]; } }
        if (!ptr) { snprintf(a->why, sizeof a->why, "the module has no constexpr-sampler variable (OpName lagfx.cxs.*)"); rc = 0; goto done; }
        for (uint32_t i = 0; i < m->first_fn; i++) {
            const uint32_t *p = m->w + m->off[i];
            if (OPC(p[0]) != 59 || WCN(p[0]) < 4 || p[3] != 0 || p[1] != ptr || a->cxs_var[p[2]]) continue;
            uint32_t bnd = 0;
            for (uint32_t j = 0; j < m->first_fn; j++) { const uint32_t *d = m->w + m->off[j]; if (OPC(d[0]) == 71 && d[1] == p[2] && d[2] == 33) bnd = d[3]; }
            if (!best || bnd >= bestb) best = p[2], bestb = bnd;
        }
        if (!best) { snprintf(a->why, sizeof a->why, "no sampler variable that is not a constexpr sampler to read instead"); rc = 0; goto done; }
        a->cxs_to = best;
    }
    if (B & (LRD_DYN_CONST | LRD_DYN_SELECT)) {
        uint32_t cnt[LAGFX_RED_MAXFN] = { 0 };
        for (uint32_t k = 0; k < m->nfn; k++) for (uint32_t i = m->fn[k].i0; i <= m->fn[k].i1; i++) { uint32_t op = iop(m, i); if (op == 77 || op == 78) cnt[k]++; }
        uint32_t top = r->dyn_top ? r->dyn_top : m->nfn;
        for (uint32_t n = 0; n < top; n++) {
            uint32_t best = ~0u, bc = 0;
            for (uint32_t k = 0; k < m->nfn; k++) if (!a->dynsel[k] && cnt[k] > bc) bc = cnt[k], best = k;
            if (best == ~0u) break;
            a->dynsel[best] = 1;
        }
    }
    /* global section */
    for (uint32_t i = 0; i < m->first_fn; i++) {
        const uint32_t *p = m->w + m->off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
        if (op == 59 && (B & LRD_IA_DROP) && p[2] < m->bound && m->ia_var[p[2]]) { a->dead[p[2]] = 1; a->c_ia++; continue; }
        if (op == 59 && (B & LRD_CXS_FOLD) && wc >= 4 && p[2] < m->bound && a->cxs_var[p[2]]) { a->dead[p[2]] = 1; continue; }
        if (op == 20 && !a->booltype) a->booltype = p[1];
        if (op == 22 && wc >= 3 && p[2] == 32 && !a->f32type) a->f32type = p[1];
        if (op == 43 && wc == 4 && p[3] == 0 && p[1] == a->f32type && a->f32type && !a->f32zero) a->f32zero = p[2];
        if (op == 41 && !a->btrue) a->btrue = p[2];
        if (op == 42 && !a->bfalse) a->bfalse = p[2];
        if (op == 46 && wc == 3 && p[1] < m->bound && !a->nullmap[p[1]]) a->nullmap[p[1]] = p[2];
        if (op == 1 && wc == 3 && undef_wanted(a, m->undef_ord[p[2]], p[1]) && null_ok(m, p[1], 0)) {
            uint32_t ops[2] = { p[1], p[2] };
            if (wb_inst(&a->gout, 46, ops, 2)) goto done;
            if (!a->nullmap[p[1]]) a->nullmap[p[1]] = p[2];
            a->c_undef++;
            continue;
        }
        if (op == 28 && wc == 4 && (B & (LRD_SSBO_SHRINK | LRD_SSBO_RT)) && p[3] < m->bound && m->op_of[p[3]] == 43 && m->w[m->defoff[p[3]] + 3] >= 1024 && (!r->ssbo_len || m->w[m->defoff[p[3]] + 3] > r->ssbo_len) && m->ty_of[p[3]] < m->bound) {
            if ((B & LRD_SSBO_RT) && a->arr_last[p[1]] && !a->arr_other[p[1]]) {
                uint32_t o[2] = { p[1], p[2] };
                if (wb_inst(&a->gout, 29, o, 2)) goto done;
                a->c_ssbo++;
                continue;
            }
            uint32_t lty = m->ty_of[p[3]];
            if (!a->lenc[lty]) {
                uint32_t id = newid(a), o[3] = { lty, id, r->ssbo_len ? r->ssbo_len : 16u };
                if (wb_inst(&a->gout, 43, o, 3)) goto done;
                a->lenc[lty] = id;
            }
            uint32_t o[3] = { p[1], p[2], a->lenc[lty] };
            if (wb_inst(&a->gout, 28, o, 3)) goto done;
            a->c_ssbo++;
            continue;
        }
        if (r->use_class && (op == 44)) { uint32_t did = 0; if (emit_with_use_class(a, &a->gout, p, &did)) goto done; a->c_use += did; continue; }
        if (wb_put(&a->gout, p, wc)) goto done;
    }
    a->in_body = 1;
    for (uint32_t k = 0; k < m->nfn; k++) {
        int e = emit_function(a, k);
        if (e < 0) goto done;
        if (e == 0) { snprintf(a->why, sizeof a->why, "function %u (id %u) cannot be rewritten safely (return type not null-able / unexpected layout)", k, m->fn[k].id); rc = 0; goto done; }
    }
    uint32_t changes = a->c_stub + a->c_undef + a->c_use + a->c_sw + a->c_swc + a->c_lp + a->c_smp + a->c_lod0 + a->c_deriv + a->c_read + a->c_ia + a->c_pruned + a->c_min + a->c_dynconst + a->c_dynsel + a->c_ssbo + a->c_input + a->c_cxs + (a->cut_idx ? 1u : 0u);
    if (!changes && !(B & LRD_POST_MASK)) { snprintf(a->why, sizeof a->why, "the rewrite changes nothing in this module"); rc = 0; goto done; }
    /* assemble: header + filtered global section + new globals + functions */
    wb_t fin = { 0 };
    if (wb_put(&fin, w, 5)) goto done;
    for (size_t p = 0; p < a->gout.n;) {
        const uint32_t *q = a->gout.w + p;
        uint32_t op = OPC(q[0]), wc = WCN(q[0]);
        if (is_annot(op) && wc >= 2 && q[1] < m->bound && a->dead[q[1]]) { p += wc; continue; }
        if (op == 15 && wc >= 4) {
            uint32_t s = 3;
            while (s < wc && !((q[s] & 0xff) == 0 || (q[s] & 0xff00) == 0 || (q[s] & 0xff0000) == 0 || (q[s] & 0xff000000u) == 0)) s++;
            s++;
            if (wb_put(&fin, q, s < wc ? s : wc)) { free(fin.w); goto done; }
            uint32_t nw = s < wc ? s : wc;
            for (uint32_t t = s; t < wc; t++) if (!(q[t] < m->bound && a->dead[q[t]])) { if (wb_put(&fin, q + t, 1)) { free(fin.w); goto done; } nw++; }
            fin.w[fin.n - (nw - (s < wc ? s : wc)) - (s < wc ? s : wc)] = (nw << 16) | 15u;
            p += wc;
            continue;
        }
        if (wb_put(&fin, q, wc)) { free(fin.w); goto done; }
        p += wc;
    }
    if (wb_put(&fin, a->gtail.w, a->gtail.n) || wb_put(&fin, a->body.w, a->body.n)) { free(fin.w); goto done; }
    fin.w[3] = a->next;
    /* no remaining use of a deleted id */
    {
        int any = 0;
        for (uint32_t i = 0; i < m->bound && !any; i++) any = a->dead[i];
        for (size_t p = 5; any && p < fin.n;) {
            const uint32_t *q = fin.w + p;
            uint32_t op = OPC(q[0]), wc = WCN(q[0]);
            if (!is_annot(op)) for (uint32_t t = 1; t < wc; t++) if (q[t] < m->bound && a->dead[q[t]] && is_id_pos(op, t, wc)) {
                snprintf(a->why, sizeof a->why, "id %u (deleted) is still used by an op %u instruction: not applicable", q[t], op);
                free(fin.w); rc = 0; goto done;
            }
            p += wc;
        }
    }
    post_t ps;
    memset(&ps, 0, sizeof ps);
    if (B & LRD_POST_MASK) {
        uint32_t *o2 = NULL;
        size_t n2 = 0;
        int pr = post_prune(fin.w, fin.n, B, &o2, &n2, &ps, a->why, sizeof a->why);
        if (pr < 0) { free(fin.w); goto done; }
        if (pr == 0) { free(fin.w); rc = 0; goto done; }
        if (pr == 2) { free(fin.w); snprintf(a->why, sizeof a->why, "the post pass finds nothing to remove in this module (the result would equal its base)"); rc = 0; goto done; }
        if (pr == 1) { free(fin.w); fin.w = o2, fin.n = n2; }
    }
    *out = fin.w, *out_words = fin.n;
    if (info && cap) {
        char *o = info;
        size_t c = cap;
        int n;
#define ADDI(cond, ...) do { if ((cond) && c > 1) { n = snprintf(o, c, __VA_ARGS__); if (n > 0 && (size_t)n < c) o += n, c -= (size_t)n; } } while (0)
        ADDI(a->c_stub, "stubbed=%u ", a->c_stub); ADDI(a->c_pruned, "pruned-fns=%u ", a->c_pruned); ADDI(a->cut_idx, "cut-at-instr=%u ", a->cut_idx); ADDI(a->c_min, "entry-minimal ");
        ADDI(a->c_undef, "undef->null=%u ", a->c_undef); ADDI(a->c_use, "undef-operands->null=%u ", a->c_use); ADDI(a->c_sw, "switch->default=%u ", a->c_sw); ADDI(a->c_swc, "switch->frequent-case=%u ", a->c_swc);
        ADDI(a->c_lp, "loop-back-edges->one-trip=%u ", a->c_lp); ADDI(a->c_smp, "samples->null=%u ", a->c_smp); ADDI(a->c_lod0, "implicit->explicit-lod0=%u ", a->c_lod0);
        ADDI(a->c_deriv, "derivatives->null=%u ", a->c_deriv); ADDI(a->c_read, "imageread->null=%u ", a->c_read); ADDI(a->c_ia, "input-attachment-decls/loads-dropped=%u ", a->c_ia); ADDI(a->c_dynconst, "dyn-access->composite-extract/insert=%u ", a->c_dynconst); ADDI(a->c_dynsel, "dyn-access->select-chain=%u ", a->c_dynsel); ADDI(a->c_ssbo, "big-arrays-shrunk/runtime=%u ", a->c_ssbo); ADDI(a->c_input, "input-loads->null=%u ", a->c_input);
        ADDI(a->c_cxs, "constexpr-sampler-loads-redirected=%u ", a->c_cxs); ADDI(a->c_minload, "entry-loads=%u ", a->c_minload); ADDI(a->c_wire, "outputs-wired=%u ", a->c_wire);
        ADDI(ps.blk || ps.blk_stub, "unreachable-blocks-removed=%u (+%u kept as stubs, %u instr) ", ps.blk, ps.blk_stub, ps.blk_instr); ADDI(ps.var, "unused-locals-removed=%u ", ps.var);
        ADDI(ps.wovar, "write-only-locals-removed=%u (+%u stores, %u chains) ", ps.wovar, ps.wo_store, ps.wo_chain); ADDI(ps.store, "stores-to-write-only-locals-removed=%u ", ps.store);
        ADDI(ps.load || ps.load_in || ps.chain, "unused-loads-removed=%u (+%u from Input variables) unused-access-chains-removed=%u ", ps.load, ps.load_in, ps.chain);
#undef ADDI
        size_t l = strlen(info);
        if (l && info[l - 1] == ' ') info[l - 1] = 0;
    }
    rc = 1;
done:
    if (rc == 0 && info && cap) snprintf(info, cap, "%s", a->why);
    free(a->gout.w), free(a->gtail.w), free(a->body.w), free(a->dead), free(a->keep), free(a->seen_hdr), free(a->nullmap), free(a->hdr_merge), free(a->cont_hdr), free(a->refd), free(a->lenc), free(a->arr_other), free(a->arr_last), free(a->arr_ok), free(a->insel), free(a->cxs_var);
    mod_free(&a->m);
    free(a);
    return rc;
}

/* ---- a vertex stage mirroring the fragment inputs ---- */
int lagfx_red_make_vs_for_fs(const uint32_t *fs, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap) {
    mod_t m;
    if (out) *out = NULL;
    int mo = mod_open(&m, fs, words);
    if (mo <= 0) return mo;
    struct { uint32_t loc, comp, kind, ncomp; int flat; } v[64];
    uint32_t nv = 0;
    int bad = 0;
    char why[120] = "";
    for (uint32_t i = 0; i < m.first_fn && !bad; i++) {
        const uint32_t *p = m.w + m.off[i];
        if (OPC(p[0]) != 59 || p[3] != 1 /* Input */) continue;
        uint32_t id = p[2], loc = ~0u, comp = 0;
        int flat = 0, builtin = 0;
        for (uint32_t j = 0; j < m.first_fn; j++) {
            const uint32_t *d = m.w + m.off[j];
            if (OPC(d[0]) == 71 && d[1] == id) { if (d[2] == 30) loc = d[3]; else if (d[2] == 24) comp = d[3]; else if (d[2] == 14) flat = 1; else if (d[2] == 11) builtin = 1; }
        }
        if (builtin || loc == ~0u) continue;
        uint32_t pt = m.w[m.defoff[p[1]] + 3], ncomp = 1, base = pt;
        if (m.op_of[pt] == 23) ncomp = m.w[m.defoff[pt] + 3], base = m.w[m.defoff[pt] + 2];
        if ((m.op_of[base] != 22 && m.op_of[base] != 21) || m.w[m.defoff[base] + 2] != 32 || ncomp > 4 || nv >= 64) { snprintf(why, sizeof why, "an FS input (Location %u) is not a 32-bit scalar/vector", loc); bad = 1; break; }
        uint32_t kind = m.op_of[base] == 22 ? 0 : (m.w[m.defoff[base] + 3] ? 1 : 2);
        v[nv].loc = loc, v[nv].comp = comp, v[nv].kind = kind, v[nv].ncomp = ncomp, v[nv].flat = flat || kind != 0, nv++;
    }
    mod_free(&m);
    if (bad) { if (info && cap) snprintf(info, cap, "%s", why); return 0; }
    wb_t an = { 0 }, ty = { 0 }, fn = { 0 }, fin = { 0 };
    uint32_t next = 1, idvoid = next++, idfnty = next++, idf32 = next++, idv4 = next++, idptr4 = next++, idpos = next++, idz = next++, ido = next++, idpc = next++, idmain = next++;
    uint32_t tid[3][5] = { { 0 } }, pid[3][5] = { { 0 } }, nid[3][5] = { { 0 } };
    uint32_t varid[64], iface[70], ni = 0;
    int rc = -1;
    uint32_t t3[3];
    wb_inst(&ty, 19, &idvoid, 1);
    { uint32_t o[2] = { idvoid, 0 }; (void)o; }
    uint32_t fo[2] = { idfnty, idvoid };
    wb_inst(&ty, 33, fo, 2);
    uint32_t f32o[2] = { idf32, 32 };
    wb_inst(&ty, 22, f32o, 2);
    uint32_t v4o[3] = { idv4, idf32, 4 };
    wb_inst(&ty, 23, v4o, 3);
    tid[0][1] = idf32, tid[0][4] = idv4;
    uint32_t p4o[3] = { idptr4, 3, idv4 };
    wb_inst(&ty, 32, p4o, 3);
    { uint32_t zo[3] = { idf32, idz, 0 }; wb_inst(&ty, 43, zo, 3); }
    { uint32_t oo[3] = { idf32, ido, 0x3f800000u }; wb_inst(&ty, 43, oo, 3); }
    { uint32_t co[6] = { idv4, idpc, idz, idz, idz, ido }; wb_inst(&ty, 44, co, 6); }
    { uint32_t vo[4] = { idptr4, idpos, 3 }; wb_inst(&ty, 59, vo, 3); }
    { uint32_t d3[3] = { idpos, 11, 0 }; wb_inst(&an, 71, d3, 3); }
    for (uint32_t k = 0; k < nv; k++) {
        uint32_t kd = v[k].kind, nc = v[k].ncomp;
        if (!tid[kd][1]) {
            if (kd == 0) tid[0][1] = idf32;
            else { tid[kd][1] = next++; uint32_t o[3] = { tid[kd][1], 32, kd == 1 ? 1u : 0u }; wb_inst(&ty, 21, o, 3); }
        }
        if (nc > 1 && !tid[kd][nc]) { tid[kd][nc] = next++; uint32_t o[3] = { tid[kd][nc], tid[kd][1], nc }; wb_inst(&ty, 23, o, 3); }
        uint32_t t = tid[kd][nc];
        if (!pid[kd][nc]) { pid[kd][nc] = next++; uint32_t o[3] = { pid[kd][nc], 3, t }; wb_inst(&ty, 32, o, 3); }
        if (!nid[kd][nc]) { nid[kd][nc] = next++; uint32_t o[2] = { t, nid[kd][nc] }; wb_inst(&ty, 46, o, 2); }
        varid[k] = next++;
        uint32_t vo[3] = { pid[kd][nc], varid[k], 3 };
        wb_inst(&ty, 59, vo, 3);
        uint32_t dl[3] = { varid[k], 30, v[k].loc };
        wb_inst(&an, 71, dl, 3);
        if (v[k].comp) { uint32_t dc[3] = { varid[k], 24, v[k].comp }; wb_inst(&an, 71, dc, 3); }
        if (v[k].flat) { uint32_t df[2] = { varid[k], 14 }; wb_inst(&an, 71, df, 2); }
    }
    (void)t3;
    { uint32_t fo2[4] = { idvoid, idmain, 0, idfnty }; wb_inst(&fn, 54, fo2, 4); }
    { uint32_t lb = next++; wb_inst(&fn, 248, &lb, 1); }
    { uint32_t so[2] = { idpos, idpc }; wb_inst(&fn, 62, so, 2); }
    for (uint32_t k = 0; k < nv; k++) { uint32_t so[2] = { varid[k], nid[v[k].kind][v[k].ncomp] }; wb_inst(&fn, 62, so, 2); }
    wb_inst(&fn, 253, NULL, 0), wb_inst(&fn, 56, NULL, 0);
    iface[ni++] = idpos;
    for (uint32_t k = 0; k < nv; k++) iface[ni++] = varid[k];
    { uint32_t cap1 = 1; wb_inst(&fin, 17, &cap1, 1); }
    uint32_t hdr[5] = { 0x07230203u, 0x00010000u, 0, next, 0 };
    wb_t outb = { 0 };
    wb_put(&outb, hdr, 5);
    wb_put(&outb, fin.w, fin.n);
    { uint32_t mm[2] = { 0, 1 }; wb_inst(&outb, 14, mm, 2); }
    { uint32_t ep[16], ne = 0; ep[ne++] = 0, ep[ne++] = idmain, ep[ne++] = 0x6e69616du, ep[ne++] = 0; for (uint32_t k = 0; k < ni; k++) ep[ne++] = iface[k]; wb_inst(&outb, 15, ep, 4); outb.n -= 0; (void)ep; }
    /* the entry point has a variable interface list: write it by hand */
    outb.n -= 5;   /* undo the 4-operand placeholder (header word + 4 operands) */
    { uint32_t ne = 4 + ni; wb_reserve(&outb, ne + 1); outb.w[outb.n++] = ((ne + 1) << 16) | 15u; outb.w[outb.n++] = 0; outb.w[outb.n++] = idmain; outb.w[outb.n++] = 0x6e69616du; outb.w[outb.n++] = 0; for (uint32_t k = 0; k < ni; k++) outb.w[outb.n++] = iface[k]; }
    wb_put(&outb, an.w, an.n), wb_put(&outb, ty.w, ty.n), wb_put(&outb, fn.w, fn.n);
    *out = outb.w, *out_words = outb.n;
    if (info && cap) snprintf(info, cap, "VS mirrors %u FS input Location(s)", nv);
    rc = 1;
    free(an.w), free(ty.w), free(fn.w), free(fin.w);
    return rc;
}

/* A cheap structural self-check of a rewritten module (NOT spirv-val): every id operand of a function-body instruction is below the bound and defined somewhere. 1 = ok. */
int lagfx_red_selfcheck(const uint32_t *w, size_t words, char *why, size_t cap) {
    mod_t m;
    int mo = mod_open(&m, w, words);
    if (mo <= 0) { if (why && cap) snprintf(why, cap, "module structure broken"); return 0; }
    int ok = 1;
    for (uint32_t i = m.first_fn; i < m.ni && ok; i++) {
        const uint32_t *p = m.w + m.off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]);
        if (op == 56 || op == 54) continue;
        for (uint32_t t = 1; t < wc; t++) {
            if (!is_id_pos(op, t, wc)) continue;
            if (op == 248 || (res_pos(op) == 2 && t == 1) || (res_pos(op) == 1 && t == 1)) { if (t == 1 && op != 248 && res_pos(op) == 1) continue; }
            if (p[t] == 0 || p[t] >= m.bound || !m.op_of[p[t]]) {
                if (op == 253 || op == 252) continue;
                if (why && cap) snprintf(why, cap, "instruction %u (op %u) word %u = %u is not a defined id", i, op, t, p[t]);
                ok = 0;
                break;
            }
        }
    }
    mod_free(&m);
    return ok;
}
