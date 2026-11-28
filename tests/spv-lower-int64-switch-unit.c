/* SPDX-License-Identifier: MIT */
/*
 * Int64 lowering: OpSwitch literals are not operands. A real S8 guest shader was refused with "unsupported 64-bit integer operation (SPIR-V opcode 251)": the translator's
 * block-dispatch switch is on a 32-bit index and its case literals are small integers, and one of them equalled the id of a 64-bit constant, which the operand scan took for a
 * 64-bit operand. This module (assembled from the spvasm below, vulkan1.2, spirv-val clean) has exactly that: id 5 is an i64 constant and `case 5` is a literal. No device needed.
 *
 *   OpCapability Shader / Int64 ; %ulong_7 = OpConstant %ulong 7 (id 5) ; %x = OpIAdd %ulong %ulong_7 %ulong_7 ; OpSelectionMerge ; OpSwitch %uint_1 %merge 5 %a 1 %b
 */
#include "air2spv/spv_lower_int64.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t K[] = { 0x07230203,0x00010500,0x00070000,0x0000000e,0x00000000,0x00020011,0x00000001,0x00020011,0x0000000b,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000005,0x00000001,0x6e69616d,0x00000000,0x00060010,0x00000001,0x00000011,0x00000001,0x00000001,0x00000001,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,0x00000004,0x00000040,0x00000000,0x0005002b,0x00000004,0x00000005,0x00000007,0x00000000,0x00040015,0x00000006,0x00000020,0x00000000,0x0004002b,0x00000006,0x00000007,0x00000000,0x0004002b,0x00000006,0x00000008,0x00000001,0x00050036,0x00000002,0x00000001,0x00000000,0x00000003,0x000200f8,0x00000009,0x00050080,0x00000004,0x0000000a,0x00000005,0x00000005,0x000300f7,0x0000000b,0x00000000,0x000700fb,0x00000008,0x0000000b,0x00000005,0x0000000c,0x00000001,0x0000000d,0x000200f8,0x0000000c,0x000200f9,0x0000000b,0x000200f8,0x0000000d,0x000200f9,0x0000000b,0x000200f8,0x0000000b,0x000100fd,0x00010038 };

int main(void) {
    char why[160]; uint8_t *out = NULL; size_t on = 0;
    int rc = lagfx_spv_lower_int64((const uint8_t *)(const void *)K, sizeof K, &out, &on, why, sizeof why);
    if (rc != 1) { printf("FAIL: lowering rc=%d (%s)\n", rc, why); return 1; }
    /* the OpSwitch survives byte for byte: opcode 251, 7 words: selector, default, (5, label), (1, label) */
    const uint32_t *w = (const uint32_t *)(const void *)out; int found = 0;
    for (size_t i = 5; i < on / 4;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc) break;
        if (op == 251u) { found++; assert(wc == 7u && w[i + 3] == 5u && w[i + 5] == 1u); } i += wc; }
    assert(found == 1);
    free(out); out = NULL;
    /* a genuine 64-bit selector (the i64 constant, id 5) is still refused, not mis-lowered */
    uint32_t *K2 = malloc(sizeof K); memcpy(K2, K, sizeof K);
    for (size_t i = 5; i < sizeof K / 4;) { uint32_t wc = K2[i] >> 16, op = K2[i] & 0xffffu; if (op == 251u) K2[i + 1] = 5u; i += wc; }
    rc = lagfx_spv_lower_int64((const uint8_t *)(const void *)K2, sizeof K, &out, &on, why, sizeof why);
    assert(rc == -1 && !out);
    free(K2);
    puts("PASS: OpSwitch case literals / labels are not 64-bit operands (a literal equal to an i64 constant id no longer refuses the module); a real 64-bit selector is still refused");
    return 0;
}
