/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — the descriptor binding allocator / table (one set+binding -> one descriptor type)
 * src/air2spv/binding_alloc.h
 *
 * Copyright © 2026 Matthew Jackson
 *
 * ONE place decides which (set, binding) a descriptor lives at and whether a table of them is valid. It is used by
 *   - the AIR -> SPIR-V translator (every resource variable, the default sampler, the constexpr samplers, the input attachments and the compute imageblock
 *     images take their binding from lagfx_ba_alloc(); nothing is numbered by hand any more),
 *   - the descriptor-set layout builder (src/vulkan/descriptor_layout.c merges the stages through lagfx_ba_merge_modules() and refuses a table with a conflict),
 *   - the resolver / tools that derive a table from finished SPIR-V (lagfx_ba_import), and their diagnostics (lagfx_ba_describe),
 *   - the pipeline cache key (lagfx_ba_key: changes whenever a binding, a type, a stage set or an input-attachment index changes).
 *
 * Rules (a Vulkan descriptor set layout has exactly one VkDescriptorType per binding):
 *   - two descriptors on one (set, binding) must have the SAME kind. The same kind from two stages is legitimate sharing (stage flags are OR-ed); the same kind
 *     twice in one stage is the translator's byte-buffer views of one buffer (one descriptor, several Block types) and is accepted too;
 *   - two input attachments on one binding must read the same attachment (InputAttachmentIndex), two sampled images the same image dimensionality;
 *   - anything else is a CONFLICT: counted and recorded (first lagfx_ba_t.nconflict entries), never silently resolved (the old layout builder kept the first
 *     variable of a binding and dropped the rest without a word);
 *   - a table that does not fit is an OVERFLOW (counted), never truncated silently.
 * The binding NUMBER of an input attachment or an imageblock image has no meaning: they are identified by role_index (InputAttachmentIndex / render target), which the
 * reflector reports (lagfx_spv_binding_t.role_index).
 *
 * Pure C (no Vulkan): unit-tests on any host.
 */
#ifndef LAGFX_AIR2SPV_BINDING_ALLOC_H
#define LAGFX_AIR2SPV_BINDING_ALLOC_H

#include "spv_reflect.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGFX_BA_MAX 256u          /* distinct (set, binding) entries of one table */
#define LAGFX_BA_MAX_CONFLICTS 8u  /* conflicts recorded in detail (nconflict counts all) */

#define LAGFX_BA_STAGE_VERTEX   1u
#define LAGFX_BA_STAGE_FRAGMENT 2u
#define LAGFX_BA_STAGE_COMPUTE  4u

typedef struct {
    uint32_t                 set, binding;
    lagfx_spv_binding_kind_t kind;
    uint32_t                 stages;       /* LAGFX_BA_STAGE_* bits of the stages that declare it */
    uint32_t                 image_dim;    /* SAMPLED_IMAGE / STORAGE_IMAGE: OpTypeImage Dim (0 = unknown) */
    uint32_t                 role_index;   /* LAGFX_SPV_NO_ROLE or the input-attachment index / imageblock render target */
    uint8_t                  may_write;    /* STORAGE_BUFFER: some declaring variable may write */
} lagfx_ba_entry_t;

enum { LAGFX_BA_REASON_KIND = 1, LAGFX_BA_REASON_ROLE = 2, LAGFX_BA_REASON_DIM = 3 };
typedef struct { uint32_t reason, set, binding; lagfx_spv_binding_kind_t kind_a, kind_b; uint32_t role_a, role_b; } lagfx_ba_conflict_t;

typedef struct {
    lagfx_ba_entry_t    e[LAGFX_BA_MAX];   /* in claim order until lagfx_ba_sort() */
    uint32_t            n;
    uint32_t            nconflict;         /* claims refused because they disagree with an entry already there */
    uint32_t            noverflow;         /* claims / reflected variables that did not fit */
    lagfx_ba_conflict_t conflict[LAGFX_BA_MAX_CONFLICTS];
} lagfx_ba_t;

enum { LAGFX_BA_OK = 0, LAGFX_BA_SHARED = 1, LAGFX_BA_CONFLICT = -1, LAGFX_BA_FULL = -2 };

void lagfx_ba_init(lagfx_ba_t *a);

/* Takes the lowest binding of `set` that nothing holds and records the descriptor there; *binding gets it. LAGFX_BA_FULL (and noverflow++) when the table is full. */
int lagfx_ba_alloc(lagfx_ba_t *a, uint32_t set, lagfx_spv_binding_kind_t kind, uint32_t stages, uint32_t image_dim, uint32_t role_index, uint32_t *binding);

/* Records a descriptor at a given (set, binding). LAGFX_BA_OK = new entry; LAGFX_BA_SHARED = it joined a compatible entry (stages OR-ed); LAGFX_BA_CONFLICT = an
 * incompatible entry holds the binding (nothing recorded, nconflict++, detail kept); LAGFX_BA_FULL = table full (noverflow++). */
int lagfx_ba_claim(lagfx_ba_t *a, const lagfx_ba_entry_t *want);

/* True when no conflict and no overflow happened. */
int lagfx_ba_ok(const lagfx_ba_t *a);

/* claims every reflected variable of one stage (n may exceed the entries really present: pass the count reflect returned, capped by the caller's buffer); returns the number of
 * claims that conflicted */
uint32_t lagfx_ba_import(lagfx_ba_t *a, const lagfx_spv_binding_t *refl, size_t n, uint32_t stages);

/* Reflects every module (stage bit from its entry point's execution model) and claims all of it. A module with more variables than LAGFX_BA_MAX is an overflow. Returns
 * lagfx_ba_ok(). `a` should be freshly initialised; the entries end up sorted by (set, binding). */
int lagfx_ba_merge_modules(lagfx_ba_t *a, const uint8_t *const *spv, const size_t *len, uint32_t n_modules);

/* Sorts the entries by (set, binding). */
void lagfx_ba_sort(lagfx_ba_t *a);

/* FNV-1a 64 over the sorted table (set, binding, kind, stages, image_dim, role_index) + the conflict / overflow counters: any change of the layout the table stands for changes it. */
uint64_t lagfx_ba_key(const lagfx_ba_t *a);

/* The first binding a stage placed AFTER a stage with these SPIR-V bytes can use: max(min_base, highest set-0 binding of the module + 1). */
uint32_t lagfx_ba_stage_base(const uint8_t *prev_stage_spv, size_t len, uint32_t min_base);

const char *lagfx_ba_kind_name(lagfx_spv_binding_kind_t k);

/* Text table: one line per entry ("set S binding B KIND stages=VF role=R") and one per recorded conflict. Returns the length it needs (snprintf semantics). */
size_t lagfx_ba_describe(const lagfx_ba_t *a, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_AIR2SPV_BINDING_ALLOC_H */
