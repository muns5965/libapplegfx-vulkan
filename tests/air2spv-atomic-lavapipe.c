/* AIR atomics (air.atomic.*), translated and EXECUTED on a Vulkan 1.3 device (lavapipe) against CPU references.
 * SPDX-License-Identifier: MIT
 *
 * Fixtures: tests/fixtures/atomic_*.air.bc from the sibling .ll files (gen-atomic-fixtures.sh, llvm-as; HAND-WRITTEN IR, NOT Apple output). Spelling status per
 * docs/spec-audit.md "Atomics": air.atomic.global.{load,store,xchg}.i32 and {add,and,or}.u.i32 are [V] (seen in Apple-compiled modules); sub / xor / min / max / add.s,
 * air.atomic.local.* and cmpxchg.weak are [N] (grammar extrapolation) - the translator accepts them by grammar, these tests prove the SPIR-V semantics, not Apple's spelling.
 *
 * What is checked (each against a CPU model of the Metal semantics):
 *   rmw      256 invocations, one atomic of every kind each: add / sub / and / or / xor, min / max (unsigned and signed), exchange (every written value observed exactly once),
 *            signed add of negative values, store then load, a fetch_add ticket (distinct old values 0..255).
 *   cas      the Metal CAS loop  { e = load(c); while (!compare_exchange_weak(c, &e, e + 1)) ; }  over 256 invocations: total = 256 (needs the write-back of the observed value).
 *   cas_wb   single invocation: a FAILING compare_exchange returns false and writes the observed value into *expected; a succeeding one returns true, stores desired, leaves *expected.
 *   neg      negative controls: the sub variant of the add kernel and the no-increment CAS variant must NOT reproduce the expected counters (the test has teeth).
 *   refuse   an unknown atomic spelling / unsupported operand / non-relaxed memory order fails the translation (never an undefined value, never a weakened order) and is counted.
 *   caps     32-bit kernels need no atomic capability bit; the 64-bit kernel needs LAGFX_VK_CAP_ATOMIC64 and runs when the device has it (shaderBuffer/SharedInt64Atomics); the float-add kernel needs
 *            LAGFX_VK_CAP_ATOMIC_F32_ADD (never enabled by lagfx_vk_init: the gate refuses it), and runs here only when the device offers VK_EXT_shader_atomic_float.
 * Exit 77 (skip) when there is no Vulkan device or it lacks shaderInt64.
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
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

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf; static uint32_t g_have_caps; static int g_run_f32;

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

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
typedef struct { VkBuffer b; VkDeviceMemory m; uint32_t *p; } buf_t;
static int mkbuf(buf_t *o, VkDeviceSize n) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(g_dev, &ci, NULL, &o->b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, o->b, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(g_dev, &ai, NULL, &o->m)); VK(vkBindBufferMemory(g_dev, o->b, o->m, 0));
    void *p; VK(vkMapMemory(g_dev, o->m, 0, VK_WHOLE_SIZE, 0, &p)); o->p = p;
    memset(p, 0, (size_t)n);
    return 0;
}

/* Translate fixture `name`; the SPIR-V is validated by the caller's tools (tests/ spirv-val runs in CI), here the device compiles it. */
static int translate(const char *name, uint8_t **spv, size_t *n, lagfx_air_module_t **m) {
    size_t an = 0; uint8_t *air = slurp(name, &an);
    CHECK(air, "fixture %s not found", name);
    CHECK(lagfx_air_module_open(air, an, m) == LAGFX_OK && *m, "%s: module_open", name);
    lagfx_status_t st = lagfx_air2spv_translate_module(*m, spv, n);
    CHECK(st == LAGFX_OK && *spv, "%s: translate = %d", name, (int)st);
    return 0;
}

