/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — OPT-IN local-variable SSA promotion for translated SPIR-V (Function-storage variables -> values + OpPhi), plus the dead-value cleanup that goes with it.
 * Pure C, no Vulkan headers. Applied to a finished module (post-translation); the translator's own output is never touched, and without LAGFX_SPV_SSA=1 nothing in this file runs.
 *
 * Why it exists: the Adreno 840 driver rejects the translated fragment function A2Xghfc (c65cc4d1ec02658b; 1944 Function variables in the entry, 473 blocks, 3171 in the module) with
 * vkCreateGraphicsPipelines = -13. spirv-opt cannot be used to reduce it with the usual recipe: its dead-code pass refuses a module that declares VariablePointersStorageBuffer
 * (which the translator needs for its pointer parameters), so the stores that --ssa-rewrite leaves behind are never removed (docs/research/ssa-promotion.md). This file does
 * the same job (own implementation; Cytron et al. pruned SSA, Cooper-Harvey-Kennedy dominators; no code from any other project) and removes the dead stores and values itself.
 *
 * What is promoted: a Function variable declared in the first block of its function whose every use is a plain (non-volatile) OpLoad of it or OpStore into it, and whose type is
 * a bool / int / float / vector / matrix (arrays and structs only with LSSA_AGG). Variables whose address escapes (OpAccessChain, OpFunctionCall argument, OpCopyMemory, ...) stay in memory.
 * What is NOT done: no inlining, no scalar replacement of aggregates, no change to any global variable, interface, binding, decoration other than the ones that named a deleted id.
 * A module with an opcode this file does not know in a function body, a 64-bit OpSwitch selector or a malformed structure is left untouched (return 0, `why` says what).
 */
#ifndef LIBAPPLEGFX_VULKAN_SPV_SSA_H
#define LIBAPPLEGFX_VULKAN_SPV_SSA_H

#include <stddef.h>
#include <stdint.h>

enum {
    LSSA_DCE       = 1u << 0,   /* also delete unused side-effect-free values (ALU, composite, convert, access chains, loads of non-Input memory, image samples, unused variables) */
    LSSA_DCE_INPUT = 1u << 1,   /* with LSSA_DCE: unused loads of Input variables go too (the interface declaration stays) */
    LSSA_AGG       = 1u << 2,   /* promote array / struct typed variables as well */
};
/* flags bits 16..31 = N: a variable whose promotion would need an OpPhi with more than N incoming edges is NOT promoted (0 = no limit): keeps the giant merge-block phis out of the module */
#define LSSA_MAXPHI(n) ((unsigned)(n) << 16)

typedef struct {
    uint32_t fns, vars, promoted, escaped, skipped_agg, skipped_type, skipped_wide;
    uint32_t phis_placed, phis_trivial, phis_dead, phis_kept, undefs;
    uint32_t loads_removed, stores_removed, decl_removed, dce_removed, decos_dropped;
    uint32_t inst_before, inst_after, words_before, words_after;
} lagfx_ssa_stats_t;

/* 1 = changed (*out malloc'd, caller frees), 0 = not applicable / nothing to do (why), -1 = out of memory or malformed. */
int lagfx_spv_ssa_promote(const uint32_t *w, size_t words, unsigned flags, uint32_t **out, size_t *out_words, lagfx_ssa_stats_t *st, char *why, size_t cap);

/* Process-wide counters of the env hook below. */
typedef struct {
    uint64_t modules, changed, unchanged, failed, fallback;   /* fallback = the promoted module did not pass the cheap self-check and the ORIGINAL was kept */
    uint64_t vars, promoted, phis, loads_removed, stores_removed, dce_removed, words_before, words_after;
} lagfx_ssa_counters_t;
void lagfx_spv_ssa_counters(lagfx_ssa_counters_t *c);

/* LAGFX_SPV_SSA=1 (default OFF; =2: also LSSA_DCE; =3: DCE incl. Input loads; =4: DCE + aggregates). 0 when unset or "0". */
unsigned lagfx_spv_ssa_env_mode(void);
/* Post-translation hook: if the mode is on, replaces *spv (malloc'd, `*sz` bytes) with the promoted module; any failure keeps the original untouched. `what` is a log label. 1 = replaced. */
int lagfx_spv_ssa_apply_env(uint8_t **spv, size_t *sz, const char *what);

#endif
