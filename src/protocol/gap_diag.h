/*
 * libapplegfx-vulkan — idle-gap diagnostics (default ON, light weight, logging only). See gap_diag.c.
 *
 * Why: run 20261127-152759-034 showed 579 s without a single command (47 s .. 626 s), then op 0x0009 ch4 (payload 1), a teardown (85 unmaps, 109 remove-object,
 * 2 drop-task-cache), op 0x0009 (payload 0), CmdDefineHostTask REDEFINED, 32 s of nothing, and two draws + two swaps that left a black frame with the cursor. The existing
 * logs sample commands (first 64 per op) and cap the g7-pvg-ops lines, so what happened around the gap had to be reconstructed. This module adds, per gap:
 *
 *   g7-gap:          one line when a command arrives after >= gap_ms of silence (default 20000, LAGFX_GAPDIAG_GAP_MS): the length of the gap, the last command before it, the first
 *                    after it, the time since the first display swap, swap number, and the same counters as g7-pvg-ops (g7-gap-pvg-ops line). Max 16 gaps.
 *   g7-op09:         every ch4 op 0x0009 (max 16): payload words, time since the previous command / first swap / last swap, and the counts of unmap (0x22), object (0x25), drop-task-cache
 *                    (0x20), define-host-task (0x38) and set-objects (0x44) commands since the previous op 0x0009 (or start).
 *   g7-gap-swap:     the first 6 ch4 op 0x08 after a gap: swap number, the mapping id the swap names, whether it is the mapping of the last swap before the gap, time since the gap ended.
 *   g7-gap-pass:     the first 4 render passes after a gap: colour attachment ref / slot, load action, store action, clear colour, render area, how many attachments.
 *   g7-gap-draw:     the first 4 draws after a gap: primitive, viewport (x y w h znear zfar) and scissor as the executor will use them, render area.
 *   g7-gap-final:    once at the end: number of gaps, longest gap, op 0x0009 count, last command age.
 *
 * It never changes behaviour: no command is delayed, dropped or answered. Cost per command: one relaxed atomic exchange and a compare. LAGFX_GAPDIAG=0 turns it off.
 * Threading: the fast path is lock free; lines are produced under one mutex (rare).
 * SPDX-License-Identifier: MIT
 */
#ifndef LAGFX_PROTOCOL_GAP_DIAG_H
#define LAGFX_PROTOCOL_GAP_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_GAP_DEFAULT_MS    20000u
#define LAGFX_GAP_MAX_GAPS      16u
#define LAGFX_GAP_MAX_OP09      16u
#define LAGFX_GAP_WINDOW_SWAPS  6u
#define LAGFX_GAP_WINDOW_PASSES 4u
#define LAGFX_GAP_WINDOW_DRAWS  4u

bool lagfx_gap_enabled(void);   /* LAGFX_GAPDIAG != "0" (default on) */
uint64_t lagfx_gap_threshold_ms(void);

/* One guest command (any source). Called from lagfx_diag_cmd with the monotonic time `now_ns`, the protocol's diagnostic clock origin `t0_ns` (the "t=+" of the
 * diag cmd lines), the channel, the raw command (header at raw[0..11]; arg8 barrier slots of 8 bytes follow before the payload), the running per-(source, op) count `op_n`
 * and the command sequence number. `src`: 0 root, 1 compute, 2 display (as LAGFX_DIAG_*). */
void lagfx_gap_note_cmd(uint64_t now_ns, uint64_t t0_ns, int src, uint32_t chan, uint64_t seq, uint64_t op_n, const uint8_t *raw, uint32_t length);

/* Render pass opened (after the attachments were set up). `ncolor` = colour attachments in the descriptor. Logged only inside a post-gap window. */
void lagfx_gap_note_pass(uint32_t ref, uint32_t slot, uint32_t load, uint32_t store, const double clear[4], uint32_t w, uint32_t h, uint32_t ncolor);
/* A draw is about to be recorded with this viewport / scissor (the executor's own values). Logged only inside a post-gap window. */
void lagfx_gap_note_draw(uint32_t prim, uint32_t vp_x, uint32_t vp_y, uint32_t vp_w, uint32_t vp_h, float znear, float zfar, uint32_t sc_x, uint32_t sc_y, uint32_t sc_w, uint32_t sc_h, uint32_t rt_w, uint32_t rt_h);

/* End of run: g7-gap-final. Safe to call more than once (the later calls log again). */
void lagfx_gap_final(uint64_t now_ns);

/* Tests: capture lines instead of the log (NULL = default: stderr + LAGFX_LOG), clear every counter and window. */
typedef void (*lagfx_gap_sink_t)(const char *line);
void lagfx_gap_set_sink(lagfx_gap_sink_t sink);
void lagfx_gap_reset(void);
/* Tests: window state (remaining counts). */
void lagfx_gap_window(uint32_t *swaps, uint32_t *passes, uint32_t *draws);
uint64_t lagfx_gap_count(void);

#endif
