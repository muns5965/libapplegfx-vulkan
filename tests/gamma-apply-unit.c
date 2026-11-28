/* SPDX-License-Identifier: MIT */
/* Display gamma LUT application (gamma_apply.c): table building, channel order, alpha, identity / bad tables, the env gate (only "1"), and the end-to-end path through the checksum
 * selection (gamma_diag). Host test, no Vulkan device. Nothing here proves a visible effect on a device: no non-identity table has ever been captured from a guest. */
#include "vulkan/gamma_apply.h"
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include "protocol/gamma_diag.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CNT 257u
static float g_tab[3 * CNT];
static void fill(int plane_kind[3]) {   /* 0 identity, 1 x^2, 2 inverted */
    for (int c = 0; c < 3; c++) for (uint32_t i = 0; i < CNT; i++) { float x = (float)i / (float)(CNT - 1u); g_tab[c * CNT + i] = plane_kind[c] == 0 ? x : plane_kind[c] == 1 ? x * x : 1.0f - x; }
}
static int near(int a, int b, int tol) { return abs(a - b) <= tol; }

/* selection plumbing (the same fake reader approach as gamma-select-unit.c) */
static int fake(lagfx_protocol_t *p, uint32_t task, uint64_t va, uint32_t len, uint8_t *out) { (void)p; (void)va; if (task != 0) return LAGFX_GD_RD_UNMAPPED; memset(out, 0, len); memcpy(out, g_tab, sizeof g_tab); return LAGFX_GD_RD_OK; }
static bool owner_fn(lagfx_protocol_t *p, uint64_t m, uint32_t *t) { (void)p; (void)m; (void)t; return false; }
static void w32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static uint32_t tab_sum(void) { uint32_t sum = 0; for (uint32_t i = 0; i < 3 * CNT; i++) { uint32_t w; memcpy(&w, &g_tab[i], 4); sum += w; } return sum; }
static void op8_table(lagfx_protocol_t *p, uint32_t sum) { uint8_t pl[40]; memset(pl, 0, sizeof pl); w32(pl, 8, 7); w32(pl, 16, 0x118000u); w32(pl, 24, 0x4000u); w32(pl, 32, CNT); w32(pl, 36, sum); lagfx_gd_note_op8(p, 4, pl, sizeof pl, 0, NULL, owner_fn); }
static lagfx_protocol_t *select_table(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p); assert(p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p); assert(t); t->id = 0; t->live = true;
    p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 0;
    lagfx_gd_test_reset(); lagfx_gd_set_reader(fake);
    uint32_t sum = 0; for (uint32_t i = 0; i < 3 * CNT; i++) { uint32_t w; memcpy(&w, &g_tab[i], 4); sum += w; }
    uint8_t pl[40]; memset(pl, 0, sizeof pl);
    uint8_t z[40]; memset(z, 0, sizeof z); for (int i = 0; i < 4; i++) lagfx_gd_note_op8(p, 4, z, sizeof z, 0, NULL, owner_fn);   /* swaps 1..4: no table */
    w32(pl, 8, 7); w32(pl, 16, 0x118000u); w32(pl, 24, 0x4000u); w32(pl, 32, CNT); w32(pl, 36, sum);
    lagfx_gd_note_op8(p, 4, pl, sizeof pl, 0, NULL, owner_fn);
    return p;
}

