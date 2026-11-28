/*
 * libapplegfx-vulkan — census of ARM64 exec segments the executor does not run (compute 1, information 4).
 * The record framing [tag u32][size u32][payload] is the Render body's; for these types it is an ASSUMPTION, so what contradicts it is recorded as well (stop reason,
 * truncated body, over-limit). Read-only: nothing here changes execution state. No Vulkan dependency (unit tested on its own).
 */
#ifndef LAGFX_PROTOCOL_SEG_CENSUS_H
#define LAGFX_PROTOCOL_SEG_CENSUS_H
#include <stdbool.h>
#include <stdint.h>

#define LAGFX_SEG_CENSUS_TABLE 64u        /* distinct (type, tag) pairs described */
#define LAGFX_SEG_DUMP_FILES   32u        /* distinct segment contents written, per type */
#define LAGFX_SEG_DUMP_MAX     (256u * 1024u)

enum { LAGFX_SEG_STOP_SIZE_LT8, LAGFX_SEG_STOP_PAST_BODY, LAGFX_SEG_STOP_INNER_LIMIT, LAGFX_SEG_STOP_TRAILING, LAGFX_SEG_STOP_N };

typedef struct lagfx_seg_census_stats {
    uint64_t segments[8];                         /* segments seen by type (type >= 7 -> 7) */
    uint64_t records;                             /* well-framed records walked */
    uint64_t overflow_records;                    /* RECORDS (not distinct pairs) whose (type, tag) did not fit the 64-entry table */
    uint64_t stops[8][LAGFX_SEG_STOP_N];          /* walks that ended before the body did, by type and reason (all of them; the log shows the first 4 per reason) */
    uint64_t truncated;                           /* segments whose body_len is larger than the bytes available (declared > captured) */
    uint64_t dump_files, dump_bytes;              /* files written (verified) */
    uint64_t dump_dup;                            /* content already written */
    uint64_t dump_too_big;                        /* captured > LAGFX_SEG_DUMP_MAX: not written */
    uint64_t dump_limit;                          /* new content beyond LAGFX_SEG_DUMP_FILES for its type: not written */
    uint64_t dump_fail;                           /* open / short write / close error: NOT registered as seen, so it is retried */
} lagfx_seg_census_stats_t;

/* One segment. buf = start of the segment record (header included), avail = bytes readable from buf, body_len = the DECLARED body length, prot_skipped as in
 * lagfx_exec_seg_t (24-byte header instead of 8). dump_dir may be NULL (no files). Types other than 1 / 4 are ignored. */
void lagfx_seg_census_note(uint32_t type, bool prot_skipped, uint64_t body_len, bool cont, const uint8_t *buf, uint32_t avail, const char *dump_dir);
const lagfx_seg_census_stats_t *lagfx_seg_census_stats(void);
void lagfx_seg_census_reset(void);   /* tests */
void lagfx_seg_census_summary(void); /* one g7-seg-census: SUMMARY line (stats above) + one per (type, tag) */
#endif
