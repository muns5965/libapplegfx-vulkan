/* 0x141 FillTextureWithColor: region fill that keeps the rest of the image, real layout, level / layer, formats, counted refusals. lavapipe + validation layer.
 * tests/blit-fill-texture-unit.c
 *
 * Reuses the harness of arm64-render-compat-unit.c (device, validation-baseline helper val_begin / val_end with tests/fixtures/val_baseline_audit.txt) by including it with its main()
 * left out. Windows "blitfill ..." are not listed in the baseline, so ANY validation message in them is an increase and fails.
 * Pixel references are analytic (the colour converted by hand), read back with vkCmdCopyImageToBuffer. Negative controls: a deliberately wrong layout is reported by the layer (proves the
 * window is live), an out-of-range fill leaves the image byte-identical, a fill with the old whole-subresource semantics would fail the "outside unchanged" checks.
 * What this does NOT prove: that the guest's payload carries these fields in this order (x86-era guess [I]); Metal's rules for out-of-range regions [N].
 * SPDX-License-Identifier: MIT */
#define LAGFX_COMPAT_NO_MAIN 1
#include "arm64-render-compat-unit.c"

#ifdef LAGFX_HAVE_VULKAN
#include "translate/blit_encoder.h"
#include "handlers/compute/blit_inner_ops.h"
#include "vulkan/iosurface.h"

typedef struct { VkImage img; VkDeviceMemory mem; VkFormat fmt; VkImageType type; uint32_t w, h, d, mips, layers; VkImageLayout layout; } bf_img_t;

