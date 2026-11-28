/* SPDX-License-Identifier: MIT */
/* '422f' bi-planar surfaces as two device images. See planar422.h. */
#include "planar422.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(char *why, size_t cap, const char *s) { if (why && cap) snprintf(why, cap, "%s", s); }

static bool mul_ovf(uint64_t a, uint64_t b, uint64_t *r) { return __builtin_mul_overflow(a, b, r); }
static bool add_ovf(uint64_t a, uint64_t b, uint64_t *r) { return __builtin_add_overflow(a, b, r); }

/* last byte + 1 of a plane's payload: off + pitch * (rows - 1) + row_bytes; false on overflow */
static bool plane_end(const lagfx_p422_layout_t *l, uint32_t plane, uint64_t *end) {
    const uint64_t off = plane == LAGFX_P422_Y ? l->y_off : l->c_off, pitch = plane == LAGFX_P422_Y ? l->y_pitch : l->c_pitch;
    uint64_t t;
    if (mul_ovf(pitch, (uint64_t)l->height - 1u, &t) || add_ovf(off, t, &t) || add_ovf(t, lagfx_p422_row_bytes(l, plane), end)) return false;
    return true;
}

int lagfx_p422_layout_check(const lagfx_p422_layout_t *l, char *why, size_t why_cap) {
    if (!l || !l->width || !l->height || l->width > 8192u || l->height > 8192u) { say(why, why_cap, "size outside 1..8192"); return LAGFX_P422_BAD_SIZE; }
    if (l->y_pitch < lagfx_p422_row_bytes(l, LAGFX_P422_Y) || l->c_pitch < lagfx_p422_row_bytes(l, LAGFX_P422_CBCR)) { say(why, why_cap, "pitch smaller than a row's payload"); return LAGFX_P422_BAD_PITCH; }
    uint64_t ye, ce;
    if (!plane_end(l, LAGFX_P422_Y, &ye) || !plane_end(l, LAGFX_P422_CBCR, &ce) || ye > l->alloc || ce > l->alloc) { say(why, why_cap, "a plane lies outside the allocation"); return LAGFX_P422_BAD_BOUNDS; }
    if (l->y_off < ce && l->c_off < ye) { say(why, why_cap, "the planes overlap"); return LAGFX_P422_BAD_OVERLAP; }
    return LAGFX_P422_OK;
}

bool lagfx_p422_xfer_plane(lagfx_p422_rw_fn rw, void *ctx, const lagfx_p422_layout_t *l, uint32_t plane, uint8_t *tight, bool to_guest) {
    if (!rw || !l || !tight || plane > LAGFX_P422_CBCR || lagfx_p422_layout_check(l, NULL, 0) != LAGFX_P422_OK) return false;
    const uint64_t off = plane == LAGFX_P422_Y ? l->y_off : l->c_off, pitch = plane == LAGFX_P422_Y ? l->y_pitch : l->c_pitch, rb = lagfx_p422_row_bytes(l, plane);
    for (uint32_t y = 0; y < l->height; y++)
        if (!rw(ctx, off + (uint64_t)y * pitch, rb, tight + (size_t)y * rb, to_guest)) return false;
    return true;
}

