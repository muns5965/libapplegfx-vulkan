/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — SPIR-V descriptor-binding reflection
 * src/air2spv/spv_reflect.h
 *
 * Copyright © 2026 Matthew Jackson
 *
 * Scans a translated SPIR-V module for the descriptor bindings it declares
 * (set + binding + kind) so the host can build a matching
 * VkDescriptorSetLayout for the pipeline. The air2spv translator emits
 * texture/sampler resources as UniformConstant OpTypeImage / OpTypeSampler
 * and [[buffer(n)]] args as StorageBuffer Block structs, all in
 * DescriptorSet 0 with sequential bindings; this recovers that layout from
 * the finished blob without re-running translation. Pure (no Vulkan
 * dependency) so it unit-tests on any host.
 */
#ifndef LAGFX_AIR2SPV_SPV_REFLECT_H
#define LAGFX_AIR2SPV_SPV_REFLECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAGFX_SPV_BINDING_UNKNOWN        = 0,
    LAGFX_SPV_BINDING_SAMPLED_IMAGE  = 1, /* OpTypeImage, Sampled=1  -> VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE */
    LAGFX_SPV_BINDING_SAMPLER        = 2, /* OpTypeSampler           -> VK_DESCRIPTOR_TYPE_SAMPLER */
    LAGFX_SPV_BINDING_STORAGE_BUFFER = 3, /* StorageBuffer Block     -> VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */
    LAGFX_SPV_BINDING_INPUT_ATTACHMENT = 4, /* OpTypeImage Dim=SubpassData -> VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT (framebuffer fetch) */
    LAGFX_SPV_BINDING_STORAGE_IMAGE  = 5, /* OpTypeImage Sampled=2   -> VK_DESCRIPTOR_TYPE_STORAGE_IMAGE (texture2d<T, access::write>) */
} lagfx_spv_binding_kind_t;

typedef struct {
    uint32_t                 set;
    uint32_t                 binding;
    lagfx_spv_binding_kind_t kind;
    uint8_t                  image_dim;  /* SAMPLED_IMAGE / STORAGE_IMAGE only: the OpTypeImage Dim operand (1 = Dim2D, 2 = Dim3D, 3 = Cube; 0 = not an image) */
    uint8_t                  may_write;  /* STORAGE_BUFFER only: 0 = the module PROVABLY never writes through it (every use of the variable / its access chains is a load or an
                                          * array length), 1 = a store / atomic / copy / call / phi / pointer escape was found (or the module is too large to analyse).
                                          * Several variables of one binding (byte-buffer views) are OR-ed. Not set for other kinds. */
    uint8_t                  sampled_class; /* INPUT_ATTACHMENT / SAMPLED_IMAGE / STORAGE_IMAGE: numeric class of the OpTypeImage Sampled Type (0 float, 1 unsigned int, 2 signed int); 0 otherwise */
    uint32_t                 role_index; /* INPUT_ATTACHMENT: the InputAttachmentIndex decoration = the colour attachment the variable reads (the BINDING NUMBER carries no meaning);
                                          * STORAGE_IMAGE named "lagfx.imageblock.<n>" (translator imageblock image): n = the colour attachment; otherwise LAGFX_SPV_NO_ROLE. */
} lagfx_spv_binding_t;
#define LAGFX_SPV_NO_ROLE 0xffffffffu
/* OpName prefix of a translator imageblock storage image (compute / tile kernels): "lagfx.imageblock.<colour attachment>" */
#define LAGFX_IMAGEBLOCK_NAME_PREFIX "lagfx.imageblock."

/* Execution model of the module's first OpEntryPoint (0 Vertex, 4 Fragment, 5 GLCompute), 0xffffffff when there is none / the blob is not SPIR-V. */
uint32_t lagfx_spv_reflect_exec_model(const uint8_t *spv, size_t spv_len);

