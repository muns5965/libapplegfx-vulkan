/*
 * libapplegfx-vulkan — Phase 5 per-function AIR → SPIR-V translator
 * src/air2spv/translate_function.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Translates one AIR function body (a decoded FUNCTION_BLOCK) into a
 * complete Vulkan-compliant SPIR-V module. Composes the per-pattern
 * lessons from the Phase 4 reference emitters (emit_position,
 * emit_extinst_glsl, emit_vector_shuffle, …) into a per-AIR-record
 * dispatch.
 *
 * Two-pass shape:
 *   - Pass 1: classify instructions, build the SSA-id map sizing,
 *     identify the interface (output vars, input builtins), enumerate
 *     types referenced.
 *   - Pass 2: emit SPIR-V per AIR record, looking up operand SPIR-V ids
 *     from the map.
 *
 * Operand value-id resolution follows LLVM's bitcode rules:
 *   - Module values live at IDs 0..(num_functions + num_module_consts - 1)
 *     in ValueEnumerator order: functions first, then module constants.
 *   - Function arguments take the next N IDs.
 *   - Function-local CONSTANTS_BLOCK adds K IDs after args (NOT decoded
 *     by the reader yet — operands referencing these resolve to OpUndef
 *     placeholders for now; tracked in [[task_function_local_consts]]).
 *   - Each value-producing instruction adds the next ID.
 *
 * Many AIR opcodes encode operand value-ids relative to the next
 * instruction's value-id (`actual = next_inst_val - encoded`). The
 * resolver handles both forms.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H
#define LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H

#include "air/bitcode_reader.h"
#include "libapplegfx-vulkan.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAGFX_XLATE_STAGE_VERTEX   = 0,
    LAGFX_XLATE_STAGE_FRAGMENT = 1,
    LAGFX_XLATE_STAGE_COMPUTE  = 2,   /* GLCompute: air.kernel (compute kernel or tile function) */
} lagfx_xlate_stage_t;

/* Translate the body of `m->functions[fn_idx]` into a SPIR-V module.
 *
 * The function MUST have a body (is_proto == false; body_offset != 0).
 * The caller is responsible for selecting the entry-point function
 * (typically: the one whose name appears in the air.vertex /
 * air.fragment named-metadata's target NODE).
 *
 * On success: *out_blob is a malloc'd SPIR-V buffer; caller frees with
 * free(). *out_size_bytes is the byte length.
 *
 * Returns:
 *   LAGFX_OK              — translated; blob spirv-val-clean (subject
 *                           to Phase 5 maturity)
 *   LAGFX_ERR_INVALID_ARG — bad inputs; fn has no body
 *   LAGFX_ERR_PROTOCOL    — body decode failed or contains opcodes the
 *                           current translator cannot handle without
 *                           losing structural validity
 *   LAGFX_ERR_OUT_OF_MEMORY — allocation failure
 */
lagfx_status_t lagfx_air2spv_translate_function(
    const lagfx_air_module_t *m,
    uint32_t                  fn_idx,
    lagfx_xlate_stage_t       stage,
    uint8_t                 **out_blob,
    size_t                   *out_size_bytes);

/* Stage-interface keys. Metal links a vertex output to a fragment input BY
 * NAME ("generated(10texcoord0ADv2_f)", "air.position", ...), not by
 * position. `key[i]` is the key of the i-th vertex-output struct member (for
 * a vertex module) or of the i-th function argument (for a fragment module);
 * "" = no key. */
#define LAGFX_STAGE_KEYS_MAX 128
#define LAGFX_STAGE_KEY_LEN  72
typedef struct {
    uint32_t n;
    char     key[LAGFX_STAGE_KEYS_MAX][LAGFX_STAGE_KEY_LEN];
    /* VERTEX OUTPUT keys only (lagfx_air_stage_keys(outputs=true)): the shape of
     * the Output variable the vertex translator declares for member i, used by
     * the stage linker. `typed` = 0 -> shapes absent/disabled, the linker is off
     * and the fragment stage-in Locations behave exactly as before.
     * kind: LAGFX_SHAPE_NONE = not modelled, _FLOAT = 32-bit float lanes (a half
     * member is declared as float32), _INT = 32-bit integer lanes. */
    uint8_t  typed;
    uint8_t  kind[LAGFX_STAGE_KEYS_MAX];
    uint8_t  lanes[LAGFX_STAGE_KEYS_MAX];
} lagfx_stage_keys_t;
#define LAGFX_SHAPE_NONE  0
#define LAGFX_SHAPE_FLOAT 1
#define LAGFX_SHAPE_INT   2

/* Stage-linker counters (process totals, see lagfx_air2spv_link_totals):
 *   mismatch_fixed  FS input read with the VS output's type and converted
 *   missing_zeroed  FS input with no VS output -> zero-initialised Private
 *   unlinked        FS input the linker could not resolve (left as before) */
typedef struct { uint64_t mismatch_fixed, missing_zeroed, unlinked; } lagfx_stage_link_totals_t;

/* air.convert integer -> integer calls (process totals): int_int = lowered by the SOURCE signedness (S07); int_int_unhandled = a spelling this lowering does not know
 * (bool source, widths outside 8/16/32/64): previous destination-kind mapping kept, counted. */
