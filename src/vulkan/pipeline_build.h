/* SPDX-License-Identifier: MIT */
#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H

#include "libapplegfx-vulkan.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#else
/* Vulkan-disabled stub build: declare opaque handles so this header
 * still parses. Callers under LAGFX_HAVE_VULKAN see real Vulkan types. */
typedef void *VkDevice;
typedef void *VkShaderModule;
typedef void *VkPipeline;
typedef void *VkPipelineLayout;
typedef void *VkDescriptorSetLayout;
typedef int   VkFormat;
#endif

struct lagfx_vk_state;
/* OPTIONAL failure-bundle inputs (src/vulkan/pipeline_bundle.c). Only read when vkCreateGraphicsPipelines fails AND the bundle option is on (LAGFX_PIPEFAIL_BUNDLE_DIR, or
 * LAGFX_ARM64_DUMP=1 with a dump dir); never changes what is built. All pointers stay owned by the caller and must outlive the lagfx_pipeline_build() call. */
typedef struct {
    const struct lagfx_vk_state *vk;     /* the device the pipeline was built on (properties, enabled extensions / features); NULL = recorded as unknown */
    const uint8_t *vs_bc, *fs_bc;        /* the AIR bitcode both SPIR-V modules were translated from (NULL = not available) */
    size_t         vs_bc_bytes, fs_bc_bytes;
    const char    *vs_fn, *fs_fn;        /* Metal function names */
    const char    *origin;               /* free text: where the pipeline came from (e.g. "arm64 pso task=2 ref=30") */
} lagfx_pipeline_diag_t;

/**
 * MVP pipeline descriptor — fields hardcoded to match triangle.metallib
 * for first-pixel work. Will be extended in Stage 70b+ as wire-opcode
 * handlers populate more fields.
 */