/* Reflect descriptor bindings from a SPIR-V blob. Writes up to `cap`
 * entries to `out` (sorted by set then binding) and returns the TOTAL
 * number found (which may exceed `cap`; callers should size `cap` to the
 * device's max or check the return). 0 = no descriptor resources (e.g. a
 * resource-free colour shader -> an empty pipeline layout is correct). */
size_t lagfx_spv_reflect_bindings(const uint8_t *spv, size_t spv_len,
                                  lagfx_spv_binding_t *out, size_t cap);

/* A constexpr sampler of the module: the Binding of its sampler variable and the raw i64 sampler word of the AIR (decode with lagfx_constexpr_sampler_decode). The
 * translator names such variables "lagfx.cxs.<16 hex>" (OpName; LAGFX_CXS_NAME_PREFIX). Returns the TOTAL found; writes up to `cap` (ascending id order). */
typedef struct { uint32_t binding; uint64_t raw; } lagfx_spv_cxs_t;
size_t lagfx_spv_reflect_constexpr_samplers(const uint8_t *spv, size_t spv_len, lagfx_spv_cxs_t *out, size_t cap);

/* A vertex stage-in attribute: a Location-decorated Input variable in the
 * VERTEX shader (the translator emits one per float/vector arg that is not a
 * resource or a builtin). `components` is 1..4 (scalar/vec2/vec3/vec4 float)
 * → maps to VK_FORMAT_R32[G32[B32[A32]]]_SFLOAT. The host needs these to build
 * a non-empty VkPipelineVertexInputState + bind the guest's vertex buffer;
 * without them the vertex shader reads unbound attributes → 0 positions →
 * degenerate draws → black. */
typedef struct {
    uint32_t location;
    uint32_t components;  /* 1..4 */
    /* Source in the vertex buffer, when the translator knew it (a vertex fetch wrapper's constant-offset load; OpName "lagfx.vin.o<off>[.s<bytes>][.b<slot>]").
     * has_src_offset = 0 means UNKNOWN (offset 0 is a valid offset, so the flag is separate). Other inputs/modules: all three flags 0. */
    uint8_t  has_src_offset, has_src_bytes, has_src_buffer;
    uint8_t  src_unfed;   /* 1 = the vertex fetch wrapper passes undef for this input (OpName "lagfx.vin.u"): no buffer source exists */
    uint32_t src_offset;  /* byte offset inside the per-vertex record */
    uint32_t src_bytes;   /* store size of the loaded value */
    uint32_t src_buffer;  /* Metal buffer slot */
} lagfx_spv_vertex_input_t;

/* Reflect vertex stage-in attributes (Location-decorated Input vars) from a
 * translated VERTEX SPIR-V blob. Writes up to `cap` entries (sorted by
 * location) and returns the total found. 0 = the vertex shader uses no
 * stage-in attributes (purely vertex_id / builtin-driven). */
size_t lagfx_spv_reflect_vertex_inputs(const uint8_t *spv, size_t spv_len,
                                       lagfx_spv_vertex_input_t *out, size_t cap);

/* True if the SPIR-V contains any image sample/fetch/read instruction (the
 * shader USES a texture). If true but reflection found no SAMPLED_IMAGE
 * binding, the module is INCONSISTENT (translator dropped the texture
 * binding) → must not be drawn or it samples an unbound descriptor → crash. */
bool lagfx_spv_has_image_sample(const uint8_t *spv, size_t spv_len);

/* In-place ADD `base` to every `OpDecorate <id> Binding <n>` literal in the
 * SPIR-V module (DescriptorSet decorations are left untouched). Used to give
 * the FRAGMENT stage a disjoint binding range from the VERTEX stage: the
 * translator emits both stages' resources in descriptor set 0 starting at
 * binding 0, so vertex `[[buffer(0)]]` and fragment `[[texture(0)]]` collide
 * at set0/binding0 — and Vulkan cannot hold two descriptorTypes at one
 * (set,binding), so the merged pipeline layout drops one and the shader
 * samples/loads an unbound descriptor. Offsetting the fragment stage by a
 * fixed base (e.g. 16) makes the merged set-0 layout unambiguous: bindings
 * < base are vertex resources, >= base are fragment resources. The draw site
 * demuxes on the same base. Mutates `spv` in place; safe to call once per
 * fragment module before vkCreateShaderModule + reflection. */
