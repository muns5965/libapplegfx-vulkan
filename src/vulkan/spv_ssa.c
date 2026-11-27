/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — opt-in SSA promotion of Function variables (see spv_ssa.h). Pure C. */
#include "spv_ssa.h"
#include "spv_reduce.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPC(w) ((w) & 0xffffu)
#define WCN(w) ((w) >> 16)

/* ---------- small vectors ---------- */
typedef struct { uint32_t *a; uint32_t n, cap; } vec_t;
static int vec_push(vec_t *v, uint32_t x) {
    if (v->n == v->cap) {
        uint32_t c = v->cap ? v->cap * 2 : 4;
        uint32_t *p = realloc(v->a, (size_t)c * 4);
        if (!p) return -1;
        v->a = p, v->cap = c;
    }
    v->a[v->n++] = x;
    return 0;
}
static void vec_free(vec_t *v) { free(v->a); memset(v, 0, sizeof *v); }

/* ---------- opcode knowledge ---------- */
/* 0: no result id, 1: result at word 1, 2: result type at word 1 and result id at word 2 (same table as spv_reduce.c) */
static int res_pos(uint32_t op) {
    switch (op) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 10: case 14: case 15: case 16: case 17: case 39: case 56: case 62: case 63: case 64: case 71: case 72: case 74:
    case 99: case 218: case 219: case 220: case 221: case 224: case 225: case 228: case 246: case 247: case 249: case 250: case 251: case 252: case 253: case 254: case 255:
    case 256: case 317: case 330: case 332: case 4416: case 5380: case 5632: case 5633: return 0;
    case 7: case 11: case 73: case 248: return 1;
    default: return (op >= 19 && op <= 38) ? 1 : 2;
    }
}

/* Is word `pos` (>= 1) of a function-body instruction an id? 1 = id, 0 = literal, -1 = opcode not known (the module is left alone). */
static int idpos(uint32_t op, uint32_t pos) {
    switch (op) {
    case 1: case 55: case 56: case 57: case 65: case 66: case 67: case 70: case 77: case 78: case 80: case 83: case 84: case 86: case 100:
    case 245: case 248: case 249: case 252: case 253: case 254: case 255: case 224: case 225: case 4416: case 5380: return 1;
    case 8: return pos == 1;                                   /* OpLine file line col */
    case 12: return pos != 4;                                  /* OpExtInst: the instruction number is a literal */
    case 54: return pos != 3;                                  /* OpFunction control */
    case 59: return pos != 3;                                  /* OpVariable storage class */
    case 61: return pos < 4;                                   /* OpLoad ptr [mask ...] */
    case 62: return pos < 3;                                   /* OpStore ptr obj [mask ...] */
    case 63: return pos < 3;
    case 64: return pos < 4;
    case 68: return pos != 4;                                  /* OpArrayLength member */
    case 79: return pos < 5;                                   /* OpVectorShuffle components */
    case 81: return pos < 4;
    case 82: return pos < 5;
    case 87: case 88: case 91: case 92: case 95: case 98: return pos != 5;   /* image operands mask */
    case 89: case 90: case 93: case 94: case 96: return pos != 6;
    case 99: return pos != 4;                                  /* OpImageWrite */
    case 246: return pos < 3;                                  /* OpLoopMerge merge continue mask [params] */
    case 247: return pos < 2;
    case 250: return pos < 4;
    case 251: return pos < 3 || ((pos - 3) & 1u);   /* OpSwitch sel default (literal label)* : 32-bit selectors only (checked by the caller) */
    default: break;
    }
    if ((op >= 101 && op <= 108) || (op >= 109 && op <= 205) || (op >= 207 && op <= 215) || (op >= 227 && op <= 242)) return 1;
    return -1;
}

/* side-effect-free value producers whose unused result may be deleted */
static int pure_op(uint32_t op) {
    switch (op) {
    case 1: case 65: case 66: case 67: case 70: case 77: case 78: case 79: case 80: case 81: case 82: case 83: case 84: case 86: case 100: case 12: case 61: return 1;
    default: break;
    }
    return (op >= 87 && op <= 98) || (op >= 109 && op <= 205) || (op >= 207 && op <= 215);
}
static int is_term(uint32_t op) { return (op >= 249 && op <= 255) || op == 4416; }
static int is_annot(uint32_t op) { return op == 5 || op == 6 || op == 71 || op == 72 || op == 332 || op == 5632 || op == 5633; }

/* ---------- module ---------- */
typedef struct { uint32_t lab, i0, i1; } blk_t;   /* label instruction index, terminator instruction index */

typedef struct {
    uint32_t block, var;    /* function-local block index, variable slot */
    uint32_t id, ty;
    uint32_t nops;          /* = number of unique predecessors of the block */
    uint32_t *val;          /* value per predecessor (0 = not set yet) */
    uint32_t *plab;         /* label id of each predecessor, same order */
    uint8_t dead;
} phi_t;

typedef struct {
    const uint32_t *w; size_t n; uint32_t bound, ni, first_fn;
    size_t *off;
    uint32_t *opof, *tyof, *defi, *ptee;
    uint8_t *sc;               /* storage class of a pointer type / variable */
    uint8_t *gone;             /* original id whose definition is deleted */
    uint8_t *rm;               /* instruction removed */
    uint32_t *repl; uint32_t repl_cap;
    uint32_t next;
    /* new ids */
    vec_t undef_ty;            /* undef k has id bound_undef0 + ... : stored as (id, type) pairs */
    uint32_t *undef_of_ty;     /* type id -> undef id */
    phi_t *phi; uint32_t nphi, cap_phi;
    vec_t blk_phi_head;        /* per global block: first phi idx + 1 (linked list through phi_next) */
    uint32_t *phi_next;
    uint32_t *blk_gstart;      /* per function: first global block index */
    uint32_t *nid_phi;         /* new id - bound -> phi idx + 1 (0 = undef / unused) */
    uint32_t nid_cap;
    uint32_t glsl_set;
    lagfx_ssa_stats_t *st;
    unsigned flags;
    char *why; size_t wcap;
} S_t;

static void why_set(S_t *s, const char *fmt, uint32_t a, uint32_t b) {
    if (s->why && s->wcap) snprintf(s->why, s->wcap, fmt, a, b);
}

