/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — DIAGNOSTIC SPIR-V rewrites for the pipeline repro's variant matrix (src/vulkan/pipeline_repro.c).
 * NEVER used by the normal pipeline path. Each rewrite works on a COPY and reports how many places it changed; 0 = the variant does not apply to this module.
 * Pure C, no Vulkan headers.
 */
#ifndef LIBAPPLEGFX_VULKAN_SPV_VARIANTS_H
#define LIBAPPLEGFX_VULKAN_SPV_VARIANTS_H

#include <stddef.h>
#include <stdint.h>

/* Every rewrite: in = SPIR-V words; *out = malloc'd result (caller frees; NULL when 0 changes or on error); returns the number of changes, or -1 on a malformed module / OOM.
 * `info` (may be NULL, cap bytes) receives a short human-readable note. */

/* (e) 64-bit OpVectorExtractDynamic / OpVectorInsertDynamic index -> OpUConvert to 32 bit first. Value-preserving for an in-range index (out-of-range is undefined in SPIR-V anyway). */
int lagfx_spv_variant_index32(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (f) drops the Int8 / Int16 / Float16 / Int64 capability declarations (and the 8/16-bit storage capabilities that depend on them) that NOTHING in the module uses (no OpType of that
 * width). A capability that is in use is kept and named in `info`: it cannot be stripped without changing the program. */
int lagfx_spv_variant_strip_caps(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (g) OpUndef of a composite type (vector / matrix / array / struct) -> OpConstantNull (hoisted to the module scope when it was inside a function). CHANGES MEANING: undefined -> zero. */
int lagfx_spv_variant_undef_composites(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);

/* (h) ONLY the struct- and pointer-typed OpUndef -> OpConstantNull (hoisted like (g)); vector / scalar OpUndef stay. CHANGES MEANING for a struct undef that is consumed (undefined -> zero). */
int lagfx_spv_variant_undef_struct_ptr(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (i) drops every OpUndef that nothing references (value-preserving; isolates "an unused OpUndef of a type the driver may dislike" from everything else). */
int lagfx_spv_variant_drop_dead_undef(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);

/* (j) ONLY the struct-typed OpUndef -> OpConstantNull (no pointer): (h) minus the pointer-typed ones. CHANGES MEANING for a consumed struct undef. */
int lagfx_spv_variant_undef_struct(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);

/* (k) ONLY the OpReturnValue operands that are an OpUndef -> a fresh OpConstantNull (the OpUndef and its other uses stay). CHANGES MEANING for a returned undef (undefined -> zero). */
int lagfx_spv_variant_undef_returns(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (n) D2 candidate: GLSL.std.450 FMin / FMax / FClamp -> NMin / NMax / NClamp (a NaN operand yields the other operand). CHANGES MEANING for NaN inputs only. The translator has the same switch at
 * the source (LAGFX_SPV_NANMAX=1, opt-in, default off); this rewrite applies it to an already translated module for the repro / reduce ladders. */
int lagfx_spv_variant_nanmax(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (M2) SPIR-V 1.4 -> 1.3 (version word, OpEntryPoint interface lists trimmed to Input / Output variables, scalar-condition OpSelect of a vector rewritten with a bool-vector condition) when the module uses no other 1.4-only feature (OpCopyLogical, OpPtrEqual/NotEqual/Diff, aggregate OpSelect, ...); 0 = refused / not 1.4 (reason in info),
 * otherwise 1 + the number of interface ids dropped. Value-preserving. */
int lagfx_spv_variant_downgrade13(const uint32_t *in, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* (M3b / M4a) input-attachment renumbering read from the module (InputAttachmentIndex k marks an input attachment). mode 0: input attachments -> Binding k, every other descriptor variable
 * shifted up by `nia`; mode 1: input attachments -> DescriptorSet 1, Binding k. The pipeline layout must be renumbered the same way. Value-preserving then. */
int lagfx_spv_variant_ia_rebind(const uint32_t *in, size_t words, unsigned mode, uint32_t nia, uint32_t **out, size_t *out_words, char *info, size_t cap);

/* Minimal valid stage stubs ("main"; vertex writes gl_Position = (0,0,0,1) and has no inputs / outputs; fragment writes nothing). */
extern const uint32_t lagfx_stub_vs_spv[67], lagfx_stub_fs_spv[32];

#endif
