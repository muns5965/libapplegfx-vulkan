/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — VK_QCOM_tile_shading capability probe and opt-in device enablement (default OFF).
 * src/vulkan/qcom_tile.h
 *
 * Basis (Khronos only): Vulkan registry vk.xml (VK_QCOM_tile_shading #310, VK_QCOM_tile_properties #485, VK_QCOM_tile_memory_heap #548) and the Khronos feature
 * proposal / SPV_QCOM_tile_shading registry page. See docs/qcom-tile-shading.md for the mapping and the open questions.
 *
 * What this module does: (a) decides, from a capability record, whether lagfx_vk_init may enable the extension pair at vkCreateDevice; (b) reads the record from a
 * physical device (read-only vkGet* queries); (c) formats the one-line diagnostic g7-gpu-qcomtile. It does NOT record or run any tile-shading command: the Metal tile
 * path stays the compute emulation of src/protocol/arm64_tile.c. The decision function is pure so tests can inject device records lavapipe does not have.
 *
 * Gate: LAGFX_QCOM_TILE=1 (exactly "1"); anything else / unset = OFF = no change to what vkCreateDevice is given.
 */
#ifndef LIBAPPLEGFX_VULKAN_QCOM_TILE_H
#define LIBAPPLEGFX_VULKAN_QCOM_TILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Feature bits of VkPhysicalDeviceTileShadingFeaturesQCOM, in struct order. */
enum {
    LAGFX_QCT_F_TILE_SHADING = 0, LAGFX_QCT_F_FRAGMENT_STAGE, LAGFX_QCT_F_COLOR, LAGFX_QCT_F_DEPTH, LAGFX_QCT_F_STENCIL, LAGFX_QCT_F_INPUT, LAGFX_QCT_F_SAMPLED,
    LAGFX_QCT_F_PER_TILE_DRAW, LAGFX_QCT_F_PER_TILE_DISPATCH, LAGFX_QCT_F_DISPATCH_TILE, LAGFX_QCT_F_APRON, LAGFX_QCT_F_ANISO_APRON, LAGFX_QCT_F_ATOMIC_OPS,
    LAGFX_QCT_F_IMAGE_PROCESSING, LAGFX_QCT_F_COUNT
};
#define LAGFX_QCT_BIT(f) (1u << (f))
/* Required to consider enabling: the extension itself, colour tile attachments (the imageblock is the colour attachments) and per-tile dispatch (the tile kernel). */
#define LAGFX_QCT_REQUIRED (LAGFX_QCT_BIT(LAGFX_QCT_F_TILE_SHADING) | LAGFX_QCT_BIT(LAGFX_QCT_F_COLOR) | LAGFX_QCT_BIT(LAGFX_QCT_F_PER_TILE_DISPATCH))
/* Enabled when reported (the experiments of docs/qcom-tile-shading.md need them); apron, depth/stencil, per-tile draw, fragment stage, image processing are never enabled here. */
#define LAGFX_QCT_WANTED (LAGFX_QCT_REQUIRED | LAGFX_QCT_BIT(LAGFX_QCT_F_DISPATCH_TILE) | LAGFX_QCT_BIT(LAGFX_QCT_F_INPUT) | LAGFX_QCT_BIT(LAGFX_QCT_F_SAMPLED) | LAGFX_QCT_BIT(LAGFX_QCT_F_ATOMIC_OPS))

typedef struct {
    uint8_t ext_tile_shading, ext_tile_properties, ext_tile_memory_heap;   /* extension listed by vkEnumerateDeviceExtensionProperties */
    uint8_t f_tile_properties, f_tile_memory_heap;                         /* VkPhysicalDeviceTilePropertiesFeaturesQCOM::tileProperties / TileMemoryHeapFeaturesQCOM::tileMemoryHeap */
    uint8_t heap_queue_submit_boundary, heap_tile_buffer_transfers;        /* VkPhysicalDeviceTileMemoryHeapPropertiesQCOM */
    uint32_t feat;                                                         /* LAGFX_QCT_BIT(LAGFX_QCT_F_*) as reported */
    uint32_t max_apron, prefer_non_coherent, gran_w, gran_h, rate_w, rate_h;
} lagfx_qcomtile_caps_t;

typedef enum {
    LAGFX_QCT_OFF_FLAG = 0,            /* LAGFX_QCOM_TILE is not "1" */
    LAGFX_QCT_NO_DYNAMIC_RENDERING,    /* tile shading is used with VkRenderingInfo here: dynamic rendering must be enabled */
    LAGFX_QCT_NO_EXT_TILE_SHADING,
    LAGFX_QCT_NO_EXT_TILE_PROPERTIES,  /* VK_QCOM_tile_shading depends on VK_QCOM_tile_properties (vk.xml) */
    LAGFX_QCT_NO_FEATURE,              /* tileShading / tileShadingColorAttachments / tileShadingPerTileDispatch not all reported */
    LAGFX_QCT_BAD_PROPS,               /* tileGranularity or maxTileShadingRate reported as 0 */
    LAGFX_QCT_ENABLE,                  /* request the extension pair + the features of lagfx_qcomtile_enable_mask() at vkCreateDevice */
    LAGFX_QCT_REASON_COUNT
} lagfx_qcomtile_reason_t;

typedef enum { LAGFX_QCT_FB_NONE = 0, LAGFX_QCT_FB_DEVICE_CREATE_FAILED } lagfx_qcomtile_fallback_t;   /* vkCreateDevice refused the chain: retried without it */

/* Raw layouts of the Khronos structs (header-independent: the build header may predate the extension). LP64: 72 / 40 / 24 / 24 / 24 bytes; asserted in qcom_tile.c. */
typedef struct { int32_t sType; void *pNext;
    uint32_t tileShading, tileShadingFragmentStage, tileShadingColorAttachments, tileShadingDepthAttachments, tileShadingStencilAttachments, tileShadingInputAttachments,
             tileShadingSampledAttachments, tileShadingPerTileDraw, tileShadingPerTileDispatch, tileShadingDispatchTile, tileShadingApron, tileShadingAnisotropicApron,
             tileShadingAtomicOps, tileShadingImageProcessing; } lagfx_qct_feat_t;
typedef struct { int32_t sType; void *pNext; uint32_t maxApronSize, preferNonCoherent, tileGranularityW, tileGranularityH, maxTileShadingRateW, maxTileShadingRateH; } lagfx_qct_prop_t;
typedef struct { int32_t sType; void *pNext; uint32_t value; } lagfx_qct_bool_feat_t;   /* VkPhysicalDeviceTilePropertiesFeaturesQCOM / VkPhysicalDeviceTileMemoryHeapFeaturesQCOM */
typedef struct { int32_t sType; void *pNext; uint32_t a, b; } lagfx_qct_heap_prop_t;     /* VkPhysicalDeviceTileMemoryHeapPropertiesQCOM: queueSubmitBoundary, tileBufferTransfers */
#define LAGFX_QCT_ST_TILE_SHADING_FEATURES 1000309000
#define LAGFX_QCT_ST_TILE_SHADING_PROPERTIES 1000309001
#define LAGFX_QCT_ST_TILE_PROPERTIES_FEATURES 1000484000
#define LAGFX_QCT_ST_TILE_MEMORY_HEAP_FEATURES 1000547000
#define LAGFX_QCT_ST_TILE_MEMORY_HEAP_PROPERTIES 1000547001

/* LAGFX_QCOM_TILE: true only for exactly "1". */
bool lagfx_qcomtile_flag(const char *env_value);
/* The record of the Adreno 840 of docs/qcom-tile-shading.md (device log g7-vkdev: qcom_tile_shading): the test-injection seam (LAGFX_QCOM_TILE_INJECT=adreno840, honoured only with the flag on). */
void lagfx_qcomtile_caps_adreno840(lagfx_qcomtile_caps_t *out);
/* Pure decision. flag = LAGFX_QCOM_TILE state; dynamic_rendering = the device is created with dynamicRendering. */
lagfx_qcomtile_reason_t lagfx_qcomtile_decide(bool flag, bool dynamic_rendering, const lagfx_qcomtile_caps_t *c);
/* Features to turn on for LAGFX_QCT_ENABLE: reported AND wanted, as LAGFX_QCT_BIT()s. Fills the raw feature struct (all 14 members; only the mask is VK_TRUE). */
uint32_t lagfx_qcomtile_enable_mask(const lagfx_qcomtile_caps_t *c);
void lagfx_qcomtile_fill_features(const lagfx_qcomtile_caps_t *c, lagfx_qct_feat_t *out, lagfx_qct_bool_feat_t *tile_props_out);
const char *lagfx_qcomtile_reason_name(lagfx_qcomtile_reason_t r);

/* g7-gpu-qcomtile base line (device caps, flag state, decision, outcome, chosen path, fallback); returns the length. */
size_t lagfx_qcomtile_format_line(char *buf, size_t cap, bool flag, bool injected, const lagfx_qcomtile_caps_t *c, lagfx_qcomtile_reason_t r, uint32_t enable_mask,
                                  bool enabled, lagfx_qcomtile_fallback_t fb, int create_result);

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
/* Read-only: vkGet{Features,Properties}2 with a struct chained ONLY for each extension the device lists. */
void lagfx_qcomtile_probe(VkPhysicalDevice pd, bool ext_shading, bool ext_properties, bool ext_heap, lagfx_qcomtile_caps_t *out);
#endif

#endif
