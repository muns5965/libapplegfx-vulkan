/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — display gamma LUT application. See gamma_apply.h. */
#include "vulkan/gamma_apply.h"
#include "protocol/gamma_diag.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static lagfx_gamma_stats_t s_st;
#define INC(f) __atomic_add_fetch(&s_st.f, 1u, __ATOMIC_RELAXED)

lagfx_gamma_result_t lagfx_gamma_build(const float *planar, uint32_t count, uint8_t tab[3][256]) {
    if (!planar || !tab) return LAGFX_GAMMA_BAD_ARGS;
    if (count < 2u || count > 4096u) return LAGFX_GAMMA_BAD_TABLE;
    for (size_t i = 0; i < (size_t)count * 3u; i++) if (!isfinite(planar[i])) return LAGFX_GAMMA_BAD_TABLE;
    int differs = 0;
    for (int c = 0; c < 3; c++) {
        const float *p = planar + (size_t)c * count;
        for (int v = 0; v < 256; v++) {
            const double x = (double)v / 255.0 * (double)(count - 1u);
            uint32_t i0 = (uint32_t)x; if (i0 >= count - 1u) i0 = count - 2u;
            const double f = x - (double)i0;
            double y = (double)p[i0] * (1.0 - f) + (double)p[i0 + 1u] * f;
            if (y < 0.0) y = 0.0; else if (y > 1.0) y = 1.0;
            const int o = (int)(y * 255.0 + 0.5);
            tab[c][v] = (uint8_t)o;
            if (abs(o - v) > 1) differs = 1;
        }
    }
    return differs ? LAGFX_GAMMA_APPLIED : LAGFX_GAMMA_IDENTITY;
}

void lagfx_gamma_apply8(uint8_t *px, size_t npx, int bgra, const uint8_t tab[3][256]) {
    const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (size_t i = 0; i < npx; i++, px += 4) { px[ri] = tab[0][px[ri]]; px[1] = tab[1][px[1]]; px[bi] = tab[2][px[bi]]; }
}

lagfx_gamma_result_t lagfx_gamma_apply_present(uint8_t *px, size_t npx, int bgra) {
    const char *e = getenv("LAGFX_GAMMA_APPLY");
    if (!(e && e[0] == '1' && !e[1])) { INC(disabled); return LAGFX_GAMMA_BAD_ARGS; }
    lagfx_gd_select_t sel; static uint8_t copy[3u * 4096u * 4u];   /* serialised by the present path (one display) */
    const size_t n = lagfx_gd_select_get(&sel, copy, sizeof copy);
    if (sel.state != LAGFX_GD_SEL_ACCEPTED || n != (size_t)sel.nwords * 4u || sel.count < 2u) { INC(not_selected); return LAGFX_GAMMA_BAD_ARGS; }
    float planar[3u * 4096u];
    for (uint32_t i = 0; i < sel.nwords && i < 3u * 4096u; i++) { uint32_t w = (uint32_t)copy[4u * i] | (uint32_t)copy[4u * i + 1u] << 8 | (uint32_t)copy[4u * i + 2u] << 16 | (uint32_t)copy[4u * i + 3u] << 24; memcpy(&planar[i], &w, 4); }
    uint8_t tab[3][256];
    const lagfx_gamma_result_t r = lagfx_gamma_build(planar, sel.count, tab);
    if (r == LAGFX_GAMMA_APPLIED) { lagfx_gamma_apply8(px, npx, bgra, tab); INC(applied); }
    else if (r == LAGFX_GAMMA_IDENTITY) INC(identity);
    else INC(bad_table);
    return r;
}

void lagfx_gamma_stats_get(lagfx_gamma_stats_t *s) { if (s) memcpy(s, &s_st, sizeof *s); }
void lagfx_gamma_test_reset(void) { memset(&s_st, 0, sizeof s_st); }
