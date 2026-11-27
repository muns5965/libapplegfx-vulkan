/* ARM64 render draw records (tags 0x00-0x0b, 0x10, 0x11) and the serialized sampler (tag 3): pure decoders, every field distinct so a swapped / shifted field fails.
 * Layouts: src/protocol/arm64_draw_decode.h, arm64_sampler.h. No device needed.
 * SPDX-License-Identifier: MIT */
#include "protocol/arm64_draw_decode.h"
#include "protocol/arm64_sampler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void w16(uint8_t *b, uint32_t o, uint16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *b, uint32_t o, uint32_t v) { w16(b, o, (uint16_t)v); w16(b, o + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *b, uint32_t o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }

static void test_draws(void) {
    uint8_t r[64]; lagfx_a64_draw_t d;
    memset(r, 0xaa, sizeof r);   /* the unwritten tail of a record is ring noise: it must never be read */
    w32(r, 0, 3), w16(r, 4, 7), w16(r, 6, 11);
    assert(lagfx_a64_draw_decode(0x01, r, 8, &d) == 0 && d.prim == 3 && d.first_vertex == 7 && d.count == 11 && d.instances == 1 && !d.indexed && !d.indirect);
    w32(r, 0, 4), w64(r, 4, 0x11111), w64(r, 12, 0x22222);
    assert(lagfx_a64_draw_decode(0x00, r, 20, &d) == 0 && d.prim == 4 && d.first_vertex == 0x11111 && d.count == 0x22222 && d.instances == 1);
    w16(r, 0, 0x1111), w16(r, 2, 0x2222), w16(r, 4, 0x3333), w16(r, 6, 4);
    assert(lagfx_a64_draw_decode(0x03, r, 8, &d) == 0 && d.first_vertex == 0x1111 && d.count == 0x2222 && d.instances == 0x3333 && d.prim == 4);
    w64(r, 0, 0x10001), w64(r, 8, 0x10002), w64(r, 16, 0x10003), w16(r, 24, 3);
    assert(lagfx_a64_draw_decode(0x02, r, 26, &d) == 0 && d.first_vertex == 0x10001 && d.count == 0x10002 && d.instances == 0x10003 && d.prim == 3);
    w16(r, 0, 1), w16(r, 2, 2), w16(r, 4, 3), w16(r, 6, 4), w16(r, 8, 3);
    assert(lagfx_a64_draw_decode(0x05, r, 10, &d) == 0 && d.first_vertex == 1 && d.count == 2 && d.instances == 3 && d.base_instance == 4 && d.prim == 3);
    w64(r, 0, 1), w64(r, 8, 2), w64(r, 16, 3), w64(r, 24, 0x10004), w16(r, 32, 4);
    assert(lagfx_a64_draw_decode(0x04, r, 34, &d) == 0 && d.first_vertex == 1 && d.count == 2 && d.instances == 3 && d.base_instance == 0x10004 && d.prim == 4);
    /* indexed: u16 prim, u16 index type (1 = UInt32), u32 ref, then count / offset */
    w16(r, 0, 3), w16(r, 2, 1), w32(r, 4, 5151), w16(r, 8, 0x1234), w16(r, 10, 0x5678), w16(r, 12, 9);
    assert(lagfx_a64_draw_decode(0x07, r, 12, &d) == 0 && d.indexed && d.prim == 3 && d.index_type == 1 && d.index_ref == 5151 && d.count == 0x1234 && d.index_offset == 0x5678 && d.instances == 1);
    assert(lagfx_a64_draw_decode(0x09, r, 14, &d) == 0 && d.count == 0x1234 && d.index_offset == 0x5678 && d.instances == 9);
    w64(r, 8, 0x10005), w64(r, 16, 0x10006), w64(r, 24, 0x10007);
    assert(lagfx_a64_draw_decode(0x06, r, 24, &d) == 0 && d.count == 0x10005 && d.index_offset == 0x10006 && d.instances == 1 && d.index_ref == 5151);
    assert(lagfx_a64_draw_decode(0x08, r, 32, &d) == 0 && d.count == 0x10005 && d.index_offset == 0x10006 && d.instances == 0x10007);
    /* 0x0b: the byte offset comes BEFORE the count, base vertex is signed 16 bit */
    w16(r, 8, 0x2222), w16(r, 10, 0x1111), w16(r, 12, 7), w16(r, 14, (uint16_t)-2), w16(r, 16, 8);
    assert(lagfx_a64_draw_decode(0x0b, r, 18, &d) == 0 && d.index_offset == 0x2222 && d.count == 0x1111 && d.instances == 7 && d.base_vertex == -2 && d.base_instance == 8);
    w64(r, 8, 0x10009), w64(r, 16, 0x1000a), w64(r, 24, 0x1000b), w64(r, 32, (uint64_t)-70000ll), w64(r, 40, 0x1000c);
    assert(lagfx_a64_draw_decode(0x0a, r, 48, &d) == 0 && d.index_offset == 0x10009 && d.count == 0x1000a && d.instances == 0x1000b && d.base_vertex == -70000 && d.base_instance == 0x1000c);
    /* indirect */
    w64(r, 0, 0x1111), w32(r, 8, 5151), w16(r, 12, 3);
    assert(lagfx_a64_draw_decode(0x10, r, 14, &d) == 0 && d.indirect && !d.indexed && d.indirect_offset == 0x1111 && d.indirect_ref == 5151 && d.prim == 3);
    w16(r, 0, 4), w16(r, 2, 1), w32(r, 4, 5151), w32(r, 8, 5252), w64(r, 12, 0x1111), w64(r, 20, 0x2222);
    assert(lagfx_a64_draw_decode(0x11, r, 28, &d) == 0 && d.indirect && d.indexed && d.prim == 4 && d.index_type == 1 && d.index_ref == 5151 && d.indirect_ref == 5252 && d.index_offset == 0x1111 && d.indirect_offset == 0x2222);
    uint8_t blk[20]; uint32_t istart = 99;
    w32(blk, 0, 6), w32(blk, 4, 7), w32(blk, 8, 8), w32(blk, 12, (uint32_t)-3), w32(blk, 16, 9);
    assert(lagfx_a64_draw_apply_indirect(&d, blk, 20, &istart) && d.count == 6 && d.instances == 7 && istart == 8 && d.base_vertex == -3 && d.base_instance == 9);
    assert(!lagfx_a64_draw_apply_indirect(&d, blk, 19, &istart));                         /* a short block is refused, not read as zeros */
    assert(lagfx_a64_draw_decode(0x10, r, 14, &d) == 0 && lagfx_a64_draw_apply_indirect(&d, blk, 16, NULL) && d.count == 6 && d.instances == 7 && d.first_vertex == 8 && d.base_instance == (uint32_t)-3);
    /* short and foreign records */
    static const uint32_t need[] = { 20, 8, 26, 8, 34, 10, 24, 12, 32, 14, 48, 18 };      /* tags 0..11 */
    for (uint32_t t = 0; t < 12; t++) assert(lagfx_a64_draw_decode(t, r, need[t] - 1, &d) == LAGFX_A64_DRAW_SHORT && lagfx_a64_draw_decode(t, r, need[t], &d) == 0);
    assert(lagfx_a64_draw_decode(0x10, r, 13, &d) == LAGFX_A64_DRAW_SHORT && lagfx_a64_draw_decode(0x11, r, 27, &d) == LAGFX_A64_DRAW_SHORT);
    assert(lagfx_a64_draw_decode(0x0c, r, 64, &d) == LAGFX_A64_DRAW_NOT_A_DRAW && lagfx_a64_draw_decode(0x74, r, 64, &d) == LAGFX_A64_DRAW_NOT_A_DRAW);
    puts("PASS: draw record layouts (0x00-0x0b, 0x10, 0x11), indirect argument blocks");
}

