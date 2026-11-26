/*
 * libapplegfx-vulkan — constexpr sampler state extraction from AIR
 * src/air2spv/constexpr_sampler.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Encoding as observed in the captured AIR modules (no public spec exists;
 * everything below is derived from the dumps; bits not listed are NOT decoded):
 *
 *   - `!air.sampler_states = !{ !{ !"air.sampler_state", ptr @G } ... }`
 *     names one module global per constexpr sampler.
 *   - Each @G is `addrspace(2) constant i64 <word>` (GLOBALVAR record,
 *     isconst=1, explicit type, addrspace 2, initializer = a module i64
 *     constant).
 *   - A sample call's sampler operand is a constant expression
 *     (CST_CODE_CE_CAST) of @G.
 *
 * Decoded from <word> (the layout check requires bits 56..62 == 0):
 *   bit  63      coordinates normalized (set in every observed word)
 *   bits 40..55  max LOD clamp, IEEE binary16 (observed 0x7bff = 65504,
 *                the largest finite half = Metal's default lodMaxClamp)
 *   bits 24..39  min LOD clamp, IEEE binary16 (observed 0x0000 = 0.0)
 * bits 0..23 (filters / address modes / mip / compare / ...) are NOT
 * evidenced and are carried only as raw_low24.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_CONSTEXPR_SAMPLER_H
#define LIBAPPLEGFX_AIR2SPV_CONSTEXPR_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#include "air/bitcode_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGFX_CONSTEXPR_SAMPLER_MAX 16u

typedef struct {
    uint32_t global_index;   /* module value id of the @__air_sampler_state global */
    uint64_t raw;            /* the i64 initializer */
    bool     valid;          /* layout check passed; the decoded fields below are meaningful */
    bool     normalized;     /* bit 63 */
    float    lod_min;        /* bits 24..39 as half */
    float    lod_max;        /* bits 40..55 as half */
    uint32_t raw_low24;      /* bits 0..23, undecoded */
} lagfx_constexpr_sampler_t;

/* Decode one word. Always fills raw/raw_low24; returns out->valid. */
bool lagfx_constexpr_sampler_decode(uint64_t raw, lagfx_constexpr_sampler_t *out);

/* IEEE binary16 -> float (exposed for tests). */
float lagfx_half_to_float(uint16_t h);

/* Enumerate the module's constexpr samplers (air.sampler_states order).
 * Returns the number written (<= max); modules without any return 0. A
 * sampler whose global/initializer cannot be resolved is skipped. */
uint32_t lagfx_air_extract_constexpr_samplers(const lagfx_air_module_t *m,
                                              lagfx_constexpr_sampler_t *out,
                                              uint32_t max);

/* Follow a sampler call operand (an ABSOLUTE value id) to the module
 * global behind it: a direct global, or CE_CAST chains in the function
 * body's local constants (first_local_value_id = value id of local
 * constant 0, i.e. globals + functions + module constants + args) or the
 * module constants. Returns the global's value id, or -1 when the operand
 * is not a constexpr-sampler global (e.g. a sampler argument). */
int32_t lagfx_air_sampler_operand_global(const lagfx_air_module_t *m,
                                         const lagfx_air_function_body_t *body,
                                         uint32_t first_local_value_id,
                                         uint32_t abs_value_id);

#ifdef __cplusplus
}
#endif
#endif
