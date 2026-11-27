/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — VK_QCOM_tile_shading probe / decision / diagnostic line (see qcom_tile.h). src/vulkan/qcom_tile.c */
#include "qcom_tile.h"

#include <stdio.h>
#include <string.h>

#if UINTPTR_MAX == 0xffffffffffffffffu   /* LP64: 4-byte sType + padding + 8-byte pNext, as in the Khronos headers */
_Static_assert(sizeof(lagfx_qct_feat_t) == 72 && sizeof(lagfx_qct_prop_t) == 40 && sizeof(lagfx_qct_bool_feat_t) == 24 && sizeof(lagfx_qct_heap_prop_t) == 24, "qcom tile struct layout (LP64)");
#endif
#ifdef VK_QCOM_tile_shading
_Static_assert((int)LAGFX_QCT_ST_TILE_SHADING_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_FEATURES_QCOM, "tile shading features sType");
_Static_assert((int)LAGFX_QCT_ST_TILE_SHADING_PROPERTIES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_PROPERTIES_QCOM, "tile shading properties sType");
#endif
#ifdef VK_QCOM_tile_properties
_Static_assert((int)LAGFX_QCT_ST_TILE_PROPERTIES_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM, "tile properties features sType");
#endif
#ifdef VK_QCOM_tile_memory_heap
_Static_assert((int)LAGFX_QCT_ST_TILE_MEMORY_HEAP_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_MEMORY_HEAP_FEATURES_QCOM, "tile memory heap features sType");
_Static_assert((int)LAGFX_QCT_ST_TILE_MEMORY_HEAP_PROPERTIES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_MEMORY_HEAP_PROPERTIES_QCOM, "tile memory heap properties sType");
#endif

bool lagfx_qcomtile_flag(const char *v) { return v && v[0] == '1' && !v[1]; }

void lagfx_qcomtile_caps_adreno840(lagfx_qcomtile_caps_t *c) {
    memset(c, 0, sizeof *c);
    c->ext_tile_shading = c->ext_tile_properties = c->ext_tile_memory_heap = 1;
    /* tileShading fragmentStage colorAtt depthAtt stencilAtt inputAtt sampledAtt perTileDraw perTileDispatch dispatchTile = 1; apron anisoApron = 0; atomicOps = 1; imageProcessing = 0 */
    c->feat = LAGFX_QCT_BIT(LAGFX_QCT_F_TILE_SHADING) | LAGFX_QCT_BIT(LAGFX_QCT_F_FRAGMENT_STAGE) | LAGFX_QCT_BIT(LAGFX_QCT_F_COLOR) | LAGFX_QCT_BIT(LAGFX_QCT_F_DEPTH) |
              LAGFX_QCT_BIT(LAGFX_QCT_F_STENCIL) | LAGFX_QCT_BIT(LAGFX_QCT_F_INPUT) | LAGFX_QCT_BIT(LAGFX_QCT_F_SAMPLED) | LAGFX_QCT_BIT(LAGFX_QCT_F_PER_TILE_DRAW) |
              LAGFX_QCT_BIT(LAGFX_QCT_F_PER_TILE_DISPATCH) | LAGFX_QCT_BIT(LAGFX_QCT_F_DISPATCH_TILE) | LAGFX_QCT_BIT(LAGFX_QCT_F_ATOMIC_OPS);
    c->max_apron = 8, c->prefer_non_coherent = 1, c->gran_w = c->gran_h = 16, c->rate_w = c->rate_h = 8;
    /* f_tile_properties / f_tile_memory_heap stay 0: the device log does not record those two feature values (experiment 0 of the doc prints them) */
}

