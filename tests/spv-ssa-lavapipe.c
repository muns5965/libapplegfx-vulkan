/*
 * libapplegfx-vulkan — opt-in SSA promotion of translated SPIR-V (LAGFX_SPV_SSA): structure checks, spirv-val and a NUMERIC comparison against the unreduced shader on lavapipe
 * tests/spv-ssa-lavapipe.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Part 1 (no GPU): default OFF (env unset: nothing touched, byte-identical), counters, determinism, idempotence, structural invariants (no promotable local left, the escaped
 *   ones stay), spirv-val when the tool exists, a module with an unknown opcode is left alone, a module without locals is "unchanged".
 * Part 2 (Vulkan 1.3 device, lavapipe; skip 77 without one): every shader is run unreduced and promoted (modes 1..4) with identical inputs, outputs compared BIT FOR BIT:
 *   - tests/fixtures/ssa_stress.comp (compute; loops, switch fall-through, partial definitions, escaping local, early return) over 256 invocations x 3 input seeds;
 *   - translated fixtures with Function variables run as fragment shaders on a 16x16 target: i1const_fragment, dump_f16_fmax(3)_frag, login_tvcmxc_isrc_frag and the two large
 *     translated shaders uber_a2xghfc_231501 (the A2Xghfc of the 20261126-231501 dump, 3135 locals / 473 blocks in the entry) and login_xgc_frag. Their resources are derived
 *     from the module (SSBOs with a pseudo-random pattern, sampled images with random texels, samplers, input attachments on the colour targets); the vertex stage is generated
 *     here and feeds every Location with a value that varies across the triangle;
 *   - NEGATIVE CONTROL: the promoted compute module with the two incoming values of one OpPhi swapped (a valid module that computes something else) must produce DIFFERENT
 *     output, and a stage with the unreduced shader vs itself must be identical (determinism); a harness that cannot tell the difference would fail here.
 * A fixture whose pipeline lavapipe refuses is reported as NOT RUN (never as a pass).
 */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"
#include "vulkan/descriptor_layout.h"
#include "vulkan/spv_ssa.h"
#include "vulkan/spv_reduce.h"
#include "spv-ssa-stress.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_notrun;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

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
static uint32_t *translate_fixture(const char *name, size_t *words) {
    size_t bn = 0;
    uint8_t *bc = slurp(name, &bn);
    if (!bc) return NULL;
    lagfx_air_module_t *m = NULL;
    uint8_t *spv = NULL;
    size_t sz = 0;
    if (lagfx_air_module_open(bc, bn, &m) != LAGFX_OK || !m || lagfx_air2spv_translate_module(m, &spv, &sz) != LAGFX_OK || !spv) { free(bc); return NULL; }
    lagfx_air_module_free(m);
    free(bc);
    uint32_t *w = malloc(sz);
    memcpy(w, spv, sz);
    free(spv);
    *words = sz / 4;
    return w;
}

