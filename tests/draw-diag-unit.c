/* Bounded per-draw diagnostics: dedup by tuple with counters, hard line cap, per-flush rate limit, table overflow, no flush work when idle, and no allocation when disabled. SPDX-License-Identifier: MIT */
#include "protocol/draw_diag.h"
#include "protocol/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long file_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[2048]; long n = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    fclose(f); return n;
}

static lagfx_draw_ev_t ev(uint32_t pso, uint8_t reason) {
    lagfx_draw_ev_t e; memset(&e, 0, sizeof e);
    e.task = 5, e.pass = 12, e.draw = 3, e.pso = pso, e.vs = 0xab12cd34u, e.fs = 0x99887766u, e.reason = reason, e.slot = -1, e.rt_w = 1170, e.rt_h = 2532, e.rt_fmt = 37;
    return e;
}

int main(void) {
    char path[] = "/tmp/lagfx-draw-diag-unit-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    setenv("LAGFX_LOG_FILE", path, 1); setenv("LAGFX_LOG_LEVEL", "info", 1);

    /* disabled: get() is NULL, nothing allocated, flush and record are no-ops */
    setenv("G7_DRAW_DIAG", "0", 1);
    lagfx_protocol_t *off = calloc(1, sizeof *off);
    assert(lagfx_draw_diag_get(off) == NULL && off->draw_diag == NULL && off->draw_diag_init);
    assert(lagfx_draw_diag_get(off) == NULL);
    lagfx_draw_ev_t e0 = ev(1, LAGFX_DD_OK);
    lagfx_draw_diag_record(NULL, &e0);
    assert(lagfx_draw_diag_flush(off, true) == 0 && file_lines(path, "g7-draw") == 0);
    lagfx_draw_diag_free(off); free(off);

    /* default (unset) = on */
    unsetenv("G7_DRAW_DIAG");
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    struct lagfx_draw_diag *dd = lagfx_draw_diag_get(p);
    assert(dd && lagfx_draw_diag_get(p) == dd);

    /* dedup: 1000 identical draws -> one tuple line with a counter; a different pso / reason / binding / target is a new tuple */
    lagfx_draw_ev_t e = ev(7, LAGFX_DD_OK);
    for (int i = 0; i < 1000; i++) lagfx_draw_diag_record(dd, &e);
    lagfx_draw_ev_t t = ev(7, LAGFX_DD_UNBOUND_TEX); t.bkind = 1, t.stage = 1, t.slot = 2, t.vkb = 3, t.bref = 17;
    for (int i = 0; i < 40; i++) lagfx_draw_diag_record(dd, &t);
    lagfx_draw_ev_t t2 = t; t2.slot = 4;                       /* another unbound slot: its own tuple */
    lagfx_draw_diag_record(dd, &t2);
    lagfx_draw_ev_t np = ev(8, LAGFX_DD_NO_PIPELINE); np.why = "none_set"; np.rt_w = 0, np.rt_h = 0, np.rt_fmt = 0;
    lagfx_draw_diag_record(dd, &np);
    lagfx_draw_ev_t t3 = t; t3.pass = 99, t3.draw = 9, t3.task = 6;   /* task / pass / draw are not part of the tuple */
    lagfx_draw_diag_record(dd, &t3);
    assert(dd && lagfx_draw_diag_flush(p, false) == 5);        /* 4 new tuples + 1 summary */
    assert(file_lines(path, "g7-draw: new") == 4 && file_lines(path, "g7-draw-summary") == 1);
    assert(file_lines(path, "reason=unbound_tex(tex slot=2 stage=fragment vkb=3 ref=17)") == 1 && file_lines(path, "n=41") == 1 && file_lines(path, "n=1000") == 1);
    assert(file_lines(path, "pso=7 vs=ab12cd34 fs=99887766") == 3 && file_lines(path, "rt0=1170x2532 fmt=37") == 3 && file_lines(path, "reason=no_pipeline(none_set)") == 1);
    assert(file_lines(path, "draws=1043 ok=1000 no_pipeline=1 unbound_tex=42") == 1);

    /* idle: nothing new -> no new tuple lines and no summary */
    assert(lagfx_draw_diag_flush(p, false) == 0);
    /* a repeat of a logged tuple is counted, not logged again; the summary reports the changed totals */
    lagfx_draw_diag_record(dd, &t);
    assert(lagfx_draw_diag_flush(p, false) == 1 && file_lines(path, "g7-draw: new") == 4);

    /* rate: 100 distinct tuples -> at most PER_FLUSH new lines per flush (+ summary) */
    for (uint32_t i = 0; i < 100; i++) { lagfx_draw_ev_t x = ev(1000 + i, LAGFX_DD_OTHER); x.why = "no_target"; lagfx_draw_diag_record(dd, &x); }
    assert(lagfx_draw_diag_flush(p, false) == LAGFX_DRAW_DIAG_PER_FLUSH + 1);

    /* table bound: tuples beyond the table are counted in overflow only */
    for (uint32_t i = 0; i < 500; i++) { lagfx_draw_ev_t x = ev(5000 + i, LAGFX_DD_OTHER); x.why = "no_target"; lagfx_draw_diag_record(dd, &x); }
    for (int k = 0; k < 40; k++) lagfx_draw_diag_flush(p, false);
    assert(file_lines(path, "g7-draw: new") <= (long)LAGFX_DRAW_DIAG_TABLE);
    assert(file_lines(path, "g7-draw: new") == (long)LAGFX_DRAW_DIAG_TABLE);   /* all kept tuples reached the log, no more */
    assert(file_lines(path, "overflow=0") < file_lines(path, "g7-draw-summary"));
    assert(lagfx_draw_diag_flush(p, true) >= 1 && file_lines(path, "g7-draw-summary: final") == 1);
    free(p->draw_diag); p->draw_diag = NULL; free(p);

    /* hard cap: many flushes with changing totals never exceed MAX_LINES in total */
    lagfx_protocol_t *q = calloc(1, sizeof *q);
    struct lagfx_draw_diag *d2 = lagfx_draw_diag_get(q);
    long before = file_lines(path, "g7-draw");
    for (int k = 0; k < 2000; k++) { lagfx_draw_ev_t x = ev(1u + (uint32_t)(k % 100), LAGFX_DD_OTHER); x.why = "bad_rec"; x.rt_w = (uint32_t)k; lagfx_draw_diag_record(d2, &x); lagfx_draw_diag_flush(q, false); }
    lagfx_draw_diag_flush(q, true);
    assert(lagfx_draw_diag_lines(d2) <= LAGFX_DRAW_DIAG_MAX_LINES);
    assert(file_lines(path, "g7-draw") - before == (long)lagfx_draw_diag_lines(d2));
    assert(lagfx_draw_diag_lines(d2) >= LAGFX_DRAW_DIAG_MAX_LINES - LAGFX_DRAW_DIAG_RESERVE);
    /* lines stay at the cap afterwards */
    for (int k = 0; k < 10; k++) { lagfx_draw_ev_t x = ev(77, LAGFX_DD_OK); x.rt_h = (uint32_t)k; lagfx_draw_diag_record(d2, &x); lagfx_draw_diag_flush(q, true); }
    assert(lagfx_draw_diag_lines(d2) <= LAGFX_DRAW_DIAG_MAX_LINES);
    lagfx_draw_diag_free(q); free(q);

    unlink(path);
    puts("draw-diag-unit: ok");
    return 0;
}
