/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — pipeline failure bundle: everything needed to rebuild a failed vkCreateGraphicsPipelines call offline.
 * src/vulkan/pipeline_bundle.h
 *
 * Option (default OFF): LAGFX_PIPEFAIL_BUNDLE_DIR=<dir>, or LAGFX_ARM64_DUMP=1 with LAGFX_ARM64_DUMP_DIR=<dir>. When lagfx_pipeline_build() fails, the first 8 DISTINCT failures
 * (per process) write <dir>/pipefail_<hash>/ :
 *   state      text, one "key value..." per line (format below); lines starting with '#' are informational (the derived Vulkan create-info state)
 *   vs.spv fs.spv   the two SPIR-V modules exactly as handed to vkCreateShaderModule
 *   vs.bc fs.bc     the AIR bitcode they were translated from (only when the caller supplied it: lagfx_pipeline_diag_t; each <= 4 MiB)
 * Bounded: state <= 64 KiB, spv <= 8 MiB each. Never overwrites (O_EXCL); an existing directory is left alone.
 *
 * state keys: lagfx-pipefail-bundle 1 | result <VkResult> | translator <lagfx_build_info()> | origin <text> | vs_entry/fs_entry/vs_fn/fs_fn | vs_hash/fs_hash/vs_bytes/fs_bytes |
 *   desc.<field> <values...> (every scalar field of lagfx_pipeline_desc_t; the repro rebuilds the descriptor from them and calls lagfx_pipeline_build() itself) |
 *   layout.sets <0|1> | layout.binding <binding> <VkDescriptorType> <count> <stageFlags> | layout.key <hex> | layout.error <status> conflicts=<n> overflows=<n> | layout.push <stageFlags> <offset> <size> | spec_constants 0 |
 *   dev.name/dev.driver_version/dev.api_version/dev.vendor_id/dev.device_id/dev.driver_name/dev.driver_info/dev.extensions/dev.features/dev.caps_enabled |
 *   env.<NAME> <value|(unset)> (the environment variables lagfx_pipeline_build() reads: they change the built state, the repro only REPORTS a difference).
 */
#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_BUNDLE_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_BUNDLE_H

#include "pipeline_build.h"

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

#define LAGFX_BUNDLE_MAX_DSL 128u   /* = LAGFX_LAYOUT_MAX_BINDINGS */
#define LAGFX_BUNDLE_MAX_PUSH 4u
#define LAGFX_BUNDLE_MAX_FAILURES 8u

/* The directory the option names ("" / NULL = the option is off). */
const char *lagfx_pipeline_bundle_dir(void);

/* Writes the bundle for a failed lagfx_pipeline_build(); `derived` = informational text (the Vulkan create-info state), may be NULL. The layout bindings are derived from
 * the SPIR-V exactly as lagfx_build_pipeline_layout_from_spv() does. Returns true and the directory name when a NEW bundle was written. */
bool lagfx_pipeline_bundle_write(const lagfx_pipeline_desc_t *desc, VkResult vr, const char *derived, char *out_dir, size_t out_cap);
void lagfx_pipeline_bundle_reset(void);
void lagfx_pipeline_bundle_suppress(int on);   /* this thread writes no bundle while on (the repro rebuilds failing pipelines on purpose) */   /* test seam: forgets the distinct-failure set */

/* A parsed bundle (lagfx_bundle_load). desc has its scalar fields set, every pointer NULL. */
typedef struct {
    lagfx_pipeline_desc_t desc;
    int32_t  result;
    char     translator[160], origin[160], vs_entry[64], fs_entry[64], vs_fn[96], fs_fn[96];
    uint64_t vs_hash, fs_hash;
    uint32_t n_sets, n_dsl, n_push;
    uint64_t layout_key;        /* layout.key: lagfx_ba_key of the binding table the writer derived (0 = a bundle from before the key existed) */
    bool     layout_error;      /* layout.error: the writer's table had a conflict / overflow, layout.binding lines are empty on purpose */
    VkDescriptorSetLayoutBinding dsl[LAGFX_BUNDLE_MAX_DSL];
    VkPushConstantRange push[LAGFX_BUNDLE_MAX_PUSH];
    char     dev_name[256], dev_driver_name[VK_MAX_DRIVER_NAME_SIZE], dev_driver_info[VK_MAX_DRIVER_INFO_SIZE], dev_exts[512], dev_features[1024];
    uint32_t dev_driver_version, dev_api_version, dev_vendor, dev_device, dev_caps;
    bool     have_dev;
    char     env[8][96];   /* "NAME=value" as recorded */
    uint32_t n_env;
    uint8_t *vs_spv, *fs_spv, *vs_bc, *fs_bc;
    size_t   vs_bytes, fs_bytes, vs_bc_bytes, fs_bc_bytes;
} lagfx_bundle_t;

bool lagfx_bundle_load(const char *dir, lagfx_bundle_t *b, char *err, size_t err_cap);
void lagfx_bundle_free(lagfx_bundle_t *b);
const char *lagfx_vk_result_name(VkResult r);
#endif /* LAGFX_HAVE_VULKAN */
#endif
