/* Tile kernel subgroup plan (pure decision, no Vulkan device): which logical simdgroup width a tile kernel is translated for and whether the dispatch pins the pipeline.
 * The S8 (Adreno 730) case is subgroup size 64 in the range 64..128 with VK_EXT_subgroup_size_control on compute: before this, it was skipped (no_sg32); now it is emulated
 * as logical 32-lane groups inside a subgroup PINNED to 64. Host decision table only: that Adreno honours requiredSubgroupSize = 64 is NOT verified here.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vulkan/instance.h"
#include "protocol/arm64_tile.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static struct lagfx_vk_state mk(uint32_t size, uint32_t mn, uint32_t mx, bool ctl) {
    struct lagfx_vk_state s; memset(&s, 0, sizeof s);
    s.sg_size = size; s.sg_min = mn; s.sg_max = mx; s.sg_control = ctl;
    return s;
}
int main(void) {
    unsetenv("LAGFX_TILE_SIMD_EMULATE");
    struct lagfx_vk_state s;
    s = mk(32, 32, 32, false);  CHECK(lagfx_a64_tile_simd_emul_width(&s) == 0, "native 32: no emulation");
    s = mk(32, 8, 64, true);    CHECK(lagfx_a64_tile_simd_emul_width(&s) == 0, "range containing 32 and pinnable: pin 32, no emulation");
    CHECK(lagfx_vk_wide_pin_subgroup(&s) == 0, "range with min <= 32 is not a wide pin");
    s = mk(64, 64, 64, false);  CHECK(lagfx_a64_tile_simd_emul_width(&s) == 32, "fixed 64: emulate 32 (unchanged)");
    s = mk(64, 64, 128, true);  CHECK(lagfx_a64_tile_simd_emul_width(&s) == 32 && lagfx_vk_wide_pin_subgroup(&s) == 64, "Adreno 64..128 with control: emulate 32, pin 64");
    s = mk(64, 64, 128, false); CHECK(lagfx_a64_tile_simd_emul_width(&s) == 0 && lagfx_vk_wide_pin_subgroup(&s) == 0, "Adreno range WITHOUT control: not pinnable, still skipped (0)");
    s = mk(48, 48, 96, true);   CHECK(lagfx_vk_wide_pin_subgroup(&s) == 0 && lagfx_a64_tile_simd_emul_width(&s) == 0, "min 48 is not a power-of-two multiple of 32: no");
    s = mk(128, 128, 128, false); CHECK(lagfx_a64_tile_simd_emul_width(&s) == 32, "fixed 128: emulate 32");
    s = mk(16, 16, 16, false);  CHECK(lagfx_a64_tile_simd_emul_width(&s) == 0, "narrow subgroup: no emulation (skipped by the dispatch)");
    /* test hook still forces a width on any device and never exceeds the subgroup size */
    setenv("LAGFX_TILE_SIMD_EMULATE", "16", 1); s = mk(32, 32, 32, false);
    CHECK(lagfx_a64_tile_simd_emul_width(&s) == 16, "hook W=16 on 32");
    setenv("LAGFX_TILE_SIMD_EMULATE", "64", 1); CHECK(lagfx_a64_tile_simd_emul_width(&s) == 0, "hook wider than the subgroup: refused");
    unsetenv("LAGFX_TILE_SIMD_EMULATE");
    if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    printf("PASS: tile subgroup plan decision table (Adreno 64..128 with control now emulated + pinned)\n");
    return 0;
}
