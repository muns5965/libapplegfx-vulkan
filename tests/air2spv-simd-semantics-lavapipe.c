/*
 * libapplegfx-vulkan — Metal SIMD-group semantics of translated compute kernels, executed against a CPU reference (lavapipe / any Vulkan 1.3 device)
 * tests/air2spv-simd-semantics-lavapipe.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixtures (hand-written AIR, tests/fixtures/gen-simd-fixtures.sh; NOT Apple output): simd_semantics.air.bc is a tile kernel that stores, per thread, the thread-position
 * queries and simd_shuffle / _down / _up / _xor / _and_fill_down / _and_fill_up on uint, ushort, half, uint2 and ulong into the Rgba16f imageblock (so the tile path is exercised
 * too); workgroup g of the dispatch selects the operands. The CPU reference below follows the Metal Shading
 * Language Specification, section 6.10.2 (SIMD-group functions):
 *   - simd_shuffle_down / _up "doesn't modify the upper / lower delta lanes of data because it doesn't wrap values around the SIMD-group": a lane whose source
 *     lane is outside the SIMD-group keeps its own data;
 *   - simd_shuffle_and_fill_down / _up: lanes are split into vectors of `modulo` lanes; the lanes whose source is past the vector edge read filling_data
 *     of the wrapped lane;
 *   - simd_shuffle(lane) / _xor need a valid lane id.
 * Every case runs for the NATIVE simdgroup (the device subgroup) and for LOGICAL simdgroups of width W carved out of the subgroup (lagfx_air2spv_set_simd_emulation:
 * W = 2, 4, ... up to the device subgroup size), so a device with a wider subgroup than lavapipe's 8 is simulated by a narrower logical width inside it. The CPU reference is
 * parameterised by the width the kernel is supposed to see. NOTHING here says what a 64/128-wide Adreno subgroup does (see the phone-only checks in the report).
 *
 * Also: the invocation -> subgroup mapping the emulation (and simdgroup_index_in_threadgroup) assume, LocalInvocationIndex == SubgroupId * SubgroupSize + lane, is MEASURED on
 * the device for several workgroup shapes and printed; on a device where it does not hold the emulation cases are skipped (INFO) instead of asserted.
 * And: simd operations the translator has no implementation for (air.simd_sum, air.simd_ballot, ...) must make a compute translation FAIL (never an OpUndef store).
 *
 * Exit 77 (skip) when there is no Vulkan device, or the device lacks int16/int64/float16 / subgroup shuffle in compute.
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

static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_queue; static uint32_t g_qf, g_S, g_have;
static unsigned g_val_errors;

static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT ty, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)ty; (void)u;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { g_val_errors++; printf("VALIDATION: %s\n", d->pMessage); }
    return VK_FALSE;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } buf_t;
static int mkbuf(buf_t *o, VkDeviceSize n) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(g_dev, &ci, NULL, &o->b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, o->b, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->m)); VK(vkBindBufferMemory(g_dev, o->b, o->m, 0)); VK(vkMapMemory(g_dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p));
    memset(o->p, 0xFF, (size_t)n);   /* 0xFFFF halves are NaN: an unwritten texel never equals an expected integer */
    return 0;
}

/* Translate `fixture` (compute) for logical simdgroup width emu (0 = native) and return the SPIR-V. */
static lagfx_status_t translate(const char *fixture, uint32_t emu, uint8_t **spv, size_t *spv_n) {
    size_t an = 0; uint8_t *air = slurp(fixture, &an);
    if (!air) { printf("fixture %s not found\n", fixture); return LAGFX_ERR_NOT_FOUND; }
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, an, &m) != LAGFX_OK || !m) { free(air); return LAGFX_ERR_INVALID_ARG; }
    *spv = NULL; *spv_n = 0;
    lagfx_air2spv_set_simd_emulation(emu);
    lagfx_status_t st = lagfx_air2spv_translate_module(m, spv, spv_n);
    lagfx_air2spv_set_simd_emulation(0);
    lagfx_air_module_free(m); free(air);
    return st;
}

