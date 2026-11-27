/*
 * libapplegfx-vulkan — time spent inside the library per call site ("hold ledger")
 * src/protocol/arm64_hold.h
 *
 * Why: the shell calls the library with its own device lock held (include/libapplegfx-vulkan.h: "under the BQL"). Every microsecond a call spends here
 * is a microsecond a guest CPU that touches the same device waits (Henge log: "wd: dev_lock ... wmax_ms", "device lock N/s P%"). The shell can see the
 * wait but not WHO held the lock; this ledger names the library phases (measured with CLOCK_MONOTONIC around the call, so it includes any wait inside
 * it: fence waits, driver pipeline compilation, guest-memory copies, logging).
 *
 * Phases are NESTED, not disjoint: DRAIN contains EXEC_END, RESOLVE, PIPE_BUILD, PIPE_FAIL; do not add them up.
 *
 * Output (no behaviour change, no new env var needed):
 *   - "g7-gpu-slow: phase=<name> ms=<x> ..." (WARN, at most 100 lines) when ONE call of a phase takes >= LAGFX_SLOW_MS (default 20; 0 = off);
 *   - "g7-gpu-hold: win=<s> drain[n= ms= max=] ..." (INFO) at most once per 10 s, only if something was recorded in that window, so it lines up with
 *     the shell's 10 s watchdog windows. Idle = no lines.
 * Thread-safety: relaxed atomics; counters may be read while being updated (monotonic, each field individually consistent).
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBAPPLEGFX_PROTOCOL_ARM64_HOLD_H
#define LIBAPPLEGFX_PROTOCOL_ARM64_HOLD_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    LAGFX_HOLD_DRAIN = 0,    /* outermost channel_compute_dispatcher_drain (everything one doorbell makes the library do on the compute rings) */
    LAGFX_HOLD_POLL,         /* lagfx_arm64_poll_completions (doorbell reads and the shell's timer) */
    LAGFX_HOLD_EXEC_END,     /* lagfx_a64_exec_end: submit + bounded fence wait + flag-4 / write-back copies into guest memory (bytes = written back) */
    LAGFX_HOLD_RESOLVE,      /* pipeline object creation: guest reads + AIR->SPIR-V for both stages + module creation */
    LAGFX_HOLD_PIPE_BUILD,   /* lagfx_pipeline_build that succeeded (driver compile) */
    LAGFX_HOLD_PIPE_FAIL,    /* lagfx_pipeline_build that failed (driver compile + failure diagnostics: bundle, matrix, reduce when enabled) */
    LAGFX_HOLD_COUNT
} lagfx_hold_phase_t;

typedef struct { uint64_t n, total_ns, max_ns, slow_n, bytes; } lagfx_hold_cell_t;
typedef struct { lagfx_hold_cell_t ph[LAGFX_HOLD_COUNT]; uint64_t slow_lines, slow_suppressed, summaries; } lagfx_hold_stats_t;

const char *lagfx_hold_name(lagfx_hold_phase_t ph);
uint64_t lagfx_hold_now_ns(void);   /* CLOCK_MONOTONIC */

/* Records one call of `ph` that started at t0_ns (from lagfx_hold_now_ns). bytes: payload moved (0 if n/a). */
void lagfx_hold_note(lagfx_hold_phase_t ph, uint64_t t0_ns, uint64_t bytes);
/* Same with an explicit duration and "now" (tests, no clock reads). */
void lagfx_hold_note_dur(lagfx_hold_phase_t ph, uint64_t dur_ns, uint64_t bytes, uint64_t now_ns);

void lagfx_hold_get(lagfx_hold_stats_t *out);
void lagfx_hold_reset(void);   /* tests: zero the counters, the window and the log rate limits; re-reads LAGFX_SLOW_MS */

/* The summary line for the counters accumulated since `since` (a lagfx_hold_get snapshot) over win_s seconds; returns snprintf's length. Pure. */
int lagfx_hold_format(char *buf, size_t cap, const lagfx_hold_stats_t *now, const lagfx_hold_stats_t *since, double win_s);

#endif