/* Run a compute kernel `spv` with `nb` storage buffers (bufs[i] bound to the i-th reflected STORAGE_BUFFER binding, i.e. the AIR argument order), `groups` workgroups of `local`. */
static int run_kernel(const uint8_t *spv, size_t spv_n, buf_t *bufs, uint32_t nb, uint32_t groups, uint32_t local) {
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule sm; VK(vkCreateShaderModule(g_dev, &smi, NULL, &sm));
    lagfx_spv_binding_t rf[8]; size_t nrf = lagfx_spv_reflect_bindings(spv, spv_n, rf, 8);
    uint32_t bind[8]; uint32_t nsb = 0;
    for (size_t i = 0; i < nrf && i < 8; i++) if (rf[i].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER) bind[nsb++] = rf[i].binding;
    CHECK(nsb == nb, "kernel declares %u storage buffers, the test binds %u", nsb, nb);
    VkDescriptorSetLayoutBinding lb[8]; VkWriteDescriptorSet wr[8]; VkDescriptorBufferInfo bi[8];
    for (uint32_t i = 0; i < nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){ bind[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = lb };
    VkDescriptorSetLayout dl; VK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    uint32_t lsz[3] = { local, 1, 1 };
    VkSpecializationMapEntry sme[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
    VkSpecializationInfo si = { 3, sme, sizeof lsz, lsz };
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main", .pSpecializationInfo = &si } };
    VkPipeline pipe; VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nb };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(g_dev, &dai, &ds));
    for (uint32_t i = 0; i < nb; i++) {
        bi[i] = (VkDescriptorBufferInfo){ bufs[i].b, 0, VK_WHOLE_SIZE };
        wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = bind[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
    }
    vkUpdateDescriptorSets(g_dev, nb, wr, 0, NULL);
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
    VK(vkQueueSubmit(g_q, 1, &sub, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(g_q));
    vkDestroyCommandPool(g_dev, cp, NULL); vkDestroyDescriptorPool(g_dev, dp, NULL); vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dl, NULL); vkDestroyShaderModule(g_dev, sm, NULL);
    return 0;
}

#define N 256u
/* c[] initial values of the rmw kernels */
static void rmw_init(uint32_t *c) {
    memset(c, 0, 16 * 4);
    c[1] = 1000u; c[2] = 0xFFFFFFFFu; c[5] = 0xFFFFFFFFu; c[7] = 0x7FFFFFFFu; c[8] = 0x80000000u; c[9] = 0xABCDu;
}
/* CPU model of the rmw kernels over N invocations (any order: every operation here is commutative and associative, except exchange / ticket, checked separately) */
static void rmw_model(uint32_t *e, int word0_sub) {
    rmw_init(e);
    for (uint32_t t = 0; t < N; t++) {
        uint32_t bit = 1u << (t & 31u);
        e[0] = word0_sub ? e[0] - (t + 1u) : e[0] + (t + 1u);
        e[1] -= 1u; e[2] &= ~bit; e[3] |= bit; e[4] ^= (t + 1u);
        if (t + 7u < e[5]) e[5] = t + 7u;
        if (t + 7u > e[6]) e[6] = t + 7u;
        if ((int32_t)(t - 100u) < (int32_t)e[7]) e[7] = t - 100u;
        if ((int32_t)(t - 100u) > (int32_t)e[8]) e[8] = t - 100u;
        e[10] += (t - 100u);
    }
    e[11] = 4660u; e[14] = N;
}

static int check_rmw(const char *fx, int word0_sub, int *mismatch_out) {
    uint8_t *spv = NULL; size_t sn = 0; lagfx_air_module_t *m = NULL;
    if (translate(fx, &spv, &sn, &m)) return 1;
    buf_t c, o;
    if (mkbuf(&c, 64) || mkbuf(&o, 3 * N * 4)) return 1;
    rmw_init(c.p);
    buf_t bufs[2] = { c, o };
    if (run_kernel(spv, sn, bufs, 2, N / 64u, 64u)) return 1;
    uint32_t e[16]; rmw_model(e, word0_sub);
    int bad = 0;
    for (int i = 0; i < 15; i++) {
        if (i == 9) continue;   /* exchange: the final value is whichever invocation exchanged last */
        if (c.p[i] != e[i]) { bad++; printf("  %s: c[%d] = %u (0x%x), expected %u (0x%x)\n", fx, i, c.p[i], c.p[i], e[i], e[i]); }
    }
    /* exchange: initial value + every written value (t + 1) is observed exactly once, as an old value (o[t]) or the final value */
    {
        static uint8_t seen[N + 2]; memset(seen, 0, sizeof seen);
        uint32_t vals[N + 1]; for (uint32_t t = 0; t < N; t++) vals[t] = o.p[t]; vals[N] = c.p[9];
        for (uint32_t k = 0; k <= N; k++) {
            uint32_t v = vals[k], idx = v == 0xABCDu ? 0u : v;   /* 0 = initial, 1..N = written */
            if ((v != 0xABCDu && (v < 1u || v > N)) || seen[idx]) { bad++; printf("  %s: exchange value %u observed twice or never written\n", fx, v); break; }
            seen[idx] = 1;
        }
    }
    /* store then load: every invocation read 4660 */
    for (uint32_t t = 0; t < N; t++) if (o.p[N + t] != 4660u) { bad++; printf("  %s: o[%u] = %u, expected 4660 (atomic store, atomic load)\n", fx, N + t, o.p[N + t]); break; }
    /* ticket: the old values of fetch_add(1) are 0..N-1 each exactly once */
    {
        static uint8_t seen[N]; memset(seen, 0, sizeof seen);
        for (uint32_t t = 0; t < N; t++) { uint32_t v = o.p[2 * N + t]; if (v >= N || seen[v]) { bad++; printf("  %s: ticket %u duplicated / out of range\n", fx, v); break; } seen[v] = 1; }
    }
    if (mismatch_out) *mismatch_out = bad;
    free(spv); lagfx_air_module_free(m);
    return 0;
}

static int check_cas(const char *fx, uint32_t expect_total, int *mismatch) {
    uint8_t *spv = NULL; size_t sn = 0; lagfx_air_module_t *m = NULL;
    if (translate(fx, &spv, &sn, &m)) return 1;
    buf_t c; if (mkbuf(&c, 64)) return 1;
    buf_t bufs[1] = { c };
    if (run_kernel(spv, sn, bufs, 1, N / 64u, 64u)) return 1;
    *mismatch = c.p[0] != expect_total;
    if (*mismatch) printf("  %s: counter = %u, expected %u\n", fx, c.p[0], expect_total);
    free(spv); lagfx_air_module_free(m);
    return 0;
}

/* threadgroup atomics: 4 groups x 64 invocations (see the fixture's header in gen-atomic-fixtures.sh); returns the number of mismatching words */
static int check_tg(const char *fx, int w0_sub, int *bad_out) {
    uint8_t *spv = NULL; size_t sn = 0; lagfx_air_module_t *m = NULL;
    if (translate(fx, &spv, &sn, &m)) return 1;
    buf_t o; if (mkbuf(&o, 320 * 4)) return 1;
    buf_t bufs[1] = { o };
    if (run_kernel(spv, sn, bufs, 1, 4, 64)) return 1;
    uint32_t e[12] = { 0, 0, 0, 0, 0x7FFFFFFFu, 0xFFFFFFFFu, 0, 0, 0, 0, 5u, 0 };
    for (uint32_t l = 0; l < 64; l++) {
        uint32_t bit = 1u << (l & 31u);
        e[0] = w0_sub ? e[0] - (l + 1u) : e[0] + (l + 1u);
        e[1] |= bit; e[2] ^= (l + 1u);
        if (l + 7u > e[3]) e[3] = l + 7u;
        if ((int32_t)(l - 20u) < (int32_t)e[4]) e[4] = l - 20u;
        e[6] += 1u;
        e[8] += 2u; e[9] |= bit;
    }
    int bad = 0;
    for (uint32_t g = 0; g < 4; g++) {
        for (int k = 0; k < 12; k++) {
            uint32_t got = o.p[g * 16 + k];
            if (k == 10) { if (got < 1u || got > 64u) { bad++; printf("  %s: group %u a2 (xchg) = %u, not a written value\n", fx, g, got); } continue; }
            if (k == 5 || k == 7 || k == 11) { if (got != (k == 5 ? 0xFFFFFFFFu : 0u)) { bad++; printf("  %s: group %u word %d = 0x%x\n", fx, g, k, got); } continue; }
            if (got != e[k]) { bad++; printf("  %s: group %u word %d = 0x%x, expected 0x%x\n", fx, g, k, got, e[k]); }
        }
        uint8_t seen[64]; memset(seen, 0, sizeof seen);
        for (uint32_t l = 0; l < 64; l++) { uint32_t v = o.p[64 + g * 64 + l]; if (v >= 64u || seen[v]) { bad++; printf("  %s: group %u ticket %u duplicated / out of range\n", fx, g, v); break; } seen[v] = 1; }
    }
    *bad_out = bad;
    free(spv); lagfx_air_module_free(m);
    return 0;
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "air2spv-atomic-lavapipe", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) SKIP("no Vulkan 1.3 instance");
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) SKIP("no Vulkan device");
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT qaf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features q12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    int have_af_ext = 0;
    { uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL); VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
      for (uint32_t i = 0; i < ne; i++) { if (!strcmp(ep[i].extensionName, "VK_EXT_shader_atomic_float")) have_af_ext = 1; }
      free(ep); }
    q12.pNext = have_af_ext ? (void *)&qaf : NULL;
    VkPhysicalDeviceFeatures2 q2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q12 };
    vkGetPhysicalDeviceFeatures2(g_pd, &q2);
    if (!q2.features.shaderInt64) SKIP("device lacks shaderInt64");
    /* the capability bits this device offers for the atomics (the same rule lagfx_vk_init applies; ATOMIC_F32_ADD is never enabled there) */
    uint32_t have_caps = LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14);
    if (q12.shaderBufferInt64Atomics && q12.shaderSharedInt64Atomics) have_caps |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64);
    g_have_caps = have_caps;
    int run_f32 = have_af_ext && qaf.shaderBufferFloat32AtomicAdd; g_run_f32 = run_f32;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT eaf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, .shaderBufferFloat32AtomicAdd = (VkBool32)run_f32 };
    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .shaderBufferInt64Atomics = q12.shaderBufferInt64Atomics, .shaderSharedInt64Atomics = q12.shaderSharedInt64Atomics,
                                             .pNext = run_f32 ? (void *)&eaf : NULL };
    VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e12, .features = { .shaderInt64 = VK_TRUE } };
    const char *dev_exts[1] = { "VK_EXT_shader_atomic_float" };
    float prio = 1.0f;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    g_qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qf = i; break; }
    free(qp);
    CHECK(g_qf != ~0u, "no compute queue");
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = run_f32 ? 1u : 0u, .ppEnabledExtensionNames = run_f32 ? dev_exts : NULL };
    VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);

    int bad = 0;
    /* ---- rmw ---- */
    CHECK(!check_rmw("atomic_rmw.air.bc", 0, &bad), "rmw run");
    CHECK(bad == 0, "atomic_rmw: %d mismatching counters (Metal semantics: add/sub/and/or/xor/min/max/xchg/load/store)", bad);
    printf("PASS: atomic_rmw: 256 invocations x {add sub and or xor umin umax smin smax xchg add.s store load ticket} all match the CPU model\n");
    /* negative control: the kernel with SUB on word 0 must NOT match the ADD model, and must match the SUB model (the harness distinguishes the two) */
    {
        int b_add = 0, b_sub = 0;
        CHECK(!check_rmw("atomic_rmw_neg.air.bc", 0, &b_add), "neg run");
        CHECK(!check_rmw("atomic_rmw_neg.air.bc", 1, &b_sub), "neg run 2");
        CHECK(b_add > 0, "negative control: the sub kernel matched the ADD model (the test cannot tell add from sub)");
        CHECK(b_sub == 0, "negative control: the sub kernel does not match the SUB model (%d mismatches)", b_sub);
        printf("PASS: negative control: swapping add for sub breaks the add model (%d mismatching counter(s)) and matches the sub model\n", b_add);
    }
    /* ---- cas ---- */
    {
        int mm = 0;
        CHECK(!check_cas("atomic_cas.air.bc", N, &mm), "cas run");
        CHECK(!mm, "CAS-loop increment: lost or duplicated updates");
        printf("PASS: atomic_cas: 256 invocations spin-CAS increment -> counter = 256\n");
        CHECK(!check_cas("atomic_cas_neg.air.bc", N, &mm), "cas neg run");
        CHECK(mm, "negative control: the CAS kernel that stores the unchanged value reached 256");
        printf("PASS: negative control: a CAS loop without the increment does not reach 256\n");
    }
    /* ---- cas write-back / bool result ---- */
    {
        uint8_t *spv = NULL; size_t sn = 0; lagfx_air_module_t *m = NULL;
        CHECK(!translate("atomic_cas_wb.air.bc", &spv, &sn, &m), "cas_wb translate");
        buf_t c; CHECK(!mkbuf(&c, 64), "buf");
        c.p[0] = 5u;
        buf_t bufs[1] = { c };
        CHECK(!run_kernel(spv, sn, bufs, 1, 1, 1), "cas_wb run");
        /* c[0]=5; first exchange expects 7: fails -> c[1] = observed 5 (written into *expected), c[2] = 0 (false); second expects 5, desires 9: succeeds -> c[0] = 9, c[3] = 1,
         * *expected unchanged (5) -> c[4] */
        CHECK(c.p[1] == 5u && c.p[2] == 0u && c.p[0] == 9u && c.p[3] == 1u && c.p[4] == 5u,
              "cas_wb: expected-after-fail %u (want 5), fail result %u (want 0), memory %u (want 9), success result %u (want 1), expected-after-success %u (want 5)", c.p[1], c.p[2], c.p[0], c.p[3], c.p[4]);
        printf("PASS: atomic_cas_wb: a failed compare_exchange writes the observed value back and returns false; a successful one stores desired and returns true\n");
        free(spv); lagfx_air_module_free(m);
    }
    /* ---- threadgroup (Workgroup storage class) ---- */
    {
        int b0 = 0, bn = 0, bs = 0;
        CHECK(!check_tg("atomic_tg.air.bc", 0, &b0), "tg run");
        CHECK(b0 == 0, "atomic_tg: %d mismatching words", b0);
        printf("PASS: atomic_tg: threadgroup atomics (add or xor umax smin and xchg load store, a [4 x atomic_uint] static, local ticket) in 4 groups match the CPU model\n");
        CHECK(!check_tg("atomic_tg_neg.air.bc", 0, &bn) && !check_tg("atomic_tg_neg.air.bc", 1, &bs), "tg neg run");
        CHECK(bn > 0 && bs == 0, "negative control (threadgroup): the sub kernel must break the add model (%d) and match the sub model (%d)", bn, bs);
        printf("PASS: negative control: local.sub instead of local.add breaks the threadgroup add model (%d mismatching word(s)) and matches the sub model\n", bn);
    }
    /* ---- capability bits ---- */
    {
        uint8_t *spv = NULL; size_t sn = 0; lagfx_air_module_t *m = NULL; uint32_t need = 0;
        CHECK(!translate("atomic_rmw.air.bc", &spv, &sn, &m) && lagfx_spv_required_caps_ex(spv, sn, &need), "caps scan rmw");
        CHECK(!(need & (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_ADD))), "32-bit integer atomics must need no atomic capability bit (need 0x%x)", need);
        free(spv); lagfx_air_module_free(m); spv = NULL;
        CHECK(!translate("atomic_i64.air.bc", &spv, &sn, &m) && lagfx_spv_required_caps_ex(spv, sn, &need), "caps scan i64");
        CHECK(need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64), "64-bit atomics must require LAGFX_VK_CAP_ATOMIC64 (need 0x%x)", need);
        CHECK(!(need & ~(g_have_caps | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64))), "unexpected extra capability bits 0x%x", need);
        const uint32_t lacking = need & ~(g_have_caps & ~LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64));   /* a device without Int64 atomics: the gate (need & ~enabled) must fire */
        CHECK(lacking & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64), "the capability gate would not refuse the 64-bit atomics on a device without them");
        printf("PASS: caps: 32-bit atomics need no atomic cap bit; the 64-bit kernel requires atomic64 (the gate refuses it on a device without Int64 atomics)\n");
        if (g_have_caps & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64)) {
            buf_t c; CHECK(!mkbuf(&c, 64), "buf");
            uint64_t *c64 = (uint64_t *)c.p;
            buf_t bufs[1] = { c };
            c64[1] = ~0ull;
            CHECK(!run_kernel(spv, sn, bufs, 1, N / 64u, 64u), "i64 run");
            uint64_t e0 = 0, e1 = ~0ull;
            for (uint64_t t = 0; t < N; t++) { if (t * 4294967297ull > e0) e0 = t * 4294967297ull; uint64_t v = (t + 5u) * 4294967297ull + 7u; if (v < e1) e1 = v; }
            CHECK(c64[0] == e0 && c64[1] == e1, "i64 atomics: umax %llx (want %llx), umin %llx (want %llx)", (unsigned long long)c64[0], (unsigned long long)e0, (unsigned long long)c64[1], (unsigned long long)e1);
            printf("PASS: atomic_i64: 64-bit umax / umin over 256 invocations match the CPU model (Int64Atomics)\n");
        } else printf("note: device has no Int64 atomics: the 64-bit kernel was not run\n");
        free(spv); lagfx_air_module_free(m); spv = NULL;
        CHECK(!translate("atomic_f32.air.bc", &spv, &sn, &m) && lagfx_spv_required_caps_ex(spv, sn, &need), "caps scan f32");
        CHECK(need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_ADD), "float atomic add must require LAGFX_VK_CAP_ATOMIC_F32_ADD (need 0x%x)", need);
        printf("PASS: caps: the float atomic add requires atomic_f32_add (not enabled by lagfx_vk_init, so the gate refuses it; never emulated)\n");
        if (g_run_f32) {
            buf_t c; CHECK(!mkbuf(&c, 64), "buf");
            float *cf = (float *)c.p; buf_t bufs[1] = { c };
            CHECK(!run_kernel(spv, sn, bufs, 1, N / 64u, 64u), "f32 run");
            CHECK(cf[0] == (float)N, "float atomic add: %g, want %u", cf[0], N);
            printf("PASS: atomic_f32: 256 float atomic adds of 1.0 = 256.0 (VK_EXT_shader_atomic_float)\n");
        } else printf("note: device has no VK_EXT_shader_atomic_float: the float kernel was not run\n");
        free(spv); lagfx_air_module_free(m);
    }
    /* ---- refusal ---- */
    {
        char first[96] = ""; lagfx_atomic_totals_t t0, t1;
        lagfx_air2spv_atomic_totals(&t0);
        size_t an = 0; uint8_t *air = slurp("atomic_unknown.air.bc", &an);
        CHECK(air, "fixture atomic_unknown.air.bc not found");
        lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sn = 0;
        CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK, "open");
        lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sn);
        CHECK(st != LAGFX_OK && !spv, "a kernel with an unimplemented atomic (air.atomic.fence) must not translate (st=%d)", (int)st);
        uint32_t n = lagfx_air2spv_last_unsupported_atomic(first, sizeof first);
        lagfx_air2spv_atomic_totals(&t1);
        CHECK(n >= 1u && strstr(first, "air.atomic.") && t1.refused > t0.refused && t1.modules_rejected > t0.modules_rejected, "refusal not counted (n=%u first='%s')", n, first);
        printf("PASS: refusal: an unimplemented atomic ('%s') fails the translation and is counted (%u call(s), %llu rejected module(s))\n", first, n, (unsigned long long)(t1.modules_rejected - t0.modules_rejected));
        lagfx_air_module_free(m); free(air);
    }
    {   /* a non-relaxed memory order is refused, never weakened */
        size_t an = 0; uint8_t *air = slurp("atomic_order.air.bc", &an);
        CHECK(air, "fixture atomic_order.air.bc not found");
        lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sn = 0; char first[96] = "";
        CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK, "open");
        lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sn);
        CHECK(st != LAGFX_OK && !spv && lagfx_air2spv_last_unsupported_atomic(first, sizeof first) == 1u, "an atomic with memory order operand 2 must be refused (st=%d)", (int)st);
        printf("PASS: refusal: a non-relaxed memory order operand ('%s') fails the translation instead of being weakened to relaxed\n", first);
        lagfx_air_module_free(m); free(air);
    }
    vkDeviceWaitIdle(g_dev);
    printf("PASS air2spv-atomic-lavapipe\n");
    return 0;
}
