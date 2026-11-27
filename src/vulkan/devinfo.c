/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — read-only device identity / extension / feature diagnostic (see devinfo.h).
 * src/vulkan/devinfo.c
 */
#ifdef LAGFX_HAVE_VULKAN
#include "devinfo.h"
#include "common/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Structs newer than the oldest header we build against are declared here (layouts from the Vulkan registry, vk.xml). */
#define ST_TILE_SHADING_FEATURES_QCOM 1000309000
#define ST_TILE_SHADING_PROPERTIES_QCOM 1000309001
typedef struct { VkStructureType sType; void *pNext;
    VkBool32 tileShading, tileShadingFragmentStage, tileShadingColorAttachments, tileShadingDepthAttachments, tileShadingStencilAttachments, tileShadingInputAttachments,
             tileShadingSampledAttachments, tileShadingPerTileDraw, tileShadingPerTileDispatch, tileShadingDispatchTile, tileShadingApron, tileShadingAnisotropicApron,
             tileShadingAtomicOps, tileShadingImageProcessing; } tile_shading_feat_t;
typedef struct { VkStructureType sType; void *pNext; uint32_t maxApronSize; VkBool32 preferNonCoherent; VkExtent2D tileGranularity, maxTileShadingRate; } tile_shading_prop_t;
#define ST_SHADER_TILE_IMAGE_FEATURES 1000395000
#define ST_SHADER_TILE_IMAGE_PROPERTIES 1000395001
typedef struct { VkStructureType sType; void *pNext; VkBool32 shaderTileImageColorReadAccess, shaderTileImageDepthReadAccess, shaderTileImageStencilReadAccess; } tile_image_feat_t;
typedef struct { VkStructureType sType; void *pNext; VkBool32 shaderTileImageCoherentReadAccelerated, shaderTileImageReadSampleFromPixelRateInvocation, shaderTileImageReadFromHelperInvocation; } tile_image_prop_t;
#define ST_LOCAL_READ_FEATURES 1000232000
typedef struct { VkStructureType sType; void *pNext; VkBool32 dynamicRenderingLocalRead; } local_read_feat_t;
#define ST_RASTER_ORDER_FEATURES 1000342000
typedef struct { VkStructureType sType; void *pNext; VkBool32 color, depth, stencil; } raster_order_feat_t;
#define ST_VIEW_FORMAT_INFO 1000170000
#define ST_FILTER_CUBIC_PROPS 1000170001
typedef struct { VkStructureType sType; void *pNext; VkImageViewType imageViewType; } view_fmt_info_t;
typedef struct { VkStructureType sType; void *pNext; VkBool32 filterCubic, filterCubicMinmax; } cubic_view_props_t;

/* ABI / header-clash guards: the local structs have their own names (no clash with any Vulkan header type). Where a header that ships the extension is in use, the sType numbers are
 * cross-checked against it at compile time (the extension macro is defined by exactly the headers that declare the enum). LP64 layout is asserted for the local structs. */
#ifdef VK_QCOM_tile_shading
_Static_assert((int)ST_TILE_SHADING_FEATURES_QCOM == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_FEATURES_QCOM, "tile shading features sType");
_Static_assert((int)ST_TILE_SHADING_PROPERTIES_QCOM == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_PROPERTIES_QCOM, "tile shading properties sType");
#endif
#ifdef VK_EXT_shader_tile_image
_Static_assert((int)ST_SHADER_TILE_IMAGE_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT, "tile image features sType");
_Static_assert((int)ST_SHADER_TILE_IMAGE_PROPERTIES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_PROPERTIES_EXT, "tile image properties sType");
#endif
#ifdef VK_KHR_dynamic_rendering_local_read
_Static_assert((int)ST_LOCAL_READ_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, "local read features sType");
#endif
#ifdef VK_EXT_rasterization_order_attachment_access
_Static_assert((int)ST_RASTER_ORDER_FEATURES == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, "raster order features sType");
#endif
#ifdef VK_EXT_filter_cubic
_Static_assert((int)ST_VIEW_FORMAT_INFO == (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT && (int)ST_FILTER_CUBIC_PROPS == (int)VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT, "filter cubic sTypes");
#endif
#if UINTPTR_MAX == 0xffffffffffffffffu   /* LP64 (x86-64, aarch64 incl. Android arm64-v8a): header layouts of the Khronos structs are 4-byte sType + padding + 8-byte pNext */
_Static_assert(sizeof(tile_shading_feat_t) == 72 && sizeof(tile_shading_prop_t) == 40 && sizeof(tile_image_feat_t) == 32 && sizeof(tile_image_prop_t) == 32 && sizeof(local_read_feat_t) == 24, "local struct layout (LP64)");
#endif

/* format feature bits used below (core values; written as numbers because the cubic bit is named differently across header generations) */
#define FF_SAMPLED_IMAGE 0x0001u
#define FF_COLOR_ATTACHMENT 0x0080u
#define FF_COLOR_ATTACHMENT_BLEND 0x0100u
#define FF_TRANSFER_SRC 0x4000u
#define FF_TRANSFER_DST 0x8000u
#define FF_SAMPLED_FILTER_LINEAR 0x1000u
#define FF_SAMPLED_FILTER_CUBIC 0x2000u

typedef struct { lagfx_vkdev_emit_fn fn; void *user; size_t n; int overflow; size_t cap; size_t dropped; } sink_t;

static void put(sink_t *s, const char *fmt, ...) {
    char buf[LAGFX_VKDEV_LINE_MAX + 64];
    if (s->n >= s->cap) { s->overflow = 1, s->dropped++; return; }
    int w = snprintf(buf, sizeof buf, "g7-vkdev: ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + w, sizeof buf - (size_t)w, fmt, ap);
    va_end(ap);
    if (strlen(buf) >= LAGFX_VKDEV_LINE_MAX) buf[LAGFX_VKDEV_LINE_MAX - 1] = 0;   /* size cap per line (never exceeded by the producers below: they split before) */
    s->fn(s->user, buf);
    s->n++;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}
static int cmp_ext(const void *a, const void *b) { return strcmp(((const VkExtensionProperties *)a)->extensionName, ((const VkExtensionProperties *)b)->extensionName); }

static void san(char *dst, size_t cap, const char *src) {   /* log-safe: printable, no quotes/newlines */
    size_t o = 0;
    for (; src && *src && o + 1 < cap; src++) dst[o++] = (*src >= 0x20 && *src < 0x7f && *src != '\'') ? *src : '_';
    dst[o] = 0;
}

static void stages_text(char *o, size_t cap, uint32_t m) {
    static const struct { uint32_t b; const char *n; } t[] = { { 1, "V" }, { 2, "TC" }, { 4, "TE" }, { 8, "G" }, { 16, "F" }, { 32, "C" }, { 64, "TASK" }, { 128, "MESH" } };
    size_t n = 0;
    o[0] = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (m & t[i].b) n += (size_t)snprintf(o + n, cap - n, "%s%s", n ? "+" : "", t[i].n);
    if ((m & ~255u) && n < cap) snprintf(o + n, cap - n, "%sother(0x%x)", n ? "+" : "", m & ~255u);
    if (!o[0]) snprintf(o, cap, "none");
}
static void ops_text(char *o, size_t cap, uint32_t m) {
    static const struct { uint32_t b; const char *n; } t[] = { { 1, "BASIC" }, { 2, "VOTE" }, { 4, "ARITH" }, { 8, "BALLOT" }, { 16, "SHUF" }, { 32, "SHUFREL" }, { 64, "CLUSTER" }, { 128, "QUAD" }, { 256, "PARTITIONED_NV" }, { 512, "ROT" } };
    size_t n = 0;
    o[0] = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0] && n + 1 < cap; i++) if (m & t[i].b) n += (size_t)snprintf(o + n, cap - n, "%s%s", n ? "+" : "", t[i].n);
    if (!o[0]) snprintf(o, cap, "none");
}