static float h2f(uint16_t h) {   /* integers below 2048 only (exactly what the kernel stores) */
    uint32_t e = (h >> 10) & 0x1fu, m = h & 0x3ffu; float r;
    if (e == 31) return -1.0f;   /* NaN / inf: never an expected value */
    if (e == 0) r = (float)m / 1024.0f / 16384.0f;
    else { r = 1.0f + (float)m / 1024.0f; for (int k = (int)e - 15; k > 0; k--) r *= 2.0f; for (int k = (int)e - 15; k < 0; k++) r *= 0.5f; }
    return (h >> 15) ? -r : r;
}

#define NGROUPS 48u   /* workgroups of the dispatch: the operand tuple is chosen by the group index (see gen-simd-fixtures.sh) */

/* Run the translated tile kernel as NGROUPS workgroups of lx*ly threads over an Rgba16f attachment (4*ly rows per group column); out[g][t][16]. */
static int run(const uint8_t *spv, size_t spv_n, uint32_t lx, uint32_t ly, uint32_t *out) {
    lagfx_spv_binding_t rf[8]; size_t nrf = lagfx_spv_reflect_bindings(spv, spv_n, rf, 8);
    CHECK(nrf == 1 && rf[0].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE && rf[0].role_index == 0u, "expected the imageblock storage image alone, got %zu bindings", nrf);
    const uint32_t IW = NGROUPS * lx, IH = 4u * ly, n = lx * ly;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = { IW, IH, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; VK(vkCreateImage(g_dev, &ic, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory im; VK(vkAllocateMemory(g_dev, &ai, NULL, &im)); VK(vkBindImageMemory(g_dev, img, im, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ic.format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView iv; VK(vkCreateImageView(g_dev, &vi, NULL, &iv));
    buf_t rb; if (mkbuf(&rb, (VkDeviceSize)IW * IH * 8)) return 1;
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb = { rf[0].binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    uint32_t lsz[3] = { lx, ly, 1 };
    VkSpecializationMapEntry sme[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
    VkSpecializationInfo si = { 3, sme, sizeof lsz, lsz };
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main", .pSpecializationInfo = &si } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = rf[0].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii };
    vkUpdateDescriptorSets(g_dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, NGROUPS, 1, 1);
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VkBufferImageCopy oc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { IW, IH, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &oc);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(g_queue, 1, &sub, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(g_queue));
    const uint16_t *hp = rb.p;
    for (uint32_t g = 0; g < NGROUPS; g++) for (uint32_t t = 0; t < n; t++) {
        uint32_t x = t % lx, y = t / lx;
        for (uint32_t j = 0; j < 4; j++) for (uint32_t c = 0; c < 4; c++) {
            float f = h2f(hp[(((size_t)(y + j * ly)) * IW + (size_t)g * lx + x) * 4 + c]);
            out[((size_t)g * n + t) * 16 + j * 4 + c] = f < 0.0f ? 0xFFFFFFFFu : (uint32_t)f;   /* -1 (NaN / unwritten) */
        }
    }
    vkDestroyCommandPool(g_dev, cp, NULL); vkDestroyDescriptorPool(g_dev, dp, NULL); vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dl, NULL); vkDestroyShaderModule(g_dev, sm, NULL);
    vkUnmapMemory(g_dev, rb.m); vkDestroyBuffer(g_dev, rb.b, NULL); vkFreeMemory(g_dev, rb.m, NULL);
    vkDestroyImageView(g_dev, iv, NULL); vkDestroyImage(g_dev, img, NULL); vkFreeMemory(g_dev, im, NULL);
    return 0;
}

/* ---- the CPU reference (Metal semantics, logical simdgroup width Wl, consecutive thread indices form a simdgroup) ---- */
static uint32_t V(uint32_t t) { return t * 7u + 3u; }
static uint32_t VF(uint32_t t) { return V(t) + 100u; }
static uint32_t VM(uint32_t t) { return V(t) & 1023u; }

typedef struct { bool fill_ok; } refinfo_t;
static void params_of_group(uint32_t g, uint32_t Wl, uint32_t p[4]) {   /* mirrors the operand selection in simd_semantics.air.bc */
    uint32_t dsel = (g >> 1) & 3u, msel = (g >> 3) % 3u;
    p[0] = (g & 1u) * 2u + 1u;
    p[1] = dsel == 0 ? 1u : dsel == 1 ? Wl >> 1 : dsel == 2 ? Wl - 1u : Wl;
    p[2] = msel == 0 ? 1u : msel == 1 ? Wl >> 1 : Wl - 1u;
    p[3] = (g / 24u) & 1u;
}
static void reference(uint32_t n, uint32_t Wl, const uint32_t p[4], uint32_t *e, refinfo_t *ri) {
    uint32_t d = p[1], mask = p[2], mod = Wl >> p[3], nsg = (n + Wl - 1) / Wl;
    ri->fill_ok = mod >= 2 && d < mod;   /* Metal: modulo is 2..32 and delta < modulo; outside it the result is not defined */
    for (uint32_t t = 0; t < n; t++) {
        uint32_t g = t / Wl, lane = t % Wl, base = g * Wl, idx = (lane + p[0]) & (Wl - 1u), *o = e + t * 16u;
        o[0] = lane; o[1] = Wl; o[2] = g; o[3] = nsg;
        o[4] = V(base + idx);
        o[5] = lane + d < Wl ? V(base + lane + d) : V(t);
        o[6] = d <= lane ? V(base + lane - d) : V(t);
        o[7] = V(base + (lane ^ mask));
        if (ri->fill_ok) {
            uint32_t r = lane % mod, vb = base + (lane - r);
            o[8] = r + d < mod ? V(vb + r + d) : VF(vb + r + d - mod);
            o[9] = r >= d ? V(vb + r - d) : VF(vb + r + mod - d);
        } else o[8] = o[9] = 0;
        o[10] = V(base + idx) & 0xffffu;
        o[11] = lane + d < Wl ? VM(base + lane + d) : VM(t);
        o[12] = V(base + idx);
        o[13] = VF(base + idx);
        o[14] = V(base + idx);
        o[15] = VF(base + idx);
    }
}
static const char *kname[16] = { "lane", "size", "sg_index", "sg_count", "shuffle", "down", "up", "xor", "fill_down", "fill_up", "u16 shuffle", "half down", "uint2.x", "uint2.y", "ulong.lo", "ulong.hi" };

/* Compare; prints the first mismatches per word kind. */
static unsigned g_shown, g_tot_k[10][16];   /* [width index][word kind]: mismatching words over all cases */
static unsigned g_wi;
static unsigned compare(const uint32_t *got, const uint32_t *exp, uint32_t n, bool fill_ok, const char *what) {
    unsigned bad = 0, perk[16] = { 0 };
    for (uint32_t t = 0; t < n; t++) for (int k = 0; k < 16; k++) {
        if ((k == 8 || k == 9) && !fill_ok) continue;
        if (got[t * 16 + k] != exp[t * 16 + k]) { g_tot_k[g_wi][k]++; if (perk[k]++ < 2 && g_shown < 24) printf("  %s: thread %u word %s: got %u expected %u\n", what, t, kname[k], got[t * 16 + k], exp[t * 16 + k]); bad++; }
    }
    if (bad && g_shown++ < 24) { printf("  %s: %u mismatching words (", what, bad); for (int k = 0; k < 16; k++) if (perk[k]) printf(" %s:%u", kname[k], perk[k]); printf(" )\n"); }
    return bad;
}

int main(void) {
    /* ---- device ---- */
    int want_val = 0; { uint32_t nl = 0; vkEnumerateInstanceLayerProperties(&nl, NULL); VkLayerProperties *lp = calloc(nl ? nl : 1, sizeof *lp); vkEnumerateInstanceLayerProperties(&nl, lp);
        for (uint32_t i = 0; i < nl; i++) { if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) want_val = 1; }
        free(lp); }
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" }, *exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "air2spv-simd-semantics", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (want_val) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = exts; }
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) { want_val = 0; ici.enabledLayerCount = 0; ici.enabledExtensionCount = 0; if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) SKIP("no Vulkan 1.3 instance"); }
    if (want_val) {
        PFN_vkCreateDebugUtilsMessengerEXT cr = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, .pfnUserCallback = dbg_cb };
        VkDebugUtilsMessengerEXT mess; if (cr) cr(g_inst, &mi, NULL, &mess);
    }
    printf("INFO: Khronos validation layer %s\n", want_val ? "enabled (errors fail the test)" : "NOT available (no validation)");
    uint32_t nd = 0; vkEnumeratePhysicalDevices(g_inst, &nd, NULL);
    if (!nd) SKIP("no Vulkan device");
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(g_inst, &nd, pds);
    g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11 };
    vkGetPhysicalDeviceProperties2(g_pd, &pp);
    if (!(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !(p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) ||
        !(p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)) SKIP("device has no compute subgroup shuffle");
    g_S = p11.subgroupSize;
    if (g_S < 4 || g_S > 64 || (g_S & (g_S - 1))) SKIP("subgroup size %u not handled by this test", g_S);
    printf("INFO: device %s, subgroup size %u (an Adreno 64 / 128 is NOT predicted by these results)\n", pp.properties.deviceName, g_S);
    VkPhysicalDeviceVulkan12Features q12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features q11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &q12 };
    VkPhysicalDeviceFeatures2 q2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q11 };
    vkGetPhysicalDeviceFeatures2(g_pd, &q2);
    if (q2.features.shaderInt64) g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64);
    if (q2.features.shaderInt16) g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16);
    if (q12.shaderInt8) g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8);
    if (q12.shaderFloat16) g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16);
    if (q11.storageBuffer16BitAccess) g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16);
    g_have |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE);

    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .shaderInt8 = q12.shaderInt8, .shaderFloat16 = q12.shaderFloat16 };
    VkPhysicalDeviceVulkan11Features e11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12, .storageBuffer16BitAccess = q11.storageBuffer16BitAccess };
    VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11, .features = { .shaderInt64 = q2.features.shaderInt64, .shaderInt16 = q2.features.shaderInt16 } };
    float prio = 1.0f;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    g_qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qf = i; break; }
    free(qp);
    CHECK(g_qf != ~0u, "no compute queue");
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_queue);

    /* ---- workgroup shapes: n = 4 subgroups of the device size; linear and 2-D ---- */
    const uint32_t S = g_S, n = 4u * S;
    const uint32_t shapes[3][2] = { { n, 1 }, { 8, n / 8 }, { 16, n / 16 } };
    const uint32_t nshapes = 3;
    unsigned fails = 0, cases = 0, skipped = 0;
    uint32_t *got = malloc((size_t)NGROUPS * n * 64), *exp = malloc((size_t)n * 64);

    /* ---- 1. the invocation -> subgroup mapping the emulation assumes (native kernel: sgi * size + lane must equal the thread index) ---- */
    bool mapping_ok = true;
    {
        uint8_t *spv; size_t sn; lagfx_status_t st = translate("simd_semantics.air.bc", 0, &spv, &sn);
        CHECK(st == LAGFX_OK && spv, "native translation of simd_semantics failed (st=%d)", (int)st);
        uint32_t need = 0; CHECK(lagfx_spv_required_caps_ex(spv, sn, &need), "caps scan");
        if (need & ~g_have) SKIP("module needs capabilities the device lacks (need 0x%x, have 0x%x)", need, g_have);
        for (uint32_t s = 0; s < nshapes; s++) {
            if (run(spv, sn, shapes[s][0], shapes[s][1], got)) return 1;
            unsigned bad = 0;
            for (uint32_t t = 0; t < n; t++) if (got[t * 16 + 2] * got[t * 16 + 1] + got[t * 16 + 0] != t) bad++;
            printf("INFO: mapping probe, workgroup %ux%u: %u of %u threads break LocalInvocationIndex == SubgroupId*SubgroupSize + lane (reported size %u)\n", shapes[s][0], shapes[s][1], bad, n, got[1]);
            if (bad) mapping_ok = false;
        }
        free(spv);
    }

    /* ---- 2. semantics for the native simdgroup and logical widths ---- */
    uint32_t widths[8]; unsigned nw = 0;
    widths[nw++] = 0;                                        /* native */
    for (uint32_t w = 2; w <= S; w <<= 1) widths[nw++] = w;  /* logical simdgroups of W lanes inside the device subgroup */
    for (unsigned wi = 0; wi < nw; wi++) {
        uint32_t W = widths[wi], Wl = W ? W : S;
        g_wi = wi;
        if (W && !mapping_ok) { printf("INFO: logical width %u skipped: the device's invocation -> subgroup mapping is not linear (emulation is not offered there)\n", W); skipped++; continue; }
        uint8_t *spv; size_t sn; lagfx_status_t st = translate("simd_semantics.air.bc", W, &spv, &sn);
        CHECK(st == LAGFX_OK && spv, "translation (logical width %u) failed st=%d", W, (int)st);
        for (uint32_t s = 0; s < nshapes; s++) {
            if (run(spv, sn, shapes[s][0], shapes[s][1], got)) return 1;
            for (uint32_t g = 0; g < NGROUPS; g++) {
                uint32_t prm[4]; params_of_group(g, Wl, prm);
                if (prm[3] && Wl < 4) continue;   /* modulo would be 1 */
                refinfo_t ri; reference(n, Wl, prm, exp, &ri);
                char what[112]; snprintf(what, sizeof what, "W=%u(%u) shape %ux%u g=%u p=(%u,%u,%u,%u)", W, Wl, shapes[s][0], shapes[s][1], g, prm[0], prm[1], prm[2], prm[3]);
                unsigned bad = compare(got + (size_t)g * n * 16, exp, n, ri.fill_ok, what);
                cases++; if (bad) fails++;
            }
        }
        free(spv);
    }
    for (unsigned wi = 0; wi < nw; wi++) { printf("INFO: width %u mismatching words by kind:", widths[wi]); unsigned any = 0; for (int k = 0; k < 16; k++) if (g_tot_k[wi][k]) { printf(" %s=%u", kname[k], g_tot_k[wi][k]); any = 1; } printf("%s\n", any ? "" : " none"); }
    printf("semantics: %u cases (%u widths incl. native, %u shapes), %u failed, %u skipped\n", cases, nw, nshapes, fails, skipped);
    int rc = 0;
    if (fails) { printf("FAIL: translated SIMD-group operations differ from the Metal reference in %u case(s)\n", fails); rc = 1; }
    else printf("PASS: simdgroup queries and simd_shuffle / down / up / xor / and_fill_down / and_fill_up (uint, ushort, half, uint2, ulong) equal the Metal reference for native and logical widths\n");

    /* ---- 3. an operation without an implementation must make a compute translation fail, never store an OpUndef ---- */
    {
        const char *unsup[] = { "simd_unsup_sum.air.bc", "simd_unsup_ballot.air.bc" }; unsigned bad_unsup = 0;
        for (unsigned i = 0; i < 2; i++) {
            uint8_t *spv; size_t sn; lagfx_status_t st = translate(unsup[i], 0, &spv, &sn);
            if (st == LAGFX_OK || spv) { printf("FAIL: %s: translation succeeded (st=%d) although the kernel uses a SIMD-group operation with no implementation (its result would be an OpUndef)\n", unsup[i], (int)st); rc = 1; bad_unsup++; }
            free(spv);
        }
        if (!bad_unsup) printf("PASS: SIMD-group operations without an implementation (air.simd_sum, air.simd_ballot) fail the compute translation\n");
    }
    if (g_val_errors) { printf("FAIL: %u Vulkan validation error(s)\n", g_val_errors); rc = 1; }
    vkDeviceWaitIdle(g_dev);
    free(got); free(exp);
    return rc;
}
