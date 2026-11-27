/* libapplegfx-vulkan — Vulkan multisample colour path (see msaa.h). SPDX-License-Identifier: MIT */
#include "msaa.h"
#include "instance.h"

#include <stdio.h>
#include <string.h>

lagfx_msaa_mode_t lagfx_msaa_mode(void) { return lagfx_msaa_compat_env() ? LAGFX_MSAA_MODE_COMPAT : LAGFX_MSAA_MODE_EXACT; }

VkSampleCountFlagBits lagfx_msaa_choose(VkSampleCountFlags supported, uint32_t requested, bool *clamped) {
    if (requested == 0) requested = 1;
    if (requested > 16) requested = 16;
    uint32_t want = 1;
    while ((want << 1) <= requested) want <<= 1;   /* largest power of two <= requested */
    VkSampleCountFlagBits got = VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t s = want; s >= 1; s >>= 1) if (supported & s) { got = (VkSampleCountFlagBits)s; break; }
    if (clamped) *clamped = ((uint32_t)got != requested);
    return got;
}

bool lagfx_msaa_store_map(uint32_t a, bool have_resolve_target, VkAttachmentStoreOp *store, bool *resolve) {
    *store = VK_ATTACHMENT_STORE_OP_STORE; *resolve = false;   /* safe default */
    switch (a) {
    case 0: *store = VK_ATTACHMENT_STORE_OP_DONT_CARE; return true;
    case 1: return true;
    case 2: if (!have_resolve_target) return false; *store = VK_ATTACHMENT_STORE_OP_DONT_CARE; *resolve = true; return true;   /* the multisampled content is not kept */
    case 3: if (!have_resolve_target) return false; *resolve = true; return true;
    default: return false;   /* 4 Unknown, 5 CustomSampleDepthStore, others */
    }
}