static void fmt_line(sink_t *s, VkPhysicalDevice pd, uint32_t mtl, VkFormat f, int pd2, int ext_cubic) {
    VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    if (pd2) vkGetPhysicalDeviceFormatProperties2(pd, f, &fp);
    else vkGetPhysicalDeviceFormatProperties(pd, f, &fp.formatProperties);
    const uint32_t o = fp.formatProperties.optimalTilingFeatures, l = fp.formatProperties.linearTilingFeatures;
    char cub[96] = "n/a(no VK_EXT_filter_cubic)";
    if (ext_cubic && pd2) {
        view_fmt_info_t vi = { .sType = (VkStructureType)ST_VIEW_FORMAT_INFO, .imageViewType = VK_IMAGE_VIEW_TYPE_2D };
        cubic_view_props_t cp = { .sType = (VkStructureType)ST_FILTER_CUBIC_PROPS };
        VkPhysicalDeviceImageFormatInfo2 ii = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &vi, .format = f, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
        VkImageFormatProperties2 ip = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &cp };
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip);
        if (r == VK_SUCCESS) snprintf(cub, sizeof cub, "view2d filterCubic=%d filterCubicMinmax=%d", (int)cp.filterCubic, (int)cp.filterCubicMinmax);
        else snprintf(cub, sizeof cub, "view2d query vk=%d", (int)r);
    }
    put(s, "fmt mtl=%u vk=%d optimal=0x%x [COLOR_ATTACHMENT=%d COLOR_ATTACHMENT_BLEND=%d SAMPLED_IMAGE=%d SAMPLED_IMAGE_FILTER_LINEAR=%d SAMPLED_IMAGE_FILTER_CUBIC=%d TRANSFER_SRC=%d TRANSFER_DST=%d] linear=0x%x [COLOR_ATTACHMENT=%d SAMPLED_IMAGE=%d] %s",
        mtl, (int)f, o, !!(o & FF_COLOR_ATTACHMENT), !!(o & FF_COLOR_ATTACHMENT_BLEND), !!(o & FF_SAMPLED_IMAGE), !!(o & FF_SAMPLED_FILTER_LINEAR), !!(o & FF_SAMPLED_FILTER_CUBIC),
        !!(o & FF_TRANSFER_SRC), !!(o & FF_TRANSFER_DST), l, !!(l & FF_COLOR_ATTACHMENT), !!(l & FF_SAMPLED_IMAGE), cub);
}

/* ---- descriptor limits / MSAA / memory types / supported-vs-enabled features (new lines only) ---- */
static void sc_text(char *o, size_t cap, uint32_t m) {
    size_t n = 0;
    o[0] = 0;
    for (uint32_t b = 1; b <= 64 && n + 4 < cap; b <<= 1) if (m & b) n += (size_t)snprintf(o + n, cap - n, "%sx%u", n ? "+" : "", b);
    if (!o[0]) snprintf(o, cap, "none");
}

static int en_ext(const lagfx_vkdev_enabled_t *en, const char *name) {   /* membership in the REAL ppEnabledExtensionNames array; -2 = not recorded */
    if (!en || !en->exts) return -2;
    for (uint32_t i = 0; i < en->n_exts; i++) if (en->exts[i] && !strcmp(en->exts[i], name)) return 1;
    return 0;
}
static const char *ev(char *b, int v) { if (v < 0) return "not-recorded"; snprintf(b, 16, "%d", v); return b; }   /* -1 / -2 = not recorded */