static uint32_t resolve(S_t *s, uint32_t id) {
    uint32_t d = 0;
    while (id < s->repl_cap && s->repl[id] && d++ < 100000) id = s->repl[id];
    return id;
}
static int repl_set(S_t *s, uint32_t id, uint32_t v) {
    if (id >= s->repl_cap) {
        uint32_t c = s->repl_cap ? s->repl_cap : 1024;
        while (c <= id) c *= 2;
        uint32_t *p = realloc(s->repl, (size_t)c * 4);
        if (!p) return -1;
        memset(p + s->repl_cap, 0, (size_t)(c - s->repl_cap) * 4);
        s->repl = p, s->repl_cap = c;
    }
    s->repl[id] = v;
    return 0;
}
static uint32_t new_id(S_t *s) { return s->next++; }

static uint32_t undef_for(S_t *s, uint32_t ty) {
    if (s->undef_of_ty[ty]) return s->undef_of_ty[ty];
    uint32_t id = new_id(s);
    s->undef_of_ty[ty] = id;
    vec_push(&s->undef_ty, id), vec_push(&s->undef_ty, ty);
    s->st->undefs++;
    return id;
}

/* ---------- per-function work ---------- */
typedef struct {
    uint32_t nb;
    blk_t *b;
    uint32_t *succ_off, *succ;     /* CSR, unique successors */
    uint32_t *pred_off, *pred;     /* CSR, unique predecessors (unreachable ones included) */
    uint8_t *reach;
    uint32_t *rpo, *rpo_of, *idom;
    uint32_t *df_off, *df;
    uint32_t *ch_head, *ch_next;
} cfg_t;

static void cfg_free(cfg_t *c) {
    free(c->b), free(c->succ_off), free(c->succ), free(c->pred_off), free(c->pred), free(c->reach), free(c->rpo), free(c->rpo_of), free(c->idom), free(c->df_off), free(c->df), free(c->ch_head), free(c->ch_next);
    memset(c, 0, sizeof *c);
}

static int terminator_succ(const S_t *s, uint32_t ti, uint32_t *out, uint32_t cap, const uint32_t *lab2blk, uint32_t *n) {
    const uint32_t *p = s->w + s->off[ti];
    uint32_t op = OPC(p[0]), wc = WCN(p[0]);
    uint32_t k = 0;
#define ADD(l) do { uint32_t l_ = (l); if (l_ >= s->bound || !lab2blk[l_]) return 0; uint32_t b_ = lab2blk[l_] - 1; int dup_ = 0; for (uint32_t q_ = 0; q_ < k; q_++) dup_ |= out[q_] == b_; if (!dup_) { if (k >= cap) return 0; out[k++] = b_; } } while (0)
    if (op == 249) ADD(p[1]);
    else if (op == 250) { ADD(p[2]); ADD(p[3]); }
    else if (op == 251) { ADD(p[2]); for (uint32_t q = 4; q < wc; q += 2) ADD(p[q]); }
#undef ADD
    *n = k;
    return 1;
}

/* Builds blocks, edges, reachability, dominators (Cooper-Harvey-Kennedy), dominance frontiers, dominator-tree children. 1 ok, 0 unsupported, -1 OOM */
static int cfg_build(S_t *s, uint32_t fi0, uint32_t fi1, uint32_t *lab2blk, cfg_t *c) {
    uint32_t nb = 0;
    for (uint32_t i = fi0; i <= fi1; i++) if (OPC(s->w[s->off[i]]) == 248) nb++;
    c->nb = nb;
    if (!nb) return 0;
    c->b = calloc(nb, sizeof(blk_t));
    if (!c->b) return -1;
    uint32_t k = 0;
    for (uint32_t i = fi0; i <= fi1; i++) {
        const uint32_t *p = s->w + s->off[i];
        uint32_t op = OPC(p[0]);
        if (op == 248) { c->b[k].lab = i; lab2blk[p[1]] = k + 1; k++; }
        if (k && (is_term(op))) { c->b[k - 1].i1 = i; }
    }
    for (uint32_t q = 0; q < nb; q++) if (!c->b[q].i1) return 0;
    c->succ_off = calloc(nb + 1, 4), c->pred_off = calloc(nb + 1, 4);
    c->reach = calloc(nb, 1), c->rpo = calloc(nb, 4), c->rpo_of = calloc(nb, 4), c->idom = calloc(nb, 4);
    c->ch_head = calloc(nb, 4), c->ch_next = calloc(nb, 4), c->df_off = calloc(nb + 1, 4);
    if (!c->succ_off || !c->pred_off || !c->reach || !c->rpo || !c->rpo_of || !c->idom || !c->ch_head || !c->ch_next || !c->df_off) return -1;
    uint32_t tmp[4096];
    size_t tot = 0;
    uint32_t *sl = NULL; size_t scap = 0;
    for (uint32_t q = 0; q < nb; q++) {
        uint32_t n = 0;
        if (!terminator_succ(s, c->b[q].i1, tmp, 4096, lab2blk, &n)) { free(sl); return 0; }
        c->succ_off[q] = (uint32_t)tot;
        if (tot + n > scap) { scap = (tot + n) * 2 + 16; uint32_t *p = realloc(sl, scap * 4); if (!p) { free(sl); return -1; } sl = p; }
        if (n) memcpy(sl + tot, tmp, n * 4);   /* integ6: sl is NULL while no successor has been stored; memcpy(NULL, .., 0) is UB (UBSan) */
        tot += n;
    }
    c->succ_off[nb] = (uint32_t)tot;
    c->succ = sl;
    /* preds */
    uint32_t *cnt = calloc(nb + 1, 4);
    if (!cnt) return -1;
    for (uint32_t q = 0; q < tot; q++) cnt[c->succ[q]]++;
    uint32_t acc = 0;
    for (uint32_t q = 0; q < nb; q++) { c->pred_off[q] = acc; acc += cnt[q]; cnt[q] = 0; }
    c->pred_off[nb] = acc;
    c->pred = malloc((acc ? acc : 1) * 4);
    if (!c->pred) { free(cnt); return -1; }
    for (uint32_t q = 0; q < nb; q++) for (uint32_t e = c->succ_off[q]; e < c->succ_off[q + 1]; e++) { uint32_t t = c->succ[e]; c->pred[c->pred_off[t] + cnt[t]++] = q; }
    free(cnt);
    /* reachability + reverse postorder (iterative DFS) */
    uint32_t *stk = malloc((size_t)nb * 4), *it = calloc(nb, 4), *post = malloc((size_t)nb * 4);
    if (!stk || !it || !post) { free(stk), free(it), free(post); return -1; }
    uint32_t sp = 0, np = 0;
    stk[sp++] = 0, c->reach[0] = 1;
    while (sp) {
        uint32_t x = stk[sp - 1];
        uint32_t e = c->succ_off[x] + it[x];
        if (e < c->succ_off[x + 1]) {
            it[x]++;
            uint32_t y = c->succ[e];
            if (!c->reach[y]) { c->reach[y] = 1; stk[sp++] = y; }
        } else { post[np++] = x; sp--; }
    }
    for (uint32_t q = 0; q < np; q++) { c->rpo[q] = post[np - 1 - q]; c->rpo_of[c->rpo[q]] = q; }
    free(stk), free(it), free(post);
    /* dominators */
    const uint32_t UND = 0xffffffffu;
    for (uint32_t q = 0; q < nb; q++) c->idom[q] = UND;
    c->idom[0] = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (uint32_t q = 1; q < np; q++) {
            uint32_t b = c->rpo[q], nd = UND;
            for (uint32_t e = c->pred_off[b]; e < c->pred_off[b + 1]; e++) {
                uint32_t p = c->pred[e];
                if (!c->reach[p] || c->idom[p] == UND) continue;
                if (nd == UND) { nd = p; continue; }
                uint32_t f1 = p, f2 = nd;
                while (f1 != f2) {
                    while (c->rpo_of[f1] > c->rpo_of[f2]) f1 = c->idom[f1];
                    while (c->rpo_of[f2] > c->rpo_of[f1]) f2 = c->idom[f2];
                }
                nd = f1;
            }
            if (nd != c->idom[b]) c->idom[b] = nd, changed = 1;
        }
    }
    memset(c->ch_head, 0, (size_t)nb * 4);
    memset(c->ch_next, 0, (size_t)nb * 4);
    for (uint32_t q = np; q-- > 1;) { uint32_t b = c->rpo[q], d = c->idom[b]; c->ch_next[b] = c->ch_head[d]; c->ch_head[d] = b + 1; }
    /* dominance frontiers (runner method), CSR via two passes */
    uint32_t *dfn = calloc(nb + 1, 4);
    uint32_t *last = malloc((size_t)nb * 4);
    if (!dfn || !last) { free(dfn), free(last); return -1; }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t q = 0; q < nb; q++) last[q] = UND;
        if (pass) { uint32_t a = 0; for (uint32_t q = 0; q < nb; q++) { c->df_off[q] = a; a += dfn[q]; dfn[q] = 0; } c->df_off[nb] = a; c->df = malloc((a ? a : 1) * 4); if (!c->df) { free(dfn), free(last); return -1; } }
        for (uint32_t q = 0; q < np; q++) {
            uint32_t b = c->rpo[q];
            uint32_t npr = 0;
            for (uint32_t e = c->pred_off[b]; e < c->pred_off[b + 1]; e++) npr += c->reach[c->pred[e]];
            if (npr < 2) continue;
            for (uint32_t e = c->pred_off[b]; e < c->pred_off[b + 1]; e++) {
                uint32_t r = c->pred[e];
                if (!c->reach[r]) continue;
                while (r != c->idom[b]) {
                    if (last[r] != b) { last[r] = b; if (pass) c->df[c->df_off[r] + dfn[r]] = b; dfn[r]++; }
                    if (r == 0) break;
                    r = c->idom[r];
                }
            }
        }
    }
    free(dfn), free(last);
    return 1;
}

