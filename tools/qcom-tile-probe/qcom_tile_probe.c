/* SPDX-License-Identifier: MIT */
/*
 * qcom-tile-probe - standalone Vulkan probe for VK_QCOM_tile_shading (experiments E1..E6 of docs/qcom-tile-shading.md, section 7).
 *
 * Does not boot a VM and does not use libapplegfx-vulkan: it links only the Vulkan loader. All QCOM structs / commands / enums are declared
 * locally from vk.xml (VK_QCOM_tile_shading #310 rev 2, VK_QCOM_tile_properties #485) so the build header may predate the extension
 * (the host header here has the tile_properties structs but not tile_shading; NDK headers vary). Commands are loaded with vkGetDeviceProcAddr.
 *
 * Output contract (machine parsable, one line per experiment):
 *     qcomprobe: E<n> status=PASS|FAIL|UNSUPPORTED <key=value> <key=value> ...
 *   PASS         the path was exercised on the device and the result equals the CPU model
 *   FAIL         the path was attempted and was refused (VkResult in the line), crashed (device lost / timeout) or produced different pixels
 *   UNSUPPORTED  the extension / feature / command / subgroup capability needed is absent (nothing was attempted)
 * Extra lines:  "qcomprobe: info ..." (device record), "qcomprobe-detail: E<n> ..." (per-case rows), "qcomprobe: summary ..." (last line).
 * Exit status: 0 unless the probe could not start (2), a watchdog fired (3) or --strict and an experiment FAILed (1).
 *
 * Licence note: written from the Khronos registry / proposal text only. The qualcomm/qhwi sample sources (BSD-3-Clause-Clear, no patent
 * grant) were not read.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "qp_spv.h"

#define QP_VERSION 1

/* ------------------------------------------------------------------ local declarations (vk.xml) */
#define QP_ST_TILE_SHADING_FEATURES   1000309000
#define QP_ST_TILE_SHADING_PROPERTIES 1000309001
#define QP_ST_RP_TILE_SHADING_CREATE  1000309002
#define QP_ST_PER_TILE_BEGIN          1000309003
#define QP_ST_PER_TILE_END            1000309004
#define QP_ST_DISPATCH_TILE           1000309005
#define QP_ST_TILE_PROPS_FEATURES     1000484000
#define QP_ST_TILE_PROPS              1000484001
#define QP_ST_LOCAL_READ_FEATURES     1000232000
#define QP_TS_ENABLE_BIT   0x1u
#define QP_TS_PER_TILE_BIT 0x2u
#define QP_ACCESS2_TILE_READ  (1ULL << 51)
#define QP_ACCESS2_TILE_WRITE (1ULL << 52)

typedef struct { VkStructureType sType; void *pNext;
    VkBool32 tileShading, tileShadingFragmentStage, tileShadingColorAttachments, tileShadingDepthAttachments, tileShadingStencilAttachments,
             tileShadingInputAttachments, tileShadingSampledAttachments, tileShadingPerTileDraw, tileShadingPerTileDispatch, tileShadingDispatchTile,
             tileShadingApron, tileShadingAnisotropicApron, tileShadingAtomicOps, tileShadingImageProcessing; } QPTileShadingFeat;
typedef struct { VkStructureType sType; void *pNext; uint32_t maxApronSize; VkBool32 preferNonCoherent; VkExtent2D tileGranularity, maxTileShadingRate; } QPTileShadingProps;
typedef struct { VkStructureType sType; const void *pNext; uint32_t flags; VkExtent2D tileApronSize; } QPRPTileShading;
typedef struct { VkStructureType sType; const void *pNext; } QPPerTileInfo;   /* VkPerTileBeginInfoQCOM / VkPerTileEndInfoQCOM / VkDispatchTileInfoQCOM */
typedef struct { VkStructureType sType; void *pNext; VkBool32 tileProperties; } QPTilePropsFeat;
typedef struct { VkStructureType sType; void *pNext; VkExtent3D tileSize; VkExtent2D apronSize; VkOffset2D origin; } QPTileProps;
typedef struct { VkStructureType sType; void *pNext; VkBool32 dynamicRenderingLocalRead; } QPLocalReadFeat;
_Static_assert(sizeof(void *) != 8 || sizeof(QPTileShadingFeat) == 72, "VkPhysicalDeviceTileShadingFeaturesQCOM layout");
_Static_assert(sizeof(void *) != 8 || sizeof(QPTileShadingProps) == 40, "VkPhysicalDeviceTileShadingPropertiesQCOM layout");
_Static_assert(sizeof(void *) != 8 || sizeof(QPRPTileShading) == 32, "VkRenderPassTileShadingCreateInfoQCOM layout");
_Static_assert(sizeof(void *) != 8 || sizeof(QPTileProps) == 48, "VkTilePropertiesQCOM layout");

typedef VkResult (VKAPI_PTR *PFN_qpGetTileProps)(VkDevice, const VkRenderingInfo *, QPTileProps *);
typedef void (VKAPI_PTR *PFN_qpDispatchTile)(VkCommandBuffer, const QPPerTileInfo *);
typedef void (VKAPI_PTR *PFN_qpPerTile)(VkCommandBuffer, const QPPerTileInfo *);

/* ------------------------------------------------------------------ output / watchdog */
static char g_exp[16] = "setup";
static int g_mock;   /* --mock-tile: analysis self-test on a device WITHOUT tile shading: plain kernels, no render pass, no per-tile bracket. Every experiment line then carries mock=1. */
static int g_watch_s = 0;
static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fputc('\n', stdout); fflush(stdout); }

static void on_alarm(int sig) {
    (void)sig;
    char m[160]; int n = snprintf(m, sizeof m, "qcomprobe: %s status=FAIL reason=watchdog_timeout seconds=%d\nqcomprobe: summary aborted=1 watchdog=%s\n", g_exp, g_watch_s, g_exp);
    if (n > 0) { ssize_t r = write(1, m, (size_t)n); (void)r; }
    _exit(3);
}
static void arm(int sec) { g_watch_s = sec; alarm(sec > 0 ? (unsigned)sec : 0); }

typedef struct { char *p; size_t n, cap; } Str;
static void sadd(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sadd(Str *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap); va_end(ap);
    if (n < 0) return;
    s->n += (size_t)n; if (s->n >= s->cap) s->n = s->cap - 1;
}
#define STR_DECL(name, size) char name##_buf[size]; Str name = { name##_buf, 0, size }; name##_buf[0] = 0

/* ------------------------------------------------------------------ options / context */
typedef struct {
    int device, list, selftest, strict, no_risky, only_mask, rp_flags, child, no_isolate, e3_row;
    int exp_timeout_s, wait_ms;
    const char *dump_dir;
} Opts;

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkImageLayout layout; uint32_t w, h; VkFormat fmt; } QPImg;
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } QPBuf;

typedef struct {
    Opts o;
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue queue; uint32_t qf; VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties mp; VkPhysicalDeviceProperties props;
    uint32_t api;
    int lost;
    int ext_ts, ext_tp, ext_lr, ext_tmh;          /* listed by the device */
    int has_f13, f_dyn, f_s2, f_lr, f_wwf, f_rwf;  /* features reported */
    int qcom_enabled; VkResult qcom_create_res; int qcom_fallback;
    QPTileShadingFeat tsf;                         /* as reported */
    QPTilePropsFeat tpf;
    QPTileShadingProps tsp;
    uint32_t sg_size, sg_min, sg_max, sg_ops, sg_stages;
    PFN_qpGetTileProps getTileProps; PFN_qpDispatchTile cmdDispatchTile; PFN_qpPerTile cmdBeginPerTile, cmdEndPerTile;
    PFN_vkCmdBeginRendering cmdBeginRendering; PFN_vkCmdEndRendering cmdEndRendering; PFN_vkCmdPipelineBarrier2 cmdBarrier2;
    VkDescriptorSetLayout dsl; VkPipelineLayout pl_cs, pl_gfx; VkDescriptorPool dpool;
} QP;

/* ------------------------------------------------------------------ half / decode / model */
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, o;
    if (e == 0) { if (m == 0) o = s; else { e = 127 - 14; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; o = s | (e << 23) | (m << 13); } }
    else if (e == 31) o = s | 0x7f800000u | (m << 13);
    else o = s | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &o, 4); return f;
}
static uint16_t f2h(float f) {   /* round to nearest even */
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000, a = x & 0x7fffffffu;
    if (a >= 0x7f800000u) return (uint16_t)(s | 0x7c00 | (a > 0x7f800000u ? 0x200 : 0));
    if (a >= 0x477ff000u) return (uint16_t)(s | 0x7c00);     /* overflow */
    if (a < 0x33000001u) return (uint16_t)s;                  /* underflow to zero */
    int e = (int)(a >> 23) - 127;
    if (e < -14) {                                            /* subnormal */
        uint32_t m = (a & 0x7fffffu) | 0x800000u; int sh = -14 - e + 13;
        uint32_t r = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return (uint16_t)(s | r);
    }
    uint32_t m = a & 0x7fffffu, r = ((uint32_t)(e + 15) << 10) | (m >> 13), rem = m & 0x1fffu;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return (uint16_t)(s | r);
}

typedef struct { VkFormat fmt; const char *name; int bpp; } FmtInfo;
static const FmtInfo *fmt_info(VkFormat f) {
    static const FmtInfo t[] = {
        { VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4 }, { VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 4 },
        { VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 8 }, { VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT", 16 },
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM", 4 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].fmt == f) return &t[i];
    return NULL;
}
static void decode_px(VkFormat f, const uint8_t *p, float o[4]) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; break;
    case VK_FORMAT_B8G8R8A8_UNORM: o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t h[4]; memcpy(h, p, 8); for (int i = 0; i < 4; i++) o[i] = h2f(h[i]); } break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(o, p, 16); break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t v; memcpy(&v, p, 4); o[0] = (v & 1023) / 1023.0f; o[1] = ((v >> 10) & 1023) / 1023.0f; o[2] = ((v >> 20) & 1023) / 1023.0f; o[3] = (v >> 30) / 3.0f; } break;
    default: o[0] = o[1] = o[2] = o[3] = NAN;
    }
}
static float quant8(float v) { float c = v < 0 ? 0 : v > 1 ? 1 : v; return floorf(c * 255.0f + 0.5f) / 255.0f; }

/* pattern of the QK_PATTERN kernel (and the graphics fragment shader): CPU model */
static void pat_px(uint32_t x, uint32_t y, int div, float o[4]) {
    float sc = (div == 16) ? 0.0625f : (1.0f / 15.0f);
    o[0] = (float)(x & 15) * sc; o[1] = (float)(y & 15) * sc; o[2] = (float)((x >> 4) & 15) * sc; o[3] = 1.0f;
}
static void rmw_px(const float in[4], float o[4]) { o[0] = in[1] * 0.5f + 0.125f; o[1] = in[0] * 0.5f + 0.25f; o[2] = in[2]; o[3] = in[3]; }
static float simd_f(int64_t x, int64_t y) { int64_t t = x * 3 + y * 5; return (float)((t % 61) + 1); }

/* compare readback to model; exp = W*H*4 floats */
static int cmp_model(VkFormat f, const uint8_t *data, uint32_t W, uint32_t H, const float *ex, float tol, int *fx, int *fy, float got[4], float want[4]) {
    const FmtInfo *fi = fmt_info(f); int bad = 0;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float g[4]; decode_px(f, data + ((size_t)y * W + x) * (size_t)fi->bpp, g);
        const float *e = ex + ((size_t)y * W + x) * 4; int diff = 0;
        for (int c = 0; c < 4; c++) if (!(fabsf(g[c] - e[c]) <= tol)) diff = 1;
        if (diff) { if (!bad) { *fx = (int)x; *fy = (int)y; memcpy(got, g, 16); memcpy(want, e, 16); } bad++; }
    }
    return bad;
}

/* ------------------------------------------------------------------ kernel table */
enum { K_DUMP_ABS, K_DUMP_ABS_ARR, K_DUMP_REL,
       K_PAT_T_RGBA8, K_PAT_T_UNK, K_PAT_T_RGBA16F, K_PAT_T_RGBA32F, K_PAT_T_RGB10A2,
       K_PAT_P_RGBA16F, K_PAT_P_RGBA8,
       K_RMW_T_16F, K_RMW_T_8, K_RMW_P_16F, K_RMW_T_R1, K_RMW_T_R1_NOLS, K_RMW_T_R2,
       K_SIMD_P, K_SIMD_T, K_COUNT };