int main(void) {
    uint8_t tab[3][256];
    /* 1. identity (what every capture so far holds): IDENTITY, nothing to apply */
    { int k[3] = { 0, 0, 0 }; fill(k); assert(lagfx_gamma_build(g_tab, CNT, tab) == LAGFX_GAMMA_IDENTITY);   /* EXACT identity */ for (int v = 0; v < 256; v++) assert(near(tab[0][v], v, 1) && near(tab[1][v], v, 1) && near(tab[2][v], v, 1)); }
    /* 2. per-plane curves: plane 0 identity (R), plane 1 x^2 (G), plane 2 inverted (B); RGBA and BGRA byte orders, alpha untouched */
    { int k[3] = { 0, 1, 2 }; fill(k); assert(lagfx_gamma_build(g_tab, CNT, tab) == LAGFX_GAMMA_APPLIED);
      assert(tab[1][128] == (uint8_t)(0.5019608 * 0.5019608 * 255.0 + 0.5) && near(tab[2][0], 255, 0) && near(tab[2][255], 0, 0) && near(tab[0][77], 77, 1));
      uint8_t rgba[8] = { 200, 128, 10, 77, 50, 255, 255, 5 }, bgra[8] = { 10, 128, 200, 77, 255, 255, 50, 5 };   /* same two pixels: (R,G,B,A) = (200,128,10,77) and (50,255,255,5) */
      lagfx_gamma_apply8(rgba, 2, 0, (const uint8_t (*)[256])tab); lagfx_gamma_apply8(bgra, 2, 1, (const uint8_t (*)[256])tab);
      assert(rgba[0] == tab[0][200] && rgba[1] == tab[1][128] && rgba[2] == tab[2][10] && rgba[3] == 77 && rgba[4] == tab[0][50] && rgba[5] == tab[1][255] && rgba[6] == tab[2][255] && rgba[7] == 5);
      assert(bgra[2] == rgba[0] && bgra[1] == rgba[1] && bgra[0] == rgba[2] && bgra[3] == 77 && bgra[6] == rgba[4] && bgra[5] == rgba[5] && bgra[4] == rgba[6] && bgra[7] == 5);   /* plane 0 is R in both orders */ }
    /* 3. bad tables: non-finite, count out of range, null */
    { int k[3] = { 0, 0, 0 }; fill(k); g_tab[5] = NAN; assert(lagfx_gamma_build(g_tab, CNT, tab) == LAGFX_GAMMA_BAD_TABLE); fill(k); assert(lagfx_gamma_build(g_tab, 1, tab) == LAGFX_GAMMA_BAD_TABLE && lagfx_gamma_build(g_tab, 5000, tab) == LAGFX_GAMMA_BAD_TABLE && lagfx_gamma_build(NULL, CNT, tab) == LAGFX_GAMMA_BAD_ARGS); }
    /* 4. a 2-entry table is a linear ramp between its ends; out-of-range values clamp */
    { float t2[6] = { 0.f, 1.f, 0.f, 1.f, 0.25f, 2.0f }; assert(lagfx_gamma_build(t2, 2, tab) == LAGFX_GAMMA_APPLIED); assert(near(tab[0][128], 128, 1) && near(tab[2][0], 64, 1) && tab[2][255] == 255); }
    /* 3b. near identity: one level off somewhere is NOT the exact identity, and is not applied either (counted apart) */
    { int k[3] = { 0, 0, 0 }; fill(k); g_tab[100] += 1.0f / 255.0f; assert(lagfx_gamma_build(g_tab, CNT, tab) == LAGFX_GAMMA_NEAR_IDENTITY); g_tab[100] += 3.0f / 255.0f; assert(lagfx_gamma_build(g_tab, CNT, tab) == LAGFX_GAMMA_APPLIED); }
    puts("PASS gamma table build: identity, per-plane curves, RGBA / BGRA order, alpha untouched, bad tables, 2-entry ramp, clamping");

    /* 5. end to end through the checksum selection and the env gate */
    { int k[3] = { 0, 1, 2 }; fill(k); lagfx_protocol_t *p = select_table(); lagfx_gd_select_t sel; assert(lagfx_gd_select_get(&sel, NULL, 0) == 0 && sel.state == LAGFX_GD_SEL_ACCEPTED);
      lagfx_gamma_stats_t st; uint8_t px[4] = { 40, 128, 40, 255 }, orig[4]; memcpy(orig, px, 4);
      unsetenv("LAGFX_GAMMA_APPLY"); lagfx_gamma_test_reset();
      lagfx_gamma_apply_present(px, 1, 0); assert(!memcmp(px, orig, 4));                       /* default OFF */
      setenv("LAGFX_GAMMA_APPLY", "true", 1); lagfx_gamma_apply_present(px, 1, 0); assert(!memcmp(px, orig, 4));   /* only "1" turns it on */
      setenv("LAGFX_GAMMA_APPLY", "10", 1); lagfx_gamma_apply_present(px, 1, 0); assert(!memcmp(px, orig, 4));
      lagfx_gamma_stats_get(&st); assert(st.disabled == 3 && st.applied == 0);
      setenv("LAGFX_GAMMA_APPLY", "1", 1); assert(lagfx_gamma_apply_present(px, 1, 0) == LAGFX_GAMMA_APPLIED);
      assert(px[0] == tab[0][40] || near(px[0], 40, 1)); assert(near(px[1], (int)(0.5019608 * 0.5019608 * 255.0 + 0.5), 1) && near(px[2], 255 - 40, 1) && px[3] == 255);
      lagfx_gamma_stats_get(&st); assert(st.applied == 1 && st.table_builds == 1 && st.cache_hits == 0);
      memcpy(px, orig, 4); lagfx_gamma_apply_present(px, 1, 0); lagfx_gamma_stats_get(&st); assert(st.applied == 2 && st.table_builds == 1 && st.cache_hits == 1);   /* same generation: no rebuild */
      /* a CHANGED table after acceptance is validated again and replaces the old one (cache rebuilt) */
      { int k2[3] = { 2, 2, 2 }; fill(k2); op8_table(p, tab_sum()); memcpy(px, orig, 4); lagfx_gamma_apply_present(px, 1, 0);
        lagfx_gamma_stats_get(&st); assert(st.table_builds == 2 && px[0] == 255 - 40 && px[1] == 255 - 128 && px[2] == 255 - 40); }
      /* a zero descriptor never clears it */
      { uint8_t z[40]; memset(z, 0, sizeof z); lagfx_gd_note_op8(p, 4, z, sizeof z, 0, NULL, owner_fn); memcpy(px, orig, 4); lagfx_gamma_stats_get(&st); const uint64_t b = st.applied; lagfx_gamma_apply_present(px, 1, 0); lagfx_gamma_stats_get(&st); assert(st.applied == b + 1); }
      /* a changed table that fails validation (wrong checksum) DROPS the old one: nothing is applied rather than a stale table */
      { int k3[3] = { 1, 1, 1 }; fill(k3); op8_table(p, tab_sum() ^ 0x1u); memcpy(px, orig, 4); lagfx_gamma_stats_get(&st); const uint64_t ns = st.not_selected; lagfx_gamma_apply_present(px, 1, 0); assert(!memcmp(px, orig, 4)); lagfx_gamma_stats_get(&st); assert(st.not_selected == ns + 1); }
      /* the format API: BGRA / RGBA, SRGB counted, a 16-bit format untouched */
      { int k4[3] = { 2, 2, 2 }; fill(k4); lagfx_gd_test_reset(); lagfx_gd_set_reader(fake); free(p); p = select_table(); lagfx_gamma_test_reset();
        uint8_t a[4] = { 10, 20, 30, 99 }, b[4] = { 10, 20, 30, 99 }, c[4] = { 10, 20, 30, 99 }, d[4] = { 10, 20, 30, 99 };
        assert(lagfx_gamma_apply_present_fmt(a, 1, 44) == LAGFX_GAMMA_APPLIED && a[0] == 245 && a[1] == 235 && a[2] == 225 && a[3] == 99);
        assert(lagfx_gamma_apply_present_fmt(b, 1, 37) == LAGFX_GAMMA_APPLIED && b[0] == 245 && b[3] == 99);
        assert(lagfx_gamma_apply_present_fmt(c, 1, 50) == LAGFX_GAMMA_APPLIED);
        assert(lagfx_gamma_apply_present_fmt(d, 1, 97 /* R16G16B16A16_SFLOAT */) == LAGFX_GAMMA_BAD_ARGS && !memcmp(d, (uint8_t[4]){ 10, 20, 30, 99 }, 4));
        lagfx_gamma_stats_get(&st); assert(st.applied == 3 && st.applied_srgb == 1 && st.bad_format == 1); }
      /* no accepted selection (reset): counted, pixels untouched */
      lagfx_gd_test_reset(); memcpy(px, orig, 4); lagfx_gamma_test_reset();
      lagfx_gamma_apply_present(px, 1, 0); assert(!memcmp(px, orig, 4)); lagfx_gamma_stats_get(&st); assert(st.not_selected == 1 && st.applied == 0);
      /* lagfx_gd_reset (what lagfx_protocol_free calls) drops the selection too */
      unsetenv("LAGFX_GAMMA_APPLY"); lagfx_gd_set_reader(NULL); free(p); }
    puts("PASS gamma application through the accepted selection: default OFF, only the value 1 enables it, applied once, no selection -> untouched and counted");
    return 0;
}