/* ---------- the pass ---------- */
typedef struct {
    uint32_t id, ty, init;
    uint8_t ok;            /* promotable */
    vec_t defs;            /* blocks with a store (and block 0 for an initializer) */
    vec_t ups;             /* blocks with an upward-exposed load */
} var_t;

static int type_scalarish(const S_t *s, uint32_t ty, unsigned flags) {
    if (ty == 0 || ty >= s->bound) return 0;
    uint32_t op = s->opof[ty];
    if (op == 20 || op == 21 || op == 22 || op == 23 || op == 24) return 1;
    if ((op == 28 || op == 30) && (flags & LSSA_AGG)) return 1;
    return 0;
}

static int do_function(S_t *s, uint32_t fi, uint32_t fe, uint32_t gbase, uint32_t *lab2blk, uint32_t *vslot) {
    cfg_t c;
    memset(&c, 0, sizeof c);
    int rc = -1;
    var_t *vars = NULL;
    uint32_t nv = 0;
    uint8_t *live = NULL, *defb = NULL;
    uint32_t *cur = NULL, *log = NULL;
    uint32_t *wl = NULL, *stk = NULL;
    uint8_t *state = NULL;
    int cr = cfg_build(s, fi, fe, lab2blk, &c);
    if (cr <= 0) { rc = cr == 0 ? 0 : -1; if (cr == 0) why_set(s, "function at instruction %u: unsupported control flow (%u)", fi, 0); goto done; }
    uint32_t nb = c.nb;
    /* variables */
    uint32_t cap = 0;
    for (uint32_t i = c.b[0].lab; i <= c.b[0].i1; i++) if (OPC(s->w[s->off[i]]) == 59) cap++;
    vars = calloc(cap ? cap : 1, sizeof *vars);
    if (!vars) goto done;
    for (uint32_t i = c.b[0].lab; i <= c.b[0].i1; i++) {
        const uint32_t *p = s->w + s->off[i];
        if (OPC(p[0]) != 59 || p[3] != 7) continue;
        uint32_t pty = p[1], t = pty < s->bound && s->opof[pty] == 32 ? s->ptee[pty] : 0;
        s->st->vars++;
        vars[nv].id = p[2], vars[nv].ty = t, vars[nv].init = WCN(p[0]) >= 5 ? p[4] : 0;
        if (type_scalarish(s, t, s->flags)) vars[nv].ok = 1;
        else if (t && (s->opof[t] == 28 || s->opof[t] == 30)) s->st->skipped_agg++;
        else s->st->skipped_type++;
        vslot[p[2]] = nv + 1;
        nv++;
    }
    /* uses: anything but a plain load / store makes the variable escape */
    for (uint32_t i = fi; i <= fe; i++) {
        const uint32_t *p = s->w + s->off[i];
        uint32_t op = OPC(p[0]), wc = WCN(p[0]), rp = (uint32_t)res_pos(op);
        if (op == 251) {
            uint32_t sty = p[1] < s->bound ? s->tyof[p[1]] : 0;
            if (!sty || sty >= s->bound || s->opof[sty] != 21 || s->w[s->off[s->defi[sty]] + 2] > 32) { why_set(s, "OpSwitch at instruction %u: selector is not a 32-bit-or-narrower integer (%u)", i, 0); rc = 0; goto done; }
        }
        for (uint32_t t = 1; t < wc; t++) {
            int ip = idpos(op, t);
            if (ip < 0) { why_set(s, "unknown opcode %u in a function body (instruction %u): module left alone", op, i); rc = 0; goto done; }
            if (!ip || p[t] >= s->bound || !vslot[p[t]]) continue;
            if (t == rp || (rp == 2 && t == 1)) continue;   /* the variable's own definition */
            var_t *v = &vars[vslot[p[t]] - 1];
            if (!v->ok) continue;
            int okuse = 0;
            if (op == 61 && t == 3 && !(wc > 4 && (p[4] & 1u))) okuse = 1;                       /* OpLoad ptr [non-volatile] */
            else if (op == 62 && t == 1 && !(wc > 3 && (p[3] & 1u)) && p[2] != v->id) okuse = 1; /* OpStore ptr value [non-volatile] */
            if (!okuse) v->ok = 0, s->st->escaped++;
        }
    }
    uint32_t nprom = 0;
    for (uint32_t q = 0; q < nv; q++) if (vars[q].ok) nprom++;
    s->st->promoted += nprom;
    /* per-var def / upward-exposed-use blocks */
    uint32_t *seen = calloc(nv ? nv : 1, 4);
    if (!seen) goto done;
    if (nprom) {
        for (uint32_t q = 0; q < nv; q++) if (vars[q].ok && vars[q].init) { if (vec_push(&vars[q].defs, 0)) { free(seen); goto done; } }
        for (uint32_t bq = 0; bq < nb; bq++) {
            if (!c.reach[bq]) continue;
            for (uint32_t i = c.b[bq].lab; i <= c.b[bq].i1; i++) {
                const uint32_t *p = s->w + s->off[i];
                uint32_t op = OPC(p[0]);
                if (op == 61 && p[3] < s->bound && vslot[p[3]]) {
                    var_t *v = &vars[vslot[p[3]] - 1];
                    if (!v->ok) continue;
                    uint32_t k = vslot[p[3]] - 1;
                    int stored_here = seen[k] == bq + 1 || (bq == 0 && v->init);
                    if (!stored_here && (v->ups.n == 0 || v->ups.a[v->ups.n - 1] != bq) && vec_push(&v->ups, bq)) { free(seen); goto done; }
                } else if (op == 62 && p[1] < s->bound && vslot[p[1]]) {
                    var_t *v = &vars[vslot[p[1]] - 1];
                    if (!v->ok) continue;
                    uint32_t k = vslot[p[1]] - 1;
                    if (seen[k] != bq + 1) { seen[k] = bq + 1; if ((v->defs.n == 0 || v->defs.a[v->defs.n - 1] != bq) && vec_push(&v->defs, bq)) { free(seen); goto done; } }
                }
            }
        }
    }
    free(seen);
    if (nprom) {
        /* liveness (upward-exposed uses propagated up, stopped by blocks that store) and phi placement */
        live = calloc((size_t)nv * nb, 1), defb = calloc((size_t)nv * nb, 1);
        wl = malloc((size_t)nb * 4);
        if (!live || !defb || !wl) goto done;
        uint32_t *work = malloc((size_t)nb * 8 + 8);
        uint32_t *phmark = calloc(nb, 4), *cand = malloc((size_t)nb * 4 + 4);
        if (!work || !phmark || !cand) { free(work), free(phmark), free(cand); goto done; }
        for (uint32_t q = 0; q < nv; q++) {
            var_t *v = &vars[q];
            if (!v->ok) continue;
            uint8_t *lv = live + (size_t)q * nb, *db = defb + (size_t)q * nb;
            for (uint32_t k = 0; k < v->defs.n; k++) db[v->defs.a[k]] = 1;
            uint32_t sp = 0;
            for (uint32_t k = 0; k < v->ups.n; k++) { uint32_t u = v->ups.a[k]; if (!lv[u]) lv[u] = 1, wl[sp++] = u; }
            while (sp) {
                uint32_t b = wl[--sp];
                for (uint32_t e = c.pred_off[b]; e < c.pred_off[b + 1]; e++) {
                    uint32_t p = c.pred[e];
                    if (!c.reach[p] || db[p] || lv[p]) continue;
                    lv[p] = 1, wl[sp++] = p;
                }
            }
            /* iterated dominance frontier of the def blocks, restricted to blocks where the variable is live-in */
            uint32_t nw = 0, ncand = 0, maxnops = 0;
            for (uint32_t k = 0; k < v->defs.n; k++) work[nw++] = v->defs.a[k];
            uint32_t stamp = q + 1;
            while (nw) {   /* the iterated dominance frontier restricted to live-in blocks, collected first so that a too wide phi can veto the variable */
                uint32_t x = work[--nw];
                for (uint32_t e = c.df_off[x]; e < c.df_off[x + 1]; e++) {
                    uint32_t y = c.df[e];
                    if (phmark[y] == stamp) continue;
                    phmark[y] = stamp;
                    if (!lv[y]) continue;
                    cand[ncand++] = y;
                    if (c.pred_off[y + 1] - c.pred_off[y] > maxnops) maxnops = c.pred_off[y + 1] - c.pred_off[y];
                    work[nw++] = y;
                }
            }
            if ((s->flags >> 16) && maxnops > (s->flags >> 16)) { v->ok = 0, s->st->promoted--, s->st->skipped_wide++; continue; }
            for (uint32_t ci = 0; ci < ncand; ci++) {
                uint32_t y = cand[ci];
                {
                    if (s->nphi == s->cap_phi) {
                        uint32_t nc = s->cap_phi ? s->cap_phi * 2 : 256;
                        phi_t *pp = realloc(s->phi, (size_t)nc * sizeof *pp);
                        uint32_t *pn = realloc(s->phi_next, (size_t)nc * 4);
                        if (!pp || !pn) { if (pp) s->phi = pp; if (pn) s->phi_next = pn; free(work), free(phmark), free(cand); goto done; }
                        s->phi = pp, s->phi_next = pn, s->cap_phi = nc;
                    }
                    phi_t *ph = &s->phi[s->nphi];
                    memset(ph, 0, sizeof *ph);
                    ph->block = y, ph->var = q, ph->ty = v->ty, ph->id = new_id(s);
                    ph->nops = c.pred_off[y + 1] - c.pred_off[y];
                    ph->val = calloc(ph->nops ? ph->nops : 1, 4);
                    ph->plab = calloc(ph->nops ? ph->nops : 1, 4);
                    if (!ph->val || !ph->plab) { free(work), free(phmark), free(cand); goto done; }
                    for (uint32_t t = 0; t < ph->nops; t++) ph->plab[t] = s->w[s->off[c.b[c.pred[c.pred_off[y] + t]].lab] + 1];
                    uint32_t no = ph->id - s->bound;
                    if (no >= s->nid_cap) {
                        uint32_t nc = s->nid_cap ? s->nid_cap * 2 : 1024;
                        while (nc <= no) nc *= 2;
                        uint32_t *pp2 = realloc(s->nid_phi, (size_t)nc * 4);
                        if (!pp2) { free(work), free(phmark), free(cand); goto done; }
                        memset(pp2 + s->nid_cap, 0, (size_t)(nc - s->nid_cap) * 4);
                        s->nid_phi = pp2, s->nid_cap = nc;
                    }
                    s->nid_phi[no] = s->nphi + 1;
                    uint32_t gb = gbase + y;
                    while (s->blk_phi_head.n <= gb) vec_push(&s->blk_phi_head, 0);
                    s->phi_next[s->nphi] = s->blk_phi_head.a[gb];
                    s->blk_phi_head.a[gb] = s->nphi + 1;
                    s->nphi++;
                    s->st->phis_placed++;
                }
            }
        }
        free(work), free(phmark), free(cand);
        /* renaming: dominator-tree walk, per-variable current value with an undo log */
        cur = calloc(nv ? nv : 1, 4);
        uint32_t logcap = 4096, logn = 0;
        log = malloc((size_t)logcap * 8);
        stk = malloc((size_t)nb * 4 * 2 + 8), state = calloc(nb, 1);
        uint32_t *logmark = calloc(nb, 4);
        if (!cur || !log || !stk || !state || !logmark) { free(logmark); goto done; }
        uint32_t sp = 0;
        stk[sp++] = 0;
        int oom = 0;
        while (sp && !oom) {
            uint32_t b = stk[sp - 1];
            if (state[b] == 0) {
                state[b] = 1;
                logmark[b] = logn;
#define SETCUR(v_, val_) do { if (logn == logcap) { logcap *= 2; uint32_t *lp_ = realloc(log, (size_t)logcap * 8); if (!lp_) { oom = 1; break; } log = lp_; } log[logn * 2] = (v_); log[logn * 2 + 1] = cur[(v_)]; logn++; cur[(v_)] = (val_); } while (0)
                if (b == 0) for (uint32_t q = 0; q < nv; q++) if (vars[q].ok && vars[q].init) SETCUR(q, vars[q].init);
                for (uint32_t pi = s->blk_phi_head.n > gbase + b ? s->blk_phi_head.a[gbase + b] : 0; pi && !oom; pi = s->phi_next[pi - 1]) SETCUR(s->phi[pi - 1].var, s->phi[pi - 1].id);
                for (uint32_t i = c.b[b].lab + 1; i <= c.b[b].i1 && !oom; i++) {
                    const uint32_t *p = s->w + s->off[i];
                    uint32_t op = OPC(p[0]);
                    if (op == 61 && p[3] < s->bound && vslot[p[3]] && vars[vslot[p[3]] - 1].ok) {
                        uint32_t q = vslot[p[3]] - 1;
                        uint32_t val = cur[q] ? cur[q] : undef_for(s, vars[q].ty);
                        if (repl_set(s, p[2], val)) { oom = 1; break; }
                        s->rm[i] = 1, s->gone[p[2]] = 1, s->st->loads_removed++;
                    } else if (op == 62 && p[1] < s->bound && vslot[p[1]] && vars[vslot[p[1]] - 1].ok) {
                        uint32_t q = vslot[p[1]] - 1;
                        SETCUR(q, resolve(s, p[2]));
                        s->rm[i] = 1, s->st->stores_removed++;
                    }
                }
                if (oom) break;
                for (uint32_t e = c.succ_off[b]; e < c.succ_off[b + 1]; e++) {
                    uint32_t sb = c.succ[e];
                    uint32_t pidx = 0;
                    for (uint32_t t = c.pred_off[sb]; t < c.pred_off[sb + 1]; t++) if (c.pred[t] == b) { pidx = t - c.pred_off[sb]; break; }
                    for (uint32_t pi = s->blk_phi_head.n > gbase + sb ? s->blk_phi_head.a[gbase + sb] : 0; pi; pi = s->phi_next[pi - 1]) {
                        phi_t *ph = &s->phi[pi - 1];
                        ph->val[pidx] = cur[ph->var] ? cur[ph->var] : undef_for(s, ph->ty);
                    }
                }
                for (uint32_t ch = c.ch_head[b]; ch; ch = c.ch_next[ch - 1]) stk[sp++] = ch - 1;
            } else {
                for (uint32_t k = logn; k-- > logmark[b];) cur[log[k * 2]] = log[k * 2 + 1];
                logn = logmark[b];
                sp--;
            }
        }
#undef SETCUR
        free(logmark);
        if (oom) goto done;
        /* operands from unreachable predecessors: undef */
        for (uint32_t bq = 0; bq < nb; bq++) {
            for (uint32_t pi = s->blk_phi_head.n > gbase + bq ? s->blk_phi_head.a[gbase + bq] : 0; pi; pi = s->phi_next[pi - 1]) {
                phi_t *ph = &s->phi[pi - 1];
                for (uint32_t t = 0; t < ph->nops; t++) if (!ph->val[t]) ph->val[t] = undef_for(s, ph->ty);
            }
        }
    }
    /* unreachable blocks: loads of promoted variables read an undefined value, stores vanish */
    for (uint32_t bq = 0; bq < nb; bq++) {
        if (c.reach[bq]) continue;
        for (uint32_t i = c.b[bq].lab; i <= c.b[bq].i1; i++) {
            const uint32_t *p = s->w + s->off[i];
            uint32_t op = OPC(p[0]);
            if (op == 61 && p[3] < s->bound && vslot[p[3]] && vars[vslot[p[3]] - 1].ok) {
                if (repl_set(s, p[2], undef_for(s, vars[vslot[p[3]] - 1].ty))) goto done;
                s->rm[i] = 1, s->gone[p[2]] = 1, s->st->loads_removed++;
            } else if (op == 62 && p[1] < s->bound && vslot[p[1]] && vars[vslot[p[1]] - 1].ok) s->rm[i] = 1, s->st->stores_removed++;
        }
    }
    /* the promoted declarations go */
    for (uint32_t q = 0; q < nv; q++) if (vars[q].ok) {
        for (uint32_t i = c.b[0].lab; i <= c.b[0].i1; i++) { const uint32_t *p = s->w + s->off[i]; if (OPC(p[0]) == 59 && p[2] == vars[q].id) { s->rm[i] = 1; break; } }
        s->gone[vars[q].id] = 1, s->st->decl_removed++;
    }
    rc = 1;
done:
    for (uint32_t q = 0; q < nv; q++) vec_free(&vars[q].defs), vec_free(&vars[q].ups);
    free(vars), free(live), free(defb), free(cur), free(log), free(wl), free(stk), free(state);
    if (rc >= 0 && s->blk_phi_head.n < gbase + c.nb) { while (s->blk_phi_head.n < gbase + c.nb) vec_push(&s->blk_phi_head, 0); }
    s->st->fns++;
    cfg_free(&c);
    return rc;
}

