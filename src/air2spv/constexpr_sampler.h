/*
 * libapplegfx-vulkan — constexpr sampler state extraction from AIR
 * src/air2spv/constexpr_sampler.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Layout (external, UNVERIFIED on device): field positions come from two
 * independent published sources (an MIT project that states it derived them
 * from Apple's compiler output, and Apache-2.0 LuisaCompute, which emits
 * AIR that runs on Apple GPUs); no Apple documentation exists. Our four
 * captured words decode consistently with it (tests/air2spv-constexpr-
 * sampler-unit.c). Field NAMES applied to our shaders are INFER.
 *
 *   - `!air.sampler_states = !{ !{ !"air.sampler_state", ptr @G } ... }`
 *     names one module global per constexpr sampler; each @G is
 *     `addrspace(2) constant i64 <word>`; a sample call's sampler operand
 *     is a CE_CAST of @G.
 *
 *   bits  0..8   address modes s,t,r (3 bits each): 0 clamp_to_zero,
 *                1 clamp_to_edge, 2 repeat, 3 mirrored_repeat,
 *                4 clamp_to_border
 *   bits  9..10  mag filter  (0 nearest, 1 linear, 2 bicubic)
 *   bits 11..12  min filter  (same values)
 *   bits 13..14  mip filter  (0 none, 1 nearest, 2 linear)
 *   bit  15      pixel (unnormalized) coordinates when SET
 *   bits 16..19  compare function (observed 8 in every word = no compare;
 *                other values undocumented)
 *   bits 20..23  max anisotropy - 1
 *   bits 24..39  min LOD clamp, binary16;  bits 40..55 max LOD clamp, binary16
 *   bits 56..59  border colour / reduction (undocumented; required 0 here)
 *   bit  63      one-word-form marker (set in every observed word). It is NOT
 *                a "normalized coordinates" flag - that was an earlier
 *                misreading (508dad5); normalized == bit 15 clear.
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

/* The translator names the sampler variable of a constexpr sampler "lagfx.cxs.<16 hex digits of the raw i64 word>" (OpName) so that the host can recover the state
 * from the SPIR-V alone (spv_reflect: lagfx_spv_reflect_constexpr_samplers). Coordinates of a pixel (unnormalized) constexpr sampler are divided by the image size
 * IN THE SHADER; the host sampler is always a normalized one (see sampler_cache.h lagfx_sampler_state_from_constexpr_norm). */
#define LAGFX_CXS_NAME_PREFIX "lagfx.cxs."

typedef struct {
    uint32_t global_index;   /* module value id of the @__air_sampler_state global */
    uint64_t raw;            /* the i64 initializer */
    bool     valid;          /* layout check passed; the decoded fields below are meaningful */
    bool     normalized;     /* bit 15 clear (coordinates normalized) */
    uint8_t  addr[3];        /* bits 0..8: s,t,r (0 zero,1 edge,2 repeat,3 mirror,4 border) */
    uint8_t  mag_filter;     /* bits 9..10 (0 nearest,1 linear,2 bicubic) */
    uint8_t  min_filter;     /* bits 11..12 */
    uint8_t  mip_filter;     /* bits 13..14 (0 none,1 nearest,2 linear) */
    uint8_t  compare_func;   /* bits 16..19 (8 = no compare in every observed word) */
    uint8_t  max_aniso;      /* bits 20..23 + 1 */
    float    lod_min;        /* bits 24..39 as half */
    float    lod_max;        /* bits 40..55 as half */
    uint32_t raw_low24;      /* bits 0..23, as captured */
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