static void extra_lines(sink_t *s, VkPhysicalDevice pd, uint32_t api, const VkExtensionProperties *ex, uint32_t ne, const lagfx_vkdev_enabled_t *en_p, const VkPhysicalDeviceProperties *p) {
    const uint32_t amaj = VK_API_VERSION_MAJOR(api), amin = VK_API_VERSION_MINOR(api);
    const int pd2 = amaj > 1 || amin >= 1, core12 = amaj > 1 || amin >= 2, core13 = amaj > 1 || amin >= 3;
    const VkPhysicalDeviceLimits *L = &p->limits;
    /* (a) descriptor limits */
    put(s, "desc_limits maxBoundDescriptorSets=%u perStage[StorageBuffers=%u SampledImages=%u Samplers=%u InputAttachments=%u Resources=%u UniformBuffers=%u StorageImages=%u] set[StorageBuffers=%u SampledImages=%u Samplers=%u InputAttachments=%u] maxFragmentInputComponents=%u maxFragmentOutputAttachments=%u maxFragmentCombinedOutputResources=%u maxColorAttachments=%u",
        L->maxBoundDescriptorSets, L->maxPerStageDescriptorStorageBuffers, L->maxPerStageDescriptorSampledImages, L->maxPerStageDescriptorSamplers, L->maxPerStageDescriptorInputAttachments,
        L->maxPerStageResources, L->maxPerStageDescriptorUniformBuffers, L->maxPerStageDescriptorStorageImages, L->maxDescriptorSetStorageBuffers, L->maxDescriptorSetSampledImages,
        L->maxDescriptorSetSamplers, L->maxDescriptorSetInputAttachments, L->maxFragmentInputComponents, L->maxFragmentOutputAttachments, L->maxFragmentCombinedOutputResources, L->maxColorAttachments);
    const int ext_di = ex && has_ext(ex, ne, "VK_EXT_descriptor_indexing");
    uint32_t uab[8] = { 0 };   /* perStage: samplers, sampledImages, storageBuffers, inputAttachments, resources; set: storageBuffers, sampledImages, inputAttachments */
    int have_uab = 0;
    if (pd2 && (core12 || ext_di)) {
        VkPhysicalDeviceDescriptorIndexingProperties di = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceProperties2 qp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &di };
        vkGetPhysicalDeviceProperties2(pd, &qp);
        uab[0] = di.maxPerStageDescriptorUpdateAfterBindSamplers, uab[1] = di.maxPerStageDescriptorUpdateAfterBindSampledImages, uab[2] = di.maxPerStageDescriptorUpdateAfterBindStorageBuffers;
        uab[3] = di.maxPerStageDescriptorUpdateAfterBindInputAttachments, uab[4] = di.maxPerStageUpdateAfterBindResources, uab[5] = di.maxDescriptorSetUpdateAfterBindStorageBuffers;
        uab[6] = di.maxDescriptorSetUpdateAfterBindSampledImages, uab[7] = di.maxDescriptorSetUpdateAfterBindInputAttachments;
        have_uab = 1;
        put(s, "desc_limits_uab perStage[Samplers=%u SampledImages=%u StorageBuffers=%u InputAttachments=%u Resources=%u] set[StorageBuffers=%u SampledImages=%u InputAttachments=%u] maxUpdateAfterBindDescriptorsInAllPools=%u",
            uab[0], uab[1], uab[2], uab[3], uab[4], uab[5], uab[6], uab[7], di.maxUpdateAfterBindDescriptorsInAllPools);
    } else put(s, "desc_limits_uab n/a (no core 1.2 / VK_EXT_descriptor_indexing)");
    /* The failing PSO layout (bundle pipefail_50a6804e2e421a09 / b7d3e8365670fd6c state file): 1 set; per binding type: STORAGE_BUFFER x13 (stage VS|FS = 17), SAMPLED_IMAGE x12 (17),
     * SAMPLER x3 (17), INPUT_ATTACHMENT x2 (FS = 16); 2 colour attachments. Counts are DERIVED from this table. Assumptions printed in the line: descriptorCount=1 per binding, no UPDATE_AFTER_BIND.
     * maxPerStageResources (spec, limits chapter): plain SAMPLER descriptors are NOT counted, sampled images / storage buffers / input attachments are, and in the fragment stage the
     * framebuffer colour attachments count too. */
    {
        static const struct { VkDescriptorType type; const char *n; uint32_t count, stages; } tab[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "StorageBuffers", 13, 1u | 16u }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, "SampledImages", 12, 1u | 16u },
            { VK_DESCRIPTOR_TYPE_SAMPLER, "Samplers", 3, 1u | 16u }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, "InputAttachments", 2, 16u } };
        const uint32_t color_att = 2, lim[4] = { L->maxPerStageDescriptorStorageBuffers, L->maxPerStageDescriptorSampledImages, L->maxPerStageDescriptorSamplers, L->maxPerStageDescriptorInputAttachments };
        const uint32_t setl[4] = { L->maxDescriptorSetStorageBuffers, L->maxDescriptorSetSampledImages, L->maxDescriptorSetSamplers, L->maxDescriptorSetInputAttachments };
        uint32_t cnt_vs = 0, cnt_fs = 0, used_vs = 0, used_fs = 0, per_vs[4], per_fs[4], total = 0;
        for (int i = 0; i < 4; i++) {
            per_vs[i] = (tab[i].stages & 1u) ? tab[i].count : 0, per_fs[i] = (tab[i].stages & 16u) ? tab[i].count : 0;
            cnt_vs += per_vs[i], cnt_fs += per_fs[i], total += tab[i].count;
            if (tab[i].type != VK_DESCRIPTOR_TYPE_SAMPLER) used_vs += per_vs[i], used_fs += per_fs[i];
        }
        used_fs += color_att;
        char o[640];
        size_t n = (size_t)snprintf(o, sizeof o, "layout_vs_limits layout=1set/%ubindings(13SB,12SI,3S,2IA) assumes descriptorCount=1 per binding, no UPDATE_AFTER_BIND; fragment[", total);
        for (int i = 0; i < 4; i++) n += (size_t)snprintf(o + n, sizeof o - n, "%s%s %u<=%u:%d", i ? " " : "", tab[i].n, per_fs[i], lim[i], per_fs[i] <= lim[i]);
        n += (size_t)snprintf(o + n, sizeof o - n, "] vertex[");
        for (int i = 0; i < 4; i++) n += (size_t)snprintf(o + n, sizeof o - n, "%s%s %u<=%u:%d", i ? " " : "", tab[i].n, per_vs[i], lim[i], per_vs[i] <= lim[i]);
        n += (size_t)snprintf(o + n, sizeof o - n, "] set[");
        for (int i = 0; i < 4; i++) n += (size_t)snprintf(o + n, sizeof o - n, "%s%s %u<=%u:%d", i ? " " : "", tab[i].n, tab[i].count, setl[i], tab[i].count <= setl[i]);
        n += (size_t)snprintf(o + n, sizeof o - n, "] colorOut %u<=%u:%d", color_att, L->maxFragmentOutputAttachments, color_att <= L->maxFragmentOutputAttachments);
        put(s, "%s", o);
        put(s, "layout_vs_limits_res descriptor_count[VS=%u FS=%u] (all bindings, descriptorCount=1) maxPerStageResources_used[VS=%u FS=%u] (SAMPLER not counted; FS = storage+sampled+input attachments + %u colour attachments) vs maxPerStageResources=%u: VS %u<=%u:%d FS %u<=%u:%d | fsOutputResources %u<=%u:%d",
            cnt_vs, cnt_fs, used_vs, used_fs, color_att, L->maxPerStageResources, used_vs, L->maxPerStageResources, used_vs <= L->maxPerStageResources, used_fs, L->maxPerStageResources, used_fs <= L->maxPerStageResources,
            color_att, L->maxFragmentCombinedOutputResources, color_att <= L->maxFragmentCombinedOutputResources);
        if (have_uab) put(s, "layout_vs_limits_uab (meaningful only if the layout used UPDATE_AFTER_BIND; the recorded layout has no binding flags) fragment[StorageBuffers %u<=%u:%d SampledImages %u<=%u:%d Samplers %u<=%u:%d InputAttachments %u<=%u:%d] maxPerStageUpdateAfterBindResources_used[FS=%u] %u<=%u:%d",
            per_fs[0], uab[2], per_fs[0] <= uab[2], per_fs[1], uab[1], per_fs[1] <= uab[1], per_fs[2], uab[0], per_fs[2] <= uab[0], per_fs[3], uab[3], per_fs[3] <= uab[3], used_fs, used_fs, uab[4], used_fs <= uab[4]);
    }
    /* (b) MSAA */
    {
        char a[120], b[120];
        sc_text(a, sizeof a, L->framebufferColorSampleCounts), sc_text(b, sizeof b, L->sampledImageColorSampleCounts);
        put(s, "msaa limits framebufferColorSampleCounts=0x%x(%s) sampledImageColorSampleCounts=0x%x(%s) framebufferNoAttachmentsSampleCounts=0x%x", L->framebufferColorSampleCounts, a, L->sampledImageColorSampleCounts, b, L->framebufferNoAttachmentsSampleCounts);
        static const struct { uint32_t mtl; VkFormat f; } fm[2] = { { 80, VK_FORMAT_B8G8R8A8_UNORM }, { 115, VK_FORMAT_R16G16B16A16_SFLOAT } };
        for (int i = 0; i < 2; i++) {
            VkImageFormatProperties ca, sm, fb;
            VkResult r1 = vkGetPhysicalDeviceImageFormatProperties(pd, fm[i].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ca);
            VkResult r2 = vkGetPhysicalDeviceImageFormatProperties(pd, fm[i].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &sm);
            VkResult r3 = vkGetPhysicalDeviceImageFormatProperties(pd, fm[i].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, 0, &fb);
            char t1[80], t2[80], t3[80];
            sc_text(t1, sizeof t1, r1 == VK_SUCCESS ? ca.sampleCounts : 0), sc_text(t2, sizeof t2, r2 == VK_SUCCESS ? sm.sampleCounts : 0), sc_text(t3, sizeof t3, r3 == VK_SUCCESS ? fb.sampleCounts : 0);
            put(s, "msaa fmt mtl=%u vk=%d colorAttachment[vk=%d counts=0x%x(%s)] sampled[vk=%d counts=0x%x(%s)] colorAttachment+inputAttachment[vk=%d counts=0x%x(%s)] (2D, optimal tiling, vkGetPhysicalDeviceImageFormatProperties)",
                fm[i].mtl, (int)fm[i].f, (int)r1, r1 == VK_SUCCESS ? ca.sampleCounts : 0, t1, (int)r2, r2 == VK_SUCCESS ? sm.sampleCounts : 0, t2, (int)r3, r3 == VK_SUCCESS ? fb.sampleCounts : 0, t3);
        }
    }
    /* (c) memory types (the library's selection code is only noted in the report, not printed) */
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        char o[620];
        size_t n = 0;
        uint32_t part = 0;
        o[0] = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++) {
            char t[200];
            const uint32_t f = mp.memoryTypes[i].propertyFlags;
            int w = snprintf(t, sizeof t, "type%u[heap=%u 0x%x%s%s%s%s%s%s] ", i, mp.memoryTypes[i].heapIndex, f, (f & 1) ? " DEVICE_LOCAL" : "", (f & 2) ? " HOST_VISIBLE" : "", (f & 4) ? " HOST_COHERENT" : "",
                             (f & 8) ? " HOST_CACHED" : "", (f & 16) ? " LAZILY_ALLOCATED" : "", (f & ~31u) ? " +other" : "");
            if (n && n + (size_t)w > 560) { put(s, "mem types n=%u part=%u %s", mp.memoryTypeCount, ++part, o); n = 0, o[0] = 0; }
            memcpy(o + n, t, (size_t)w + 1), n += (size_t)w;
        }
        if (n) put(s, "mem types n=%u part=%u %s", mp.memoryTypeCount, ++part, o);
        n = 0, o[0] = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
            n += (size_t)snprintf(o + n, sizeof o - n, "heap%u[%llu MiB flags=0x%x%s] ", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags, (mp.memoryHeaps[i].flags & 1) ? " DEVICE_LOCAL" : "");
        put(s, "mem heaps n=%u %s", mp.memoryHeapCount, o);
    }
    /* (d) supported vs ENABLED. extension_*: name in the device's list / in the REAL ppEnabledExtensionNames array given to vkCreateDevice. feature_*: queried here / the value in the feature
     * structs given to vkCreateDevice (lagfx_vkdev_enabled_t, filled in instance.c from feat2/feat11/feat12/feat13/lr_en/ro_en). Not derived from extension names. */
    {
        VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        if (pd2) {
            void *chain = NULL;
            if (core12) { f12.pNext = chain; chain = &f12; f11.pNext = chain; chain = &f11; }
            if (core13) { f13.pNext = chain; chain = &f13; }
            q.pNext = chain;
            vkGetPhysicalDeviceFeatures2(pd, &q);
        }
        const int ext_lr = ex && has_ext(ex, ne, "VK_KHR_dynamic_rendering_local_read"), ro_e = ex && has_ext(ex, ne, "VK_EXT_rasterization_order_attachment_access"), ro_a = ex && has_ext(ex, ne, "VK_ARM_rasterization_order_attachment_access");
        local_read_feat_t lr = { .sType = (VkStructureType)ST_LOCAL_READ_FEATURES };
        raster_order_feat_t ro = { .sType = (VkStructureType)ST_RASTER_ORDER_FEATURES };
        tile_image_feat_t ti = { .sType = (VkStructureType)ST_SHADER_TILE_IMAGE_FEATURES };
        const int core14 = amaj > 1 || amin >= 4, ext_ti = ex && has_ext(ex, ne, "VK_EXT_shader_tile_image");
        if (pd2 && (core14 || ext_lr)) { VkPhysicalDeviceFeatures2 t = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &lr }; vkGetPhysicalDeviceFeatures2(pd, &t); }
        if (pd2 && (ro_e || ro_a)) { VkPhysicalDeviceFeatures2 t = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ro }; vkGetPhysicalDeviceFeatures2(pd, &t); }
        if (pd2 && ext_ti) { VkPhysicalDeviceFeatures2 t = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ti }; vkGetPhysicalDeviceFeatures2(pd, &t); }
        /* extensions */
        {
            static const char *const en[] = { "VK_KHR_dynamic_rendering", "VK_KHR_dynamic_rendering_local_read", "VK_EXT_rasterization_order_attachment_access", "VK_ARM_rasterization_order_attachment_access",
                                              "VK_EXT_shader_tile_image", "VK_QCOM_tile_shading", "VK_EXT_filter_cubic", "VK_EXT_subgroup_size_control", "VK_KHR_variable_pointers", "VK_KHR_shader_float16_int8", "VK_KHR_8bit_storage", "VK_KHR_16bit_storage" };
            char o[620];
            size_t n = 0;
            uint32_t part = 0;
            o[0] = 0;
            for (size_t i = 0; i < sizeof en / sizeof en[0]; i++) {
                char eb[16], t[200];
                int w = snprintf(t, sizeof t, "%s extension_supported=%d extension_enabled=%s; ", en[i], ex ? has_ext(ex, ne, en[i]) : 0, ev(eb, en_ext(en_p, en[i])));
                if (n && n + (size_t)w > 560) { put(s, "ext_state part=%u %s", ++part, o); n = 0, o[0] = 0; }
                memcpy(o + n, t, (size_t)w + 1), n += (size_t)w;
            }
            if (n) put(s, "ext_state part=%u %s", ++part, o);
        }
        struct { const char *n; int sup; int en; } r[] = {
            { "dynamicRendering", core13 ? (int)f13.dynamicRendering : 0, en_p ? en_p->dynamicRendering : -1 },
            { "dynamicRenderingLocalRead", (core14 || ext_lr) ? (int)lr.dynamicRenderingLocalRead : 0, en_p ? en_p->dynamicRenderingLocalRead : -1 },
            { "rasterizationOrderColorAttachmentAccess", (ro_e || ro_a) ? (int)ro.color : 0, en_p ? en_p->rasterizationOrderColorAttachmentAccess : -1 },
            { "shaderTileImageColorReadAccess", ext_ti ? (int)ti.shaderTileImageColorReadAccess : 0, en_p ? en_p->shaderTileImageColorReadAccess : -1 },
            { "shaderTileImageDepthReadAccess", ext_ti ? (int)ti.shaderTileImageDepthReadAccess : 0, en_p ? en_p->shaderTileImageDepthReadAccess : -1 },
            { "shaderTileImageStencilReadAccess", ext_ti ? (int)ti.shaderTileImageStencilReadAccess : 0, en_p ? en_p->shaderTileImageStencilReadAccess : -1 },
            { "subgroupSizeControl", core13 ? (int)f13.subgroupSizeControl : 0, en_p ? en_p->subgroupSizeControl : -1 },
            { "computeFullSubgroups", core13 ? (int)f13.computeFullSubgroups : 0, en_p ? en_p->computeFullSubgroups : -1 },
            { "shaderFloat16", core12 ? (int)f12.shaderFloat16 : 0, en_p ? en_p->shaderFloat16 : -1 },
            { "shaderInt64", (int)q.features.shaderInt64, en_p ? en_p->shaderInt64 : -1 },
            { "shaderInt8", core12 ? (int)f12.shaderInt8 : 0, en_p ? en_p->shaderInt8 : -1 },
            { "storageBuffer16BitAccess", core12 ? (int)f11.storageBuffer16BitAccess : 0, en_p ? en_p->storageBuffer16BitAccess : -1 },
            { "storageBuffer8BitAccess", core12 ? (int)f12.storageBuffer8BitAccess : 0, en_p ? en_p->storageBuffer8BitAccess : -1 },
            { "variablePointers", core12 ? (int)f11.variablePointers : 0, en_p ? en_p->variablePointers : -1 },
            { "variablePointersStorageBuffer", core12 ? (int)f11.variablePointersStorageBuffer : 0, en_p ? en_p->variablePointersStorageBuffer : -1 },
        };
        char o[620];
        size_t n = 0;
        uint32_t part = 0;
        o[0] = 0;
        for (size_t i = 0; i < sizeof r / sizeof r[0]; i++) {
            char eb[16], t[200];
            int w = snprintf(t, sizeof t, "%s feature_supported=%d feature_enabled=%s; ", r[i].n, r[i].sup, ev(eb, r[i].en));
            if (n && n + (size_t)w > 560) { put(s, "feat part=%u %s", ++part, o); n = 0, o[0] = 0; }
            memcpy(o + n, t, (size_t)w + 1), n += (size_t)w;
        }
        if (n) put(s, "feat part=%u %s", ++part, o);
        /* the complete enabled-extension list from the real array (no fixed-size string) */
        if (!en_p || !en_p->exts) put(s, "feat enabled_exts=not-recorded");
        else {
            uint32_t i = 0, k = 0;
            put(s, "feat enabled_exts n=%u (from the ppEnabledExtensionNames array passed to vkCreateDevice)", en_p->n_exts);
            while (i < en_p->n_exts) {
                char l[620];
                size_t m = 0;
                l[0] = 0;
                while (i < en_p->n_exts && m + strlen(en_p->exts[i]) + 2 < 560) m += (size_t)snprintf(l + m, sizeof l - m, "%s%s", m ? " " : "", en_p->exts[i]), i++;
                if (!m) break;
                put(s, "feat enabled_exts part=%u %s", ++k, l);
            }
        }
        put(s, "feat note: feature_enabled = value in the feature structs given to vkCreateDevice (not derived from extension names; core-promoted features need no extension string); extension_enabled = name in ppEnabledExtensionNames; not-recorded = the caller passed no enabled set");
    }
}