/* ---------- driver ---------- */
int lagfx_spv_ssa_promote(const uint32_t *w, size_t n, unsigned flags, uint32_t **out, size_t *outn, lagfx_ssa_stats_t *st_out, char *why, size_t wcap) {
    lagfx_ssa_stats_t st0;
    lagfx_ssa_stats_t *st = st_out ? st_out : &st0;
    memset(st, 0, sizeof *st);
    if (out) *out = NULL;
    if (why && wcap) why[0] = 0;
    S_t s;
    memset(&s, 0, sizeof s);
    s.w = w, s.n = n, s.st = st, s.flags = flags, s.why = why, s.wcap = wcap;
    int rc = -1;
    uint32_t *lab2blk = NULL, *vslot = NULL, *uses = NULL, *blkbase = NULL;
    uint32_t *fn0 = NULL, *fn1 = NULL;
    uint32_t nfn = 0;
    if (n < 5 || w[0] != 0x07230203u || w[3] == 0 || w[3] > (1u << 24)) { if (why && wcap) snprintf(why, wcap, "not a SPIR-V module"); return 0; }
    s.bound = w[3];
    uint32_t ni = 0;
    for (size_t p = 5; p < n;) { uint32_t wc = WCN(w[p]); if (!wc || p + wc > n) { if (why && wcap) snprintf(why, wcap, "malformed module"); return 0; } p += wc; ni++; }
    s.ni = ni, s.first_fn = ni;
    s.off = malloc(((size_t)ni + 1) * sizeof(size_t));
    s.opof = calloc(s.bound + 1, 4), s.tyof = calloc(s.bound + 1, 4), s.defi = calloc(s.bound + 1, 4), s.ptee = calloc(s.bound + 1, 4);
    s.sc = calloc(s.bound + 1, 1), s.gone = calloc(s.bound + 1, 1), s.rm = calloc(ni + 1, 1);
    s.undef_of_ty = calloc(s.bound + 1, 4);
    lab2blk = calloc(s.bound + 1, 4), vslot = calloc(s.bound + 1, 4), uses = NULL;
    if (!s.off || !s.opof || !s.tyof || !s.defi || !s.ptee || !s.sc || !s.gone || !s.rm || !s.undef_of_ty || !lab2blk || !vslot) goto done;
    s.next = s.bound;
    {
        uint32_t i = 0;
        uint32_t cap_fn = 64;
        fn0 = malloc(cap_fn * 4), fn1 = malloc(cap_fn * 4);
        if (!fn0 || !fn1) goto done;
        int infn = 0;
        for (size_t p = 5; p < n; i++) {
            uint32_t op = OPC(w[p]), wc = WCN(w[p]);
            s.off[i] = p;
            int rp = res_pos(op);
            if (rp == 1 && wc >= 2 && w[p + 1] < s.bound) s.opof[w[p + 1]] = op, s.defi[w[p + 1]] = i;
            else if (rp == 2 && wc >= 3 && w[p + 2] < s.bound) s.opof[w[p + 2]] = op, s.defi[w[p + 2]] = i, s.tyof[w[p + 2]] = w[p + 1];
            if (op == 32 && wc >= 4) s.ptee[w[p + 1]] = w[p + 3], s.sc[w[p + 1]] = (uint8_t)w[p + 2];
            if (op == 11 && wc >= 4 && !strncmp((const char *)(w + p + 2), "GLSL.std.450", 12)) s.glsl_set = w[p + 1];
            if (op == 54) {
                if (s.first_fn == ni) s.first_fn = i;
                if (infn) { rc = 0; if (why && wcap) snprintf(why, wcap, "nested OpFunction"); goto done; }
                if (nfn == cap_fn) { cap_fn *= 2; uint32_t *a = realloc(fn0, cap_fn * 4), *b = realloc(fn1, cap_fn * 4); if (!a || !b) { if (a) fn0 = a; if (b) fn1 = b; goto done; } fn0 = a, fn1 = b; }
                fn0[nfn] = i, infn = 1;
            } else if (op == 56 && infn) { fn1[nfn++] = i, infn = 0; }
            p += wc;
        }
        s.off[ni] = n;
        if (infn) { rc = 0; goto done; }
        st->inst_before = ni, st->words_before = (uint32_t)n;
    }
    if (!nfn) { rc = 0; if (why && wcap) snprintf(why, wcap, "no function"); goto done; }
    blkbase = calloc(nfn + 1, 4);
    if (!blkbase) goto done;
    for (uint32_t f = 0; f < nfn; f++) {
        uint32_t nb = 0;
        for (uint32_t i = fn0[f]; i <= fn1[f]; i++) if (OPC(w[s.off[i]]) == 248) nb++;
        blkbase[f + 1] = blkbase[f] + nb;
    }
    s.blk_gstart = blkbase;
    for (uint32_t f = 0; f < nfn; f++) {
        int r = do_function(&s, fn0[f], fn1[f], blkbase[f], lab2blk, vslot);
        if (r < 0) goto done;
        if (r == 0) { rc = 0; goto done; }
    }
    if (!st->promoted) { rc = 0; if (why && wcap) snprintf(why, wcap, "no promotable Function variable"); goto done; }

    /* trivial phis: every operand (ignoring the phi itself) is one and the same value */
    for (int again = 1, iter = 0; again && iter < 64; iter++) {
        again = 0;
        for (uint32_t q = 0; q < s.nphi; q++) {
            phi_t *ph = &s.phi[q];
            if (ph->dead) continue;
            uint32_t uniq = 0;
            int multi = 0;
            for (uint32_t t = 0; t < ph->nops && !multi; t++) {
                uint32_t v = resolve(&s, ph->val[t]);
                if (v == ph->id) continue;
                if (!uniq) uniq = v; else if (uniq != v) multi = 1;
            }
            if (multi || !uniq) continue;
            if (repl_set(&s, ph->id, uniq)) goto done;
            ph->dead = 1, st->phis_trivial++, again = 1;
        }
    }
    /* use counts over what will be emitted; dead phis (and, with LSSA_DCE, dead pure values) go */
    {
        uint32_t tot = s.next + 1;
        uses = calloc(tot, 4);
        if (!uses) goto done;
#define COUNT_USES(P_, WC_, OP_) do { uint32_t rp_ = (uint32_t)res_pos(OP_); for (uint32_t t_ = 1; t_ < (WC_); t_++) { if (idpos((OP_), t_) != 1) continue; if (t_ == rp_ || (rp_ == 2 && t_ == 1)) continue; uint32_t x_ = resolve(&s, (P_)[t_]); if (x_ < tot) uses[x_]++; } } while (0)
        for (uint32_t i = s.first_fn; i < ni; i++) {
            if (s.rm[i]) continue;
            const uint32_t *p = w + s.off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            COUNT_USES(p, wc, op);
        }
        for (uint32_t q = 0; q < s.nphi; q++) if (!s.phi[q].dead) for (uint32_t t = 0; t < s.phi[q].nops; t++) { uint32_t x = resolve(&s, s.phi[q].val[t]); if (x < tot) uses[x]++; }
        /* worklist */
        uint32_t *stk = malloc(((size_t)ni + s.nphi + 16) * 4 * 2);
        if (!stk) goto done;
        uint32_t sp = 0;   /* entries: instr index + 1, or 0x80000000 | phi idx */
        for (uint32_t q = 0; q < s.nphi; q++) if (!s.phi[q].dead && !uses[s.phi[q].id]) stk[sp++] = 0x80000000u | q;
        if (flags & LSSA_DCE) for (uint32_t i = s.first_fn; i < ni; i++) {
            if (s.rm[i]) continue;
            const uint32_t *p = w + s.off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (op == 59 && wc >= 4 && p[3] == 7 && p[2] < s.bound && !uses[p[2]]) stk[sp++] = i + 1;
            else if (pure_op(op) && res_pos(op) == 2 && p[2] < s.bound && !uses[p[2]]) stk[sp++] = i + 1;
        }
        while (sp) {
            uint32_t e = stk[--sp];
            if (e & 0x80000000u) {
                phi_t *ph = &s.phi[e & 0x7fffffffu];
                if (ph->dead) continue;
                ph->dead = 1, st->phis_dead++;
                for (uint32_t t = 0; t < ph->nops; t++) {
                    uint32_t x = resolve(&s, ph->val[t]);
                    if (x >= tot || !uses[x]) continue;
                    if (--uses[x] == 0) {
                        if (x >= s.bound) { uint32_t pi = s.nid_phi && x - s.bound < s.nid_cap ? s.nid_phi[x - s.bound] : 0; if (pi && !s.phi[pi - 1].dead) stk[sp++] = 0x80000000u | (pi - 1); }
                        else if ((flags & LSSA_DCE) && s.defi[x] >= s.first_fn && !s.rm[s.defi[x]]) {
                            const uint32_t *dp = w + s.off[s.defi[x]];
                            uint32_t dop = OPC(dp[0]);
                            if (pure_op(dop) || dop == 59) stk[sp++] = s.defi[x] + 1;
                        }
                    }
                }
                continue;
            }
            uint32_t i = e - 1;
            if (s.rm[i]) continue;
            const uint32_t *p = w + s.off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            int okdel = 1;
            if (op == 61) {   /* load: not volatile, and not an Input load unless asked */
                if (wc > 4 && (p[4] & 1u)) okdel = 0;
                uint32_t r = p[3];
                for (int d = 0; d < 64 && r < s.bound && (s.opof[r] == 65 || s.opof[r] == 66 || s.opof[r] == 67 || s.opof[r] == 70); d++) r = w[s.off[s.defi[r]] + 3];
                if (r < s.bound && s.opof[r] == 59 && w[s.off[s.defi[r]] + 3] == 1 && !(flags & LSSA_DCE_INPUT)) okdel = 0;
            }
            if (op == 12 && !(wc >= 4 && p[3] == s.glsl_set && s.glsl_set)) okdel = 0;
            if (op == 59 && !(wc >= 4 && p[3] == 7)) okdel = 0;
            if (!okdel) continue;
            s.rm[i] = 1, st->dce_removed++;
            if (p[2] < s.bound) s.gone[p[2]] = 1;
            uint32_t rp = (uint32_t)res_pos(op);
            for (uint32_t t = 1; t < wc; t++) {
                if (idpos(op, t) != 1 || t == rp || (rp == 2 && t == 1)) continue;
                uint32_t x = resolve(&s, p[t]);
                if (x >= tot || !uses[x]) continue;
                if (--uses[x] == 0) {
                    if (x >= s.bound) { uint32_t pi = s.nid_phi && x - s.bound < s.nid_cap ? s.nid_phi[x - s.bound] : 0; if (pi && !s.phi[pi - 1].dead) stk[sp++] = 0x80000000u | (pi - 1); }
                    else if ((flags & LSSA_DCE) && s.defi[x] >= s.first_fn && !s.rm[s.defi[x]]) {
                        const uint32_t *dp = w + s.off[s.defi[x]];
                        uint32_t dop = OPC(dp[0]);
                        if (pure_op(dop) || dop == 59) stk[sp++] = s.defi[x] + 1;
                    }
                }
            }
        }
        free(stk);
#undef COUNT_USES
    }
    for (uint32_t q = 0; q < s.nphi; q++) if (!s.phi[q].dead) st->phis_kept++;
    /* emit */
    {
        size_t cap = n + (size_t)s.nphi * 24 + (size_t)s.undef_ty.n * 4 + 64;
        for (uint32_t q = 0; q < s.nphi; q++) cap += (size_t)s.phi[q].nops * 2;
        uint32_t *o = malloc(cap * 4);
        if (!o) goto done;
        size_t k = 0;
        memcpy(o, w, 5 * 4), k = 5;
        uint32_t gb = 0;
        for (uint32_t i = 0; i < ni; i++) {
            const uint32_t *p = w + s.off[i];
            uint32_t op = OPC(p[0]), wc = WCN(p[0]);
            if (i == s.first_fn) {
                for (uint32_t u = 0; u + 1 < s.undef_ty.n; u += 2) { o[k++] = (3u << 16) | 1u; o[k++] = s.undef_ty.a[u + 1]; o[k++] = s.undef_ty.a[u]; }
            }
            if (i < s.first_fn) {
                if (is_annot(op) && wc >= 2 && p[1] < s.bound && s.gone[p[1]]) { st->decos_dropped++; continue; }
                memcpy(o + k, p, (size_t)wc * 4), k += wc;
                continue;
            }
            if (s.rm[i]) { if (op == 248) gb++; continue; }
            size_t k0 = k;
            memcpy(o + k, p, (size_t)wc * 4), k += wc;
            for (uint32_t t = 1; t < wc; t++) if (idpos(op, t) == 1) o[k0 + t] = resolve(&s, o[k0 + t]);
            if (op == 248) {
                for (uint32_t pi = s.blk_phi_head.n > gb ? s.blk_phi_head.a[gb] : 0; pi; pi = s.phi_next[pi - 1]) {
                    phi_t *ph = &s.phi[pi - 1];
                    if (ph->dead) continue;
                    o[k++] = ((3u + 2u * ph->nops) << 16) | 245u, o[k++] = ph->ty, o[k++] = ph->id;
                    for (uint32_t t = 0; t < ph->nops; t++) o[k++] = resolve(&s, ph->val[t]), o[k++] = ph->plab[t];
                }
                gb++;
            }
        }
        o[3] = s.next;
        st->inst_after = 0, st->words_after = (uint32_t)k;
        for (size_t p = 5; p < k;) { p += WCN(o[p]); st->inst_after++; }
        *out = o, *outn = k;
        rc = 1;
    }
done:
    free(uses);
    free(lab2blk), free(vslot), free(blkbase), free(fn0), free(fn1);
    free(s.off), free(s.opof), free(s.tyof), free(s.defi), free(s.ptee), free(s.sc), free(s.gone), free(s.rm), free(s.repl), free(s.undef_of_ty), free(s.phi_next), free(s.nid_phi);
    for (uint32_t q = 0; q < s.nphi; q++) free(s.phi[q].val), free(s.phi[q].plab);
    free(s.phi);
    vec_free(&s.undef_ty), vec_free(&s.blk_phi_head);
    return rc;
}

