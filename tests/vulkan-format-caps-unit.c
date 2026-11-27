/*
 * libapplegfx-vulkan — format / usage / blend / sample-count capability queries (src/vulkan/format_caps.c).
 * tests/vulkan-format-caps-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * (A) pure: the numeric class of every VkFormat the mirror creates equals the class implied by its MTLPixelFormat number (an independent list from Apple's enum:
 *     the *Uint / *Sint formats are 13 14 23 24 33 34 53 54 63 64 73 74 103 104 113 114 123 124).
 * (B) real device (lavapipe in CI / this container; says nothing about Adreno): for every format of the mirror's table the granted usage is sound against raw
 *     vkGetPhysicalDeviceFormatProperties2, a VkImage is really creatable with exactly the granted mask, blend / sample-count answers equal raw queries.
 * (C) injected backends = devices that are LESS capable than lavapipe: no features, transfer-only, a refused usage COMBINATION, size / mip / layer / sample limits,
 *     no blend feature. Every unsupported case must come back as an explicit result (why / dropped), never as a silently smaller mask. Each check has a positive control
 *     (the capable backend returns the full mask) and a negative one (the limited backend does not).
 * Skips (exit 77) when no Vulkan device is usable.
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/format_caps.h"
#include "protocol/arm64_resources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

#define U_REQ (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
#define U_OPT (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)

/* ---- injected device model ---- */
typedef struct {
    VkFormatFeatureFlags feat;
    VkImageUsageFlags refuse_if_any;   /* image-format query fails (FORMAT_NOT_SUPPORTED) when the mask contains ANY of these (a refused combination) */
    VkImageUsageFlags refuse_if_all;   /* ... or contains ALL of these */
    uint32_t max_dim, max_mips, max_layers;
    VkSampleCountFlags samples;
    uint32_t n_fp, n_ifp;
} model_t;
static void m_fp(void *c, VkFormat f, VkFormatProperties2 *o) { (void)f; model_t *m = c; m->n_fp++; memset(&o->formatProperties, 0, sizeof o->formatProperties); o->formatProperties.optimalTilingFeatures = m->feat; }
static VkResult m_ifp(void *c, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *o) {
    model_t *m = c; m->n_ifp++;
    if (m->refuse_if_any && (in->usage & m->refuse_if_any)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (m->refuse_if_all && (in->usage & m->refuse_if_all) == m->refuse_if_all) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(&o->imageFormatProperties, 0, sizeof o->imageFormatProperties);
    o->imageFormatProperties.maxExtent = (VkExtent3D){ m->max_dim, m->max_dim, m->max_dim };
    o->imageFormatProperties.maxMipLevels = m->max_mips, o->imageFormatProperties.maxArrayLayers = m->max_layers, o->imageFormatProperties.sampleCounts = m->samples;
    return VK_SUCCESS;
}
#define ALLF (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
static model_t mk_model(void) { return (model_t){ .feat = ALLF, .max_dim = 16384, .max_mips = 15, .max_layers = 2048, .samples = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT }; }
static lagfx_fmtcap_backend_t be_of(model_t *m) { return (lagfx_fmtcap_backend_t){ m, m_fp, m_ifp }; }

static int part_a(void) {
    static const uint32_t ints[] = { 13, 14, 23, 24, 33, 34, 53, 54, 63, 64, 73, 74, 103, 104, 113, 114, 123, 124 };   /* MTLPixelFormat R8Uint .. RGBA32Sint */
    VkFormat vk[128]; uint32_t mtl[128];
    size_t n = lagfx_a64_res_format_list(vk, mtl, 128);
    CHECK(n > 40 && n < 128, "format list size %zu", n);
    unsigned nu = 0, ns = 0, nf = 0;
    for (size_t i = 0; i < n; i++) {
        int want = LAGFX_FMTCAP_CLASS_FLOAT;
        for (size_t k = 0; k < sizeof ints / sizeof *ints; k++) if (mtl[i] == ints[k]) want = (ints[k] % 10 == 3) ? LAGFX_FMTCAP_CLASS_UINT : LAGFX_FMTCAP_CLASS_SINT;   /* ...Uint end in 3, ...Sint in 4 */
        if (mtl[i] == 253) want = LAGFX_FMTCAP_CLASS_UINT;   /* Stencil8 (S8_UINT): the stencil aspect reads as unsigned */
        int got = lagfx_fmtcap_class(vk[i]);
        CHECK(got == want, "mtl %u vk %d: class %s, expected %s", mtl[i], (int)vk[i], lagfx_fmtcap_class_name(got), lagfx_fmtcap_class_name(want));
        nu += got == LAGFX_FMTCAP_CLASS_UINT, ns += got == LAGFX_FMTCAP_CLASS_SINT, nf += got == LAGFX_FMTCAP_CLASS_FLOAT;
    }
    CHECK(nu == 10 && ns == 9 && nf == n - 19, "class counts uint=%u sint=%u float=%u of %zu", nu, ns, nf, n);   /* 9 Uint + Stencil8, 9 Sint */
    CHECK(lagfx_fmtcap_class(VK_FORMAT_B8G8R8A8_UNORM) == LAGFX_FMTCAP_CLASS_FLOAT && lagfx_fmtcap_class(VK_FORMAT_R32_UINT) == LAGFX_FMTCAP_CLASS_UINT &&
          lagfx_fmtcap_class(VK_FORMAT_R32_SINT) == LAGFX_FMTCAP_CLASS_SINT, "spot checks");
    CHECK(lagfx_fmtcap_max_samples(0) == 0 && lagfx_fmtcap_max_samples(1) == 1 && lagfx_fmtcap_max_samples(1 | 2) == 2 && lagfx_fmtcap_max_samples(1 | 2 | 4 | 8) == 4 && lagfx_fmtcap_max_samples(8 | 1) == 1, "max_samples");
    printf("PASS: (A) numeric class of all %zu mirror formats equals the Metal-number-implied class (uint=%u sint=%u float=%u)\n", n, nu, ns, nf);
    return 0;
}

static int part_b(VkPhysicalDevice pd, VkDevice dev) {
    lagfx_fmtcap_backend_t st; const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_device_backend(pd, &st);
    VkFormat vk[128]; uint32_t mtl[128];
    size_t n = lagfx_a64_res_format_list(vk, mtl, 128);
    unsigned granted_all = 0, trimmed = 0, created = 0, color_att = 0, blend = 0, noblend = 0;
    for (size_t i = 0; i < n; i++) {
        VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
        vkGetPhysicalDeviceFormatProperties2(pd, vk[i], &fp);
        const VkFormatFeatureFlags ft = fp.formatProperties.optimalTilingFeatures;
        CHECK(lagfx_fmtcap_features(be, vk[i]) == ft, "features differ for vk %d", (int)vk[i]);
        const bool ds = mtl[i] >= 250;
        lagfx_fmtcap_usage_t cu;
        bool ok = ds ? lagfx_fmtcap_image_usage(be, vk[i], VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &cu)
                     : lagfx_fmtcap_image_usage(be, vk[i], VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu);
        if (ds) { CHECK(!ok || (cu.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT), "ds usage"); }
        else {
            const bool xfer = (ft & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (ft & VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            CHECK(ok == xfer, "mtl %u: transfer feature %d but result %d (why %s)", mtl[i], (int)xfer, (int)ok, lagfx_fmtcap_why_name(cu.why));
        }
        if (!ok) continue;
        /* soundness: a granted usage has its format feature; a dropped one lacks it or was refused in the combination (then mask is smaller than the feature-implied one) */
        if (!ds) {
            if (cu.usage & VK_IMAGE_USAGE_SAMPLED_BIT) CHECK(ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "mtl %u sampled without feature", mtl[i]);
            if (cu.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) CHECK(ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "mtl %u colour att without feature", mtl[i]);
            if (cu.usage & VK_IMAGE_USAGE_STORAGE_BIT) CHECK(ft & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "mtl %u storage without feature", mtl[i]);
            if (!(ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) CHECK(cu.dropped & VK_IMAGE_USAGE_SAMPLED_BIT, "mtl %u: sampled drop not reported", mtl[i]);
            if (!(ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) CHECK((cu.dropped & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (cu.dropped & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT), "mtl %u: colour/input drop not reported", mtl[i]);
            if (!(ft & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) CHECK(cu.dropped & VK_IMAGE_USAGE_STORAGE_BIT, "mtl %u: storage drop not reported", mtl[i]);
            CHECK((cu.usage & cu.dropped) == 0 && (cu.usage | cu.dropped) == (U_REQ | U_OPT), "mtl %u: usage %x + dropped %x != asked %x", mtl[i], cu.usage, cu.dropped, U_REQ | U_OPT);
            color_att += (cu.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
            const bool b = lagfx_fmtcap_blend_ok(be, vk[i]);
            CHECK(b == ((ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0), "blend answer");
            if (cu.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) blend += b, noblend += !b;
        }
        granted_all += cu.dropped == 0, trimmed += cu.dropped != 0;
        /* the granted mask must really create an image */
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = vk[i], .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = cu.usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage img = VK_NULL_HANDLE;
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        CHECK(r == VK_SUCCESS, "mtl %u vk %d: vkCreateImage with the granted usage %x -> %d", mtl[i], (int)vk[i], cu.usage, (int)r);
        vkDestroyImage(dev, img, NULL), created++;
        /* sample counts equal a raw query for the colour-attachment usage */
        if (!ds && (cu.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            VkPhysicalDeviceImageFormatInfo2 in = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = vk[i], .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
            VkImageFormatProperties2 pr = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
            CHECK(vkGetPhysicalDeviceImageFormatProperties2(pd, &in, &pr) == VK_SUCCESS, "raw ifp2");
            CHECK(lagfx_fmtcap_sample_counts(be, vk[i], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == pr.imageFormatProperties.sampleCounts, "sample counts differ");
        }
    }
    VkFormat lst[128]; uint32_t ml[128]; size_t nl = lagfx_a64_res_format_list(lst, ml, 128);
    lagfx_fmtcap_summary_t sum; size_t lines = lagfx_fmtcap_report(be, lst, ml, nl, NULL, NULL, &sum);
    CHECK(lines == 0 && sum.formats == nl, "report summary without emit");
    printf("PASS: (B) lavapipe: %zu formats, %u granted the full mask, %u trimmed (reported), %u images really created; colour-attachable %u (blendable %u, not blendable %u); report missing[sampled=%u color_or_ds=%u blend=%u storage=%u msaa4=%u]\n",
           n, granted_all, trimmed, created, color_att, blend, noblend, sum.no_sampled, sum.no_color_att, sum.no_blend, sum.no_storage, sum.no_msaa4);
    return 0;
}

static int part_c(void) {
    lagfx_fmtcap_usage_t cu;
    /* positive control: a fully capable device grants the whole mask, reports nothing dropped, 1 query */
    model_t cap = mk_model(); lagfx_fmtcap_backend_t bc = be_of(&cap);
    CHECK(lagfx_fmtcap_image_usage(&bc, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 1920, 1080, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.usage == (U_REQ | U_OPT) && !cu.dropped &&
          cu.why == LAGFX_FMTCAP_OK && cu.queries == 1, "capable device: usage %x dropped %x why %d", cu.usage, cu.dropped, cu.why);
    CHECK(lagfx_fmtcap_blend_ok(&bc, VK_FORMAT_B8G8R8A8_UNORM), "capable device blends");

    /* no features at all: refused with NO_REQUIRED (never an empty mask presented as success) */
    model_t none = mk_model(); none.feat = 0; lagfx_fmtcap_backend_t bn = be_of(&none);
    CHECK(!lagfx_fmtcap_image_usage(&bn, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_NO_REQUIRED && cu.usage == 0, "no features");
    CHECK(!lagfx_fmtcap_blend_ok(&bn, VK_FORMAT_B8G8R8A8_UNORM), "no features: no blend");

    /* transfer only: granted = transfer, ALL four optional usages reported as dropped (the old code dropped them silently) */
    model_t xf = mk_model(); xf.feat = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT; lagfx_fmtcap_backend_t bx = be_of(&xf);
    CHECK(lagfx_fmtcap_image_usage(&bx, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.usage == U_REQ && cu.dropped == U_OPT, "transfer only: usage %x dropped %x", cu.usage, cu.dropped);

    /* the feature bits are all there but the DEVICE refuses the combination with STORAGE (a real class of limits): STORAGE is dropped, the others survive, 2 queries */
    model_t cs = mk_model(); cs.refuse_if_any = VK_IMAGE_USAGE_STORAGE_BIT; lagfx_fmtcap_backend_t bs = be_of(&cs);
    CHECK(lagfx_fmtcap_image_usage(&bs, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.dropped == VK_IMAGE_USAGE_STORAGE_BIT &&
          cu.usage == (U_REQ | U_OPT) - VK_IMAGE_USAGE_STORAGE_BIT && cu.queries == 3, "refused storage (1 refused + 1 probe + 1 confirm): usage %x dropped %x queries %u", cu.usage, cu.dropped, cu.queries);

    /* colour attachment AND input attachment together refused (needed for framebuffer fetch): input attachment goes first, colour attachment stays */
    model_t ci2 = mk_model(); ci2.refuse_if_all = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT; lagfx_fmtcap_backend_t bi = be_of(&ci2);
    CHECK(lagfx_fmtcap_image_usage(&bi, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.dropped == VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT &&
          (cu.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), "colour+input refused: usage %x dropped %x", cu.usage, cu.dropped);

    /* required usage refused outright */
    model_t rq = mk_model(); rq.refuse_if_any = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; lagfx_fmtcap_backend_t br = be_of(&rq);
    CHECK(!lagfx_fmtcap_image_usage(&br, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_NO_REQUIRED && cu.usage == 0 && cu.dropped == 0, "required refused");

    /* limits: extent, mips, layers, samples (each with its positive control right before) */
    model_t lim = mk_model(); lim.max_dim = 8192, lim.max_mips = 4, lim.max_layers = 8, lim.samples = VK_SAMPLE_COUNT_1_BIT; lagfx_fmtcap_backend_t bl = be_of(&lim);
    CHECK(lagfx_fmtcap_image_usage(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 8192, 8192, 1, 4, 8, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu), "limits positive control (exactly at the limits)");
    CHECK(!lagfx_fmtcap_image_usage(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 16384, 64, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_EXTENT, "extent");
    CHECK(!lagfx_fmtcap_image_usage(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 5, 1, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_MIPS, "mips");
    CHECK(!lagfx_fmtcap_image_usage(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 9, VK_SAMPLE_COUNT_1_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_LAYERS, "layers");
    CHECK(!lagfx_fmtcap_image_usage(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_4_BIT, U_REQ, U_OPT, &cu) && cu.why == LAGFX_FMTCAP_SAMPLES, "samples 4 on a 1-only device");
    CHECK(lagfx_fmtcap_image_usage(&bc, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, 64, 64, 1, 1, 1, VK_SAMPLE_COUNT_4_BIT, U_REQ, U_OPT, &cu), "samples 4 on a capable device");
    CHECK(lagfx_fmtcap_sample_counts(&bl, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == VK_SAMPLE_COUNT_1_BIT && lagfx_fmtcap_max_samples(1) == 1, "1-only device: max 1");
    CHECK(lagfx_fmtcap_sample_counts(&bc, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 7 && lagfx_fmtcap_max_samples(7) == 4, "capable device: max 4");
    model_t nos = mk_model(); nos.refuse_if_any = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; lagfx_fmtcap_backend_t bo = be_of(&nos);
    CHECK(lagfx_fmtcap_sample_counts(&bo, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0, "refused attachment usage: no sample counts at all");

    /* the report: one line per format + summary, counts the missing capabilities of a transfer-only device */
    const VkFormat lst[3] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8_UINT }; const uint32_t ml[3] = { 80, 125, 13 };
    lagfx_fmtcap_summary_t sm;
    CHECK(lagfx_fmtcap_report(&bx, lst, ml, 3, NULL, NULL, &sm) == 0 && sm.formats == 3 && sm.no_transfer == 0 && sm.no_sampled == 3 && sm.no_color_att == 3 && sm.no_storage == 3 && sm.no_input == 3, "transfer-only summary");
    CHECK(lagfx_fmtcap_report(&bc, lst, ml, 3, NULL, NULL, &sm) == 0 && sm.no_sampled == 0 && sm.no_color_att == 0 && sm.no_blend == 0 && sm.no_msaa4 == 0, "capable summary (positive control)");
    printf("PASS: (C) injected devices: capable grants all; no-feature / transfer-only / refused combinations / size / mip / layer / sample limits are explicit (why + dropped), blend + sample-count answers follow the backend\n");
    return 0;
}

int main(void) {
    int rc = part_a();
    if (rc) return rc;
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip (B)\n"); return part_c() ? 1 : 77; }
    if ((rc = part_b(vk->phys_device, vk->device))) return rc;
    return part_c();
}
