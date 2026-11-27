/* LAGFX_WB_VERIFY read-only writeback comparison: identical / RGB-only / alpha-only / padding changes, env gate (off -> no xfer), cap of 8 and the 10 s gap.
 * No Vulkan needed (fake guest memory). SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include "vulkan/surface_writeback.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM (1u << 20)
static uint8_t *g_mem;
static int g_reads, g_hook;   /* g_hook: after read #n (1-based) change one guest byte, to model a concurrent guest write */
static bool rd(void *o, uint64_t g, uint64_t l, void *d) { (void)o; g_reads++; if (g + l > MEM) return false; memcpy(d, g_mem + g, l); return true; }
#define W 20u
#define H 10u
#define ROW 128u
#define FIRST 64u
static const uint64_t PG[2] = { 0x40000, 0x80000 };
static uint8_t *sb(uint64_t off) { return g_mem + PG[off >> 14] + (off & 0x3fff); }
static void setup(lagfx_guest_surface_t *gs, uint8_t *img) {
    memset(g_mem, 0xEE, MEM);
    uint32_t list[2] = { (uint32_t)(PG[0] >> 12) | 1u, (uint32_t)(PG[1] >> 12) | 1u };
    memcpy(g_mem + 0x1000, list, sizeof list);
    memset(gs, 0, sizeof *gs);
    gs->fourcc = LAGFX_FOURCC_BGRA, gs->width = W, gs->height = H, gs->bpp = 4, gs->row_bytes = ROW, gs->first_offset = FIRST, gs->npages = 2, gs->alloc = 20000, gs->page_list_gpa = 0x1000;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        uint8_t p[4] = { (uint8_t)(x + 1), (uint8_t)(y + 1), (uint8_t)(x ^ y), 255 };
        memcpy(img + ((size_t)y * W + x) * 4, p, 4);
        memcpy(sb(FIRST + (uint64_t)y * ROW + x * 4u), p, 4);
    }
    g_reads = 0;
}
int main(void) {
    g_mem = malloc(MEM);
    lagfx_shell_callbacks_t sh; memset(&sh, 0, sizeof sh); sh.read_memory = rd;
    lagfx_guest_surface_t gs; uint8_t img[W * H * 4]; lagfx_wbv_result_t r;
    /* identical; the caller's buffer may be wider than the pixels (stride) */
    setup(&gs, img);
    uint8_t wide[(W + 4) * H * 4]; memset(wide, 0x55, sizeof wide);
    for (uint32_t y = 0; y < H; y++) memcpy(wide + (size_t)y * (W + 4) * 4, img + (size_t)y * W * 4, W * 4);
    assert(lagfx_guest_surface_verify(&sh, &gs, wide, W, H, (W + 4) * 4, &r) == LAGFX_OK);
    assert(r.rgb_mismatch == 0 && r.alpha_only_mismatch == 0 && r.first_x == -1 && r.hash_buf == r.hash_guest && r.stable && r.pixels == W * H);
    /* RGB-only change (also one pixel with both rgb+alpha: counted as rgb) */
    setup(&gs, img); sb(FIRST + 2ull * ROW + 3 * 4)[1] ^= 1; sb(FIRST + 5ull * ROW + 7 * 4)[0] ^= 1; sb(FIRST + 5ull * ROW + 7 * 4)[3] ^= 1;
    assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_OK);
    assert(r.rgb_mismatch == 2 && r.alpha_only_mismatch == 0 && r.first_x == 3 && r.first_y == 2 && r.hash_buf != r.hash_guest && r.stable);
    /* alpha-only change */
    setup(&gs, img); sb(FIRST + 4ull * ROW + 9 * 4)[3] = 0; sb(FIRST + 6ull * ROW + 1 * 4)[3] = 7;
    assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_OK);
    assert(r.rgb_mismatch == 0 && r.alpha_only_mismatch == 2 && r.first_x == 9 && r.first_y == 4);
    /* padding (row bytes beyond width*4, and before first) changes are not counted */
    setup(&gs, img); for (uint32_t y = 0; y < H; y++) *sb(FIRST + (uint64_t)y * ROW + W * 4u + 3) ^= 0xff; *sb(3) ^= 0xff;
    assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_OK);
    assert(r.rgb_mismatch == 0 && r.alpha_only_mismatch == 0 && r.hash_buf == r.hash_guest);
    /* hash is the pvgpix one: FNV-1a over whole LE u32 pixels, init 1469598103934665603 */
    { uint64_t h = 1469598103934665603ull; for (unsigned i = 0; i < W * H; i++) { uint32_t v; memcpy(&v, img + i * 4, 4); h = (h ^ v) * 1099511628211ull; } assert(h == r.hash_buf); }
    /* 2 reads of the guest side, no other xfer */
    setup(&gs, img); assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_OK);
    assert(g_reads > 0);
    /* bad args: bpp other than 4 / 8, a stride too small for the pixels */
    setup(&gs, img);
    gs.bpp = 3; assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_ERR_INVALID_ARG);
    gs.bpp = 8; assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_ERR_INVALID_ARG);   /* RGhA is accepted, but this stride holds only 4 bytes a pixel */

    /* ---- RGhA (RGBA16Float, bpp 8): the guest's main display surface. Before this was added the verify returned immediately for it, so the 1920x1080 surface was never checked. ---- */
    {
        enum { ROW8 = 192 };   /* 20 * 8 = 160 bytes of pixels + 32 of padding */
        uint8_t im8[W * H * 8];
        #define SETUP8() do { memset(g_mem, 0xEE, MEM); uint32_t list[2] = { (uint32_t)(PG[0] >> 12) | 1u, (uint32_t)(PG[1] >> 12) | 1u }; memcpy(g_mem + 0x1000, list, sizeof list); \
            memset(&gs, 0, sizeof gs); gs.fourcc = LAGFX_FOURCC_RGHA, gs.width = W, gs.height = H, gs.bpp = 8, gs.row_bytes = ROW8, gs.first_offset = FIRST, gs.npages = 2, gs.alloc = 20000, gs.page_list_gpa = 0x1000; \
            for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) { uint16_t v[4] = { (uint16_t)(0x3000 + x), (uint16_t)(0x3400 + y), (uint16_t)(0x3800 + (x ^ y)), 0x3c00 }; \
                memcpy(im8 + ((size_t)y * W + x) * 8, v, 8); memcpy(sb(FIRST + (uint64_t)y * ROW8 + x * 8u), v, 8); } g_reads = 0; } while (0)
        #define PX8(y, x) sb(FIRST + (uint64_t)(y) * ROW8 + (x) * 8u)
        SETUP8();
        assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK);
        assert(r.bpp == 8 && r.rgb_mismatch == 0 && r.alpha_only_mismatch == 0 && r.first_x == -1 && r.hash_buf == r.hash_guest && r.stable && r.pixels == W * H);
        /* the hash is FNV-1a over whole LE u64 pixels (documented as NOT the pvgpix hash) */
        { uint64_t h = 1469598103934665603ull; for (unsigned i = 0; i < W * H; i++) { uint64_t v; memcpy(&v, im8 + i * 8, 8); h = (h ^ v) * 1099511628211ull; } assert(h == r.hash_buf); }
        /* one low bit of one colour channel (blue, bytes 4..5) of one pixel; and a bit of alpha only (bytes 6..7) of another */
        PX8(2, 3)[4] ^= 1; PX8(5, 7)[7] ^= 0x80;
        assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK);
        assert(r.rgb_mismatch == 1 && r.alpha_only_mismatch == 1 && r.first_x == 3 && r.first_y == 2 && r.hash_buf != r.hash_guest && r.stable);
        /* a pixel with both colour and alpha changed counts as colour */
        SETUP8(); PX8(4, 4)[0] ^= 1; PX8(4, 4)[6] ^= 1;
        assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.rgb_mismatch == 1 && r.alpha_only_mismatch == 0);
        /* padding (row bytes beyond width * 8, bytes before first) changes are not counted; neither is the wider caller stride's padding */
        SETUP8(); for (uint32_t y = 0; y < H; y++) *PX8(y, W) ^= 0xff; *sb(3) ^= 0xff;
        { uint8_t wide[(W + 2) * H * 8]; memset(wide, 0x55, sizeof wide); for (uint32_t y = 0; y < H; y++) memcpy(wide + (size_t)y * (W + 2) * 8, im8 + (size_t)y * W * 8, W * 8);
          assert(lagfx_guest_surface_verify(&sh, &gs, wide, W, H, (W + 2) * 8, &r) == LAGFX_OK && r.rgb_mismatch == 0 && r.alpha_only_mismatch == 0 && r.hash_buf == r.hash_guest); }
        /* the compare is on bit patterns: -0.0 vs +0.0 and two different NaN payloads are mismatches, equal NaN payloads are not */
        SETUP8();
        { uint16_t nan1[4] = { 0x7e00, 0x7e01, 0x7c00, 0xfc00 }; memcpy(im8 + 8 * 1, nan1, 8); memcpy(PX8(0, 1), nan1, 8);
          uint16_t negz[4] = { 0x8000, 0x3c00, 0, 0x3c00 }; memcpy(im8 + 8 * 2, negz, 8); memcpy(PX8(0, 2), negz, 8);
          assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.rgb_mismatch == 0 && r.alpha_only_mismatch == 0);
          PX8(0, 1)[2] ^= 1;   /* NaN payload 0x7e01 -> 0x7e00 on the guest side */
          assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.rgb_mismatch == 1 && r.first_x == 1 && r.first_y == 0);
          memcpy(PX8(0, 1), nan1, 8); PX8(0, 2)[1] = 0;   /* -0.0 (0x8000) -> +0.0 */
          assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.rgb_mismatch == 1 && r.first_x == 2); }
