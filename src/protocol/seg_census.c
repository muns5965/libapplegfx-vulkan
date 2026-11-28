/* libapplegfx-vulkan — see seg_census.h */
#include "seg_census.h"
#include "../common/le.h"
#include "../common/log.h"
#include <stdio.h>
#include <string.h>

#define MAX_INNER 4096u
static struct { uint32_t type, tag, szmin, szmax; uint64_t n; } g_tab[LAGFX_SEG_CENSUS_TABLE];
static uint32_t g_ntab;
static lagfx_seg_census_stats_t g_st;
static uint64_t g_seen[8][LAGFX_SEG_DUMP_FILES]; static uint32_t g_nseen[8];

const lagfx_seg_census_stats_t *lagfx_seg_census_stats(void) { return &g_st; }
void lagfx_seg_census_reset(void) { memset(&g_st, 0, sizeof g_st); memset(g_tab, 0, sizeof g_tab); memset(g_seen, 0, sizeof g_seen); memset(g_nseen, 0, sizeof g_nseen); g_ntab = 0; }

static uint64_t fnv(const uint8_t *b, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }

/* Write `n` bytes to path; true only when open, the full write and close all succeeded. */
static bool write_verified(const char *path, const uint8_t *b, uint64_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(b, 1, (size_t)n, f) == (size_t)n;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

static void dump_whole(uint32_t type, uint32_t t, bool prot, uint64_t declared, bool cont, const uint8_t *buf, uint64_t captured, const char *dir) {
    if (!dir || !dir[0] || !captured) return;
    if (captured > LAGFX_SEG_DUMP_MAX) { if (!g_st.dump_too_big++) LAGFX_LOG("g7-seg-census: DUMP-SKIP seg_type=%u captured=%llu > %u bytes (counted in SUMMARY dump_too_big)", type, (unsigned long long)captured, LAGFX_SEG_DUMP_MAX); return; }
    uint64_t h = fnv(buf, captured); uint32_t k = 0;
    while (k < g_nseen[t] && g_seen[t][k] != h) k++;
    if (k < g_nseen[t]) { g_st.dump_dup++; return; }
    if (g_nseen[t] >= LAGFX_SEG_DUMP_FILES) { if (!g_st.dump_limit++) LAGFX_LOG("g7-seg-census: DUMP-SKIP seg_type=%u: %u distinct contents already written (counted in SUMMARY dump_limit)", type, LAGFX_SEG_DUMP_FILES); return; }
    const bool trunc = declared > captured;
    char path[512]; snprintf(path, sizeof path, "%s/segdump_t%u_%016llx%s.bin", dir, type, (unsigned long long)h, trunc ? "_trunc" : "");
    if (!write_verified(path, buf, captured)) { if (!g_st.dump_fail++) LAGFX_LOG("g7-seg-census: DUMP-FAIL seg_type=%u path=%s (counted in SUMMARY dump_fail; retried on the next sight)", type, path); return; }
    g_seen[t][g_nseen[t]++] = h;   /* registered only after the file is complete */
    g_st.dump_files++, g_st.dump_bytes += captured;
    LAGFX_LOG("g7-seg-census: DUMP seg_type=%u declared_len=%llu captured_len=%llu truncated=%d prot_skipped=%d cont=%d -> %s", type, (unsigned long long)declared, (unsigned long long)captured, (int)trunc, (int)prot, (int)cont, path);
}

void lagfx_seg_census_note(uint32_t type, bool prot_skipped, uint64_t body_len, bool cont, const uint8_t *buf, uint32_t avail, const char *dump_dir) {
    if (type != 1u && type != 4u) return;
    const uint32_t t = type & 7u;
    const uint64_t pos = prot_skipped ? 24u : 8u;
    const uint64_t have = avail > pos ? avail - pos : 0, lim = body_len < have ? body_len : have;
    g_st.segments[t]++;
    const bool trunc = body_len > have;
    if (trunc) g_st.truncated++;
    dump_whole(type, t, prot_skipped, pos + body_len, cont, buf, pos + lim, dump_dir);
    uint64_t q = 0; uint32_t guard = 0, wsz = 0; int why = -1;
    while (q + 8u <= lim) {
        if (guard++ >= MAX_INNER) { why = LAGFX_SEG_STOP_INNER_LIMIT; break; }
        uint32_t tag = lagfx_le32(buf + pos + q), sz = lagfx_le32(buf + pos + q + 4);
        if (sz < 8u) { why = LAGFX_SEG_STOP_SIZE_LT8; wsz = sz; break; }
        if (q + sz > lim) { why = LAGFX_SEG_STOP_PAST_BODY; wsz = sz; break; }
        g_st.records++;
        uint32_t k = 0;
        while (k < g_ntab && !(g_tab[k].type == type && g_tab[k].tag == tag)) k++;
        if (k == g_ntab) {
            if (k >= LAGFX_SEG_CENSUS_TABLE) { g_st.overflow_records++; q += sz; continue; }
            g_tab[k].type = type, g_tab[k].tag = tag, g_tab[k].szmin = g_tab[k].szmax = sz, g_tab[k].n = 0, g_ntab++;
            char hx[3 * 64 + 1]; uint32_t nb = sz - 8u < 64u ? sz - 8u : 64u, o = 0;
            for (uint32_t i = 0; i < nb; i++) o += (uint32_t)snprintf(hx + o, sizeof hx - o, "%02x%s", buf[pos + q + 8u + i], i + 1u < nb ? " " : "");
            hx[o] = 0;
            LAGFX_LOG("g7-seg-census: FIRST seg_type=%u tag=%u size=%u (payload %u bytes) first %u payload bytes: %s", type, tag, sz, sz - 8u, nb, hx);
        }
        if (sz < g_tab[k].szmin) g_tab[k].szmin = sz;
        if (sz > g_tab[k].szmax) g_tab[k].szmax = sz;
        uint64_t n = ++g_tab[k].n;
        if (n >= 4u && !(n & (n - 1u)) && !(__builtin_ctzll(n) & 1))   /* 4, 16, 64, 256, ... */
            LAGFX_LOG("g7-seg-census: COUNT seg_type=%u tag=%u n=%llu size[min=%u max=%u]", type, tag, (unsigned long long)n, g_tab[k].szmin, g_tab[k].szmax);
        q += sz;
    }
    if (why < 0 && q < lim) why = LAGFX_SEG_STOP_TRAILING;
    if (why < 0 && trunc) why = LAGFX_SEG_STOP_TRAILING;   /* the body itself was cut short by the data available */
    if (why >= 0) {
        static const char *nm[LAGFX_SEG_STOP_N] = { "size<8", "size-past-body", "inner-limit", "trailing-bytes" };
        if (g_st.stops[t][why]++ < 4u) {
            char hx[3 * 32 + 1]; uint64_t rem = lim - q; uint32_t nb = rem < 32u ? (uint32_t)rem : 32u, o = 0;
            for (uint32_t i = 0; i < nb; i++) o += (uint32_t)snprintf(hx + o, sizeof hx - o, "%02x%s", buf[pos + q + i], i + 1u < nb ? " " : "");
            hx[o] = 0;
            LAGFX_LOG("g7-seg-census: STOP seg_type=%u why=%s at_off=%llu body_declared=%llu body_available=%llu rec_size=%u bytes: %s", type, nm[why], (unsigned long long)q, (unsigned long long)body_len, (unsigned long long)have, wsz, hx);
        }
    }
}

void lagfx_seg_census_summary(void) {
    const lagfx_seg_census_stats_t *s = &g_st;
    if (!s->segments[1] && !s->segments[4]) return;
    uint64_t stops = 0; for (int t = 0; t < 8; t++) for (int w = 0; w < LAGFX_SEG_STOP_N; w++) stops += s->stops[t][w];
    LAGFX_LOG("g7-seg-census: SUMMARY segments[compute=%llu info=%llu] records=%llu pairs=%u/%u overflow_records=%llu stops=%llu truncated_segments=%llu dump[files=%llu bytes=%llu dup=%llu too_big=%llu limit=%llu fail=%llu]",
              (unsigned long long)s->segments[1], (unsigned long long)s->segments[4], (unsigned long long)s->records, g_ntab, LAGFX_SEG_CENSUS_TABLE, (unsigned long long)s->overflow_records,
              (unsigned long long)stops, (unsigned long long)s->truncated, (unsigned long long)s->dump_files, (unsigned long long)s->dump_bytes, (unsigned long long)s->dump_dup,
              (unsigned long long)s->dump_too_big, (unsigned long long)s->dump_limit, (unsigned long long)s->dump_fail);
    for (uint32_t k = 0; k < g_ntab; k++)
        LAGFX_LOG("g7-seg-census: PAIR seg_type=%u tag=%u n=%llu size[min=%u max=%u]", g_tab[k].type, g_tab[k].tag, (unsigned long long)g_tab[k].n, g_tab[k].szmin, g_tab[k].szmax);
}
