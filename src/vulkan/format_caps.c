/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — format / usage / blend / sample-count capabilities. See format_caps.h. */
#include "vulkan/format_caps.h"

#ifdef LAGFX_HAVE_VULKAN
#include <stdio.h>
#include <string.h>

/* ---- production backend ---- */
static void dev_fp2(void *ctx, VkFormat f, VkFormatProperties2 *out) { vkGetPhysicalDeviceFormatProperties2((VkPhysicalDevice)ctx, f, out); }
static VkResult dev_ifp2(void *ctx, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *out) {
    return vkGetPhysicalDeviceImageFormatProperties2((VkPhysicalDevice)ctx, in, out);
}

const lagfx_fmtcap_backend_t *lagfx_fmtcap_test_backend;

const lagfx_fmtcap_backend_t *lagfx_fmtcap_device_backend(VkPhysicalDevice pd, lagfx_fmtcap_backend_t *storage) {
    if (!pd || !storage) return NULL;
    storage->ctx = (void *)pd, storage->format_props2 = dev_fp2, storage->image_format_props2 = dev_ifp2;
    return storage;
}

const lagfx_fmtcap_backend_t *lagfx_fmtcap_active(VkPhysicalDevice pd, lagfx_fmtcap_backend_t *storage) {
    return lagfx_fmtcap_test_backend ? lagfx_fmtcap_test_backend : lagfx_fmtcap_device_backend(pd, storage);
}

const char *lagfx_fmtcap_why_name(int why) {
    static const char *const n[LAGFX_FMTCAP_WHY_N] = { "ok", "no_required_usage", "extent", "mips", "layers", "samples" };
    return why >= 0 && why < LAGFX_FMTCAP_WHY_N ? n[why] : "?";
}