typedef struct { uint64_t int_int, int_int_unhandled; } lagfx_conv_totals_t;
void lagfx_air2spv_conv_totals(lagfx_conv_totals_t *out);

/* air.discard_fragment (MSL discard_fragment()) lowering. demote = true: OpDemoteToHelperInvocation (needs the device feature shaderDemoteToHelperInvocation; the module then
 * declares Capability DemoteToHelperInvocation, gated by LAGFX_VK_CAP_DEMOTE) - the fragment stops writing but still takes part in derivatives, like a Metal discarded fragment [I].
 * demote = false (the default; valid on every device): OpKill, which ends the invocation; it is a block terminator, so the block is split with a constant-true
 * OpBranchConditional. That also holds inside the two recognised single-loop plans: the only OpPhi parent that can be a block being split is the plan's ENTRY block, and the
 * split moves that label (S06; an earlier revision dropped the discard there). Kill vs demote [V: SPV_EXT_demote_to_helper_invocation text]: demote
 * "does not necessarily terminate the invocation", is not a flow-control instruction (control flow stays uniform, derivatives stay valid), stores after it are suppressed;
 * OpKill terminates the invocation and is a block terminator (SPIR-V 1.6 deprecates it in favour of OpTerminateInvocation / OpDemoteToHelperInvocation [V: changelog]).
 * Per thread (the resolver translates on one thread); set it before each translation. */
void lagfx_air2spv_set_discard_demote(bool demote);
/* Process totals: lowered as demote / as OpKill; `dropped` is kept for the log format and is always 0 since the loop-plan case is lowered. */
typedef struct { uint64_t demote, kill, dropped; } lagfx_discard_totals_t;
void lagfx_air2spv_discard_totals(lagfx_discard_totals_t *out);

/* D2 candidate, OPT-IN (default OFF): lower air.fmin / fmax / clamp / fast_* / saturate with GLSL.std.450 NMin / NMax / NClamp instead of FMin / FMax / FClamp (a NaN operand then yields the other
 * operand). mode 1 = on, 0 = off, -1 = follow the environment variable LAGFX_SPV_NANMAX=1 (the default state). Process-wide. Off leaves every translation byte-identical. */
void lagfx_air2spv_set_nanmax(int mode);

/* Same as lagfx_air2spv_translate_function; a fragment module's stage-in
 * Locations follow `vs_out` (the paired vertex module's output keys) via
 * `own_in` (this module's per-argument keys). Either may be NULL → legacy
 * sequential locations. */
lagfx_status_t lagfx_air2spv_translate_function_ex(
    const lagfx_air_module_t *m,
    uint32_t                  fn_idx,
    lagfx_xlate_stage_t       stage,
    const lagfx_stage_keys_t *own_in,
    const lagfx_stage_keys_t *vs_out,
    uint8_t                 **out_blob,
    size_t                   *out_size_bytes);

/* Compute kernels: translate Metal's simdgroup operations (thread_index_in_simdgroup, threads_per_simdgroup, simdgroup_index_in_threadgroup, simdgroups_per_threadgroup,
 * simd_shuffle*, simd_shuffle_and_fill_*) for LOGICAL groups of `width` lanes (a power of two, 32 for Metal) carved out of the device's wider subgroup, for the calling
 * thread's following translations. 0 = native (the device subgroup is the simdgroup; the default). See t_simd_emul in translate_function.c for the assumptions. */
void lagfx_air2spv_set_simd_emulation(uint32_t width);

/* SIMD-group / quad-group operations (air.simd*, air.quad*, ...) the translator cannot translate (anything but the shuffles / queries listed above). Such a call used to
 * leave an OpUndef behind silently. A COMPUTE translation that meets one now FAILS (LAGFX_ERR_PROTOCOL); graphics stages still build but count them.
 *   lagfx_air2spv_last_unsupported_simd   the calling thread's last translation: number of such calls (0 = none) and the first one's name (first_name may be NULL)
 *   lagfx_air2spv_simd_unsupported_totals process-wide: translations that met one, calls, compute kernels rejected */
typedef struct { uint64_t modules, calls, kernels_rejected; } lagfx_simd_unsupported_totals_t;
uint32_t lagfx_air2spv_last_unsupported_simd(char *first_name, size_t cap);
void lagfx_air2spv_simd_unsupported_totals(lagfx_simd_unsupported_totals_t *out);

/* Internal (tests): look up an AIR math-intrinsic callee name in the GLSL.std.450 dispatch table. On a hit returns true and fills the GLSL.std.450 instruction
 * number, the operand count and whether it is a chained three-operand form (fmax3/fmin3); any out pointer may be NULL. */
bool lagfx_air2spv_glsl_intrinsic_lookup(const char *fn_name, uint32_t *glsl_inst, uint32_t *num_args, bool *three_way);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* std430-style size and alignment of an AIR type under the AIR data layout (LLVM alloc size: <3 x float> is 16 bytes, align 16). Exposed for tests. */
bool lagfx_air2spv_type_layout(const lagfx_air_module_t *m, uint32_t ty, uint32_t *size, uint32_t *align);
#ifdef __cplusplus
}
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H */
