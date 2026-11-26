/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — the fragment-module REDUCTION LADDER: finds WHICH PART of a fragment module that vkCreateGraphicsPipelines rejects (Adreno 840: VK_ERROR_UNKNOWN, -13) makes the
 * driver reject it. DIAGNOSTIC only: opt-in, every row builds a rewritten COPY of the fragment module (pure SPIR-V rewrites in src/vulkan/spv_reduce.c, or a spirv-opt output file that was
 * generated offline) with the failing pipeline state and destroys the pipeline again. Nothing here changes what the normal path builds.
 *
 * Enable: LAGFX_PIPELINE_REDUCE=1 (app failure path: the first distinct failing fragment module of the process; =N up to 4), or pipeline-repro --reduce /
 * lagfx_pipeline_repro_run(flags | LAGFX_PIPELINE_REPRO_REDUCE). LAGFX_PIPELINE_MATRIX_DIR=<dir> (else the bundle dir) supplies the offline files, one directory layout:
 *     <dir>/<fs hash, 16 hex>.red.<name>.fs.spv      rows F:<name>  (spirv-opt outputs, plus u2/u3/u5 = the U1 module optimised)
 *     <dir>/<fs hash, 16 hex>.m8.fs.spv / .m8c.fs.spv rows M8 / M8c of the -13 matrix (pipeline_matrix.h)
 * Other switches: LAGFX_PIPELINE_REDUCE_GROUPS=<letters> (h helpers, s sampling/fetch, c control flow, u undef, i stage interface, o offline files, k cumulative chains; default hscuiok),
 * LAGFX_PIPELINE_REDUCE_MAX=<rows> (default 160), LAGFX_PIPELINE_REDUCE_ONLY=<id,id,..>, LAGFX_PIPELINE_REDUCE_FROM=<id> (resume), LAGFX_PIPELINE_REDUCE_LOG=<path> (every row line is also
 * appended and flushed there at once), LAGFX_PIPELINE_REDUCE_TIMEOUT_MS (isolated rows, default 20000), LAGFX_PIPELINE_REDUCE_CRASH_ROW / _HANG_ROW (TEST injection, isolated child only).
 *
 * One log line per row:
 *   pipeline-reduce: row=<id> result=BUILT|FAIL|CRASH|TIMEOUT|INVALID|n/a vk=<n>(<name>) ms=<t> bytes=<n> inst=<n> sha=<16 hex of the variant> orig=<16 hex> val=<..> [<meaning>] <what changed>
 * BUILT = vkCreateGraphicsPipelines returned VK_SUCCESS on the device that wrote the log (pipeline creation only: it says NOTHING about rendering correctness, and a lavapipe BUILT says nothing
 * about Adreno). val=offline: spirv-val cannot run on the device; the sha in the log keys the offline manifest (manifest.tsv) that carries the spirv-val result of every file.
 */
#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_REDUCE_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_REDUCE_H

#ifdef LAGFX_HAVE_VULKAN
#include "pipeline_build.h"
#include "spv_reduce.h"
#include <stdbool.h>
#include <vulkan/vulkan.h>

enum { LRS_BUILT = 0, LRS_FAIL, LRS_CRASH, LRS_TIMEOUT, LRS_INVALID, LRS_NA };
#define LRS_VK_NODEVICE (-1000000)

/* what an isolated child needs to open its OWN device and build (descriptor + layout description; no module handles) */
typedef struct {
    lagfx_pipeline_desc_t d0;                       /* every scalar state field; layout / modules / spv pointers are ignored */
    const VkDescriptorSetLayoutBinding *bind; uint32_t nbind;
    const VkPushConstantRange *push; uint32_t npush;
    const char *fs_entry;
} lagfx_reduce_target_t;

typedef struct {
    /* the failing pair */
    const uint32_t *fs; size_t fs_bytes;
    const uint32_t *vs; size_t vs_bytes;            /* the real vertex stage (rows with `real VS`); NULL = those rows are n/a */
    const char *file_dir, *file_dir2;               /* offline files (the red.<name> rows); NULL = absent */
    const char *only, *from, *groups;               /* NULL = all */
    uint32_t max_rows, max_adaptive, timeout_ms;
    void (*emit)(void *user, const char *line); void *user;
    /* evaluation: either an existing device (dev + layout, in-process) or `isolate` (fork per row; the parent must NOT have created a Vulkan device) */
    VkDevice dev; VkPipelineLayout layout;
    int isolate;
    const lagfx_reduce_target_t *target;            /* required for isolate */
    const char *dump_dir;                           /* DRY mode: write every module + manifest.tsv instead of building; every row counts as FAIL (adaptive rows follow the worst case) */
    int dry;
    int list_only;                                  /* print the static row ids only */
    VkResult (*test_override)(void *user, const char *row, const uint32_t *fs, size_t bytes, VkResult real);   /* TEST SEAM */
} lagfx_reduce_cfg_t;

typedef struct { uint32_t rows, built, fail, crash, timeout, invalid, na; int32_t r0; char smallest_fail[64]; uint32_t smallest_fail_bytes; } lagfx_reduce_result_t;

/* fills only/from/groups/max_rows/timeout_ms from the LAGFX_PIPELINE_REDUCE_* environment (fields already set are kept) */
void lagfx_pipeline_reduce_cfg_env(lagfx_reduce_cfg_t *cfg);
void lagfx_pipeline_reduce_run(const lagfx_reduce_cfg_t *cfg, lagfx_reduce_result_t *res);
/* Opt-in production hook (lagfx_pipeline_build failure path): no-op unless LAGFX_PIPELINE_REDUCE is set. */
void lagfx_pipeline_reduce_on_failure(VkDevice dev, const lagfx_pipeline_desc_t *failed, VkResult vr);
void lagfx_pipeline_reduce_set_device(bool have_local_read, bool have_raster_order);
/* Hex sha-256 (first 16 hex chars) of a module; exposed for the tool and the tests. */
void lagfx_reduce_sha16(const void *data, size_t bytes, char out[17]);
#else
#define lagfx_pipeline_reduce_on_failure(d, f, r) ((void)0)
#endif
#endif
