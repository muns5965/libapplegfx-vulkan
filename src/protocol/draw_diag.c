/* libapplegfx-vulkan — bounded per-draw diagnostics. See draw_diag.h. Logging and counters only. */
#include "draw_diag.h"
#include "state.h"
#include "../common/log.h"
#include "../common/perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t a, b; uint64_t n; } dd_pair_t;
typedef struct { lagfx_draw_ev_t ev; uint64_t count, first_ms, last_ms; bool logged; } dd_entry_t;

struct lagfx_draw_diag {
    volatile int lock;
    dd_entry_t e[LAGFX_DRAW_DIAG_TABLE];
    uint32_t n;
    uint64_t draws, by_reason[LAGFX_DD_REASONS], overflow;   /* overflow = draws whose tuple did not fit the table */
    uint64_t multi, pair_other;     /* draws with >1 skip counter changed / those whose pair did not fit the pair table */
    dd_pair_t pr[LAGFX_DD_PAIRS]; uint32_t npr;
    uint64_t t0_ns;
    /* flush path only (no lock): */
    uint64_t prev_draws, prev_by[LAGFX_DD_REASONS], prev_multi, suppressed;
    uint64_t lines;                 /* written so far (only the flush path touches it, but tests read it) */
    uint64_t sum_draws_last;        /* draws at the last summary line */
    uint64_t flushes;
};

static const char *const ctr_name[LAGFX_DD_NCTR] = { "draws", "skipped", "no_pipeline", "ub_buf", "ub_input", "ub_storage_img", "ub_tex", "ub_view", "ub_samp", "no_target", "no_vertex_buf", "no_index_buf",
    "prim_unsupported", "indirect_bad", "desc_unaligned", "desc_feedback", "desc_failed", "point_no_size", "stub_fbfetch", "unnorm_skipped", "zero_draw", "bad_rec", "tile_pipeline" };
static const char *const reason_name[LAGFX_DD_REASONS] = { "ok", "no_pipeline", "unbound_tex", "unbound_buf", "other" };

static inline void dd_lock(struct lagfx_draw_diag *d) { while (__atomic_exchange_n(&d->lock, 1, __ATOMIC_ACQUIRE)) { } }
static inline void dd_unlock(struct lagfx_draw_diag *d) { __atomic_store_n(&d->lock, 0, __ATOMIC_RELEASE); }

struct lagfx_draw_diag *lagfx_draw_diag_get(lagfx_protocol_t *p) {
    if (!p) return NULL;
    if (p->draw_diag || p->draw_diag_init) return p->draw_diag;   /* hot path: one load, NULL when disabled */
    p->draw_diag_init = true;
    const char *e = getenv("G7_DRAW_DIAG");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')) return NULL;
    p->draw_diag = calloc(1, sizeof *p->draw_diag);
    if (p->draw_diag) p->draw_diag->t0_ns = lagfx_now_ns();
    return p->draw_diag;
}

void lagfx_draw_diag_free(lagfx_protocol_t *p) {
    if (!p) return;
    free(p->draw_diag);
    p->draw_diag = NULL;
}

uint64_t lagfx_draw_diag_lines(struct lagfx_draw_diag *d) { return d ? d->lines : 0; }

static bool same(const lagfx_draw_ev_t *a, const lagfx_draw_ev_t *b) {
    return a->pso == b->pso && a->vs == b->vs && a->fs == b->fs && a->reason == b->reason && a->why == b->why && a->bkind == b->bkind && a->stage == b->stage &&
           a->slot == b->slot && a->vkb == b->vkb && a->rt_w == b->rt_w && a->rt_h == b->rt_h && a->rt_fmt == b->rt_fmt;
}