VkFormatFeatureFlags lagfx_fmtcap_features(const lagfx_fmtcap_backend_t *be, VkFormat fmt) {
    if (!be || !be->format_props2 || fmt == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    be->format_props2(be->ctx, fmt, &fp);
    return fp.formatProperties.optimalTilingFeatures;
}

bool lagfx_fmtcap_blend_ok(const lagfx_fmtcap_backend_t *be, VkFormat fmt) {
    return (lagfx_fmtcap_features(be, fmt) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
}

/* The format feature a usage bit needs (Vulkan "Image Usage" / VUID-VkImageCreateInfo-usage-00964.. family; INPUT_ATTACHMENT: colour OR depth/stencil attachment). */
static bool usage_feature_ok(VkFormatFeatureFlags f, VkImageUsageFlagBits u) {
    switch (u) {
    case VK_IMAGE_USAGE_TRANSFER_SRC_BIT: return (f & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    case VK_IMAGE_USAGE_TRANSFER_DST_BIT: return (f & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
    case VK_IMAGE_USAGE_SAMPLED_BIT: return (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    case VK_IMAGE_USAGE_STORAGE_BIT: return (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT: return (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT: return (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    case VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT: return (f & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    default: return true;
    }
}

static const VkImageUsageFlagBits k_all_usage[] = { VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_STORAGE_BIT,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT };
/* the order optional usages are given up in when the combined mask is refused */
static const VkImageUsageFlagBits k_drop_order[] = { VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT };

bool lagfx_fmtcap_image_usage(const lagfx_fmtcap_backend_t *be, VkFormat fmt, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers,
                              VkSampleCountFlagBits samples, VkImageUsageFlags required, VkImageUsageFlags optional, lagfx_fmtcap_usage_t *out) {
    memset(out, 0, sizeof *out);
    optional &= ~required;
    if (!be || !be->format_props2 || !be->image_format_props2 || fmt == VK_FORMAT_UNDEFINED) { out->why = LAGFX_FMTCAP_NO_REQUIRED; return false; }
    const VkFormatFeatureFlags feat = lagfx_fmtcap_features(be, fmt);
    for (size_t i = 0; i < sizeof k_all_usage / sizeof *k_all_usage; i++)
        if ((required & k_all_usage[i]) && !usage_feature_ok(feat, k_all_usage[i])) { out->why = LAGFX_FMTCAP_NO_REQUIRED; return false; }
    VkImageUsageFlags mask = required | optional;
    for (size_t i = 0; i < sizeof k_all_usage / sizeof *k_all_usage; i++)
        if ((optional & k_all_usage[i]) && !usage_feature_ok(feat, k_all_usage[i])) mask &= ~(VkImageUsageFlags)k_all_usage[i], out->dropped |= k_all_usage[i];
    if ((mask & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && !(mask & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) && !(required & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
        mask &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, out->dropped |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    for (;;) {
        VkPhysicalDeviceImageFormatInfo2 in = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = fmt, .type = type, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = mask };
        VkImageFormatProperties2 pr = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        VkResult r = be->image_format_props2(be->ctx, &in, &pr);
        out->queries++;
        if (r == VK_SUCCESS) {
            const VkImageFormatProperties *ip = &pr.imageFormatProperties;
            out->sample_counts = ip->sampleCounts;
            if (w > ip->maxExtent.width || h > ip->maxExtent.height || d > ip->maxExtent.depth) { out->why = LAGFX_FMTCAP_EXTENT; return false; }
            if (mips > ip->maxMipLevels) { out->why = LAGFX_FMTCAP_MIPS; return false; }
            if (layers > ip->maxArrayLayers) { out->why = LAGFX_FMTCAP_LAYERS; return false; }
            if (samples != VK_SAMPLE_COUNT_1_BIT && !(ip->sampleCounts & samples)) { out->why = LAGFX_FMTCAP_SAMPLES; return false; }
            if (samples == VK_SAMPLE_COUNT_1_BIT && !(ip->sampleCounts & VK_SAMPLE_COUNT_1_BIT)) { out->why = LAGFX_FMTCAP_SAMPLES; return false; }
            out->usage = mask, out->why = LAGFX_FMTCAP_OK;
            return true;
        }
        /* refused: give up OPTIONAL usages, as few as possible. First look for ONE usage whose removal makes the mask acceptable (in k_drop_order); when no single removal
         * does, remove the first one in that order and look again (a pair of usages that is refused together drops only one of them). Any other failure than
         * "not supported" is treated the same (the image would not be creatable). */
        VkImageUsageFlagBits pick = 0;
        for (size_t i = 0; i < sizeof k_drop_order / sizeof *k_drop_order && !pick; i++) {
            const VkImageUsageFlagBits u = k_drop_order[i];
            if (!((mask & u) && (optional & u))) continue;
            VkImageUsageFlags m2 = mask & ~(VkImageUsageFlags)u;
            if (u == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && (optional & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) m2 &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            VkPhysicalDeviceImageFormatInfo2 in2 = in; in2.usage = m2;
            VkImageFormatProperties2 pr2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
            VkResult r2 = be->image_format_props2(be->ctx, &in2, &pr2);
            out->queries++;
            if (r2 == VK_SUCCESS) pick = u;
        }
        for (size_t i = 0; i < sizeof k_drop_order / sizeof *k_drop_order && !pick; i++)
            if ((mask & k_drop_order[i]) && (optional & k_drop_order[i])) pick = k_drop_order[i];
        if (!pick) { out->why = LAGFX_FMTCAP_NO_REQUIRED; out->dropped = 0; return false; }
        mask &= ~(VkImageUsageFlags)pick, out->dropped |= pick;
        if (pick == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && (mask & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && (optional & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
            mask &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, out->dropped |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
}

VkSampleCountFlags lagfx_fmtcap_sample_counts(const lagfx_fmtcap_backend_t *be, VkFormat fmt, VkImageUsageFlags usage) {
    if (!be || !be->image_format_props2 || fmt == VK_FORMAT_UNDEFINED) return 0;
    VkPhysicalDeviceImageFormatInfo2 in = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = fmt, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
    VkImageFormatProperties2 pr = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    if (be->image_format_props2(be->ctx, &in, &pr) != VK_SUCCESS) return 0;
    return pr.imageFormatProperties.sampleCounts;
}

uint32_t lagfx_fmtcap_max_samples(VkSampleCountFlags mask) {
    return (mask & VK_SAMPLE_COUNT_4_BIT) ? 4u : (mask & VK_SAMPLE_COUNT_2_BIT) ? 2u : (mask & VK_SAMPLE_COUNT_1_BIT) ? 1u : 0u;
}

int lagfx_fmtcap_class(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_S8_UINT:
        return LAGFX_FMTCAP_CLASS_UINT;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return LAGFX_FMTCAP_CLASS_SINT;
    default: return LAGFX_FMTCAP_CLASS_FLOAT;   /* UNORM / SNORM / SRGB / SFLOAT / UFLOAT / depth: read as float */
    }
}

/* Bits of component `ch` (0..3, in RGBA order) of an integer colour format; 0 for any other. */
static unsigned int_comp_bits(VkFormat f, int ch) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT: return 8;
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: return 16;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 32;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32: return ch == 3 ? 2u : 10u;
    default: return 0;
    }
}

void lagfx_fmtcap_clear_color(VkFormat fmt, const double c[4], VkClearColorValue *out) {
    memset(out, 0, sizeof *out);
    const int cls = lagfx_fmtcap_class(fmt);
    for (int i = 0; i < 4; i++) {
        const double v = c[i];
        if (cls == LAGFX_FMTCAP_CLASS_FLOAT) { out->float32[i] = (float)v; continue; }
        unsigned bits = int_comp_bits(fmt, i);
        if (!bits) bits = 32u;   /* S8_UINT and any class-integer format this table does not list: the full 32-bit member */
        if (cls == LAGFX_FMTCAP_CLASS_UINT) {
            const double mx = (double)((bits == 32u) ? 0xffffffffull : ((1ull << bits) - 1ull));
            out->uint32[i] = v != v || v <= 0.0 ? 0u : v >= mx ? (uint32_t)mx : (uint32_t)v;
        } else {
            const double hi = (double)((1ll << (bits - 1u)) - 1ll), lo = -(double)(1ll << (bits - 1u));
            out->int32[i] = v != v ? 0 : v >= hi ? (int32_t)hi : v <= lo ? (int32_t)lo : (int32_t)v;
        }
    }
}

const char *lagfx_fmtcap_class_name(int cls) {
    static const char *const n[LAGFX_FMTCAP_CLASS_N] = { "float", "uint", "sint" };
    return cls >= 0 && cls < LAGFX_FMTCAP_CLASS_N ? n[cls] : "?";
}

size_t lagfx_fmtcap_report(const lagfx_fmtcap_backend_t *be, const VkFormat *list, const uint32_t *mtl, size_t n, lagfx_fmtcap_emit_fn emit, void *user, lagfx_fmtcap_summary_t *sum) {
    lagfx_fmtcap_summary_t s;
    memset(&s, 0, sizeof s);
    size_t lines = 0;
    for (size_t i = 0; i < n; i++) {
        const VkFormat f = list[i];
        const VkFormatFeatureFlags ft = lagfx_fmtcap_features(be, f);
        const bool xfer = (ft & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (ft & VK_FORMAT_FEATURE_TRANSFER_DST_BIT), smp = (ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
                   ca = (ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, bl = (ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0, st = (ft & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0,
                   ds = (ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const VkSampleCountFlags sc = (ca || ds) ? lagfx_fmtcap_sample_counts(be, f, ds ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0u;
        s.formats++, s.no_transfer += !xfer, s.no_sampled += !smp, s.no_color_att += !(ca || ds), s.no_blend += (ca && !bl), s.no_storage += !st, s.no_input += !(ca || ds),
            s.no_msaa4 += (ca || ds) && !(sc & VK_SAMPLE_COUNT_4_BIT), s.no_msaa2 += (ca || ds) && !(sc & VK_SAMPLE_COUNT_2_BIT);
        if (emit) {
            char ln[400];
            snprintf(ln, sizeof ln, "g7-fmtcap: mtl=%u vk=%d class=%s transfer=%d sampled=%d color_att=%d ds_att=%d blend=%d storage=%d input_att=%d msaa_counts=0x%x features=0x%x",
                     mtl ? mtl[i] : 0u, (int)f, lagfx_fmtcap_class_name(lagfx_fmtcap_class(f)), (int)xfer, (int)smp, (int)ca, (int)ds, (int)bl, (int)st, (int)(ca || ds), (unsigned)sc, (unsigned)ft);
            emit(user, ln), lines++;
        }
    }
    if (emit) {
        char ln[400];
        snprintf(ln, sizeof ln, "g7-fmtcap: summary formats=%u missing[transfer=%u sampled=%u color_att_or_ds=%u blend(of attachable)=%u storage=%u input_att=%u msaa4=%u msaa2=%u]",
                 s.formats, s.no_transfer, s.no_sampled, s.no_color_att, s.no_blend, s.no_storage, s.no_input, s.no_msaa4, s.no_msaa2);
        emit(user, ln), lines++;
    }
    if (sum) *sum = s;
    return lines;
}
#endif
