/*
 * Idle-gap diagnostics (src/protocol/gap_diag.c): gap detector, op 0x0009 line with the counts since the previous one, the first swaps / pass / draws after a gap,
 * caps, the off switch, the final line. Pure: synthetic times and raw commands, no device, no Vulkan.
 * SPDX-License-Identifier: MIT
 */
#include "protocol/gap_diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_lines[256][2048];
static int g_n;
static void sink(const char *l) { if (g_n < 256) snprintf(g_lines[g_n++], sizeof g_lines[0], "%s", l); }
static int count(const char *prefix) { int c = 0; for (int i = 0; i < g_n; i++) if (!strncmp(g_lines[i], prefix, strlen(prefix))) c++; return c; }
static const char *nth(const char *prefix, int k) { for (int i = 0; i < g_n; i++) if (!strncmp(g_lines[i], prefix, strlen(prefix)) && !k--) return g_lines[i]; return NULL; }

#define MS 1000000ull
static uint64_t g_seq;
/* a raw command: header (op, arg8, len, stamp), arg8 barrier slots, then `words` payload words */
static void cmd(uint64_t t_ms, int src, uint32_t ch, uint32_t op, uint32_t arg8, const uint32_t *words, uint32_t nw) {
    uint8_t raw[256]; memset(raw, 0, sizeof raw);
    uint32_t len = 12u + arg8 * 8u + nw * 4u;
    raw[0] = (uint8_t)op, raw[1] = (uint8_t)(op >> 8), raw[2] = (uint8_t)arg8, raw[3] = (uint8_t)(arg8 >> 8); memcpy(raw + 4, &len, 4);
    for (uint32_t i = 0; i < nw; i++) memcpy(raw + 12 + arg8 * 8u + 4u * i, &words[i], 4);
    static uint64_t opn[3][256];
    lagfx_gap_note_cmd(t_ms * MS, 1000 * MS /* t0 */, src, ch, ++g_seq, ++opn[src][op & 0xff], raw, len);
}
static void reset(void) { lagfx_gap_reset(); g_n = 0; g_seq = 0; }