size_t lagfx_vk_devinfo_collect_ex(VkPhysicalDevice pd, const lagfx_vkdev_enabled_t *en, lagfx_vkdev_emit_fn emit, void *user) {
    sink_t s = { emit, user, 0, 0, LAGFX_VKDEV_MAX_LINES, 0 };
    if (!pd || !emit) return 0;
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    vkGetPhysicalDeviceProperties(pd, &p);
    const uint32_t amaj = VK_API_VERSION_MAJOR(p.apiVersion), amin = VK_API_VERSION_MINOR(p.apiVersion);
    const int pd2 = amaj > 1 || amin >= 1, core13 = amaj > 1 || amin >= 3, core14 = amaj > 1 || amin >= 4;

    /* extensions (full list) */
    uint32_t ne = 0;
    VkExtensionProperties *ex = NULL;
    VkResult er = vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    if (er == VK_SUCCESS && ne) {
        ex = calloc(ne, sizeof *ex);
        if (ex) { er = vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex); if (er != VK_SUCCESS && er != VK_INCOMPLETE) ne = 0; }
        else ne = 0;
        if (ex && ne) qsort(ex, ne, sizeof *ex, cmp_ext);
    }

    /* identity */
    {
        char nm[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE], dn[VK_MAX_DRIVER_NAME_SIZE] = "", di[VK_MAX_DRIVER_INFO_SIZE] = "";
        char tn[64];
        int drvid = 0;
        uint32_t cm = 0, cn = 0, cs = 0, cp = 0;
        san(nm, sizeof nm, p.deviceName);
        if (pd2) {
            VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
            vkGetPhysicalDeviceProperties2(pd, &pp);
            san(dn, sizeof dn, drv.driverName), san(di, sizeof di, drv.driverInfo);
            drvid = (int)drv.driverID, cm = drv.conformanceVersion.major, cn = drv.conformanceVersion.minor, cs = drv.conformanceVersion.subminor, cp = drv.conformanceVersion.patch;
        }
        static const char *const types[] = { "other", "integrated", "discrete", "virtual", "cpu" };
        snprintf(tn, sizeof tn, "%s", p.deviceType <= 4 ? types[p.deviceType] : "unknown");
        put(&s, "id name='%s' type=%s vendor=0x%x device=0x%x api=%u.%u.%u driverVersion=0x%x driverName='%s' driverInfo='%.160s' driverID=%d conformance=%u.%u.%u.%u maxColorAttachments=%u maxInputAttachments=%u",
            nm, tn, p.vendorID, p.deviceID, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, dn, di, drvid, cm, cn, cs, cp,
            p.limits.maxColorAttachments, p.limits.maxPerStageDescriptorInputAttachments);
    }
    if (er != VK_SUCCESS && er != VK_INCOMPLETE && !ne) put(&s, "exts enumeration failed vk=%d", (int)er);
    else {   /* split into lines: tag + "exts n=%u part=%u/%u " header + names */
        const size_t budget = LAGFX_VKDEV_LINE_MAX - 1 - 48;
        char (*tok)[VK_MAX_EXTENSION_NAME_SIZE + 16] = ne ? malloc((size_t)ne * sizeof *tok) : NULL;
        uint32_t *start = malloc(((size_t)ne + 2) * sizeof *start), parts = 0;
        if (ne && (!tok || !start)) put(&s, "exts n=%u (out of memory building the list)", ne);
        else {
            size_t cur = 0;
            for (uint32_t i = 0; i < ne; i++) {   /* pass 1: tokens + part boundaries */
                size_t w = (size_t)snprintf(tok[i], sizeof tok[i], "%s/%u", ex[i].extensionName, ex[i].specVersion) + 1;
                if (!i || cur + w > budget) start[parts++] = i, cur = 0;
                cur += w;
            }
            start[parts] = ne;
            if (!ne) put(&s, "exts n=0");
            for (uint32_t k = 0; k < parts; k++) {
                char line[LAGFX_VKDEV_LINE_MAX];
                size_t o = 0;
                line[0] = 0;
                for (uint32_t i = start[k]; i < start[k + 1]; i++) o += (size_t)snprintf(line + o, sizeof line - o, "%s%s", i > start[k] ? " " : "", tok[i]);
                put(&s, "exts n=%u part=%u/%u %s", ne, k + 1, parts, line);
            }
        }
        free(tok), free(start);
    }

    /* shader tile image */
    if (ex && has_ext(ex, ne, "VK_EXT_shader_tile_image") && pd2) {
        tile_image_feat_t f = { .sType = (VkStructureType)ST_SHADER_TILE_IMAGE_FEATURES };
        tile_image_prop_t pr = { .sType = (VkStructureType)ST_SHADER_TILE_IMAGE_PROPERTIES };
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f };
        VkPhysicalDeviceProperties2 qp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pr };
        vkGetPhysicalDeviceFeatures2(pd, &q);
        vkGetPhysicalDeviceProperties2(pd, &qp);
        put(&s, "shader_tile_image ext=1 shaderTileImageColorReadAccess=%d shaderTileImageDepthReadAccess=%d shaderTileImageStencilReadAccess=%d coherentReadAccelerated=%d readSampleFromPixelRateInvocation=%d readFromHelperInvocation=%d",
            (int)f.shaderTileImageColorReadAccess, (int)f.shaderTileImageDepthReadAccess, (int)f.shaderTileImageStencilReadAccess, (int)pr.shaderTileImageCoherentReadAccelerated,
            (int)pr.shaderTileImageReadSampleFromPixelRateInvocation, (int)pr.shaderTileImageReadFromHelperInvocation);
    } else put(&s, "shader_tile_image ext=0");

    /* QCOM tile shading */
    if (ex && has_ext(ex, ne, "VK_QCOM_tile_shading") && pd2) {
        tile_shading_feat_t f = { .sType = (VkStructureType)ST_TILE_SHADING_FEATURES_QCOM };
        tile_shading_prop_t pr = { .sType = (VkStructureType)ST_TILE_SHADING_PROPERTIES_QCOM };
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f };
        VkPhysicalDeviceProperties2 qp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pr };
        vkGetPhysicalDeviceFeatures2(pd, &q);
        vkGetPhysicalDeviceProperties2(pd, &qp);
        put(&s, "qcom_tile_shading ext=1 tileShading=%d fragmentStage=%d colorAtt=%d depthAtt=%d stencilAtt=%d inputAtt=%d sampledAtt=%d perTileDraw=%d perTileDispatch=%d dispatchTile=%d apron=%d anisoApron=%d atomicOps=%d imageProcessing=%d maxApronSize=%u preferNonCoherent=%d tileGranularity=%ux%u maxTileShadingRate=%ux%u",
            (int)f.tileShading, (int)f.tileShadingFragmentStage, (int)f.tileShadingColorAttachments, (int)f.tileShadingDepthAttachments, (int)f.tileShadingStencilAttachments, (int)f.tileShadingInputAttachments,
            (int)f.tileShadingSampledAttachments, (int)f.tileShadingPerTileDraw, (int)f.tileShadingPerTileDispatch, (int)f.tileShadingDispatchTile, (int)f.tileShadingApron, (int)f.tileShadingAnisotropicApron,
            (int)f.tileShadingAtomicOps, (int)f.tileShadingImageProcessing, pr.maxApronSize, (int)pr.preferNonCoherent, pr.tileGranularity.width, pr.tileGranularity.height, pr.maxTileShadingRate.width, pr.maxTileShadingRate.height);
    } else put(&s, "qcom_tile_shading ext=0");

    /* filter cubic: extension presence (the per-format part is in the fmt lines) */
    const int ext_cubic = ex && (has_ext(ex, ne, "VK_EXT_filter_cubic") || has_ext(ex, ne, "VK_IMG_filter_cubic"));
    put(&s, "filter_cubic VK_EXT_filter_cubic=%d VK_IMG_filter_cubic=%d VK_QCOM_filter_cubic_clamp=%d VK_QCOM_filter_cubic_weights=%d (per-format SAMPLED_IMAGE_FILTER_CUBIC in the fmt lines)",
        ex ? has_ext(ex, ne, "VK_EXT_filter_cubic") : 0, ex ? has_ext(ex, ne, "VK_IMG_filter_cubic") : 0, ex ? has_ext(ex, ne, "VK_QCOM_filter_cubic_clamp") : 0, ex ? has_ext(ex, ne, "VK_QCOM_filter_cubic_weights") : 0);

    /* dynamic rendering local read (core in 1.4, else the KHR extension) + rasterization order access */
    {
        const int ext_lr = ex && has_ext(ex, ne, "VK_KHR_dynamic_rendering_local_read");
        local_read_feat_t lr = { .sType = (VkStructureType)ST_LOCAL_READ_FEATURES };
        if (pd2 && (core14 || ext_lr)) {
            VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &lr };
            vkGetPhysicalDeviceFeatures2(pd, &q);
        }
        put(&s, "dyn_local_read core1.4=%d ext=%d dynamicRenderingLocalRead=%d dynamicRendering_core1.3=%d", core14, ext_lr, (core14 || ext_lr) ? (int)lr.dynamicRenderingLocalRead : 0, core13);
        const int ro_e = ex && has_ext(ex, ne, "VK_EXT_rasterization_order_attachment_access"), ro_a = ex && has_ext(ex, ne, "VK_ARM_rasterization_order_attachment_access");
        raster_order_feat_t ro = { .sType = (VkStructureType)ST_RASTER_ORDER_FEATURES };
        if (pd2 && (ro_e || ro_a)) {
            VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ro };
            vkGetPhysicalDeviceFeatures2(pd, &q);
        }
        put(&s, "raster_order ext_EXT=%d ext_ARM=%d colorAttachmentAccess=%d depthAccess=%d stencilAccess=%d", ro_e, ro_a, (ro_e || ro_a) ? (int)ro.color : 0, (ro_e || ro_a) ? (int)ro.depth : 0, (ro_e || ro_a) ? (int)ro.stencil : 0);
    }

    /* subgroup (core 1.1 properties) */
    if (pd2) {
        VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 qp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
        vkGetPhysicalDeviceProperties2(pd, &qp);
        char st[96], op[200];
        stages_text(st, sizeof st, sg.supportedStages), ops_text(op, sizeof op, sg.supportedOperations);
        put(&s, "subgroup size=%u stages=0x%x(%s) operations=0x%x(%s) quadOperationsInAllStages=%d", sg.subgroupSize, (unsigned)sg.supportedStages, st, (unsigned)sg.supportedOperations, op, (int)sg.quadOperationsInAllStages);
    } else put(&s, "subgroup n/a (api < 1.1)");

    /* formats: Metal 80 (BGRA8Unorm) -> VK 44, Metal 115 (RGBA16Float) -> VK 97 */
    fmt_line(&s, pd, 80, VK_FORMAT_B8G8R8A8_UNORM, pd2, ext_cubic);
    fmt_line(&s, pd, 115, VK_FORMAT_R16G16B16A16_SFLOAT, pd2, ext_cubic);
    extra_lines(&s, pd, p.apiVersion, ex, ne, en, &p);
    if (s.overflow) { s.cap++; put(&s, "truncated: line cap %u reached, omitted_lines=%zu", LAGFX_VKDEV_MAX_LINES, s.dropped); }
    free(ex);
    return s.n;
}

