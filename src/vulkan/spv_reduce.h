/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — DIAGNOSTIC SPIR-V reduction rewrites for the pipeline-reduce ladder (src/vulkan/pipeline_reduce.c). Pure C, no Vulkan headers, NEVER used by the
 * normal pipeline path. Every rewrite works on a COPY of a fragment module and is built to keep the module spirv-val valid (verified offline on the real failing modules, see
 * tests/pipeline-reduce-unit.c); when a rewrite cannot be done safely it reports "not applicable" instead of emitting a doubtful module.
 *
 * One recipe = a set of independent changes applied in one walk:
 *   stub[k]        function k (k = index among the module's OpFunction, the entry point excluded) keeps its signature but its body becomes `return OpConstantNull` (OpReturn for void)
 *   undef_*        OpUndef -> OpConstantNull (by type class, by ordinal, and by USE class: only the operands of SHUF / INSDYN / CCOMP / CINS / ... instructions)
 *   SW_*           OpSwitch -> OpBranch to the default (or to the most frequent case target); the OpSelectionMerge before it is dropped
 *   LP_*           every loop runs ONE trip: the back-edge becomes OpBranchConditional(false, header, merge)
 *   SMP_NULL_*     OpImageSample* / Fetch / Gather -> OpCopyObject(OpConstantNull)       SMP_LOD0  OpImageSample*ImplicitLod -> ExplicitLod Lod 0.0
 *   DERIV_NULL     OpDPdx.. OpFwidth.. -> null    READ_NULL  OpImageRead -> null    IA_DROP  the input-attachment variables and their loads disappear
 *   CUT            the entry function is cut at the first block (index >= cut_blocks) that is outside every structured construct: OpReturn there; the rest is deleted
 *   MIN_ENTRY      the entry function writes OpConstantNull to every Output variable and returns (the interface and every global declaration stay)
*   DYN_CONST      OpVectorExtractDynamic / InsertDynamic whose index folds to a constant -> OpCompositeExtract / Insert (value-preserving);  DYN_SELECT  the rest become OpSelect chains
 *   SSBO_SHRINK    OpTypeArray with >= 1024 elements -> 16 elements;  SSBO_RT  such an array that is only ever the LAST member of StorageBuffer structs becomes an OpTypeRuntimeArray
 *   PRUNE          (implied by CUT / MIN_ENTRY) functions no longer called are removed
 *   MIN_LOADS      (with MIN_ENTRY) the minimal entry also OpLoads the Location inputs selected by input_sel (and the BuiltIn inputs selected by blt_sel); the loaded values stay unused
 *   MIN_WIRE       (with MIN_ENTRY) the same loads, and every float32 Output variable receives the sum of them (chunks of the loaded scalars), i.e. the inputs are wired straight to the outputs
 *   CXS_FOLD       the translator's constexpr-sampler variables (OpName "lagfx.cxs.*") are removed and their loads read the sampler variable with the highest binding that is NOT a constexpr one
 *   SSA            (LRD_SSA*) the finished module goes through lagfx_spv_ssa_promote (src/vulkan/spv_ssa.h; meaning-preserving) BEFORE the POST passes below
 *   POST passes (run on the finished module, any combination, each independent; all keep spirv-val validity and the observable behaviour of the module):
 *     PRUNE_BLK    blocks unreachable from the function's first block are deleted (a block still named as a merge / continue target of a reachable construct stays as a one-instruction stub)
 *     PRUNE_VAR    Function variables that no instruction refers to are deleted
 *     PRUNE_WOVAR  Function variables that are only ever written (never loaded, never passed on) are deleted together with their stores and access chains
 *     MEM_STORE    stores (and copies) INTO such write-only Function variables are deleted, the variable and its access chains stay
 *     MEM_LOAD     OpLoad (not from an Input variable) whose result is unused, and access chains whose result is unused, are deleted, repeated until nothing more goes
 *     MEM_LOAD_IN  as MEM_LOAD, and the unused loads of Input variables too
 * Each is labelled in the ladder log as meaning-preserving (an OpUndef may take any value, zero is one) or "diagnostic: meaning changed".
 */
#ifndef LIBAPPLEGFX_VULKAN_SPV_REDUCE_H
#define LIBAPPLEGFX_VULKAN_SPV_REDUCE_H

#include <stddef.h>
#include <stdint.h>

#define LAGFX_RED_MAXFN 128
#define LAGFX_RED_MAXUNDEF 32

enum {
    LRD_SW_E = 1u << 0, LRD_SW_H = 1u << 1, LRD_LP_E = 1u << 2, LRD_LP_H = 1u << 3, LRD_SMP_NULL_E = 1u << 4, LRD_SMP_NULL_H = 1u << 5, LRD_SMP_LOD0 = 1u << 6,
    LRD_DERIV_NULL = 1u << 7, LRD_READ_NULL = 1u << 8, LRD_IA_DROP = 1u << 9, LRD_CUT = 1u << 10, LRD_MIN_ENTRY = 1u << 11, LRD_SWC_E = 1u << 12, LRD_SWC_H = 1u << 13,
    LRD_SSBO_SHRINK = 1u << 14, LRD_SSBO_RT = 1u << 15, LRD_DYN_CONST = 1u << 16, LRD_DYN_SELECT = 1u << 17, LRD_INPUT_CONST = 1u << 18,
    LRD_PRUNE_BLK = 1u << 19, LRD_PRUNE_VAR = 1u << 20, LRD_PRUNE_WOVAR = 1u << 21, LRD_MEM_STORE = 1u << 22, LRD_MEM_LOAD = 1u << 23, LRD_MEM_LOAD_IN = 1u << 24,
    LRD_MIN_LOADS = 1u << 25, LRD_MIN_WIRE = 1u << 26, LRD_CXS_FOLD = 1u << 27,
    LRD_NANMAX = 1u << 28,   /* D2 candidate: GLSL.std.450 FMin / FMax / FClamp -> NMin / NMax / NClamp (changes meaning only for NaN operands) */
    LRD_POST_MASK = LRD_PRUNE_BLK | LRD_PRUNE_VAR | LRD_PRUNE_WOVAR | LRD_MEM_STORE | LRD_MEM_LOAD | LRD_MEM_LOAD_IN,
};
/* integ6: g7-ssa wanted bits 28..31 for its four SSA flags, g7-audit-fix2 had already taken bit 28 for LRD_NANMAX. The mask is therefore 64 bit (lagfx_spv_reduce_t.bits, the B locals of
 * spv_reduce.c, the SSA recipe table); the SSA flags live above bit 28 and are macros (an enum constant cannot exceed int). */
#define LRD_SSA      (1ull << 29)
#define LRD_SSA_DCE  (1ull << 30)
#define LRD_SSA_DCEIN (1ull << 31)
#define LRD_SSA_AGG  (1ull << 32)   /* spv_ssa.c: Function variables -> SSA values (+ dead-value cleanup / Input loads / aggregates); runs before the POST passes */
#define LRD_SSA_MASK (LRD_SSA | LRD_SSA_DCE | LRD_SSA_DCEIN | LRD_SSA_AGG)
enum { LRD_UC_NUM = 1, LRD_UC_STRUCT = 2, LRD_UC_PTR = 4, LRD_UC_COMPOSITE = 8 };                                   /* OpUndef type classes */
enum { LRD_UU_SHUF = 1, LRD_UU_INSDYN = 2, LRD_UU_CCOMP = 4, LRD_UU_CINS = 8, LRD_UU_STORE = 16, LRD_UU_CALLARG = 32, LRD_UU_CCONS = 64, LRD_UU_EXTDYN = 128 };   /* use classes */

typedef struct {
    uint64_t bits;
    uint32_t undef_class;      /* LRD_UC_* : every OpUndef of these type classes -> null */
    uint32_t undef_sel;        /* bit k: the k-th OpUndef of the module (declaration order) -> null */
    uint32_t use_class;        /* LRD_UU_* : the OpUndef OPERANDS of these instruction kinds -> a null of the same type */
    uint32_t cut_blocks;       /* LRD_CUT */
    uint32_t ssbo_len;         /* LRD_SSBO_SHRINK: new element count (0 = 16) */
    uint32_t input_sel;        /* LRD_INPUT_CONST: bit k = loads of the k-th Location-decorated Input variable become OpConstantNull; LRD_MIN_LOADS / MIN_WIRE: bit k = that input is loaded */
    uint32_t blt_sel;          /* LRD_MIN_LOADS / MIN_WIRE: bit k = the k-th BuiltIn Input variable (declaration order) is loaded */
    uint32_t ssa_maxphi;       /* LRD_SSA*: a local whose promotion would need an OpPhi with more than this many incoming edges stays in memory (0 = no limit) */
    uint32_t dyn_top;          /* LRD_DYN_*: only the dyn_top functions with the most dynamic vector accesses (0 = every function) */
    uint8_t  stub[LAGFX_RED_MAXFN];   /* by function index */
} lagfx_red_recipe_t;

typedef struct {
    int      ok;               /* 1 = module parsed and analysed */
    uint32_t version, bound, ninst, words;
    uint32_t nfn, entry;       /* number of functions; index of the entry point's function */
    struct { uint32_t id, ninst, nblocks, nsamp, nread, nderiv, nloop, nswitch, ncall, ret_op; } fn[LAGFX_RED_MAXFN];
    uint32_t nundef;
    struct { uint32_t id, type, cls, nuse; } undef[LAGFX_RED_MAXUNDEF];
    uint32_t nia, nphi, nswitch, nloop, nselmerge, nbranch, ncoopmat_cap, nfn_uncalled;
    /* constants: unique OpConstant by type (index: f16 f32 f64 i8 i16 i32 i64 other), unique by (type,value), uses inside function bodies */
    struct { uint32_t unique, distinct_values, uses; } k[8];
    uint32_t kbool, kcomposite, kspec, knull, kundef_global, kuses_total;
    uint32_t entry_blocks;
    uint32_t nfnvar, nvar_total, nstore, nload, nlabel, nfconvert, nptrchain, nssbo_big;
    uint32_t dyn_total, dyn_const, dyn_truly, dyn_fn[LAGFX_RED_MAXFN];
    struct { uint32_t ncomp, width, kind; uint32_t n; } dyn_vec[8];   /* by vector type: kind 0 float 1 int */
    uint32_t dyn_idx_const, dyn_idx_load, dyn_idx_other;
    struct { uint32_t op, n; } i64_op[10];
    uint32_t i64_total;
    uint32_t ncollide;         /* (set, binding) pairs shared by variables of DIFFERENT descriptor kinds (buffer views of one buffer share a binding on purpose and are not counted) */
    struct { uint32_t set, binding, ka, kb; } collide[4];   /* kinds: 1 sampler 2 image 3 input attachment 4 buffer 5 other */
    uint32_t nblt;             /* BuiltIn Input variables (declaration order) */
    struct { uint32_t id, builtin; } blt[8];
    uint32_t ncxs;             /* sampler variables named lagfx.cxs.* (translator constexpr samplers) */
    struct { uint32_t id, binding, nuse; } cxs[8];
    uint32_t nin;              /* Location-decorated Input variables (declaration order) */
    struct { uint32_t id, loc, comp, ncomp, width, kind, flat; } in[32];
    uint32_t nkill, nbit_small, nbit_total, nstruct_load, nstruct_store, ncopymem, nprivate_vecarr, nprivate_arr_max;
    uint32_t ssbo_arr_len, ssbo_arr_stride, ssbo_offsets[8], ssbo_noff;   /* the first array with >= 1024 elements: length, ArrayStride, Offsets of its struct's members */
} lagfx_red_info_t;

/* 1 = analysed, 0 = not a module this code can handle (malformed, > LAGFX_RED_MAXFN functions). */
int lagfx_red_analyze(const uint32_t *w, size_t words, lagfx_red_info_t *info);
/* Applies a recipe to a COPY. Returns 1 = changed (*out malloc'd, caller frees), 0 = not applicable (info says why; *out NULL), -1 = malformed / out of memory.
 * `info` (may be NULL) receives a short description of what changed (counts). */
int lagfx_red_apply(const uint32_t *w, size_t words, const lagfx_red_recipe_t *r, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* A vertex stage whose Output variables match every Location the fragment module reads (same type, Flat for integers; builtin inputs need no VS output) and that writes
 * gl_Position and constant zero to each. 1 = made, 0 = a FS input type cannot be mirrored (info says which), -1 = malformed / OOM. `missing`(may be NULL) receives the number of
 * Locations the FS reads. */
int lagfx_red_make_vs_for_fs(const uint32_t *fs, size_t words, uint32_t **out, size_t *out_words, char *info, size_t cap);
/* Cheap structural self-check of a module (id operands defined / below the bound); NOT a substitute for spirv-val. 1 = ok. */
int lagfx_red_selfcheck(const uint32_t *w, size_t words, char *why, size_t cap);
/* Count of instructions of a module (for the ladder log). */
uint32_t lagfx_red_count_inst(const uint32_t *w, size_t words);

#endif
