/*
 * libapplegfx-vulkan — gamma-descriptor diagnostics (READ-ONLY: logging and counters; nothing here changes what is rendered)
 * src/protocol/gamma_diag.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Evidence tags used in this file's comments: [V] verified from source / logs, [I] inferred, [N] not known.
 *
 * What the op 0x08 (display swap, channel 4) descriptor is [V: the host's decode in devices/pvg_lite.c display_swap prints the same words; the library's raw dump finds them at
 * these word positions]: payload after the barrier slots, little-endian u32 words
 *     w0 port, w1, w2:w3 mapping id (u64), w4:w5 off (u64), w6:w7 len (u64), w8 count, w9 sum.
 * The only live example is {off 0x118000, len 0x4000, count 257 (0x101), sum 0x48400000} on the first swap, all zero afterwards.
 *
 * What it points at [V from three older host-side logs g7_cboot_1795609367823 / ..612612463 / ..617740573, "hvgamma: task 0@off ..." lines, host task_read of task 0 at VA 0x118000;
 * the lagfx library did not read this memory in those runs]:
 *   words 0..23 at +0x000 = 0x3b800000 (0.00390625 = 1/256), 0x3c000000 (2/256) ... i.e. word i = i / 256.0f (float32, little endian);
 *   words at +0x600 (word 384) = 127/256, 128/256 ...; words at +0xbf0 (word 764) = 250/256 ... 256/256 = 1.0f, then zeros.
 *   => [I] ONE table = 3 planes x 257 float32 (count = 257 entries per plane, entry i = i/256, i = 0..256), plane c starts at word c * 257 (plane 1 at word 257: 384-257 = 127 OK;
 *      plane 2 at word 514: 764-514 = 250 OK; the last entry 256 sits at word 770, word 771 on is zero). 771 words = 3084 bytes inside the 0x4000 (one 16 KiB page) region.
 *      Plane order (R,G,B?) is [N]: the three planes were identical in the logs (identity), so the order cannot be told apart.
 *   => [V arithmetic, reproduced offline] the wrapping 32-bit sum of those 771 words, taken as raw u32 bit patterns, is EXACTLY 0x48400000 (the descriptor's `sum`).
 *      So `sum` is an additive checksum over the 3 * count words (as integers, not a float sum: the float sum would be 385.5). That 0x48400000 also happens to be float 196608.0 is a
 *      coincidence of the identity ramp. [I] generalising "sum = wrapping u32 sum of 3*count words" from one example (the identity table) is untested for a non-identity table.
 * In the same older logs task 2 held shader-library text ("color", "generated(5color...") or zeros at VA 0x118000, i.e. the SAME VA exists in several tasks with different content:
 * a read that merely succeeds proves nothing, the checksum is what tells the right task. [I] the LUT lives in task 0, not in the display-mapping owner (task 2 in the integ4 run).
 *
 * Log lines (all bounded; through the same stderr + LAGFX_LOG channel as display_diag.c):
 *   g7-pvg-gamma: op8 desc #<n> ch=<c> mapping=<m> off=0x.. len=0x.. count=<k> sum=0x..   on every CHANGE of the descriptor (max 64 lines), zero descriptors included
 *   g7-pvg-gamma: op8 desc #<n> PARSE FAIL: <why>                                          (max 8 lines)
 *   g7-pvg-gamma: region ... / region read FAILED ...                                       the read-only copy attempt (formats as before display_diag.c grew this module)
 *   g7-pvg-gamma: walk swap#<n> task=<t> ...                                                level-by-level 16 KiB walk of VA 0x114000 and 0x118000..0x134000 (max 64 lines)
 *   g7-pvg-gamma: ram-probe swap#<n> ...                                                    first 64 bytes at RAM base + 0x118000 (max 12 lines)
 *   g7-pvg-gamma: map-probe swap#<n> mapping=<m> ...                                        first 64 bytes at offset 0x118000 inside the swap's mapping (max 12 lines)
 *   g7-pvg-gamma: lut-probe swap#<n> task=<t> ...                                           read of the descriptor's region in every live task + checksum / ramp decode (max 64 lines)
 * Sampled swaps (n = index of the channel-4 op 0x08): 1, 2, 3, 4, 8, 16, 32, 64, 128.
 */
#ifndef LAGFX_PROTOCOL_GAMMA_DIAG_H
#define LAGFX_PROTOCOL_GAMMA_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "display_diag.h"

