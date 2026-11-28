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
    int differs = 0, exact = 1;
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
            if (o != v) exact = 0;
            if (abs(o - v) > 1) differs = 1;
        }
    }
    return differs ? LAGFX_GAMMA_APPLIED : exact ? LAGFX_GAMMA_IDENTITY : LAGFX_GAMMA_NEAR_IDENTITY;
}

void lagfx_gamma_apply8(uint8_t *px, size_t npx, int bgra, const uint8_t tab[3][256]) {
    const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (size_t i = 0; i < npx; i++, px += 4) { px[ri] = tab[0][px[ri]]; px[1] = tab[1][px[1]]; px[bi] = tab[2][px[bi]]; }
}

/* the built tables of the last accepted generation (a frame costs one gen compare, not a 49 KB copy + 768 interpolations) */
static struct { uint32_t gen; int valid; lagfx_gamma_result_t res; uint8_t tab[3][256]; } s_cache;
static int s_cache_lock;
static void cache_lock(void) { while (__atomic_test_and_set(&s_cache_lock, __ATOMIC_ACQUIRE)) { } }
static void cache_unlock(void) { __atomic_clear(&s_cache_lock, __ATOMIC_RELEASE); }

static lagfx_gamma_result_t apply_bytes(uint8_t *px, size_t npx, int bgra, int srgb) {
    const char *e = getenv("LAGFX_GAMMA_APPLY");
    if (!(e && e[0] == '1' && !e[1])) { INC(disabled); return LAGFX_GAMMA_BAD_ARGS; }
    const uint32_t gen = lagfx_gd_select_gen();
    if (!gen) { INC(not_selected); return LAGFX_GAMMA_BAD_ARGS; }   /* no accepted table now (none yet, ambiguous, or the last re-validation failed: the old table is dropped, not kept) */
    uint8_t tab[3][256]; lagfx_gamma_result_t r;
    cache_lock();
    if (s_cache.valid && s_cache.gen == gen) { memcpy(tab, s_cache.tab, sizeof tab); r = s_cache.res; cache_unlock(); INC(cache_hits); }
    else {
        cache_unlock();
        lagfx_gd_select_t sel; static uint8_t copy[3u * 4096u * 4u];
        const size_t n = lagfx_gd_select_get(&sel, copy, sizeof copy);   /* gen may have moved on since the check above: the table copied is the one built, and its gen is what is cached */
        if (sel.state != LAGFX_GD_SEL_ACCEPTED || sel.count < 2u || sel.count > 4096u || sel.nwords != 3u * sel.count || n != (size_t)sel.nwords * 4u) { INC(bad_table); return LAGFX_GAMMA_BAD_TABLE; }
        static float planar[3u * 4096u];
        for (uint32_t i = 0; i < sel.nwords; i++) { uint32_t w = (uint32_t)copy[4u * i] | (uint32_t)copy[4u * i + 1u] << 8 | (uint32_t)copy[4u * i + 2u] << 16 | (uint32_t)copy[4u * i + 3u] << 24; memcpy(&planar[i], &w, 4); }
        r = lagfx_gamma_build(planar, sel.count, tab); INC(table_builds);
        cache_lock(); s_cache.gen = sel.gen; s_cache.valid = 1; s_cache.res = r; memcpy(s_cache.tab, tab, sizeof tab); cache_unlock();
    }
    if (r == LAGFX_GAMMA_APPLIED) { lagfx_gamma_apply8(px, npx, bgra, tab); INC(applied); if (srgb) INC(applied_srgb); }
    else if (r == LAGFX_GAMMA_IDENTITY) INC(identity_exact);
    else if (r == LAGFX_GAMMA_NEAR_IDENTITY) INC(identity_near);
    else INC(bad_table);
    return r;
}

lagfx_gamma_result_t lagfx_gamma_apply_present(uint8_t *px, size_t npx, int bgra) { return apply_bytes(px, npx, bgra, 0); }

lagfx_gamma_result_t lagfx_gamma_apply_present_fmt(uint8_t *px, size_t npx, int vkfmt) {
    int bgra, srgb;
    switch (vkfmt) { case 37: bgra = 0, srgb = 0; break; case 43: bgra = 0, srgb = 1; break; case 44: bgra = 1, srgb = 0; break; case 50: bgra = 1, srgb = 1; break; default: INC(bad_format); return LAGFX_GAMMA_BAD_ARGS; }
    return apply_bytes(px, npx, bgra, srgb);
}

void lagfx_gamma_stats_get(lagfx_gamma_stats_t *s) {
    if (!s) return;
    const uint64_t *src = (const uint64_t *)(const void *)&s_st; uint64_t *dst = (uint64_t *)(void *)s;
    for (size_t i = 0; i < sizeof *s / sizeof(uint64_t); i++) dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);   /* every counter is a u64: read each atomically */
}
void lagfx_gamma_test_reset(void) { memset(&s_st, 0, sizeof s_st); cache_lock(); s_cache.valid = 0; cache_unlock(); }