/* name kind tile ifmt arrayed rel ls_x ls_y rate_x rate_y pat_div logical */
static qp_kspec KS[K_COUNT] = {
    [K_DUMP_ABS]      = { "dump_abs",      QK_DUMP,    1, QP_IF_RGBA16F, 0, 0, 8, 8, 0, 0, 0, 0 },
    [K_DUMP_ABS_ARR]  = { "dump_abs_arr",  QK_DUMP,    1, QP_IF_RGBA16F, 1, 0, 8, 8, 0, 0, 0, 0 },
    [K_DUMP_REL]      = { "dump_rel",      QK_DUMP,    1, QP_IF_RGBA16F, 0, 1, 8, 8, 0, 0, 0, 0 },
    [K_PAT_T_RGBA8]   = { "pat_tile_rgba8",  QK_PATTERN, 1, QP_IF_RGBA8,   0, 0, 8, 8, 0, 0, 15, 0 },
    [K_PAT_T_UNK]     = { "pat_tile_unknown",QK_PATTERN, 1, QP_IF_UNKNOWN, 0, 0, 8, 8, 0, 0, 15, 0 },
    [K_PAT_T_RGBA16F] = { "pat_tile_rgba16f",QK_PATTERN, 1, QP_IF_RGBA16F, 0, 0, 8, 8, 0, 0, 16, 0 },
    [K_PAT_T_RGBA32F] = { "pat_tile_rgba32f",QK_PATTERN, 1, QP_IF_RGBA32F, 0, 0, 8, 8, 0, 0, 16, 0 },
    [K_PAT_T_RGB10A2] = { "pat_tile_rgb10a2",QK_PATTERN, 1, QP_IF_RGB10A2, 0, 0, 8, 8, 0, 0, 15, 0 },
    [K_PAT_P_RGBA16F] = { "pat_plain_rgba16f",QK_PATTERN,0, QP_IF_RGBA16F, 0, 0, 8, 8, 0, 0, 16, 0 },
    [K_PAT_P_RGBA8]   = { "pat_plain_rgba8", QK_PATTERN, 0, QP_IF_RGBA8,   0, 0, 8, 8, 0, 0, 15, 0 },
    [K_RMW_T_16F]     = { "rmw_tile_rgba16f",QK_RMW,     1, QP_IF_RGBA16F, 0, 0, 8, 8, 0, 0, 0, 0 },
    [K_RMW_T_8]       = { "rmw_tile_rgba8",  QK_RMW,     1, QP_IF_RGBA8,   0, 0, 8, 8, 0, 0, 0, 0 },
    [K_RMW_P_16F]     = { "rmw_plain_rgba16f",QK_RMW,    0, QP_IF_RGBA16F, 0, 0, 8, 8, 0, 0, 0, 0 },
    [K_RMW_T_R1]      = { "rmw_tile_rate1",  QK_RMW,     1, QP_IF_RGBA16F, 0, 0, 8, 8, 1, 1, 0, 0 },
    [K_RMW_T_R1_NOLS] = { "rmw_tile_rate1_nolocalsize", QK_RMW, 1, QP_IF_RGBA16F, 0, 0, 0, 0, 1, 1, 0, 0 },
    [K_RMW_T_R2]      = { "rmw_tile_rate2x2",QK_RMW,     1, QP_IF_RGBA16F, 0, 0, 8, 8, 2, 2, 0, 0 },
    [K_SIMD_P]        = { "simd_plain",      QK_SIMD,    0, QP_IF_RGBA16F, 0, 0, 64, 1, 0, 0, 0, 32 },
    [K_SIMD_T]        = { "simd_tile",       QK_SIMD,    1, QP_IF_RGBA16F, 0, 0, 64, 1, 0, 0, 0, 32 },
};

/* ------------------------------------------------------------------ Vulkan helpers */
static const char *vkr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS"; case VK_NOT_READY: return "VK_NOT_READY"; case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY"; case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED"; case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT"; case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED"; case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    default: return "VK_RESULT_OTHER";
    }
}
#define VKR(r) vkr(r), (int)(r)   /* usage: "%s(%d)" */

static int find_mem(QP *q, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < q->mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (q->mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static VkResult img_create(QP *q, QPImg *im, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u, const char **stage) {
    memset(im, 0, sizeof *im); im->w = w; im->h = h; im->fmt = f; im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult r = vkCreateImage(q->dev, &ci, NULL, &im->img);
    if (r) { *stage = "create_image"; im->img = VK_NULL_HANDLE; return r; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(q->dev, im->img, &mr);
    int mt = find_mem(q, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt < 0) mt = find_mem(q, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(q->dev, &ai, NULL, &im->mem); if (r) { *stage = "alloc_image"; return r; }
    r = vkBindImageMemory(q->dev, im->img, im->mem, 0); if (r) { *stage = "bind_image"; return r; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(q->dev, &vi, NULL, &im->view); if (r) { *stage = "create_view"; return r; }
    return VK_SUCCESS;
}
static void img_destroy(QP *q, QPImg *im) {
    if (q->lost) return;
    if (im->view) vkDestroyImageView(q->dev, im->view, NULL);
    if (im->img) vkDestroyImage(q->dev, im->img, NULL);
    if (im->mem) vkFreeMemory(q->dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}
static VkResult buf_create(QP *q, QPBuf *b, VkDeviceSize size) {
    memset(b, 0, sizeof *b); b->size = size;
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkResult r = vkCreateBuffer(q->dev, &ci, NULL, &b->buf); if (r) return r;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(q->dev, b->buf, &mr);
    int mt = find_mem(q, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); if (mt < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(q->dev, &ai, NULL, &b->mem); if (r) return r;
    r = vkBindBufferMemory(q->dev, b->buf, b->mem, 0); if (r) return r;
    return vkMapMemory(q->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map);
}
static void buf_destroy(QP *q, QPBuf *b) {
    if (q->lost) return;
    if (b->map) vkUnmapMemory(q->dev, b->mem);
    if (b->buf) vkDestroyBuffer(q->dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(q->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

typedef void (*RecFn)(QP *, VkCommandBuffer, void *);
/* record + submit + bounded wait. A timeout or DEVICE_LOST marks the device lost: later GPU work is skipped. */
static VkResult submit_wait(QP *q, RecFn rec, void *ud) {
    if (q->lost) return VK_ERROR_DEVICE_LOST;
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = q->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkResult r = vkAllocateCommandBuffers(q->dev, &ai, &cb); if (r) return r;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi); rec(q, cb, ud); r = vkEndCommandBuffer(cb); if (r) return r;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; r = vkCreateFence(q->dev, &fi, NULL, &fence); if (r) return r;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    r = vkQueueSubmit(q->queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(q->dev, 1, &fence, VK_TRUE, (uint64_t)q->o.wait_ms * 1000000ull);
    if (r == VK_TIMEOUT || r == VK_ERROR_DEVICE_LOST) { q->lost = 1; return r; }   /* leak cb/fence on purpose: the device is not trusted any more */
    vkDestroyFence(q->dev, fence, NULL); vkFreeCommandBuffers(q->dev, q->pool, 1, &cb);
    return r;
}

static void bar_img(QP *q, VkCommandBuffer cb, QPImg *im, VkImageLayout nl, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
        .oldLayout = im->layout, .newLayout = nl, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkDependencyInfo d = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b };
    q->cmdBarrier2(cb, &d); im->layout = nl;
}
static void bar_mem(QP *q, VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da, VkDependencyFlags fl) {
    VkMemoryBarrier2 m = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da };
    VkDependencyInfo d = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = fl, .memoryBarrierCount = 1, .pMemoryBarriers = &m };
    q->cmdBarrier2(cb, &d);
}
#define ST_ALL VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
#define AC_ALL (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)

static void rec_readback(QP *q, VkCommandBuffer cb, QPImg *im, QPBuf *bf) {
    bar_img(q, cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { im->w, im->h, 1 } };
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bf->buf, 1, &c);
    VkMemoryBarrier2 m = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT };
    VkDependencyInfo d = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m };
    q->cmdBarrier2(cb, &d);
}

static VkResult make_cs(QP *q, const qp_kspec *k0, VkPipeline *pipe, const char **stage) {
    qp_kspec kk = *k0; const qp_kspec *k = &kk;
    if (g_mock) { kk.tile = 0; kk.rate_x = kk.rate_y = 0; kk.rel = 0; kk.arrayed = 0; if (!kk.ls_x) { kk.ls_x = 8; kk.ls_y = 8; } }
    uint32_t n; uint32_t *w = qp_gen_kernel(k, 0, &n);
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n * 4, .pCode = w };
    VkShaderModule mod; VkResult r = vkCreateShaderModule(q->dev, &mi, NULL, &mod); free(w);
    if (r) { *stage = "create_shader_module"; return r; }
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = q->pl_cs,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" } };
    r = vkCreateComputePipelines(q->dev, VK_NULL_HANDLE, 1, &ci, NULL, pipe);
    vkDestroyShaderModule(q->dev, mod, NULL);
    if (r) { *stage = "create_compute_pipeline"; *pipe = VK_NULL_HANDLE; }
    return r;
}

static VkResult alloc_set(QP *q, QPImg *im, VkDescriptorSet *ds) {
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = q->dpool, .descriptorSetCount = 1, .pSetLayouts = &q->dsl };
    VkResult r = vkAllocateDescriptorSets(q->dev, &ai, ds); if (r) return r;
    VkDescriptorImageInfo ii = { .imageView = im->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = *ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii };
    vkUpdateDescriptorSets(q->dev, 1, &w, 0, NULL);
    return VK_SUCCESS;
}

static void begin_render(QP *q, VkCommandBuffer cb, QPImg *im, VkRect2D area, VkImageLayout lay, VkAttachmentLoadOp load, VkClearColorValue clear, int tile_flags) {
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = im->view, .imageLayout = lay, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = clear } };
    QPRPTileShading ts = { .sType = QP_ST_RP_TILE_SHADING_CREATE, .flags = (uint32_t)tile_flags };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .pNext = tile_flags >= 0 ? &ts : NULL, .renderArea = area, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    q->cmdBeginRendering(cb, &ri);
}

static void pc_area(uint32_t pc[8], VkRect2D a) {
    pc[0] = (uint32_t)a.offset.x; pc[1] = (uint32_t)a.offset.y; pc[2] = a.offset.x + a.extent.width; pc[3] = a.offset.y + a.extent.height;
    memcpy(pc + 4, pc, 16);
}

static int tile_ok(QP *q, const char **why) {   /* prerequisites of a per-tile dispatch experiment */
    if (g_mock) return 1;
    if (!q->ext_ts || !q->ext_tp) { *why = "extension_not_listed"; return 0; }
    if (!q->qcom_enabled) { *why = "device_create_refused_qcom_chain"; return 0; }
    if (!q->tsf.tileShading) { *why = "feature_tileShading_0"; return 0; }
    if (!q->tsf.tileShadingColorAttachments) { *why = "feature_tileShadingColorAttachments_0"; return 0; }
    if (!q->tsf.tileShadingPerTileDispatch) { *why = "feature_tileShadingPerTileDispatch_0"; return 0; }
    if (!q->cmdBeginPerTile || !q->cmdEndPerTile) { *why = "proc_addr_missing"; return 0; }
    return 1;
}

/* tile properties for a render area (VK_QCOM_tile_properties) */
static VkResult query_tile(QP *q, QPImg *im, VkRect2D area, int with_ts, QPTileProps *tp) {
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = im->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    QPRPTileShading ts = { .sType = QP_ST_RP_TILE_SHADING_CREATE, .flags = QP_TS_ENABLE_BIT };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .pNext = with_ts ? &ts : NULL, .renderArea = area, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    memset(tp, 0, sizeof *tp); tp->sType = QP_ST_TILE_PROPS;
    return q->getTileProps(q->dev, &ri, tp);
}

/* ------------------------------------------------------------------ experiment bookkeeping */
static const char *g_status[8];   /* index 1..6 */
static int exp_selected(QP *q, int n) { return (q->o.only_mask & (1 << n)) != 0; }
static void exp_begin(QP *q, int n) { snprintf(g_exp, sizeof g_exp, "E%d", n); arm(q->o.exp_timeout_s); }
static void exp_end(int n, const char *status, const Str *detail) {
    arm(0); g_status[n] = status;
    out("qcomprobe: E%d status=%s%s%s%s", n, status, detail && detail->n ? " " : "", detail ? detail->p : "", g_mock ? " mock=1" : "");
    snprintf(g_exp, sizeof g_exp, "between");
}
static void exp_unsupported(QP *q, int n, const char *why, const char *extra) {
    STR_DECL(d, 512); sadd(&d, "reason=%s", why); if (extra) sadd(&d, " %s", extra);
    (void)q; exp_end(n, "UNSUPPORTED", &d);
}
static void exp_lost(QP *q, int n) {   /* device lost before / during the experiment */
    STR_DECL(d, 128); sadd(&d, "reason=device_lost_earlier"); (void)q; exp_end(n, "FAIL", &d);
}

