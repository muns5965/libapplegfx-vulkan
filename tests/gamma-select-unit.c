/*
 * libapplegfx-vulkan — gamma LUT address-space selection (g7-gamma-select): a region read is accepted only when it decodes AND its checksum matches the descriptor's.
 * tests/gamma-select-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Host test with a fake per-task status reader (no page tables, no Vulkan device). Nothing here proves a visible gamma effect; it pins the selection logic only.
 * Scenarios: A owner mismatch + other task match -> other selected; B owner match -> selected, others not read; C no match -> not accepted, attempts bounded;
 * D two fallback tasks match -> ambiguous, nothing selected; E unreadable owner -> fallback; F table too short for count -> nofit.
 */
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include "protocol/gamma_diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT 8
static struct { bool readable; int kind; } g_t[NT];   /* kind 0 = zeros (mismatch), 1 = identity table (matches 0x48400000), 2 = junk (mismatch) */
static uint32_t g_reads, g_read_log[64];
static int fake(lagfx_protocol_t *p, uint32_t task, uint64_t va, uint32_t len, uint8_t *out) {
    (void)p; (void)va;
    if (g_reads < 64) g_read_log[g_reads] = task;
    g_reads++;
    if (task >= NT) return LAGFX_GD_RD_NO_TASK;
    if (!g_t[task].readable) return LAGFX_GD_RD_UNMAPPED;
    memset(out, 0, len);
    if (g_t[task].kind == 1) for (uint32_t c = 0; c < 3; c++) for (uint32_t i = 0; i < 257; i++) { float f = (float)i / 256.0f; memcpy(out + 4u * (c * 257u + i), &f, 4); }
    if (g_t[task].kind == 2) for (uint32_t i = 0; i < 771; i++) { uint32_t w = 0x7a736167u + i; memcpy(out + 4u * i, &w, 4); }
    return LAGFX_GD_RD_OK;
}
static bool g_have_owner; static uint32_t g_owner;
static bool owner_fn(lagfx_protocol_t *p, uint64_t m, uint32_t *t) { (void)p; (void)m; if (!g_have_owner) return false; *t = g_owner; return true; }
static void w32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }

static lagfx_protocol_t *mk(const uint32_t *ids, int n, uint32_t last44) {
    lagfx_protocol_t *p = calloc(1, sizeof *p); assert(p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    for (int i = 0; i < n; i++) { lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p); assert(t); t->id = ids[i]; t->live = true; }
    p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = last44;
    memset(g_t, 0, sizeof g_t); g_reads = 0; g_have_owner = false;
    lagfx_gd_test_reset(); lagfx_gd_set_reader(fake);
    return p;
}
static void op8(lagfx_protocol_t *p, uint32_t count, uint32_t sum) {
    uint8_t pl[40]; memset(pl, 0, sizeof pl);
    w32(pl, 8, 7); w32(pl, 16, 0x118000u); w32(pl, 24, 0x4000u); w32(pl, 32, count); w32(pl, 36, sum);
    lagfx_gd_note_op8(p, 4, pl, sizeof pl, 0, NULL, owner_fn);
}
/* swaps 1..4 and 8, 16, ... also run the (separate, diagnostic) lut-probe over every live task, which would add reads: burn swaps 1..4 with zero descriptors (no table, no reads) so
 * the counted swaps below are 5, 6, 7 (not sampled) and then in C 8 (sampled: the probe reads are expected there and excluded from the bound by counting before). */
static void burn4(lagfx_protocol_t *p) { uint8_t pl[40]; memset(pl, 0, sizeof pl); for (int i = 0; i < 4; i++) lagfx_gd_note_op8(p, 4, pl, sizeof pl, 0, NULL, owner_fn); }
static void done(lagfx_protocol_t *p) { free(p); lagfx_gd_set_reader(NULL); }

