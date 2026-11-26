/*
 * libapplegfx-vulkan — bounded per-draw diagnostics (logging only; nothing here changes behaviour)
 * src/protocol/draw_diag.h
 *
 * Every draw record of an ARM64 exec reports one event (task, pass, pipeline ref, vs/fs function hash, outcome, the binding that stopped it, colour target 0).
 * Events are deduplicated into a small fixed table by (pso, vs, fs, reason, binding, target) with a counter. Recording takes a tiny spin lock for a table lookup and
 * does no formatting, no logging and no I/O; lines are formatted and written by lagfx_draw_diag_flush(), called from the periodic statistics path
 * (lagfx_diag_stats, every 10 s, and at teardown). Output is capped: at most LAGFX_DRAW_DIAG_MAX_LINES lines in total, LAGFX_DRAW_DIAG_PER_FLUSH new lines per flush.
 * Each tuple carries first-seen / last-seen time (ms since the diagnostics object was created, CLOCK_MONOTONIC). The summary reports cumulative counts with the delta since the
 * previous summary, the table overflow, the lines suppressed by the cap, and multi_reason (draws where more than one skip counter changed, with the top 3 counter pairs).
 * Environment: G7_DRAW_DIAG=0 disables it (default on, capped). Disabled = one NULL check per draw, nothing allocated.
 */
#ifndef LAGFX_PROTOCOL_DRAW_DIAG_H
#define LAGFX_PROTOCOL_DRAW_DIAG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

#define LAGFX_DRAW_DIAG_TABLE      128u   /* distinct tuples kept; more are counted in `overflow` only */
#define LAGFX_DRAW_DIAG_MAX_LINES  400u   /* total lines ever written (new-tuple lines + summaries) */
#define LAGFX_DRAW_DIAG_PER_FLUSH  24u    /* new-tuple lines per flush */
#define LAGFX_DRAW_DIAG_RESERVE    16u    /* lines kept back for the final summary */

typedef enum { LAGFX_DD_OK = 0, LAGFX_DD_NO_PIPELINE, LAGFX_DD_UNBOUND_TEX, LAGFX_DD_UNBOUND_BUF, LAGFX_DD_OTHER, LAGFX_DD_REASONS } lagfx_dd_reason_t;

typedef struct {
    uint32_t task, draw;           /* task id; 1-based draw index in the exec (first occurrence is kept for the log) */
    uint64_t pass;                 /* pass counter at the first occurrence */
    uint32_t pso;                  /* pipeline object ref (0 = none bound) */
    uint32_t vs, fs;               /* top 32 bits of the vertex / fragment function hash (the first 8 hex digits of %016llx) */
    uint8_t  reason;               /* lagfx_dd_reason_t */
    const char *why;               /* static detail name for OTHER / NO_PIPELINE (e.g. "no_target"), NULL otherwise */
    uint8_t  bkind;                /* 0 none, 1 texture, 2 buffer, 3 other binding (sampler / input attachment / storage image) */
    uint8_t  stage;                /* 0 vertex, 1 fragment */
    int16_t  slot;                 /* Metal slot, -1 unknown */
    uint32_t vkb, bref;            /* Vulkan binding, guest object ref of the unbound thing (0 unknown) */
    uint32_t rt_w, rt_h, rt_fmt;   /* colour attachment 0 size and Vulkan format (0 = no target) */
    uint32_t chg;                  /* bit i = skip counter i (LAGFX_DD_CTR_NAMES order) changed during this draw; not part of the tuple; draw/skipped totals excluded by the caller */
} lagfx_draw_ev_t;

/* Skip-counter names, indexed by the bit number in lagfx_draw_ev_t.chg. Keep in step with the DD_C_* enum in arm64_exec_run.c. */
#define LAGFX_DD_NCTR 23u
#define LAGFX_DD_PAIRS 16u   /* distinct counter pairs tracked for multi_reason; further pairs only add to multi_reason itself */

/* The per-device state, created on first use; NULL when disabled (G7_DRAW_DIAG=0) or out of memory. */
struct lagfx_draw_diag *lagfx_draw_diag_get(lagfx_protocol_t *p);
void lagfx_draw_diag_free(lagfx_protocol_t *p);
/* Hot path: dedup + count under a spin lock, no formatting. */
void lagfx_draw_diag_record(struct lagfx_draw_diag *dd, const lagfx_draw_ev_t *ev);
/* Periodic / final path: snapshot under the lock, format and log after releasing it. Returns the number of lines written. */
unsigned lagfx_draw_diag_flush(lagfx_protocol_t *p, bool final);
/* Test helpers. */
uint64_t lagfx_draw_diag_lines(struct lagfx_draw_diag *dd);

#endif
