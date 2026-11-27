/*
 * libapplegfx-vulkan — sampler address / filter / mip mapping and fidelity counters (unit, no device)
 * tests/vulkan-sampler-mapping-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * State level only (no VkDevice): constexpr words -> lagfx_sampler_state_t through the production path (from_constexpr_norm) and the test-only path
 * (from_constexpr), both of which call the one mapper (lagfx_cxs_map_*); the runtime sampler (arm64_sampler.c) for values outside the public Metal enum range.
 * Field numbering of the constexpr word is INFER (see constexpr_sampler.h). The T-numbers refer to the audit's test list.
 */

#include "libapplegfx-vulkan.h"
#include <stdio.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { fprintf(stderr, "built without Vulkan; skipping\n"); return 77; }
#else

#include "vulkan/sampler_cache.h"
#include "protocol/arm64_sampler.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED comes from vulkan/sampler_cache.h (one definition for product and test). No compat-mode selector exists in the product
 * yet (the bicubic -> linear mapping is the only mode), so the test assumes compat mode explicitly; when a selector is added, set g_compat_mode from it. */
static const int g_compat_mode = 1;

static int g_nwarn, g_nlog;
static char g_last[512];
void lagfx_log_impl(const char *fmt, ...)   { va_list a; va_start(a, fmt); vsnprintf(g_last, sizeof g_last, fmt, a); va_end(a); g_nlog++; }
void lagfx_warn_impl(const char *fmt, ...)  { va_list a; va_start(a, fmt); vsnprintf(g_last, sizeof g_last, fmt, a); va_end(a); g_nwarn++; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#define EDGE VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
#define BORD VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
#define REP  VK_SAMPLER_ADDRESS_MODE_REPEAT
#define MIR  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT

/* base word of the captured family: marker bit 63, compare field 8 (meaning unverified), LOD clamp [0, max half]; low 24 bits are the address/filter/mip/pixel fields */
#define BASE 0x807bff0000080000ull
static uint64_t W(unsigned a0, unsigned a1, unsigned a2, unsigned mag, unsigned mn, unsigned mip, bool pixel, unsigned cmp) {
    uint64_t w = (BASE & ~(0xfull << 16)) | (uint64_t)(cmp & 15u) << 16;
    w |= (uint64_t)a0 | (uint64_t)a1 << 3 | (uint64_t)a2 << 6 | (uint64_t)mag << 9 | (uint64_t)mn << 11 | (uint64_t)mip << 13 | (pixel ? 1ull << 15 : 0);
    return w;
}
static lagfx_constexpr_sampler_t D(uint64_t w) { lagfx_constexpr_sampler_t c; bool ok = lagfx_constexpr_sampler_decode(w, &c); CHECK(ok); return c; }
static bool seq(const lagfx_sampler_state_t *a, const lagfx_sampler_state_t *b) {
    return a->mag_filter == b->mag_filter && a->min_filter == b->min_filter && a->mip_mode == b->mip_mode && a->addr_u == b->addr_u && a->addr_v == b->addr_v &&
           a->addr_w == b->addr_w && a->lod_min == b->lod_min && a->lod_max == b->lod_max && a->max_anisotropy == b->max_anisotropy && a->compare_enable == b->compare_enable &&
           a->compare_op == b->compare_op && a->border == b->border && a->unnormalized == b->unnormalized;
}

/* The mapping of the previous revision (verbatim semantics: address % 5 table, nonzero filter = linear, mip 2 = linear), for the byte-identity assertions. */
static lagfx_sampler_state_t old_ref(const lagfx_constexpr_sampler_t *cs) {
    lagfx_sampler_state_t s = lagfx_sampler_state_default();
    static const VkSamplerAddressMode am[5] = { BORD, EDGE, REP, MIR, BORD };
    s.addr_u = am[cs->addr[0] % 5u]; s.addr_v = am[cs->addr[1] % 5u]; s.addr_w = am[cs->addr[2] % 5u];
    s.mag_filter = cs->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.min_filter = cs->min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.mip_mode = cs->mip_filter == 2u ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.lod_min = cs->lod_min; s.lod_max = cs->mip_filter ? cs->lod_max : 0.0f;
    s.max_anisotropy = (float)cs->max_aniso; s.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (!cs->normalized) {
        bool ok = true;
        for (int i = 0; i < 2; i++) if (cs->addr[i] == 2u || cs->addr[i] == 3u) ok = false;
        if (ok && cs->mip_filter == 0u && cs->max_aniso == 1u) {
            s.unnormalized = true; s.lod_min = 0.0f; s.lod_max = 0.0f; s.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST; s.addr_w = s.addr_u; s.mag_filter = s.min_filter;
        }
    }
    if (s.lod_max >= 1000.0f) s.lod_max = VK_LOD_CLAMP_NONE;
    if (s.lod_min < 0.0f) s.lod_min = 0.0f;
    if (s.max_anisotropy < 1.0f) s.max_anisotropy = 1.0f;
    return s;
}

static lagfx_sampler_diag_t dg(void) { lagfx_sampler_diag_t d; lagfx_sampler_diag_get(&d); return d; }

static void samp_blob(uint8_t *b, uint32_t st) {
    memset(b, 0, 36);
    b[0] = 3; b[4] = 36; b[8] = 7; memcpy(b + 12, &st, 4);
    float hi = 3.4028235e38f; memcpy(b + 24, &hi, 4);
}

int main(void) {
    setenv("LAGFX_LOG_LEVEL", "info", 1);   /* the approximation lines are INFO (the unsupported ones WARN) */
    VkSamplerAddressMode am;
    VkFilter f; VkSamplerMipmapMode mm;

    /* ---- the mapper itself ---- */
    CHECK(lagfx_cxs_map_address(0, &am) == LAGFX_CXS_APPROX && am == BORD);
    CHECK(lagfx_cxs_map_address(1, &am) == LAGFX_CXS_EXACT && am == EDGE);
    CHECK(lagfx_cxs_map_address(2, &am) == LAGFX_CXS_EXACT && am == REP);
    CHECK(lagfx_cxs_map_address(3, &am) == LAGFX_CXS_EXACT && am == MIR);
    CHECK(lagfx_cxs_map_address(4, &am) == LAGFX_CXS_EXACT && am == BORD);
    for (uint32_t v = 5; v < 256; v++) CHECK(lagfx_cxs_map_address(v, &am) == LAGFX_CXS_UNSUPPORTED && am == EDGE);   /* never wraps onto another mode */
    CHECK(lagfx_cxs_map_filter(0, &f) == LAGFX_CXS_EXACT && f == VK_FILTER_NEAREST);
    CHECK(lagfx_cxs_map_filter(1, &f) == LAGFX_CXS_EXACT && f == VK_FILTER_LINEAR);
    CHECK(lagfx_cxs_map_filter(2, &f) == LAGFX_CXS_APPROX && f == VK_FILTER_LINEAR);
    CHECK(lagfx_cxs_map_filter(3, &f) == LAGFX_CXS_UNSUPPORTED && f == VK_FILTER_LINEAR);
    CHECK(lagfx_cxs_map_mip(0, &mm) == LAGFX_CXS_EXACT && mm == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    CHECK(lagfx_cxs_map_mip(1, &mm) == LAGFX_CXS_EXACT && mm == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    CHECK(lagfx_cxs_map_mip(2, &mm) == LAGFX_CXS_EXACT && mm == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    CHECK(lagfx_cxs_map_mip(3, &mm) == LAGFX_CXS_UNSUPPORTED && mm == VK_SAMPLER_MIPMAP_MODE_NEAREST);

    /* ---- byte identity for every VALID input (address 0..4, filters 0..1, mip 0..2, normalized and pixel, both paths) and observed words ---- */
    lagfx_sampler_diag_reset();
    int n_valid = 0;
    for (unsigned a = 0; a <= 4; a++) for (unsigned b = 0; b <= 4; b++) for (unsigned mg = 0; mg <= 1; mg++) for (unsigned mn = 0; mn <= 1; mn++)
        for (unsigned mip = 0; mip <= 2; mip++) for (unsigned px = 0; px <= 1; px++) {
            lagfx_constexpr_sampler_t cs = D(W(a, b, a, mg, mn, mip, px, 8));
            lagfx_sampler_state_t nw = lagfx_sampler_state_from_constexpr(&cs), ol = old_ref(&cs);
            CHECK(seq(&nw, &ol)); n_valid++;
            lagfx_sampler_state_t pn = lagfx_sampler_state_from_constexpr_norm(&cs);   /* production: never Vulkan-unnormalized; shares the address / filter fields */
            CHECK(!pn.unnormalized && pn.addr_u == nw.addr_u && pn.addr_v == nw.addr_v && pn.addr_w == (nw.unnormalized ? pn.addr_w : nw.addr_w) && pn.mag_filter == (nw.unnormalized ? pn.mag_filter : nw.mag_filter));
        }
    CHECK(n_valid == 5 * 5 * 2 * 2 * 3 * 2);
    static const uint64_t observed[4] = { 0x807bff0000088a00ull, 0x807bff0000088000ull, 0x807bff0000080092ull, 0x807bff0000080a49ull };   /* the four captured words */
    for (int i = 0; i < 4; i++) {
        lagfx_constexpr_sampler_t cs = D(observed[i]);
        lagfx_sampler_state_t nw = lagfx_sampler_state_from_constexpr(&cs), ol = old_ref(&cs), pn = lagfx_sampler_state_from_constexpr_norm(&cs);
        CHECK(seq(&nw, &ol));
        CHECK(!pn.unnormalized);
    }
    /* T1..T5, T10: the documented states */
    { lagfx_constexpr_sampler_t cs = D(0x807bff0000080000ull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T1 */
      CHECK(s.addr_u == BORD && s.addr_v == BORD && s.addr_w == BORD && s.border == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK && s.mag_filter == VK_FILTER_NEAREST); }
    { lagfx_constexpr_sampler_t cs = D(0x807bff0000080049ull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.addr_u == EDGE && s.addr_v == EDGE && s.addr_w == EDGE); }   /* T2 */
    { lagfx_constexpr_sampler_t cs = D(0x807bff0000080092ull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.addr_u == REP && s.addr_w == REP); }   /* T3 */
    { lagfx_constexpr_sampler_t cs = D(0x807bff00000800dbull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.addr_u == MIR && s.addr_v == MIR); }   /* T4 */
    { lagfx_constexpr_sampler_t cs = D(0x807bff0000080124ull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.addr_u == BORD && s.border == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK); }   /* T5 */
    { lagfx_constexpr_sampler_t cs = D(0x807bff0000080a00ull); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.mag_filter == VK_FILTER_LINEAR && s.min_filter == VK_FILTER_LINEAR); }   /* T10 */
    /* Valid words: only the documented approximation counter for clamp_to_zero moved; nothing unsupported. */
    { lagfx_sampler_diag_t d = dg(); CHECK(d.cxs_state_builds_addr_outside_table == 0 && d.cxs_state_builds_filter_outside_table == 0 && d.cxs_state_builds_mip_outside_table == 0 && d.cxs_state_builds_bicubic_as_linear == 0 && d.cxs_state_builds_compare_field_not8 == 0 && d.cxs_testpath_state_builds_unnorm_illegal_addr > 0 && d.cxs_state_builds_clamp_zero_as_border > 0); }

    /* ---- address values 5..7, outside our INFER table (T6, T7, T8, T17, T20) ---- */
    lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
    lagfx_sampler_state_t edge_state, st5, st6, st7;
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 1, 1, 0, false, 8)); edge_state = lagfx_sampler_state_from_constexpr_norm(&cs); }
    { lagfx_constexpr_sampler_t cs = D(W(5, 5, 5, 1, 1, 0, false, 8)); st5 = lagfx_sampler_state_from_constexpr_norm(&cs); }   /* T6 (old: CLAMP_TO_BORDER via % 5) */
    CHECK(st5.addr_u == EDGE && st5.addr_v == EDGE && st5.addr_w == EDGE);
    CHECK(dg().cxs_state_builds_addr_outside_table == 1 && g_nwarn == 1 && strstr(g_last, "addr_outside_table[5,5,5]->clamp_to_edge"));
    CHECK(strstr(g_last, "cxs word=0x") && strstr(g_last, "fidelity=OUTSIDE_TABLE:"));
    { lagfx_constexpr_sampler_t cs = D(W(6, 6, 6, 1, 1, 0, false, 8)); st6 = lagfx_sampler_state_from_constexpr_norm(&cs); }   /* T7 */
    { lagfx_constexpr_sampler_t cs = D(W(7, 7, 7, 1, 1, 0, false, 8)); st7 = lagfx_sampler_state_from_constexpr_norm(&cs); }   /* T8 (old: REPEAT via % 5) */
    CHECK(st6.addr_u == EDGE && st7.addr_u == EDGE && st7.addr_v == EDGE && st7.addr_w == EDGE);
    CHECK(dg().cxs_state_builds_addr_outside_table == 3 && g_nwarn == 3);
    CHECK(seq(&st5, &edge_state) && seq(&st6, &edge_state) && seq(&st7, &edge_state));   /* T20: same post-mapping state => one cache entry (the key is built from this state) */
    { lagfx_constexpr_sampler_t cs = D(W(7, 7, 7, 1, 1, 0, false, 8)); (void)lagfx_sampler_state_from_constexpr_norm(&cs); }   /* the same word again: counted, NOT logged again */
    CHECK(dg().cxs_state_builds_addr_outside_table == 4 && g_nwarn == 3 && dg().cxs_distinct_words_logged == 3);
    { lagfx_constexpr_sampler_t cs = D(W(2, 7, 1, 1, 1, 0, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.addr_u == REP && s.addr_v == EDGE && s.addr_w == EDGE); }   /* per axis */
    /* T17 / the L9 bug: a pixel word with a raw 7 must not become unnormalized + REPEAT (illegal Vulkan); repeat/mirror still keep the state normalized */
    for (unsigned a = 0; a <= 7; a++) for (unsigned b = 0; b <= 7; b++) {
        lagfx_constexpr_sampler_t cs = D(W(a, b, 0, 1, 1, 0, true, 8));
        lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr(&cs);
        if (s.unnormalized) CHECK((s.addr_u == EDGE || s.addr_u == BORD) && (s.addr_v == EDGE || s.addr_v == BORD) && (s.addr_w == EDGE || s.addr_w == BORD));
        bool rm = (a == 2 || a == 3 || b == 2 || b == 3);
        CHECK(s.unnormalized == !rm);
        lagfx_sampler_state_t pn = lagfx_sampler_state_from_constexpr_norm(&cs);
        CHECK(!pn.unnormalized && pn.addr_u == s.addr_u && pn.addr_v == s.addr_v);   /* production and test-only path agree on the mapping */
    }
    { lagfx_sampler_diag_reset(); lagfx_constexpr_sampler_t cs = D(W(2, 1, 1, 0, 0, 0, true, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr(&cs);
      CHECK(!s.unnormalized && s.addr_u == REP && dg().cxs_testpath_state_builds_unnorm_illegal_addr == 1);
      lagfx_sampler_state_t pn = lagfx_sampler_state_from_constexpr_norm(&cs); (void)pn; CHECK(dg().cxs_testpath_state_builds_unnorm_illegal_addr == 1); }   /* production never builds an unnormalized VkSampler: nothing to refuse */

    /* ---- filters, mip, compare (T9 needs a device and is not covered here; T11..T15) ---- */
    lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 2, 2, 0, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T11 */
      /* Current compatibility behaviour: a bicubic word (filter 2) gets exactly the state of a plain linear word and is counted as such. */
      lagfx_sampler_state_t lin; { lagfx_constexpr_sampler_t cl = D(W(1, 1, 1, 1, 1, 0, false, 8)); lin = lagfx_sampler_state_from_constexpr_norm(&cl); }
      lagfx_sampler_diag_t d = dg();
if (!LAGFX_CXS_EXACT_BICUBIC_IMPLEMENTED || g_compat_mode) {
      CHECK(s.mag_filter == VK_FILTER_LINEAR && s.min_filter == VK_FILTER_LINEAR);
      CHECK(seq(&s, &lin));
      CHECK(d.cxs_state_builds_bicubic_as_linear == 1 && d.cxs_state_builds_filter_outside_table == 0);
      CHECK(g_nlog == 1 && g_nwarn == 0 && strstr(g_last, "fidelity=APPROX:") && strstr(g_last, "bicubic_as_linear[mag=2,min=2]")); }
      else {
      /* Placeholder for future work: the exact path must not be counted as an approximation; its state expectations belong to that implementation. */
      CHECK(d.cxs_state_builds_bicubic_as_linear == 0 && d.cxs_state_builds_filter_outside_table == 0);
      (void)s; (void)lin; }
    }
    { lagfx_sampler_diag_reset(); lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 3, 3, 0, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T11b: filter 3 is outside the table, not bicubic */
      CHECK(s.mag_filter == VK_FILTER_LINEAR && s.min_filter == VK_FILTER_LINEAR);
      lagfx_sampler_diag_t d = dg(); CHECK(d.cxs_state_builds_filter_outside_table == 1 && d.cxs_state_builds_bicubic_as_linear == 0); }
    lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 3, 1, 0, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T12 */
      CHECK(s.mag_filter == VK_FILTER_LINEAR); CHECK(dg().cxs_state_builds_filter_outside_table == 1 && g_nwarn == 1); }
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 0, 0, 1, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T13 */
      CHECK(s.mip_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST && s.lod_max >= 1000.f); }
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 0, 0, 2, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.mip_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR); }
    { lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 0, 0, 0, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(s.lod_max == 0.0f); }
    { uint64_t m = dg().cxs_state_builds_mip_outside_table; lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 0, 0, 3, false, 8)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T14 */
      CHECK(s.mip_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST && dg().cxs_state_builds_mip_outside_table == m + 1); }
    { uint64_t c0 = dg().cxs_state_builds_compare_field_not8; lagfx_constexpr_sampler_t cs = D(W(1, 1, 1, 0, 0, 0, false, 3)); lagfx_sampler_state_t s = lagfx_sampler_state_from_constexpr_norm(&cs);   /* T15 */
      CHECK(!s.compare_enable && s.compare_op == VK_COMPARE_OP_NEVER && dg().cxs_state_builds_compare_field_not8 == c0 + 1);
      lagfx_constexpr_sampler_t c8 = D(W(1, 1, 1, 0, 0, 0, false, 8)); (void)lagfx_sampler_state_from_constexpr_norm(&c8); CHECK(dg().cxs_state_builds_compare_field_not8 == c0 + 1); }
    { uint64_t c0 = dg().cxs_state_builds_clamp_zero_as_border; lagfx_constexpr_sampler_t cs = D(W(0, 1, 1, 0, 0, 0, false, 8)); (void)lagfx_sampler_state_from_constexpr_norm(&cs); CHECK(dg().cxs_state_builds_clamp_zero_as_border == c0 + 1); }
    { lagfx_constexpr_sampler_t bad; CHECK(!lagfx_constexpr_sampler_decode(0x0ull, &bad));   /* T16: an undecodable word is the default state (no counter yet) */
      lagfx_sampler_state_t d0 = lagfx_sampler_state_default(), s = lagfx_sampler_state_from_constexpr_norm(&bad); CHECK(seq(&d0, &s)); }

    /* ---- runtime sampler (T18, T19). Words below are SYNTHETIC (0x84000000|m<<8|m<<12, 0x84000100, 0x87000000, 0x84000600) except the 4 captured runtime words in obs[]. Values 6 / 7 keep their clamp-to-edge mapping; counted + logged once per state word; valid values unchanged ---- */
    lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
    { uint8_t b[36]; VkSamplerCreateInfo ci; uint32_t fl;
      for (uint32_t m = 0; m <= 7; m++) {
          samp_blob(b, 0x84000000u | m << 8 | m << 12);
          fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, true, &ci);
          static const VkSamplerAddressMode exp[8] = { EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, REP, MIR, BORD, BORD, EDGE, EDGE };
          CHECK(ci.addressModeU == exp[m] && ci.addressModeV == exp[m]);   /* exact mirror-clamp-to-edge and all valid values unchanged */
          CHECK(((fl & LAGFX_A64_SAMP_ADDR_UNKNOWN) != 0) == (m >= 6u));
      }
      samp_blob(b, 0x84000000u | 1u << 8); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci);   /* T19 */
      CHECK(ci.addressModeU == MIR && (fl & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX) && !(fl & LAGFX_A64_SAMP_ADDR_UNKNOWN));
      samp_blob(b, 0x87000000u); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci);   /* mip 3 */
      CHECK((fl & LAGFX_A64_SAMP_MIP_UNKNOWN) && ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
      for (uint32_t m = 0; m <= 2; m++) { samp_blob(b, 0x84000000u | m << 24); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci); CHECK(!(fl & LAGFX_A64_SAMP_MIP_UNKNOWN)); }
      /* the 4 CAPTURED runtime words (own dumps) are out-of-range-free */
      static const uint32_t obs[4] = { 0x84000000u, 0x84000005u, 0x04000005u, 0x04000000u };
      for (int i = 0; i < 4; i++) { samp_blob(b, obs[i]); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci); CHECK(!(fl & (LAGFX_A64_SAMP_ADDR_UNKNOWN | LAGFX_A64_SAMP_MIP_UNKNOWN)) && ci.addressModeU == EDGE); }
      /* the counter / once-per-word log (0x84000600 and 0x87000000 are SYNTHETIC) */
      lagfx_sampler_diag_rt_count(true, false, false, 0x84000600u); lagfx_sampler_diag_rt_count(true, false, false, 0x84000600u); lagfx_sampler_diag_rt_count(false, true, false, 0x87000000u);
      lagfx_sampler_diag_t d = dg(); CHECK(d.rt_sampler_objs_addr_out_of_range == 2 && d.rt_sampler_objs_mip_out_of_range == 1 && g_nwarn == 2); }
    /* ---- runtime min / mag filter fields (2 bits each, INFER): 0 nearest, 1 linear are exact; 2 / 3 are outside MTLSamplerMinMagFilter -> LINEAR + FILTER_UNKNOWN. The old decoder read bit 0 only, so
     * the code 2 (bit 1 set, bit 0 clear: a would-be bicubic code) silently became NEAREST. SYNTHETIC words (the captured ones only have the bit 0 / bit 2 patterns). ---- */
    { uint8_t b[36]; VkSamplerCreateInfo ci; uint32_t fl;
      static const struct { uint32_t mn, mg; int unknown; VkFilter emin, emag; } F[] = {
          { 0, 0, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST }, { 1, 0, 0, VK_FILTER_LINEAR, VK_FILTER_NEAREST }, { 0, 1, 0, VK_FILTER_NEAREST, VK_FILTER_LINEAR }, { 1, 1, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR },
          { 2, 0, 1, VK_FILTER_LINEAR, VK_FILTER_NEAREST }, { 0, 2, 1, VK_FILTER_NEAREST, VK_FILTER_LINEAR }, { 3, 3, 1, VK_FILTER_LINEAR, VK_FILTER_LINEAR }, { 2, 2, 1, VK_FILTER_LINEAR, VK_FILTER_LINEAR } };
      for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
          samp_blob(b, 0x84000000u | F[i].mn | F[i].mg << 2); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci);
          CHECK(ci.minFilter == F[i].emin && ci.magFilter == F[i].emag && !!(fl & LAGFX_A64_SAMP_FILTER_UNKNOWN) == F[i].unknown);
      }
      /* the 4 CAPTURED words never raise it */
      static const uint32_t cw[4] = { 0x84000000u, 0x84000005u, 0x04000005u, 0x04000000u };
      for (int i = 0; i < 4; i++) { samp_blob(b, cw[i]); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci); CHECK(!(fl & LAGFX_A64_SAMP_FILTER_UNKNOWN)); }
      /* unnormalized: the unknown min code still ends as one legal LINEAR / LINEAR pair */
      samp_blob(b, 0x00000002u); fl = lagfx_a64_sampler_decode_ex(b, 36, false, 0, false, &ci);
      CHECK((fl & LAGFX_A64_SAMP_UNNORM_APPLIED) && (fl & LAGFX_A64_SAMP_FILTER_UNKNOWN) && ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR);
      lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
      lagfx_sampler_diag_rt_count(false, false, true, 0x84000002u); lagfx_sampler_diag_rt_count(false, false, true, 0x84000002u);
      lagfx_sampler_diag_t d = dg(); CHECK(d.rt_sampler_objs_filter_out_of_range == 2 && g_nwarn == 1); }

    /* ---- 64-word log cap: counting continues, log lines stop, suppressed builds are counted ---- */
    lagfx_sampler_diag_reset(); g_nwarn = g_nlog = 0;
    for (uint32_t k = 0; k < 70; k++) { lagfx_constexpr_sampler_t cs = D(W(5, 1, 1, 1, 1, 0, false, 8) | ((uint64_t)k << 32)); (void)lagfx_sampler_state_from_constexpr_norm(&cs); }
    { lagfx_sampler_diag_t d = dg(); CHECK(d.cxs_state_builds_addr_outside_table == 70 && d.cxs_distinct_words_logged == 64 && d.cxs_words_log_suppressed == 6 && g_nwarn == 64); }

    /* ---- cache-key note: lagfx_sampler_cache_get keys on the post-mapping lagfx_sampler_state_t (see seq() checks above), the bound is LAGFX_SAMPLER_CACHE_MAX ---- */
    CHECK(LAGFX_SAMPLER_CACHE_MAX == 32u);

    if (g_fail) { fprintf(stderr, "%d failure(s) of %d checks\n", g_fail, g_checks); return 1; }
    printf("vulkan sampler mapping unit: ok (%d checks)\n", g_checks);
    return 0;
}
#endif