/* ------------------------------------------------------------------ E1: tile properties */
static void run_e1(QP *q) {
    exp_begin(q, 1);
    const char *why;
    if (g_mock) { exp_unsupported(q, 1, "mock_not_applicable", NULL); return; }
    if (!q->ext_tp) { exp_unsupported(q, 1, "VK_QCOM_tile_properties_not_listed", NULL); return; }
    if (!q->qcom_enabled) { STR_DECL(d, 160); sadd(&d, "qcom_create_result=%s(%d)", VKR(q->qcom_create_res)); exp_unsupported(q, 1, "device_create_refused_qcom_chain", d.p); return; }
    if (!q->tpf.tileProperties) { exp_unsupported(q, 1, "feature_tileProperties_0", NULL); return; }
    if (!q->getTileProps) { exp_unsupported(q, 1, "proc_addr_missing", NULL); return; }
    (void)why;
    static const VkFormat fm[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
    static const VkRect2D ar[] = { { { 0, 0 }, { 16, 16 } }, { { 0, 0 }, { 32, 32 } }, { { 0, 0 }, { 64, 64 } }, { { 0, 0 }, { 100, 60 } }, { { 0, 0 }, { 256, 256 } },
        { { 0, 0 }, { 1024, 768 } }, { { 0, 0 }, { 1920, 1080 } }, { { 16, 16 }, { 64, 64 } }, { { 7, 5 }, { 200, 100 } } };
    int queries = 0, ok = 0, fail = 0, m32 = 1, gran_mult = 1, apron_nz = 0, origin_nz = 0;
    uint32_t tmin_w = ~0u, tmin_h = ~0u, tmax_w = 0, tmax_h = 0; char first_err[96] = "";
    char dist[256] = ""; int ndist = 0;
    uint32_t tsz_w[8], tsz_h[8];
    for (size_t fi = 0; fi < 3; fi++) {
        QPImg im; const char *st; VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        VkResult r = img_create(q, &im, 1920, 1080, fm[fi], u, &st);
        if (r) { img_destroy(q, &im); r = img_create(q, &im, 1920, 1080, fm[fi], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &st); out("qcomprobe-detail: E1 fmt=%s storage_usage_refused_retry_color_only=1", fmt_info(fm[fi])->name); }
        if (r) { out("qcomprobe-detail: E1 fmt=%s create_image=%s(%d) stage=%s", fmt_info(fm[fi])->name, VKR(r), st); img_destroy(q, &im); fail++; continue; }
        for (size_t ai = 0; ai < sizeof ar / sizeof ar[0]; ai++) for (int ts = 0; ts < 2; ts++) {
            QPTileProps tp; r = query_tile(q, &im, ar[ai], ts, &tp); queries++;
            if (r) { fail++; if (!first_err[0]) snprintf(first_err, sizeof first_err, "%s(%d)", VKR(r)); out("qcomprobe-detail: E1 fmt=%s area=%d,%d,%ux%u ts=%d result=%s(%d)", fmt_info(fm[fi])->name, ar[ai].offset.x, ar[ai].offset.y, ar[ai].extent.width, ar[ai].extent.height, ts, VKR(r)); continue; }
            ok++;
            out("qcomprobe-detail: E1 fmt=%s area=%d,%d,%ux%u ts=%d tile=%ux%ux%u apron=%ux%u origin=%d,%d", fmt_info(fm[fi])->name, ar[ai].offset.x, ar[ai].offset.y, ar[ai].extent.width, ar[ai].extent.height, ts,
                tp.tileSize.width, tp.tileSize.height, tp.tileSize.depth, tp.apronSize.width, tp.apronSize.height, tp.origin.x, tp.origin.y);
            if (tp.tileSize.width < tmin_w) { tmin_w = tp.tileSize.width; }
            if (tp.tileSize.height < tmin_h) { tmin_h = tp.tileSize.height; }
            if (tp.tileSize.width > tmax_w) { tmax_w = tp.tileSize.width; }
            if (tp.tileSize.height > tmax_h) { tmax_h = tp.tileSize.height; }
            if (tp.tileSize.width < 32 || tp.tileSize.height < 32) m32 = 0;
            if (q->tsp.tileGranularity.width && (tp.tileSize.width % q->tsp.tileGranularity.width || tp.tileSize.height % q->tsp.tileGranularity.height)) gran_mult = 0;
            if (tp.apronSize.width || tp.apronSize.height) apron_nz = 1;
            if (tp.origin.x || tp.origin.y) origin_nz = 1;
            int seen = 0; for (int i = 0; i < ndist; i++) if (tsz_w[i] == tp.tileSize.width && tsz_h[i] == tp.tileSize.height) seen = 1;
            if (!seen && ndist < 8) { tsz_w[ndist] = tp.tileSize.width; tsz_h[ndist] = tp.tileSize.height; ndist++; }
        }
        img_destroy(q, &im);
    }
    for (int i = 0; i < ndist; i++) { char t[24]; snprintf(t, sizeof t, "%s%ux%u", i ? "," : "", tsz_w[i], tsz_h[i]); strncat(dist, t, sizeof dist - strlen(dist) - 1); }
    STR_DECL(d, 700);
    sadd(&d, "queries=%d ok=%d failed=%d tile_min=%ux%u tile_max=%ux%u distinct=%s metal32_fits_all=%d gran_multiple=%d apron_nonzero=%d origin_nonzero=%d",
         queries, ok, fail, ok ? tmin_w : 0, ok ? tmin_h : 0, tmax_w, tmax_h, dist[0] ? dist : "none", ok ? m32 : 0, gran_mult, apron_nz, origin_nz);
    if (first_err[0]) sadd(&d, " first_error=%s", first_err);
    exp_end(1, (ok == queries && queries > 0 && tmax_w > 0) ? "PASS" : "FAIL", &d);
}

/* ------------------------------------------------------------------ shared tile-pass recorder (E2, E3, E4, E6) */
typedef struct {
    QPImg *im; QPBuf *bf; VkPipeline pipe; VkDescriptorSet ds; VkRect2D area; uint32_t gx, gy; int rp_flags; VkImageLayout attl;
    VkAttachmentLoadOp load; VkClearColorValue clear; int bracket; int dispatch_tile; uint32_t pc[8];
} TileRec;

static void rec_tile_pass(QP *q, VkCommandBuffer cb, void *ud) {
    TileRec *t = ud;
    if (g_mock) {
        VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar_img(q, cb, t->im, VK_IMAGE_LAYOUT_GENERAL, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (t->load == VK_ATTACHMENT_LOAD_OP_CLEAR) { vkCmdClearColorImage(cb, t->im->img, VK_IMAGE_LAYOUT_GENERAL, &t->clear, 1, &sr); bar_mem(q, cb, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
        vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
        vkCmdDispatch(cb, t->gx, t->gy, 1);
        rec_readback(q, cb, t->im, t->bf);
        return;
    }
    bar_img(q, cb, t->im, t->attl, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    begin_render(q, cb, t->im, t->area, t->attl, t->load, t->clear, t->rp_flags);
    QPPerTileInfo bi = { .sType = QP_ST_PER_TILE_BEGIN }, ei = { .sType = QP_ST_PER_TILE_END }, di = { .sType = QP_ST_DISPATCH_TILE };
    if (t->bracket) q->cmdBeginPerTile(cb, &bi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
    vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
    if (t->dispatch_tile) q->cmdDispatchTile(cb, &di); else vkCmdDispatch(cb, t->gx, t->gy, 1);
    if (t->bracket) q->cmdEndPerTile(cb, &ei);
    q->cmdEndRendering(cb);
    t->im->layout = t->attl;
    rec_readback(q, cb, t->im, t->bf);
}

/* tile -> dispatch group counts for 8x8 local size; falls back to 128x128 when properties are not available */
static void tile_groups(QP *q, QPImg *im, VkRect2D area, uint32_t lsx, uint32_t lsy, uint32_t *gx, uint32_t *gy, uint32_t *tw, uint32_t *th) {
    uint32_t w = 128, h = 128;
    if (g_mock) { uint32_t aw = area.offset.x + area.extent.width, ah = area.offset.y + area.extent.height; *tw = aw; *th = ah; *gx = (aw + lsx - 1) / lsx; *gy = lsy ? (ah + lsy - 1) / lsy : ah; return; }
    if (q->ext_tp && q->tpf.tileProperties && q->getTileProps) { QPTileProps tp; if (query_tile(q, im, area, 1, &tp) == VK_SUCCESS && tp.tileSize.width && tp.tileSize.height) { w = tp.tileSize.width; h = tp.tileSize.height; } }
    *tw = w; *th = h; *gx = (w + lsx - 1) / lsx; *gy = lsy ? (h + lsy - 1) / lsy : h;
}

static VkClearColorValue clr(float r, float g, float b, float a) { VkClearColorValue c; c.float32[0] = r; c.float32[1] = g; c.float32[2] = b; c.float32[3] = a; return c; }

/* ------------------------------------------------------------------ E2: builtins + coordinate convention */
typedef struct { int pass; int unwritten, wrong, ntiles; uint32_t tw, th; int vkres_stage; char note[160]; } DumpRes;

static VkResult run_dump(QP *q, int kidx, VkImageLayout attl, int rp_flags, DumpRes *res) {
    memset(res, 0, sizeof *res);
    const uint32_t W = 192, H = 128;
    QPImg im; QPBuf bf; const char *st = ""; VkResult r;
    r = img_create(q, &im, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &st);
    if (r) { snprintf(res->note, sizeof res->note, "%s=%s(%d)", st, VKR(r)); img_destroy(q, &im); return r; }
    r = buf_create(q, &bf, (VkDeviceSize)W * H * 8); if (r) { snprintf(res->note, sizeof res->note, "buffer=%s(%d)", VKR(r)); buf_destroy(q, &bf); img_destroy(q, &im); return r; }
    VkPipeline pipe; r = make_cs(q, &KS[kidx], &pipe, &st);
    if (r) { snprintf(res->note, sizeof res->note, "%s=%s(%d)", st, VKR(r)); buf_destroy(q, &bf); img_destroy(q, &im); return r; }
    VkDescriptorSet ds; r = alloc_set(q, &im, &ds);
    if (r) { snprintf(res->note, sizeof res->note, "descriptor=%s(%d)", VKR(r)); return r; }
    TileRec t = { .im = &im, .bf = &bf, .pipe = pipe, .ds = ds, .area = { { 0, 0 }, { W, H } }, .rp_flags = rp_flags, .attl = attl, .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .clear = clr(-1, -1, -1, -1), .bracket = 1 };
    tile_groups(q, &im, t.area, 8, 8, &t.gx, &t.gy, &res->tw, &res->th);
    pc_area(t.pc, t.area);
    r = submit_wait(q, rec_tile_pass, &t);
    if (r) { snprintf(res->note, sizeof res->note, "submit_wait=%s(%d)", VKR(r)); res->vkres_stage = 1; if (!q->lost) { vkDestroyPipeline(q->dev, pipe, NULL); buf_destroy(q, &bf); img_destroy(q, &im); } return r; }
    const uint8_t *p = bf.map; uint32_t ox[256], oy[256]; int nt = 0;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float v[4]; decode_px(VK_FORMAT_R16G16B16A16_SFLOAT, p + ((size_t)y * W + x) * 8, v);
        if (v[0] == -1.0f && v[1] == -1.0f && v[2] == -1.0f && v[3] == -1.0f) { res->unwritten++; continue; }
        if (!(v[0] <= (float)x && (float)x < v[0] + v[2] && v[1] <= (float)y && (float)y < v[1] + v[3])) { res->wrong++; continue; }
        int seen = 0; for (int i = 0; i < nt; i++) if (ox[i] == (uint32_t)v[0] && oy[i] == (uint32_t)v[1]) { seen = 1; break; }
        if (!seen && nt < 256) { ox[nt] = (uint32_t)v[0]; oy[nt] = (uint32_t)v[1]; nt++; if ((uint32_t)v[2] > res->tw || 1) { res->tw = (uint32_t)v[2]; res->th = (uint32_t)v[3]; } }
    }
    res->ntiles = nt; res->pass = (res->unwritten == 0 && res->wrong == 0 && nt > 0);
    vkDestroyPipeline(q->dev, pipe, NULL); buf_destroy(q, &bf); img_destroy(q, &im);
    return VK_SUCCESS;
}

static void run_e2(QP *q) {
    exp_begin(q, 2);
    const char *why;
    if (g_mock) { exp_unsupported(q, 2, "mock_not_applicable", NULL); return; }
    if (!tile_ok(q, &why)) { STR_DECL(d, 160); if (q->qcom_fallback) sadd(&d, "qcom_create_result=%s(%d)", VKR(q->qcom_create_res)); exp_unsupported(q, 2, why, d.n ? d.p : NULL); return; }
    if (q->lost) { exp_lost(q, 2); return; }
    struct { const char *name; int k; VkImageLayout attl; int flags; int risky; } var[] = {
        { "abs_general", K_DUMP_ABS, VK_IMAGE_LAYOUT_GENERAL, QP_TS_ENABLE_BIT, 0 },
        { "abs_attopt", K_DUMP_ABS, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, QP_TS_ENABLE_BIT, 0 },
        { "abs_arrayed_general", K_DUMP_ABS_ARR, VK_IMAGE_LAYOUT_GENERAL, QP_TS_ENABLE_BIT, 0 },
        { "abs_general_pertile_flag", K_DUMP_ABS, VK_IMAGE_LAYOUT_GENERAL, QP_TS_ENABLE_BIT | QP_TS_PER_TILE_BIT, 1 },
        { "rel_general", K_DUMP_REL, VK_IMAGE_LAYOUT_GENERAL, QP_TS_ENABLE_BIT, 1 } };
    int any = 0, abs_ok = 0, rel_ok = 0; STR_DECL(d, 1200); uint32_t tw = 0, th = 0; int nt = 0;
    int user_flags = q->o.rp_flags;
    for (size_t i = 0; i < sizeof var / sizeof var[0]; i++) {
        if (q->lost) { sadd(&d, "%s=skipped_device_lost ", var[i].name); continue; }
        if (var[i].risky && (q->o.no_risky || (i == 4 && abs_ok))) { sadd(&d, "%s=not_run ", var[i].name); continue; }
        DumpRes dr; VkResult r = run_dump(q, var[i].k, var[i].attl, user_flags >= 0 && i != 3 ? user_flags : var[i].flags, &dr);
        if (r) { out("qcomprobe-detail: E2 variant=%s refused=%s", var[i].name, dr.note); sadd(&d, "%s=REFUSED(%s) ", var[i].name, dr.note); continue; }
        out("qcomprobe-detail: E2 variant=%s unwritten=%d wrong=%d tiles=%d tile_seen=%ux%u", var[i].name, dr.unwritten, dr.wrong, dr.ntiles, dr.tw, dr.th);
        sadd(&d, "%s=%s ", var[i].name, dr.pass ? "PASS" : "MISMATCH");
        if (dr.pass) { any = 1; if (i == 4) rel_ok = 1; else abs_ok = 1; tw = dr.tw; th = dr.th; nt = dr.ntiles; }
    }
    /* trim trailing space, then facts */
    if (d.n && d.p[d.n - 1] == ' ') d.p[--d.n] = 0;
    sadd(&d, " coord_convention=%s tile_builtin=%ux%u tiles_seen=%d", abs_ok ? "framebuffer_absolute" : rel_ok ? "tile_relative" : "unknown", tw, th, nt);
    exp_end(2, q->lost ? "FAIL" : any ? "PASS" : "FAIL", &d);
}

/* ------------------------------------------------------------------ process helpers (crash isolation) */
static const char *g_self = "/proc/self/exe";
/* run this binary again with extra arguments, capture its stdout; *wstatus = waitpid status. Returns the number of bytes captured. */
static size_t spawn_self(const Opts *o, const char *const *extra, int nextra, char *buf, size_t cap, int *wstatus) {
    int pfd[2]; buf[0] = 0; *wstatus = -1;
    if (pipe(pfd)) return 0;
    char dev[16], rpf[16], wms[16], tmo[16]; const char *av[40]; int n = 0;
    av[n++] = g_self;
    if (o->device >= 0) { snprintf(dev, sizeof dev, "%d", o->device); av[n++] = "--device"; av[n++] = dev; }
    if (o->rp_flags >= 0) { snprintf(rpf, sizeof rpf, "%d", o->rp_flags); av[n++] = "--rp-flags"; av[n++] = rpf; }
    snprintf(wms, sizeof wms, "%d", o->wait_ms); av[n++] = "--wait-ms"; av[n++] = wms;
    snprintf(tmo, sizeof tmo, "%d", o->exp_timeout_s); av[n++] = "--timeout"; av[n++] = tmo;
    if (o->no_risky) av[n++] = "--no-risky";
    if (g_mock) av[n++] = "--mock-tile";
    for (int i = 0; i < nextra && n < 38; i++) av[n++] = extra[i];
    av[n] = NULL;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return 0; }
    if (pid == 0) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); execv(g_self, (char *const *)av); if (strcmp(g_self, "/proc/self/exe")) _exit(127); _exit(127); }
    close(pfd[1]);
    size_t len = 0; ssize_t r;
    while ((r = read(pfd[0], buf + len, cap - 1 - len)) > 0) { len += (size_t)r; if (len >= cap - 1) break; }
    buf[len] = 0; close(pfd[0]);
    waitpid(pid, wstatus, 0);
    return len;
}

/* ------------------------------------------------------------------ E3: colour attachment formats */
typedef struct { VkFormat f; int k; float tol; } E3Fmt;
static const E3Fmt E3F[] = {
    { VK_FORMAT_R8G8B8A8_UNORM, K_PAT_T_RGBA8, 0.004f }, { VK_FORMAT_B8G8R8A8_UNORM, K_PAT_T_UNK, 0.004f }, { VK_FORMAT_R16G16B16A16_SFLOAT, K_PAT_T_RGBA16F, 0.0f },
    { VK_FORMAT_R32G32B32A32_SFLOAT, K_PAT_T_RGBA32F, 1e-6f }, { VK_FORMAT_A2B10G10R10_UNORM_PACK32, K_PAT_T_RGB10A2, 0.002f } };
#define E3_NF ((int)(sizeof E3F / sizeof E3F[0]))

/* one matrix row; returns the result word, details in row */
static const char *e3_row(QP *q, int i, int stor, char *row, size_t cap) {
    const uint32_t W = 100, H = 70; row[0] = 0;
    if (q->lost) return "SKIPPED_DEVICE_LOST";
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(q->pd, E3F[i].f, &fp);
    snprintf(row, cap, "storage_feature=%d color_att_feature=%d ", !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT), !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    if (E3F[i].k == K_PAT_T_UNK && !q->f_wwf) return "UNSUPPORTED_PREREQ(shaderStorageImageWriteWithoutFormat=0)";
    size_t o = strlen(row);
    QPImg im; QPBuf bf; const char *st = ""; VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (stor ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    VkResult r = img_create(q, &im, W, H, E3F[i].f, u, &st);
    if (r) { snprintf(row + o, cap - o, "%s=%s(%d)", st, VKR(r)); img_destroy(q, &im); return "REFUSED"; }
    buf_create(q, &bf, (VkDeviceSize)W * H * 16);
    VkPipeline pipe; r = make_cs(q, &KS[E3F[i].k], &pipe, &st);
    if (r) { snprintf(row + o, cap - o, "%s=%s(%d)", st, VKR(r)); buf_destroy(q, &bf); img_destroy(q, &im); return "REFUSED"; }
    VkDescriptorSet ds; r = alloc_set(q, &im, &ds);
    TileRec t = { .im = &im, .bf = &bf, .pipe = pipe, .ds = ds, .area = { { 0, 0 }, { W, H } }, .rp_flags = QP_TS_ENABLE_BIT, .attl = VK_IMAGE_LAYOUT_GENERAL, .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .clear = clr(0, 0, 0, 0), .bracket = 1 };
    if (q->o.rp_flags >= 0) t.rp_flags = q->o.rp_flags;
    uint32_t tw, th; tile_groups(q, &im, t.area, 8, 8, &t.gx, &t.gy, &tw, &th); pc_area(t.pc, t.area);
    r = submit_wait(q, rec_tile_pass, &t);
    if (r) { snprintf(row + o, cap - o, "submit_wait=%s(%d)", VKR(r)); if (!q->lost) { vkDestroyPipeline(q->dev, pipe, NULL); buf_destroy(q, &bf); img_destroy(q, &im); } return "FAILED_RUN"; }
    float *ex = malloc((size_t)W * H * 16);
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) pat_px(x, y, KS[E3F[i].k].pat_div, ex + ((size_t)y * W + x) * 4);
    int fx = 0, fy = 0; float g[4], w[4]; int bad = cmp_model(E3F[i].f, bf.map, W, H, ex, E3F[i].tol, &fx, &fy, g, w); free(ex);
    vkDestroyPipeline(q->dev, pipe, NULL); buf_destroy(q, &bf); img_destroy(q, &im);
    if (bad) { snprintf(row + o, cap - o, "mismatch=%d/%u first=(%d,%d) got=%.4f,%.4f,%.4f,%.4f want=%.4f,%.4f,%.4f,%.4f", bad, W * H, fx, fy, g[0], g[1], g[2], g[3], w[0], w[1], w[2], w[3]); return "ACCEPTED_BAD_PIXELS"; }
    return "ACCEPTED_PASS";
}
static const char *e3_class(const char *res) {
    return strncmp(res, "ACCEPTED_PASS", 13) == 0 ? "PASS" : strncmp(res, "UNSUPPORTED", 11) == 0 ? "UNSUPPORTED" : strncmp(res, "REFUSED", 7) == 0 ? "REFUSED" : strncmp(res, "CRASH", 5) == 0 ? "CRASH" : "FAIL";
}

static void run_e3(QP *q) {
    exp_begin(q, 3);
    const char *why;
    if (!tile_ok(q, &why)) { exp_unsupported(q, 3, why, NULL); return; }
    if (q->lost) { exp_lost(q, 3); return; }
    STR_DECL(d, 1800); int base_ok = 0, rows = 0, accepted = 0;
    /* storage-usage rows first; the rows whose image lacks STORAGE usage use a storage descriptor on that view (outside the core validity rules, the
       driver may crash) so each runs in its own child process and a crash is reported as a row result */
    for (int stor = 1; stor >= 0; stor--) for (int i = 0; i < E3_NF; i++) {
        const char *fn = fmt_info(E3F[i].f)->name; char row[300]; const char *res; char cres[96];
        if (!stor && q->o.no_risky) continue;
        if (stor) res = e3_row(q, i, 1, row, sizeof row);
        else {
            char idx[8], cap[2048]; snprintf(idx, sizeof idx, "%d", i); const char *ex[] = { "--child", "--e3-row", idx }; int ws;
            if (q->lost) { res = "SKIPPED_DEVICE_LOST"; row[0] = 0; }
            else {
                arm(q->o.exp_timeout_s + 5);
                spawn_self(&q->o, ex, 3, cap, sizeof cap, &ws);
                const char *line = strstr(cap, "qcomprobe-detail: E3 ");
                if (WIFSIGNALED(ws)) { snprintf(cres, sizeof cres, "CRASH(signal=%d)", WTERMSIG(ws)); res = cres; row[0] = 0; }
                else if (WIFEXITED(ws) && WEXITSTATUS(ws) == 3) { res = "FAILED_RUN(child_watchdog_timeout)"; row[0] = 0; }
                else if (line && WIFEXITED(ws) && WEXITSTATUS(ws) == 0) {
                    const char *rp = strstr(line, "result="); char *e = strchr(rp ? rp : line, ' ');
                    snprintf(cres, sizeof cres, "%.*s", (int)(e && rp ? e - (rp + 7) : 0), rp ? rp + 7 : "");
                    res = cres; const char *rest = e ? e + 1 : ""; snprintf(row, sizeof row, "%s", rest); char *nl = strchr(row, '\n'); if (nl) *nl = 0;
                } else { snprintf(cres, sizeof cres, "FAILED_RUN(child_exit=%d)", WIFEXITED(ws) ? WEXITSTATUS(ws) : -1); res = cres; row[0] = 0; }
                arm(q->o.exp_timeout_s);
            }
        }
        out("qcomprobe-detail: E3 fmt=%s storage_usage=%d result=%s %s", fn, stor, res, row);
        sadd(&d, "%s/%s=%s ", fn, stor ? "S" : "noS", e3_class(res));
        rows++; if (!strcmp(res, "ACCEPTED_PASS")) accepted++;
        if (E3F[i].f == VK_FORMAT_R16G16B16A16_SFLOAT && stor && !strcmp(res, "ACCEPTED_PASS")) base_ok = 1;
    }
    if (d.n && d.p[d.n - 1] == ' ') d.p[--d.n] = 0;
    sadd(&d, " rows=%d accepted_pass=%d baseline_rgba16f_storage=%d", rows, accepted, base_ok);
    exp_end(3, (!q->lost && base_ok) ? "PASS" : "FAIL", &d);
}

/* ------------------------------------------------------------------ E4: per-tile read/modify/write */
typedef struct { QPImg *im; VkPipeline pipe; VkDescriptorSet ds; VkRect2D area; uint32_t pc[8]; } InitRec;
static void rec_init_pattern(QP *q, VkCommandBuffer cb, void *ud) {
    InitRec *t = ud;
    bar_img(q, cb, t->im, VK_IMAGE_LAYOUT_GENERAL, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
    vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
    vkCmdDispatch(cb, (t->im->w + 7) / 8, (t->im->h + 7) / 8, 1);
    bar_mem(q, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ST_ALL, AC_ALL, 0);
}

static void run_e4(QP *q) {
    exp_begin(q, 4);
    const char *why;
    if (!tile_ok(q, &why)) { exp_unsupported(q, 4, why, NULL); return; }
    if (q->lost) { exp_lost(q, 4); return; }
    struct { VkFormat f; int kp, kr; float tol; } fm[] = { { VK_FORMAT_R16G16B16A16_SFLOAT, K_PAT_P_RGBA16F, K_RMW_T_16F, 0.0f }, { VK_FORMAT_R8G8B8A8_UNORM, K_PAT_P_RGBA8, K_RMW_T_8, 0.005f } };
    const uint32_t W = 208, H = 144; STR_DECL(d, 900); int base_ok = 0;
    for (size_t i = 0; i < 2; i++) for (int sub = 0; sub < 2; sub++) {
        const char *fn = fmt_info(fm[i].f)->name; char row[300]; row[0] = 0; const char *res = "?";
        if (q->lost) { res = "SKIPPED_DEVICE_LOST"; goto emit; }
        {
            VkRect2D area = sub ? (VkRect2D){ { 8, 8 }, { W - 24, H - 20 } } : (VkRect2D){ { 0, 0 }, { W, H } };
            QPImg im; QPBuf bf; const char *st = ""; VkResult r = img_create(q, &im, W, H, fm[i].f, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &st);
            if (r) { snprintf(row, sizeof row, "%s=%s(%d)", st, VKR(r)); res = "REFUSED"; img_destroy(q, &im); goto emit; }
            buf_create(q, &bf, (VkDeviceSize)W * H * 8);
            VkPipeline pp, pr; r = make_cs(q, &KS[fm[i].kp], &pp, &st);
            if (!r) r = make_cs(q, &KS[fm[i].kr], &pr, &st);
            if (r) { snprintf(row, sizeof row, "%s=%s(%d)", st, VKR(r)); res = "REFUSED"; buf_destroy(q, &bf); img_destroy(q, &im); goto emit; }
            VkDescriptorSet ds; alloc_set(q, &im, &ds);
            InitRec ir = { .im = &im, .pipe = pp, .ds = ds, .area = { { 0, 0 }, { W, H } } }; pc_area(ir.pc, ir.area);
            r = submit_wait(q, rec_init_pattern, &ir);
            if (r) { snprintf(row, sizeof row, "init_submit=%s(%d)", VKR(r)); res = "FAILED_RUN"; goto emit; }
            TileRec t = { .im = &im, .bf = &bf, .pipe = pr, .ds = ds, .area = area, .rp_flags = q->o.rp_flags >= 0 ? q->o.rp_flags : (int)QP_TS_ENABLE_BIT, .attl = VK_IMAGE_LAYOUT_GENERAL, .load = VK_ATTACHMENT_LOAD_OP_LOAD, .bracket = 1 };
            uint32_t tw, th; tile_groups(q, &im, area, 8, 8, &t.gx, &t.gy, &tw, &th); pc_area(t.pc, area);
            r = submit_wait(q, rec_tile_pass, &t);
            if (r) { snprintf(row, sizeof row, "tile_submit=%s(%d)", VKR(r)); res = "FAILED_RUN"; goto emit; }
            float *ex = malloc((size_t)W * H * 16); int pdiv = KS[fm[i].kp].pat_div;
            for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
                float p[4], *e = ex + ((size_t)y * W + x) * 4; pat_px(x, y, pdiv, p);
                if (fm[i].f == VK_FORMAT_R8G8B8A8_UNORM) for (int c = 0; c < 4; c++) p[c] = quant8(p[c]);
                int in = (int)x >= area.offset.x && (int)x < area.offset.x + (int)area.extent.width && (int)y >= area.offset.y && (int)y < area.offset.y + (int)area.extent.height;
                if (in) rmw_px(p, e); else memcpy(e, p, 16);
            }
            int fx = 0, fy = 0; float g[4], w[4]; int bad = cmp_model(fm[i].f, bf.map, W, H, ex, fm[i].tol, &fx, &fy, g, w); free(ex);
            if (bad) { snprintf(row, sizeof row, "mismatch=%d/%u first=(%d,%d) got=%.4f,%.4f,%.4f,%.4f want=%.4f,%.4f,%.4f,%.4f", bad, W * H, fx, fy, g[0], g[1], g[2], g[3], w[0], w[1], w[2], w[3]); res = "MISMATCH"; }
            else res = "PASS";
            vkDestroyPipeline(q->dev, pp, NULL); vkDestroyPipeline(q->dev, pr, NULL); buf_destroy(q, &bf); img_destroy(q, &im);
        }
    emit:
        out("qcomprobe-detail: E4 fmt=%s area=%s result=%s %s", fn, sub ? "sub" : "full", res, row);
        sadd(&d, "%s/%s=%s ", fn, sub ? "sub" : "full", res);
        if (fm[i].f == VK_FORMAT_R16G16B16A16_SFLOAT && !sub && !strcmp(res, "PASS")) base_ok = 1;
    }
    if (d.n && d.p[d.n - 1] == ' ') d.p[--d.n] = 0;
    exp_end(4, (!q->lost && base_ok) ? "PASS" : "FAIL", &d);
}

/* ------------------------------------------------------------------ E5: draw -> per-tile dispatch -> draw in one pass vs split passes */
typedef struct {
    QPImg *im; QPBuf *bf; VkPipeline gfx, cs; VkDescriptorSet ds; int variant, bracket, dispatch_tile, use_barriers; uint32_t gx, gy, rp_flags; uint32_t pc[8]; VkRect2D s1, s2;
} E5Rec;
static void e5_draw(QP *q, VkCommandBuffer cb, E5Rec *t, VkRect2D sc, float blue) {
    VkViewport vp = { 0, 0, (float)t->im->w, (float)t->im->h, 0, 1 };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, t->gfx);
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, q->pl_gfx, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &blue);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
static void rec_e5(QP *q, VkCommandBuffer cb, void *ud) {
    E5Rec *t = ud; VkRect2D full = { { 0, 0 }, { t->im->w, t->im->h } };
    QPPerTileInfo bi = { .sType = QP_ST_PER_TILE_BEGIN }, ei = { .sType = QP_ST_PER_TILE_END }, di = { .sType = QP_ST_DISPATCH_TILE };
    bar_img(q, cb, t->im, VK_IMAGE_LAYOUT_GENERAL, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (t->variant == 0) {   /* split: render, end, plain compute over the image, render again with LOAD */
        begin_render(q, cb, t->im, full, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR, clr(0.25f, 0.5f, 0.75f, 1.0f), -1);
        e5_draw(q, cb, t, t->s1, 0.25f);
        q->cmdEndRendering(cb);
        bar_mem(q, cb, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->cs);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
        vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
        vkCmdDispatch(cb, (t->im->w + 7) / 8, (t->im->h + 7) / 8, 1);
        bar_mem(q, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0);
        begin_render(q, cb, t->im, full, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_LOAD, clr(0, 0, 0, 0), -1);
        e5_draw(q, cb, t, t->s2, 0.75f);
        q->cmdEndRendering(cb);
    } else {                 /* one render pass instance with tile shading */
        begin_render(q, cb, t->im, full, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR, clr(0.25f, 0.5f, 0.75f, 1.0f), (int)t->rp_flags);
        e5_draw(q, cb, t, t->s1, 0.25f);
        if (t->use_barriers) bar_mem(q, cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     QP_ACCESS2_TILE_READ | QP_ACCESS2_TILE_WRITE, VK_DEPENDENCY_BY_REGION_BIT);
        if (t->bracket) q->cmdBeginPerTile(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->cs);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
        vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
        if (t->dispatch_tile) q->cmdDispatchTile(cb, &di); else vkCmdDispatch(cb, t->gx, t->gy, 1);
        if (t->bracket) q->cmdEndPerTile(cb, &ei);
        if (t->use_barriers) bar_mem(q, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, QP_ACCESS2_TILE_WRITE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT);
        e5_draw(q, cb, t, t->s2, 0.75f);
        q->cmdEndRendering(cb);
    }
    rec_readback(q, cb, t->im, t->bf);
}

static VkResult make_gfx(QP *q, VkFormat f, VkPipeline *pipe, const char **stage) {
    uint32_t nv, nf; uint32_t *wv = qp_gen_vert(&nv), *wf = qp_gen_frag(&nf);
    VkShaderModuleCreateInfo mv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)nv * 4, .pCode = wv };
    VkShaderModuleCreateInfo mf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)nf * 4, .pCode = wf };
    VkShaderModule v, fr; VkResult r = vkCreateShaderModule(q->dev, &mv, NULL, &v); if (!r) r = vkCreateShaderModule(q->dev, &mf, NULL, &fr);
    free(wv); free(wf); if (r) { *stage = "create_shader_module"; return r; }
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = v, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fr, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = q->pl_gfx };
    r = vkCreateGraphicsPipelines(q->dev, VK_NULL_HANDLE, 1, &gi, NULL, pipe);
    vkDestroyShaderModule(q->dev, v, NULL); vkDestroyShaderModule(q->dev, fr, NULL);
    if (r) *stage = "create_graphics_pipeline";
    return r;
}

