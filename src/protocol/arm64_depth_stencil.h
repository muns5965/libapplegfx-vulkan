/*
 * libapplegfx-vulkan — ARM64 depth/stencil state object (serialized object tag 4) decoder.
 * src/protocol/arm64_depth_stencil.h
 * SPDX-License-Identifier: MIT
 *
 * The object table entry of a depth/stencil state is a type-7 "serialized" object, like a sampler (tag 3) or a render pipeline (tag 0xe). Its payload is
 * the 40-byte record -[PGSerializer newDepthStencilStateWithDescriptor:allocator:] writes:
 *
 *     +00 u32 tag  (4)            +04 u32 size (40)           +08 u32 object ref
 *     +0c u8  depth state: bits 2:0 MTLCompareFunction of the depth test, bit 3 depthWriteEnabled, bits 5:4 written but of unknown meaning
 *             (bytes +0d..+0f are not written by the serializer: stale guest memory, never read here)
 *     +10 front face   {u32 ops, u32 readMask, u32 writeMask}      ops: bits 2:0 compare, 5:3 stencil-fail op, 8:6 depth-fail op, 11:9 depth-stencil-pass op
 *     +1c back face    {same}                                       ops bits 31:12 are zero in every observed case and are ignored
 *
 * Evidence for the layout: the reference implementation medfa12/apple-silicon-anyos-project (pvg-vulkan-wire, ops/depth_stencil.rs), which derived it by
 * setting one MTLDepthStencilDescriptor property at a time on a real Apple serializer; only the layout and the field meanings were taken from it, no code.
 * The real device dump this library was developed against holds no depth/stencil object, so the decoder is checked with synthetic records only.
 * The values are the Metal enums: MTLCompareFunction 0 Never .. 7 Always (Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6),
 * MTLStencilOperation 0 Keep, 1 Zero, 2 Replace, 3 IncrementClamp, 4 DecrementClamp, 5 Invert, 6 IncrementWrap, 7 DecrementWrap.
 *
 * Pure (no Vulkan, no guest memory), so it is unit-testable anywhere.
 */
#ifndef LAGFX_PROTOCOL_ARM64_DEPTH_STENCIL_H
#define LAGFX_PROTOCOL_ARM64_DEPTH_STENCIL_H

#include <stdbool.h>
#include <stdint.h>

#define LAGFX_A64_DSS_TAG  4u
#define LAGFX_A64_DSS_SIZE 40u

enum { LAGFX_MTL_CMP_NEVER = 0, LAGFX_MTL_CMP_LESS, LAGFX_MTL_CMP_EQUAL, LAGFX_MTL_CMP_LESS_EQUAL, LAGFX_MTL_CMP_GREATER, LAGFX_MTL_CMP_NOT_EQUAL,
       LAGFX_MTL_CMP_GREATER_EQUAL, LAGFX_MTL_CMP_ALWAYS };
enum { LAGFX_MTL_STENCIL_KEEP = 0, LAGFX_MTL_STENCIL_ZERO, LAGFX_MTL_STENCIL_REPLACE, LAGFX_MTL_STENCIL_INC_CLAMP, LAGFX_MTL_STENCIL_DEC_CLAMP,
       LAGFX_MTL_STENCIL_INVERT, LAGFX_MTL_STENCIL_INC_WRAP, LAGFX_MTL_STENCIL_DEC_WRAP };

typedef struct {
    uint8_t  compare, fail_op, depth_fail_op, pass_op;   /* Metal enum values, each 0..7 */
    uint32_t read_mask, write_mask;
} lagfx_a64_ds_face_t;

typedef struct {
    uint32_t ref;                     /* the object ref stored in the record */
    uint8_t  depth_compare;           /* MTLCompareFunction */
    bool     depth_write;
    uint8_t  unknown_bits;            /* depth state bits 5:4 as written (0b11 in every observed record) */
    lagfx_a64_ds_face_t front, back;
} lagfx_a64_dss_t;

/* Decodes a payload of exactly 40 bytes whose tag is 4 and whose size word is 40 (the same acceptance rule as the reference). False otherwise; *out is
 * then untouched. */
bool lagfx_a64_dss_decode(const uint8_t *blob, uint32_t n, lagfx_a64_dss_t *out);

/* Metal's defaults (what an encoder uses until setDepthStencilState: runs): depth compare Always, no depth write, both stencil faces Always / Keep. */
void lagfx_a64_dss_default(lagfx_a64_dss_t *out);

/* A stencil face that cannot change anything: compare Always and every operation Keep (the masks then do not matter). */
static inline bool lagfx_a64_ds_face_noop(const lagfx_a64_ds_face_t *f) {
    return f->compare == LAGFX_MTL_CMP_ALWAYS && f->fail_op == LAGFX_MTL_STENCIL_KEEP && f->depth_fail_op == LAGFX_MTL_STENCIL_KEEP && f->pass_op == LAGFX_MTL_STENCIL_KEEP;
}
/* Whether a Vulkan depth test has to be enabled to reproduce the state. Metal always compares, and with depthWriteEnabled writes even for Always; Vulkan
 * writes depth only while the test is enabled, so "test on" = compare != Always || write. */
static inline bool lagfx_a64_dss_depth_test(const lagfx_a64_dss_t *d) { return d->depth_compare != LAGFX_MTL_CMP_ALWAYS || d->depth_write; }
static inline bool lagfx_a64_dss_stencil_test(const lagfx_a64_dss_t *d) { return !lagfx_a64_ds_face_noop(&d->front) || !lagfx_a64_ds_face_noop(&d->back); }

#endif
