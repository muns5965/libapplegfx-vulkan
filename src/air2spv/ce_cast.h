/*
 * libapplegfx-vulkan - constant-expression casts of scalar constants (AIR CST_CODE_CE_CAST)
 * src/air2spv/ce_cast.h
 *
 * Pure folding with LLVM LangRef semantics (public): the value of `trunc/zext/sext/fptoui/fptosi/uitofp/sitofp/fptrunc/fpext/ptrtoint/inttoptr/bitcast/
 * addrspacecast <constant>`. No Apple GPU semantics are involved. Used by translate_function.c (bind_ce_casts) and unit-tested on its own (tests/air2spv-ce-cast-unit.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LAGFX_AIR2SPV_CE_CAST_H
#define LAGFX_AIR2SPV_CE_CAST_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ok;          /* value known */
    bool undef;       /* undef / poison */
    bool is_float;
    bool is_ptr_null; /* null pointer */
    uint32_t width;   /* bits: 1,8,16,32,64 for ints; 16 (half) / 32 (float) for floats */
    uint64_t bits;    /* integer value masked to width / IEEE bit pattern */
} lagfx_ce_val_t;

uint64_t lagfx_ce_mask(uint32_t width);
uint32_t lagfx_f32_to_f16(float f);     /* round to nearest even */
float    lagfx_f16_to_f32(uint32_t h);

/* Fold `opc` (LLVM bitcode cast opcode: 0 trunc, 1 zext, 2 sext, 3 fptoui, 4 fptosi, 5 uitofp, 6 sitofp, 7 fptrunc, 8 fpext, 9 ptrtoint, 10 inttoptr, 11 bitcast,
 * 12 addrspacecast) of the known scalar constant `src` to a destination of the described type (dst_f: float/half, dst_p: pointer, dw: bit width).
 * Returns false when the cast cannot be evaluated here (the caller leaves the constant unresolved); true with out->undef set when the result is undef / poison
 * (cast of undef / poison, or a float -> int conversion whose result is out of range / NaN: poison per LangRef). */
bool lagfx_ce_fold(uint32_t opc, const lagfx_ce_val_t *src, bool dst_f, bool dst_p, uint32_t dw, lagfx_ce_val_t *out);

#endif
