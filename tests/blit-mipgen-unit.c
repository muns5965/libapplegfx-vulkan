/* Standalone mip-chain recorder (src/translate/mip_gen.c) on lavapipe + validation layer: a generated RGBA8 chain over 3 array layers (non-square, so a dimension clamps to 1)
 * against a CPU box-filter reference, layout restored, and the refusals (depth, multisample, compressed, usage, features, type, args) each with its own counter, recording nothing.
 * Reference: level i is computed on the CPU from the GPU's own level i-1 bytes (2x2 average, clamped at a 1-wide dimension), so rounding does not accumulate; tolerance +-1 of 255
 * [I: linear filtering of exactly 2x minification samples at texel-pair centres, so it is the 2-tap average per axis; the spec leaves filter arithmetic precision implementation defined].
 * NOT connected to the ARM64 dispatcher (tag 307 payload unknown); not device-verified.
 * SPDX-License-Identifier: MIT */
#define LAGFX_COMPAT_NO_MAIN 1
#include "arm64-render-compat-unit.c"

#ifdef LAGFX_HAVE_VULKAN
#include "translate/mip_gen.h"

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

static uint32_t lv(uint32_t d, uint32_t l) { uint32_t v = d >> l; return v ? v : 1; }
static lagfx_mipgen_image_t info_of(const bf_img_t *o) {
    lagfx_mipgen_image_t i = { o->fmt, o->w, o->h, o->mips, o->layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    return i;
}
static void run_record(bf_img_t *o, const lagfx_mipgen_image_t *in, lagfx_mipgen_status_t want) {
    VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool);
    lagfx_mipgen_status_t s = lagfx_mipgen_record(cb, g_dev->vk->phys_device, o->img, in, VK_IMAGE_LAYOUT_GENERAL);
    bf_end(cb, pool);
    BF_CHECK(s == want, "status %s, wanted %s", lagfx_mipgen_status_name(s), lagfx_mipgen_status_name(want));
}

static void test_chain(void) {
    uint64_t v0 = lagfx_vk_validation_msg_count;
    bf_img_t o; BF_CHECK(bf_make(&o, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 16, 4, 1, 5, 3, 0), "image");
    uint32_t seed = 12345;
    for (uint32_t layer = 0; layer < 3; layer++) {
        uint8_t *px = malloc(16 * 4 * 4);
        for (uint32_t i = 0; i < 16 * 4 * 4; i++) seed = seed * 1664525u + 1013904223u, px[i] = (uint8_t)(seed >> 24);
        bf_buf_t b = bf_buffer(16 * 4 * 4); memcpy(b.p, px, 16 * 4 * 4); free(px);
        VkCommandPool pool; VkCommandBuffer cb = bf_begin(&pool);
        /* every level of the layer starts UNDEFINED -> GENERAL (levels > 0 hold zeros / garbage the generator must overwrite) */
        for (uint32_t l = 0; l < 5; l++) bf_barrier(cb, o.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, l, layer);
        VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 }, .imageExtent = { 16, 4, 1 } };
        vkCmdCopyBufferToImage(cb, b.b, o.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        for (uint32_t l = 0; l < 5; l++) bf_barrier(cb, o.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, l, layer);
        bf_end(cb, pool); bf_buf_free(&b);
    }
    lagfx_mipgen_stats_reset();
    lagfx_mipgen_image_t in = info_of(&o);
    run_record(&o, &in, LAGFX_MIPGEN_OK);
    /* 16x4 -> 8x2 -> 4x1 -> 2x1 -> 1x1: height clamps to 1 from level 2 */
    unsigned maxdiff = 0, texels = 0;
    for (uint32_t layer = 0; layer < 3; layer++) {
        uint32_t w, h, d; uint8_t *prev = bf_read(&o, 0, layer, 4, VK_IMAGE_LAYOUT_GENERAL, &w, &h, &d);
        uint32_t pw = w, ph = h;
        for (uint32_t l = 1; l < 5; l++) {
            uint8_t *cur = bf_read(&o, l, layer, 4, VK_IMAGE_LAYOUT_GENERAL, &w, &h, &d);
            BF_CHECK(w == lv(16, l) && h == lv(4, l), "level %u extent %ux%u", l, w, h);
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t ch = 0; ch < 4; ch++) {
                uint32_t x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1, y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
                double e = (prev[(y0 * pw + x0) * 4 + ch] + prev[(y0 * pw + x1) * 4 + ch] + prev[(y1 * pw + x0) * 4 + ch] + prev[(y1 * pw + x1) * 4 + ch]) / 4.0;
                int diff = (int)cur[(y * w + x) * 4 + ch] - (int)(e + 0.5); if (diff < 0) diff = -diff;
                if ((unsigned)diff > maxdiff) maxdiff = (unsigned)diff;
                texels++;
            }
            free(prev); prev = cur; pw = w, ph = h;
        }
        free(prev);
    }
    printf("mipgen chain: %u channel samples compared, max |gpu - cpu box| = %u\n", texels, maxdiff);
    BF_CHECK(maxdiff <= 1, "max diff %u exceeds the +-1 tolerance", maxdiff);
    /* a real chain was produced: level 1 differs from the zero-initialised content for random data (negative control against a vacuous pass) */
    uint32_t w, h, d; uint8_t *l1 = bf_read(&o, 1, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &w, &h, &d); bool nz = false; for (uint32_t i = 0; i < w * h * 4; i++) if (l1[i]) nz = true; free(l1);
    BF_CHECK(nz, "level 1 is all zero");
    uint64_t st[LAGFX_MIPGEN_REASON_COUNT]; lagfx_mipgen_stats_get(st);
    BF_CHECK(st[LAGFX_MIPGEN_OK] == 1, "ok counter");
    BF_CHECK(lagfx_vk_validation_msg_count == v0, "validation messages: %llu new", (unsigned long long)(lagfx_vk_validation_msg_count - v0));
    bf_free(&o);
}

