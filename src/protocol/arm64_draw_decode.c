/*
 * libapplegfx-vulkan — ARM64 render segment: draw command decoder. See arm64_draw_decode.h for the record layouts.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_draw_decode.h"
#include "../common/le.h"
#include <string.h>

#define L16(o) ((uint64_t)lagfx_le16(rec + (o)))
#define L32(o) ((uint64_t)lagfx_le32(rec + (o)))
#define L64(o) lagfx_le64(rec + (o))

int lagfx_a64_draw_decode(uint32_t tag, const uint8_t *rec, uint32_t n, lagfx_a64_draw_t *d) {
    if (!d) return LAGFX_A64_DRAW_NOT_A_DRAW;
    memset(d, 0, sizeof *d);
    d->instances = 1;
    uint32_t need;
    switch (tag) {
    case 0x01: need = 8; break;   case 0x00: need = 20; break;
    case 0x03: need = 8; break;   case 0x02: need = 26; break;
    case 0x05: need = 10; break;  case 0x04: need = 34; break;
    case 0x07: need = 12; break;  case 0x06: need = 24; break;
    case 0x09: need = 14; break;  case 0x08: need = 32; break;
    case 0x0b: need = 18; break;  case 0x0a: need = 48; break;
    case 0x10: need = 14; break;  case 0x11: need = 28; break;
    default: return LAGFX_A64_DRAW_NOT_A_DRAW;
    }
    if (!rec || n < need) return LAGFX_A64_DRAW_SHORT;
    switch (tag) {
    case 0x01: d->prim = (uint32_t)L32(0), d->first_vertex = L16(4), d->count = L16(6); break;
    case 0x00: d->prim = (uint32_t)L32(0), d->first_vertex = L64(4), d->count = L64(12); break;
    case 0x03: d->first_vertex = L16(0), d->count = L16(2), d->instances = L16(4), d->prim = (uint32_t)L16(6); break;
    case 0x02: d->first_vertex = L64(0), d->count = L64(8), d->instances = L64(16), d->prim = (uint32_t)L16(24); break;
    case 0x05: d->first_vertex = L16(0), d->count = L16(2), d->instances = L16(4), d->base_instance = L16(6), d->prim = (uint32_t)L16(8); break;
    case 0x04: d->first_vertex = L64(0), d->count = L64(8), d->instances = L64(16), d->base_instance = L64(24), d->prim = (uint32_t)L16(32); break;
    case 0x07: case 0x09:
        d->indexed = true, d->prim = (uint32_t)L16(0), d->index_type = (uint32_t)L16(2), d->index_ref = (uint32_t)L32(4), d->count = L16(8), d->index_offset = L16(10);
        if (tag == 0x09) d->instances = L16(12);
        break;
    case 0x06: case 0x08:
        d->indexed = true, d->prim = (uint32_t)L16(0), d->index_type = (uint32_t)L16(2), d->index_ref = (uint32_t)L32(4), d->count = L64(8), d->index_offset = L64(16);
        if (tag == 0x08) d->instances = L64(24);
        break;
    case 0x0b:   /* the offset comes BEFORE the count here, the opposite of 0x07 / 0x09 */
        d->indexed = true, d->prim = (uint32_t)L16(0), d->index_type = (uint32_t)L16(2), d->index_ref = (uint32_t)L32(4), d->index_offset = L16(8), d->count = L16(10);
        d->instances = L16(12), d->base_vertex = (int16_t)lagfx_le16(rec + 14), d->base_instance = L16(16);
        break;
    case 0x0a:
        d->indexed = true, d->prim = (uint32_t)L16(0), d->index_type = (uint32_t)L16(2), d->index_ref = (uint32_t)L32(4), d->index_offset = L64(8), d->count = L64(16);
        d->instances = L64(24), d->base_vertex = (int64_t)L64(32), d->base_instance = L64(40);
        break;
    case 0x10: d->indirect = true, d->indirect_offset = L64(0), d->indirect_ref = (uint32_t)L32(8), d->prim = (uint32_t)L16(12); break;
    case 0x11:
        d->indirect = d->indexed = true, d->prim = (uint32_t)L16(0), d->index_type = (uint32_t)L16(2), d->index_ref = (uint32_t)L32(4), d->indirect_ref = (uint32_t)L32(8);
        d->index_offset = L64(12), d->indirect_offset = L64(20);
        break;
    }
    return LAGFX_A64_DRAW_OK;
}

bool lagfx_a64_draw_apply_indirect(lagfx_a64_draw_t *d, const uint8_t *rec, uint32_t n, uint32_t *index_start) {
    if (!d || !rec || !d->indirect) return false;
    if (n < (d->indexed ? LAGFX_A64_DRAW_INDEXED_ARGS_LEN : LAGFX_A64_DRAW_UNINDEXED_ARGS_LEN)) return false;
    d->count = L32(0), d->instances = L32(4);
    if (d->indexed) {
        if (index_start) *index_start = (uint32_t)L32(8);
        d->base_vertex = (int32_t)lagfx_le32(rec + 12), d->base_instance = L32(16);
    } else {
        d->first_vertex = L32(8), d->base_instance = L32(12);
    }
    return true;
}
