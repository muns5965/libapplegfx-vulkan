/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan - 64-bit integer lowering for devices without shaderInt64 (a SPIR-V -> SPIR-V pass)
 * src/air2spv/spv_lower_int64.h
 *
 * Every 64-bit integer value becomes a uvec2 {low word, high word} (the memory layout of a 64-bit little-endian integer, so loads / stores / buffer and workgroup arrays keep their
 * byte layout and strides). Operations are rewritten as 32-bit sequences with the 64-bit MEANING preserved (carry / borrow, 32x32 -> 64 extended multiply, word-crossing shifts, two-word
 * compares), not truncated to 32 bits like the narrowing mode. OpCapability Int64 is removed when nothing 64-bit remains.
 *
 *   lowered:   OpConstant / OpConstantNull / OpUndef, OpIAdd, OpISub, OpIMul, OpSNegate, OpBitwiseAnd / Or / Xor, OpNot, OpShiftLeftLogical / RightLogical / RightArithmetic (amount = low 6 bits),
 *              OpIEqual / INotEqual / U|S GreaterThan(Equal) / LessThan(Equal), OpSelect, OpUConvert / OpSConvert (to / from 8..32 bit), OpPhi, OpLoad / OpStore, OpCopyObject,
 *              function parameters / calls / returns, pointer types to any of these, and 64-bit access-chain INDICES (the low word is the index: an index >= 2^32 is out of range anyway)
 *   refused:   OpUDiv / SDiv / UMod / SRem / SMod, conversions to or from floating point, OpBitcast, bit counting, atomics, vectors of 64-bit integers, OpSpecConstant of a 64-bit integer,
 *              and any other instruction that has a 64-bit integer operand or result. The pass then returns -1 with a reason; it never emits a module it did not fully lower.
 */
#ifndef LAGFX_AIR2SPV_SPV_LOWER_INT64_H
#define LAGFX_AIR2SPV_SPV_LOWER_INT64_H
#include <stddef.h>
#include <stdint.h>

/* in / in_len: a SPIR-V module. Returns 1 and a malloc'ed lowered module (*out, *out_len) when it contained 64-bit integers; 0 (nothing allocated) when it had none; -1 when something could not be
 * lowered (reason in `why`, may be NULL) or on an allocation failure. */
int lagfx_spv_lower_int64(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len, char *why, size_t why_cap);
#endif