void lagfx_draw_diag_record(struct lagfx_draw_diag *d, const lagfx_draw_ev_t *ev) {
    if (!d || !ev || ev->reason >= LAGFX_DD_REASONS) return;
    uint64_t ms = (lagfx_now_ns() - d->t0_ns) / 1000000ull;   /* read before the lock */
    uint32_t m = ev->chg & ((1u << LAGFX_DD_NCTR) - 1u);
    dd_lock(d);
    d->draws++, d->by_reason[ev->reason]++;
    uint32_t i = 0;
    for (; i < d->n; i++) if (same(&d->e[i].ev, ev)) { d->e[i].count++, d->e[i].last_ms = ms; break; }
    if (i == d->n) {
        if (d->n < LAGFX_DRAW_DIAG_TABLE) d->e[d->n].ev = *ev, d->e[d->n].count = 1, d->e[d->n].first_ms = d->e[d->n].last_ms = ms, d->e[d->n].logged = false, d->n++;
        else d->overflow++;
    }
    if (m & (m - 1u)) {   /* more than one bit: count the multi draw and each counter pair (a < b) */
        d->multi++;
        for (unsigned a = 0; a < LAGFX_DD_NCTR; a++) {
            if (!(m >> a & 1u)) continue;
            for (unsigned b = a + 1; b < LAGFX_DD_NCTR; b++) {
                if (!(m >> b & 1u)) continue;
                uint32_t k = 0;
                for (; k < d->npr; k++) if (d->pr[k].a == a && d->pr[k].b == b) { d->pr[k].n++; break; }
                if (k == d->npr) { if (d->npr < LAGFX_DD_PAIRS) d->pr[d->npr].a = (uint8_t)a, d->pr[d->npr].b = (uint8_t)b, d->pr[d->npr].n = 1, d->npr++; else d->pair_other++; }
            }
        }
    }
    dd_unlock(d);
}

static void emit(const char *l) {
    fprintf(stderr, "%s\n", l);
    LAGFX_LOG("%s", l);
}

static void fmt_entry(char *l, size_t n, const char *tag, const dd_entry_t *x) {   /* first / last in ms since start */
    const lagfx_draw_ev_t *v = &x->ev;
    char why[200];
    if (v->bkind) snprintf(why, sizeof why, "%s(%s slot=%d stage=%s vkb=%u ref=%u)", reason_name[v->reason], v->bkind == 1 ? "tex" : v->bkind == 2 ? "buf" : "bind", (int)v->slot, v->stage ? "fragment" : "vertex", (unsigned)v->vkb, (unsigned)v->bref);
    else if (v->why) snprintf(why, sizeof why, "%s(%s)", reason_name[v->reason], v->why);
    else snprintf(why, sizeof why, "%s", reason_name[v->reason]);
    snprintf(l, n, "%s task=%u pass=%llu draw=%u pso=%u vs=%08x fs=%08x reason=%s rt0=%ux%u fmt=%u n=%llu first=%llums last=%llums", tag, (unsigned)v->task, (unsigned long long)v->pass, (unsigned)v->draw,
             (unsigned)v->pso, (unsigned)v->vs, (unsigned)v->fs, why, (unsigned)v->rt_w, (unsigned)v->rt_h, (unsigned)v->rt_fmt, (unsigned long long)x->count, (unsigned long long)x->first_ms, (unsigned long long)x->last_ms);
}

/* True when the MAX_LINES cap (not the per-flush rate) is what stops further lines now. */
static bool budget_cap_hit(const struct lagfx_draw_diag *d, unsigned w) { return d->lines + w + LAGFX_DRAW_DIAG_RESERVE >= LAGFX_DRAW_DIAG_MAX_LINES; }

