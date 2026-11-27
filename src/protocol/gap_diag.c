/*
 * libapplegfx-vulkan — idle-gap diagnostics. See gap_diag.h. Logging and counters only; nothing here can change what the guest sees.
 * SPDX-License-Identifier: MIT
 */
#include "gap_diag.h"
#include "display_diag.h"
#include "../common/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_last_ns, g_t0_ns, g_pk;   /* g_pk: (ch << 56 | op << 40 | per-op count) of the latest command, for the line about the command BEFORE a gap */
static _Atomic uint32_t g_win_swaps, g_win_passes, g_win_draws;
static _Atomic uint64_t g_cnt_unmap, g_cnt_obj, g_cnt_drop, g_cnt_define, g_cnt_setobj;   /* since the previous op 0x0009 */
/* the rest is touched under g_mu */
static uint64_t g_gaps, g_longest_ms, g_op09, g_swaps, g_cmds;
static uint64_t g_first_swap_ns, g_last_swap_ns, g_last_swap_mapping, g_gap_end_ns, g_gap_ms_last, g_prev_op09_ns;
static uint64_t g_mapping_before_gap;
static uint32_t g_swaps_after_gap, g_passes_after_gap, g_draws_after_gap;
static lagfx_gap_sink_t g_sink;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool lagfx_gap_enabled(void) { const char *e = getenv("LAGFX_GAPDIAG"); return !(e && e[0] == '0' && !e[1]); }
uint64_t lagfx_gap_threshold_ms(void) {
    const char *e = getenv("LAGFX_GAPDIAG_GAP_MS");
    long long v = e ? strtoll(e, NULL, 10) : 0;
    return v > 0 ? (uint64_t)v : LAGFX_GAP_DEFAULT_MS;
}

void lagfx_gap_set_sink(lagfx_gap_sink_t s) { g_sink = s; }
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[1800];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (g_sink) { g_sink(line); return; }
    fprintf(stderr, "%s\n", line);   /* like the g7-pvg-ops lines: reaches the runner's log too */
    LAGFX_LOG("%s", line);
}

void lagfx_gap_reset(void) {
    pthread_mutex_lock(&g_mu);
    atomic_store(&g_last_ns, 0), atomic_store(&g_t0_ns, 0);
    atomic_store(&g_win_swaps, 0), atomic_store(&g_win_passes, 0), atomic_store(&g_win_draws, 0);
    atomic_store(&g_cnt_unmap, 0), atomic_store(&g_cnt_obj, 0), atomic_store(&g_cnt_drop, 0), atomic_store(&g_cnt_define, 0), atomic_store(&g_cnt_setobj, 0);
    g_gaps = g_longest_ms = g_op09 = g_swaps = g_cmds = 0;
    g_first_swap_ns = g_last_swap_ns = g_last_swap_mapping = g_gap_end_ns = g_gap_ms_last = g_prev_op09_ns = g_mapping_before_gap = 0;
    atomic_store(&g_pk, 0);
    g_swaps_after_gap = g_passes_after_gap = g_draws_after_gap = 0;
    pthread_mutex_unlock(&g_mu);
}
void lagfx_gap_window(uint32_t *s, uint32_t *p, uint32_t *d) { if (s) *s = atomic_load(&g_win_swaps); if (p) *p = atomic_load(&g_win_passes); if (d) *d = atomic_load(&g_win_draws); }
uint64_t lagfx_gap_count(void) { pthread_mutex_lock(&g_mu); uint64_t g = g_gaps; pthread_mutex_unlock(&g_mu); return g; }

static uint64_t ms_since(uint64_t a, uint64_t b) { return (a && b >= a) ? (b - a) / 1000000ull : 0; }