size_t lagfx_vk_devinfo_collect(VkPhysicalDevice pd, lagfx_vkdev_emit_fn emit, void *user) { return lagfx_vk_devinfo_collect_ex(pd, NULL, emit, user); }

static void warn_emit(void *u, const char *line) { (void)u; LAGFX_WARN("%s", line); }

size_t lagfx_vk_devinfo_log_once_ex(VkPhysicalDevice pd, const lagfx_vkdev_enabled_t *en) {
    static int done;
    if (done) return 0;
    done = 1;
    return lagfx_vk_devinfo_collect_ex(pd, en, warn_emit, NULL);
}
size_t lagfx_vk_devinfo_log_once(VkPhysicalDevice pd) { return lagfx_vk_devinfo_log_once_ex(pd, NULL); }

void lagfx_vk_devinfo_note_staging(VkPhysicalDevice pd, uint32_t chosen, uint32_t bits, uint64_t size, uint32_t usage) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t seen[4], nseen;
    if (!pd) return;
    pthread_mutex_lock(&mu);
    for (uint32_t i = 0; i < nseen; i++) if (seen[i] == chosen) { pthread_mutex_unlock(&mu); return; }
    if (nseen >= 4) { pthread_mutex_unlock(&mu); return; }
    seen[nseen++] = chosen;
    pthread_mutex_unlock(&mu);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (chosen == UINT32_MAX || chosen >= mp.memoryTypeCount) { LAGFX_WARN("g7-vkdev: staging make_buf memoryTypeIndex=none(no type matched HOST_VISIBLE|HOST_COHERENT) memoryTypeBits=0x%x size=%llu usage=0x%x", bits, (unsigned long long)size, usage); return; }
    const uint32_t f = mp.memoryTypes[chosen].propertyFlags;
    LAGFX_WARN("g7-vkdev: staging make_buf memoryTypeIndex=%u heap=%u propertyFlags=0x%x[%s%s%s%s%s] memoryTypeBits=0x%x size=%llu usage=0x%x (first allocation with this type; selection = first index in memoryTypeBits with HOST_VISIBLE|HOST_COHERENT)",
               chosen, mp.memoryTypes[chosen].heapIndex, f, (f & 1) ? " DEVICE_LOCAL" : "", (f & 2) ? " HOST_VISIBLE" : "", (f & 4) ? " HOST_COHERENT" : "", (f & 8) ? " HOST_CACHED" : "", (f & 16) ? " LAZILY_ALLOCATED" : "",
               bits, (unsigned long long)size, usage);
}
#endif /* LAGFX_HAVE_VULKAN */