typedef struct {
    VkShaderModule   vertex_shader;            /* required */
    VkShaderModule   fragment_shader;          /* required */
    VkPipelineLayout layout;                   /* required — Vulkan spec forbids VK_NULL_HANDLE */
    const char      *vertex_entry_point;       /* default: "main" if NULL */
    const char      *fragment_entry_point;     /* default: "main" if NULL */
    VkFormat         color_format;             /* default: VK_FORMAT_B8G8R8A8_UNORM */
    VkFormat         depth_format;             /* default: VK_FORMAT_UNDEFINED (no depth) */
    /* Vertex stage-in attributes (from lagfx_spv_reflect_vertex_inputs). When
     * n_vtx_inputs > 0, the pipeline gets a non-empty vertex-input state
     * (binding 0, tightly-packed R32..A32_SFLOAT) so the vertex shader's
     * stage-in reads come from the bound guest vertex buffer instead of an
     * unbound attribute (= 0 = degenerate positions = black). */
    uint8_t          vtx_in_loc[8];
    uint8_t          vtx_in_comp[8];            /* 1..4 components per attribute */
    uint8_t          n_vtx_inputs;
    /* Real per-vertex stride decoded from the PSO's MTLVertexDescriptor
     * (lagfx_parse_pso_vertex_stride). 0 = unknown → fall back to round8(sum).
     * The round8 heuristic is wrong for the CoreAnimation composites (real 48,
     * heuristic 24) and smears the geometry into horizontal bands. */
    uint32_t         vtx_stride;
    /* GOAL-M2z: real per-attribute {MTLVertexFormat, offset} from the PSO
     * descriptor (sorted by offset; n_pso_attrs 0 = not decoded -> tight-pack
     * SFLOAT fallback). Fixes the rgba8 color attr read as SFLOAT NaN. */
    uint32_t         pso_attr_fmt[8];
    uint32_t         pso_attr_off[8];
    uint8_t          n_pso_attrs;
    /* Primitive topology as VkPrimitiveTopology + 1; 0 = default (TRIANGLE_LIST), so every existing caller is unchanged. */
    uint8_t          topology_plus1;
    /* OPTIONAL diagnostics (zero = unknown): the SPIR-V the two modules were made from and the enabled device-capability mask. Only read when vkCreateGraphicsPipelines
     * fails (failure log line, per-module vkCreateShaderModule check); never changes what is built. The caller keeps the words alive across the call. */
    const uint32_t  *vertex_spv;   size_t vertex_spv_bytes;
    const uint32_t  *fragment_spv; size_t fragment_spv_bytes;
    uint32_t         caps_enabled;
    /* Colour attachment 0 blend state. blend_set == 0 keeps the fixed premultiplied source-over below (every existing caller); blend_set == 1 uses these
     * Vulkan values (enable false = blending off, the factors/ops/mask are then ignored except the write mask). */
    uint8_t          blend_set, blend_enable;
    uint32_t         blend_src_rgb, blend_dst_rgb, blend_op_rgb, blend_src_a, blend_dst_a, blend_op_a;   /* VkBlendFactor / VkBlendOp */
    uint32_t         color_write_mask;   /* VkColorComponentFlags */
    /* Several colour attachments: n_color 0/1 = the single attachment above (color_format, blend_*). n_color 2..4: attachment i has format color_formats[i]
     * (VK_FORMAT_UNDEFINED = an unused slot: nothing written) and the blend state above with write mask color_write_masks[i] (when independent_blend == 0 every
     * mask is forced to color_write_mask: without the device feature all attachments must be identical). */
    uint8_t          n_color, independent_blend;
    /* masks_valid: color_write_masks[] were decoded (an all-zero array is then a real "write nothing"); 0 = unknown, the builder uses 0xf for every slot.
     * slot_blend[i] (i >= 1): that attachment's own blend state when .set, used only with independent_blend; otherwise slot i copies attachment 0. */
    uint8_t          masks_valid;
    uint8_t          raster_order_access;   /* set the colour-blend rasterization-order attachment access flag (framebuffer-fetch pipelines; needs the device feature) */
    struct { uint8_t set, enable; uint32_t src_rgb, dst_rgb, op_rgb, src_a, dst_a, op_a; } slot_blend[4];
    uint32_t         color_formats[4];
    uint32_t         color_write_masks[4];
    /* Encoder-state mode of the ARM64 executor (all zero = every existing caller unchanged). dyn_state != 0 makes cull mode, front face and depth bias dynamic
     * (Vulkan 1.3 core extended dynamic state; the caller checks the device) and, for the attachments named by ds_depth_format / ds_stencil_format (the
     * dynamic-rendering formats, VK_FORMAT_UNDEFINED = no such attachment; separate from the legacy depth_format above, which this builder ignores), depth test /
     * write / compare, stencil test / ops / masks / reference dynamic as well. no_color != 0: the pass has no colour attachment (one unused slot). */
    VkFormat         ds_depth_format, ds_stencil_format;
    uint8_t          dyn_state, no_color;
    const lagfx_pipeline_diag_t *diag;   /* OPTIONAL, failure bundle only (see lagfx_pipeline_diag_t) */
    /* DIAGNOSTIC-ONLY hooks of the -13 matrix (src/vulkan/pipeline_matrix.c). Zero / NULL = nothing changes: no production caller sets them and a pipeline built with them zero is
     * exactly the one built before they existed.
     *   diag_gpci_pnext: structs appended to the pNext chain of the VkGraphicsPipelineCreateInfo after VkPipelineRenderingCreateInfo (VkRenderingAttachmentLocationInfo /
     *                    VkRenderingInputAttachmentIndexInfo).
     *   diag_render_pass (a VkRenderPass handle) != 0: build against that legacy render pass / subpass (no VkPipelineRenderingCreateInfo is chained). */
    const void      *diag_gpci_pnext;
    uint64_t         diag_render_pass;
    uint32_t         diag_subpass;
    /* diag_no_fragment != 0 (pipeline-reduce rows only): no fragment stage (fragment_shader may be VK_NULL_HANDLE) and rasterizerDiscardEnable = VK_TRUE (a vertex-only pipeline). */
    uint8_t          diag_no_fragment;
    uint32_t         diag_create_flags;   /* DIAGNOSTIC (pipeline-reduce): VkPipelineCreateFlags of the VkGraphicsPipelineCreateInfo; 0 in production */
    /* Future fields (Stage 70b+):
     *   cull_mode, front_face, blend factor, depth-stencil state... */
} lagfx_pipeline_desc_t;

/**
 * Build a VkGraphicsPipeline from the MVP descriptor. Mirrors the
 * exact pipeline-build sequence used by tests/triangle-lavapipe-e2e.c
 * but parameterised on the two shader modules and color/depth formats.
 *
 * Uses VK_KHR_dynamic_rendering when available (the e2e test does).
 *
 * @param device  VkDevice handle (must outlive the returned VkPipeline)
 * @param desc    populated descriptor; both shader modules required
 * @param out_pipeline  receives the new VkPipeline on success
 * @return LAGFX_OK on success; LAGFX_ERR_INVALID_ARG on NULL inputs;
 *         LAGFX_ERR_BACKEND on Vulkan failures.
 */
lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline);

/* While on, THIS thread's failing lagfx_pipeline_build() calls log nothing, write no bundle and start no matrix (the matrix / repro rebuild failing pipelines on purpose). */
void lagfx_pipeline_build_quiet(int on);

/* VkResult of the calling thread's last vkCreateGraphicsPipelines inside lagfx_pipeline_build() (lagfx_pipeline_build itself only returns LAGFX_ERR_BACKEND). */
int32_t lagfx_pipeline_build_last_result(void);

/* FNV-1a 64 (historical offset basis 1469598103934665603, see pipeline_build.c) of a byte range = the "hash=" of the "pipeline-build-fail:" device log line (compare it with examples/dump-pairhash on a dump). */
uint64_t lagfx_pipeline_spv_hash(const void *data, size_t n);

VkFormat lagfx_pso_vertex_format_to_vk(uint32_t mtl_vertex_format);   /* VK_FORMAT_UNDEFINED = not handled */

#ifdef __cplusplus
}
#endif

#endif /* LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H */
