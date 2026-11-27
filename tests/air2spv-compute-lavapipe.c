/*
 * libapplegfx-vulkan — the translated tile kernel, executed (lavapipe / any Vulkan 1.3 device with subgroup shuffle)
 * tests/air2spv-compute-lavapipe.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Translates tests/fixtures/tile_simd_blur_15.air.bc (Apple's horizontal/vertical simd blur: 15-tap filter over a simd_shuffle_and_fill_down of two
 * texture reads, result stored into the [[imageblock]]) and runs it as a compute pipeline:
 *   binding 0 sampled image (in_tex, RGBA16F)   binding 1 SSBO {ushort2 src_offset, ushort2 fill_offset, ushort horizontal, half edr_scale}
 *   binding 2 SSBO half weights[15]             binding 3 sampler (the translator's default sampler; unused)   storage image of imageblock rt 0 (its binding comes from the translator's allocator:
 *   found by reflection - the variable is named lagfx.imageblock.0 -, it is NOT a fixed number)
 *   spec constants 0/1/2 = local size (subgroupSize, 4, 1)
 * and compares every pixel with a CPU model of the kernel:
 *   out(x,y).rgb = edr * sum_i w[i] * tex(x+i, y).rgb ;  out.a = sum_i w[i] * tex(x+i, y).a      (i = 0..subgroupSize, weights beyond are 0)
 * (fill_offset = (S,0) makes "data of lane+i, or filling data of lane+i-S" exactly tex(x+i)). All values are small multiples of 1/16 -> exact in half.
 *
 * Exit 77 (skip) when there is no Vulkan device, or the device lacks int16/int64/int8/float16/16-bit storage/subgroup shuffle in compute.
 */
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "air/bitcode_reader.h"
#include "vulkan/caps.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP(...) do { printf("SKIP: "); printf(__VA_ARGS__); printf("\n"); return 77; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

/* ---- half <-> float (values in the test are exactly representable) ---- */
static uint16_t f2h(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t s = (v.u >> 16) & 0x8000u; int e = (int)((v.u >> 23) & 0xff) - 127 + 15; uint32_t m = v.u & 0x7fffffu;
    if (f == 0.0f) return (uint16_t)s;
    if (e <= 0 || e >= 31) return (uint16_t)s;   /* not used */
    return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu; float r;
    if (e == 0) r = (float)m / 1024.0f / 16384.0f;
    else { r = 1.0f + (float)m / 1024.0f; for (int k = (int)e - 15; k > 0; k--) r *= 2.0f; for (int k = (int)e - 15; k < 0; k++) r *= 0.5f; }
    return s ? -r : r;
}

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

