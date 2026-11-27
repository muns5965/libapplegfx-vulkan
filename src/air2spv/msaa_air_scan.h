/*
 * libapplegfx-vulkan — AIR multisample operations and builtins: detection and counting only. No translation.
 * src/air2spv/msaa_air_scan.h
 *
 * SPDX-License-Identifier: MIT
 *
 * Why only counting: the AIR spelling of a multisample texture read (texture2d_ms::read(coord, sample)), of get_num_samples() and of the [[sample_id]] / [[sample_mask]] /
 * sample-position builtins is NOT in any fixture or dump in hand (grep of the 3 GPU dumps, tests/fixtures and the repository finds none), and Apple does not publish the AIR
 * specification. The MSL source spelling is documented (Metal Shading Language Specification: texture2d_ms<T>::read(uint2 coord, uint sample), get_num_samples(),
 * [[sample_id]], [[sample_mask]] as input and output) but the mapping to AIR names is not. Translating by a guessed name could silently produce wrong shaders, so the translator
 * does not translate them; this scan reports them so a run that uses them says so (g7-gpu-msaa air_ms_ops / air_sample_builtins) and the next capture names the real spellings.
 *
 * The detection itself is [I]: it looks for the documented MSL words embedded in AIR-style names ("air." prefix, "_ms" texture-type suffix, "sample_id", "sample_mask",
 * "sample_position", "num_samples"). A module whose spelling differs is simply not counted (never a wrong translation): the counters can under-report, never over-translate.
 * The Vulkan side (OpImageFetch with the Sample image operand on an MS=1 image, BuiltIn SampleId / SampleMask with the SampleRateShading capability) is proven by hand-written
 * SPIR-V in tests/arm64-msaa-guest-unit.c; it is not reachable from AIR until a spelling is verified.
 */
#ifndef LAGFX_AIR2SPV_MSAA_AIR_SCAN_H
#define LAGFX_AIR2SPV_MSAA_AIR_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include "../air/bitcode_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t ms_ops, sample_builtins; char first[96]; } lagfx_air_msaa_scan_t;

/* Pure classifiers (by name). */
int lagfx_air_msaa_is_ms_op(const char *fn_name);           /* 1 = multisample texture read / query declaration name */
int lagfx_air_msaa_is_sample_builtin(const char *md_string); /* 1 = metadata string naming a sample builtin */

/* Scans the module's function declarations and metadata strings; adds to the process-wide totals when anything is found. */
void lagfx_air2spv_msaa_scan(const lagfx_air_module_t *m, lagfx_air_msaa_scan_t *out);

typedef struct { uint64_t modules, ms_ops, sample_builtins; } lagfx_air_msaa_totals_t;
void lagfx_air2spv_msaa_totals(lagfx_air_msaa_totals_t *out);

#ifdef __cplusplus
}
#endif
#endif