static void e5_model(uint32_t W, uint32_t H, VkRect2D s1, VkRect2D s2, float *ex) {
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float *e = ex + ((size_t)y * W + x) * 4; e[0] = 0.25f; e[1] = 0.5f; e[2] = 0.75f; e[3] = 1.0f;
        if ((int)x >= s1.offset.x && (int)x < s1.offset.x + (int)s1.extent.width && (int)y >= s1.offset.y && (int)y < s1.offset.y + (int)s1.extent.height) {
            e[0] = (float)(x & 15) * 0.0625f; e[1] = (float)(y & 15) * 0.0625f; e[2] = 0.25f; e[3] = 1.0f; }
        float in[4]; memcpy(in, e, 16); rmw_px(in, e);
        if ((int)x >= s2.offset.x && (int)x < s2.offset.x + (int)s2.extent.width && (int)y >= s2.offset.y && (int)y < s2.offset.y + (int)s2.extent.height) {
            e[0] = (float)(x & 15) * 0.0625f; e[1] = (float)(y & 15) * 0.0625f; e[2] = 0.75f; e[3] = 1.0f; }
    }
}

static void run_e5(QP *q) {
    exp_begin(q, 5);
    const char *why = "";
    int tile = !g_mock && tile_ok(q, &why); if (g_mock) why = "mock";   /* without tile shading only the split-pass reference runs (it checks the draw model + plain kernel on any device) */
    if (q->lost) { exp_lost(q, 5); return; }
    const uint32_t W = 192, H = 128; VkRect2D s1 = { { 16, 8 }, { 120, 80 } }, s2 = { { 64, 40 }, { 100, 70 } }; VkFormat F = VK_FORMAT_R16G16B16A16_SFLOAT;
    struct { const char *name; int variant, kidx, bracket, dtile; } vs[] = {
        { "split_reference", 0, K_RMW_P_16F, 0, 0 }, { "tile_bracket_dispatch", 1, K_RMW_T_16F, 1, 0 }, { "tile_bracket_dispatchtile_rate1", 2, K_RMW_T_R1, 1, 1 },
        { "tile_nobracket_dispatchtile_rate1", 3, K_RMW_T_R1, 0, 1 }, { "tile_bracket_dispatchtile_rate1_nolocalsize", 4, K_RMW_T_R1_NOLS, 1, 1 }, { "tile_bracket_dispatchtile_rate2x2", 5, K_RMW_T_R2, 1, 1 } };
    QPImg im; QPBuf bf; const char *st = ""; VkResult r = img_create(q, &im, W, H, F, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &st);
    STR_DECL(d, 1500);
    if (r) { sadd(&d, "reason=%s=%s(%d)", st, VKR(r)); exp_end(5, "FAIL", &d); img_destroy(q, &im); return; }
    buf_create(q, &bf, (VkDeviceSize)W * H * 8);
    VkPipeline gfx; r = make_gfx(q, F, &gfx, &st);
    if (r) { sadd(&d, "reason=%s=%s(%d)", st, VKR(r)); exp_end(5, "FAIL", &d); return; }
    VkDescriptorSet ds; alloc_set(q, &im, &ds);
    float *ex = malloc((size_t)W * H * 16); e5_model(W, H, s1, s2, ex);
    uint32_t tw, th, gx, gy; tile_groups(q, &im, (VkRect2D){ { 0, 0 }, { W, H } }, 8, 8, &gx, &gy, &tw, &th);
    int ran = 0, passed = 0, failed = 0, ref_ok = 0; int refused_dt = 0;
    uint8_t *ref = NULL;
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) {
        char row[300]; row[0] = 0; const char *res;
        if (q->lost) { res = "SKIPPED_DEVICE_LOST"; goto emit; }
        if (vs[i].variant && !tile) { res = "UNSUPPORTED(tile_shading_not_available)"; goto emit; }
        if (vs[i].variant == 3 && q->o.no_risky) { res = "NOT_RUN(no_risky)"; goto emit; }
        if (vs[i].dtile && (!q->tsf.tileShadingDispatchTile || !q->cmdDispatchTile)) { res = "UNSUPPORTED(feature_tileShadingDispatchTile_0)"; refused_dt++; goto emit; }
        if (vs[i].variant == 0) { if (0) {} }
        {
            VkPipeline cs; r = make_cs(q, &KS[vs[i].kidx], &cs, &st);
            if (r) { snprintf(row, sizeof row, "%s=%s(%d)", st, VKR(r)); res = "REFUSED"; failed += (vs[i].variant == 0); goto emit; }
            E5Rec t = { .im = &im, .bf = &bf, .gfx = gfx, .cs = cs, .ds = ds, .variant = vs[i].variant, .bracket = vs[i].bracket, .dispatch_tile = vs[i].dtile, .use_barriers = q->f_lr,
                .gx = gx, .gy = gy, .rp_flags = q->o.rp_flags >= 0 ? (uint32_t)q->o.rp_flags : QP_TS_ENABLE_BIT, .s1 = s1, .s2 = s2 };
            pc_area(t.pc, (VkRect2D){ { 0, 0 }, { W, H } });
            im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            r = submit_wait(q, rec_e5, &t);
            if (r) { snprintf(row, sizeof row, "submit_wait=%s(%d)", VKR(r)); res = "FAILED_RUN"; failed++; goto emit; }
            int fx = 0, fy = 0; float g[4], w[4]; int bad = cmp_model(F, bf.map, W, H, ex, 0.0f, &fx, &fy, g, w);
            ran++;
            if (vs[i].variant == 0) { ref = malloc((size_t)W * H * 8); memcpy(ref, bf.map, (size_t)W * H * 8); ref_ok = !bad; }
            if (bad) { snprintf(row, sizeof row, "mismatch_vs_model=%d/%u first=(%d,%d) got=%.4f,%.4f,%.4f,%.4f want=%.4f,%.4f,%.4f,%.4f", bad, W * H, fx, fy, g[0], g[1], g[2], g[3], w[0], w[1], w[2], w[3]); res = "MISMATCH"; failed++; }
            else { res = "PASS"; passed++; }
            if (vs[i].variant && ref) { size_t diff = 0; for (size_t b = 0; b < (size_t)W * H * 8; b++) if (((uint8_t *)bf.map)[b] != ref[b]) diff++; snprintf(row + strlen(row), sizeof row - strlen(row), " bytes_differing_from_split=%zu", diff); }
            vkDestroyPipeline(q->dev, cs, NULL);
        }
    emit:
        out("qcomprobe-detail: E5 variant=%s barriers=%s result=%s %s", vs[i].name, !vs[i].variant ? "plain_pipeline_barriers" : q->f_lr ? "tile_access_by_region" : "none(no_dynamicRenderingLocalRead)", res, row);
        sadd(&d, "%s=%s ", vs[i].name, strncmp(res, "UNSUPPORTED", 11) == 0 ? "UNSUPPORTED" : res);
    }
    free(ex); free(ref);
    if (d.n && d.p[d.n - 1] == ' ') d.p[--d.n] = 0;
    sadd(&d, " split_matches_model=%d variants_run=%d passed=%d failed=%d tile=%ux%u", ref_ok, ran, passed, failed, tw, th);
    const char *s = q->lost ? "FAIL" : (ran == 0 ? "UNSUPPORTED" : (failed == 0 ? "PASS" : "FAIL"));
    (void)refused_dt;
    if (!tile) { sadd(&d, " reason=%s", why); s = (q->lost || failed) ? "FAIL" : "UNSUPPORTED"; }
    else if (!q->lost && ran > 0 && passed == 0) s = "FAIL";
    else if (!q->lost && ran > 0 && passed == 1 && ref_ok) s = "FAIL";   /* only the split reference ran: no tile variant was exercised */
    exp_end(5, s, &d);
    if (!q->lost) { vkDestroyPipeline(q->dev, gfx, NULL); buf_destroy(q, &bf); img_destroy(q, &im); }
}

