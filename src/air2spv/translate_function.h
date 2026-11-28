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
    int16_t  loc[LAGFX_STAGE_KEYS_MAX];   /* KERNEL keys only: the argument's "air.location_index" (the Metal [[buffer/texture/threadgroup(n)]] index), -1 when absent */
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

/* Tile kernels: imageblock colour attachments named by `rt_mask` (bit n = attachment n) are declared as unformatted storage images (Image Format Unknown + the StorageImage
 * Read/WriteWithoutFormat capabilities) instead of Rgba16f, for the calling thread's following translations. 0 = all Rgba16f (the default; output unchanged). */
void lagfx_air2spv_set_imageblock_unformatted(uint32_t rt_mask);

/* SIMD-group / quad-group operations (air.simd*, air.quad*, ...) the translator cannot translate (anything but the shuffles / queries listed above). Such a call used to
 * leave an OpUndef behind silently. A COMPUTE translation that meets one now FAILS (LAGFX_ERR_PROTOCOL); graphics stages still build but count them.
 *   lagfx_air2spv_last_unsupported_simd   the calling thread's last translation: number of such calls (0 = none) and the first one's name (first_name may be NULL)
 *   lagfx_air2spv_simd_unsupported_totals process-wide: translations that met one, calls, compute kernels rejected */
typedef struct { uint64_t modules, calls, kernels_rejected; } lagfx_simd_unsupported_totals_t;
uint32_t lagfx_air2spv_last_unsupported_simd(char *first_name, size_t cap);
/* `threadgroup T *p [[threadgroup(n)]]` kernel arguments (metadata key "air.threadgroup_buffer"; the key spelling is the translator's reading of Apple's AIR, NOT yet seen in a captured
 * module): modelled as a Workgroup array T[N] with N a SpecConstant, SpecId 3 + n (n = the Metal [[threadgroup(n)]] index = air.location_index, 0..30), unit = elements, default 1024 (a
 * dispatcher must give every one an explicit length: lagfx_spv_reflect_threadgroup_args); the constant is OpName'd "lagfx.tgarg.<n>.<element bytes>". An argument the translator cannot model (element not a 32/64-bit integer / float, a bare dereference, declared and accessed element types that differ, no or
 * duplicate location index) makes the COMPUTE translation fail (LAGFX_ERR_PROTOCOL); this returns the calling thread's last translation's count (0 = none) and the first reason. */
uint32_t lagfx_air2spv_last_unsupported_threadgroup_arg(char *first_reason, size_t cap);
void lagfx_air2spv_simd_unsupported_totals(lagfx_simd_unsupported_totals_t *out);

/* Atomics (air.atomic.*): 32-bit integer load / store / exchange / compare-exchange / add sub and or xor min max on device (StorageBuffer) and threadgroup (Workgroup) memory,
 * 64-bit integer (needs LAGFX_VK_CAP_ATOMIC64_BUF for device memory / LAGFX_VK_CAP_ATOMIC64_SHARED for threadgroup memory) and 32-bit float add (needs the per-storage-class
 * LAGFX_VK_CAP_ATOMIC_F32_*_ADD). An air.atomic.* call whose name, operand shape or type the translator does
 * not implement is NEVER dropped or emulated: the translation FAILS (LAGFX_ERR_PROTOCOL, any stage) and is counted, per named reason.
 * STRICT operand grammar (default): only the operand combinations seen in real Apple modules are accepted: load(ptr, 0, 2, true), store(ptr, v, 0, 2, true),
 * rmw(ptr, v, 0, 2, 0, true); any other literal in the order / scope / extra / final-flag position, or a different operand count, is refused with a named reason (LAGFX_ATOMIC_R_*).
 * Float atomics are refused (their AIR spelling was never observed) and a 64-bit atomic whose declared return is not void is refused (return type unobserved).
 * LAGFX_ATOMIC_LENIENT=1 (or lagfx_air2spv_set_atomic_lenient(1) in tests) restores the earlier permissive grammar for experiments: scope / extra / flag literals are not
 * checked, float add and a non-void 64-bit return are accepted; the memory order operand must still be 0 and the CAS form guard still applies. Calls that strict mode would
 * have refused but lenient mode accepted are counted (lenient_accepted).
 *   lagfx_air2spv_last_unsupported_atomic  the calling thread's last translation: number of refused atomic calls (0 = none) and the first one's name (first_name may be NULL)
 *   lagfx_air2spv_last_atomic_reason       the calling thread's last translation: LAGFX_ATOMIC_R_* of the first refused call (LAGFX_ATOMIC_R_NONE = none)
 *   lagfx_air2spv_atomic_totals            process-wide: translations that contained atomics, atomic calls translated, calls refused, modules rejected, refusals per reason */
