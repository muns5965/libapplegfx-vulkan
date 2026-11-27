/*
 * libapplegfx-vulkan — guard for the legacy (x86, 4 KiB) task translator chain in ARM64 vmapple mode
 * src/protocol/xlate_guard.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Why: the legacy command-buffer chain (exec_cmdbuf.c, compute_inner_ops.c, draw_*.c, object_resolver.c) reads guest memory through
 * lagfx_task_translate / lagfx_task_read_virtual, which walk a 4 KiB, 10-bit-per-level radix tree with root_page_pfn << 12. An ARM64 guest's
 * task page tables are 16 KiB / 12-bit-per-level with root_page_pfn << 14 (arm64_exec_decode.c), so the legacy walk reads the wrong pages, and its
 * "identity" fallbacks (VA used as a GPA when translation fails) turn that into reads/writes at arbitrary guest-physical addresses.
 * In arm64 mode none of that may run. This module:
 *   - counts every entry into a legacy translator API while protocol_mode == arm64 (legacy_xlate_in_arm64) and logs the first one per site
 *     (site name + return address of the caller), so a regression is visible in a log;
 *   - counts and logs explicit rejections of commands the arm64 protocol has no executor for (compute 0x37, root 0x20): op, channel, stamp, size,
 *     first payload bytes (bounded), so the real layout can be read from a log later. The caller still completes the stamp (the drain loops do).
 * x86 mode: every function here is a no-op returning "not denied"; nothing changes.
 *
 * Statistics line (own line; diag.c is not modified, so there is no hook there): lagfx_xg_tick() is called from the compute drain and prints
 *   g7-gpu-xlate: t=+<s>s legacy_xlate_in_arm64=<n>[blocked=<n> redirected=<n>] sites[<name>=<n> ...] rejected=<n>[op37=<n> root20=<n>] ...
 * once at the first tick (all zero is then visible) and afterwards at most every 10 s when a counter changed.
 */
#ifndef LAGFX_PROTOCOL_XLATE_GUARD_H
#define LAGFX_PROTOCOL_XLATE_GUARD_H

#include <stdbool.h>
#include <stdint.h>
#include "state.h"
#include "opcodes.h"

#define LAGFX_XG_MAX_SITES 12u

typedef enum {
    LAGFX_XG_REJ_COMPUTE_37 = 0,   /* compute channel op 0x37 in arm64 mode */
    LAGFX_XG_REJ_ROOT_20,          /* root channel op 0x20 in arm64 mode */
    LAGFX_XG_REJ_EXEC_ENTRY,       /* lagfx_compute_exec_cmdbuf reached in arm64 mode by any other path (defence in depth) */
    LAGFX_XG_REJ_KINDS
} lagfx_xg_reject_kind_t;

typedef struct {
    uint64_t legacy_xlate_in_arm64;   /* entries into a legacy translator API in arm64 mode (blocked + redirected) */
    uint64_t blocked;                 /* refused: translation / identity read / identity write not performed */
    uint64_t redirected;              /* served through the arm64 (16 KiB) reader instead (lagfx_task_read_virtual only) */
    uint64_t rejected[LAGFX_XG_REJ_KINDS];
    uint64_t rejected_total;
    uint32_t nsites;
    char     site_name[LAGFX_XG_MAX_SITES][24];
    uint64_t site_count[LAGFX_XG_MAX_SITES];
} lagfx_xg_stats_t;

/* Legacy translator API entered with `p`. Returns true (arm64 mode: counted, logged once per site) when the caller must NOT proceed with the legacy
 * behaviour; false in x86 mode. `ret` = return address of the caller, for the one-time log line (may be NULL). */
bool lagfx_xg_legacy_entered(const lagfx_protocol_t *p, const char *site, const void *ret);
#define LAGFX_XG_LEGACY(p, site) lagfx_xg_legacy_entered((p), (site), __builtin_return_address(0))
/* Same, for a legacy entry that is served by the arm64 reader (counted as redirected instead of blocked). */
void lagfx_xg_note_redirect(const lagfx_protocol_t *p, const char *site, const void *ret);

/* Explicit counted rejection of one command (no execution). Logs the first 8 of each kind, then every 256th. */
void lagfx_xg_reject(const lagfx_protocol_t *p, lagfx_xg_reject_kind_t kind, const lagfx_cmd_header_t *hdr);

void lagfx_xg_snapshot(lagfx_xg_stats_t *out);
void lagfx_xg_reset(void);                       /* tests */
void lagfx_xg_stats_line(const lagfx_protocol_t *p, bool final);
void lagfx_xg_tick(const lagfx_protocol_t *p);   /* periodic: see above */

#endif
