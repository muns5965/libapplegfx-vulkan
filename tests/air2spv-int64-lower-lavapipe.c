/* General Int64 lowering (SPIR-V uvec2 words) EXECUTED on a Vulkan device WITHOUT shaderInt64 (lavapipe, the S8 condition), against a CPU uint64_t model.
 * SPDX-License-Identifier: MIT
 *
 * Fixture tests/fixtures/int64_ops.air.bc (HAND-WRITTEN IR via llvm-as, NOT Apple output): per thread i = gid*64 + lid (the original `mul i64 gid, 64` form), a = i*C1, b = ((a ^ a>>29)*C2)+12345,
 * then 25 results: add sub mul shl lshr ashr and or xor not neg, 10 compares (eq ne ult ule ugt uge slt sle sgt sge), select, sext(i32), zext(i32), and a 4-iteration phi loop acc = acc*3 + a.
 * Checked: the module translated WITHOUT narrowing needs Int64, is lowered, no longer declares Int64, builds on a device without shaderInt64, and every 64-bit output word of 256 threads matches the model.
 * (Not tested: that the device refuses the unlowered module - lavapipe may not enforce it.) Exit 77 (skip) without a Vulkan device. */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/spv_lower_int64.h"
#include "air/bitcode_reader.h"
#include "protocol/compute_exec.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512]; const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
typedef struct { VkBuffer b; VkDeviceMemory m; uint32_t *p; } buf_t;
static int mkbuf(buf_t *o, VkDeviceSize n) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VK(vkCreateBuffer(g_dev, &ci, NULL, &o->b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, o->b, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->m)); VK(vkBindBufferMemory(g_dev, o->b, o->m, 0));
    void *p; VK(vkMapMemory(g_dev, o->m, 0, VK_WHOLE_SIZE, 0, &p)); o->p = p; memset(p, 0xEE, (size_t)n);
    return 0;
}