typedef enum {
    LAGFX_ATOMIC_R_NONE = 0,
    LAGFX_ATOMIC_R_SPELLING,        /* name / type combination not in the implemented grammar (float load, signedness-less min, ...) */
    LAGFX_ATOMIC_R_OPERAND_COUNT,   /* argument count differs from the observed form (strict) or is too small */
    LAGFX_ATOMIC_R_ORDER,           /* memory order operand is not the literal 0 (relaxed); always refused */
    LAGFX_ATOMIC_R_SCOPE,           /* scope operand is not the observed literal 2 (strict) */
    LAGFX_ATOMIC_R_RMW_EXTRA,       /* the extra i32 operand of the rmw form is not the observed literal 0 (strict) */
    LAGFX_ATOMIC_R_FLAG,            /* the final i1 operand is not the observed constant true (strict) */
    LAGFX_ATOMIC_R_FLOAT,           /* float atomic: AIR spelling never observed (strict) */
    LAGFX_ATOMIC_R_I64_RETURN,      /* 64-bit atomic whose declared return is not void: the real AIR return type is unknown (strict) */
    LAGFX_ATOMIC_R_CAS_FORM,        /* compare-exchange not in the one implemented (synthetic) form: expected by pointer in function memory, i1 result */
    LAGFX_ATOMIC_R_POINTER,         /* the atomic's pointer does not resolve to device (StorageBuffer) / threadgroup (Workgroup) memory of the spelled address space */
    LAGFX_ATOMIC_R_TYPE,            /* the module has no scalar type of the atomic's element type */
    LAGFX_ATOMIC_R_HELPER,          /* the atomic sits in a helper function whose call could not be wired (unregistered signature, e.g. a device-pointer parameter, or an argument type mismatch): the call would degrade to an undefined value */
    LAGFX_ATOMIC_R_COUNT
} lagfx_atomic_reason_t;
typedef struct { uint64_t modules, translated, refused, modules_rejected; uint64_t reason[LAGFX_ATOMIC_R_COUNT]; uint64_t lenient_accepted, cas_synthetic; } lagfx_atomic_totals_t;
uint32_t lagfx_air2spv_last_unsupported_atomic(char *first_name, size_t cap);
unsigned lagfx_air2spv_last_atomic_reason(void);
const char *lagfx_air2spv_atomic_reason_name(unsigned reason);
void lagfx_air2spv_atomic_totals(lagfx_atomic_totals_t *out);
/* Int64 narrowing for devices without shaderInt64 (process-wide, default off): every LLVM i64 is translated as a 32-bit integer (index / address arithmetic); a module with genuine
 * 64-bit data (64-bit load / store / atomic / bit-count helpers) is refused (LAGFX_ERR_PROTOCOL). lagfx_air2spv_last_narrowed_int64(): the last translation of this thread narrowed an i64. */
void lagfx_air2spv_set_narrow_int64(int on);
/* this thread only: translate with 64-bit integers kept wide even while the global narrowing mode is on (the caller then lowers them with lagfx_spv_lower_int64) */
void lagfx_air2spv_force_wide_int64_thread(int on);
/* re-install the narrowing refusal reason of the failed first attempt after a retry (so callers still see why narrowing was refused) */
void lagfx_air2spv_restore_narrow_refusal(const char *why);
bool lagfx_air2spv_last_narrowed_int64(void);
const char *lagfx_air2spv_last_narrow_refusal(void);   /* why the last translation was refused while narrowing ("" if it was not) */
void lagfx_air2spv_set_atomic_lenient(int mode);   /* tests: 1 / 0 force lenient on / off, -1 back to the LAGFX_ATOMIC_LENIENT environment (default off) */

/* Unlowered-call ledger (integ7b). A CALL that no lowering handled does NOT fail the translation by default: a non-void call is bound to a typed OpUndef (so value numbering stays
 * in sync) and a void call is dropped. Every such call site is counted here by callee name (process-wide, always on, cheap) so that a silent undef is visible:
 *   lagfx_air2spv_unlowered_calls        copy up to `cap` ledger rows (name, call sites, translations that contained it, void flag); returns the number of rows (may exceed cap)
 *   lagfx_air2spv_unlowered_reset        clear the ledger (tests / scans)
 *   lagfx_air2spv_last_unlowered         the calling thread's last translation: number of non-void unlowered calls named air.* / llvm.* (typed undef) and the first one's name
 * Environment (read at every translation, default OFF, default behaviour unchanged):
 *   LAGFX_LOG_UNLOWERED=1       one stderr line "g7-xlate-unlowered: <name> (<non-void: typed undef | void: dropped>)" per call site
 *   LAGFX_STRICT_INTRINSICS=1   a translation containing a non-void unlowered air.* / llvm.* call FAILS (LAGFX_ERR_PROTOCOL) with a warning naming the first such call
 * Default refusal (integ7b): a non-void unlowered `llvm.*` call (never air.*) fails the translation BY NAME; LAGFX_ALLOW_UNLOWERED_LLVM=1 restores the typed undef (the old behaviour). */
#define LAGFX_UNLOWERED_NAME_MAX 96
typedef struct { char name[LAGFX_UNLOWERED_NAME_MAX]; uint64_t sites, modules; uint8_t is_void; } lagfx_unlowered_call_t;
size_t lagfx_air2spv_unlowered_calls(lagfx_unlowered_call_t *out, size_t cap);
void lagfx_air2spv_unlowered_reset(void);
uint32_t lagfx_air2spv_last_unlowered(char *first_name, size_t cap);
uint64_t lagfx_air2spv_unlowered_refused(void);
bool lagfx_air2spv_last_unlowered_refused(void);   /* the calling thread's last translation was refused because of an unlowered call (translate.c then must NOT fall back to a reference stub) */   /* process-wide: translations refused because of an unlowered non-void call */

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
