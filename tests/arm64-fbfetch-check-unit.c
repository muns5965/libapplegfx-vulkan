/* Pure regression test for the framebuffer-fetch binding check (lagfx_a64_fbfetch_check): fragment modules are offset by LAGFX_A64_FRAG_BINDING_BASE before
 * reflection, so colour attachment n is at 40 + n; the first version compared against the unshifted 24..27 and rejected every real pipeline. SPDX-License-Identifier: MIT */
#include "protocol/arm64_pipeline.h"
#include <stdio.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    uint32_t lo = LAGFX_A64_FRAG_BINDING_BASE + LAGFX_A64_FBF_BINDING_BASE;
    CHECK(lo == 40u);
    for (uint32_t n = 0; n < 4; n++) CHECK(lagfx_a64_fbfetch_check(1, true, lo + n, 5) == LAGFX_A64_FBF_OK);
    CHECK(lagfx_a64_fbfetch_check(1, true, 24, 5) == LAGFX_A64_FBF_BINDING_RANGE);      /* the unshifted number is no longer accepted */
    CHECK(lagfx_a64_fbfetch_check(1, true, lo + 4, 5) == LAGFX_A64_FBF_BINDING_RANGE);
    CHECK(lagfx_a64_fbfetch_check(0, true, lo, 5) == LAGFX_A64_FBF_VERTEX_STAGE);
    CHECK(lagfx_a64_fbfetch_check(1, false, lo, 5) == LAGFX_A64_FBF_NO_LOCAL_READ);
    CHECK(lagfx_a64_fbfetch_check(1, true, lo, LAGFX_A64_MAX_BINDINGS - 1u) == LAGFX_A64_FBF_OK);
    CHECK(lagfx_a64_fbfetch_check(1, true, lo, LAGFX_A64_MAX_BINDINGS) == LAGFX_A64_FBF_TOO_MANY);
    CHECK(LAGFX_A64_MAX_BINDINGS >= 2u * LAGFX_A64_MAX_REFLECT * 3u / 4u);
    printf("fbfetch check OK\n"); return 0;
}