lagfx_qcomtile_reason_t lagfx_qcomtile_decide(bool flag, bool dyn, const lagfx_qcomtile_caps_t *c) {
    if (!flag) return LAGFX_QCT_OFF_FLAG;
    if (!c) return LAGFX_QCT_NO_EXT_TILE_SHADING;
    if (!dyn) return LAGFX_QCT_NO_DYNAMIC_RENDERING;
    if (!c->ext_tile_shading) return LAGFX_QCT_NO_EXT_TILE_SHADING;
    if (!c->ext_tile_properties) return LAGFX_QCT_NO_EXT_TILE_PROPERTIES;
    if ((c->feat & LAGFX_QCT_REQUIRED) != LAGFX_QCT_REQUIRED) return LAGFX_QCT_NO_FEATURE;
    if (!c->gran_w || !c->gran_h || !c->rate_w || !c->rate_h) return LAGFX_QCT_BAD_PROPS;
    return LAGFX_QCT_ENABLE;
}

uint32_t lagfx_qcomtile_enable_mask(const lagfx_qcomtile_caps_t *c) { return c ? (c->feat & LAGFX_QCT_WANTED) : 0u; }

void lagfx_qcomtile_fill_features(const lagfx_qcomtile_caps_t *c, lagfx_qct_feat_t *o, lagfx_qct_bool_feat_t *tp) {
    uint32_t m = lagfx_qcomtile_enable_mask(c);
    memset(o, 0, sizeof *o);
    o->sType = LAGFX_QCT_ST_TILE_SHADING_FEATURES;
    uint32_t *b = &o->tileShading;   /* the 14 VkBool32 members are consecutive, in LAGFX_QCT_F_* order */
    for (unsigned f = 0; f < LAGFX_QCT_F_COUNT; f++) b[f] = (m & LAGFX_QCT_BIT(f)) ? 1u : 0u;
    memset(tp, 0, sizeof *tp);
    tp->sType = LAGFX_QCT_ST_TILE_PROPERTIES_FEATURES;
    tp->value = (c && c->f_tile_properties) ? 1u : 0u;   /* tileProperties: only the vkGetDynamicRenderingTilePropertiesQCOM query needs it */
}

const char *lagfx_qcomtile_reason_name(lagfx_qcomtile_reason_t r) {
    static const char *const n[LAGFX_QCT_REASON_COUNT] = { "flag_off", "no_dynamic_rendering", "no_ext_tile_shading", "no_ext_tile_properties", "no_feature", "bad_props", "enable" };
    return (unsigned)r < LAGFX_QCT_REASON_COUNT ? n[r] : "?";
}

size_t lagfx_qcomtile_format_line(char *buf, size_t cap, bool flag, bool inj, const lagfx_qcomtile_caps_t *c, lagfx_qcomtile_reason_t r, uint32_t mask, bool enabled,
                                  lagfx_qcomtile_fallback_t fb, int create_result) {
    if (!buf || !cap) return 0;
#define F(x) ((c->feat >> (x)) & 1u)
    int n = snprintf(buf, cap,
        "g7-gpu-qcomtile: flag=%d inject=%d ext[tile_shading=%d tile_properties=%d tile_memory_heap=%d] "
        "feat[tileShading=%u fragment=%u color=%u depth=%u stencil=%u input=%u sampled=%u perTileDraw=%u perTileDispatch=%u dispatchTile=%u apron=%u anisoApron=%u atomics=%u imageProc=%u tileProperties=%d tileMemoryHeap=%d] "
        "props[granularity=%ux%u maxRate=%ux%u maxApron=%u preferNonCoherent=%u heap_submit_boundary=%d heap_buffer_xfer=%d] "
        "decision=%s enable_mask=0x%x enabled=%d create_result=%d chosen_path=compute_emulation qcom_native_path=not_implemented fallback=%s",
        (int)flag, (int)inj, c->ext_tile_shading, c->ext_tile_properties, c->ext_tile_memory_heap,
        F(LAGFX_QCT_F_TILE_SHADING), F(LAGFX_QCT_F_FRAGMENT_STAGE), F(LAGFX_QCT_F_COLOR), F(LAGFX_QCT_F_DEPTH), F(LAGFX_QCT_F_STENCIL), F(LAGFX_QCT_F_INPUT), F(LAGFX_QCT_F_SAMPLED),
        F(LAGFX_QCT_F_PER_TILE_DRAW), F(LAGFX_QCT_F_PER_TILE_DISPATCH), F(LAGFX_QCT_F_DISPATCH_TILE), F(LAGFX_QCT_F_APRON), F(LAGFX_QCT_F_ANISO_APRON), F(LAGFX_QCT_F_ATOMIC_OPS),
        F(LAGFX_QCT_F_IMAGE_PROCESSING), c->f_tile_properties, c->f_tile_memory_heap,
        c->gran_w, c->gran_h, c->rate_w, c->rate_h, c->max_apron, c->prefer_non_coherent, c->heap_queue_submit_boundary, c->heap_tile_buffer_transfers,
        lagfx_qcomtile_reason_name(r), mask, (int)enabled, create_result, fb == LAGFX_QCT_FB_DEVICE_CREATE_FAILED ? "device_create_failed(retried_without)" : "none");
#undef F
    return n < 0 ? 0 : ((size_t)n >= cap ? cap - 1u : (size_t)n);
}