/* ------------------------------------------------------------------ E6: logical SIMD32 on the device subgroup + region limit */
typedef struct { QPImg *im; QPBuf *bf; VkPipeline pipe; VkDescriptorSet ds; uint32_t pc[8]; uint32_t gx, gy; int tile; TileRec tr; } E6Rec;
static void rec_e6_plain(QP *q, VkCommandBuffer cb, void *ud) {
    E6Rec *t = ud; VkClearColorValue c = clr(0.5f, 0.5f, 0.5f, 0.5f); VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bar_img(q, cb, t->im, VK_IMAGE_LAYOUT_GENERAL, ST_ALL, AC_ALL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, t->im->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &sr);
    bar_mem(q, cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->pl_cs, 0, 1, &t->ds, 0, NULL);
    vkCmdPushConstants(cb, q->pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, t->pc);
    vkCmdDispatch(cb, t->gx, t->gy, 1);
    rec_readback(q, cb, t->im, t->bf);
}

/* returns number of mismatching pixels; fills a note with the first one */
static int e6_check(QP *q, const uint8_t *p, uint32_t W, uint32_t H, VkRect2D region, uint32_t L, char *note, size_t nn, int *lane_layout_bad, uint32_t *sg_lo, uint32_t *sg_hi) {
    int bad = 0; *lane_layout_bad = 0; note[0] = 0; *sg_lo = ~0u; *sg_hi = 0; (void)q;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float g[4]; decode_px(VK_FORMAT_R16G16B16A16_SFLOAT, p + ((size_t)y * W + x) * 8, g);
        int in = (int)x >= region.offset.x && (int)x < region.offset.x + (int)region.extent.width && (int)y >= region.offset.y && (int)y < region.offset.y + (int)region.extent.height;
        int ok;
        if (!in) ok = (g[0] == 0.5f && g[1] == 0.5f && g[2] == 0.5f && g[3] == 0.5f);
        else {
            uint32_t lane = (uint32_t)g[2], lm = lane & (L - 1);
            uint32_t sgs = (uint32_t)g[3]; if (sgs < *sg_lo) *sg_lo = sgs; if (sgs > *sg_hi) *sg_hi = sgs;
            if (sgs < L || lane >= sgs || lm > x) { ok = 0; (*lane_layout_bad)++; }
            else {
                float sum = 0; for (uint32_t j = 0; j < L; j++) sum += simd_f((int64_t)x - lm + j, y);
                float up = lm == 0 ? simd_f(x, y) : simd_f((int64_t)x - 1, y);
                ok = (g[0] == sum && g[1] == up);
            }
        }
        if (!ok) { if (!bad) snprintf(note, nn, "first=(%u,%u) got=%.1f,%.1f,%.1f,%.1f in_region=%d", x, y, g[0], g[1], g[2], g[3], in); bad++; }
    }
    return bad;
}

