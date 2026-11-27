/*
 * libapplegfx-vulkan — hold ledger (see arm64_hold.h). Measurement only: nothing here changes what the library does.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_hold.h"
#include "../common/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOLD_WINDOW_NS     10000000000ull   /* the shell's watchdog and the library's g7-gpu lines use 10 s windows */
#define HOLD_SLOW_DEFAULT  20u              /* ms */
#define HOLD_SLOW_LINES    100u

typedef struct { _Atomic uint64_t n, total_ns, max_ns, slow_n, bytes; } acell_t;
static acell_t g_cell[LAGFX_HOLD_COUNT];
static _Atomic uint64_t g_slow_lines, g_slow_suppressed, g_summaries;
static _Atomic uint64_t g_last_emit_ns;       /* 0 = no window open yet */
static _Atomic int64_t  g_slow_ms = -1;       /* -1 = env not read yet */
static lagfx_hold_stats_t g_since;            /* snapshot at the last summary; only the thread that wins the g_last_emit_ns exchange touches it */

const char *lagfx_hold_name(lagfx_hold_phase_t ph) {
    static const char *const nm[LAGFX_HOLD_COUNT] = { "drain", "poll", "exec_end", "resolve", "pipe_build", "pipe_fail" };
    return (unsigned)ph < LAGFX_HOLD_COUNT ? nm[ph] : "?";
}

uint64_t lagfx_hold_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t slow_ms(void) {
    int64_t v = atomic_load_explicit(&g_slow_ms, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("LAGFX_SLOW_MS");
        char *end = NULL;
        unsigned long long x = HOLD_SLOW_DEFAULT;
        if (e && *e) { unsigned long long y = strtoull(e, &end, 10); if (end && !*end) x = y; }
        if (x > 3600000ull) x = 3600000ull;
        v = (int64_t)x;
        atomic_store_explicit(&g_slow_ms, v, memory_order_relaxed);
    }
    return (uint64_t)v;
}

void lagfx_hold_get(lagfx_hold_stats_t *out) {
    if (!out) return;
    for (int i = 0; i < LAGFX_HOLD_COUNT; i++) {
        out->ph[i].n = atomic_load_explicit(&g_cell[i].n, memory_order_relaxed);
        out->ph[i].total_ns = atomic_load_explicit(&g_cell[i].total_ns, memory_order_relaxed);
        out->ph[i].max_ns = atomic_load_explicit(&g_cell[i].max_ns, memory_order_relaxed);
        out->ph[i].slow_n = atomic_load_explicit(&g_cell[i].slow_n, memory_order_relaxed);
        out->ph[i].bytes = atomic_load_explicit(&g_cell[i].bytes, memory_order_relaxed);
    }
    out->slow_lines = atomic_load_explicit(&g_slow_lines, memory_order_relaxed);
    out->slow_suppressed = atomic_load_explicit(&g_slow_suppressed, memory_order_relaxed);
    out->summaries = atomic_load_explicit(&g_summaries, memory_order_relaxed);
}

void lagfx_hold_reset(void) {
    for (int i = 0; i < LAGFX_HOLD_COUNT; i++) {
        atomic_store(&g_cell[i].n, 0); atomic_store(&g_cell[i].total_ns, 0); atomic_store(&g_cell[i].max_ns, 0);
        atomic_store(&g_cell[i].slow_n, 0); atomic_store(&g_cell[i].bytes, 0);
    }
    atomic_store(&g_slow_lines, 0); atomic_store(&g_slow_suppressed, 0); atomic_store(&g_summaries, 0);
    atomic_store(&g_last_emit_ns, 0);
    atomic_store(&g_slow_ms, -1);
    memset(&g_since, 0, sizeof g_since);
}

static int fmt_cell(char *b, size_t cap, const char *name, const lagfx_hold_cell_t *a, const lagfx_hold_cell_t *s, int with_bytes) {
    uint64_t n = a->n - s->n, t = a->total_ns - s->total_ns;
    /* max_ns is cumulative (a window maximum cannot be derived from two snapshots); the per-window slow_n says whether it happened in this window */
    int w = snprintf(b, cap, " %s[n=%llu ms=%.1f max=%.1f slow=%llu", name, (unsigned long long)n, (double)t / 1e6, (double)a->max_ns / 1e6, (unsigned long long)(a->slow_n - s->slow_n));
    if (with_bytes && w > 0 && (size_t)w < cap) w += snprintf(b + w, cap - (size_t)w, " mb=%.1f", (double)(a->bytes - s->bytes) / 1048576.0);
    if (w > 0 && (size_t)w < cap) w += snprintf(b + w, cap - (size_t)w, "]");
    return w;
}

