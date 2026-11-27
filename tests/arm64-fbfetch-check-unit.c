/* Pure regression test for the framebuffer-fetch check (lagfx_a64_fbfetch_check): the colour attachment of an input attachment is its InputAttachmentIndex (0..3), never
 * its binding number (the translator takes the binding from the shared allocator: it follows the resources, the default sampler and the constexpr samplers, and no
 * number is reserved for it). The first versions compared the BINDING against the window 24..27 (then 40..43) and so rejected - or, with enough resources, collided
 * with - real pipelines. SPDX-License-Identifier: MIT */
#include "protocol/arm64_pipeline.h"
#include "air2spv/spv_reflect.h"
#include <stdio.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    for (uint32_t n = 0; n < 4; n++) CHECK(lagfx_a64_fbfetch_check(1, true, n, 5) == LAGFX_A64_FBF_OK);
    CHECK(lagfx_a64_fbfetch_check(1, true, 4, 5) == LAGFX_A64_FBF_BINDING_RANGE);                 /* only attachments 0..3 are fed */
    CHECK(lagfx_a64_fbfetch_check(1, true, 40, 5) == LAGFX_A64_FBF_BINDING_RANGE);                /* a binding number is not an attachment index */
    CHECK(lagfx_a64_fbfetch_check(1, true, LAGFX_SPV_NO_ROLE, 5) == LAGFX_A64_FBF_BINDING_RANGE);  /* a module without InputAttachmentIndex is refused, not guessed */
    CHECK(lagfx_a64_fbfetch_check(0, true, 0, 5) == LAGFX_A64_FBF_VERTEX_STAGE);
    CHECK(lagfx_a64_fbfetch_check(1, false, 0, 5) == LAGFX_A64_FBF_NO_LOCAL_READ);
    CHECK(lagfx_a64_fbfetch_check(1, true, 0, LAGFX_A64_MAX_BINDINGS - 1u) == LAGFX_A64_FBF_OK);
    CHECK(lagfx_a64_fbfetch_check(1, true, 0, LAGFX_A64_MAX_BINDINGS) == LAGFX_A64_FBF_TOO_MANY);
    CHECK(LAGFX_A64_MAX_BINDINGS >= 2u * LAGFX_A64_MAX_REFLECT * 3u / 4u);
    /* the fragment base follows the vertex stage: never below LAGFX_A64_FRAG_BINDING_BASE, never inside the vertex range */
    CHECK(lagfx_a64_frag_base(NULL, 0) == LAGFX_A64_FRAG_BINDING_BASE);
    printf("fbfetch check OK\n"); return 0;
}