/* tg_len = 0: leave SpecId 3 at its default */
static int run(const uint8_t *spv, size_t spv_n, buf_t *out, uint32_t groups, uint32_t local, uint32_t tg_len) {
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    lagfx_spv_binding_t rf[8]; size_t nrf = lagfx_spv_reflect_bindings(spv, spv_n, rf, 8);
    uint32_t bind = ~0u; for (size_t i = 0; i < nrf; i++) if (rf[i].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER) { bind = rf[i].binding; break; }
    CHECK(bind != ~0u, "no storage buffer binding");
    VkDescriptorSetLayoutBinding lb = { bind, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    uint32_t sv[4] = { local, 1, 1, tg_len };
    VkSpecializationMapEntry sme[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
    VkSpecializationInfo si = { tg_len ? 4u : 3u, sme, sizeof sv, sv };
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main", .pSpecializationInfo = &si } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    VkDescriptorBufferInfo bi = { out->b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = bind, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(g_dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, groups, 1, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(g_q, 1, &sub, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    vkDestroyCommandPool(g_dev, cp, NULL); vkDestroyDescriptorPool(g_dev, dp, NULL); vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dl, NULL); vkDestroyShaderModule(g_dev, sm, NULL);
    return 0;
}

static int translate(const char *name, uint8_t **spv, size_t *n, lagfx_status_t *st_out) {
    size_t an = 0; uint8_t *air = slurp(name, &an); CHECK(air, "fixture %s not found", name);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK && m, "%s: module_open", name);
    *spv = NULL; *n = 0; *st_out = lagfx_air2spv_translate_module(m, spv, n);
    return 0;
}


static int has_cap_int64(const uint8_t *b, size_t n) {
    const uint32_t *w = (const uint32_t *)(const void *)b;
    for (size_t i = 5; i < n / 4;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; if (!wc) return 0; if (op == 17 && w[i + 1] == 11u) return 1; i += wc; }
    return 0;
}


/* every OpAccessChain / InBoundsAccessChain index that is not a constant must be defined by an OpSelect (the "high word is zero ? low word : 0xFFFFFFFF" guard), never by a bare low-word extract */
static int chain_indices_guarded(const uint8_t *b, size_t n, int *chains) {
    const uint32_t *w = (const uint32_t *)(const void *)b; uint32_t bound = w[3]; uint8_t *def = calloc(bound, 1); if (!def) return 0; int ok = 1; *chains = 0;
    for (size_t i = 5; i < n / 4;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; if (!wc) break;
        if (op >= 43 && op <= 52) { } else if (wc >= 3 && op != 62 && op != 5 && op != 71 && w[i + 2] < bound && op >= 61 && op != 249 && op != 250 && op != 247 && op != 248) def[w[i + 2]] = (uint8_t)(op == 169 ? 1 : 2);
        if (op == 65 || op == 66) { for (uint32_t k = 5; k < wc; k++) { (*chains)++; uint32_t id = w[i + k]; if (id < bound && def[id] == 2) ok = 0; } }
        i += wc; }
    free(def); return ok;
}

#define NOUT 25
static uint64_t ref(uint64_t i, int j) {
    uint64_t a = i * 0x9E3779B97F4A7C15ull, b = ((a ^ (a >> 29)) * 0xBF58476D1CE4E5B9ull) + 12345ull; uint32_t bt = (uint32_t)b; uint64_t amt = bt & 63u; int64_t sa = (int64_t)a, sb = (int64_t)b;
    switch (j) {
    case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return a << amt; case 4: return a >> amt; case 5: return (uint64_t)(sa >> amt);
    case 6: return a & b; case 7: return a | b; case 8: return a ^ b; case 9: return ~a; case 10: return 0 - a;
    case 11: return a == b; case 12: return a != b; case 13: return a < b; case 14: return a <= b; case 15: return a > b; case 16: return a >= b;
    case 17: return sa < sb; case 18: return sa <= sb; case 19: return sa > sb; case 20: return sa >= sb;
    case 21: return a < b ? a : b; case 22: return (uint64_t)(int64_t)(int32_t)bt; case 23: return (uint64_t)(uint32_t)a;
    default: { uint64_t acc = b; for (int k = 0; k < 4; k++) acc = acc * 3 + a; return acc; }
    }
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "air2spv-int64-lower", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("SKIP: no Vulkan 1.3 instance"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL); if (!nd) { puts("SKIP: no Vulkan device"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds); g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);
    float prio = 1.0f; uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    g_qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qf = i; break; }
    free(qp); CHECK(g_qf != ~0u, "no compute queue");
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures en = { .shaderInt64 = VK_FALSE };   /* the S8 condition */
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &en };
    VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev)); vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);

    lagfx_air2spv_set_narrow_int64(0);
    uint8_t *spv; size_t sn; lagfx_status_t st;
    if (translate("int64_ops.air.bc", &spv, &sn, &st)) return 1;
    CHECK(st == LAGFX_OK && spv, "translate (no narrowing) = %d", (int)st);
    CHECK(has_cap_int64(spv, sn), "the unlowered module must declare Int64 (otherwise this test proves nothing)");
    uint8_t *lo = NULL; size_t ln = 0; char why[160];
    int rc = lagfx_spv_lower_int64(spv, sn, &lo, &ln, why, sizeof why);
    CHECK(rc == 1 && lo, "lowering rc=%d (%s)", rc, why);
    CHECK(!has_cap_int64(lo, ln), "the lowered module must not declare Int64");
    {   FILE *f = fopen("/tmp/int64_lowered.spv", "wb"); if (f) { fwrite(lo, 1, ln, f); fclose(f); }
        if (system("command -v spirv-val >/dev/null 2>&1") == 0) CHECK(system("spirv-val --target-env vulkan1.2 /tmp/int64_lowered.spv") == 0, "spirv-val rejected the lowered module"); }

    { int nch = 0; CHECK(chain_indices_guarded(lo, ln, &nch) && nch > 0, "64-bit access-chain indices must be guarded against a non-zero high word (chains seen: %d)", nch);
      printf("PASS: %d access-chain indices are high-word guarded (not truncated to the low word)\n", nch); }
    buf_t o; if (mkbuf(&o, 256 * NOUT * 8)) return 1;
    CHECK(run(lo, ln, &o, 4, 64, 0) == 0, "lowered kernel must run on a device without shaderInt64");
    int bad = 0; const uint64_t *p = (const uint64_t *)(const void *)o.p;
    for (uint64_t i = 0; i < 256; i++) for (int j = 0; j < NOUT; j++) {
        uint64_t want = ref(i, j), got = p[i * NOUT + j];
        if (got != want && bad++ < 8) printf("  thread %llu result %d = 0x%llx, expected 0x%llx\n", (unsigned long long)i, j, (unsigned long long)got, (unsigned long long)want);
    }
    CHECK(bad == 0, "%d mismatching 64-bit results of %d", bad, 256 * NOUT);
    printf("PASS: general Int64 lowering: %d 64-bit results x 256 threads match the CPU uint64_t model on a device without shaderInt64\n", NOUT);

    /* the translator's own path: narrowing ON (device without shaderInt64) refuses these i64 products / variable shifts, then lowers instead */
    {   uint8_t *s4; size_t n4; lagfx_status_t st4; lagfx_air2spv_set_narrow_int64(1);
        if (translate("int64_ops.air.bc", &s4, &n4, &st4)) return 1;
        lagfx_air2spv_set_narrow_int64(0);
        CHECK(st4 == LAGFX_OK && s4 && !has_cap_int64(s4, n4), "narrowing mode must fall back to lowering: st=%d", (int)st4);
        buf_t o4; if (mkbuf(&o4, 256 * NOUT * 8)) return 1;
        CHECK(run(s4, n4, &o4, 4, 64, 0) == 0, "run (translator-lowered)");
        int bad4 = 0; const uint64_t *p4 = (const uint64_t *)(const void *)o4.p;
        for (uint64_t i = 0; i < 256; i++) for (int j = 0; j < NOUT; j++) if (p4[i * NOUT + j] != ref(i, j)) bad4++;
        CHECK(bad4 == 0, "translator narrowing->lowering path: %d mismatches", bad4);
        printf("PASS: narrowing refusal falls back to Int64 lowering inside lagfx_air2spv_translate_module (results match the CPU model)\n"); }
    /* i64 INPUT loads: read-modify-write of a pre-filled 64-bit array (load -> mul -> add -> store) */
    {   uint8_t *s5; size_t n5; lagfx_status_t st5; if (translate("int64_rmw.air.bc", &s5, &n5, &st5)) return 1;
        CHECK(st5 == LAGFX_OK && s5, "rmw translate %d", (int)st5);
        uint8_t *l5 = NULL; size_t ln5 = 0; CHECK(lagfx_spv_lower_int64(s5, n5, &l5, &ln5, why, sizeof why) == 1, "rmw lowering: %s", why);
        buf_t o5; if (mkbuf(&o5, 256 * 8)) return 1; uint64_t *q = (uint64_t *)(void *)o5.p;
        for (uint64_t i = 0; i < 256; i++) q[i] = 0xFEDCBA9876543210ull * (i + 1u) + (i << 33);
        CHECK(run(l5, ln5, &o5, 4, 64, 0) == 0, "rmw run");
        int bad5 = 0; for (uint64_t i = 0; i < 256; i++) { uint64_t in = 0xFEDCBA9876543210ull * (i + 1u) + (i << 33); if (q[i] != in * 3u + i) bad5++; }
        CHECK(bad5 == 0, "i64 load/store round trip: %d mismatches", bad5);
        printf("PASS: 64-bit values LOADED from a buffer (including high words) survive load -> arithmetic -> store\n"); }
    /* a module with no 64-bit integers is returned untouched (rc 0) */
    {   uint8_t *s2; size_t n2; lagfx_status_t st2; if (translate("threadgroup_arg_i32.air.bc", &s2, &n2, &st2)) return 1;
        lagfx_air2spv_set_narrow_int64(1); free(s2); s2 = NULL;
        if (translate("threadgroup_arg_i32.air.bc", &s2, &n2, &st2)) return 1;
        uint8_t *o2 = NULL; size_t l2 = 0; CHECK(st2 == LAGFX_OK && lagfx_spv_lower_int64(s2, n2, &o2, &l2, NULL, 0) == 0 && !o2, "a narrowed (no-i64) module must report rc 0");
        lagfx_air2spv_set_narrow_int64(0); }
    /* unsupported operations are refused, not emitted */
    {   uint8_t *s3; size_t n3; lagfx_status_t st3; if (translate("atomic_i64.air.bc", &s3, &n3, &st3)) return 1;
        if (st3 == LAGFX_OK && s3) { uint8_t *o3 = NULL; size_t l3 = 0; char w3[160];
            CHECK(lagfx_spv_lower_int64(s3, n3, &o3, &l3, w3, sizeof w3) == -1 && !o3, "64-bit atomics must be refused by the lowering");
            printf("PASS: 64-bit atomics refused by the lowering (%s)\n", w3); } }
    vkDestroyDevice(g_dev, NULL);
    return 0;
}