static void run_e6(QP *q) {
    exp_begin(q, 6);
    STR_DECL(d, 1000);
    int ops_ok = (q->sg_ops & VK_SUBGROUP_FEATURE_BASIC_BIT) && (q->sg_ops & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) && (q->sg_stages & VK_SHADER_STAGE_COMPUTE_BIT);
    sadd(&d, "subgroup=%u min=%u max=%u shuffle_compute=%d", q->sg_size, q->sg_min, q->sg_max, ops_ok);
    if (!ops_ok || q->sg_size < 4) { sadd(&d, " reason=%s", !ops_ok ? "subgroup_shuffle_not_supported_in_compute" : "subgroup_size_below_4"); exp_end(6, "UNSUPPORTED", &d); return; }
    if (q->lost) { exp_lost(q, 6); return; }
    const uint32_t W = 128, H = 48; VkRect2D region = { { 5, 3 }, { 90, 40 } }; VkRect2D area = { { 0, 0 }, { W, H } };
    uint32_t L = q->sg_size >= 32 ? 32 : q->sg_size;       /* logical simdgroup width; < 32 only when the device subgroup is narrower (generator self-test) */
    int logical32 = (L == 32);
    sadd(&d, " logical=%u logical_applicable=%d", L, logical32);
    QPImg im; QPBuf bf; const char *st = ""; VkResult r;
    const char *part_res[2] = { "not_run", "not_run" };
    for (int part = 0; part < 2; part++) {   /* 0 = plain compute (the emulation without tile shading), 1 = per-tile dispatch */
        if (part == 1) { const char *why; if (!tile_ok(q, &why)) { part_res[1] = "UNSUPPORTED"; sadd(&d, " tile_part_reason=%s", why); continue; } }
        if (q->lost) { part_res[part] = "SKIPPED_DEVICE_LOST"; continue; }
        qp_kspec k = KS[part ? K_SIMD_T : K_SIMD_P]; k.logical = L;
        r = img_create(q, &im, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &st);
        if (r) { sadd(&d, " p%d_%s=%s(%d)", part, st, VKR(r)); part_res[part] = "REFUSED"; img_destroy(q, &im); continue; }
        buf_create(q, &bf, (VkDeviceSize)W * H * 8);
        VkPipeline pipe; r = make_cs(q, &k, &pipe, &st);
        if (r) { sadd(&d, " p%d_%s=%s(%d)", part, st, VKR(r)); part_res[part] = "REFUSED"; buf_destroy(q, &bf); img_destroy(q, &im); continue; }
        VkDescriptorSet ds; alloc_set(q, &im, &ds);
        uint32_t pc[8]; pc_area(pc, area); pc[4] = (uint32_t)region.offset.x; pc[5] = (uint32_t)region.offset.y; pc[6] = region.offset.x + region.extent.width; pc[7] = region.offset.y + region.extent.height;
        if (!part) {
            E6Rec t = { .im = &im, .bf = &bf, .pipe = pipe, .ds = ds, .gx = (W + 63) / 64, .gy = H }; memcpy(t.pc, pc, 32);
            r = submit_wait(q, rec_e6_plain, &t);
        } else {
            TileRec t = { .im = &im, .bf = &bf, .pipe = pipe, .ds = ds, .area = area, .rp_flags = q->o.rp_flags >= 0 ? q->o.rp_flags : (int)QP_TS_ENABLE_BIT, .attl = VK_IMAGE_LAYOUT_GENERAL,
                .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .clear = clr(0.5f, 0.5f, 0.5f, 0.5f), .bracket = 1 };
            uint32_t tw, th, gx, gy; tile_groups(q, &im, area, 64, 0, &gx, &gy, &tw, &th); t.gx = gx; t.gy = th; memcpy(t.pc, pc, 32);
            r = submit_wait(q, rec_tile_pass, &t);
        }
        if (r) { sadd(&d, " p%d_submit_wait=%s(%d)", part, VKR(r)); part_res[part] = "FAILED_RUN"; continue; }
        char note[200]; int lb; uint32_t slo, shi; int bad = e6_check(q, bf.map, W, H, region, L, note, sizeof note, &lb, &slo, &shi);
        out("qcomprobe-detail: E6 part=%s mismatch=%d lane_layout_bad=%d subgroup_size_seen=%u..%u %s", part ? "tile" : "plain", bad, lb, slo, shi, note);
        sadd(&d, " p%d_sgs_seen=%u..%u", part, slo, shi);
        part_res[part] = bad ? "MISMATCH" : "PASS";
        vkDestroyPipeline(q->dev, pipe, NULL); buf_destroy(q, &bf); img_destroy(q, &im);
    }
    sadd(&d, " plain=%s tile=%s region=%d,%d,%ux%u", part_res[0], part_res[1], region.offset.x, region.offset.y, region.extent.width, region.extent.height);
    int any_fail = 0, any_pass = 0;
    for (int i = 0; i < 2; i++) { if (!strcmp(part_res[i], "PASS")) any_pass = 1; else if (strcmp(part_res[i], "UNSUPPORTED") && strcmp(part_res[i], "not_run")) any_fail = 1; }
    if (!logical32) { sadd(&d, " reason=device_subgroup_%u_below_32_selftest_of_generator_only", q->sg_size); exp_end(6, any_fail ? "FAIL" : "UNSUPPORTED", &d); return; }
    exp_end(6, (any_fail || !any_pass) ? "FAIL" : "PASS", &d);
}

/* ------------------------------------------------------------------ setup */
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