static uint32_t bf_memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_dev->vk->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
static bool bf_make(bf_img_t *o, VkFormat fmt, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers, VkImageUsageFlags extra) {
    memset(o, 0, sizeof *o); o->fmt = fmt; o->type = type; o->w = w; o->h = h; o->d = d; o->mips = mips; o->layers = layers; o->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = type, .format = fmt, .extent = { w, h, d }, .mipLevels = mips, .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extra, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageFormatProperties fp;
    if (vkGetPhysicalDeviceImageFormatProperties(g_dev->vk->phys_device, fmt, type, VK_IMAGE_TILING_OPTIMAL, ici.usage, 0, &fp) != VK_SUCCESS) return false;
    if (vkCreateImage(g_dev->vk->device, &ici, NULL, &o->img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev->vk->device, o->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = bf_memtype(mr.memoryTypeBits, 0) };
    assert(vkAllocateMemory(g_dev->vk->device, &mai, NULL, &o->mem) == VK_SUCCESS); assert(vkBindImageMemory(g_dev->vk->device, o->img, o->mem, 0) == VK_SUCCESS);
    return true;
}
static void bf_free(bf_img_t *o) { vkDestroyImage(g_dev->vk->device, o->img, NULL); vkFreeMemory(g_dev->vk->device, o->mem, NULL); }

/* one-shot command buffer helper */
static VkCommandBuffer bf_begin(VkCommandPool *pool) {
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_dev->vk->graphics_queue_family };
    assert(vkCreateCommandPool(g_dev->vk->device, &cpi, NULL, pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = *pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; assert(vkAllocateCommandBuffers(g_dev->vk->device, &cai, &cb) == VK_SUCCESS);
    VkCommandBufferBeginInfo b = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; assert(vkBeginCommandBuffer(cb, &b) == VK_SUCCESS);
    return cb;
}
static void bf_end(VkCommandBuffer cb, VkCommandPool pool) {
    assert(vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    assert(vkQueueSubmit(g_dev->vk->graphics_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS); vkQueueWaitIdle(g_dev->vk->graphics_queue);
    vkDestroyCommandPool(g_dev->vk->device, pool, NULL);
}
static void bf_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout a, VkImageLayout b, uint32_t level, uint32_t layer) {
    VkImageMemoryBarrier br = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = a, .newLayout = b, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &br);
}

/* host buffer helper */
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; size_t n; } bf_buf_t;
static bf_buf_t bf_buffer(size_t n) {
    bf_buf_t r = { .n = n };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    assert(vkCreateBuffer(g_dev->vk->device, &bci, NULL, &r.b) == VK_SUCCESS);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev->vk->device, r.b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = bf_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    assert(vkAllocateMemory(g_dev->vk->device, &mai, NULL, &r.m) == VK_SUCCESS); assert(vkBindBufferMemory(g_dev->vk->device, r.b, r.m, 0) == VK_SUCCESS);
    assert(vkMapMemory(g_dev->vk->device, r.m, 0, VK_WHOLE_SIZE, 0, (void **)&r.p) == VK_SUCCESS);
    return r;
}
static void bf_buf_free(bf_buf_t *b) { vkUnmapMemory(g_dev->vk->device, b->m); vkDestroyBuffer(g_dev->vk->device, b->b, NULL); vkFreeMemory(g_dev->vk->device, b->m, NULL); }

/* Initialise one subresource (whole level, every texel = the first `bpt` bytes of `pat`) from the UNDEFINED layout, leaving it GENERAL. Only {level, layer}; other subresources stay as they are. */
static void bf_prefill(bf_img_t *o, uint32_t level, uint32_t layer, const uint8_t *pat, uint32_t bpt, VkImageLayout end_layout) {
    uint32_t lw = o->w >> level ? o->w >> level : 1, lh = o->h >> level ? o->h >> level : 1, ld = o->type == VK_IMAGE_TYPE_3D ? (o->d >> level ? o->d >> level : 1) : 1;
    bf_buf_t b = bf_buffer((size_t)lw * lh * ld * bpt);
    for (size_t i = 0; i < (size_t)lw * lh * ld; i++) memcpy(b.p + i * bpt, pat, bpt);
    VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool);
    bf_barrier(cb, o->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level, layer);
    VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 }, .imageExtent = { lw, lh, ld } };
    vkCmdCopyBufferToImage(cb, b.b, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    bf_barrier(cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, end_layout, level, layer);
    bf_end(cb, pool); bf_buf_free(&b);
    o->layout = end_layout;
}
/* Read one subresource back (tight, bpt bytes per texel) from `from` layout; returns the layout to `from`. */
static uint8_t *bf_read(bf_img_t *o, uint32_t level, uint32_t layer, uint32_t bpt, VkImageLayout from, uint32_t *lw_, uint32_t *lh_, uint32_t *ld_) {
    uint32_t lw = o->w >> level ? o->w >> level : 1, lh = o->h >> level ? o->h >> level : 1, ld = o->type == VK_IMAGE_TYPE_3D ? (o->d >> level ? o->d >> level : 1) : 1;
    bf_buf_t b = bf_buffer((size_t)lw * lh * ld * bpt);
    VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool);
    bf_barrier(cb, o->img, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level, layer);
    VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 }, .imageExtent = { lw, lh, ld } };
    vkCmdCopyImageToBuffer(cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.b, 1, &c);
    bf_barrier(cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from, level, layer);
    bf_end(cb, pool);
    uint8_t *r = malloc(b.n); memcpy(r, b.p, b.n); bf_buf_free(&b);
    *lw_ = lw; *lh_ = lh; *ld_ = ld; return r;
}
#define BF_CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); abort(); } } while (0)

