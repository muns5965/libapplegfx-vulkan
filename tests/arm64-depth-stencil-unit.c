/* ARM64 depth/stencil state object (serialized tag 4): decoder checked with synthetic 40-byte records. The field values are the ones the reference
 * implementation's one-property-at-a-time experiments on a real serializer observed (compare Always = 7 -> depth byte 0x37, Greater = 4 -> 0x34, write on ->
 * 0x3f; ops words 0x1f / 0x107 / 0xa07 / 0x37 / 0x1c7 / 0x407; masks 0x11223344 / 0x55667788 front, 0x99aabbcc / 0xddeeff00 back). No Vulkan needed.
 * SPDX-License-Identifier: MIT */
#include "protocol/arm64_depth_stencil.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void w32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v, b[1] = (uint8_t)(v >> 8), b[2] = (uint8_t)(v >> 16), b[3] = (uint8_t)(v >> 24); }
static void mk(uint8_t b[40], uint32_t ref, uint8_t depth_state, uint32_t fo, uint32_t fr, uint32_t fw, uint32_t bo, uint32_t br, uint32_t bw) {
    memset(b, 0xaa, 40);   /* the bytes the serializer leaves alone are stale guest memory */
    w32(b, 4), w32(b + 4, 40), w32(b + 8, ref);
    b[12] = depth_state;
    w32(b + 16, fo), w32(b + 20, fr), w32(b + 24, fw), w32(b + 28, bo), w32(b + 32, br), w32(b + 36, bw);
}