VkResolveModeFlagBits lagfx_msaa_resolve_mode(VkFormat fmt) {
    return lagfx_fmtcap_class(fmt) == 0 ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool make_image_a(struct lagfx_vk_state *vk, VkFormat fmt, uint32_t w, uint32_t h, VkSampleCountFlagBits s, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vk->device, &ci, NULL, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk->device, *img, &mr);
    uint32_t mt = mem_type(vk->phys_device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(vk->phys_device, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (mt == UINT32_MAX || vkAllocateMemory(vk->device, &ai, NULL, mem) != VK_SUCCESS) { vkDestroyImage(vk->device, *img, NULL); *img = VK_NULL_HANDLE; return false; }
    if (vkBindImageMemory(vk->device, *img, *mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, 1, 0, 1 } };
    if (view && vkCreateImageView(vk->device, &vi, NULL, view) != VK_SUCCESS) goto fail;
    return true;
fail:
    vkFreeMemory(vk->device, *mem, NULL); vkDestroyImage(vk->device, *img, NULL); *mem = VK_NULL_HANDLE; *img = VK_NULL_HANDLE;
    return false;
}

static bool make_image(struct lagfx_vk_state *vk, VkFormat fmt, uint32_t w, uint32_t h, VkSampleCountFlagBits s, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    return make_image_a(vk, fmt, w, h, s, usage, VK_IMAGE_ASPECT_COLOR_BIT, img, mem, view);
}

int lagfx_msaa_target_create(struct lagfx_vk_state *vk, const lagfx_fmtcap_backend_t *be, VkFormat fmt, uint32_t w, uint32_t h, uint32_t requested, bool want_resolve, lagfx_msaa_target_t *t) {
    memset(t, 0, sizeof *t);
    t->fmt = fmt, t->w = w, t->h = h, t->requested = requested ? requested : 1;
    /* what the format can do as a multisampled colour attachment */
    lagfx_fmtcap_usage_t u1;
    if (!lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &u1))
        return LAGFX_MSAA_NO_FORMAT;
    VkSampleCountFlags sc = lagfx_fmtcap_sample_counts(be, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    t->samples = lagfx_msaa_choose(sc | VK_SAMPLE_COUNT_1_BIT, t->requested, &t->clamped);
    /* the granted count must hold for this exact size too (maxExtent / the usage combination can differ per sample count) */
    while (t->samples != VK_SAMPLE_COUNT_1_BIT) {
        lagfx_fmtcap_usage_t um;
        if (lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, w, h, 1, 1, 1, t->samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &um)) break;
        sc &= ~(VkSampleCountFlags)t->samples;
        t->samples = lagfx_msaa_choose(sc | VK_SAMPLE_COUNT_1_BIT, t->requested, NULL);
    }
    if ((uint32_t)t->samples != t->requested) t->clamped = true;
    t->ms_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (t->samples == VK_SAMPLE_COUNT_1_BIT ? (u1.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) : 0);
    if (!make_image(vk, fmt, w, h, t->samples, t->ms_usage, &t->ms_image, &t->ms_mem, &t->ms_view)) return LAGFX_MSAA_VK_FAIL;
    if (want_resolve && t->samples != VK_SAMPLE_COUNT_1_BIT) {
        t->resolve_usage = u1.usage;   /* COLOR_ATTACHMENT + whatever of TRANSFER_SRC / SAMPLED the format offers */
        if (!make_image(vk, fmt, w, h, VK_SAMPLE_COUNT_1_BIT, t->resolve_usage, &t->resolve_image, &t->resolve_mem, &t->resolve_view)) { lagfx_msaa_target_destroy(vk, t); return LAGFX_MSAA_VK_FAIL; }
    }
    return LAGFX_MSAA_OK;
}

void lagfx_msaa_target_destroy(struct lagfx_vk_state *vk, lagfx_msaa_target_t *t) {
    if (t->ms_view) vkDestroyImageView(vk->device, t->ms_view, NULL);
    if (t->resolve_view) vkDestroyImageView(vk->device, t->resolve_view, NULL);
    if (t->ms_image) vkDestroyImage(vk->device, t->ms_image, NULL);
    if (t->resolve_image) vkDestroyImage(vk->device, t->resolve_image, NULL);
    if (t->ms_mem) vkFreeMemory(vk->device, t->ms_mem, NULL);
    if (t->resolve_mem) vkFreeMemory(vk->device, t->resolve_mem, NULL);
    memset(t, 0, sizeof *t);
}

bool lagfx_msaa_color_attachment(const lagfx_msaa_target_t *t, VkAttachmentLoadOp load, const VkClearColorValue *clear, uint32_t mtl_store_action, VkRenderingAttachmentInfo *out) {
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    out->imageView = t->ms_view, out->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, out->loadOp = load;
    if (clear) out->clearValue.color = *clear;
    bool resolve;
    const bool ok = lagfx_msaa_store_map(mtl_store_action, t->resolve_view != VK_NULL_HANDLE, &out->storeOp, &resolve);
    if (resolve) {
        out->resolveMode = lagfx_msaa_resolve_mode(t->fmt);
        out->resolveImageView = t->resolve_view, out->resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    return ok;
}

/* the usage mask EXACT mode asks for per count (lagfx_a64_msaa_plan_image): required attachment usage, SAMPLED optional (colour only) */
static VkSampleCountFlags exact_mask(const lagfx_fmtcap_backend_t *be, VkFormat fmt, bool depth, bool *attachable) {
    const VkImageUsageFlags req = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, opt = depth ? 0u : VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormatFeatureFlags want = depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    VkSampleCountFlags m = 0;
    *attachable = (lagfx_fmtcap_features(be, fmt) & want) != 0;
    if (!*attachable) return 0;
    for (uint32_t n = 1; n <= 16u; n <<= 1) {
        lagfx_fmtcap_usage_t u;
        if (lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, (VkSampleCountFlagBits)n, req, opt, &u)) m |= n;
    }
    return m;
}

void lagfx_msaa_host_caps(const lagfx_fmtcap_backend_t *be, uint32_t ceiling, lagfx_msaa_caps_t *out) {
    static const struct { VkFormat f; bool depth, gating; } tab[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, false, true }, { VK_FORMAT_R16G16B16A16_SFLOAT, false, true }, { VK_FORMAT_D32_SFLOAT, true, true }, { VK_FORMAT_D32_SFLOAT_S8_UINT, true, true },
        { VK_FORMAT_R8G8B8A8_UNORM, false, false }, { VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, false }, { VK_FORMAT_D16_UNORM, true, false }, { VK_FORMAT_D24_UNORM_S8_UINT, true, false } };
    memset(out, 0, sizeof *out);
    out->ceiling = ceiling < 1u ? 1u : ceiling;
    out->common = 0x1fu;   /* counts 1 .. 16, narrowed by every gating format below */
    bool colour_missing = false;
    for (size_t i = 0; i < sizeof tab / sizeof *tab && out->n < LAGFX_MSAA_CAPS_MAX_FMT; i++) {
        lagfx_msaa_fmt_caps_t *c = &out->f[out->n++];
        c->fmt = tab[i].f, c->depth = tab[i].depth, c->gating = tab[i].gating;
        c->mask = exact_mask(be, c->fmt, c->depth, &c->attachable);
        if (!c->gating) continue;
        if (!c->attachable) { if (!c->depth) colour_missing = true; continue; }   /* a depth format the device cannot attach is not used by the mirror either (create_image falls back / refuses) */
        out->common &= c->mask | VK_SAMPLE_COUNT_1_BIT;
    }
    if (colour_missing) out->common = VK_SAMPLE_COUNT_1_BIT;   /* no usable gating colour format: only the N = 1 gate stays open */
    uint32_t top = 1;
    for (uint32_t n = 2; n <= 16u && n <= out->ceiling; n <<= 1) if (out->common & n) top = n;
    out->advertised = top;
    for (uint32_t n = 2; n <= top; n <<= 1) if (!(out->common & n)) out->gaps |= n;
}