/* ---------- tiny SPIR-V readers ---------- */
static uint32_t count_op_in_fn(const uint32_t *w, size_t n, uint32_t opc, uint32_t extra_pos, uint32_t extra_val) {
    uint32_t c = 0;
    int infn = 0;
    for (size_t p = 5; p < n;) {
        uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
        if (!wc) break;
        if (op == 54) infn = 1;
        if (infn && op == opc && (!extra_pos || w[p + extra_pos] == extra_val)) c++;
        if (op == 56) infn = 0;
        p += wc;
    }
    return c;
}
static int have_spirv_val(void) { static int v = -1; if (v < 0) v = system("spirv-val --version >/dev/null 2>&1") == 0; return v; }
static int spirv_val(const uint32_t *w, size_t n, const char *tag) {
    if (!have_spirv_val()) return -1;
    char path[256], cmd[400];
    snprintf(path, sizeof path, "/tmp/spv-ssa-%d-%s.spv", (int)getpid(), tag);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(w, 4, n, f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.2 %s >/dev/null 2>&1", path);
    int r = system(cmd);
    remove(path);
    return r == 0 ? 1 : 0;
}

/* ---------- part 1 ---------- */
static int run_modes(const char *name, const uint32_t *w, size_t n, int expect_change) {
    lagfx_ssa_stats_t st[5];
    for (unsigned mode = 1; mode <= 4; mode++) {
        unsigned fl = mode == 1 ? 0u : mode == 2 ? LSSA_DCE : mode == 3 ? (LSSA_DCE | LSSA_DCE_INPUT) : (LSSA_DCE | LSSA_AGG);
        uint32_t *o = NULL, *o2 = NULL;
        size_t on = 0, on2 = 0;
        char why[200];
        int r = lagfx_spv_ssa_promote(w, n, fl, &o, &on, &st[mode], why, sizeof why);
        if (!expect_change) { CHECK(r == 0, "%s mode %u: expected 'nothing to do', got %d", name, mode, r); free(o); continue; }
        CHECK(r == 1, "%s mode %u: promote returned %d (%s)", name, mode, r, why);
        if (r != 1) continue;
        char sc[160];
        CHECK(lagfx_red_selfcheck(o, on, sc, sizeof sc), "%s mode %u: self-check: %s", name, mode, sc);
        int vv = spirv_val(o, on, "m");
        CHECK(vv != 0, "%s mode %u: spirv-val rejects the promoted module", name, mode);
        CHECK(st[mode].promoted > 0 && st[mode].loads_removed + st[mode].stores_removed > 0, "%s mode %u: nothing promoted", name, mode);
        /* determinism: same input, same bytes */
        r = lagfx_spv_ssa_promote(w, n, fl, &o2, &on2, NULL, NULL, 0);
        CHECK(r == 1 && on2 == on && !memcmp(o, o2, on * 4), "%s mode %u: output is not deterministic", name, mode);
        free(o2); o2 = NULL;
        /* idempotence: a second run promotes nothing more (every promotable local is gone) */
        lagfx_ssa_stats_t st2;
        r = lagfx_spv_ssa_promote(o, on, fl, &o2, &on2, &st2, why, sizeof why);
        CHECK(r != 1 || st2.promoted == 0, "%s mode %u: second run still promotes %u variables", name, mode, st2.promoted);
        free(o2);
        /* the result still has exactly the locals that escape (+ aggregates in modes 1..3) */
        uint32_t left = count_op_in_fn(o, on, 59, 3, 7);
        CHECK(left <= st[mode].vars - st[mode].promoted + 0u, "%s mode %u: %u locals left, expected at most %u", name, mode, left, st[mode].vars - st[mode].promoted);
        printf("  %-28s mode %u: locals %u -> %u (escaped %u, aggregates skipped %u) phis %u loads -%u stores -%u dce -%u inst %u -> %u%s\n", name, mode, st[mode].vars, left, st[mode].escaped, st[mode].skipped_agg,
               st[mode].phis_kept, st[mode].loads_removed, st[mode].stores_removed, st[mode].dce_removed, st[mode].inst_before, st[mode].inst_after, vv < 0 ? " (spirv-val not available)" : "");
        free(o);
    }
    return 0;
}

static void part1(void) {
    printf("-- part 1: structure --\n");
    /* default OFF: env unset -> nothing happens, byte-identical */
    unsetenv("LAGFX_SPV_SSA");
    CHECK(lagfx_spv_ssa_env_mode() == 0, "mode must be 0 without the variable");
    size_t uw = 0;
    uint32_t *uber = translate_fixture("uber_a2xghfc_231501.air.bc", &uw);
    CHECK(uber != NULL, "fixture uber_a2xghfc_231501 did not translate");
    if (uber) {
        uint8_t *buf = malloc(uw * 4), *keep = malloc(uw * 4);
        memcpy(buf, uber, uw * 4), memcpy(keep, uber, uw * 4);
        size_t sz = uw * 4;
        lagfx_ssa_counters_t c0, c1;
        lagfx_spv_ssa_counters(&c0);
        CHECK(lagfx_spv_ssa_apply_env(&buf, &sz, "off") == 0 && sz == uw * 4 && !memcmp(buf, keep, sz), "env unset: the hook changed the module");
        lagfx_spv_ssa_counters(&c1);
        CHECK(!memcmp(&c0, &c1, sizeof c0), "env unset: counters moved");
        setenv("LAGFX_SPV_SSA", "0", 1);
        CHECK(lagfx_spv_ssa_apply_env(&buf, &sz, "zero") == 0 && !memcmp(buf, keep, sz), "LAGFX_SPV_SSA=0 changed the module");
        setenv("LAGFX_SPV_SSA", "3", 1);
        CHECK(lagfx_spv_ssa_env_mode() == 3, "mode 3");
        CHECK(lagfx_spv_ssa_apply_env(&buf, &sz, "uber") == 1 && sz < uw * 4 * 4 && sz != uw * 4, "LAGFX_SPV_SSA=3 did not replace the module");
        lagfx_spv_ssa_counters(&c1);
        CHECK(c1.modules == 1 && c1.changed == 1 && c1.promoted > 3000 && c1.loads_removed > 1000, "counters after one module: modules=%llu changed=%llu promoted=%llu", (unsigned long long)c1.modules, (unsigned long long)c1.changed, (unsigned long long)c1.promoted);
        CHECK(spirv_val((const uint32_t *)(const void *)buf, sz / 4, "hook") != 0, "hook output is not valid SPIR-V");
        unsetenv("LAGFX_SPV_SSA");
        free(buf), free(keep);
        run_modes("uber_a2xghfc_231501", uber, uw, 1);
        {   /* the wide-phi veto: locals that would need a phi with more than 8 incoming edges stay in memory (the 467-way merges of the entry's big switch) */
            uint32_t *o = NULL;
            size_t on = 0;
            lagfx_ssa_stats_t st;
            int r = lagfx_spv_ssa_promote(uber, uw, LSSA_DCE | LSSA_MAXPHI(8), &o, &on, &st, NULL, 0);
            CHECK(r == 1 && st.skipped_wide > 100 && st.phis_kept < 200, "max-phi 8: rc=%d wide-skipped=%u phis=%u", r, st.skipped_wide, st.phis_kept);
            CHECK(r == 1 && spirv_val(o, on, "w8") != 0, "max-phi 8 output is not valid SPIR-V");
            if (r == 1) printf("  uber_a2xghfc_231501          max-phi 8: promoted %u, kept in memory because of wide phis %u, phis %u, inst %u -> %u, bytes %u -> %u\n", st.promoted, st.skipped_wide, st.phis_kept, st.inst_before, st.inst_after, st.words_before * 4, st.words_after * 4);
            free(o);
        }
        free(uber);
    }
    static const char *fx[] = { "login_xgc_frag", "i1const_fragment", "dump_f16_fmax_frag", "dump_f16_fmax3_frag", "login_tvcmxc_isrc_frag", "vfxxgh_231501", "login_vfxxgb_vert", "dump_unpack_unorm4x8_vert" };
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        char fn[128];
        snprintf(fn, sizeof fn, "%s.air.bc", fx[i]);
        size_t n = 0;
        uint32_t *w = translate_fixture(fn, &n);
        CHECK(w != NULL, "fixture %s did not translate", fx[i]);
        if (w) { run_modes(fx[i], w, n, 1); free(w); }
    }
    {   /* a module without Function variables: nothing to do */
        size_t n = 0;
        uint32_t *w = translate_fixture("branch_fragment.air.bc", &n);
        if (w) { uint32_t nl = count_op_in_fn(w, n, 59, 3, 7); if (!nl) run_modes("branch_fragment (no locals)", w, n, 0); free(w); }
    }
    {   /* the stress shader (glslang -Od) */
        run_modes("ssa_stress.comp", SSA_STRESS_CS, sizeof SSA_STRESS_CS / 4, 1);
    }
    {   /* an opcode this pass does not know in a function body: left alone */
        size_t n = sizeof SSA_STRESS_CS / 4;
        uint32_t *w = malloc(n * 4);
        memcpy(w, SSA_STRESS_CS, n * 4);
        int done = 0;
        int infn = 0;
        for (size_t p = 5; p < n && !done;) {
            uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
            if (op == 54) infn = 1;
            if (infn && op == 133 /* OpFMul */) { w[p] = (wc << 16) | 5400u; done = 1; }   /* an opcode in the vendor range */
            p += wc;
        }
        uint32_t *o = NULL;
        size_t on = 0;
        char why[200];
        int r = lagfx_spv_ssa_promote(w, n, LSSA_DCE, &o, &on, NULL, why, sizeof why);
        CHECK(done && r == 0 && o == NULL, "unknown opcode: expected the module to be left alone (rc=%d) %s", r, why);
        free(w);
    }
}

/* ======================= Vulkan harness ======================= */
/* the installed Vulkan headers may predate VK_KHR_dynamic_rendering_local_read: the few names needed are spelled out (the library does the same, src/vulkan/devinfo.c) */
#define LR_EXT_NAME "VK_KHR_dynamic_rendering_local_read"
#define LR_FEATURES_STYPE ((VkStructureType)1000232000)
#define LR_LAYOUT ((VkImageLayout)1000232000)
typedef struct { VkStructureType sType; void *pNext; VkBool32 dynamicRenderingLocalRead; } lr_features_t;
typedef struct { VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; VkCommandPool pool; VkPhysicalDeviceMemoryProperties mp; int local_read; } vk_t;

#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); g_fail++; return -1; } } while (0)

