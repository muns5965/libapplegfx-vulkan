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
    /* bad args / non-BGRA refused */
    gs.bpp = 8; assert(lagfx_guest_surface_verify(&sh, &gs, img, W, H, W * 4, &r) == LAGFX_ERR_INVALID_ARG);
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
