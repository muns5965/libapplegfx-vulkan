/* libapplegfx-vulkan — bounded per-draw diagnostics. See draw_diag.h. Logging and counters only. */
#include "draw_diag.h"
#include "state.h"
#include "../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lagfx_draw_ev_t ev; uint64_t count; bool logged; } dd_entry_t;

struct lagfx_draw_diag {
    volatile int lock;
    dd_entry_t e[LAGFX_DRAW_DIAG_TABLE];
    uint32_t n;
    uint64_t draws, by_reason[LAGFX_DD_REASONS], overflow;
    uint64_t lines;                 /* written so far (only the flush path touches it, but tests read it) */
    uint64_t sum_draws_last;        /* draws at the last summary line */
    uint64_t flushes;
};

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
    dd_lock(d);
    d->draws++, d->by_reason[ev->reason]++;
    uint32_t i = 0;
    for (; i < d->n; i++) if (same(&d->e[i].ev, ev)) { d->e[i].count++; break; }
    if (i == d->n) {
        if (d->n < LAGFX_DRAW_DIAG_TABLE) d->e[d->n].ev = *ev, d->e[d->n].count = 1, d->e[d->n].logged = false, d->n++;
        else d->overflow++;
    }
    dd_unlock(d);
}

static void emit(const char *l) {
    fprintf(stderr, "%s\n", l);
    LAGFX_LOG("%s", l);
}

static void fmt_entry(char *l, size_t n, const char *tag, const dd_entry_t *x) {
    const lagfx_draw_ev_t *v = &x->ev;
    char why[200];
    if (v->bkind) snprintf(why, sizeof why, "%s(%s slot=%d stage=%s vkb=%u ref=%u)", reason_name[v->reason], v->bkind == 1 ? "tex" : v->bkind == 2 ? "buf" : "bind", (int)v->slot, v->stage ? "fragment" : "vertex", (unsigned)v->vkb, (unsigned)v->bref);
    else if (v->why) snprintf(why, sizeof why, "%s(%s)", reason_name[v->reason], v->why);
    else snprintf(why, sizeof why, "%s", reason_name[v->reason]);
    snprintf(l, n, "%s task=%u pass=%llu draw=%u pso=%u vs=%08x fs=%08x reason=%s rt0=%ux%u fmt=%u n=%llu", tag, (unsigned)v->task, (unsigned long long)v->pass, (unsigned)v->draw,
             (unsigned)v->pso, (unsigned)v->vs, (unsigned)v->fs, why, (unsigned)v->rt_w, (unsigned)v->rt_h, (unsigned)v->rt_fmt, (unsigned long long)x->count);
}

unsigned lagfx_draw_diag_flush(lagfx_protocol_t *p, bool final) {
    struct lagfx_draw_diag *d = p ? p->draw_diag : NULL;
    if (!d) return 0;
    dd_entry_t fresh[LAGFX_DRAW_DIAG_PER_FLUSH], top[8];
    unsigned nf = 0, nt = 0;
    uint64_t draws, by[LAGFX_DD_REASONS], ovf; uint32_t n;
    unsigned budget = d->lines + LAGFX_DRAW_DIAG_RESERVE >= LAGFX_DRAW_DIAG_MAX_LINES ? 0u : (unsigned)(LAGFX_DRAW_DIAG_MAX_LINES - LAGFX_DRAW_DIAG_RESERVE - d->lines);
    if (budget > LAGFX_DRAW_DIAG_PER_FLUSH) budget = LAGFX_DRAW_DIAG_PER_FLUSH;
    dd_lock(d);   /* copy only: no formatting, no I/O */
    for (uint32_t i = 0; i < d->n && nf < budget; i++) if (!d->e[i].logged) d->e[i].logged = true, fresh[nf++] = d->e[i];
    draws = d->draws, ovf = d->overflow, n = d->n;
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
        d->sum_draws_last = draws;
    }
    dd_unlock(d);

    unsigned w = 0;
    char l[512];
    for (unsigned i = 0; i < nf; i++) { fmt_entry(l, sizeof l, "g7-draw: new", &fresh[i]); emit(l); w++; }
    if (summary && (final ? d->lines + w < LAGFX_DRAW_DIAG_MAX_LINES : d->lines + w + 1 < LAGFX_DRAW_DIAG_MAX_LINES)) {
        char s[900]; size_t o = 0;
        o += (size_t)snprintf(s, sizeof s, "g7-draw-summary:%s draws=%llu ok=%llu no_pipeline=%llu unbound_tex=%llu unbound_buf=%llu other=%llu distinct=%u/%u overflow=%llu lines=%llu/%u top[",
                              final ? " final" : "", (unsigned long long)draws, (unsigned long long)by[0], (unsigned long long)by[1], (unsigned long long)by[2], (unsigned long long)by[3],
                              (unsigned long long)by[4], (unsigned)n, LAGFX_DRAW_DIAG_TABLE, (unsigned long long)ovf, (unsigned long long)(d->lines + w + 1), LAGFX_DRAW_DIAG_MAX_LINES);
        for (unsigned i = 0; i < nt && o + 80 < sizeof s; i++)
            o += (size_t)snprintf(s + o, sizeof s - o, "%s%s%s%s(s%d%c)@pso%u/%08x/%08x=%llu", i ? " " : "", reason_name[top[i].ev.reason], top[i].ev.why ? ":" : "", top[i].ev.why ? top[i].ev.why : "",
                                  (int)top[i].ev.slot, top[i].ev.stage ? 'f' : 'v', (unsigned)top[i].ev.pso, (unsigned)top[i].ev.vs, (unsigned)top[i].ev.fs, (unsigned long long)top[i].count);
        snprintf(s + o, sizeof s - o, "]");
        emit(s); w++;
    }
    d->lines += w;   /* flush is only called from the statistics path (one caller at a time) */
    return w;
}