static int vk_init(vk_t *v) {
    memset(v, 0, sizeof *v);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "spv-ssa", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    if (vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) return 77;
    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(v->inst, &nd, NULL);
    if (!nd) return 77;
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds);
    vkEnumeratePhysicalDevices(v->inst, &nd, pds);
    v->pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { v->pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(v->pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3) return 77;
    printf("device: %s (Vulkan %u.%u)\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion));
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = malloc(ne * sizeof *ep);
    vkEnumerateDeviceExtensionProperties(v->pd, NULL, &ne, ep);
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, LR_EXT_NAME)) v->local_read = 1;
    free(ep);
    lr_features_t flr = { .sType = LR_FEATURES_STYPE };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = v->local_read ? &flr : NULL };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    vkGetPhysicalDeviceFeatures2(v->pd, &f2);   /* enable everything the device supports: the translated modules use int8/16/64, float16, 16/8-bit storage, variable pointers */
    v->local_read = v->local_read && flr.dynamicRenderingLocalRead;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *exts[] = { LR_EXT_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .enabledExtensionCount = v->local_read ? 1u : 0u, .ppEnabledExtensionNames = exts };
    VKC(vkCreateDevice(v->pd, &dci, NULL, &v->dev));
    vkGetDeviceQueue(v->dev, 0, 0, &v->q);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VKC(vkCreateCommandPool(v->dev, &cpi, NULL, &v->pool));
    vkGetPhysicalDeviceMemoryProperties(v->pd, &v->mp);
    return 0;
}