void lagfx_gap_note_cmd(uint64_t now_ns, uint64_t t0_ns, int src, uint32_t chan, uint64_t seq, uint64_t op_n, const uint8_t *raw, uint32_t length) {
    if (!raw || length < 12u || !lagfx_gap_enabled()) return;
    uint32_t op = rd32(raw) & 0xffffu;
    uint64_t z = 0;
    atomic_compare_exchange_strong(&g_t0_ns, &z, t0_ns ? t0_ns : now_ns);
    /* fast path: counters for the op 0x0009 line, then the silence test */
    if (src == 1) {
        if (op == 0x22u) atomic_fetch_add_explicit(&g_cnt_unmap, 1, memory_order_relaxed);
        else if (op == 0x25u) atomic_fetch_add_explicit(&g_cnt_obj, 1, memory_order_relaxed);
        else if (op == 0x20u) atomic_fetch_add_explicit(&g_cnt_drop, 1, memory_order_relaxed);
        else if (op == 0x38u) atomic_fetch_add_explicit(&g_cnt_define, 1, memory_order_relaxed);
        else if (op == 0x44u) atomic_fetch_add_explicit(&g_cnt_setobj, 1, memory_order_relaxed);
    }
    uint64_t prev = atomic_exchange_explicit(&g_last_ns, now_ns, memory_order_relaxed);
    uint64_t pk = atomic_exchange_explicit(&g_pk, ((uint64_t)(chan & 0xffu) << 56) | ((uint64_t)op << 40) | (op_n & 0xffffffffffull), memory_order_relaxed);
    const uint32_t prev_ch = (uint32_t)(pk >> 56), prev_op = (uint32_t)((pk >> 40) & 0xffffu); const uint64_t prev_n = (uint64_t)(pk & 0xffffffffffull);
    const uint64_t thr_ns = lagfx_gap_threshold_ms() * 1000000ull;
    const bool is_gap = prev && now_ns > prev && now_ns - prev >= thr_ns;
    const bool is_swap = src == 1 && chan == 4u && op == 0x08u;
    const bool is_op09 = src == 1 && chan == 4u && op == 0x09u;
    if (!is_gap && !is_swap && !is_op09) { return; }   /* the usual command: nothing more to do (g_cmds is not counted here, seq is passed in) */

    pthread_mutex_lock(&g_mu);
    g_cmds = seq;
    uint64_t t0 = atomic_load(&g_t0_ns);
    if (is_gap) {
        uint64_t gap_ms = (now_ns - prev) / 1000000ull;
        g_gaps++;
        if (gap_ms > g_longest_ms) g_longest_ms = gap_ms;
        g_gap_ms_last = gap_ms, g_gap_end_ns = now_ns, g_mapping_before_gap = g_last_swap_mapping;
        g_swaps_after_gap = g_passes_after_gap = g_draws_after_gap = 0;
        if (g_gaps <= LAGFX_GAP_MAX_GAPS) {
            say("g7-gap: n=%" PRIu64 " gap_ms=%" PRIu64 " (threshold %" PRIu64 " ms) prev(ch=%u op=0x%04x n=%" PRIu64 " at +%" PRIu64 "ms) next(src=%d ch=%u op=0x%04x n=%" PRIu64 " at +%" PRIu64 "ms) cmd_seq=%" PRIu64
                " swaps_so_far=%" PRIu64 " since_first_swap_ms=%" PRIu64 " since_last_swap_ms=%" PRIu64 " last_swap_mapping=%" PRIu64 " window[swaps=%u passes=%u draws=%u] (read-only diagnostic)",
                g_gaps, gap_ms, lagfx_gap_threshold_ms(), prev_ch, prev_op, prev_n, ms_since(t0, prev), src, chan, op, op_n, ms_since(t0, now_ns), seq,
                g_swaps, ms_since(g_first_swap_ns, now_ns), ms_since(g_last_swap_ns, now_ns), g_last_swap_mapping,
                LAGFX_GAP_WINDOW_SWAPS, LAGFX_GAP_WINDOW_PASSES, LAGFX_GAP_WINDOW_DRAWS);
            char st[1500];
            if (lagfx_dd_stats_line(st, sizeof st)) say("g7-gap-pvg-ops: n=%" PRIu64 " %s", g_gaps, st);
        }
        atomic_store(&g_win_swaps, LAGFX_GAP_WINDOW_SWAPS), atomic_store(&g_win_passes, LAGFX_GAP_WINDOW_PASSES), atomic_store(&g_win_draws, LAGFX_GAP_WINDOW_DRAWS);
    }
    if (is_op09) {
        g_op09++;
        if (g_op09 <= LAGFX_GAP_MAX_OP09) {
            uint32_t arg8 = (rd32(raw) >> 16) & 0xffffu;
            uint32_t skip = arg8 * 8u, plen = length - 12u;
            const uint8_t *pl = raw + 12;
            char w[8 * 9 + 4]; size_t o = 0;
            for (uint32_t i = 0; i < 8u && skip + 4u * (i + 1u) <= plen; i++) o += (size_t)snprintf(w + o, sizeof w - o, "%s%08x", i ? "," : "", rd32(pl + skip + 4u * i));
            if (!o) strcpy(w, "-");
            say("g7-op09: n=%" PRIu64 " ch=%u payload_words=%s len=%u at=+%" PRIu64 "ms since_prev_cmd_ms=%" PRIu64 " since_first_swap_ms=%" PRIu64 " since_last_swap_ms=%" PRIu64 " since_prev_op09_ms=%" PRIu64
                " swaps_so_far=%" PRIu64 " since_prev_op09[unmap_0x22=%" PRIu64 " obj_0x25=%" PRIu64 " drop_task_0x20=%" PRIu64 " define_task_0x38=%" PRIu64 " set_objects_0x44=%" PRIu64 "] (counts include this window's commands up to now)",
                g_op09, chan, w, length, ms_since(t0, now_ns), ms_since(prev, now_ns), ms_since(g_first_swap_ns, now_ns), ms_since(g_last_swap_ns, now_ns),
                ms_since(g_prev_op09_ns, now_ns), g_swaps, atomic_load(&g_cnt_unmap), atomic_load(&g_cnt_obj), atomic_load(&g_cnt_drop), atomic_load(&g_cnt_define), atomic_load(&g_cnt_setobj));
        }
        g_prev_op09_ns = now_ns;
        atomic_store(&g_cnt_unmap, 0), atomic_store(&g_cnt_obj, 0), atomic_store(&g_cnt_drop, 0), atomic_store(&g_cnt_define, 0), atomic_store(&g_cnt_setobj, 0);
    }
    if (is_swap) {
        g_swaps++;
        if (!g_first_swap_ns) g_first_swap_ns = now_ns;
        uint32_t arg8 = (rd32(raw) >> 16) & 0xffffu;
        uint32_t skip = arg8 * 8u, plen = length - 12u;
        const uint8_t *pl = raw + 12;
        uint64_t mapping = skip + 16u <= plen ? (uint64_t)rd32(pl + skip + 8u) | (uint64_t)rd32(pl + skip + 12u) << 32 : 0;   /* same field as g7-pvg-gamma op8 raw */
        uint32_t left = atomic_load(&g_win_swaps);
        if (left) {
            atomic_store(&g_win_swaps, left - 1u);
            g_swaps_after_gap++;
            say("g7-gap-swap: k=%u swap=#%" PRIu64 " ch=%u mapping=%" PRIu64 " mapping_before_gap=%" PRIu64 " same_mapping_as_before_gap=%d since_gap_end_ms=%" PRIu64 " since_prev_cmd_ms=%" PRIu64 " payload_len=%u at=+%" PRIu64 "ms",
                g_swaps_after_gap, g_swaps, chan, mapping, g_mapping_before_gap, mapping == g_mapping_before_gap, ms_since(g_gap_end_ns, now_ns), ms_since(prev, now_ns), plen, ms_since(t0, now_ns));
        }
        g_last_swap_ns = now_ns, g_last_swap_mapping = mapping;
    }
    pthread_mutex_unlock(&g_mu);
}