#ifdef LAGFX_HAVE_VULKAN
void lagfx_qcomtile_probe(VkPhysicalDevice pd, bool ext_ts, bool ext_tp, bool ext_heap, lagfx_qcomtile_caps_t *c) {
    memset(c, 0, sizeof *c);
    c->ext_tile_shading = ext_ts, c->ext_tile_properties = ext_tp, c->ext_tile_memory_heap = ext_heap;
    if (!pd || !(ext_ts || ext_tp || ext_heap)) return;
    lagfx_qct_feat_t f = { .sType = LAGFX_QCT_ST_TILE_SHADING_FEATURES };
    lagfx_qct_prop_t p = { .sType = LAGFX_QCT_ST_TILE_SHADING_PROPERTIES };
    lagfx_qct_bool_feat_t tpf = { .sType = LAGFX_QCT_ST_TILE_PROPERTIES_FEATURES }, hf = { .sType = LAGFX_QCT_ST_TILE_MEMORY_HEAP_FEATURES };
    lagfx_qct_heap_prop_t hp = { .sType = LAGFX_QCT_ST_TILE_MEMORY_HEAP_PROPERTIES };
    void *fc = NULL, *pc = NULL;
    if (ext_ts) { f.pNext = fc; fc = &f; p.pNext = pc; pc = &p; }
    if (ext_tp) { tpf.pNext = fc; fc = &tpf; }
    if (ext_heap) { hf.pNext = fc; fc = &hf; hp.pNext = pc; pc = &hp; }
    VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fc };
    VkPhysicalDeviceProperties2 qp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = pc };
    vkGetPhysicalDeviceFeatures2(pd, &q);
    vkGetPhysicalDeviceProperties2(pd, &qp);
    if (ext_ts) {
        const uint32_t *b = &f.tileShading;
        for (unsigned i = 0; i < LAGFX_QCT_F_COUNT; i++) if (b[i]) c->feat |= LAGFX_QCT_BIT(i);
        c->max_apron = p.maxApronSize, c->prefer_non_coherent = p.preferNonCoherent ? 1u : 0u;
        c->gran_w = p.tileGranularityW, c->gran_h = p.tileGranularityH, c->rate_w = p.maxTileShadingRateW, c->rate_h = p.maxTileShadingRateH;
    }
    if (ext_tp) c->f_tile_properties = tpf.value ? 1 : 0;
    if (ext_heap) c->f_tile_memory_heap = hf.value ? 1 : 0, c->heap_queue_submit_boundary = hp.a ? 1 : 0, c->heap_tile_buffer_transfers = hp.b ? 1 : 0;
}
#endif
