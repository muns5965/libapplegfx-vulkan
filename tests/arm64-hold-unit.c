/* Hold ledger (src/protocol/arm64_hold.[ch]): per-phase counters, the slow-call threshold + line cap, the 10 s summary window (idle = silent),
 * the pure formatter, and the per-call overhead. CPU only. SPDX-License-Identifier: MIT */
#include "protocol/arm64_hold.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS 1000000ull
#define S  1000000000ull

static char g_log[256];
static size_t count_in_log(const char *needle) {
    FILE *f = fopen(g_log, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = 0;
    while (fgets(buf, sizeof buf, f)) if (strstr(buf, needle)) n++;
    fclose(f);
    return n;
}

int main(void) {
    snprintf(g_log, sizeof g_log, "/tmp/arm64-hold-unit-%d.log", (int)getpid());
    remove(g_log);
    setenv("LAGFX_LOG_FILE", g_log, 1);
    setenv("LAGFX_LOG_LEVEL", "info", 1);
    setenv("LAGFX_SLOW_MS", "5", 1);
    lagfx_hold_reset();

    /* 1. counters: n, total, max, bytes per phase; phases are independent */
    lagfx_hold_stats_t st;
    lagfx_hold_note_dur(LAGFX_HOLD_DRAIN, 2 * MS, 0, 1 * S);
    lagfx_hold_note_dur(LAGFX_HOLD_DRAIN, 3 * MS, 0, 1 * S);
    lagfx_hold_note_dur(LAGFX_HOLD_EXEC_END, 4 * MS, 1000, 1 * S);
    lagfx_hold_get(&st);
    assert(st.ph[LAGFX_HOLD_DRAIN].n == 2 && st.ph[LAGFX_HOLD_DRAIN].total_ns == 5 * MS && st.ph[LAGFX_HOLD_DRAIN].max_ns == 3 * MS);
    assert(st.ph[LAGFX_HOLD_EXEC_END].n == 1 && st.ph[LAGFX_HOLD_EXEC_END].bytes == 1000 && st.ph[LAGFX_HOLD_POLL].n == 0);
    assert(st.ph[LAGFX_HOLD_DRAIN].slow_n == 0 && st.slow_lines == 0);   /* all below the 5 ms threshold */
    assert(count_in_log("g7-gpu-slow") == 0);

    /* 2. a call at / above the threshold is counted slow and logged once with its phase name; just below is not */
    lagfx_hold_note_dur(LAGFX_HOLD_PIPE_FAIL, 5 * MS - 1, 0, 1 * S);
    lagfx_hold_note_dur(LAGFX_HOLD_PIPE_FAIL, 661 * MS, 0, 1 * S);
    lagfx_hold_get(&st);
    assert(st.ph[LAGFX_HOLD_PIPE_FAIL].n == 2 && st.ph[LAGFX_HOLD_PIPE_FAIL].slow_n == 1 && st.ph[LAGFX_HOLD_PIPE_FAIL].max_ns == 661 * MS);
    assert(st.slow_lines == 1 && count_in_log("g7-gpu-slow: phase=pipe_fail ms=661.0") == 1);

    /* 3. the line cap: 100 lines, the rest only counted (counters stay exact) */
    for (int i = 0; i < 150; i++) lagfx_hold_note_dur(LAGFX_HOLD_RESOLVE, 6 * MS, 0, 1 * S);
    lagfx_hold_get(&st);
    assert(st.ph[LAGFX_HOLD_RESOLVE].n == 150 && st.ph[LAGFX_HOLD_RESOLVE].slow_n == 150);
    assert(count_in_log("g7-gpu-slow") == 100 && st.slow_suppressed == 51);   /* 1 pipe_fail line + 99 resolve lines; 51 suppressed */

    /* 4. threshold 0 turns the slow lines off, counters keep going */
    setenv("LAGFX_SLOW_MS", "0", 1);
    lagfx_hold_reset();
    lagfx_hold_note_dur(LAGFX_HOLD_DRAIN, 900 * MS, 0, 1 * S);
    lagfx_hold_get(&st);
    assert(st.ph[LAGFX_HOLD_DRAIN].n == 1 && st.ph[LAGFX_HOLD_DRAIN].slow_n == 0 && st.slow_lines == 0);
    assert(count_in_log("g7-gpu-slow") == 100);   /* nothing new after the reset */
    assert(truncate(g_log, 0) == 0);              /* the library keeps its append-mode handle open: empty the file, do not unlink it */

    /* 5. summary window: the first record opens it; a record >= 10 s later emits ONE line covering the window; idle windows emit nothing */
    setenv("LAGFX_SLOW_MS", "20", 1);
    lagfx_hold_reset();
    lagfx_hold_note_dur(LAGFX_HOLD_DRAIN, 1 * MS, 0, 100 * S);          /* opens the window at t=100 s */
    lagfx_hold_note_dur(LAGFX_HOLD_EXEC_END, 2 * MS, 4 << 20, 105 * S);
    assert(count_in_log("g7-gpu-hold") == 0);                            /* 5 s: still open */
    lagfx_hold_note_dur(LAGFX_HOLD_DRAIN, 1 * MS, 0, 110 * S + 1);       /* 10 s: emits */
    assert(count_in_log("g7-gpu-hold") == 1);
    assert(count_in_log("drain[n=2 ms=2.0") == 1 && count_in_log("exec_end[n=1 ms=2.0") == 1 && count_in_log("mb=4.0") == 1);
    lagfx_hold_note_dur(LAGFX_HOLD_POLL, 1 * MS, 0, 125 * S);            /* next window: only the poll happened in it */
    assert(count_in_log("g7-gpu-hold") == 2 && count_in_log("poll[n=1 ms=1.0") == 1 && count_in_log("drain[n=0 ms=0.0") == 1);
    lagfx_hold_get(&st);
    assert(st.summaries == 2);
    /* a record that arrives after a long idle gap emits the (empty-but-for-itself) previous window once, not once per missed window */
    lagfx_hold_note_dur(LAGFX_HOLD_POLL, 1 * MS, 0, 1000 * S);
    assert(count_in_log("g7-gpu-hold") == 3);

    /* 6. the formatter is pure and truncates safely */
    lagfx_hold_stats_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.ph[LAGFX_HOLD_DRAIN].n = 7, a.ph[LAGFX_HOLD_DRAIN].total_ns = 21 * MS, a.ph[LAGFX_HOLD_DRAIN].max_ns = 9 * MS;
    b.ph[LAGFX_HOLD_DRAIN].n = 2, b.ph[LAGFX_HOLD_DRAIN].total_ns = 6 * MS;
    char line[768];
    int len = lagfx_hold_format(line, sizeof line, &a, &b, 10.0);
    assert(len > 0 && (size_t)len < sizeof line && strstr(line, "drain[n=5 ms=15.0 max=9.0 slow=0]") && strstr(line, "win=10.0s"));
    char tiny[16];
    len = lagfx_hold_format(tiny, sizeof tiny, &a, &b, 10.0);
    assert(len > 0 && strlen(tiny) < sizeof tiny);
    assert(lagfx_hold_name((lagfx_hold_phase_t)99)[0] == '?' && !strcmp(lagfx_hold_name(LAGFX_HOLD_EXEC_END), "exec_end"));

    /* 7. overhead of a real note (two CLOCK_MONOTONIC reads + a few relaxed atomics): the ledger sits on the doorbell path, it must stay ~100 ns */
    lagfx_hold_reset();
    setenv("LAGFX_SLOW_MS", "0", 1);
    enum { N = 200000 };
    uint64_t t0 = lagfx_hold_now_ns();
    for (int i = 0; i < N; i++) { uint64_t s = lagfx_hold_now_ns(); lagfx_hold_note(LAGFX_HOLD_POLL, s, 0); }
    uint64_t per = (lagfx_hold_now_ns() - t0) / N;
    printf("hold ledger: %llu ns per timed call (clock read + note)\n", (unsigned long long)per);
    assert(per < 5000);   /* generous: a loaded CI box; typical is 50-100 ns */

    remove(g_log);
    puts("arm64-hold-unit: OK");
    return 0;
}