static VkPhysicalDevice g_pd; static VkDevice g_dev;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } buf_t;
static int mkbuf(buf_t *o, VkDeviceSize n, VkBufferUsageFlags u) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(g_dev, &ci, NULL, &o->b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, o->b, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->m)); VK(vkBindBufferMemory(g_dev, o->b, o->m, 0)); VK(vkMapMemory(g_dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p));
    memset(o->p, 0, (size_t)n);
    return 0;
}
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; } img_t;
static int mkimg(img_t *o, uint32_t w, uint32_t h, VkImageUsageFlags u) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(g_dev, &ci, NULL, &o->i));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, o->i, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->m)); VK(vkBindImageMemory(g_dev, o->i, o->m, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ci.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(g_dev, &vi, NULL, &o->v));
    return 0;
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* the texture the kernel reads: small integers per channel */
static float texv(uint32_t x, uint32_t y, int ch) {
    switch (ch) { case 0: return (float)((x * 3 + y * 5) % 16); case 1: return (float)((x + y * 7) % 16); case 2: return (float)((x * y) % 16); default: return (float)((x + y) % 16); }
}

int main(void) {
    /* ---- translate ---- */
    size_t an = 0; uint8_t *air = slurp("tile_simd_blur_15.air.bc", &an);
    CHECK(air, "fixture tile_simd_blur_15.air.bc not found");
    lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t spv_n = 0;
    CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK && m, "module_open");
    CHECK(lagfx_air2spv_translate_module(m, &spv, &spv_n) == LAGFX_OK && spv, "translate");
    uint32_t need = 0; CHECK(lagfx_spv_required_caps_ex(spv, spv_n, &need), "caps scan");

    /* ---- device ---- */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "air2spv-compute-lavapipe", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) SKIP("no Vulkan 1.3 instance");
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) SKIP("no Vulkan device");
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);

    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11 };
    vkGetPhysicalDeviceProperties2(g_pd, &pp);
    if (!(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !(p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) ||
        !(p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)) SKIP("device has no compute subgroup shuffle");
    uint32_t S = p11.subgroupSize;
    if (S < 4 || S > 64) SKIP("subgroup size %u not handled by this test", S);

    VkPhysicalDeviceVulkan12Features q12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features q11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &q12 };
    VkPhysicalDeviceFeatures2 q2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q11 };
    vkGetPhysicalDeviceFeatures2(g_pd, &q2);
    uint32_t have = 0;
    if (q2.features.shaderInt64) have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64);
    if (q2.features.shaderInt16) have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16);
    if (q12.shaderInt8) have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8);
    if (q12.shaderFloat16) have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16);
    if (q11.storageBuffer16BitAccess) have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16);
    have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE);   /* Vulkan 1.3 + checked above */
    if (need & ~have) SKIP("module needs capabilities the device lacks (need 0x%x, have 0x%x)", need, have);

    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .shaderInt8 = q12.shaderInt8, .shaderFloat16 = q12.shaderFloat16 };
    VkPhysicalDeviceVulkan11Features e11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12, .storageBuffer16BitAccess = q11.storageBuffer16BitAccess };
    VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11, .features = { .shaderInt64 = q2.features.shaderInt64, .shaderInt16 = q2.features.shaderInt16 } };
    float prio = 1.0f;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    uint32_t qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    free(qp);
    CHECK(qf != ~0u, "no compute queue");
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    VkQueue queue; vkGetDeviceQueue(g_dev, qf, 0, &queue);

    /* ---- data ---- */
    const uint32_t GX = 2, GY = 2, LY = 4, W = GX * S, H = GY * LY, TW = 3 * S + 4, TH = H;
    img_t tex, out;
    if (mkimg(&tex, TW, TH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) || mkimg(&out, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return 1;
    buf_t up, ub, wb, rb;
    if (mkbuf(&up, (VkDeviceSize)TW * TH * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || mkbuf(&ub, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        mkbuf(&wb, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || mkbuf(&rb, (VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 1;
    uint16_t *tp = up.p;
    for (uint32_t y = 0; y < TH; y++) for (uint32_t x = 0; x < TW; x++) for (int ch = 0; ch < 4; ch++) tp[((size_t)y * TW + x) * 4 + ch] = f2h(texv(x, y, ch));
    uint16_t *u16 = ub.p;                       /* { ushort2 src_offset; ushort2 fill_offset; ushort horizontal; half edr_scale } */
    u16[0] = 0; u16[1] = 0; u16[2] = (uint16_t)S; u16[3] = 0; u16[4] = 1; u16[5] = f2h(0.5f);
    uint16_t *w16 = wb.p; float w[15];
    for (int i = 0; i < 15; i++) { w[i] = ((uint32_t)i <= S) ? (float)(i % 4 + 1) / 8.0f : 0.0f; w16[i] = f2h(w[i]); }

    /* ---- pipeline ---- */
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    uint32_t ibb = ~0u;   /* the imageblock image's binding: reflected (role_index = render target) */
    { lagfx_spv_binding_t rf[16]; size_t nrf = lagfx_spv_reflect_bindings(spv, spv_n, rf, 16); for (size_t i = 0; i < nrf && i < 16; i++) if (rf[i].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE && rf[i].role_index == 0u) ibb = rf[i].binding; }
    CHECK(ibb != ~0u && ibb != 0 && ibb != 1 && ibb != 2 && ibb != 3, "the imageblock image has a binding of its own (got %u)", ibb);
    VkDescriptorSetLayoutBinding lb[5] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { ibb, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    uint32_t lsz[3] = { S, LY, 1 };
    VkSpecializationMapEntry sme[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
    VkSpecializationInfo si = { 3, sme, sizeof lsz, lsz };
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main", .pSpecializationInfo = &si } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler samp; VK(vkCreateSampler(g_dev, &sci, NULL, &samp));
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 4, .pPoolSizes = ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    VkDescriptorImageInfo ii_tex = { VK_NULL_HANDLE, tex.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, ii_samp = { samp, VK_NULL_HANDLE, 0 }, ii_out = { VK_NULL_HANDLE, out.v, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo bi_u = { ub.b, 0, VK_WHOLE_SIZE }, bi_w = { wb.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr[5] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii_tex },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi_u },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi_w },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii_samp },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = ibb, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii_out } };
    vkUpdateDescriptorSets(g_dev, 5, wr, 0, NULL);

    /* ---- run ---- */
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    barrier(cb, tex.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TW, TH, 1 } };
    vkCmdCopyBufferToImage(cb, up.b, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    barrier(cb, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, out.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, GX, GY, 1);
    barrier(cb, out.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy oc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &oc);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));

    /* ---- compare ---- */
    const uint16_t *rp = rb.p; unsigned bad = 0, shown = 0;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float acc[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i <= S && i < 15; i++) for (int ch = 0; ch < 4; ch++) acc[ch] += w[i] * texv(x + i, y, ch);
        for (int ch = 0; ch < 3; ch++) acc[ch] *= 0.5f;
        for (int ch = 0; ch < 4; ch++) {
            float got = h2f(rp[((size_t)y * W + x) * 4 + ch]);
            if (got != acc[ch]) { bad++; if (shown++ < 8) printf("  mismatch (%u,%u).%d: got %g expected %g\n", x, y, ch, got, acc[ch]); }
        }
    }
    printf("subgroup size %u, image %ux%u, %u channel mismatches\n", S, W, H, bad);
    CHECK(bad == 0, "translated tile kernel output differs from the CPU model");
    printf("PASS: translated tile_simd_blur_15 executes correctly (simd_shuffle_and_fill_down, thread builtins, imageblock store)\n");
    vkDeviceWaitIdle(g_dev);
    free(spv); lagfx_air_module_free(m); free(air);
    return 0;
}