/* ---- pure helpers (static inline: the unit tests use them without any guest memory) ---- */

#define LAGFX_GD_MAX_LEN 0x10000u   /* the largest region this diagnostic will read (one 64 KiB window); the live descriptor is 0x4000 */

typedef struct { uint32_t port, w1, count, sum; uint64_t mapping, off, len; } lagfx_gd_desc_t;

enum { LAGFX_GD_DESC_OK = 0, LAGFX_GD_DESC_SHORT = -1, LAGFX_GD_DESC_BAD = -2 };

/* Parses the 10-word descriptor at pl[skip...]. SHORT: fewer than 40 bytes after the barrier slots. BAD: len above LAGFX_GD_MAX_LEN, off + len wraps, one of off / len zero while the
 * other is not, or a count whose 3 * count words do not fit in len. An all-zero descriptor is OK ("no table"). Never reads outside pl[0..plen). */
static inline int lagfx_gd_desc_parse(const uint8_t *pl, size_t plen, size_t skip, lagfx_gd_desc_t *d) {
    if (!pl || !d) return LAGFX_GD_DESC_SHORT;
    memset(d, 0, sizeof *d);
    if (skip > plen || plen - skip < 40u) return LAGFX_GD_DESC_SHORT;
    const uint8_t *b = pl + skip;
    d->port = lagfx_dd_rd32(b); d->w1 = lagfx_dd_rd32(b + 4);
    d->mapping = lagfx_dd_rd32(b + 8) | (uint64_t)lagfx_dd_rd32(b + 12) << 32;
    d->off = lagfx_dd_rd32(b + 16) | (uint64_t)lagfx_dd_rd32(b + 20) << 32;
    d->len = lagfx_dd_rd32(b + 24) | (uint64_t)lagfx_dd_rd32(b + 28) << 32;
    d->count = lagfx_dd_rd32(b + 32); d->sum = lagfx_dd_rd32(b + 36);
    if (d->len > LAGFX_GD_MAX_LEN) return LAGFX_GD_DESC_BAD;
    if (d->off + d->len < d->off) return LAGFX_GD_DESC_BAD;
    if ((d->off != 0u) != (d->len != 0u)) return LAGFX_GD_DESC_BAD;
    if (d->len != 0u && (uint64_t)d->count * 12u > d->len) return LAGFX_GD_DESC_BAD;   /* [I] 3 planes * count * 4 bytes */
    return LAGFX_GD_DESC_OK;
}

typedef struct {
    bool fits;              /* len holds 3 * count words */
    uint32_t nwords;        /* 3 * count */
    uint32_t sum;           /* wrapping u32 sum of those words (raw bit patterns) */
    bool sum_match;         /* sum == the descriptor's sum */
    bool tail_zero;         /* every byte after the 3 * count words (up to len) is zero */
    bool finite;            /* no NaN / Inf among the words */
    bool monotonic[3];      /* each plane non-decreasing */
    float maxdev;           /* max |v - i / (count - 1)| over the three planes (identity ramp i/256 for count 257); -1 if not finite / not computable */
    float first[3], last[3];/* entry 0 and entry count-1 of each plane */
} lagfx_gd_lut_t;

static inline float lagfx_gd_f32(const uint8_t *b) { uint32_t w = lagfx_dd_rd32(b); float f; memcpy(&f, &w, 4); return f; }

/* DIAGNOSTIC-ONLY decode of the [I] layout above (planar, 3 x count float32). Never applied to anything. 0 = decoded (check ->fits / ->sum_match), -1 = bad arguments. */
static inline int lagfx_gd_lut_decode(const uint8_t *buf, size_t len, uint32_t count, uint32_t expect_sum, lagfx_gd_lut_t *o) {
    if (!o) return -1;
    memset(o, 0, sizeof *o);
    o->maxdev = -1.0f;
    if (!buf || count < 2u || count > 4096u) return -1;
    o->nwords = 3u * count;
    if ((size_t)o->nwords * 4u > len) return 0;
    o->fits = true;
    uint32_t s = 0;
    for (uint32_t i = 0; i < o->nwords; i++) s += lagfx_dd_rd32(buf + 4u * i);
    o->sum = s; o->sum_match = s == expect_sum;
    o->tail_zero = true;
    for (size_t i = (size_t)o->nwords * 4u; i < len; i++) if (buf[i]) { o->tail_zero = false; break; }
    o->finite = true;
    float dev = 0.0f;
    for (int c = 0; c < 3; c++) {
        o->monotonic[c] = true;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = buf + 4u * ((size_t)c * count + i);
            uint32_t w = lagfx_dd_rd32(e);
            if (((w >> 23) & 0xffu) == 0xffu) { o->finite = false; continue; }
            float v = lagfx_gd_f32(e);
            if (i == 0) o->first[c] = v;
            if (i == count - 1u) o->last[c] = v;
            if (i && v < lagfx_gd_f32(e - 4)) o->monotonic[c] = false;
            float d = v - (float)i / (float)(count - 1u); if (d < 0) d = -d;
            if (d > dev) dev = d;
        }
    }
    if (o->finite) o->maxdev = dev;
    return 0;
}