static void test_refusals(void) {
    uint64_t v0 = lagfx_vk_validation_msg_count;
    bf_img_t o; BF_CHECK(bf_make(&o, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 8, 8, 1, 4, 1, 0), "image");
    uint8_t one[4] = { 9, 9, 9, 9 };
    for (uint32_t l = 0; l < 4; l++) bf_prefill(&o, l, 0, one, 4, VK_IMAGE_LAYOUT_GENERAL);
    lagfx_mipgen_stats_reset();
    lagfx_mipgen_image_t base = info_of(&o), i;
    i = base; i.format = VK_FORMAT_D32_SFLOAT;                      run_record(&o, &i, LAGFX_MIPGEN_REFUSED_DEPTH_STENCIL);
    i = base; i.format = VK_FORMAT_D24_UNORM_S8_UINT;               run_record(&o, &i, LAGFX_MIPGEN_REFUSED_DEPTH_STENCIL);
    i = base; i.samples = VK_SAMPLE_COUNT_4_BIT;                    run_record(&o, &i, LAGFX_MIPGEN_REFUSED_MULTISAMPLE);
    i = base; i.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;            run_record(&o, &i, LAGFX_MIPGEN_REFUSED_COMPRESSED);
    i = base; i.format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;            run_record(&o, &i, LAGFX_MIPGEN_REFUSED_COMPRESSED);
    i = base; i.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;            run_record(&o, &i, LAGFX_MIPGEN_REFUSED_USAGE);
    i = base; i.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;            run_record(&o, &i, LAGFX_MIPGEN_REFUSED_USAGE);
    i = base; i.format = VK_FORMAT_R8G8B8A8_UINT;                   run_record(&o, &i, LAGFX_MIPGEN_REFUSED_FEATURES);   /* integer: no linear filter feature */
    i = base; i.type = VK_IMAGE_TYPE_3D;                            run_record(&o, &i, LAGFX_MIPGEN_REFUSED_TYPE);
    i = base; i.mip_levels = 5;                                     run_record(&o, &i, LAGFX_MIPGEN_REFUSED_ARGS);       /* 8x8 has 4 levels */
    i = base; i.mip_levels = 1;                                     run_record(&o, &i, LAGFX_MIPGEN_NOTHING_TO_DO);
    uint64_t st[LAGFX_MIPGEN_REASON_COUNT]; lagfx_mipgen_stats_get(st);
    BF_CHECK(st[LAGFX_MIPGEN_REFUSED_DEPTH_STENCIL] == 2 && st[LAGFX_MIPGEN_REFUSED_MULTISAMPLE] == 1 && st[LAGFX_MIPGEN_REFUSED_COMPRESSED] == 2 && st[LAGFX_MIPGEN_REFUSED_USAGE] == 2 &&
             st[LAGFX_MIPGEN_REFUSED_FEATURES] == 1 && st[LAGFX_MIPGEN_REFUSED_TYPE] == 1 && st[LAGFX_MIPGEN_REFUSED_ARGS] == 1 && st[LAGFX_MIPGEN_NOTHING_TO_DO] == 1 && st[LAGFX_MIPGEN_OK] == 0,
             "each refusal counted under its own reason, none ok");
    /* nothing was recorded: every level still holds the prefill bytes (a generator that ran would have rewritten levels 1..3 from level 0 = same value here, so use a different level 0) */
    bf_free(&o);
    BF_CHECK(bf_make(&o, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 8, 8, 1, 4, 1, 0), "image2");
    uint8_t l0[4] = { 200, 200, 200, 200 }, l1[4] = { 7, 7, 7, 7 };
    bf_prefill(&o, 0, 0, l0, 4, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t l = 1; l < 4; l++) bf_prefill(&o, l, 0, l1, 4, VK_IMAGE_LAYOUT_GENERAL);
    base = info_of(&o); i = base; i.samples = VK_SAMPLE_COUNT_2_BIT; run_record(&o, &i, LAGFX_MIPGEN_REFUSED_MULTISAMPLE);
    i = base; i.usage = 0; run_record(&o, &i, LAGFX_MIPGEN_REFUSED_USAGE);
    uint32_t w, h, d; uint8_t *p = bf_read(&o, 3, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &w, &h, &d);
    BF_CHECK(p[0] == 7 && p[3] == 7, "a refused request changed level 3 (%u)", p[0]); free(p);
    run_record(&o, &base, LAGFX_MIPGEN_OK);
    p = bf_read(&o, 3, 0, 4, VK_IMAGE_LAYOUT_GENERAL, &w, &h, &d);
    BF_CHECK(p[0] == 200 && p[1] == 200 && p[3] == 200, "the accepted request did not propagate level 0 down to level 3 (%u)", p[0]); free(p);
    BF_CHECK(lagfx_vk_validation_msg_count == v0, "validation messages: %llu new", (unsigned long long)(lagfx_vk_validation_msg_count - v0));
    bf_free(&o);
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_chain();
    test_refusals();
    puts("ALL PASS");
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
