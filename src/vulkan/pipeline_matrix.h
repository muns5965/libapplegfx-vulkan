/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — the -13 pipeline-creation matrix: DIAGNOSTIC re-tries of a pipeline that vkCreateGraphicsPipelines rejected (Adreno 840: VK_ERROR_UNKNOWN for the
 * framebuffer-fetch fragment shader A2Xghfc), each with ONE independent change, to learn WHICH change the driver accepts. Nothing here changes what the normal path builds:
 * every variant is built from a COPY of the descriptor / SPIR-V / layout, the matrix is OFF unless LAGFX_PIPELINE_MATRIX is set, and a variant that builds is destroyed again.
 *
 * Rows (ids are what the log prints):
 *   M0  original                                            M6a legacy VkRenderPass + subpass input attachments + self-dependency (rast-order as recorded)
 *   M1  rasterization-order flag removed (ANGLE's           M6b legacy render pass, no rast-order flag (non-coherent + self-dependency)
 *       non-coherent path; needs the by-region colour-write   M8  the Dim3D fragment module from research/texture3d (file, pipeline creation only)
 *       -> input-read barrier at draw time, see                M9g/h/i/j/k  OpUndef rewrites (k: only the OpReturnValue(OpUndef) operands)
 *       lagfx_matrix_cmd_fetch_barrier)                        M10 combinations (`row=M10:<ids>`: all compatible changed rows together, then pairs) of the rows above that individually CHANGED the outcome vs M0
 *   M2  SPIR-V 1.4 -> 1.3 where no 1.4-only feature is used  M3a layout declares the full contiguous input-attachment block (ANGLE: 8 bindings)
 *   M5a/M5b/M5c explicit VkRenderingAttachmentLocationInfo /  M3b input attachments renumbered to the LOWEST bindings (binding = index)
 *       VkRenderingInputAttachmentIndexInfo (both / index only / location only; identity)
 *   M4a input attachments in their own descriptor set 1       M4b every non-input-attachment binding VERTEX|FRAGMENT
 * (M7 is not defined.) A row is `n/a` when the change does not apply or the device lacks what it needs (e.g. no dynamicRenderingLocalRead for M5).
 */
#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_MATRIX_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_MATRIX_H

#ifdef LAGFX_HAVE_VULKAN
#include "pipeline_build.h"
#include <stdbool.h>
#include <vulkan/vulkan.h>

/* recipe bits: a matrix row = a set of these changes (single-bit rows are the independent variants; M6b = LEGACY|NORO) */
enum {
    LAGFX_MX_NORO = 1u << 0, LAGFX_MX_V13 = 1u << 1, LAGFX_MX_BLOCK = 1u << 2, LAGFX_MX_IAFIRST = 1u << 3, LAGFX_MX_IASET1 = 1u << 4, LAGFX_MX_STAGES = 1u << 5,
    LAGFX_MX_MAPBOTH = 1u << 6, LAGFX_MX_MAPIDX = 1u << 7, LAGFX_MX_MAPLOC = 1u << 8, LAGFX_MX_LEGACY = 1u << 9, LAGFX_MX_M8 = 1u << 10,
    LAGFX_MX_UG = 1u << 11, LAGFX_MX_UH = 1u << 12, LAGFX_MX_UI = 1u << 13, LAGFX_MX_UJ = 1u << 14, LAGFX_MX_UK = 1u << 15, LAGFX_MX_M8C = 1u << 16,
    LAGFX_MX_STUBFS = 1u << 17, LAGFX_MX_STUBVS = 1u << 18, LAGFX_MX_VINF32 = 1u << 19, LAGFX_MX_STRIP = 1u << 20, LAGFX_MX_IDX32 = 1u << 21,
};

typedef struct {
    bool have_local_read, have_raster_order;     /* VK_KHR_dynamic_rendering_local_read / rasterization-order attachment access ENABLED on the device */
    uint32_t max_color_att, max_per_stage_input_att;   /* device limits; 0 = unknown */
    const char *m8_dir;                          /* directory searched for <fs hash>.m8.fs.spv / .m8c.fs.spv (+ optional <vs hash>.m8.vs.spv); NULL = no M8 */
    void (*emit)(void *user, const char *line);  /* one log line per call (no newline) */
    void *user;
    VkResult (*test_override)(void *user, uint32_t recipe, VkResult real);   /* TEST SEAM (NULL in production): replaces the VkResult of an applicable row, to simulate a driver that rejects the original */
} lagfx_matrix_env_t;

typedef struct {
    int32_t  m0_result;                          /* VkResult of M0 */
    uint32_t built, failed, na, combos;          /* rows that built / failed / did not apply; M10 rows run */
    char     built_ids[160];                     /* "M1,M6b,..." (bounded) */
} lagfx_matrix_result_t;

/* Runs the matrix for the failed `d0` (vertex_spv / fragment_spv, entry points, layout and every state field set; the module handles are not used). `bind` / `nbind` = the bindings of
 * d0->layout (needed by the layout rows M3 / M4; 0 = those rows are n/a). Builds on `dev` only; no other global state. */
void lagfx_pipeline_matrix_run(VkDevice dev, const lagfx_pipeline_desc_t *d0, const VkDescriptorSetLayoutBinding *bind, uint32_t nbind, const lagfx_matrix_env_t *env, lagfx_matrix_result_t *res);

/* The opt-in production hook (called from lagfx_pipeline_build when vkCreateGraphicsPipelines failed): a no-op unless LAGFX_PIPELINE_MATRIX is set. LAGFX_PIPELINE_MATRIX=1 expands the
 * first 2 DISTINCT failing (vs, fs) pairs of the process; =N (2..8) the first N; every other failing pair costs one bounded "suppressed" line. LAGFX_PIPELINE_MATRIX_DIR=<dir> enables M8. */
void lagfx_pipeline_matrix_on_failure(VkDevice dev, const lagfx_pipeline_desc_t *failed, VkResult vr);
/* Told once by lagfx_vk_init what the device was created with. */
void lagfx_pipeline_matrix_set_device(bool have_local_read, bool have_raster_order, uint32_t max_color_att, uint32_t max_per_stage_input_att);

/* The by-region colour-attachment-write -> input-attachment-read memory barrier ANGLE inserts for non-coherent framebuffer fetch (row M1 at draw time): COLOR_ATTACHMENT_OUTPUT ->
 * FRAGMENT_SHADER, COLOR_ATTACHMENT_WRITE -> INPUT_ATTACHMENT_READ, VK_DEPENDENCY_BY_REGION_BIT. Record it between draws inside the render pass (needs dynamicRenderingLocalRead). */
void lagfx_matrix_cmd_fetch_barrier(VkCommandBuffer cb);
#else
#define lagfx_pipeline_matrix_on_failure(d, f, r) ((void)0)
#endif
#endif