void lagfx_spv_offset_bindings(uint8_t *spv, size_t spv_len, uint32_t base);

/* Rewrites every OpImageSampleImplicitLod of the module into OpImageSampleExplicitLod with Lod 0.0 (an optional ConstOffset operand is kept). Needed
 * for a fragment shader that samples through a sampler created with unnormalizedCoordinates: Vulkan allows only explicit-LOD sampling there
 * (VUID-vkCmdDraw-None-08609 / the unnormalized-coordinates sampler rules). Returns 1 and a malloc'ed copy in *out, 0 when the module has no implicit-LOD
 * sample (nothing to change, *out untouched), -1 when it cannot be rewritten (sample with Bias / Grad / MinLod operands, no float32 type, malformed). */
int lagfx_spv_explicit_lod0(const uint8_t *spv, size_t spv_len, uint8_t **out, size_t *out_len);

/* Legacy whole-module rewrite (every OpImageSampleImplicitLod, whatever its sampler). Production no longer uses it (lagfx_spv_unnorm_rewrite works per use); kept for tests. */

/* Per-use rewrite for unnormalized-coordinates samplers (src/air2spv/spv_unnorm.c). `bindings` = the descriptor binding numbers (set 0) of the sampler variables that are
 * unnormalized in THIS draw. Only samples whose OpSampledImage sampler traces (OpLoad / access chain -> OpVariable -> Binding) to one of them are touched:
 *   OpImageSampleImplicitLod  -> OpImageSampleExplicitLod Lod 0.0 (Bias / MinLod dropped: Metal coord::pixel samples level 0 only); Const/Offset folded into the coordinate;
 *   OpImageSampleExplicitLod  -> unchanged (Lod / Grad are clamped by the sampler's lod range 0..0), except Const/Offset which is folded into the coordinate;
 *   OpImageGather / DrefGather -> unchanged (not covered by the Vulkan restrictions on OpImageSample*);
 *   Dref / Proj / sparse / OpImageQueryLod, or a 3D / cube / arrayed / multisampled image behind the sampled image -> UNSUPPORTED (Vulkan forbids them with an unnormalized sampler).
 * A sample whose sampler cannot be traced (function parameter, phi) counts as unresolved and is converted only when convert_unresolved is non-zero.
 * Returns 1 and a malloc'ed module in *out (NULL out = analysis only), 0 when no sample uses a listed sampler (or none needs a change: *out untouched), -1 when an unsupported use
 * exists (or malformed input). *rep (optional) is filled in every case. */
typedef struct {
    uint32_t samples_total;      /* OpImageSample* / Gather / Sparse / QueryLod instructions seen */
    uint32_t uses_unnorm;        /* of them: sampler traced to a listed binding */
    uint32_t uses_unresolved;    /* sampler could not be traced to a variable */
    uint32_t implicit_to_lod0;   /* ImplicitLod -> ExplicitLod 0.0 */
    uint32_t bias_dropped, minlod_dropped, offset_folded;
    uint32_t explicit_kept, grad_kept, gather_kept;
    uint32_t unsup_dref, unsup_proj, unsup_sparse, unsup_image_dim, unsup_other;
    uint32_t module_changed;     /* 1 when a rewritten module was produced */
} lagfx_spv_unnorm_report_t;
int lagfx_spv_unnorm_rewrite(const uint8_t *spv, size_t spv_len, const uint32_t *bindings, uint32_t nb, int convert_unresolved,
                             uint8_t **out, size_t *out_len, lagfx_spv_unnorm_report_t *rep);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_AIR2SPV_SPV_REFLECT_H */