int lagfx_hold_format(char *buf, size_t cap, const lagfx_hold_stats_t *now, const lagfx_hold_stats_t *since, double win_s) {
    if (!buf || !cap || !now || !since) return 0;
    int len = snprintf(buf, cap, "g7-gpu-hold: win=%.1fs (phases are nested: drain contains exec_end/resolve/pipe_*; max is cumulative since start)", win_s);
    for (int i = 0; i < LAGFX_HOLD_COUNT && len > 0 && (size_t)len < cap; i++)
        len += fmt_cell(buf + len, cap - (size_t)len, lagfx_hold_name((lagfx_hold_phase_t)i), &now->ph[i], &since->ph[i], i == LAGFX_HOLD_EXEC_END);
    return len;
}

static void maybe_summary(uint64_t now_ns) {
    uint64_t last = atomic_load_explicit(&g_last_emit_ns, memory_order_relaxed);
    if (!last) {   /* first record opens the first window */
        uint64_t zero = 0;
        if (atomic_compare_exchange_strong(&g_last_emit_ns, &zero, now_ns)) memset(&g_since, 0, sizeof g_since);
        return;
    }
    if (now_ns < last || now_ns - last < HOLD_WINDOW_NS) return;
    if (!atomic_compare_exchange_strong(&g_last_emit_ns, &last, now_ns)) return;   /* another thread emits this window */
    lagfx_hold_stats_t cur;
    lagfx_hold_get(&cur);
    int any = 0;
    for (int i = 0; i < LAGFX_HOLD_COUNT; i++) any |= cur.ph[i].n != g_since.ph[i].n;
    if (any) {
        char line[768];
        lagfx_hold_format(line, sizeof line, &cur, &g_since, (double)(now_ns - last) / 1e9);
        atomic_fetch_add_explicit(&g_summaries, 1, memory_order_relaxed);
        LAGFX_LOG("%s", line);
    }
    g_since = cur;
}

void lagfx_hold_note_dur(lagfx_hold_phase_t ph, uint64_t dur_ns, uint64_t bytes, uint64_t now_ns) {
    if ((unsigned)ph >= LAGFX_HOLD_COUNT) return;
    acell_t *c = &g_cell[ph];
    atomic_fetch_add_explicit(&c->n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->total_ns, dur_ns, memory_order_relaxed);
    if (bytes) atomic_fetch_add_explicit(&c->bytes, bytes, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&c->max_ns, memory_order_relaxed);
    while (dur_ns > m && !atomic_compare_exchange_weak_explicit(&c->max_ns, &m, dur_ns, memory_order_relaxed, memory_order_relaxed)) {}
    uint64_t thr = slow_ms();
    if (thr && dur_ns >= thr * 1000000ull) {
        atomic_fetch_add_explicit(&c->slow_n, 1, memory_order_relaxed);
        if (atomic_fetch_add_explicit(&g_slow_lines, 1, memory_order_relaxed) < HOLD_SLOW_LINES)
            LAGFX_WARN("g7-gpu-slow: phase=%s ms=%.1f bytes=%llu thr=%llums (one call; the shell's device lock was held at least this long if it holds it around the call)",
                       lagfx_hold_name(ph), (double)dur_ns / 1e6, (unsigned long long)bytes, (unsigned long long)thr);
        else atomic_fetch_add_explicit(&g_slow_suppressed, 1, memory_order_relaxed);
    }
    maybe_summary(now_ns);
}

void lagfx_hold_note(lagfx_hold_phase_t ph, uint64_t t0_ns, uint64_t bytes) {
    uint64_t now = lagfx_hold_now_ns();
    lagfx_hold_note_dur(ph, now >= t0_ns ? now - t0_ns : 0, bytes, now);
}