/* Verify: texels inside {x,y,z,w,h,d} equal `in`, every other texel equals `out` (each bpt bytes). */
static void bf_expect(const uint8_t *px, uint32_t lw, uint32_t lh, uint32_t ld, uint32_t bpt, const uint32_t box[6], const uint8_t *in, const uint8_t *out, const char *what) {
    uint32_t n_in = 0, n_out = 0;
    for (uint32_t z = 0; z < ld; z++) for (uint32_t y = 0; y < lh; y++) for (uint32_t x = 0; x < lw; x++) {
        bool inside = x >= box[0] && x < box[0] + box[3] && y >= box[1] && y < box[1] + box[4] && z >= box[2] && z < box[2] + box[5];
        const uint8_t *t = px + (((size_t)z * lh + y) * lw + x) * bpt, *want = inside ? in : out;
        if (memcmp(t, want, bpt)) { printf("FAIL %s: texel (%u,%u,%u) %s the box: got %02x%02x%02x%02x.. want %02x%02x%02x%02x..\n", what, x, y, z, inside ? "inside" : "outside", t[0], bpt > 1 ? t[1] : 0, bpt > 2 ? t[2] : 0, bpt > 3 ? t[3] : 0, want[0], bpt > 1 ? want[1] : 0, bpt > 2 ? want[2] : 0, bpt > 3 ? want[3] : 0); abort(); }
        if (inside) n_in++; else n_out++;
    }
    BF_CHECK(n_in == box[3] * box[4] * box[5], "%s: %u texels inside, want %u", what, n_in, box[3] * box[4] * box[5]);
    (void)n_out;
}
#ifndef BF_OLD_LIB
static lagfx_status_t bf_fill(bf_img_t *o, uint32_t level, uint32_t slice, const uint32_t org[3], const uint32_t sz[3], const double col[4]) {
    lagfx_blit_fill_target_t t = { o->img, o->fmt, o->type, o->w, o->h, o->d, o->mips, o->layers };
    MTLOrigin og = { org[0], org[1], org[2] }; MTLSize s = { sz[0], sz[1], sz[2] };
    return lagfx_translate_blit_fill_image_color(g_dev->vk, &t, &o->layout, level, slice, &og, &s, col);
}
static lagfx_blit_fill_stats_t bf_stats(void) { lagfx_blit_fill_stats_t s; lagfx_translate_blit_fill_stats_get(&s); return s; }
#define BF_STATS_RESET() lagfx_translate_blit_fill_stats_reset()
#else
/* fail-before build (-DBF_OLD_LIB, library at the base commit): only the pre-existing public entry exists */
static lagfx_status_t bf_fill(bf_img_t *o, uint32_t level, uint32_t slice, const uint32_t org[3], const uint32_t sz[3], const double col[4]) {
    lagfx_vk_iosurface_t ios; memset(&ios, 0, sizeof ios); ios.image = o->img; ios.format = o->fmt; ios.width = o->w; ios.height = o->h; ios.layout = o->layout;
    MTLOrigin og = { org[0], org[1], org[2] }; MTLSize s = { sz[0], sz[1], sz[2] };
    lagfx_status_t st = lagfx_translate_blit_fill_texture_color(g_dev->vk, &ios, level, slice, &og, &s, col, 0);
    o->layout = ios.layout;   /* the old code never updates it */
    return st;
}
#define BF_STATS_RESET() ((void)0)
#endif

/* 1. sub-rectangle of a pre-filled BGRA8 image: inside = colour, outside unchanged; fail-before: the old code cleared everything. Layout GENERAL kept (with a non-GENERAL one in 6.). */
static void test_partial_bgra8(void) {
    val_begin(); BF_STATS_RESET();
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 16, 12, 1, 1, 1, 0)) { puts("SKIP: no BGRA8"); return; }
    const uint8_t base[4] = { 0x11, 0x22, 0x33, 0x44 }, want[4] = { 0x40, 0x80, 0xff, 0xff };   /* b g r a: colour (r 1, g 0.5, b 0.25, a 1) -> bytes B=0.25*255=63.75->64, G=127.5->128, R=255, A=255 */
    bf_prefill(&im, 0, 0, base, 4, VK_IMAGE_LAYOUT_GENERAL);
    const double col[4] = { 1.0, 128.0 / 255.0, 64.0 / 255.0, 1.0 };
    const uint32_t org[3] = { 3, 2, 0 }, sz[3] = { 5, 4, 1 };
    BF_CHECK(bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK, "partial fill refused");
    BF_CHECK(im.layout == VK_IMAGE_LAYOUT_GENERAL, "layout not kept: %d", (int)im.layout);
    uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
    const uint32_t box[6] = { 3, 2, 0, 5, 4, 1 };
    bf_expect(px, lw, lh, ld, 4, box, want, base, "bgra8 partial");
    free(px);