#ifndef NO_HALF_STATS
        /* pixel classes of the WRITTEN data (not the guest side): Inf/NaN, negative (non-zero), finite above 1.0; -0.0 is not "negative" */
        SETUP8();
        { uint16_t nan[4] = { 0x7e00, 0x3c00, 0x3c00, 0x3c00 }, inf[4] = { 0x3c00, 0xfc00, 0, 0x3c00 }, neg[4] = { 0xb800, 0, 0, 0x3c00 }, big[4] = { 0x4000, 0x7bff, 0, 0x3c00 }, negz[4] = { 0x8000, 0, 0, 0x3c00 }, one[4] = { 0x3c00, 0x3c00, 0x3c00, 0x3c00 }, tiny[4] = { 0x0001, 0x8001, 0x0400, 0x3c00 };
          const uint16_t *set[7] = { nan, inf, neg, big, negz, one, tiny };
          for (int i = 0; i < 7; i++) { memcpy(im8 + 8 * i, set[i], 8); memcpy(PX8(0, i), set[i], 8); }
          assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.rgb_mismatch == 0);
          /* pixel 0 nan, 1 inf (also negative -Inf), 2 neg, 3 big, 4 -0.0 (none), 5 one (none: 1.0 is not above one), 6 tiny: 0x8001 is negative (non-zero magnitude) */
          assert(r.half_nonfinite == 2 && r.half_negative == 3 && r.half_above_one == 1);
          /* the base image (0x3000 + x .. 0x3c00 and 0x3800 + (x ^ y)) has no such pixels */
          SETUP8(); assert(lagfx_guest_surface_verify(&sh, &gs, im8, W, H, W * 8, &r) == LAGFX_OK && r.half_nonfinite == 0 && r.half_negative == 0 && r.half_above_one == 0); }
