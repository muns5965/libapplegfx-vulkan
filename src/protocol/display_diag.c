/*
 * libapplegfx-vulkan — display diagnostics, stateful part. See display_diag.h. Logging and counters only.
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Process-wide counters (one GPU device per process in practice; the counts are for the log, not for control flow). The drain loops
 * can run on several threads, so counters are relaxed atomics and the one-shot / capped log decisions use atomic fetch-add.
 */
#include "display_diag.h"
#include "state.h"
#include "object_resolver.h"
#include "gamma_diag.h"
#include "../common/log.h"

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

static uint64_t dd_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static uint64_t dd_t0;
static uint64_t dd_ms(void) {   /* ms since the first call of this module (INFER: close to the first display/ring activity, not VM start) */
    uint64_t t = dd_now_ns(), z = 0;
    __atomic_compare_exchange_n(&dd_t0, &z, t, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return (t - __atomic_load_n(&dd_t0, __ATOMIC_RELAXED)) / 1000000ull;
}

/* One tagged line to stderr (like the g7-gpu statistics lines, so it reaches the runner's log) and to the lagfx log. Bounded by the callers' caps. */
static lagfx_dd_reader_t s_reader;
void lagfx_dd_set_reader(lagfx_dd_reader_t r) { s_reader = r; }
static lagfx_dd_owner_t s_owner;
void lagfx_dd_set_owner_lookup(lagfx_dd_owner_t f) { s_owner = f; }

static void dd_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dd_say(const char *fmt, ...) {
    char line[2000];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", line);
    LAGFX_LOG("%s", line);
}
#define INC(x) __atomic_add_fetch(&(x), 1u, __ATOMIC_RELAXED)
#define GET(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

typedef struct { uint64_t n, first_ms; uint32_t first_chan; } opseen_t;
static opseen_t s_op37, s_op22, s_op08, s_op0a;
static uint64_t s_op08_ch4, s_op0a_ident, s_op0a_nonident;
static uint64_t s_swaps, s_swap_unresolved, s_swap_gpu, s_swap_lines, s_swap_unres_lines;
static uint32_t s_last_mtl, s_last_w, s_last_h; static int32_t s_last_vk; static uint64_t s_last_mapping;
static struct { uint64_t mapping; uint32_t w, h, mtl; } s_seen[16];
static uint64_t s_cursor_show, s_cursor_glyph, s_cursor_lines, s_cursor_poschg, s_cursor_vischg, s_cursor_glyph_chg;   /* ops seen / position changes / visibility changes / glyph size changes, separately */
static int s_cursor_have_pos, s_cursor_have_glyph;
static int s_cx, s_cy; static uint32_t s_cvis, s_cgw, s_cgh;
static uint64_t s_vbl_ticks, s_vbl_raises, s_vbl_noread, s_vbl_readbacks, s_vbl_pending_set, s_vbl_readback_zero, s_vbl_readback_fail, s_vbl_lines;
static uint64_t s_gamma_raw_lines, s_stat_lines;

static void op_seen(opseen_t *o, uint32_t op, uint32_t chan) {
    if (__atomic_add_fetch(&o->n, 1u, __ATOMIC_RELAXED) == 1u) {
        o->first_ms = dd_ms(); o->first_chan = chan;
        dd_say("g7-pvg-ops: first op=0x%02x ch=%u t=+%llums", op, chan, (unsigned long long)o->first_ms);
    }
}

void lagfx_dd_note_cmd(lagfx_protocol_t *p, int src, uint32_t chan, const uint8_t *raw, uint32_t length) {
    if (!p || !raw || length < 12u) return;
    uint32_t op = (uint32_t)raw[0] | (uint32_t)raw[1] << 8;
    if (op != 0x37u && op != 0x22u && op != 0x08u && op != 0x0au) return;
    uint32_t arg8 = (uint32_t)raw[2] | (uint32_t)raw[3] << 8;
    const uint8_t *pl = raw + 12; uint32_t plen = length - 12u;   /* payload after the 12-byte header; barrier slots (arg8 * 8 bytes) come first */
    uint32_t skip = arg8 * 8u;
    (void)src;
    if (op == 0x37u) { op_seen(&s_op37, op, chan); return; }
    if (op == 0x22u) { op_seen(&s_op22, op, chan); return; }
    if (op == 0x0au) {
        op_seen(&s_op0a, op, chan);
        if (skip + 64u <= plen) {   /* 4 control words + 3 float4 rows (see lagfx_dd_is_identity_op0a); the old packed-3x3 test at payload byte 0 mis-classified the real payload */
            int id = lagfx_dd_is_identity_op0a(pl + skip, plen - skip);
            if (id) INC(s_op0a_ident); else INC(s_op0a_nonident);
            uint64_t k = GET(s_op0a_ident) + GET(s_op0a_nonident);
            if (k <= 2u) {
                const uint8_t *m = pl + skip;
                char w[16 * 9 + 4]; size_t o = 0;
                for (int i = 0; i < 16; i++) o += (size_t)snprintf(w + o, sizeof w - o, "%s%08x", i ? "," : "", lagfx_dd_rd32(m + 4 * i));
                dd_say("g7-pvg-gamma: op0a #%llu ch=%u identity_rows=%d words=%s (read-only; layout INFER: 4 control words, then float32 rows of 4 from payload byte 16; the third diagonal element is not in older logs)",
                          (unsigned long long)k, chan, id, w);
            }
        }
        return;
    }
    /* op 0x08 */
    op_seen(&s_op08, op, chan);
    if (chan != 4u) return;
    uint64_t n = INC(s_op08_ch4);
    lagfx_gd_note_op8(p, chan, pl, plen, skip, s_reader, s_owner);   /* EVERY channel-4 op 0x08: descriptor change log, region read, address-space probes (gamma_diag.c) */
    if (n > 3u) return;   /* the raw dump: the first 3 channel-4 op 0x08 only */
    {
        char hx[3 * 96 + 8]; size_t o = 0; uint32_t shown = plen < 96u ? plen : 96u;
        for (uint32_t i = 0; i < shown; i++) o += (size_t)snprintf(hx + o, sizeof hx - o, "%02x ", pl[i]);
        int w_off = lagfx_dd_find_word(pl, plen, 0x118000u), w_len = lagfx_dd_find_word(pl, plen, 0x4000u);
        int w_cnt = lagfx_dd_find_word(pl, plen, 257u), w_ck = lagfx_dd_find_word(pl, plen, 0x48400000u);
        INC(s_gamma_raw_lines);
        dd_say("g7-pvg-gamma: op8 raw #%llu ch=%u arg8=%u payload_len=%u found_word_index[0x118000]=%d [0x4000]=%d [257]=%d [0x48400000]=%d (-1 = absent; INFER: positions searched, not assumed) bytes: %s%s",
                  (unsigned long long)n, chan, arg8, plen, w_off, w_len, w_cnt, w_ck, hx, shown < plen ? "..." : "");
    }
}

void lagfx_dd_note_swap(uint64_t mapping, const lagfx_dd_surface_t *s) {
    INC(s_swaps);
    uint64_t ms = dd_ms();
    if (!s || !s->resolved) {
        INC(s_swap_unresolved);
        if (__atomic_fetch_add(&s_swap_unres_lines, 1u, __ATOMIC_RELAXED) < 8u)
            dd_say("g7-pvg-swap: t=+%llums op8#%llu mapping=%llu unresolved (no mirror surface: no exec ever drew into it)", (unsigned long long)ms, (unsigned long long)GET(s_swaps), (unsigned long long)mapping);
        return;
    }
    if (s->gpu) INC(s_swap_gpu);
    s_last_mapping = mapping; s_last_w = s->w; s_last_h = s->h; s_last_mtl = s->mtl_format; s_last_vk = s->vk_format;
    uint64_t nl = GET(s_swap_lines);
    for (uint64_t i = 0; i < nl && i < 16u; i++)
        if (s_seen[i].mapping == mapping && s_seen[i].w == s->w && s_seen[i].h == s->h && s_seen[i].mtl == s->mtl_format) return;
    uint64_t slot = __atomic_fetch_add(&s_swap_lines, 1u, __ATOMIC_RELAXED);
    if (slot >= 16u) return;
    s_seen[slot].mapping = mapping; s_seen[slot].w = s->w; s_seen[slot].h = s->h; s_seen[slot].mtl = s->mtl_format;
    dd_say("g7-pvg-swap: t=+%llums op8#%llu mapping=%llu %ux%u fourcc=0x%08x mtl_format=%u(%s) vk_format=%d(%s) gpu=%d guest_current=%d first for this (mapping,size,format)",
              (unsigned long long)ms, (unsigned long long)GET(s_swaps), (unsigned long long)mapping, s->w, s->h, s->fourcc, s->mtl_format, lagfx_dd_mtl_name(s->mtl_format),
              (int)s->vk_format, lagfx_dd_vk_name(s->vk_format), (int)s->gpu, (int)s->guest_current);
}

static void cursor_line(const char *what) {
    uint64_t k = GET(s_cursor_show) + GET(s_cursor_glyph);
    if (k <= 8u || (k % 256u) == 0u)
        dd_say("g7-pvg-cursor: %s total=%llu show=%llu glyph=%llu last_pos=(%d,%d) visible=%u last_glyph=%ux%u t=+%llums", what, (unsigned long long)k,
                  (unsigned long long)GET(s_cursor_show), (unsigned long long)GET(s_cursor_glyph), s_cx, s_cy, s_cvis, s_cgw, s_cgh, (unsigned long long)dd_ms());
}
void lagfx_dd_note_cursor_show(uint32_t display_id, int x, int y, uint32_t visible) {
    (void)display_id; INC(s_cursor_show);
    if (!s_cursor_have_pos || x != s_cx || y != s_cy) INC(s_cursor_poschg);
    if (!s_cursor_have_pos || visible != s_cvis) INC(s_cursor_vischg);
    s_cursor_have_pos = 1; s_cx = x; s_cy = y; s_cvis = visible; cursor_line("show");
}
void lagfx_dd_note_cursor_glyph(uint32_t display_id, uint32_t w, uint32_t h) {
    (void)display_id; INC(s_cursor_glyph);
    if (!s_cursor_have_glyph || w != s_cgw || h != s_cgh) INC(s_cursor_glyph_chg);
    s_cursor_have_glyph = 1; s_cgw = w; s_cgh = h; cursor_line("glyph");
}

void lagfx_dd_note_vbl_tick(unsigned display, uint32_t enabled_mask, bool raised, bool read_ok) {
    (void)display; (void)enabled_mask;
    INC(s_vbl_ticks);
    if (!read_ok) INC(s_vbl_noread);
    if (raised) INC(s_vbl_raises);
}
void lagfx_dd_note_vbl_readback(unsigned display, int64_t pending_word) {
    INC(s_vbl_readbacks);
    if (pending_word > 0) INC(s_vbl_pending_set); else if (pending_word == 0) INC(s_vbl_readback_zero); else INC(s_vbl_readback_fail);
    if (__atomic_fetch_add(&s_vbl_lines, 1u, __ATOMIC_RELAXED) < 16u)
        dd_say("g7-pvg-vbl: display[%u] readback of ss[+0x100] after the raise = %lld (nonzero = not yet cleared by the guest; INFER: the guest clears it when it consumed the event) t=+%llums",
                  display, (long long)pending_word, (unsigned long long)dd_ms());
}

size_t lagfx_dd_stats_line(char *out, size_t cap) {
    lagfx_gd_stats_t gs; char gsuf[400];
    lagfx_gd_stats_get(&gs); lagfx_gd_stats_suffix(gsuf, sizeof gsuf);
    int n = snprintf(out, cap,
        "g7-pvg-ops: t=+%llus op37=%llu(first +%llums ch%u) op22=%llu(first +%llums ch%u) op08=%llu(ch4=%llu first +%llums ch%u) op0a=%llu(identity=%llu other=%llu first +%llums ch%u) "
        "swaps=%llu(unresolved=%llu gpu=%llu last mapping=%llu %ux%u mtl=%u(%s) vk=%d(%s)) cursor[ops_show=%llu ops_glyph=%llu pos_changes=%llu vis_changes=%llu glyph_updates=%llu(size_changes) last=(%d,%d) vis=%u glyph=%ux%u] "
        "vbl[ticks=%llu raise_attempts=%llu tick_noread=%llu readbacks=%llu readback_zero(consumed INFER)=%llu readback_nonzero=%llu readback_failed=%llu] gamma[raw_lines=%llu read_attempts=%llu read_done=%llu%s]",
        (unsigned long long)(dd_ms() / 1000ull),
        (unsigned long long)GET(s_op37.n), (unsigned long long)s_op37.first_ms, s_op37.first_chan, (unsigned long long)GET(s_op22.n), (unsigned long long)s_op22.first_ms, s_op22.first_chan,
        (unsigned long long)GET(s_op08.n), (unsigned long long)GET(s_op08_ch4), (unsigned long long)s_op08.first_ms, s_op08.first_chan,
        (unsigned long long)GET(s_op0a.n), (unsigned long long)GET(s_op0a_ident), (unsigned long long)GET(s_op0a_nonident), (unsigned long long)s_op0a.first_ms, s_op0a.first_chan,
        (unsigned long long)GET(s_swaps), (unsigned long long)GET(s_swap_unresolved), (unsigned long long)GET(s_swap_gpu), (unsigned long long)s_last_mapping, s_last_w, s_last_h,
        s_last_mtl, lagfx_dd_mtl_name(s_last_mtl), (int)s_last_vk, lagfx_dd_vk_name(s_last_vk),
        (unsigned long long)GET(s_cursor_show), (unsigned long long)GET(s_cursor_glyph), (unsigned long long)GET(s_cursor_poschg), (unsigned long long)GET(s_cursor_vischg), (unsigned long long)GET(s_cursor_glyph_chg), s_cx, s_cy, s_cvis, s_cgw, s_cgh,
        (unsigned long long)GET(s_vbl_ticks), (unsigned long long)GET(s_vbl_raises), (unsigned long long)GET(s_vbl_noread), (unsigned long long)GET(s_vbl_readbacks), (unsigned long long)GET(s_vbl_readback_zero), (unsigned long long)GET(s_vbl_pending_set), (unsigned long long)GET(s_vbl_readback_fail),
        (unsigned long long)GET(s_gamma_raw_lines), (unsigned long long)gs.read_attempts, (unsigned long long)gs.read_done, gsuf);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

void lagfx_dd_log_stats(bool final) {
    if (!final && __atomic_fetch_add(&s_stat_lines, 1u, __ATOMIC_RELAXED) >= 40u) return;
    char line[2000];
    lagfx_dd_stats_line(line, sizeof line);
    dd_say("%s", line);
}