#ifdef LAGFX_HAVE_VULKAN
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkBuffer stage; VkDeviceMemory smem; uint8_t *map; uint64_t bytes; bool storage, inited; } plane_t;
struct lagfx_p422 {
    VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    lagfx_p422_layout_t l; plane_t pl[2]; uint32_t dirty;
};
static const VkFormat k_fmt[2] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
static void plane_destroy(lagfx_p422_t *p, plane_t *pl) {
    if (pl->map) vkUnmapMemory(p->dev, pl->smem);
    if (pl->view) vkDestroyImageView(p->dev, pl->view, NULL);
    if (pl->img) vkDestroyImage(p->dev, pl->img, NULL);
    if (pl->mem) vkFreeMemory(p->dev, pl->mem, NULL);
    if (pl->stage) vkDestroyBuffer(p->dev, pl->stage, NULL);
    if (pl->smem) vkFreeMemory(p->dev, pl->smem, NULL);
    memset(pl, 0, sizeof *pl);
}
static bool plane_make(lagfx_p422_t *p, uint32_t i, const lagfx_p422_layout_t *l, char *why, size_t cap) {
    plane_t *pl = &p->pl[i];
    const uint32_t w = i == LAGFX_P422_Y ? l->width : lagfx_p422_chroma_width(l->width), h = l->height;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(p->pd, k_fmt[i], &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & need) != need) { say(why, cap, "plane format cannot be sampled and transferred on this device"); return false; }
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    pl->storage = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    if (pl->storage) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = k_fmt[i], .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(p->dev, &ci, NULL, &pl->img) != VK_SUCCESS) { pl->img = VK_NULL_HANDLE; say(why, cap, "vkCreateImage failed"); return false; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(p->dev, pl->img, &mr);
    uint32_t mt = mem_type(p->pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt == ~0u) mt = mem_type(p->pd, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (mt == ~0u || vkAllocateMemory(p->dev, &ai, NULL, &pl->mem) != VK_SUCCESS) { pl->mem = VK_NULL_HANDLE; say(why, cap, "image memory allocation failed"); return false; }
    if (vkBindImageMemory(p->dev, pl->img, pl->mem, 0) != VK_SUCCESS) { say(why, cap, "bind failed"); return false; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = pl->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = k_fmt[i],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(p->dev, &vi, NULL, &pl->view) != VK_SUCCESS) { pl->view = VK_NULL_HANDLE; say(why, cap, "view failed"); return false; }
    pl->bytes = (uint64_t)w * h * (i == LAGFX_P422_Y ? 1u : 2u);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = pl->bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (vkCreateBuffer(p->dev, &bi, NULL, &pl->stage) != VK_SUCCESS) { pl->stage = VK_NULL_HANDLE; say(why, cap, "staging buffer failed"); return false; }
    vkGetBufferMemoryRequirements(p->dev, pl->stage, &mr);
    VkMemoryAllocateInfo sa = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(p->pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    void *m = NULL;
    if (sa.memoryTypeIndex == ~0u || vkAllocateMemory(p->dev, &sa, NULL, &pl->smem) != VK_SUCCESS) { pl->smem = VK_NULL_HANDLE; say(why, cap, "staging memory failed"); return false; }
    if (vkBindBufferMemory(p->dev, pl->stage, pl->smem, 0) != VK_SUCCESS || vkMapMemory(p->dev, pl->smem, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) { say(why, cap, "staging map failed"); return false; }
    pl->map = m;
    return true;
}

/* one-shot: record, submit, wait */
typedef void (*rec_fn)(VkCommandBuffer cb, void *u);
static bool one_shot(lagfx_p422_t *p, rec_fn rec, void *u) {
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = p->qf };
    VkCommandPool pool; if (vkCreateCommandPool(p->dev, &pi, NULL, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; bool ok = false;
    if (vkAllocateCommandBuffers(p->dev, &ai, &cb) == VK_SUCCESS) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
            rec(cb, u);
            if (vkEndCommandBuffer(cb) == VK_SUCCESS) {
                VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
                ok = vkQueueSubmit(p->q, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(p->q) == VK_SUCCESS;
            }
        }
    }
    vkDestroyCommandPool(p->dev, pool, NULL);
    return ok;
}
static void to_general(VkCommandBuffer cb, VkImage img, VkImageLayout from) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier m = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m, 0, NULL, 0, NULL);
}
typedef struct { lagfx_p422_t *p; uint32_t plane; bool up; const float *v; } xfer_job_t;
static VkExtent3D plane_extent(const lagfx_p422_t *p, uint32_t plane) { return (VkExtent3D){ plane == LAGFX_P422_Y ? p->l.width : lagfx_p422_chroma_width(p->l.width), p->l.height, 1 }; }
static void rec_xfer(VkCommandBuffer cb, void *u) {
    xfer_job_t *j = u; plane_t *pl = &j->p->pl[j->plane];
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = plane_extent(j->p, j->plane) };
    to_general(cb, pl->img, pl->inited ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    if (j->up) vkCmdCopyBufferToImage(cb, pl->stage, pl->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    else vkCmdCopyImageToBuffer(cb, pl->img, VK_IMAGE_LAYOUT_GENERAL, pl->stage, 1, &r);
    mem_barrier(cb);
}
static void rec_fill(VkCommandBuffer cb, void *u) {
    xfer_job_t *j = u; plane_t *pl = &j->p->pl[j->plane];
    VkClearColorValue c = { .float32 = { j->v[0], j->plane == LAGFX_P422_CBCR ? j->v[1] : 0.f, 0.f, 1.f } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    to_general(cb, pl->img, pl->inited ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    vkCmdClearColorImage(cb, pl->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
    mem_barrier(cb);
}

static bool make_all(lagfx_p422_t *p, const lagfx_p422_layout_t *l, char *why, size_t cap) {
    for (uint32_t i = 0; i < 2; i++) plane_destroy(p, &p->pl[i]);
    p->l = *l; p->dirty = 0;
    for (uint32_t i = 0; i < 2; i++) if (!plane_make(p, i, l, why, cap)) { for (uint32_t k = 0; k < 2; k++) plane_destroy(p, &p->pl[k]); return false; }
    return true;
}

lagfx_p422_t *lagfx_p422_create(VkPhysicalDevice pd, VkDevice dev, VkQueue q, uint32_t qf, const lagfx_p422_layout_t *l, char *why, size_t cap) {
    if (why && cap) why[0] = 0;
    if (!pd || !dev || !q || !l || lagfx_p422_layout_check(l, why, cap) != LAGFX_P422_OK) return NULL;
    lagfx_p422_t *p = calloc(1, sizeof *p);
    if (!p) { say(why, cap, "out of memory"); return NULL; }
    p->pd = pd, p->dev = dev, p->q = q, p->qf = qf;
    if (!make_all(p, l, why, cap)) { free(p); return NULL; }
    return p;
}
void lagfx_p422_free(lagfx_p422_t *p) { if (!p) return; for (uint32_t i = 0; i < 2; i++) plane_destroy(p, &p->pl[i]); free(p); }
VkImage lagfx_p422_image(const lagfx_p422_t *p, uint32_t plane) { return p && plane < 2 ? p->pl[plane].img : VK_NULL_HANDLE; }
VkImageView lagfx_p422_view(const lagfx_p422_t *p, uint32_t plane) { return p && plane < 2 ? p->pl[plane].view : VK_NULL_HANDLE; }
bool lagfx_p422_has_storage(const lagfx_p422_t *p, uint32_t plane) { return p && plane < 2 && p->pl[plane].storage; }
const lagfx_p422_layout_t *lagfx_p422_layout(const lagfx_p422_t *p) { return p ? &p->l : NULL; }
uint32_t lagfx_p422_dirty(const lagfx_p422_t *p) { return p ? p->dirty : 0; }
void lagfx_p422_mark_gpu_written(lagfx_p422_t *p, uint32_t mask) { if (p) p->dirty |= mask & LAGFX_P422_PLANES_ALL; }

bool lagfx_p422_upload(lagfx_p422_t *p, lagfx_p422_rw_fn rw, void *ctx, uint32_t mask) {
    if (!p || !rw) return false;
    for (uint32_t i = 0; i < 2; i++) {
        if (!(mask & (1u << i))) continue;
        if (!lagfx_p422_xfer_plane(rw, ctx, &p->l, i, p->pl[i].map, false)) return false;
        xfer_job_t j = { p, i, true, NULL };
        if (!one_shot(p, rec_xfer, &j)) return false;
        p->pl[i].inited = true;
        p->dirty &= ~(1u << i);
    }
    return true;
}
uint32_t lagfx_p422_download(lagfx_p422_t *p, lagfx_p422_rw_fn rw, void *ctx, uint32_t mask, bool force) {
    uint32_t done = 0;
    if (!p || !rw) return 0;
    for (uint32_t i = 0; i < 2; i++) {
        if (!(mask & (1u << i)) || (!force && !(p->dirty & (1u << i)))) continue;
        xfer_job_t j = { p, i, false, NULL };
        if (!one_shot(p, rec_xfer, &j)) continue;
        p->pl[i].inited = true;
        if (!lagfx_p422_xfer_plane(rw, ctx, &p->l, i, p->pl[i].map, true)) continue;
        p->dirty &= ~(1u << i); done |= 1u << i;
    }
    return done;
}
bool lagfx_p422_fill(lagfx_p422_t *p, uint32_t plane, const float v[2]) {
    if (!p || plane > 1 || !v) return false;
    xfer_job_t j = { p, plane, true, v };
    if (!one_shot(p, rec_fill, &j)) return false;
    p->pl[plane].inited = true;
    p->dirty |= 1u << plane;
    return true;
}
bool lagfx_p422_remap(lagfx_p422_t *p, const lagfx_p422_layout_t *l, char *why, size_t cap) {
    if (why && cap) why[0] = 0;
    if (!p || !l || lagfx_p422_layout_check(l, why, cap) != LAGFX_P422_OK) return false;
    if (l->width == p->l.width && l->height == p->l.height) { p->l = *l; return true; }
    lagfx_p422_t keep = *p;   /* a failed re-make must leave the old state: build the new planes first */
    lagfx_p422_t fresh = { .pd = p->pd, .dev = p->dev, .q = p->q, .qf = p->qf };
    fresh.l = *l;
    for (uint32_t i = 0; i < 2; i++) if (!plane_make(&fresh, i, l, why, cap)) { for (uint32_t k = 0; k < 2; k++) plane_destroy(&fresh, &fresh.pl[k]); *p = keep; return false; }
    for (uint32_t i = 0; i < 2; i++) plane_destroy(&keep, &keep.pl[i]);
    *p = fresh;
    return true;
}
#endif /* LAGFX_HAVE_VULKAN */