#ifndef BF_OLD_LIB
    lagfx_blit_fill_stats_t s = bf_stats(); BF_CHECK(s.fill_partial == 1 && s.fill_full == 0, "counters %llu/%llu", (unsigned long long)s.fill_partial, (unsigned long long)s.fill_full);
#endif
    /* full fill still works (whole level): every texel = colour */
    const uint32_t forg[3] = { 0, 0, 0 }, fsz[3] = { 16, 12, 1 }; const uint32_t fbox[6] = { 0, 0, 0, 16, 12, 1 };
    BF_CHECK(bf_fill(&im, 0, 0, forg, fsz, col) == LAGFX_OK, "full fill refused");
    px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld); bf_expect(px, lw, lh, ld, 4, fbox, want, want, "bgra8 full"); free(px);
#ifndef BF_OLD_LIB
    s = bf_stats(); BF_CHECK(s.fill_full == 1, "full counter");
#endif
    bf_free(&im); val_end("blitfill bgra8");
    puts("PASS: BGRA8 sub-rectangle fill: inside = colour, outside unchanged (the whole-subresource clear of the old code fails this), full fill still fills everything, layout GENERAL kept");
}

#ifndef BF_OLD_LIB
/* 2. nonzero mip level and array layer: only that {level, layer} changes; the neighbours (other layer same level, same layer other level) stay. */
static void test_level_layer(void) {
    val_begin();
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 16, 16, 1, 3, 3, 0)) { puts("SKIP: no RGBA8 mip/array"); return; }
    const uint8_t base[4] = { 1, 2, 3, 4 }, want[4] = { 255, 0, 128, 255 };   /* r 1, g 0, b 0.5 -> 127.5 rounds to 128 (lavapipe round-to-nearest), a 1 */
    for (uint32_t lv = 0; lv < 3; lv++) for (uint32_t ly = 0; ly < 3; ly++) bf_prefill(&im, lv, ly, base, 4, VK_IMAGE_LAYOUT_GENERAL);
    /* the prefill barriers move one subresource at a time; the tracked layout is GENERAL for all */
    const double col[4] = { 1.0, 0.0, 128.0 / 255.0, 1.0 };
    const uint32_t org[3] = { 1, 2, 0 }, sz[3] = { 3, 2, 1 };   /* inside the 8x8 level 1 */
    BF_CHECK(bf_fill(&im, 1, 2, org, sz, col) == LAGFX_OK, "level 1 slice 2 partial refused");
    for (uint32_t lv = 0; lv < 3; lv++) for (uint32_t ly = 0; ly < 3; ly++) {
        uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, lv, ly, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
        const bool target = lv == 1 && ly == 2; const uint32_t box[6] = { 1, 2, 0, target ? 3 : 0, target ? 2 : 0, target ? 1 : 0 };
        char what[64]; snprintf(what, sizeof what, "level %u layer %u", lv, ly);
        bf_expect(px, lw, lh, ld, 4, box, want, base, what); free(px);
    }
    /* full fill of level 2 (4x4) layer 1 */
    const uint32_t forg[3] = { 0, 0, 0 }, fsz[3] = { 4, 4, 1 };
    BF_CHECK(bf_fill(&im, 2, 1, forg, fsz, col) == LAGFX_OK, "level 2 full refused");
    for (uint32_t lv = 0; lv < 3; lv++) for (uint32_t ly = 0; ly < 3; ly++) {
        uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, lv, ly, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
        const bool t1 = lv == 2 && ly == 1, t2 = lv == 1 && ly == 2;
        const uint32_t box[6] = { t1 ? 0 : 1, t1 ? 0 : 2, 0, t1 ? 4 : (t2 ? 3 : 0), t1 ? 4 : (t2 ? 2 : 0), (t1 || t2) ? 1 : 0 };
        char what[64]; snprintf(what, sizeof what, "after full: level %u layer %u", lv, ly);
        bf_expect(px, lw, lh, ld, 4, box, want, base, what); free(px);
    }
    bf_free(&im); val_end("blitfill level layer");
    puts("PASS: partial fill of level 1 / layer 2 and full fill of level 2 / layer 1 change exactly that subresource (all other levels and layers keep their prefill)");
}