int main(void) {
    uint8_t b[40];
    lagfx_a64_dss_t d, dflt;
    /* Metal's default state: depth Always, no write, both faces Always / Keep, masks all ones */
    mk(b, 11, 0x37, 7, 0xffffffffu, 0xffffffffu, 7, 0xffffffffu, 0xffffffffu);
    assert(lagfx_a64_dss_decode(b, 40, &d));
    assert(d.ref == 11 && d.depth_compare == LAGFX_MTL_CMP_ALWAYS && !d.depth_write && d.unknown_bits == 3);
    assert(d.front.compare == 7 && d.front.fail_op == 0 && d.front.depth_fail_op == 0 && d.front.pass_op == 0 && d.front.read_mask == 0xffffffffu && d.front.write_mask == 0xffffffffu);
    assert(!lagfx_a64_dss_depth_test(&d) && !lagfx_a64_dss_stencil_test(&d));          /* a no-op state needs no Vulkan test at all */
    lagfx_a64_dss_default(&dflt);
    assert(dflt.depth_compare == 7 && !dflt.depth_write && !lagfx_a64_dss_depth_test(&dflt) && !lagfx_a64_dss_stencil_test(&dflt));

    /* depth: Greater without write, then with write (bit 3), then Always with write (a test that Vulkan only reproduces with the test enabled) */
    mk(b, 1, 0x34, 7, 0, 0, 7, 0, 0);
    assert(lagfx_a64_dss_decode(b, 40, &d) && d.depth_compare == LAGFX_MTL_CMP_GREATER && !d.depth_write && lagfx_a64_dss_depth_test(&d));
    mk(b, 1, 0x3c, 7, 0, 0, 7, 0, 0);
    assert(lagfx_a64_dss_decode(b, 40, &d) && d.depth_compare == LAGFX_MTL_CMP_GREATER && d.depth_write);
    mk(b, 1, 0x3f, 7, 0, 0, 7, 0, 0);
    assert(lagfx_a64_dss_decode(b, 40, &d) && d.depth_compare == LAGFX_MTL_CMP_ALWAYS && d.depth_write && lagfx_a64_dss_depth_test(&d));
    mk(b, 1, 0xff, 7, 0, 0, 7, 0, 0);   /* the two unused high bits are not read */
    assert(lagfx_a64_dss_decode(b, 40, &d) && d.depth_compare == 7 && d.depth_write && d.unknown_bits == 3);

    /* every ops sub-field on its own, each face from its own bytes */
    static const struct { uint32_t ops; uint8_t cmp, fail, dfail, pass; } k[] = {
        { 0x00000007u, 7, 0, 0, 0 }, { 0x00000002u, 2, 0, 0, 0 }, { 0x0000001fu, 7, 3, 0, 0 }, { 0x00000107u, 7, 0, 4, 0 }, { 0x00000a07u, 7, 0, 0, 5 },
        { 0x00000005u, 5, 0, 0, 0 }, { 0x00000037u, 7, 6, 0, 0 }, { 0x000001c7u, 7, 0, 7, 0 }, { 0x00000407u, 7, 0, 0, 2 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
        mk(b, 2, 0x37, k[i].ops, 1, 2, 0x00000007u, 3, 4);                               /* front carries the case, back stays default */
        assert(lagfx_a64_dss_decode(b, 40, &d));
        assert(d.front.compare == k[i].cmp && d.front.fail_op == k[i].fail && d.front.depth_fail_op == k[i].dfail && d.front.pass_op == k[i].pass);
        assert(d.back.compare == 7 && d.back.fail_op == 0 && d.back.depth_fail_op == 0 && d.back.pass_op == 0);
        mk(b, 2, 0x37, 0x00000007u, 1, 2, k[i].ops, 3, 4);                               /* and the other way round */
        assert(lagfx_a64_dss_decode(b, 40, &d));
        assert(d.back.compare == k[i].cmp && d.back.fail_op == k[i].fail && d.back.depth_fail_op == k[i].dfail && d.back.pass_op == k[i].pass);
        assert(d.front.compare == 7 && d.front.fail_op == 0);
        if (k[i].ops != 7u) assert(lagfx_a64_dss_stencil_test(&d));                      /* any face that does something turns the stencil test on */
    }
    mk(b, 2, 0x37, 7, 0, 0, 7, 0, 0);
    assert(lagfx_a64_dss_decode(b, 40, &d) && !lagfx_a64_dss_stencil_test(&d));           /* Always / Keep, whatever the masks, is a no-op */
    mk(b, 2, 0x37, 0x00000002u, 0, 0, 7, 0, 0);
    assert(lagfx_a64_dss_decode(b, 40, &d) && lagfx_a64_dss_stencil_test(&d));            /* Equal on one face alone */

    /* masks: the two faces are different bytes */
    mk(b, 3, 0x37, 2, 0x11223344u, 0x55667788u, 5, 0x99aabbccu, 0xddeeff00u);
    assert(lagfx_a64_dss_decode(b, 40, &d));
    assert(d.front.compare == 2 && d.back.compare == 5);
    assert(d.front.read_mask == 0x11223344u && d.front.write_mask == 0x55667788u && d.back.read_mask == 0x99aabbccu && d.back.write_mask == 0xddeeff00u);

    /* bytes the serializer does not write must not matter */
    uint8_t c[40];
    mk(b, 4, 0x37, 7, 1, 2, 7, 3, 4);
    memcpy(c, b, 40); c[13] = 0x5a, c[14] = 0xa5, c[15] = 0xff, c[12] |= 0xc0;
    lagfx_a64_dss_t d2;
    assert(lagfx_a64_dss_decode(b, 40, &d) && lagfx_a64_dss_decode(c, 40, &d2) && !memcmp(&d, &d2, sizeof d));

    /* rejected: wrong tag, wrong size word, wrong length, NULL; *out untouched */
    memset(&d, 0x77, sizeof d);
    mk(b, 5, 0x37, 7, 0, 0, 7, 0, 0); w32(b, 3);
    assert(!lagfx_a64_dss_decode(b, 40, &d));
    mk(b, 5, 0x37, 7, 0, 0, 7, 0, 0); w32(b + 4, 36);
    assert(!lagfx_a64_dss_decode(b, 40, &d));
    mk(b, 5, 0x37, 7, 0, 0, 7, 0, 0);
    assert(!lagfx_a64_dss_decode(b, 39, &d) && !lagfx_a64_dss_decode(b, 44, &d) && !lagfx_a64_dss_decode(b, 0, &d) && !lagfx_a64_dss_decode(NULL, 40, &d) && !lagfx_a64_dss_decode(b, 40, NULL));
    { uint8_t s7[sizeof d]; memset(s7, 0x77, sizeof s7); assert(!memcmp(&d, s7, sizeof d)); }
    puts("PASS: depth/stencil state object decoder (defaults, depth, every ops field per face, masks, unwritten bytes, rejected records)");
    return 0;
}