static uint32_t memtype(const vk_t *v, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (v->mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; size_t size; } buf_t;
static int mkbuf(vk_t *v, buf_t *b, size_t size, VkBufferUsageFlags use) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use };
    VKC(vkCreateBuffer(v->dev, &ci, NULL, &b->b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(v->dev, b->b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VKC(vkAllocateMemory(v->dev, &mai, NULL, &b->m));
    VKC(vkBindBufferMemory(v->dev, b->b, b->m, 0));
    VKC(vkMapMemory(v->dev, b->m, 0, VK_WHOLE_SIZE, 0, (void **)&b->p));
    b->size = size;
    return 0;
}
static void rmbuf(vk_t *v, buf_t *b) { if (b->b) { vkUnmapMemory(v->dev, b->m); vkDestroyBuffer(v->dev, b->b, NULL); vkFreeMemory(v->dev, b->m, NULL); } memset(b, 0, sizeof *b); }

static uint32_t rng(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static void fill_pattern(uint8_t *p, size_t n, uint32_t seed) {   /* 3/4 floats in [0.25, 2), 1/4 small integers: both readings of a word stay sane */
    uint32_t s = seed * 2654435761u + 12345u;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t r = rng(&s), w;
        if ((r & 3u) == 0) w = (r >> 8) & 0xffu;
        else { float f = 0.25f + 1.75f * (float)((r >> 8) & 0xffffu) / 65536.0f; memcpy(&w, &f, 4); }
        memcpy(p + i, &w, 4);
    }
}

static int submit_wait(vk_t *v, VkCommandBuffer cmd) {
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VKC(vkQueueSubmit(v->q, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(v->q));
    return 0;
}

/* ---------- compute ---------- */
static int run_cs(vk_t *v, const uint32_t *spv, size_t nw, uint32_t seed, uint32_t *out /* 256 words */) {
    buf_t in = { 0 }, ob = { 0 };
    if (mkbuf(v, &in, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || mkbuf(v, &ob, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return -1;
    uint32_t s = seed;
    for (int i = 0; i < 256; i++) { uint32_t r = rng(&s); memcpy(in.p + 4 * i, &r, 4); }
    memset(ob.p, 0, 4096);
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
    VkDescriptorSetLayout dsl;
    VKC(vkCreateDescriptorSetLayout(v->dev, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl;
    VKC(vkCreatePipelineLayout(v->dev, &pli, NULL, &pl));
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv };
    VkShaderModule sm;
    VKC(vkCreateShaderModule(v->dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = pl };
    VkPipeline pipe;
    VKC(vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    VKC(vkCreateDescriptorPool(v->dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    VKC(vkAllocateDescriptorSets(v->dev, &dai, &ds));
    VkDescriptorBufferInfo bi[2] = { { in.b, 0, VK_WHOLE_SIZE }, { ob.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] } };
    vkUpdateDescriptorSets(v->dev, 2, wr, 0, NULL);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = v->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    VKC(vkAllocateCommandBuffers(v->dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VKC(vkBeginCommandBuffer(cmd, &cbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 4, 1, 1);
    VKC(vkEndCommandBuffer(cmd));
    int rc = submit_wait(v, cmd);
    if (!rc) memcpy(out, ob.p, 1024);
    vkFreeCommandBuffers(v->dev, v->pool, 1, &cmd);
    vkDestroyDescriptorPool(v->dev, dp, NULL), vkDestroyPipeline(v->dev, pipe, NULL), vkDestroyShaderModule(v->dev, sm, NULL), vkDestroyPipelineLayout(v->dev, pl, NULL), vkDestroyDescriptorSetLayout(v->dev, dsl, NULL);
    rmbuf(v, &in), rmbuf(v, &ob);
    return rc;
}

/* ---------- fragment: reflection, generated vertex stage ---------- */
typedef struct { uint32_t loc, kind /* 0 f 1 s 2 u */, ncomp, flat; } fsin_t;
typedef struct { uint32_t loc, kind, ncomp; } fsout_t;
typedef struct { uint32_t binding, dim, depth, arrayed, ms, sampled, skind; } fsimg_t;
typedef struct {
    fsin_t in[48]; uint32_t nin;
    fsout_t out[8]; uint32_t nout, maxloc;
    fsimg_t img[40]; uint32_t nimg;
    uint32_t ia_binding[8], ia_index[8], nia;
} fsinfo_t;

static int reflect_fs(const uint32_t *w, size_t n, fsinfo_t *f) {
    memset(f, 0, sizeof *f);
    uint32_t bound = w[3];
    uint32_t *loc = calloc(bound, 4), *bind = calloc(bound, 4), *flat = calloc(bound, 4), *iaidx = calloc(bound, 4), *builtin = calloc(bound, 4), *opof = calloc(bound, 4);
    uint32_t *a1 = calloc(bound, 4), *a2 = calloc(bound, 4), *a3 = calloc(bound, 4), *ty = calloc(bound, 4);
    uint8_t *hasloc = calloc(bound, 1), *hasbind = calloc(bound, 1), *hasia = calloc(bound, 1);
    uint32_t *img = calloc(bound * 7, 4);
    for (size_t p = 5; p < n;) {
        uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
        if (op == 71 && wc >= 4) {
            uint32_t id = w[p + 1], d = w[p + 2];
            if (d == 30) loc[id] = w[p + 3], hasloc[id] = 1; else if (d == 33) bind[id] = w[p + 3], hasbind[id] = 1; else if (d == 43) iaidx[id] = w[p + 3], hasia[id] = 1; else if (d == 11) builtin[id] = 1;
        } else if (op == 71 && wc == 3 && w[p + 2] == 14) flat[w[p + 1]] = 1;
        int rp = (op == 59 || (op >= 19 && op <= 39 && op != 19)) ? 0 : 0;
        (void)rp;
        if (op >= 19 && op <= 39 && wc >= 2) { opof[w[p + 1]] = op; if (wc >= 3) a1[w[p + 1]] = w[p + 2]; if (wc >= 4) a2[w[p + 1]] = w[p + 3]; if (wc >= 5) a3[w[p + 1]] = w[p + 4]; }
        if (op == 25 && wc >= 9) { uint32_t id = w[p + 1]; img[id * 7 + 0] = w[p + 2]; img[id * 7 + 1] = w[p + 3]; img[id * 7 + 2] = w[p + 4]; img[id * 7 + 3] = w[p + 5]; img[id * 7 + 4] = w[p + 6]; img[id * 7 + 5] = w[p + 7]; }
        if (op == 59 && wc >= 4) { ty[w[p + 2]] = w[p + 1]; }
        p += wc;
    }
    uint32_t *mem = calloc(bound, 4), *memk = calloc(bound, 4);   /* pointer type id -> pointee */
    for (size_t p = 5; p < n;) { uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16; if (op == 32 && wc >= 4) mem[w[p + 1]] = w[p + 3]; p += wc; }
    (void)memk;
    for (size_t p = 5; p < n;) {
        uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
        if (op == 59 && wc >= 4) {
            uint32_t id = w[p + 2], sc = w[p + 3], pt = mem[w[p + 1]];
            uint32_t base = pt, nc = 1;
            if (opof[pt] == 23) base = a1[pt], nc = a2[pt];
            uint32_t kind = (opof[base] == 22 && a1[base] == 32) ? 0 : (opof[base] == 21 && a1[base] == 32 ? (a2[base] ? 1u : 2u) : 9u);
            if (sc == 1 && hasloc[id] && !builtin[id] && f->nin < 48) f->in[f->nin++] = (fsin_t){ loc[id], kind, nc, flat[id] || kind != 0 };
            else if (sc == 3 && hasloc[id] && f->nout < 8) { f->out[f->nout++] = (fsout_t){ loc[id], kind, nc }; if (loc[id] > f->maxloc) f->maxloc = loc[id]; }
            else if (hasbind[id] && opof[pt] == 25 && f->nimg < 40) {
                uint32_t b = bind[id];
                if (hasia[id]) { if (f->nia < 8) f->ia_binding[f->nia] = b, f->ia_index[f->nia++] = iaidx[id]; }
                else f->img[f->nimg++] = (fsimg_t){ b, img[pt * 7 + 1], img[pt * 7 + 2], img[pt * 7 + 3], img[pt * 7 + 4], img[pt * 7 + 5], opof[img[pt * 7 + 0]] == 22 ? 0u : (a2[img[pt * 7 + 0]] ? 1u : 2u) };
            }
        }
        p += wc;
    }
    free(loc), free(bind), free(flat), free(iaidx), free(builtin), free(opof), free(a1), free(a2), free(a3), free(ty), free(hasloc), free(hasbind), free(hasia), free(img), free(mem), free(memk);
    return 1;
}

typedef struct { uint32_t *w; size_t n, cap; } wb_t;
static void wp(wb_t *b, uint32_t x) { if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->w = realloc(b->w, b->cap * 4); } b->w[b->n++] = x; }
static void wi(wb_t *b, uint32_t op, const uint32_t *o, uint32_t n) { wp(b, ((n + 1) << 16) | op); for (uint32_t i = 0; i < n; i++) wp(b, o[i]); }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* vertex stage: full-screen triangle (gl_VertexIndex 0..2) and, for every FS input Location, a value that varies from vertex to vertex (flat integers: constants) */
static uint32_t *build_vs(const fsinfo_t *f, size_t *nout) {
    wb_t ty = { 0 }, an = { 0 }, fn = { 0 }, glob = { 0 };
    uint32_t id = 1;
    uint32_t t_void = id++, t_fn = id++, t_f32 = id++, t_i32 = id++, t_u32 = id++, t_v4 = id++, p_out4 = id++, p_in_i32 = id++, v_pos = id++, v_vi = id++, c_0 = id++, c_1 = id++, c_4 = id++, c_m1 = id++, c_i1 = id++, c_i2 = id++, c_i0 = id++, main_id = id++;
    uint32_t tmp[16];
    tmp[0] = t_void; wi(&ty, 19, tmp, 1);
    tmp[0] = t_fn, tmp[1] = t_void; wi(&ty, 33, tmp, 2);
    tmp[0] = t_f32, tmp[1] = 32; wi(&ty, 22, tmp, 2);
    tmp[0] = t_i32, tmp[1] = 32, tmp[2] = 1; wi(&ty, 21, tmp, 3);
    tmp[0] = t_u32, tmp[1] = 32, tmp[2] = 0; wi(&ty, 21, tmp, 3);
    tmp[0] = t_v4, tmp[1] = t_f32, tmp[2] = 4; wi(&ty, 23, tmp, 3);
    tmp[0] = p_out4, tmp[1] = 3, tmp[2] = t_v4; wi(&ty, 32, tmp, 3);
    tmp[0] = p_in_i32, tmp[1] = 1, tmp[2] = t_i32; wi(&ty, 32, tmp, 3);
    tmp[0] = p_out4, tmp[1] = v_pos, tmp[2] = 3; wi(&ty, 59, tmp, 3);
    tmp[0] = p_in_i32, tmp[1] = v_vi, tmp[2] = 1; wi(&ty, 59, tmp, 3);
    tmp[0] = t_f32, tmp[1] = c_0, tmp[2] = fbits(0.0f); wi(&ty, 43, tmp, 3);
    tmp[0] = t_f32, tmp[1] = c_1, tmp[2] = fbits(1.0f); wi(&ty, 43, tmp, 3);
    tmp[0] = t_f32, tmp[1] = c_4, tmp[2] = fbits(4.0f); wi(&ty, 43, tmp, 3);
    tmp[0] = t_f32, tmp[1] = c_m1, tmp[2] = fbits(-1.0f); wi(&ty, 43, tmp, 3);
    tmp[0] = t_i32, tmp[1] = c_i1, tmp[2] = 1; wi(&ty, 43, tmp, 3);
    tmp[0] = t_i32, tmp[1] = c_i2, tmp[2] = 2; wi(&ty, 43, tmp, 3);
    tmp[0] = t_i32, tmp[1] = c_i0, tmp[2] = 0; wi(&ty, 43, tmp, 3);
    tmp[0] = v_pos, tmp[1] = 11, tmp[2] = 0; wi(&an, 71, tmp, 3);
    tmp[0] = v_vi, tmp[1] = 11, tmp[2] = 42; wi(&an, 71, tmp, 3);
    uint32_t iface[64], ni = 0;
    iface[ni++] = v_pos, iface[ni++] = v_vi;
    uint32_t lab = id++;
    tmp[0] = t_void, tmp[1] = main_id, tmp[2] = 0, tmp[3] = t_fn; wi(&fn, 54, tmp, 4);
    tmp[0] = lab; wi(&fn, 248, tmp, 1);
    uint32_t vi = id++;
    tmp[0] = t_i32, tmp[1] = vi, tmp[2] = v_vi; wi(&fn, 61, tmp, 3);
    uint32_t b0 = id++, b1 = id++, s0 = id++, s1 = id++, fx = id++, fy = id++, tf = id++;
    tmp[0] = t_i32, tmp[1] = b0, tmp[2] = vi, tmp[3] = c_i1; wi(&fn, 199, tmp, 4);          /* vi & 1 */
    tmp[0] = t_i32, tmp[1] = s0, tmp[2] = vi, tmp[3] = c_i1; wi(&fn, 195, tmp, 4);           /* vi >> 1 (arithmetic) */
    tmp[0] = t_i32, tmp[1] = b1, tmp[2] = s0, tmp[3] = c_i1; wi(&fn, 199, tmp, 4);           /* (vi >> 1) & 1 */
    uint32_t f0 = id++, f1 = id++, m0 = id++, m1 = id++;
    tmp[0] = t_f32, tmp[1] = f0, tmp[2] = b0; wi(&fn, 111, tmp, 3);
    tmp[0] = t_f32, tmp[1] = f1, tmp[2] = b1; wi(&fn, 111, tmp, 3);
    tmp[0] = t_f32, tmp[1] = m0, tmp[2] = f0, tmp[3] = c_4; wi(&fn, 133, tmp, 4);
    tmp[0] = t_f32, tmp[1] = m1, tmp[2] = f1, tmp[3] = c_4; wi(&fn, 133, tmp, 4);
    tmp[0] = t_f32, tmp[1] = fx, tmp[2] = m0, tmp[3] = c_1; wi(&fn, 131, tmp, 4);            /* x = 4*b0 - 1 */
    tmp[0] = t_f32, tmp[1] = fy, tmp[2] = m1, tmp[3] = c_1; wi(&fn, 131, tmp, 4);
    tmp[0] = t_f32, tmp[1] = tf, tmp[2] = vi; wi(&fn, 111, tmp, 3);                           /* float(vertex index) */
    uint32_t pos = id++;
    tmp[0] = t_v4, tmp[1] = pos, tmp[2] = fx, tmp[3] = fy, tmp[4] = c_0, tmp[5] = c_1; wi(&fn, 80, tmp, 6);
    tmp[0] = v_pos, tmp[1] = pos; wi(&fn, 62, tmp, 2);
    (void)c_m1;
    for (uint32_t k = 0; k < f->nin && k < 40; k++) {
        const fsin_t *in = &f->in[k];
        if (in->kind > 2 || in->ncomp > 4) { free(ty.w), free(an.w), free(fn.w), free(glob.w); return NULL; }
        uint32_t scal = in->kind == 0 ? t_f32 : in->kind == 1 ? t_i32 : t_u32, vt = scal, pt, var = id++;
        if (in->ncomp > 1) { vt = id++; tmp[0] = vt, tmp[1] = scal, tmp[2] = in->ncomp; wi(&ty, 23, tmp, 3); }
        pt = id++; tmp[0] = pt, tmp[1] = 3, tmp[2] = vt; wi(&ty, 32, tmp, 3);
        tmp[0] = pt, tmp[1] = var, tmp[2] = 3; wi(&ty, 59, tmp, 3);
        tmp[0] = var, tmp[1] = 30, tmp[2] = in->loc; wi(&an, 71, tmp, 3);
        if (in->flat) { tmp[0] = var, tmp[1] = 14; wi(&an, 71, tmp, 2); }
        iface[ni++] = var;
        uint32_t comp[4];
        for (uint32_t j = 0; j < in->ncomp; j++) {
            if (in->kind == 0) {
                uint32_t ca = id++, cb = id++, mu = id++, ad = id++;
                tmp[0] = t_f32, tmp[1] = ca, tmp[2] = fbits(0.11f + 0.07f * (float)((k * 5 + j) % 5)); wi(&ty, 43, tmp, 3);
                tmp[0] = t_f32, tmp[1] = cb, tmp[2] = fbits(0.3f + 0.13f * (float)(k % 7) + 0.1f * (float)j); wi(&ty, 43, tmp, 3);
                tmp[0] = t_f32, tmp[1] = mu, tmp[2] = tf, tmp[3] = ca; wi(&fn, 133, tmp, 4);
                tmp[0] = t_f32, tmp[1] = ad, tmp[2] = mu, tmp[3] = cb; wi(&fn, 129, tmp, 4);
                comp[j] = ad;
            } else { comp[j] = id++; tmp[0] = scal, tmp[1] = comp[j], tmp[2] = k * 3 + j + 1; wi(&ty, 43, tmp, 3); }
        }
        uint32_t val = comp[0];
        if (in->ncomp > 1) { val = id++; tmp[0] = vt, tmp[1] = val; for (uint32_t j = 0; j < in->ncomp; j++) tmp[2 + j] = comp[j]; wi(&fn, 80, tmp, 2 + in->ncomp); }
        tmp[0] = var, tmp[1] = val; wi(&fn, 62, tmp, 2);
    }
    (void)glob; (void)p_in_i32;
    wi(&fn, 253, NULL, 0), wi(&fn, 56, NULL, 0);
    wb_t o = { 0 };
    wp(&o, 0x07230203u), wp(&o, 0x00010000u), wp(&o, 0), wp(&o, id), wp(&o, 0);
    tmp[0] = 1; wi(&o, 17, tmp, 1);                 /* Shader */
    tmp[0] = 0, tmp[1] = 1; wi(&o, 14, tmp, 2);     /* Logical GLSL450 */
    { uint32_t e[80]; uint32_t q = 0; e[q++] = 0, e[q++] = main_id; uint32_t s0w = 0x6e69616du; e[q++] = s0w, e[q++] = 0; for (uint32_t i = 0; i < ni; i++) e[q++] = iface[i]; wi(&o, 15, e, q); }
    for (size_t i = 0; i < an.n; i++) wp(&o, an.w[i]);
    for (size_t i = 0; i < ty.n; i++) wp(&o, ty.w[i]);
    for (size_t i = 0; i < fn.n; i++) wp(&o, fn.w[i]);
    free(ty.w), free(an.w), free(fn.w), free(glob.w);
    *nout = o.n;
    return o.w;
}

typedef struct { uint8_t *data; size_t size; } img_out_t;

/* Runs FS (+ generated VS) on a 16x16 target set; returns the pixel bytes of every colour attachment. -1 = could not run (reason printed), 0 = ok. */
static int run_fs(vk_t *v, const uint32_t *fs, size_t fsn, const fsinfo_t *fi, const uint32_t *vs, size_t vsn, const uint32_t *seeds, uint32_t nseeds, uint8_t **outp, size_t *outn, const char **why) {
    enum { W = 16, H = 16 };
    *why = "";
    uint32_t natt = fi->nout ? fi->maxloc + 1 : 1;
    if (natt > 8) { *why = "more than 8 colour outputs"; return -1; }
    VkFormat afmt[8];
    uint32_t apx[8];
    for (uint32_t i = 0; i < natt; i++) {
        uint32_t kind = 0;
        for (uint32_t j = 0; j < fi->nout; j++) if (fi->out[j].loc == i) kind = fi->out[j].kind;
        afmt[i] = kind == 0 ? VK_FORMAT_R32G32B32A32_SFLOAT : kind == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
        apx[i] = 16;
    }
    VkImage aimg[8];
    VkDeviceMemory amem[8];
    VkImageView aview[8];
    for (uint32_t i = 0; i < natt; i++) {
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = afmt[i], .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VKC(vkCreateImage(v->dev, &ic, NULL, &aimg[i]));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(v->dev, aimg[i], &mr);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(v, mr.memoryTypeBits, 0) };
        VKC(vkAllocateMemory(v->dev, &mai, NULL, &amem[i]));
        VKC(vkBindImageMemory(v->dev, aimg[i], amem[i], 0));
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = aimg[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = afmt[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VKC(vkCreateImageView(v->dev, &vc, NULL, &aview[i]));
    }
    /* descriptors */
    VkDescriptorSetLayoutBinding binds[LAGFX_LAYOUT_MAX_BINDINGS];
    const uint8_t *blobs[2] = { (const uint8_t *)(const void *)vs, (const uint8_t *)(const void *)fs };
    size_t lens[2] = { vsn * 4, fsn * 4 };
    uint32_t nb = lagfx_layout_bindings_from_spv(blobs, lens, 2, binds);
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl;
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = binds };
    VKC(vkCreateDescriptorSetLayout(v->dev, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VKC(vkCreatePipelineLayout(v->dev, &pli, NULL, &pl));
    VkSampler samp;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 0.0f };
    VKC(vkCreateSampler(v->dev, &sci, NULL, &samp));
    /* pipeline */
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsn * 4, .pCode = vs };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fsn * 4, .pCode = fs };
    VkShaderModule vm, fm;
    VKC(vkCreateShaderModule(v->dev, &smv, NULL, &vm));
    VKC(vkCreateShaderModule(v->dev, &smf, NULL, &fm));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < 8; i++) cba[i] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = natt, .pAttachments = cba };
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = natt, .pColorAttachmentFormats = afmt };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
    if (pr != VK_SUCCESS) { static char m[64]; snprintf(m, sizeof m, "vkCreateGraphicsPipelines = %d", (int)pr); *why = m; return -1; }
    for (uint32_t si = 0; si < nseeds; si++) {
    uint32_t seed = seeds[si];
    VkDescriptorPoolSize psz[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 16 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 4, .pPoolSizes = psz };
    VkDescriptorPool dp;
    VKC(vkCreateDescriptorPool(v->dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    VKC(vkAllocateDescriptorSets(v->dev, &dai, &ds));
    buf_t sb[40], stage;
    memset(sb, 0, sizeof sb), memset(&stage, 0, sizeof stage);
    VkImage rimg[40]; VkDeviceMemory rmem[40]; VkImageView rview[40]; fsimg_t rinfo[40];
    memset(rimg, 0, sizeof rimg), memset(rmem, 0, sizeof rmem), memset(rview, 0, sizeof rview);
    VkDescriptorBufferInfo dbi[40]; VkDescriptorImageInfo dii[48]; VkWriteDescriptorSet wr[96];
    uint32_t nw = 0, nsb = 0, nri = 0, ndi = 0;
    size_t stage_need = 0;
    for (uint32_t i = 0; i < nb; i++) if (binds[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) stage_need = stage_need > 16 * 16 * 16 * 16 ? stage_need : 16 * 16 * 16 * 16;
    if (stage_need && mkbuf(v, &stage, stage_need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
    if (stage.p) fill_pattern(stage.p, stage.size, seed ^ 0x5bd1e995u);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = v->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    VKC(vkAllocateCommandBuffers(v->dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VKC(vkBeginCommandBuffer(cmd, &cbi));
    for (uint32_t i = 0; i < nb; i++) {
        const VkDescriptorSetLayoutBinding *b = &binds[i];
        VkWriteDescriptorSet *w = &wr[nw++];
        *w = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding, .descriptorCount = 1, .descriptorType = b->descriptorType };
        if (b->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (mkbuf(v, &sb[nsb], 4u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return -1;
            fill_pattern(sb[nsb].p, sb[nsb].size, seed + b->binding * 977u);
            dbi[nsb] = (VkDescriptorBufferInfo){ sb[nsb].b, 0, VK_WHOLE_SIZE };
            w->pBufferInfo = &dbi[nsb++];
        } else if (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
            dii[ndi] = (VkDescriptorImageInfo){ .sampler = samp };
            w->pImageInfo = &dii[ndi++];
        } else if (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            fsimg_t info = { b->binding, 1, 0, 0, 0, 1, 0 };
            for (uint32_t k = 0; k < fi->nimg; k++) if (fi->img[k].binding == b->binding) info = fi->img[k];
            if (info.depth == 1 || info.ms || info.dim == 5 /* Buffer */ || info.dim == 6) { *why = "depth / multisampled / buffer image"; return -1; }
            VkImageType it = info.dim == 0 ? VK_IMAGE_TYPE_1D : info.dim == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            uint32_t layers = info.dim == 3 ? 6u * (info.arrayed ? 2u : 1u) : info.arrayed ? 2u : 1u;
            VkImageViewType vt = info.dim == 0 ? (info.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : info.dim == 2 ? VK_IMAGE_VIEW_TYPE_3D : info.dim == 3 ? (info.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : (info.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
            VkFormat fmt = info.skind == 0 ? VK_FORMAT_R8G8B8A8_UNORM : info.skind == 1 ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
            VkExtent3D ext = { it == VK_IMAGE_TYPE_1D ? 16u : 8u, it == VK_IMAGE_TYPE_1D ? 1u : 8u, it == VK_IMAGE_TYPE_3D ? 8u : 1u };
            if (info.dim == 3) ext.width = ext.height = 8;
            VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = info.dim == 3 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = it, .format = fmt, .extent = ext, .mipLevels = 1, .arrayLayers = it == VK_IMAGE_TYPE_3D ? 1 : layers,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
            VKC(vkCreateImage(v->dev, &ic, NULL, &rimg[nri]));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(v->dev, rimg[nri], &mr);
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(v, mr.memoryTypeBits, 0) };
            VKC(vkAllocateMemory(v->dev, &mai, NULL, &rmem[nri]));
            VKC(vkBindImageMemory(v->dev, rimg[nri], rmem[nri], 0));
            VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rimg[nri], .viewType = vt, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, it == VK_IMAGE_TYPE_3D ? 1 : layers } };
            VKC(vkCreateImageView(v->dev, &vc, NULL, &rview[nri]));
            VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = rimg[nri], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, it == VK_IMAGE_TYPE_3D ? 1 : layers } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
            VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, it == VK_IMAGE_TYPE_3D ? 1 : layers }, .imageExtent = ext };
            vkCmdCopyBufferToImage(cmd, stage.b, rimg[nri], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT, b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
            rinfo[nri] = info;
            dii[ndi] = (VkDescriptorImageInfo){ .imageView = rview[nri], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w->pImageInfo = &dii[ndi++];
            nri++;
        } else if (b->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            uint32_t idx = 0;
            for (uint32_t k = 0; k < fi->nia; k++) if (fi->ia_binding[k] == b->binding) idx = fi->ia_index[k];
            if (!v->local_read || idx >= natt) { *why = "input attachments need VK_KHR_dynamic_rendering_local_read and a matching colour target"; return -1; }
            dii[ndi] = (VkDescriptorImageInfo){ .imageView = aview[idx], .imageLayout = LR_LAYOUT };
            w->pImageInfo = &dii[ndi++];
        } else { *why = "descriptor type not handled"; return -1; }
    }
    vkUpdateDescriptorSets(v->dev, nw, wr, 0, NULL);
    /* record */
    VkImageMemoryBarrier ab[8];
    for (uint32_t i = 0; i < natt; i++) ab[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = v->local_read ? LR_LAYOUT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = aimg[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, natt, ab);
    VkRenderingAttachmentInfo ca[8];
    for (uint32_t i = 0; i < natt; i++) {
        VkClearValue cv;
        memset(&cv, 0, sizeof cv);
        if (afmt[i] == VK_FORMAT_R32G32B32A32_SFLOAT) { cv.color.float32[0] = 0.25f, cv.color.float32[1] = 0.5f, cv.color.float32[2] = 0.75f, cv.color.float32[3] = 1.0f; }
        else for (int k = 0; k < 4; k++) cv.color.uint32[k] = 3u + (uint32_t)k;
        ca[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = aview[i], .imageLayout = v->local_read ? LR_LAYOUT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv };
    }
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = natt, .pColorAttachments = ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    buf_t rb[8];
    memset(rb, 0, sizeof rb);
    for (uint32_t i = 0; i < natt; i++) {
        if (mkbuf(v, &rb[i], W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
        VkImageMemoryBarrier b2 = ab[i]; b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, b2.oldLayout = ab[i].newLayout, b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
        vkCmdCopyImageToBuffer(cmd, aimg[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb[i].b, 1, &bic);
    }
    VKC(vkEndCommandBuffer(cmd));
    int rc = submit_wait(v, cmd);
    if (!rc) {
        size_t per = (size_t)natt * W * H * 16;
        *outp = realloc(*outp, per * (si + 1));
        for (uint32_t i = 0; i < natt; i++) memcpy(*outp + per * si + (size_t)i * W * H * 16, rb[i].p, W * H * 16);
        *outn = per * (si + 1);
    }
    for (uint32_t i = 0; i < natt; i++) rmbuf(v, &rb[i]);
    vkFreeCommandBuffers(v->dev, v->pool, 1, &cmd);
    vkDestroyDescriptorPool(v->dev, dp, NULL);
    for (uint32_t i = 0; i < nsb; i++) rmbuf(v, &sb[i]);
    rmbuf(v, &stage);
    for (uint32_t i = 0; i < nri; i++) vkDestroyImageView(v->dev, rview[i], NULL), vkDestroyImage(v->dev, rimg[i], NULL), vkFreeMemory(v->dev, rmem[i], NULL);
    if (rc) return rc;
    }   /* seeds */
    vkDestroyPipeline(v->dev, pipe, NULL), vkDestroyShaderModule(v->dev, vm, NULL), vkDestroyShaderModule(v->dev, fm, NULL);
    vkDestroySampler(v->dev, samp, NULL), vkDestroyPipelineLayout(v->dev, pl, NULL), vkDestroyDescriptorSetLayout(v->dev, dsl, NULL);
    for (uint32_t i = 0; i < natt; i++) vkDestroyImageView(v->dev, aview[i], NULL), vkDestroyImage(v->dev, aimg[i], NULL), vkFreeMemory(v->dev, amem[i], NULL);
    (void)apx;
    return 0;
}

/* a valid module that computes something else: the two incoming VALUES of the first OpPhi whose values differ AND that sits in the merge block of a selection (not a loop header:
 * swapping those could make a loop run forever) are swapped */
static int swap_phi(uint32_t *w, size_t n) {
    uint32_t bound = w[3];
    uint8_t *selmerge = calloc(bound, 1);
    int done = 0;
    for (size_t p = 5; p < n;) { uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16; if (op == 247 && w[p + 1] < bound) selmerge[w[p + 1]] = 1; p += wc; }
    uint32_t cur = 0;
    for (size_t p = 5; p < n && !done;) {
        uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
        if (op == 248) cur = w[p + 1];
        if (op == 245 && wc >= 7 && cur < bound && selmerge[cur] && w[p + 3] != w[p + 5]) { uint32_t t = w[p + 3]; w[p + 3] = w[p + 5]; w[p + 5] = t; done = 1; }
        p += wc;
    }
    free(selmerge);
    return done;
}

/* second mutation for the fragment negative control: the k-th OpFAdd of a function body becomes OpFSub (same operand shape, different value) */
static int flip_fadd(uint32_t *w, size_t n, uint32_t k) {
    int infn = 0;
    for (size_t p = 5; p < n;) {
        uint32_t op = w[p] & 0xffffu, wc = w[p] >> 16;
        if (op == 54) infn = 1;
        if (infn && op == 129) { if (!k) { w[p] = (wc << 16) | 131u; return 1; } k--; }
        p += wc;
    }
    return 0;
}

/* three input seeds per shader: the pipeline is built once (the JIT is the slow part), every seed re-fills the buffers / textures and draws again */
static const uint32_t SEEDS[3] = { 11u, 99u, 1234u };
#define NSEEDS 3u

static unsigned fl_of(unsigned mode) { return mode == 5 ? (LSSA_DCE | LSSA_MAXPHI(2)) : mode == 1 ? 0u : mode == 2 ? LSSA_DCE : mode == 3 ? (LSSA_DCE | LSSA_DCE_INPUT) : (LSSA_DCE | LSSA_AGG); }

static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }

static void part2(void) {
    printf("-- part 2: lavapipe, bit-exact comparison against the unreduced shader --\n");
    vk_t v;
    int ir = vk_init(&v);
    if (ir == 77) { printf("SKIP part 2: no Vulkan 1.3 device\n"); return; }
    if (ir) { g_fail++; return; }
    /* compute */
    {
        size_t n = sizeof SSA_STRESS_CS / 4;
        uint32_t base[3][256], again[256];
        for (uint32_t sd = 0; sd < 3; sd++) { if (run_cs(&v, SSA_STRESS_CS, n, 0x1234u + sd * 77u, base[sd])) { g_fail++; return; } }
        CHECK(!run_cs(&v, SSA_STRESS_CS, n, 0x1234u, again) && !memcmp(again, base[0], 1024), "ssa_stress: the unreduced shader is not deterministic");
        CHECK(memcmp(base[0], base[1], 1024) != 0, "ssa_stress: different inputs gave identical output (test would be blind)");
        int distinct = 0;
        for (int i = 1; i < 256; i++) distinct += base[0][i] != base[0][0];
        CHECK(distinct > 200, "ssa_stress: output is not varied (%d distinct)", distinct);
        for (unsigned mode = 1; mode <= 5; mode++) {
            uint32_t *o = NULL;
            size_t on = 0;
            if (lagfx_spv_ssa_promote(SSA_STRESS_CS, n, fl_of(mode), &o, &on, NULL, NULL, 0) != 1) { CHECK(0, "ssa_stress mode %u: not promoted", mode); continue; }
            int same = 1;
            for (uint32_t sd = 0; sd < 3; sd++) { uint32_t got[256]; if (run_cs(&v, o, on, 0x1234u + sd * 77u, got)) { same = 0; break; } if (memcmp(got, base[sd], 1024)) { same = 0; int bad = 0; for (int i = 0; i < 256; i++) bad += got[i] != base[sd][i]; printf("  seed %u: %d of 256 invocations differ\n", sd, bad); } }
            CHECK(same, "ssa_stress mode %u: output differs from the unreduced shader", mode);
            printf("  ssa_stress.comp mode %u: 3 seeds x 256 invocations bit-identical: %s\n", mode, same ? "yes" : "NO");
            if (mode == 2) {   /* negative control */
                uint32_t *m = malloc(on * 4);
                memcpy(m, o, on * 4);
                int sw = swap_phi(m, on);
                uint32_t got[256];
                int differs = 0;
                if (sw && !run_cs(&v, m, on, 0x1234u, got)) differs = memcmp(got, base[0], 1024) != 0;
                CHECK(sw && differs, "NEGATIVE CONTROL: a phi with swapped values must change the output (swapped=%d differs=%d)", sw, differs);
                printf("  negative control (phi values swapped): output %s (expected: differs)\n", differs ? "differs" : "IDENTICAL");
                free(m);
            }
            free(o);
        }
    }
    /* fragments */
    /* The two large shaders take lavapipe minutes to JIT per variant (the same size that the Adreno driver rejects): LAGFX_SSA_TEST_BIG=1 runs them with modes 1 and 3, =2 with all four modes. */
    const char *bigenv = getenv("LAGFX_SSA_TEST_BIG");
    int big = bigenv ? atoi(bigenv) : 0;
    static const char *fx[] = { "i1const_fragment", "dump_f16_fmax_frag", "dump_f16_fmax3_frag", "login_tvcmxc_isrc_frag", "login_xgc_frag", "uber_a2xghfc_231501" };
    for (size_t k = 0; k < sizeof fx / sizeof fx[0]; k++) {
        int isbig = k >= 4;
        if (isbig && !big) { printf("  %s: not run here (set LAGFX_SSA_TEST_BIG=1; lavapipe needs minutes per variant)\n", fx[k]); g_notrun++; continue; }
        char fn[128];
        snprintf(fn, sizeof fn, "%s.air.bc", fx[k]);
        size_t n = 0;
        uint32_t *w = translate_fixture(fn, &n);
        if (!w) { CHECK(0, "fixture %s did not translate", fx[k]); continue; }
        fsinfo_t fi;
        reflect_fs(w, n, &fi);
        size_t vn = 0;
        uint32_t *vs = build_vs(&fi, &vn);
        if (!vs) { printf("  %s: NOT RUN (cannot generate a vertex stage for its inputs)\n", fx[k]); g_notrun++; free(w); continue; }
        uint8_t *ref = NULL, *ref2 = NULL;
        size_t rn = 0, rn2 = 0;
        const char *why;
        int rr = run_fs(&v, w, n, &fi, vs, vn, SEEDS, NSEEDS, &ref, &rn, &why);
        if (rr) { printf("  %s: NOT RUN (%s)\n", fx[k], why); g_notrun++; free(w), free(vs); continue; }
        if (!isbig) {
            int rr2 = run_fs(&v, w, n, &fi, vs, vn, SEEDS, NSEEDS, &ref2, &rn2, &why);
            CHECK(!rr2 && rn == rn2 && !memcmp(ref, ref2, rn), "%s: the unreduced shader is not deterministic run to run", fx[k]);
        }
        size_t per = rn / NSEEDS;
        int seed_sens = memcmp(ref, ref + per, per) != 0 || memcmp(ref, ref + 2 * per, per) != 0;
        size_t distinct = 0;
        for (size_t i = 16; i + 16 <= per; i += 16) distinct += memcmp(ref + i, ref, 16) != 0;
        printf("  %s: reference run ok (%u seeds, %zu bytes of colour output per seed, %zu/%zu pixels differ from pixel 0 (seed 0), differs between seeds: %s, sha %016llx)\n", fx[k], NSEEDS, per, distinct, per / 16, seed_sens ? "yes" : "no", (unsigned long long)fnv(ref, rn));
        for (unsigned mode = 1; mode <= 5; mode++) {
            if (isbig && big < 2 && (mode == 2 || mode == 4)) continue;
            uint32_t *o = NULL;
            size_t on = 0;
            lagfx_ssa_stats_t st;
            if (lagfx_spv_ssa_promote(w, n, fl_of(mode), &o, &on, &st, NULL, 0) != 1) { CHECK(0, "%s mode %u not promoted", fx[k], mode); continue; }
            fsinfo_t fo;
            reflect_fs(o, on, &fo);
            uint8_t *got = NULL; size_t gn = 0;
            int g = run_fs(&v, o, on, &fo, vs, vn, SEEDS, NSEEDS, &got, &gn, &why);
            CHECK(!g, "%s mode %u: promoted shader could not run: %s", fx[k], mode, why);
            if (!g) {
                int same = gn == rn && !memcmp(got, ref, rn);
                CHECK(same, "%s mode %u: colour output differs from the unreduced shader", fx[k], mode);
                printf("    mode %u (locals %u->%u, phis %u): bit-identical over %u seeds = %s\n", mode, st.vars, st.vars - st.promoted, st.phis_kept, NSEEDS, same ? "yes" : "NO");
            }
            free(got), free(o);
        }
        if (!isbig) {   /* negative control on a fragment shader: mutate the promoted module until the colour output changes (it must, for at least one mutation, or this harness could not see a wrong transform) */
            uint32_t *o = NULL; size_t on = 0;
            if (lagfx_spv_ssa_promote(w, n, LSSA_DCE, &o, &on, NULL, NULL, 0) == 1) {
                int found = 0, tried = 0;
                for (uint32_t q = 0; q < 24 && !found; q++) {
                    uint32_t *m = malloc(on * 4);
                    memcpy(m, o, on * 4);
                    if (!flip_fadd(m, on, q)) { free(m); break; }
                    fsinfo_t fo;
                    reflect_fs(m, on, &fo);
                    uint8_t *got = NULL; size_t gn = 0;
                    int g = run_fs(&v, m, on, &fo, vs, vn, SEEDS, NSEEDS, &got, &gn, &why);
                    tried++;
                    if (!g && !(gn == rn && !memcmp(got, ref, rn))) found = 1;
                    free(got); free(m);
                }
                printf("    negative control on %s (promoted module with one OpFAdd -> OpFSub): %s after %d mutation(s)\n", fx[k], found ? "output differs (expected)" : "no mutation reached the output", tried);
                if (!strcmp(fx[k], "dump_f16_fmax3_frag")) CHECK(found, "NEGATIVE CONTROL on %s: no mutation changed the output; the comparison is blind here", fx[k]);
                free(o);
            }
        }
        free(ref), free(ref2), free(w), free(vs);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    part1();
    part2();
    printf("%s (failures=%d, fixtures not run=%d)\n", g_fail ? "FAILED" : "OK", g_fail, g_notrun);
    return g_fail ? 1 : 0;
}