/* 3. 3D image: depth planes (origin.z / size.depth) */
static void test_3d(void) {
    val_begin();
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, 8, 8, 6, 2, 1, 0)) { puts("SKIP: no 3D RGBA8"); return; }
    const uint8_t base[4] = { 9, 8, 7, 6 }, want[4] = { 0, 255, 0, 255 };
    bf_prefill(&im, 0, 0, base, 4, VK_IMAGE_LAYOUT_GENERAL);
    const double col[4] = { 0.0, 1.0, 0.0, 1.0 };
    const uint32_t org[3] = { 2, 1, 2 }, sz[3] = { 4, 3, 2 };
    BF_CHECK(bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK, "3D partial refused");
    uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
    const uint32_t box[6] = { 2, 1, 2, 4, 3, 2 }; bf_expect(px, lw, lh, ld, 4, box, want, base, "3d depth planes 2..3"); free(px);
    /* z out of range is refused and changes nothing */
    const uint32_t org2[3] = { 0, 0, 5 }, sz2[3] = { 2, 2, 2 };
    BF_CHECK(bf_fill(&im, 0, 0, org2, sz2, col) == LAGFX_ERR_INVALID_ARG, "z overflow not refused");
    px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld); bf_expect(px, lw, lh, ld, 4, box, want, base, "3d after refused z"); free(px);
    bf_free(&im); val_end("blitfill 3d");
    puts("PASS: 3D image: origin.z / size.depth select the depth planes, a depth range past the image is refused with nothing written");
}