/* ---------- env hook ---------- */
static lagfx_ssa_counters_t g_cnt;
void lagfx_spv_ssa_counters(lagfx_ssa_counters_t *c) { if (c) *c = g_cnt; }

unsigned lagfx_spv_ssa_env_mode(void) {
    const char *e = getenv("LAGFX_SPV_SSA");
    if (!e || !*e) return 0;
    char *end = NULL;
    unsigned long v = strtoul(e, &end, 10);
    return end && !*end && v <= 4 ? (unsigned)v : 0;
}

int lagfx_spv_ssa_apply_env(uint8_t **spv, size_t *sz, const char *what) {
    unsigned mode = lagfx_spv_ssa_env_mode();
    if (!mode || !spv || !*spv || !sz || (*sz & 3u) || *sz < 20) return 0;
    unsigned fl = mode == 1 ? 0u : mode == 2 ? LSSA_DCE : mode == 3 ? (LSSA_DCE | LSSA_DCE_INPUT) : (LSSA_DCE | LSSA_AGG);
    const char *mp = getenv("LAGFX_SPV_SSA_MAXPHI");   /* optional: do not promote locals that need an OpPhi wider than this */
    if (mp && *mp) { unsigned long mv = strtoul(mp, NULL, 10); if (mv && mv < 65536) fl |= LSSA_MAXPHI(mv); }
    uint32_t *w = NULL;
    size_t nw = 0;
    lagfx_ssa_stats_t st;
    char why[200];
    uint32_t *in = malloc(*sz);   /* the buffer is a byte pointer that need not be 4-aligned */
    if (!in) return 0;
    memcpy(in, *spv, *sz);
    g_cnt.modules++;
    int r = lagfx_spv_ssa_promote(in, *sz / 4, fl, &w, &nw, &st, why, sizeof why);
    free(in);
    if (r < 0) { g_cnt.failed++; LAGFX_WARN("spv-ssa %s: failed (out of memory or malformed); original kept", what ? what : "?"); return 0; }
    if (r == 0) { g_cnt.unchanged++; LAGFX_WARN("spv-ssa %s: unchanged (%s)", what ? what : "?", why); return 0; }
    char sc[160];
    if (!lagfx_red_selfcheck(w, nw, sc, sizeof sc)) { free(w); g_cnt.fallback++; LAGFX_WARN("spv-ssa %s: promoted module failed the self-check (%s); ORIGINAL kept", what ? what : "?", sc); return 0; }
    uint8_t *nb = malloc(nw * 4);
    if (!nb) { free(w); g_cnt.failed++; return 0; }
    memcpy(nb, w, nw * 4);
    free(w);
    g_cnt.changed++, g_cnt.vars += st.vars, g_cnt.promoted += st.promoted, g_cnt.phis += st.phis_kept, g_cnt.loads_removed += st.loads_removed, g_cnt.stores_removed += st.stores_removed;
    g_cnt.dce_removed += st.dce_removed, g_cnt.words_before += st.words_before, g_cnt.words_after += st.words_after;
    LAGFX_WARN("spv-ssa %s: mode=%u vars=%u promoted=%u escaped=%u agg-skipped=%u wide-skipped=%u phis=%u (placed %u, trivial %u, dead %u) undefs=%u loads-removed=%u stores-removed=%u dce-removed=%u inst %u->%u bytes %u->%u",
               what ? what : "?", mode, st.vars, st.promoted, st.escaped, st.skipped_agg, st.skipped_wide, st.phis_kept, st.phis_placed, st.phis_trivial, st.phis_dead, st.undefs, st.loads_removed, st.stores_removed, st.dce_removed,
               st.inst_before, st.inst_after, st.words_before * 4, st.words_after * 4);
    free(*spv);
    *spv = nb, *sz = nw * 4;
    return 1;
}