int main(void) {
    lagfx_gap_set_sink(sink);
    setenv("LAGFX_GAPDIAG_GAP_MS", "100", 1); unsetenv("LAGFX_GAPDIAG");
    assert(lagfx_gap_enabled() && lagfx_gap_threshold_ms() == 100);
    unsetenv("LAGFX_GAPDIAG_GAP_MS"); assert(lagfx_gap_threshold_ms() == LAGFX_GAP_DEFAULT_MS); setenv("LAGFX_GAPDIAG_GAP_MS", "100", 1);

    /* commands closer than the threshold: no gap line, no window */
    reset();
    cmd(1100, 1, 3, 0x22, 0, NULL, 0); cmd(1150, 1, 2, 0x25, 0, NULL, 0); cmd(1200, 1, 3, 0x22, 0, NULL, 0);
    assert(g_n == 0);
    { uint32_t s, p, d; lagfx_gap_window(&s, &p, &d); assert(!s && !p && !d && lagfx_gap_count() == 0); }
    puts("PASS: no gap line below the threshold");

    /* a swap (ch4 op 0x08) with the mapping id in the payload: first swap recorded (no window yet) */
    const uint32_t sw1[4] = { 0, 0, 1, 0 };   /* payload: 2 words, then mapping u64 at byte 8 (= words 2,3) */
    cmd(1250, 1, 4, 0x08, 0, sw1, 4);
    assert(g_n == 0);

    /* 900 ms of silence, then ch4 op 0x0009 payload 1 */
    { const uint32_t one[1] = { 1 };
      cmd(2150, 1, 4, 0x09, 0, one, 1);
      assert(lagfx_gap_count() == 1 && count("g7-gap: ") == 1 && count("g7-gap-pvg-ops: ") == 1 && count("g7-op09: ") == 1);
      const char *g = nth("g7-gap: ", 0);
      assert(strstr(g, "n=1 gap_ms=900 ") && strstr(g, "prev(ch=4 op=0x0008 n=1 at +250ms)") && strstr(g, "next(src=1 ch=4 op=0x0009 n=1 at +1150ms)") && strstr(g, "swaps_so_far=1 since_first_swap_ms=900") && strstr(g, "last_swap_mapping=1"));
      const char *o = nth("g7-op09: ", 0);
      assert(strstr(o, "n=1 ch=4 payload_words=00000001 len=16 at=+1150ms since_prev_cmd_ms=900 since_first_swap_ms=900 since_last_swap_ms=900 since_prev_op09_ms=0 swaps_so_far=1"));
      assert(strstr(o, "unmap_0x22=2 obj_0x25=1 drop_task_0x20=0 define_task_0x38=0 set_objects_0x44=0"));   /* the counts of everything since the start */
      uint32_t s, p, d; lagfx_gap_window(&s, &p, &d); assert(s == LAGFX_GAP_WINDOW_SWAPS && p == LAGFX_GAP_WINDOW_PASSES && d == LAGFX_GAP_WINDOW_DRAWS); }
    puts("PASS: gap line (length, previous / next command, time since first swap), op 0x0009 payload + counts, window armed");

    /* a teardown burst, op 0x0009 payload 0 with a barrier slot before the payload: counts since the previous op 0x0009 only */
    for (int i = 0; i < 5; i++) cmd(2200 + i, 1, 3, 0x22, 0, NULL, 0);
    for (int i = 0; i < 7; i++) cmd(2210 + i, 1, 2, 0x25, 0, NULL, 0);
    cmd(2230, 1, 3, 0x20, 0, NULL, 0); cmd(2231, 1, 3, 0x20, 0, NULL, 0); cmd(2240, 1, 3, 0x38, 0, NULL, 0); cmd(2241, 1, 2, 0x44, 0, NULL, 0);
    { const uint32_t zero[3] = { 0, 7, 9 };   /* arg8 = 1: the 8 barrier bytes are zero, then the payload words 0, 7, 9 follow them */
      cmd(2300, 1, 4, 0x09, 1, zero, 3);
      const char *o = nth("g7-op09: ", 1);
      assert(o && strstr(o, "n=2 ch=4 payload_words=00000000,00000007,00000009 len=32 ") && strstr(o, "since_prev_op09_ms=150") && strstr(o, "unmap_0x22=5 obj_0x25=7 drop_task_0x20=2 define_task_0x38=1 set_objects_0x44=1"));
      assert(count("g7-gap: ") == 1); }   /* 2241 -> 2300 is 59 ms: not a gap */
    puts("PASS: op 0x0009 #2: payload 0 after a barrier slot, counts since the previous op 0x0009 (unmap 5, obj 7, drop-task 2, define-task 1, set-objects 1)");

    /* after a 30 s gap: swaps with their mapping ids (the first 6), same-mapping flag, window counters */
    { const uint32_t m2[4] = { 0, 0, 2, 0 }, m1[4] = { 0, 0, 1, 0 };
      cmd(32300, 1, 4, 0x08, 0, m2, 4);
      assert(count("g7-gap: ") == 2);
      assert(strstr(nth("g7-gap: ", 1), "gap_ms=30000 ") && strstr(nth("g7-gap: ", 1), "prev(ch=4 op=0x0009 n=2 at +1300ms)"));
      assert(count("g7-gap-swap: ") == 1 && strstr(nth("g7-gap-swap: ", 0), "k=1 swap=#2 ch=4 mapping=2 mapping_before_gap=1 same_mapping_as_before_gap=0 since_gap_end_ms=0 "));
      cmd(32310, 1, 4, 0x08, 0, m1, 4);
      assert(count("g7-gap-swap: ") == 2 && strstr(nth("g7-gap-swap: ", 1), "k=2 swap=#3 ch=4 mapping=1 mapping_before_gap=1 same_mapping_as_before_gap=1 since_gap_end_ms=10 "));
      for (int i = 0; i < 8; i++) cmd(32320 + i, 1, 4, 0x08, 0, m2, 4);
      assert(count("g7-gap-swap: ") == (int)LAGFX_GAP_WINDOW_SWAPS);   /* capped */
      uint32_t s, p, d; lagfx_gap_window(&s, &p, &d); assert(s == 0 && p == LAGFX_GAP_WINDOW_PASSES && d == LAGFX_GAP_WINDOW_DRAWS); }
    puts("PASS: first 6 swaps after a gap carry the mapping id, same-as-before flag, time since the gap ended; the 7th is not logged");

    /* pass / draw records after the gap: logged for the window only */
    { const double clr[4] = { 0, 0, 0, 1 };
      for (int i = 0; i < 6; i++) lagfx_gap_note_pass(8, 0, 2, 1, clr, 1920, 1080, 2);
      assert(count("g7-gap-pass: ") == (int)LAGFX_GAP_WINDOW_PASSES);
      assert(strstr(nth("g7-gap-pass: ", 0), "k=1 ref=8 slot=0 load_action=2(clear) store_action=1 clear=(0,0,0,1) render_area=1920x1080 colour_attachments=2"));
      for (int i = 0; i < 6; i++) lagfx_gap_note_draw(3, 0, 0, 1920, 1080, 0.0f, 1.0f, 0, 0, 1920, 1080, 1920, 1080);
      assert(count("g7-gap-draw: ") == (int)LAGFX_GAP_WINDOW_DRAWS);
      assert(strstr(nth("g7-gap-draw: ", 0), "k=1 prim=3 viewport=(0,0,1920x1080 z 0..1) scissor=(0,0,1920x1080) render_area=1920x1080")); }
    puts("PASS: first passes (load / store / clear colour / area) and draws (viewport / scissor) after a gap are logged, then nothing");

    /* final */
    lagfx_gap_final(40000 * MS);
    assert(count("g7-gap-final: ") == 1 && strstr(nth("g7-gap-final: ", 0), "gaps=2 ") && strstr(nth("g7-gap-final: ", 0), "longest_gap_ms=30000 ") && strstr(nth("g7-gap-final: ", 0), "op09_events=2 "));
    puts("PASS: final line");

    /* cap: 20 gaps -> 16 g7-gap lines, 20 counted */
    reset();
    cmd(1000, 1, 3, 0x20, 0, NULL, 0);
    for (int i = 1; i <= 20; i++) cmd(1000 + 1000ull * i, 1, 3, 0x20, 0, NULL, 0);
    assert(lagfx_gap_count() == 20 && count("g7-gap: ") == (int)LAGFX_GAP_MAX_GAPS);
    puts("PASS: gap lines are capped (16), the count is not");

    /* op 0x0009 cap */
    reset();
    for (int i = 0; i < 30; i++) { const uint32_t w[1] = { (uint32_t)(i & 1) }; cmd(1000 + 10ull * i, 1, 4, 0x09, 0, w, 1); }
    assert(count("g7-op09: ") == (int)LAGFX_GAP_MAX_OP09);
    puts("PASS: op 0x0009 lines are capped (16)");

    /* off switch: nothing, not even the window */
    reset(); setenv("LAGFX_GAPDIAG", "0", 1);
    cmd(1000, 1, 4, 0x08, 0, NULL, 0); cmd(9000, 1, 4, 0x09, 0, NULL, 0); lagfx_gap_final(9100 * MS);
    { const double clr[4] = {0}; lagfx_gap_note_pass(1, 0, 2, 1, clr, 1, 1, 1); lagfx_gap_note_draw(3, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1); }
    assert(g_n == 0 && lagfx_gap_count() == 0);
    unsetenv("LAGFX_GAPDIAG");
    puts("PASS: LAGFX_GAPDIAG=0 logs nothing");

    /* a root / display command can end a gap too; a short or NULL command is ignored */
    reset();
    cmd(1000, 1, 3, 0x20, 0, NULL, 0); cmd(3000, 0, 0, 0x01, 0, NULL, 0);
    assert(count("g7-gap: ") == 1 && strstr(nth("g7-gap: ", 0), "next(src=0 ch=0 op=0x0001"));
    lagfx_gap_note_cmd(9000 * MS, 0, 1, 3, 99, 1, NULL, 0); { uint8_t s[4] = {0}; lagfx_gap_note_cmd(9000 * MS, 0, 1, 3, 99, 1, s, 4); }
    assert(count("g7-gap: ") == 1);
    puts("PASS: any source can end a gap; NULL / short commands are ignored");

    lagfx_gap_set_sink(NULL);
    puts("PASS: gap diagnostics");
    return 0;
}