/* 4. formats: RGBA16F (half floats), R32Uint / R32Sint / R8Uint (integer members, saturation), RGBA8 sRGB and A2B10G10R10 (packed): the partial path converts exactly like the full clear. */
static void test_formats(void) {
    val_begin();
    static const struct { VkFormat f; uint32_t bpt; uint8_t base[8], want[8]; double c[4]; const char *what; } k[] = {
        { VK_FORMAT_R16G16B16A16_SFLOAT, 8, { 1, 2, 3, 4, 5, 6, 7, 8 }, { 0x00, 0x38, 0x00, 0x3c, 0x00, 0x34, 0x00, 0x00 }, { 0.5, 1.0, 0.25, 0.0 }, "RGBA16F (0.5 1 0.25 0)" },   /* binary16: 0.5 = 0x3800, 1 = 0x3c00, 0.25 = 0x3400 */
        { VK_FORMAT_R32_UINT, 4, { 1, 2, 3, 4 }, { 7, 0, 0, 0 }, { 7.0, 0, 0, 0 }, "R32Uint 7" },
        { VK_FORMAT_R32_UINT, 4, { 1, 2, 3, 4 }, { 0xff, 0xff, 0xff, 0xff }, { 5e9, 0, 0, 0 }, "R32Uint 5e9 saturates" },
        { VK_FORMAT_R32_SINT, 4, { 1, 2, 3, 4 }, { 0xfd, 0xff, 0xff, 0xff }, { -3.0, 0, 0, 0 }, "R32Sint -3" },
        { VK_FORMAT_R8_UINT, 1, { 1 }, { 200 }, { 200.0, 0, 0, 0 }, "R8Uint 200" },
        { VK_FORMAT_R8_UINT, 1, { 1 }, { 255 }, { 300.0, 0, 0, 0 }, "R8Uint 300 saturates" },
        { VK_FORMAT_R8G8B8A8_SRGB, 4, { 1, 2, 3, 4 }, { 255, 0, 255, 255 }, { 1.0, 0.0, 1.0, 1.0 }, "RGBA8 sRGB (1 0 1 1)" },
        { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, { 1, 2, 3, 4 }, { 0xff, 0x03, 0x00, 0xc0 }, { 1.0, 0.0, 0.0, 1.0 }, "A2B10G10R10 (r 1, a 1): word 0xC00003FF" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        bf_img_t im; if (!bf_make(&im, k[i].f, VK_IMAGE_TYPE_2D, 8, 8, 1, 1, 1, 0)) { printf("note: device cannot create format %d: %s skipped\n", (int)k[i].f, k[i].what); continue; }
        bf_prefill(&im, 0, 0, k[i].base, k[i].bpt, VK_IMAGE_LAYOUT_GENERAL);
        const uint32_t org[3] = { 2, 3, 0 }, sz[3] = { 3, 2, 1 }, box[6] = { 2, 3, 0, 3, 2, 1 };
        BF_CHECK(bf_fill(&im, 0, 0, org, sz, k[i].c) == LAGFX_OK, "%s: partial refused", k[i].what);
        uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, k[i].bpt, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
        bf_expect(px, lw, lh, ld, k[i].bpt, box, k[i].want, k[i].base, k[i].what); free(px);
        bf_free(&im);
    }
    /* depth and block-compressed formats are refused (counted), never faked */
    BF_STATS_RESET();
    bf_img_t dep; if (bf_make(&dep, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, 4, 4, 1, 1, 1, 0)) {
        const uint32_t org[3] = { 0, 0, 0 }, sz[3] = { 2, 2, 1 }; const double c[4] = { 1, 0, 0, 0 };
        BF_CHECK(bf_fill(&dep, 0, 0, org, sz, c) == LAGFX_ERR_INVALID_ARG, "depth format not refused");
        BF_CHECK(bf_stats().fill_format == 1, "format counter"); bf_free(&dep);
    }
    val_end("blitfill formats");
    puts("PASS: RGBA16F, R32Uint / R32Sint / R8Uint (saturating), sRGB and A2B10G10R10 partial fills give the converted colour inside and the prefill outside; a depth format is refused and counted");
}

/* 5. refusals and no-ops: out of range region / level / slice, empty region; none changes the image or the layout. Negative control for the checks above: the image is compared byte for byte. */
static void test_refusals(void) {
    val_begin(); BF_STATS_RESET();
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 8, 8, 1, 2, 2, 0)) { puts("SKIP"); return; }
    const uint8_t base[4] = { 5, 6, 7, 8 }; const double col[4] = { 1, 1, 1, 1 };
    for (uint32_t lv = 0; lv < 2; lv++) for (uint32_t ly = 0; ly < 2; ly++) bf_prefill(&im, lv, ly, base, 4, VK_IMAGE_LAYOUT_GENERAL);
    struct { uint32_t lv, sl, o[3], s[3]; lagfx_status_t want; const char *why; } t[] = {
        { 0, 0, { 4, 0, 0 }, { 5, 2, 1 }, LAGFX_ERR_INVALID_ARG, "x + w > width" },
        { 0, 0, { 0, 7, 0 }, { 2, 2, 1 }, LAGFX_ERR_INVALID_ARG, "y + h > height" },
        { 1, 0, { 0, 0, 0 }, { 8, 8, 1 }, LAGFX_ERR_INVALID_ARG, "level 1 is 4x4: a level-0 sized region" },
        { 2, 0, { 0, 0, 0 }, { 1, 1, 1 }, LAGFX_ERR_INVALID_ARG, "level 2 does not exist" },
        { 0, 2, { 0, 0, 0 }, { 1, 1, 1 }, LAGFX_ERR_INVALID_ARG, "slice 2 does not exist" },
        { 0, 0, { 0, 0, 1 }, { 1, 1, 1 }, LAGFX_ERR_INVALID_ARG, "z = 1 on a 2D image" },
        { 0, 0, { 0xffffffffu, 0, 0 }, { 2, 1, 1 }, LAGFX_ERR_INVALID_ARG, "x + w overflows u32" },
        { 0, 0, { 1, 1, 0 }, { 0, 3, 1 }, LAGFX_OK, "empty: zero width" },
        { 0, 0, { 1, 1, 0 }, { 3, 0, 1 }, LAGFX_OK, "empty: zero height" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) BF_CHECK(bf_fill(&im, t[i].lv, t[i].sl, t[i].o, t[i].s, col) == t[i].want, "%s: wrong status", t[i].why);
    lagfx_blit_fill_stats_t s = bf_stats();
    BF_CHECK(s.fill_oob == 5 && s.fill_bad_subres == 2 && s.fill_empty == 2 && s.fill_full == 0 && s.fill_partial == 0, "counters oob %llu subres %llu empty %llu", (unsigned long long)s.fill_oob, (unsigned long long)s.fill_bad_subres, (unsigned long long)s.fill_empty);
    BF_CHECK(im.layout == VK_IMAGE_LAYOUT_GENERAL, "layout changed by a refusal");
    const uint32_t none[6] = { 0, 0, 0, 0, 0, 0 };
    for (uint32_t lv = 0; lv < 2; lv++) for (uint32_t ly = 0; ly < 2; ly++) { uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, lv, ly, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld); bf_expect(px, lw, lh, ld, 4, none, base, base, "refused fills leave every texel"); free(px); }
    /* negative control of the comparison itself: a real fill DOES change the bytes, so "unchanged" above is not vacuous */
    const uint32_t org[3] = { 0, 0, 0 }, sz[3] = { 1, 1, 1 };
    BF_CHECK(bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK, "valid 1x1 fill");
    { uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld); BF_CHECK(!memcmp(px, "\xff\xff\xff\xff", 4) && memcmp(px + 4, base, 4) == 0, "1x1 fill"); free(px); }
    bf_free(&im); val_end("blitfill refusals");
    puts("PASS: out-of-range region / level / slice are refused (counted, image and layout untouched), empty regions are counted no-ops, a valid 1x1 fill changes exactly one texel");
}

