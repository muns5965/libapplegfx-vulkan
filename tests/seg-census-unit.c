/* Census of compute / information segments (seg_census.c): framing, stop reasons, truncation, table overflow, verified + deduplicated dumps. No Vulkan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "protocol/seg_census.h"

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, m); exit(1); } } while (0)
static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
/* segment = 8-byte header + records; returns total length */
static uint32_t rec(uint8_t *b, uint32_t off, uint32_t tag, uint32_t size, uint8_t fill) { le32(b + off, tag); le32(b + off + 4, size); if (size > 8) memset(b + off + 8, fill, size - 8); return off + size; }
static const lagfx_seg_census_stats_t *S;

int main(void) {
    uint8_t b[8192]; memset(b, 0, sizeof b);
    S = lagfx_seg_census_stats();

    /* 1. well-framed: two tags, three records */
    lagfx_seg_census_reset();
    uint32_t o = 8; o = rec(b, o, 7, 16, 1); o = rec(b, o, 7, 24, 2); o = rec(b, o, 9, 8, 0);
    lagfx_seg_census_note(1, false, o - 8, false, b, o, NULL);
    CHECK(S->segments[1] == 1 && S->records == 3 && S->overflow_records == 0 && S->truncated == 0, "framed");
    for (int t = 0; t < 8; t++) for (int w = 0; w < LAGFX_SEG_STOP_N; w++) CHECK(S->stops[t][w] == 0, "no stop in a clean segment");

    /* 2. other types are ignored */
    lagfx_seg_census_note(0, false, o - 8, false, b, o, NULL); lagfx_seg_census_note(2, false, o - 8, false, b, o, NULL);
    CHECK(S->segments[0] == 0 && S->segments[2] == 0 && S->records == 3, "types 0/2 ignored");

    /* 3. record size < 8 */
    lagfx_seg_census_reset(); o = 8; o = rec(b, o, 1, 16, 0); le32(b + o, 5); le32(b + o + 4, 4); o += 8;
    lagfx_seg_census_note(4, false, o - 8, false, b, o, NULL);
    CHECK(S->records == 1 && S->stops[4][LAGFX_SEG_STOP_SIZE_LT8] == 1, "size<8 stop");

    /* 4. record running past the body */
    lagfx_seg_census_reset(); o = 8; o = rec(b, o, 1, 16, 0); le32(b + o, 2); le32(b + o + 4, 4096); o += 8;
    lagfx_seg_census_note(1, false, o - 8, false, b, o, NULL);
    CHECK(S->stops[1][LAGFX_SEG_STOP_PAST_BODY] == 1, "past-body stop");

    /* 5. trailing bytes shorter than a record header */
    lagfx_seg_census_reset(); o = 8; o = rec(b, o, 1, 16, 0); o += 4;
    lagfx_seg_census_note(1, false, o - 8, false, b, o, NULL);
    CHECK(S->stops[1][LAGFX_SEG_STOP_TRAILING] == 1, "trailing-bytes stop");

    /* 6. truncated: declared body longer than the bytes available */
    lagfx_seg_census_reset(); o = 8; o = rec(b, o, 1, 16, 0); o = rec(b, o, 1, 16, 0);
    lagfx_seg_census_note(1, false, 1000, false, b, o, NULL);
    CHECK(S->truncated == 1 && S->records == 2 && S->stops[1][LAGFX_SEG_STOP_TRAILING] == 1, "truncated body counted and flagged");

    /* 7. protection-skipped header (24 bytes) shifts the body */
    lagfx_seg_census_reset(); memset(b, 0, 64); o = 24; o = rec(b, o, 3, 16, 0);
    lagfx_seg_census_note(1, true, o - 24, false, b, o, NULL);
    CHECK(S->records == 1, "24-byte header");

    /* 8. table overflow counts RECORDS beyond 64 distinct (type, tag) pairs */
    lagfx_seg_census_reset(); o = 8; for (uint32_t t = 0; t < LAGFX_SEG_CENSUS_TABLE + 3; t++) o = rec(b, o, 100 + t, 8, 0);
    lagfx_seg_census_note(1, false, o - 8, false, b, o, NULL); lagfx_seg_census_note(1, false, o - 8, false, b, o, NULL);   /* the same 3 extra tags twice: 6 records, still 3 distinct */
    CHECK(S->records == 2 * (LAGFX_SEG_CENSUS_TABLE + 3) && S->overflow_records == 6, "overflow = records, not distinct pairs");

    /* 9. dumps: written, verified, deduplicated */
    char dir[] = "/tmp/segcensusXXXXXX"; CHECK(mkdtemp(dir), "mkdtemp");
    lagfx_seg_census_reset(); o = 8; o = rec(b, o, 7, 32, 5);
    lagfx_seg_census_note(1, false, o - 8, false, b, o, dir);
    CHECK(S->dump_files == 1 && S->dump_bytes == o && S->dump_fail == 0, "dump written");
    lagfx_seg_census_note(1, false, o - 8, false, b, o, dir);
    CHECK(S->dump_files == 1 && S->dump_dup == 1, "same content not written twice");
    lagfx_seg_census_note(1, false, o + 100, false, b, o, dir);   /* truncated copy of different declared length but same bytes: still the same content, dup */
    CHECK(S->dump_files == 1 && S->dump_dup == 2, "same captured bytes deduplicate");

    /* 10. failure to write is counted, NOT registered, and retried once the directory exists */
    char sub[256]; snprintf(sub, sizeof sub, "%s/later", dir);
    lagfx_seg_census_reset(); lagfx_seg_census_note(4, false, o - 8, false, b, o, sub);
    CHECK(S->dump_fail == 1 && S->dump_files == 0, "write failure counted");
    CHECK(mkdir(sub, 0700) == 0, "mkdir");
    lagfx_seg_census_note(4, false, o - 8, false, b, o, sub);
    CHECK(S->dump_files == 1 && S->dump_fail == 1, "retried after a failure (hash was not registered)");

    /* 11. oversize not written; distinct-content limit per type */
    lagfx_seg_census_reset(); static uint8_t big[LAGFX_SEG_DUMP_MAX + 64]; memset(big, 0, sizeof big); rec(big, 8, 1, 16, 0);
    lagfx_seg_census_note(1, false, sizeof big - 8, false, big, sizeof big, dir);
    CHECK(S->dump_too_big == 1 && S->dump_files == 0, "too big not written");
    for (uint32_t i = 0; i < LAGFX_SEG_DUMP_FILES + 5; i++) { o = 8; o = rec(b, o, 1000 + i, 16, (uint8_t)i); lagfx_seg_census_note(1, false, o - 8, false, b, o, dir); }
    CHECK(S->dump_files == LAGFX_SEG_DUMP_FILES && S->dump_limit == 5, "per-type file limit counted");

    lagfx_seg_census_summary();
    printf("PASS: segment census (framing, stops, truncation, overflow, verified dumps)\n");
    return 0;
}