int lagfx_msaa_caps_line(const lagfx_msaa_caps_t *c, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "g7-gpu-msaa-caps: value=MSAASamples(max count, not a bitmask) advertised=%u ceiling=%u common=0x%x gaps=0x%x(%s)", c->advertised, c->ceiling, (unsigned)c->common, (unsigned)c->gaps,
                     c->gaps ? "counts the single value cannot express: refused exactly" : "none");
    for (uint32_t i = 0; i < c->n && n > 0 && (size_t)n < cap; i++)
        n += snprintf(buf + n, cap - (size_t)n, " [vk=%d %s %s mask=0x%x]", (int)c->f[i].fmt, c->f[i].depth ? "ds" : "colour", !c->f[i].attachable ? "unattachable" : c->f[i].gating ? "gating" : "info", (unsigned)c->f[i].mask);
    return n;
}

uint32_t lagfx_msaa_devinfo_max(const lagfx_fmtcap_backend_t *be, uint32_t advertised, bool *clamped) {
    if (advertised <= 1u) return advertised;
    lagfx_msaa_caps_t c;
    lagfx_msaa_host_caps(be, advertised, &c);
    if (c.advertised >= advertised) return advertised;
    if (clamped) *clamped = true;
    return c.advertised;
}

int lagfx_msaa_plan(const lagfx_fmtcap_backend_t *be, VkFormat fmt, uint32_t w, uint32_t h, uint32_t requested, VkImageUsageFlags required, VkImageUsageFlags optional_ms,
                    VkImageUsageFlags optional_1x, lagfx_msaa_plan_t *out) {
    return lagfx_msaa_plan_mode(be, fmt, w, h, requested, required, optional_ms, optional_1x, LAGFX_MSAA_MODE_COMPAT, out);
}

int lagfx_msaa_plan_mode(const lagfx_fmtcap_backend_t *be, VkFormat fmt, uint32_t w, uint32_t h, uint32_t requested, VkImageUsageFlags required, VkImageUsageFlags optional_ms,
                         VkImageUsageFlags optional_1x, lagfx_msaa_mode_t mode, lagfx_msaa_plan_t *out) {
    memset(out, 0, sizeof *out);
    out->requested = requested ? requested : 1u;
    if (mode == LAGFX_MSAA_MODE_EXACT && out->requested > 1u) {   /* exactly the requested count or nothing */
        lagfx_fmtcap_usage_t u;
        if (out->requested > 16u || (out->requested & (out->requested - 1u))) return LAGFX_MSAA_EXACT_REFUSED;   /* not a Vulkan / Metal count */
        if (!lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, w, h, 1, 1, 1, (VkSampleCountFlagBits)out->requested, required, optional_ms, &u)) return LAGFX_MSAA_EXACT_REFUSED;
        out->samples = (VkSampleCountFlagBits)out->requested, out->usage = u.usage, out->dropped = u.dropped;
        return LAGFX_MSAA_OK;
    }
    uint32_t want = out->requested > 16u ? 16u : out->requested;
    uint32_t s = 1;
    while ((s << 1) <= want) s <<= 1;   /* largest power of two <= request (Metal counts are 1, 2, 4, 8) */
    for (; s > 1u; s >>= 1) {
        lagfx_fmtcap_usage_t u;
        if (lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, w, h, 1, 1, 1, (VkSampleCountFlagBits)s, required, optional_ms, &u)) {
            out->samples = (VkSampleCountFlagBits)s, out->usage = u.usage, out->dropped = u.dropped;
            out->clamped = s != out->requested;
            return LAGFX_MSAA_OK;
        }
    }
    lagfx_fmtcap_usage_t u;
    if (!lagfx_fmtcap_image_usage(be, fmt, VK_IMAGE_TYPE_2D, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, required, optional_1x, &u)) return LAGFX_MSAA_NO_FORMAT;
    out->samples = VK_SAMPLE_COUNT_1_BIT, out->usage = u.usage, out->dropped = u.dropped;
    out->fallback_1x = out->requested > 1u;
    return LAGFX_MSAA_OK;
}

bool lagfx_msaa_image_create(struct lagfx_vk_state *vk, VkFormat fmt, uint32_t w, uint32_t h, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                             VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    *img = VK_NULL_HANDLE, *mem = VK_NULL_HANDLE;
    if (view) *view = VK_NULL_HANDLE;
    return make_image_a(vk, fmt, w, h, samples, usage, aspect, img, mem, view);
}

bool lagfx_msaa_cmd_resolve_image(VkCommandBuffer cb, VkFormat fmt, VkImage ms, VkImageLayout ms_layout, VkImage dst, VkImageLayout dst_layout, uint32_t w, uint32_t h) {
    (void)fmt;
    if (!cb || !ms || !dst || !w || !h) return false;
    VkImageResolve r = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { w, h, 1 } };
    vkCmdResolveImage(cb, ms, ms_layout, dst, dst_layout, 1, &r);
    return true;
}