unsigned lagfx_draw_diag_flush(lagfx_protocol_t *p, bool final) {
    struct lagfx_draw_diag *d = p ? p->draw_diag : NULL;
    if (!d) return 0;
    dd_entry_t fresh[LAGFX_DRAW_DIAG_PER_FLUSH], top[8];
    dd_pair_t tp[3]; unsigned ntp = 0; uint64_t multi, pair_other; uint32_t unlogged = 0;
    unsigned nf = 0, nt = 0;
    uint64_t draws, by[LAGFX_DD_REASONS], ovf; uint32_t n;
    unsigned budget = d->lines + LAGFX_DRAW_DIAG_RESERVE >= LAGFX_DRAW_DIAG_MAX_LINES ? 0u : (unsigned)(LAGFX_DRAW_DIAG_MAX_LINES - LAGFX_DRAW_DIAG_RESERVE - d->lines);
    if (budget > LAGFX_DRAW_DIAG_PER_FLUSH) budget = LAGFX_DRAW_DIAG_PER_FLUSH;
    dd_lock(d);   /* copy only: no formatting, no I/O */
    for (uint32_t i = 0; i < d->n && nf < budget; i++) if (!d->e[i].logged) d->e[i].logged = true, fresh[nf++] = d->e[i];
    draws = d->draws, ovf = d->overflow, n = d->n, multi = d->multi, pair_other = d->pair_other;
    for (uint32_t i = 0; i < d->n; i++) if (!d->e[i].logged) unlogged++;
    memcpy(by, d->by_reason, sizeof by);
    bool summary = final ? true : draws != d->sum_draws_last;
    if (summary) {   /* the busiest non-ok tuples */
        bool used[LAGFX_DRAW_DIAG_TABLE] = {false};
        for (; nt < 8; nt++) {
            int b = -1; uint64_t bc = 0;
            for (uint32_t i = 0; i < d->n; i++) if (!used[i] && d->e[i].ev.reason != LAGFX_DD_OK && d->e[i].count > bc) bc = d->e[i].count, b = (int)i;
            if (b < 0) break;
            used[b] = true, top[nt] = d->e[b];
        }
        bool pu[LAGFX_DD_PAIRS] = {false};
        for (; ntp < 3; ntp++) {
            int b = -1; uint64_t bc = 0;
            for (uint32_t i = 0; i < d->npr; i++) if (!pu[i] && d->pr[i].n > bc) bc = d->pr[i].n, b = (int)i;
            if (b < 0) break;
            pu[b] = true, tp[ntp] = d->pr[b];
        }
        d->sum_draws_last = draws;
    }
    dd_unlock(d);

    unsigned w = 0;
    char l[512];
    for (unsigned i = 0; i < nf; i++) { fmt_entry(l, sizeof l, "g7-draw: new", &fresh[i]); emit(l); w++; }
    bool capped = budget_cap_hit(d, w);
    if (summary && (final ? d->lines + w < LAGFX_DRAW_DIAG_MAX_LINES : d->lines + w + 1 < LAGFX_DRAW_DIAG_MAX_LINES)) {
        char s[1400]; size_t o = 0;
        uint64_t sup = d->suppressed + (capped ? unlogged : 0);   /* lines never written because of the MAX_LINES cap: skipped summaries + tuples still unlogged at the cap */
        o += (size_t)snprintf(s, sizeof s, "g7-draw-summary:%s draws=%llu(+%llu) ok=%llu(+%llu) no_pipeline=%llu(+%llu) unbound_tex=%llu(+%llu) unbound_buf=%llu(+%llu) other=%llu(+%llu) distinct=%u/%u table_overflow_draws=%llu suppressed_lines=%llu multi_reason=%llu(+%llu) lines=%llu/%u",
                              final ? " final" : "", (unsigned long long)draws, (unsigned long long)(draws - d->prev_draws),
                              (unsigned long long)by[0], (unsigned long long)(by[0] - d->prev_by[0]), (unsigned long long)by[1], (unsigned long long)(by[1] - d->prev_by[1]),
                              (unsigned long long)by[2], (unsigned long long)(by[2] - d->prev_by[2]), (unsigned long long)by[3], (unsigned long long)(by[3] - d->prev_by[3]),
                              (unsigned long long)by[4], (unsigned long long)(by[4] - d->prev_by[4]), (unsigned)n, LAGFX_DRAW_DIAG_TABLE, (unsigned long long)ovf, (unsigned long long)sup,
                              (unsigned long long)multi, (unsigned long long)(multi - d->prev_multi), (unsigned long long)(d->lines + w + 1), LAGFX_DRAW_DIAG_MAX_LINES);
        o += (size_t)snprintf(s + o, sizeof s - o, " multi_pairs[");
        for (unsigned i = 0; i < ntp; i++) o += (size_t)snprintf(s + o, sizeof s - o, "%s%s+%s=%llu", i ? " " : "", ctr_name[tp[i].a], ctr_name[tp[i].b], (unsigned long long)tp[i].n);
        o += (size_t)snprintf(s + o, sizeof s - o, "%s] top[", pair_other ? " ..." : "");
        for (unsigned i = 0; i < nt && o + 120 < sizeof s; i++)
            o += (size_t)snprintf(s + o, sizeof s - o, "%s%s%s%s(s%d%c)@pso%u/%08x/%08x=%llu[%llu-%llums]", i ? " " : "", reason_name[top[i].ev.reason], top[i].ev.why ? ":" : "", top[i].ev.why ? top[i].ev.why : "",
                                  (int)top[i].ev.slot, top[i].ev.stage ? 'f' : 'v', (unsigned)top[i].ev.pso, (unsigned)top[i].ev.vs, (unsigned)top[i].ev.fs, (unsigned long long)top[i].count,
                                  (unsigned long long)top[i].first_ms, (unsigned long long)top[i].last_ms);
        snprintf(s + o, sizeof s - o, "]");
        emit(s); w++;
        d->prev_draws = draws, d->prev_multi = multi, memcpy(d->prev_by, by, sizeof by);
    } else if (summary) d->suppressed++;
    d->lines += w;   /* flush is only called from the statistics path (one caller at a time) */
    return w;
}