void lagfx_gap_note_pass(uint32_t ref, uint32_t slot, uint32_t load, uint32_t store, const double clear[4], uint32_t w, uint32_t h, uint32_t ncolor) {
    uint32_t left = atomic_load_explicit(&g_win_passes, memory_order_relaxed);
    if (!left || !lagfx_gap_enabled()) return;
    pthread_mutex_lock(&g_mu);
    left = atomic_load(&g_win_passes);
    if (left) {
        atomic_store(&g_win_passes, left - 1u);
        g_passes_after_gap++;
        say("g7-gap-pass: k=%u ref=%u slot=%u load_action=%u(%s) store_action=%u clear=(%g,%g,%g,%g) render_area=%ux%u colour_attachments=%u (load: 0 dont-care 1 load 2 clear)",
            g_passes_after_gap, ref, slot, load, load == 2u ? "clear" : load == 1u ? "load" : load == 0u ? "dont-care" : "?", store, clear ? clear[0] : 0.0, clear ? clear[1] : 0.0, clear ? clear[2] : 0.0, clear ? clear[3] : 0.0, w, h, ncolor);
    }
    pthread_mutex_unlock(&g_mu);
}

void lagfx_gap_note_draw(uint32_t prim, uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh, float zn, float zf, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t rw, uint32_t rh) {
    uint32_t left = atomic_load_explicit(&g_win_draws, memory_order_relaxed);
    if (!left || !lagfx_gap_enabled()) return;
    pthread_mutex_lock(&g_mu);
    left = atomic_load(&g_win_draws);
    if (left) {
        atomic_store(&g_win_draws, left - 1u);
        g_draws_after_gap++;
        say("g7-gap-draw: k=%u prim=%u viewport=(%u,%u,%ux%u z %g..%g) scissor=(%u,%u,%ux%u) render_area=%ux%u (0 sized viewport / scissor = never set by the guest in this encoder)",
            g_draws_after_gap, prim, vx, vy, vw, vh, (double)zn, (double)zf, sx, sy, sw, sh, rw, rh);
    }
    pthread_mutex_unlock(&g_mu);
}

void lagfx_gap_final(uint64_t now_ns) {
    if (!lagfx_gap_enabled()) return;
    pthread_mutex_lock(&g_mu);
    uint64_t last = atomic_load(&g_last_ns), t0 = atomic_load(&g_t0_ns);
    say("g7-gap-final: gaps=%" PRIu64 " (threshold %" PRIu64 " ms) longest_gap_ms=%" PRIu64 " last_gap_ms=%" PRIu64 " op09_events=%" PRIu64 " swaps=%" PRIu64 " last_swap_mapping=%" PRIu64 " last_cmd_at=+%" PRIu64 "ms last_cmd_age_ms=%" PRIu64 " now=+%" PRIu64 "ms",
        g_gaps, lagfx_gap_threshold_ms(), g_longest_ms, g_gap_ms_last, g_op09, g_swaps, g_last_swap_mapping, ms_since(t0, last), ms_since(last, now_ns), ms_since(t0, now_ns));
    pthread_mutex_unlock(&g_mu);
}
