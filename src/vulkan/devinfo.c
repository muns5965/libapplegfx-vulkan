/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — read-only device identity / extension / feature diagnostic (see devinfo.h).
 * src/vulkan/devinfo.c
 */
#ifdef LAGFX_HAVE_VULKAN
#include "devinfo.h"
#include "common/log.h"

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

/* format feature bits used below (core values; written as numbers because the cubic bit is named differently across header generations) */
#define FF_SAMPLED_IMAGE 0x0001u
#define FF_COLOR_ATTACHMENT 0x0080u
#define FF_COLOR_ATTACHMENT_BLEND 0x0100u
#define FF_TRANSFER_SRC 0x4000u
#define FF_TRANSFER_DST 0x8000u
#define FF_SAMPLED_FILTER_LINEAR 0x1000u
#define FF_SAMPLED_FILTER_CUBIC 0x2000u

typedef struct { lagfx_vkdev_emit_fn fn; void *user; size_t n; int overflow; size_t cap; } sink_t;

static void put(sink_t *s, const char *fmt, ...) {
    char buf[LAGFX_VKDEV_LINE_MAX + 64];
    if (s->n >= s->cap) { s->overflow = 1; return; }
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

size_t lagfx_vk_devinfo_collect(VkPhysicalDevice pd, lagfx_vkdev_emit_fn emit, void *user) {
    sink_t s = { emit, user, 0, 0, LAGFX_VKDEV_MAX_LINES };
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
    if (s.overflow) { s.cap++; put(&s, "truncated: line cap %u reached, later lines dropped", LAGFX_VKDEV_MAX_LINES); }
    free(ex);
    return s.n;
}

static void warn_emit(void *u, const char *line) { (void)u; LAGFX_WARN("%s", line); }

size_t lagfx_vk_devinfo_log_once(VkPhysicalDevice pd) {
    static int done;
    if (done) return 0;
    done = 1;
    return lagfx_vk_devinfo_collect(pd, warn_emit, NULL);
}
#endif /* LAGFX_HAVE_VULKAN */