static int setup(QP *q) {
    VkResult r; uint32_t iv = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion ev = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (ev) ev(&iv);
    uint32_t want = iv >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : iv;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "qcom-tile-probe", .apiVersion = want };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    r = vkCreateInstance(&ici, NULL, &q->inst);
    if (r) { out("qcomprobe: setup status=FAIL reason=vkCreateInstance result=%s(%d)", VKR(r)); return 0; }
    uint32_t np = 0; vkEnumeratePhysicalDevices(q->inst, &np, NULL);
    if (!np) { out("qcomprobe: setup status=FAIL reason=no_physical_device"); return 0; }
    VkPhysicalDevice *pds = calloc(np, sizeof *pds); vkEnumeratePhysicalDevices(q->inst, &np, pds);
    int pick = -1;
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pds[i], NULL, &ne, NULL); VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e); vkEnumerateDeviceExtensionProperties(pds[i], NULL, &ne, e);
        int ts = has_ext(e, ne, "VK_QCOM_tile_shading"); free(e);
        out("qcomprobe: info physical_device index=%u name=\"%s\" type=%d api=%u.%u.%u driver=0x%x vendor=0x%x tile_shading_listed=%d", i, p.deviceName, (int)p.deviceType,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, ts);
        if (pick < 0 && ts) pick = (int)i;
    }
    if (q->o.device >= 0) pick = q->o.device < (int)np ? q->o.device : 0; else if (pick < 0) pick = 0;
    if (q->o.list) { out("qcomprobe: info list_only=1"); return 2; }
    q->pd = pds[pick]; free(pds);
    vkGetPhysicalDeviceProperties(q->pd, &q->props); vkGetPhysicalDeviceMemoryProperties(q->pd, &q->mp);
    q->api = q->props.apiVersion;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(q->pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(q->pd, NULL, &ne, ex);
    q->ext_ts = has_ext(ex, ne, "VK_QCOM_tile_shading"); q->ext_tp = has_ext(ex, ne, "VK_QCOM_tile_properties"); q->ext_tmh = has_ext(ex, ne, "VK_QCOM_tile_memory_heap");
    q->ext_lr = has_ext(ex, ne, "VK_KHR_dynamic_rendering_local_read");
    int ext_dr = has_ext(ex, ne, "VK_KHR_dynamic_rendering"), ext_s2 = has_ext(ex, ne, "VK_KHR_synchronization2");
    free(ex);
    out("qcomprobe: info device name=\"%s\" api=%u.%u.%u driver=0x%x ext[tile_shading=%d tile_properties=%d tile_memory_heap=%d dynamic_rendering_local_read=%d]", q->props.deviceName,
        VK_VERSION_MAJOR(q->api), VK_VERSION_MINOR(q->api), VK_VERSION_PATCH(q->api), q->props.driverVersion, q->ext_ts, q->ext_tp, q->ext_tmh, q->ext_lr);
    if (q->api < VK_API_VERSION_1_3 && !(ext_dr && ext_s2)) { out("qcomprobe: setup status=FAIL reason=needs_vulkan_1_3_or_dynamic_rendering_and_synchronization2"); return 0; }
    if (q->api < VK_API_VERSION_1_3 || want < VK_API_VERSION_1_3) { out("qcomprobe: setup status=FAIL reason=instance_or_device_below_vulkan_1_3 (probe uses core 1.3 entry points)"); return 0; }

    /* features / properties */
    QPTileShadingFeat tsf = { .sType = QP_ST_TILE_SHADING_FEATURES }; QPTilePropsFeat tpf = { .sType = QP_ST_TILE_PROPS_FEATURES }; QPLocalReadFeat lrf = { .sType = QP_ST_LOCAL_READ_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    void **tail = &f13.pNext;
    if (q->ext_ts) { *tail = &tsf; tail = &tsf.pNext; } if (q->ext_tp) { *tail = &tpf; tail = &tpf.pNext; } if (q->ext_lr) { *tail = &lrf; tail = &lrf.pNext; }
    vkGetPhysicalDeviceFeatures2(q->pd, &f2);
    q->tsf = tsf; q->tpf = tpf; q->f_dyn = f13.dynamicRendering; q->f_s2 = f13.synchronization2; q->f_lr = q->ext_lr && lrf.dynamicRenderingLocalRead;
    q->f_wwf = f2.features.shaderStorageImageWriteWithoutFormat; q->f_rwf = f2.features.shaderStorageImageReadWithoutFormat;
    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES }; VkPhysicalDeviceVulkan13Properties p13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    QPTileShadingProps tsp = { .sType = QP_ST_TILE_SHADING_PROPERTIES };
    VkPhysicalDeviceProperties2 pr2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11 }; p11.pNext = &p13; if (q->ext_ts) p13.pNext = &tsp;
    vkGetPhysicalDeviceProperties2(q->pd, &pr2);
    q->tsp = tsp; q->sg_size = p11.subgroupSize; q->sg_ops = p11.subgroupSupportedOperations; q->sg_stages = p11.subgroupSupportedStages; q->sg_min = p13.minSubgroupSize; q->sg_max = p13.maxSubgroupSize;
    if (!q->f_dyn || !q->f_s2) { out("qcomprobe: setup status=FAIL reason=dynamicRendering=%d synchronization2=%d", q->f_dyn, q->f_s2); return 0; }
    out("qcomprobe: info features tileShading=%u fragmentStage=%u colorAtt=%u depthAtt=%u stencilAtt=%u inputAtt=%u sampledAtt=%u perTileDraw=%u perTileDispatch=%u dispatchTile=%u apron=%u anisoApron=%u atomicOps=%u imageProcessing=%u tileProperties=%u dynamicRenderingLocalRead=%d",
        tsf.tileShading, tsf.tileShadingFragmentStage, tsf.tileShadingColorAttachments, tsf.tileShadingDepthAttachments, tsf.tileShadingStencilAttachments, tsf.tileShadingInputAttachments,
        tsf.tileShadingSampledAttachments, tsf.tileShadingPerTileDraw, tsf.tileShadingPerTileDispatch, tsf.tileShadingDispatchTile, tsf.tileShadingApron, tsf.tileShadingAnisotropicApron,
        tsf.tileShadingAtomicOps, tsf.tileShadingImageProcessing, tpf.tileProperties, q->f_lr);
    out("qcomprobe: info props maxApronSize=%u preferNonCoherent=%u tileGranularity=%ux%u maxTileShadingRate=%ux%u subgroup[size=%u min=%u max=%u ops=0x%x stages=0x%x] shaderStorageImageWriteWithoutFormat=%d ReadWithoutFormat=%d",
        tsp.maxApronSize, tsp.preferNonCoherent, tsp.tileGranularity.width, tsp.tileGranularity.height, tsp.maxTileShadingRate.width, tsp.maxTileShadingRate.height,
        q->sg_size, q->sg_min, q->sg_max, q->sg_ops, q->sg_stages, q->f_wwf, q->f_rwf);

    /* queue */
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(q->pd, &nq, NULL); VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(q->pd, &nq, qp);
    q->qf = ~0u; for (uint32_t i = 0; i < nq; i++) if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { q->qf = i; break; }
    free(qp); if (q->qf == ~0u) { out("qcomprobe: setup status=FAIL reason=no_graphics_compute_queue"); return 0; }

    /* device: first with the QCOM chain (exactly the feature subset LAGFX_QCOM_TILE=1 requests), retried without it if refused */
    float prio = 1.0f; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = q->qf, .queueCount = 1, .pQueuePriorities = &prio };
    for (int attempt = 0; attempt < 2; attempt++) {
        int with_qcom = (attempt == 0) && q->ext_ts && q->ext_tp && tsf.tileShading;
        if (attempt == 1 && !q->qcom_fallback) break;
        if (attempt == 0 && !with_qcom) continue;
        const char *exts[4]; uint32_t nex = 0;
        if (with_qcom) { exts[nex++] = "VK_QCOM_tile_properties"; exts[nex++] = "VK_QCOM_tile_shading"; }
        if (q->ext_lr && q->f_lr) exts[nex++] = "VK_KHR_dynamic_rendering_local_read";
        QPTileShadingFeat en = { .sType = QP_ST_TILE_SHADING_FEATURES, .tileShading = tsf.tileShading, .tileShadingColorAttachments = tsf.tileShadingColorAttachments, .tileShadingPerTileDispatch = tsf.tileShadingPerTileDispatch,
            .tileShadingDispatchTile = tsf.tileShadingDispatchTile, .tileShadingInputAttachments = tsf.tileShadingInputAttachments, .tileShadingSampledAttachments = tsf.tileShadingSampledAttachments, .tileShadingAtomicOps = tsf.tileShadingAtomicOps };
        QPTilePropsFeat ent = { .sType = QP_ST_TILE_PROPS_FEATURES, .tileProperties = tpf.tileProperties }; QPLocalReadFeat enl = { .sType = QP_ST_LOCAL_READ_FEATURES, .dynamicRenderingLocalRead = (q->ext_lr && q->f_lr) };
        VkPhysicalDeviceVulkan13Features e13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
        VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e13 };
        e2.features.shaderStorageImageWriteWithoutFormat = (VkBool32)q->f_wwf; e2.features.shaderStorageImageReadWithoutFormat = (VkBool32)q->f_rwf;
        void **t2 = &e13.pNext; if (with_qcom) { *t2 = &en; t2 = &en.pNext; *t2 = &ent; t2 = &ent.pNext; } if (q->ext_lr && q->f_lr) { *t2 = &enl; t2 = &enl.pNext; }
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = nex, .ppEnabledExtensionNames = exts };
        r = vkCreateDevice(q->pd, &dci, NULL, &q->dev);
        out("qcomprobe: info device_create attempt=%d qcom_chain=%d result=%s(%d)", attempt, with_qcom, VKR(r));
        if (with_qcom) { q->qcom_create_res = r; if (r) q->qcom_fallback = 1; else q->qcom_enabled = 1; }
        if (!r) break;
        if (!with_qcom) { out("qcomprobe: setup status=FAIL reason=vkCreateDevice result=%s(%d)", VKR(r)); return 0; }
    }
    if (!q->dev) {   /* the extension is not listed: plain device */
        const char *exts[2]; uint32_t nex = 0; if (q->ext_lr && q->f_lr) exts[nex++] = "VK_KHR_dynamic_rendering_local_read";
        QPLocalReadFeat enl = { .sType = QP_ST_LOCAL_READ_FEATURES, .dynamicRenderingLocalRead = (q->ext_lr && q->f_lr) };
        VkPhysicalDeviceVulkan13Features e13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE, .pNext = (q->ext_lr && q->f_lr) ? &enl : NULL };
        VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e13 };
        e2.features.shaderStorageImageWriteWithoutFormat = (VkBool32)q->f_wwf; e2.features.shaderStorageImageReadWithoutFormat = (VkBool32)q->f_rwf;
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = nex, .ppEnabledExtensionNames = exts };
        r = vkCreateDevice(q->pd, &dci, NULL, &q->dev);
        out("qcomprobe: info device_create attempt=plain qcom_chain=0 result=%s(%d)", VKR(r));
        if (r) { out("qcomprobe: setup status=FAIL reason=vkCreateDevice result=%s(%d)", VKR(r)); return 0; }
    }
    vkGetDeviceQueue(q->dev, q->qf, 0, &q->queue);
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(q->inst, "vkGetDeviceProcAddr");
    q->cmdBeginRendering = (PFN_vkCmdBeginRendering)gdpa(q->dev, "vkCmdBeginRendering"); q->cmdEndRendering = (PFN_vkCmdEndRendering)gdpa(q->dev, "vkCmdEndRendering");
    q->cmdBarrier2 = (PFN_vkCmdPipelineBarrier2)gdpa(q->dev, "vkCmdPipelineBarrier2");
    if (!q->cmdBeginRendering || !q->cmdEndRendering || !q->cmdBarrier2) { out("qcomprobe: setup status=FAIL reason=core_1_3_proc_addr_missing"); return 0; }
    if (q->qcom_enabled) {
        q->getTileProps = (PFN_qpGetTileProps)gdpa(q->dev, "vkGetDynamicRenderingTilePropertiesQCOM"); q->cmdDispatchTile = (PFN_qpDispatchTile)gdpa(q->dev, "vkCmdDispatchTileQCOM");
        q->cmdBeginPerTile = (PFN_qpPerTile)gdpa(q->dev, "vkCmdBeginPerTileExecutionQCOM"); q->cmdEndPerTile = (PFN_qpPerTile)gdpa(q->dev, "vkCmdEndPerTileExecutionQCOM");
        out("qcomprobe: info procs getTileProps=%d dispatchTile=%d beginPerTile=%d endPerTile=%d", !!q->getTileProps, !!q->cmdDispatchTile, !!q->cmdBeginPerTile, !!q->cmdEndPerTile);
    }
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = q->qf };
    vkCreateCommandPool(q->dev, &pci, NULL, &q->pool);
    VkDescriptorSetLayoutBinding lb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    vkCreateDescriptorSetLayout(q->dev, &li, NULL, &q->dsl);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &q->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    vkCreatePipelineLayout(q->dev, &pli, NULL, &q->pl_cs);
    VkPushConstantRange pcg = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo plg = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcg };
    vkCreatePipelineLayout(q->dev, &plg, NULL, &q->pl_gfx);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256, .poolSizeCount = 1, .pPoolSizes = &ps };
    vkCreateDescriptorPool(q->dev, &dpi, NULL, &q->dpool);
    return 1;
}