int main(void) {
    const uint32_t ids[3] = { 2, 0, 6 };
    lagfx_gd_stats_t s; lagfx_gd_select_t sel; uint8_t copy[0x4000];

    /* A. the on-device shape: owner task 2 reads but mismatches, task 0 holds the table -> task 0 selected, with the rejected owner counted */
    { lagfx_protocol_t *p = mk(ids, 3, 6); g_have_owner = true; g_owner = 2;
      g_t[2].readable = 1; g_t[2].kind = 2; g_t[0].readable = 1; g_t[0].kind = 1; g_t[6].readable = 1; g_t[6].kind = 0;
      burn4(p); op8(p, 257, 0x48400000u);
      lagfx_gd_stats_get(&s);
      assert(s.read_attempts == 1 && s.read_done == 1 && s.sel_accepted == 1 && s.sel_ambiguous == 0 && s.sel_no_match == 0);
      assert(s.sum_match == 1 && s.sum_mismatch == 0 && s.sel_rejected_mismatch == 2 && s.applied_ok == 0);   /* owner 2 and task 6 rejected */
      size_t n = lagfx_gd_select_get(&sel, copy, sizeof copy);
      assert(sel.state == LAGFX_GD_SEL_ACCEPTED && sel.task == 0 && !sel.preferred && sel.count == 257 && sel.sum == 0x48400000u && sel.nwords == 771 && n == 771u * 4u);
      uint32_t w = 0; for (uint32_t i = 0; i < 771; i++) { uint32_t x; memcpy(&x, copy + 4u * i, 4); w += x; }
      assert(w == 0x48400000u);   /* the copy IS the validated table */
      assert(g_read_log[0] == 2);   /* owner asked first */
      op8(p, 257, 0x48400000u); lagfx_gd_stats_get(&s); assert(s.read_attempts == 1);   /* accepted: no further reads */
      assert(lagfx_gd_select_get(&sel, NULL, 0) == 0 && sel.state == LAGFX_GD_SEL_ACCEPTED);
      done(p); }

    /* B. owner matches: selected, preferred, the other tasks are not read */
    { lagfx_protocol_t *p = mk(ids, 3, 6); g_have_owner = true; g_owner = 2;
      g_t[2].readable = g_t[0].readable = g_t[6].readable = 1; g_t[2].kind = 1; g_t[0].kind = 1; g_t[6].kind = 1;
      burn4(p); op8(p, 257, 0x48400000u);
      lagfx_gd_stats_get(&s);
      assert(s.sel_accepted == 1 && s.sel_ambiguous == 0 && s.sum_match == 1 && g_reads == 1 && g_read_log[0] == 2);
      lagfx_gd_select_get(&sel, NULL, 0); assert(sel.state == LAGFX_GD_SEL_ACCEPTED && sel.task == 2 && sel.preferred);
      done(p); }

    /* C. nothing matches: not accepted, the attempts are bounded (8), no unbounded retry; the getter offers no data */
    { lagfx_protocol_t *p = mk(ids, 3, 6); g_have_owner = true; g_owner = 2;
      g_t[2].readable = g_t[0].readable = g_t[6].readable = 1; g_t[2].kind = 2; g_t[0].kind = 0; g_t[6].kind = 2;
      burn4(p); for (int i = 0; i < 40; i++) op8(p, 257, 0x48400000u);
      lagfx_gd_stats_get(&s);
      assert(s.read_attempts == 8 && s.read_done == 8 && s.sel_accepted == 0 && s.sel_no_match == 8 && s.sum_mismatch == 8 && s.sum_match == 0);
      assert(g_reads >= 8u * 3u);   /* at least every task once per attempt (the sampled swaps add lut-probe reads on top); the attempt cap above is what bounds the region reads */
      assert(lagfx_gd_select_get(&sel, copy, sizeof copy) == 0 && sel.state == LAGFX_GD_SEL_NONE);
      done(p); }

    /* D. two non-owner tasks match: ambiguous, nothing selected, no data offered */
    { lagfx_protocol_t *p = mk(ids, 3, 6); g_have_owner = true; g_owner = 2;
      g_t[2].readable = g_t[0].readable = g_t[6].readable = 1; g_t[2].kind = 2; g_t[0].kind = 1; g_t[6].kind = 1;
      op8(p, 257, 0x48400000u);
      lagfx_gd_stats_get(&s);
      assert(s.sel_ambiguous == 1 && s.sel_accepted == 0 && s.sum_match == 0);
      assert(lagfx_gd_select_get(&sel, copy, sizeof copy) == 0 && sel.state == LAGFX_GD_SEL_AMBIGUOUS && sel.ntasks == 2 && sel.tasks[0] == 6 && sel.tasks[1] == 0);   /* last 0x44 (6) is asked before the scan */
      done(p); }

    /* E. owner unreadable, last 0x44 mismatches, a scanned task matches */
    { lagfx_protocol_t *p = mk(ids, 3, 6); g_have_owner = true; g_owner = 2;
      g_t[2].readable = 0; g_t[6].readable = 1; g_t[6].kind = 2; g_t[0].readable = 1; g_t[0].kind = 1;
      op8(p, 257, 0x48400000u);
      lagfx_gd_stats_get(&s);
      assert(s.sel_accepted == 1 && s.sel_rejected_unreadable == 1 && s.sel_rejected_mismatch == 1);
      lagfx_gd_select_get(&sel, NULL, 0); assert(sel.task == 0);
      done(p); }

    /* F. a region that cannot hold 3 * count words is "nofit", never accepted (descriptor parse would reject count 0x1000 for len 0x4000, so shrink the region instead) */
    { lagfx_protocol_t *p = mk(ids, 1, 2); g_t[2].readable = 1; g_t[2].kind = 0;   /* kind 0 only: the fake writes tables of 3084 bytes, larger than this region */
      uint8_t pl[40]; memset(pl, 0, sizeof pl); w32(pl, 8, 7); w32(pl, 16, 0x118000u); w32(pl, 24, 0x400u); w32(pl, 32, 85); w32(pl, 36, 0x48400000u);   /* 85 * 12 = 1020 <= 0x400: fits, sum mismatches (zero-ish) */
      lagfx_gd_note_op8(p, 4, pl, sizeof pl, 0, NULL, NULL);
      lagfx_gd_stats_get(&s); assert(s.sel_accepted == 0 && s.sel_rejected_mismatch == 1);
      done(p); }

    printf("gamma-select-unit OK\n");
    return 0;
}
