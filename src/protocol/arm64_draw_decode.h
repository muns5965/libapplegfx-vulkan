/*
 * libapplegfx-vulkan — ARM64 render segment: draw command decoder (tags 0x00-0x0b direct, 0x10 / 0x11 indirect).
 * src/protocol/arm64_draw_decode.h
 *
 * Pure decoding of the draw records into one neutral description; nothing here touches Vulkan or guest memory, so the layouts are unit-tested on their own
 * (tests/arm64-draw-decode-unit.c). Record layouts (payload = after the 8-byte {tag, size} header; all little endian, packed, no alignment):
 *
 *   Every direct draw has a compact form (odd tag, 16-bit counts) and a wide form (the even tag below it, 64-bit counts); the guest writes the wide one when
 *   any count / start / offset does not fit 16 bits, and then EVERY count of the record is wide.
 *
 *   0x01 draw             u32 prim, u16 start, u16 count                                       (payload 8)
 *   0x00 draw wide        u32 prim, u64 start, u64 count                                       (20)
 *   0x03 instanced        u16 start, u16 count, u16 instances, u16 prim                        (8)
 *   0x02 instanced wide   u64 start, u64 count, u64 instances, u16 prim                        (26 of 28)
 *   0x05 instanced+base   u16 start, u16 count, u16 instances, u16 base_instance, u16 prim     (10 of 12)
 *   0x04 ... wide         u64 start, u64 count, u64 instances, u64 base_instance, u16 prim     (34 of 36)
 *   0x07 indexed          u16 prim, u16 index_type (0 u16, 1 u32), u32 index_ref, u16 count, u16 index_offset (bytes)   (12)
 *   0x06 indexed wide     u16 prim, u16 index_type, u32 index_ref, u64 count, u64 index_offset  (24)
 *   0x09 indexed inst.    as 0x07, then u16 instances                                          (14 of 16)
 *   0x08 ... wide         as 0x06, then u64 instances                                          (32)
 *   0x0b indexed full     u16 prim, u16 index_type, u32 index_ref, u16 index_offset, u16 count (NOTE: offset BEFORE count), u16 instances,
 *                         i16 base_vertex (truncated by the serializer, sign-extended here), u16 base_instance   (18 of 20)
 *   0x0a ... wide         u16 prim, u16 index_type, u32 index_ref, u64 index_offset, u64 count, u64 instances, i64 base_vertex, u64 base_instance   (48)
 *   0x10 draw indirect    u64 indirect_offset, u32 indirect_ref, u16 prim                      (14 of 16)
 *   0x11 indexed indirect u16 prim, u16 index_type, u32 index_ref, u32 indirect_ref, u64 index_offset, u64 indirect_offset   (28)
 *
 * The indirect argument blocks (Metal's MTLDrawPrimitivesIndirectArguments / MTLDrawIndexedPrimitivesIndirectArguments) are four / five u32:
 *   {vertexCount, instanceCount, vertexStart, baseInstance}   and   {indexCount, instanceCount, indexStart, baseVertex (signed), baseInstance}.
 * Source of the layouts: the independent derivation in medfa12/apple-silicon-anyos-project (pvg-vulkan-wire ops/render.rs, contract/draw.rs), used for
 * protocol facts only.
 */
#ifndef LAGFX_PROTOCOL_ARM64_DRAW_DECODE_H
#define LAGFX_PROTOCOL_ARM64_DRAW_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_A64_DRAW_UNINDEXED_ARGS_LEN 16u
#define LAGFX_A64_DRAW_INDEXED_ARGS_LEN   20u

typedef struct {
    bool     indexed, indirect;
    uint32_t prim;                 /* MTLPrimitiveType ordinal (0 point .. 4 triangle strip) */
    uint32_t index_type;           /* indexed: 0 = UInt16, 1 = UInt32 */
    uint32_t index_ref;            /* indexed: object ref of the index buffer */
    uint64_t index_offset;         /* indexed, direct: byte offset into the index buffer (the indirect form: byte offset too) */
    uint64_t first_vertex;         /* non-indexed */
    uint64_t count;                /* vertexCount, or indexCount when indexed (direct only) */
    uint64_t instances;
    uint64_t base_instance;
    int64_t  base_vertex;          /* indexed only */
    uint32_t indirect_ref;         /* indirect: buffer holding the argument block */
    uint64_t indirect_offset;
} lagfx_a64_draw_t;

enum { LAGFX_A64_DRAW_OK = 0, LAGFX_A64_DRAW_NOT_A_DRAW = 1, LAGFX_A64_DRAW_SHORT = 2 };

/* Decodes one draw record. NOT_A_DRAW for any other tag; SHORT if the payload is smaller than the written part of the record. */
int lagfx_a64_draw_decode(uint32_t tag, const uint8_t *rec, uint32_t n, lagfx_a64_draw_t *out);
/* Applies an indirect argument block (16 bytes unindexed, 20 indexed) to d: fills count / instances / first_vertex (or index start in elements ->
 * index_offset is NOT touched, the caller gets it in *index_start) / base_vertex / base_instance. False if blk_len is too short. */
bool lagfx_a64_draw_apply_indirect(lagfx_a64_draw_t *d, const uint8_t *blk, uint32_t blk_len, uint32_t *index_start);

#endif