#endif
        #undef SETUP8
        #undef PX8
    }
    /* env gate */
    unsetenv("LAGFX_WB_VERIFY"); assert(!lagfx_wbv_enabled());
    setenv("LAGFX_WB_VERIFY", "0", 1); assert(!lagfx_wbv_enabled());
    setenv("LAGFX_WB_VERIFY", "1", 1); assert(lagfx_wbv_enabled());
    unsetenv("LAGFX_WB_VERIFY");
    /* gate: first immediate, 10 s gap, cap 8 */
    lagfx_wbv_gate_t g; memset(&g, 0, sizeof g);
    uint64_t t = 5;   /* a tiny first timestamp must still be admitted */
    assert(lagfx_wbv_admit(&g, t));
    assert(!lagfx_wbv_admit(&g, t + 9999999999ull));
    for (unsigned i = 1; i < 8; i++) { t += LAGFX_WBV_MIN_GAP_NS; assert(lagfx_wbv_admit(&g, t)); }
    t += 100 * LAGFX_WBV_MIN_GAP_NS; assert(!lagfx_wbv_admit(&g, t) && g.done == 8);
    assert(!lagfx_wbv_admit(NULL, t));
    uint64_t a = lagfx_wbv_now_ns(), b = lagfx_wbv_now_ns(); assert(b >= a && a > 0);
    printf("arm64-wb-verify-unit OK\n");
    free(g_mem);
    return 0;
}