/* ---- stateful part (gamma_diag.c); process-wide counters like display_diag.c ---- */

/* Task address space read for DIAGNOSTICS, status-returning. Selects the page-table geometry by the SAME switch the real path uses (lagfx_proto_arm64(p), i.e. the device's
 * protocol_mode): arm64-vmapple -> lagfx_arm64_task_read (root page = root_pfn << 14, 16 KiB pages, 12 index bits per level; the geometry of every real read of the 0x43 exec,
 * object lists, descriptors, shader libraries); x86 -> the legacy lagfx_task_read_virtual (root = root_pfn << 12, 4 KiB pages, 10 index bits). */
enum { LAGFX_GD_RD_OK = 0, LAGFX_GD_RD_UNMAPPED = 1, LAGFX_GD_RD_PHYS_FAIL = 2, LAGFX_GD_RD_NO_TASK = 3 };
int  lagfx_gd_task_read(lagfx_protocol_t *p, uint32_t task_id, uint64_t va, uint32_t len, uint8_t *out);
/* The same, as the bool reader type display_diag.h registers (lagfx_dd_set_reader). */
bool lagfx_gd_task_read_bool(lagfx_protocol_t *p, uint32_t task_id, uint64_t va, uint32_t len, uint8_t *out);
/* Registers the status-returning reader the region read uses to classify failures (display_diag.c's bool reader classifies every failure as unmapped). */
typedef int (*lagfx_gd_reader_t)(lagfx_protocol_t *p, uint32_t task_id, uint64_t va, uint32_t len, uint8_t *out);
void lagfx_gd_set_reader(lagfx_gd_reader_t r);

typedef struct {
    uint64_t read_attempts, read_done;
    uint64_t translate_ok;           /* attempts in which some task translated the WHOLE range VA -> GPA (read_done + phys_read_fail) */
    uint64_t unmapped;               /* attempts in which no task could translate the range (page table says unmapped / no such task) */
    uint64_t phys_read_fail;         /* attempts that translated but whose guest-physical read failed (no RAM there) and no task read it */
    uint64_t descriptor_parse_fail;  /* op 0x08 descriptors that were short / inconsistent (no read attempted) */
    uint64_t applied_ok;             /* gamma applied to rendering: ALWAYS 0, nothing applies a table */
    uint64_t sum_match, sum_mismatch;/* diagnostic decode of a region that read OK: checksum equals the descriptor's / does not (region reads and lut-probes) */
    uint64_t descs_seen, desc_changes;
} lagfx_gd_stats_t;
void lagfx_gd_stats_get(lagfx_gd_stats_t *s);
/* " translate_ok=.. unmapped=.. phys_read_fail=.. descriptor_parse_fail=.. applied_ok=0 sum_match=.. sum_mismatch=.." (leading space, no brackets), appended inside gamma[...]. */
size_t lagfx_gd_stats_suffix(char *out, size_t cap);

/* Every channel-4 op 0x08 (its 1-based index among them is counted here: descs_seen, equal to display_diag.c's ch4 count in production). pl/plen = the command payload after the 12-byte header, skip = barrier bytes (arg8 * 8). `reader` / `owner` are the
 * display_diag.c registrations (the region read keeps their order: owner of the mapping, last 0x44, scan). */
void lagfx_gd_note_op8(lagfx_protocol_t *p, uint32_t chan, const uint8_t *pl, uint32_t plen, uint32_t skip, lagfx_dd_reader_t reader, lagfx_dd_owner_t owner);
void lagfx_gd_test_reset(void);   /* tests only: zero the counters and the one-shot state */

#endif