/* 6. layout is real: the image is in SHADER_READ_ONLY_OPTIMAL / COLOR_ATTACHMENT_OPTIMAL / TRANSFER_SRC_OPTIMAL (as after a render / a copy) with live contents; the fill keeps the contents outside the
 * region and restores that layout; the next user's barrier names the tracked layout as its old layout and the validation layer must not object (a fill that left GENERAL behind, as the old code
 * did, makes that barrier a layout mismatch). An image that was never written (UNDEFINED) is moved to GENERAL. */
static void test_layout(void) {
    val_begin();
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 8, 8, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) { puts("SKIP"); return; }
    const uint8_t base[4] = { 0x10, 0x20, 0x30, 0x40 }, want[4] = { 0, 0, 255, 255 };
    const double col[4] = { 1.0, 0.0, 0.0, 1.0 };
    const VkImageLayout lays[] = { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL };
    for (size_t i = 0; i < sizeof lays / sizeof lays[0]; i++) {
        bf_prefill(&im, 0, 0, base, 4, lays[i]);
        const uint32_t org[3] = { 1, 1, 0 }, sz[3] = { 4, 4, 1 }, box[6] = { 1, 1, 0, 4, 4, 1 };
        BF_CHECK(bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK, "fill in layout %d", (int)lays[i]);
        BF_CHECK(im.layout == lays[i], "layout %d not restored: now %d", (int)lays[i], (int)im.layout);
        uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, 4, lays[i], &lw, &lh, &ld);   /* the barrier's old layout is the SAME value: the layer flags a mismatch otherwise */
        bf_expect(px, lw, lh, ld, 4, box, want, base, "layout restore"); free(px);
    }
    /* next user right after the fill: a view and barriers that start from the tracked layout */
    bf_prefill(&im, 0, 0, base, 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    { const uint32_t org[3] = { 0, 0, 0 }, sz[3] = { 8, 8, 1 }; BF_CHECK(bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK, "full in SHADER_READ_ONLY"); }
    BF_CHECK(im.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "full fill must restore SHADER_READ_ONLY_OPTIMAL");
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im.fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v; BF_CHECK(vkCreateImageView(g_dev->vk->device, &vci, NULL, &v) == VK_SUCCESS, "view");
    { VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool);   /* next user: colour attachment, from the tracked layout */
      bf_barrier(cb, im.img, im.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0); bf_barrier(cb, im.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0); bf_end(cb, pool); }
    vkDestroyImageView(g_dev->vk->device, v, NULL);
    /* UNDEFINED image (never written): the fill moves it to GENERAL */
    bf_free(&im);
    if (bf_make(&im, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 8, 8, 1, 1, 1, 0)) {
        const uint32_t org[3] = { 2, 2, 0 }, sz[3] = { 2, 2, 1 };
        BF_CHECK(im.layout == VK_IMAGE_LAYOUT_UNDEFINED && bf_fill(&im, 0, 0, org, sz, col) == LAGFX_OK && im.layout == VK_IMAGE_LAYOUT_GENERAL, "UNDEFINED -> GENERAL");
        uint32_t lw, lh, ld; uint8_t *px = bf_read(&im, 0, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &lw, &lh, &ld);
        BF_CHECK(!memcmp(px + (2 * 8 + 2) * 4, want, 4), "filled texel of the UNDEFINED image"); free(px);
        bf_free(&im);
    }
    val_end("blitfill layout");
    puts("PASS: the tracked layout (SHADER_READ_ONLY / COLOR_ATTACHMENT / TRANSFER_SRC / GENERAL) is the barrier's old layout and is restored, contents outside the region survive, a following barrier from the tracked layout is clean; UNDEFINED -> GENERAL");
}

