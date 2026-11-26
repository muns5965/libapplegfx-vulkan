/*
 * libapplegfx-vulkan — ARM64 depth/stencil state object decoder. See arm64_depth_stencil.h.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_depth_stencil.h"
#include "../common/le.h"

static void face(const uint8_t *b, lagfx_a64_ds_face_t *f) {
    uint32_t ops = lagfx_le32(b);
    f->compare = (uint8_t)(ops & 7u), f->fail_op = (uint8_t)((ops >> 3) & 7u), f->depth_fail_op = (uint8_t)((ops >> 6) & 7u), f->pass_op = (uint8_t)((ops >> 9) & 7u);
    f->read_mask = lagfx_le32(b + 4), f->write_mask = lagfx_le32(b + 8);
}

bool lagfx_a64_dss_decode(const uint8_t *blob, uint32_t n, lagfx_a64_dss_t *out) {
    if (!blob || !out || n != LAGFX_A64_DSS_SIZE) return false;
    if (lagfx_le32(blob) != LAGFX_A64_DSS_TAG || lagfx_le32(blob + 4) != LAGFX_A64_DSS_SIZE) return false;
    lagfx_a64_dss_t d;
    d.ref = lagfx_le32(blob + 8);
    uint8_t st = blob[12];
    d.depth_compare = st & 7u, d.depth_write = (st & 8u) != 0, d.unknown_bits = (st >> 4) & 3u;
    face(blob + 16, &d.front);
    face(blob + 28, &d.back);
    *out = d;
    return true;
}

void lagfx_a64_dss_default(lagfx_a64_dss_t *out) {
    lagfx_a64_ds_face_t f = { LAGFX_MTL_CMP_ALWAYS, LAGFX_MTL_STENCIL_KEEP, LAGFX_MTL_STENCIL_KEEP, LAGFX_MTL_STENCIL_KEEP, 0xffffffffu, 0xffffffffu };
    out->ref = 0, out->depth_compare = LAGFX_MTL_CMP_ALWAYS, out->depth_write = false, out->unknown_bits = 3u;
    out->front = f, out->back = f;
}