#ifdef LAGFX_HAVE_VULKAN
static void samp(uint8_t *b, uint32_t state, float lo, float hi) {
    memset(b, 0xaa, 36);
    w32(b, 0, 3), w32(b, 4, 36), w32(b, 8, 7), w32(b, 12, state);
    uint32_t u; memcpy(&u, &lo, 4); w32(b, 20, u); memcpy(&u, &hi, 4); w32(b, 24, u);
}
static void test_sampler(void) {
    uint8_t b[36]; VkSamplerCreateInfo ci; uint32_t f;
    samp(b, 0x84000000u, 0.f, 3.4028235e38f);                               /* CAPTURED word (also seen in our own dump): nearest, clamp to edge, not mip mapped, normalized */
    f = lagfx_a64_sampler_decode(b, 28, true, 16.f, &ci);
    assert(f == 0 && ci.minFilter == VK_FILTER_NEAREST && ci.magFilter == VK_FILTER_NEAREST && ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
           ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && ci.maxLod == 0.25f && !ci.anisotropyEnable &&
           ci.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    f = lagfx_a64_sampler_decode(b, 28, true, 16.f, &ci); (void)f;
    /* The words below except 0x84000000 / 0x84000005 / 0x04000000 / 0x04000005 are SYNTHETIC: built from the field layout in arm64_sampler.h to exercise
     * each field, not captured from a device. */
    samp(b, 0x84000001u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_NEAREST);
    samp(b, 0x84000004u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.minFilter == VK_FILTER_NEAREST && ci.magFilter == VK_FILTER_LINEAR);
    samp(b, 0x84000300u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT && ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    samp(b, 0x84004000u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
           ci.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);       /* clamp to zero: always transparent black */
    samp(b, 0x84020000u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_REPEAT && ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    samp(b, 0x84255500u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci);   /* SYNTHETIC (built from the INFER field positions, not captured): border colour opaque white, S = T = R = clamp to border */
    assert(ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER &&
           ci.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
    samp(b, 0x84155500u, 0, 1e30f);  lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    /* mip filter + LOD clamps: min 0.25, max the FLT_MAX default -> none; not mip mapped keeps level 0 */
    samp(b, 0x86000000u, 0.25f, 3.4028235e38f); f = lagfx_a64_sampler_decode(b, 28, false, 0, &ci);
    assert((f & LAGFX_A64_SAMP_MIPMAPPED) && ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && ci.minLod == 0.25f && ci.maxLod == VK_LOD_CLAMP_NONE);
    samp(b, 0x85000000u, 0.25f, 6.5f);  f = lagfx_a64_sampler_decode(b, 28, false, 0, &ci);
    assert(ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && ci.minLod == 0.25f && ci.maxLod == 6.5f);
    samp(b, 0x84000000u, 0.25f, 6.5f);  f = lagfx_a64_sampler_decode(b, 28, false, 0, &ci);
    assert(!(f & LAGFX_A64_SAMP_MIPMAPPED) && ci.minLod == 0.f && ci.maxLod == 0.25f);
    samp(b, 0x86000000u, 5.f, 2.f);     lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(ci.minLod == 0.f && ci.maxLod == 32.f);   /* inverted: the defaults */
    samp(b, 0x86000000u, 5.f, 2.f);     lagfx_a64_sampler_decode(b, 20, false, 0, &ci); assert(ci.minLod == 0.f && ci.maxLod == VK_LOD_CLAMP_NONE);   /* record ends before the clamps */
    /* anisotropy: 13 verbatim, limited by the device; off without the feature */
    samp(b, 0xb4000000u, 0, 1e30f);     f = lagfx_a64_sampler_decode(b, 28, true, 16.f, &ci); assert((f & LAGFX_A64_SAMP_ANISO_APPLIED) && ci.anisotropyEnable && ci.maxAnisotropy == 13.f);
    f = lagfx_a64_sampler_decode(b, 28, true, 8.f, &ci); assert(ci.maxAnisotropy == 8.f);
    f = lagfx_a64_sampler_decode(b, 28, false, 16.f, &ci); assert(!ci.anisotropyEnable && !(f & LAGFX_A64_SAMP_ANISO_APPLIED));
    /* what is not mapped says so */
    samp(b, 0x84000080u, 0, 1e30f);     f = lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(f & LAGFX_A64_SAMP_COMPARE_IGNORED);
    /* unnormalized coordinates (real device samplers 0x04000000 / 0x04000005): Vulkan's restrictions are applied */
    samp(b, 0x04000005u, 0, 1e30f);     f = lagfx_a64_sampler_decode(b, 28, true, 16.f, &ci);
    assert((f & LAGFX_A64_SAMP_UNNORM_APPLIED) && ci.unnormalizedCoordinates && ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR && ci.maxLod == 0.f && !ci.anisotropyEnable);
    samp(b, 0x06000201u, 0, 1e30f);     f = lagfx_a64_sampler_decode(b, 28, true, 16.f, &ci);   /* mip linear, S = repeat, min linear / mag nearest */
    assert(ci.unnormalizedCoordinates && ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR && ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST &&
           ci.minLod == 0.f && ci.maxLod == 0.f && ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && !(f & LAGFX_A64_SAMP_MIPMAPPED));
    samp(b, 0x84000100u, 0, 1e30f);     f = lagfx_a64_sampler_decode(b, 28, false, 0, &ci); assert(f & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX && ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
    puts("PASS: serialized sampler fields (filters, address modes, border, LOD clamps, anisotropy, unmapped flags)");
}
#endif

int main(void) {
    test_draws();
#ifdef LAGFX_HAVE_VULKAN
    test_sampler();
#endif
    return 0;
}