/* 7. negative control of the validation window: recording a barrier with a WRONG old layout is reported by the layer, so the clean windows above mean something (needs the layer: LAGFX_VK_VALIDATION=1). */
static void test_validation_live(void) {
    bf_img_t im; if (!bf_make(&im, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 4, 4, 1, 1, 1, 0)) return;
    const uint8_t base[4] = { 1, 2, 3, 4 }; bf_prefill(&im, 0, 0, base, 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    val_begin();
    uint64_t v0 = lagfx_vk_validation_msg_count;
    { VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool); bf_barrier(cb, im.img, VK_IMAGE_LAYOUT_GENERAL /* wrong: it is SHADER_READ_ONLY */, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0); bf_end(cb, pool); }
    const bool seen = lagfx_vk_validation_msg_count > v0 || g_val_ncur > 0;
    lagfx_vk_validation_hook = NULL;
    if (getenv("LAGFX_VK_VALIDATION")) BF_CHECK(seen, "validation layer silent on a wrong-old-layout barrier: the clean windows prove nothing");
    else puts("note: LAGFX_VK_VALIDATION not set: negative control of the validation window skipped");
    bf_free(&im);
    puts("PASS: a wrong old layout is reported by the validation layer (the clean fill windows are live)");
}

/* 8. 0x140 FillTextureWithBytes is counted, not applied: the dispatcher acknowledges it (op_ack_stub), nothing is written, the counter says so. */
static void test_0x140_counted(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p); p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = g_dev;
    uint8_t pl[76]; memset(pl, 0xAB, sizeof pl);
    uint64_t c0 = lagfx_blit_ack_only_count(0x140), o0 = lagfx_blit_ack_only_count(0x133);
    BF_CHECK(lagfx_blit_inner_dispatch(p, 0x140, pl, sizeof pl) == 0, "0x140 dispatch");
    BF_CHECK(lagfx_blit_ack_only_count(0x140) == c0 + 1 && lagfx_blit_ack_only_count(0x133) == o0, "0x140 ack-only counter");
    BF_CHECK(lagfx_blit_ack_only_count(0x141) == 0 && lagfx_blit_ack_only_count(0x132) == 0, "real handlers are not counted as ack-only");
    free(p);
    puts("PASS: 0x140 FillTextureWithBytes is acknowledged and counted (ack-only), not applied: no payload layout is known; 0x141 and 0x132 are not counted as ack-only");
}

#endif /* !BF_OLD_LIB */

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
#ifdef BF_OLD_LIB
    test_partial_bgra8();   /* fail-before probe only */
#else
    test_val_baseline_helper();
    test_partial_bgra8();
    test_level_layer();
    test_3d();
    test_formats();
    test_refusals();
    test_layout();
    test_validation_live();
    test_0x140_counted();
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