/* ------------------------------------------------------------------ selftest (CPU only) and SPIR-V dump */
static int selftest(void) {
    int bad = 0; Str s; char buf[512]; s.p = buf; s.n = 0; s.cap = sizeof buf; buf[0] = 0;
    /* half round trip over every non-NaN half */
    int hbad = 0; for (uint32_t h = 0; h < 65536; h++) { if (((h >> 10) & 31) == 31) continue; float f = h2f((uint16_t)h); uint16_t b = f2h(f); if (b != h && !(f == 0.0f)) hbad++; }
    sadd(&s, "half_roundtrip_bad=%d ", hbad); bad += hbad;
    uint16_t a = f2h(0.0625f), c = f2h(1.0f / 15.0f); if (a != 0x2c00 || fabsf(h2f(c) - 1.0f / 15.0f) > 5e-5f) { bad++; sadd(&s, "half_values_bad "); }
    /* model: rmw is exact on multiples of 1/16 and equals (g/2+1/8, r/2+1/4, b, a) */
    float in[4] = { 0.5f, 0.25f, 0.75f, 1.0f }, o[4]; rmw_px(in, o); if (o[0] != 0.25f || o[1] != 0.5f || o[2] != 0.75f || o[3] != 1.0f) { bad++; sadd(&s, "rmw_bad "); }
    /* e5 model spot checks: outside both rects = rmw(clear), inside s1 only = rmw(pattern), inside s2 = pattern2 */
    VkRect2D s1 = { { 16, 8 }, { 120, 80 } }, s2 = { { 64, 40 }, { 100, 70 } }; float *ex = malloc(192 * 128 * 16); e5_model(192, 128, s1, s2, ex);
    float cl[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, e0[4]; rmw_px(cl, e0); if (memcmp(ex, e0, 16)) { bad++; sadd(&s, "e5_outside_bad "); }
    float *p = ex + ((size_t)10 * 192 + 20) * 4; float pat[4] = { (float)(20 & 15) * 0.0625f, (float)(10 & 15) * 0.0625f, 0.25f, 1.0f }, e1[4]; rmw_px(pat, e1); if (memcmp(p, e1, 16)) { bad++; sadd(&s, "e5_s1_bad "); }
    p = ex + ((size_t)50 * 192 + 70) * 4; float p2[4] = { (float)(70 & 15) * 0.0625f, (float)(50 & 15) * 0.0625f, 0.75f, 1.0f }; if (memcmp(p, p2, 16)) { bad++; sadd(&s, "e5_s2_bad "); } free(ex);
    /* simd model: logical-32 butterfly sum of lanes 0..31 at x=0..31,y=0 */
    float sum = 0; for (int j = 0; j < 32; j++) sum += simd_f(j, 0); float want = 0; for (int j = 0; j < 32; j++) want += (float)((j * 3) % 61 + 1); if (sum != want) { bad++; sadd(&s, "simd_model_bad "); }
    /* every kernel generates, header is sane, twin differs only by tile tokens */
    int ngen = 0;
    for (int k = 0; k < K_COUNT; k++) for (int tw = 0; tw < 2; tw++) {
        uint32_t n; uint32_t *w = qp_gen_kernel(&KS[k], tw, &n);
        if (!w || n < 20 || w[0] != 0x07230203u || w[3] < 8) { bad++; sadd(&s, "gen_bad_%s ", KS[k].name); } else ngen++;
        if (w && KS[k].tile && !tw) { int has_cap = 0, has_sc = 0; for (uint32_t i = 5; i < n; i++) { if ((w[i] & 0xffff) == 17 && w[i + 1] == 4495) has_cap = 1; if ((w[i] & 0xffff) == 59 && w[i + 3] == 4491) has_sc = 1; } if (!has_cap || !has_sc) { bad++; sadd(&s, "tokens_missing_%s ", KS[k].name); } }
        if (w && KS[k].tile && tw) { for (uint32_t i = 5; i < n; i++) if (((w[i] & 0xffff) == 17 && w[i + 1] == 4495) || ((w[i] & 0xffff) == 59 && w[i + 3] == 4491)) { bad++; sadd(&s, "twin_has_tile_%s ", KS[k].name); break; } }
        free(w);
    }
    uint32_t n; uint32_t *w = qp_gen_vert(&n); if (!w || w[0] != 0x07230203u) bad++; else ngen++; free(w); w = qp_gen_frag(&n); if (!w || w[0] != 0x07230203u) bad++; else ngen++; free(w);
    sadd(&s, "modules_generated=%d", ngen);
    out("qcomprobe: selftest status=%s %s", bad ? "FAIL" : "PASS", s.p);
    return bad;
}

static int dump_spirv(const char *dir) {
    int n = 0;
    for (int k = 0; k < K_COUNT; k++) for (int tw = 0; tw < 2; tw++) {
        if (tw && !KS[k].tile) continue;
        char path[512]; snprintf(path, sizeof path, "%s/%s%s.spv", dir, KS[k].name, tw ? ".twin" : ""); uint32_t nw; uint32_t *w = qp_gen_kernel(&KS[k], tw, &nw);
        FILE *f = fopen(path, "wb"); if (!f) { out("qcomprobe: dump status=FAIL path=%s", path); return -1; } fwrite(w, 4, nw, f); fclose(f); free(w); n++;
    }
    uint32_t nw; uint32_t *w; char path[512];
    snprintf(path, sizeof path, "%s/gfx_vert.spv", dir); w = qp_gen_vert(&nw); FILE *f = fopen(path, "wb"); if (f) { fwrite(w, 4, nw, f); fclose(f); n++; } free(w);
    snprintf(path, sizeof path, "%s/gfx_frag.spv", dir); w = qp_gen_frag(&nw); f = fopen(path, "wb"); if (f) { fwrite(w, 4, nw, f); fclose(f); n++; } free(w);
    out("qcomprobe: dump status=OK dir=%s modules=%d note=\"*.twin.spv replace the tile tokens by core ones for spirv-val; the others carry capability 4495\"", dir, n);
    return n;
}

static void usage(void) {
    puts("qcom-tile-probe: VK_QCOM_tile_shading device probe (experiments E1..E6, docs/qcom-tile-shading.md section 7)\n"
         "  --only E1,E3      run only these experiments (default: all)\n"
         "  --device N        physical device index (default: first with VK_QCOM_tile_shading, else 0)\n"
         "  --list            list physical devices and exit\n"
         "  --timeout S       watchdog seconds per experiment (default 60; process exits 3 when it fires)\n"
         "  --wait-ms MS      bounded fence wait per submit (default 8000)\n"
         "  --rp-flags N      VkRenderPassTileShadingCreateInfoQCOM::flags for E3-E6 (default 1 = ENABLE; 3 = ENABLE|PER_TILE_EXECUTION)\n"
         "  --no-risky        skip cases that use APIs outside their stated validity (storage descriptor of a view without STORAGE usage; tile-relative store)\n"
         "  --strict          exit 1 when any experiment is FAIL\n"
         "  --no-isolate      run all experiments in this process (default: one child process per experiment; a crash is reported as FAIL reason=child_crashed)\n"
         "  --mock-tile       analysis self-test for devices WITHOUT tile shading (plain kernels, no render pass); every line carries mock=1, never a device result\n"
         "  --selftest        CPU-only checks of the models and the SPIR-V generator, then exit\n"
         "  --dump-spirv DIR  write every generated module (and *.twin.spv without tile tokens) to DIR, then exit");
}

/* parent: one child process per experiment, so a driver crash / hang in one experiment cannot take the other results with it */
static int parent_run(const Opts *o) {
    static char buf[1 << 17]; int first = 1, exit_code = 0, setup_failed = 0, lost_any = 0; char tail[300] = ""; char stat[8][16] = { { 0 } };
    for (int n = 1; n <= 6 && !setup_failed; n++) {
        if (!(o->only_mask & (1 << n))) continue;
        char en[8]; snprintf(en, sizeof en, "E%d", n); const char *ex[] = { "--child", "--only", en }; int ws;
        snprintf(g_exp, sizeof g_exp, "E%d", n); arm(o->exp_timeout_s * 2 + 70);
        spawn_self(o, ex, 3, buf, sizeof buf, &ws);
        arm(0);
        int got = 0; char *save = NULL; char want[24]; snprintf(want, sizeof want, "qcomprobe: E%d status=", n);
        for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(ln, "qcomprobe: info tool=", 21)) continue;
            if (!strncmp(ln, "qcomprobe: info ", 16)) { if (first) out("%s", ln); continue; }
            if (!strncmp(ln, "qcomprobe: summary ", 19)) {
                const char *t = strstr(ln, "qcom_chain_enabled="); if (t) snprintf(tail, sizeof tail, "%s", t);
                if (strstr(ln, "device_lost=1")) lost_any = 1;
                if (strstr(ln, "setup=FAIL")) setup_failed = 1;
                continue; }
            if (!strncmp(ln, want, strlen(want))) {
                got = 1; const char *st = ln + strlen(want); snprintf(stat[n], sizeof stat[n], "%s", !strncmp(st, "PASS", 4) ? "PASS" : !strncmp(st, "UNSUPPORTED", 11) ? "UNSUPPORTED" : "FAIL"); }
            out("%s", ln);
        }
        first = 0;
        if (WIFEXITED(ws) && WEXITSTATUS(ws) == 3) exit_code = 3;
        if (setup_failed) { exit_code = 2; break; }
        if (!got) {
            snprintf(stat[n], sizeof stat[n], "FAIL");
            if (WIFSIGNALED(ws)) out("qcomprobe: E%d status=FAIL reason=child_crashed signal=%d", n, WTERMSIG(ws));
            else out("qcomprobe: E%d status=FAIL reason=child_exit_without_result exit=%d", n, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
        }
    }
    int np = 0, nf = 0, nu = 0; STR_DECL(s, 400);
    for (int n = 1; n <= 6; n++) { const char *st = stat[n][0] ? stat[n] : "NOT_RUN"; sadd(&s, "E%d=%s ", n, st); if (!strcmp(st, "PASS")) np++; else if (!strcmp(st, "FAIL")) nf++; else if (!strcmp(st, "UNSUPPORTED")) nu++; }
    if (setup_failed) { out("qcomprobe: summary setup=FAIL"); return 2; }
    out("qcomprobe: summary %spass=%d fail=%d unsupported=%d isolated=1 device_lost_any=%d %s", s.p, np, nf, nu, lost_any, tail);
    return exit_code ? exit_code : ((o->strict && nf) ? 1 : 0);
}

int main(int argc, char **argv) {
    QP q; memset(&q, 0, sizeof q);
    q.o.device = -1; q.o.only_mask = 0x7e; q.o.rp_flags = -1; q.o.exp_timeout_s = 60; q.o.wait_ms = 8000; q.o.e3_row = -1;
    int do_selftest = 0; const char *dump = NULL;
    g_self = access("/proc/self/exe", X_OK) == 0 ? "/proc/self/exe" : argv[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--selftest")) do_selftest = 1;
        else if (!strcmp(a, "--strict")) q.o.strict = 1;
        else if (!strcmp(a, "--mock-tile")) g_mock = 1;
        else if (!strcmp(a, "--no-risky")) q.o.no_risky = 1;
        else if (!strcmp(a, "--no-isolate")) q.o.no_isolate = 1;
        else if (!strcmp(a, "--child")) q.o.child = 1;
        else if (!strcmp(a, "--e3-row") && i + 1 < argc) q.o.e3_row = atoi(argv[++i]);
        else if (!strcmp(a, "--list")) q.o.list = 1;
        else if (!strcmp(a, "--device") && i + 1 < argc) q.o.device = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && i + 1 < argc) q.o.exp_timeout_s = atoi(argv[++i]);
        else if (!strcmp(a, "--wait-ms") && i + 1 < argc) q.o.wait_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--rp-flags") && i + 1 < argc) q.o.rp_flags = atoi(argv[++i]);
        else if (!strcmp(a, "--dump-spirv") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(a, "--only") && i + 1 < argc) { q.o.only_mask = 0; for (const char *p = argv[++i]; *p; p++) if (*p == 'E' && p[1] >= '1' && p[1] <= '6') q.o.only_mask |= 1 << (p[1] - '0'); }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, on_alarm);
    out("qcomprobe: info tool=qcom-tile-probe version=%d spec_basis=vk.xml(VK_QCOM_tile_shading_rev2,VK_QCOM_tile_properties)+SPV_QCOM_tile_shading_rev1 isolate=%d", QP_VERSION, !q.o.no_isolate);
    if (do_selftest) return selftest() ? 1 : 0;
    if (dump) return dump_spirv(dump) > 0 ? 0 : 2;
    if (!q.o.child && !q.o.no_isolate && !q.o.list) return parent_run(&q.o);
    arm(60);
    int ok = setup(&q);
    arm(0);
    if (!ok) { out("qcomprobe: summary setup=FAIL"); return q.o.list ? 0 : 2; }
    if (q.o.list) return 0;
    if (q.o.e3_row >= 0) {   /* child of E3: one no-STORAGE row */
        snprintf(g_exp, sizeof g_exp, "E3"); arm(q.o.exp_timeout_s);
        char row[300]; const char *res = q.o.e3_row < E3_NF && tile_ok(&q, &(const char *){ "" }) ? e3_row(&q, q.o.e3_row, 0, row, sizeof row) : "UNSUPPORTED_PREREQ(tile_path_not_available)";
        if (q.o.e3_row >= E3_NF) return 2;
        if (res[0] == 'U' && strstr(res, "tile_path")) row[0] = 0;
        out("qcomprobe-detail: E3 fmt=%s storage_usage=0 result=%s %s", fmt_info(E3F[q.o.e3_row].f)->name, res, row);
        return 0;
    }
    void (*fn[7])(QP *) = { 0, run_e1, run_e2, run_e3, run_e4, run_e5, run_e6 };
    for (int n = 1; n <= 6; n++) if (exp_selected(&q, n)) fn[n](&q);
    int np = 0, nf = 0, nu = 0; STR_DECL(s, 400);
    for (int n = 1; n <= 6; n++) { const char *st = g_status[n] ? g_status[n] : "NOT_RUN"; sadd(&s, "E%d=%s ", n, st); if (!strcmp(st, "PASS")) np++; else if (!strcmp(st, "FAIL")) nf++; else if (!strcmp(st, "UNSUPPORTED")) nu++; }
    out("qcomprobe: summary %spass=%d fail=%d unsupported=%d device_lost=%d qcom_chain_enabled=%d device=\"%s\"%s", s.p, np, nf, nu, q.lost, q.qcom_enabled, q.props.deviceName, g_mock ? " mock=1" : "");
    if (!q.lost) { vkDeviceWaitIdle(q.dev); }
    return (q.o.strict && nf) ? 1 : 0;
}
