/* `threadgroup T *p [[threadgroup(n)]]` kernel arguments, translated and EXECUTED on a Vulkan device (lavapipe) against a CPU model.
 * SPDX-License-Identifier: MIT
 *
 * Fixtures tests/fixtures/threadgroup_arg_{i32,f32,direct}.air.bc (HAND-WRITTEN IR via llvm-as, NOT Apple output; the metadata key "air.threadgroup_buffer" is the translator's
 * reading of Apple's AIR and has not been seen in a captured module). Kernel: each thread stores f(lid) into scratch[lid], barrier, reads scratch[(lid+1) & 63], writes it to
 * o[gid*64 + lid]. The scratch length is a specialization constant (SpecId 3, elements). Checked:
 *   - i32 / f32: every output word of 4 groups of 64 matches the model, with the length specialized to 64 AND left at its default (1024)
 *   - the length constant carries the reflection name lagfx.tgarg.0.4
 *   - the bare-dereference kernel (*scratch = 7) is REFUSED (translation fails, reason reported), never an undefined pointer
 * Exit 77 (skip) without a Vulkan device or shaderInt64. */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air2spv/spv_reflect.h"
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

static int check(const char *fx, int is_float) {
    uint8_t *spv; size_t sn; lagfx_status_t st;
    if (translate(fx, &spv, &sn, &st)) return 1;
    CHECK(st == LAGFX_OK && spv, "%s: translate = %d (narrow refusal: '%s')", fx, (int)st, lagfx_air2spv_last_narrow_refusal());
    CHECK(memmem(spv, sn, "lagfx.tgarg.0.4", 15) != NULL, "%s: reflection name lagfx.tgarg.0.4 missing", fx);
    const uint32_t lens[2] = { 64u, 0u };   /* specialized, then the default length (1024) */
    for (int v = 0; v < 2; v++) {
        buf_t o; if (mkbuf(&o, 4 * 64 * 4)) return 1;
        if (run(spv, sn, &o, 4, 64, lens[v])) return 1;
        int bad = 0;
        for (uint32_t g = 0; g < 4; g++) for (uint32_t l = 0; l < 64; l++) {
            uint32_t src = (l + 1u) & 63u, want;
            if (is_float) { float f = (float)(src * 3u) + 0.5f; memcpy(&want, &f, 4); } else want = src * 3u + 1u;
            if (o.p[g * 64 + l] != want) { if (bad++ < 4) printf("  %s len=%u: group %u lane %u = 0x%x, expected 0x%x\n", fx, lens[v], g, l, o.p[g * 64 + l], want); }
        }
        CHECK(bad == 0, "%s (tg length %s): %d mismatching words", fx, lens[v] ? "64" : "default", bad);
    }
    printf("PASS: %s: threadgroup argument scratch[lid] -> barrier -> scratch[(lid+1)&63] matches the CPU model (length specialized and default)\n", fx);
    free(spv);
    return 0;
}

/* Two threadgroup arguments with Metal indices 5 (a) and 2 (b), declared in that order: SpecIds follow the Metal index (8, 5), not the argument order; run through the common compute base. */
static int check_two(void) {
    uint8_t *spv; size_t sn; lagfx_status_t st;
    if (translate("threadgroup_arg_two.air.bc", &spv, &sn, &st)) return 1;
    CHECK(st == LAGFX_OK && spv, "two: translate = %d", (int)st);
    lagfx_spv_tgarg_t tg[4]; size_t ntg = lagfx_spv_reflect_threadgroup_args(spv, sn, tg, 4);
    CHECK(ntg == 2 && tg[0].metal_index == 2 && tg[0].spec_id == 5 && tg[0].elem_bytes == 4 && tg[1].metal_index == 5 && tg[1].spec_id == 8, "reflection: n=%zu (%u,%u) (%u,%u)", ntg, tg[0].metal_index, tg[0].spec_id, tg[1].metal_index, tg[1].spec_id);
    uint64_t bytes = 0; const uint32_t sp_a[2][2] = { { 5, 64 }, { 8, 64 } }, sp_b[2][2] = { { 5, 128 }, { 8, 64 } };
    CHECK(lagfx_spv_reflect_workgroup_bytes(spv, sn, sp_a, 2, &bytes) && bytes == 512, "workgroup bytes (64+64 elements) = %llu", (unsigned long long)bytes);
    CHECK(lagfx_spv_reflect_workgroup_bytes(spv, sn, sp_b, 2, &bytes) && bytes == 768, "workgroup bytes (128+64 elements) = %llu", (unsigned long long)bytes);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g_pd, &pp);
    lagfx_spv_binding_t rf[8]; size_t nrf = lagfx_spv_reflect_bindings(spv, sn, rf, 8);
    CHECK(nrf == 1, "bindings %zu", nrf);
    lagfx_cx_bind_t bd = { rf[0].binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    const uint32_t *w = (const uint32_t *)(const void *)spv;
    const uint32_t full[5][2] = { { 0, 64 }, { 1, 1 }, { 2, 1 }, { 5, 64 }, { 8, 64 } };
    const uint32_t nob[4][2] = { { 0, 64 }, { 1, 1 }, { 2, 1 }, { 5, 64 } };            /* argument 5 (SpecId 8) has no length */
    const uint32_t dup[6][2] = { { 0, 64 }, { 1, 1 }, { 2, 1 }, { 5, 64 }, { 8, 64 }, { 5, 32 } };
    CHECK(!lagfx_cx_pipe_create_ex(g_dev, w, sn, NULL, &bd, 1, 0, nob, 4, &pp.limits), "a threadgroup argument without a length must not build (no silent default)");
    CHECK(!lagfx_cx_pipe_create_ex(g_dev, w, sn, NULL, &bd, 1, 0, dup, 6, &pp.limits), "a duplicate SpecId must be refused");
    VkPhysicalDeviceLimits small = pp.limits; small.maxComputeSharedMemorySize = 500;
    CHECK(!lagfx_cx_pipe_create_ex(g_dev, w, sn, NULL, &bd, 1, 0, full, 5, &small), "512 bytes of workgroup memory on a 500-byte device must be refused");
    VkPhysicalDeviceLimits tiny = pp.limits; tiny.maxComputeWorkGroupInvocations = 32;
    CHECK(!lagfx_cx_pipe_create_ex(g_dev, w, sn, NULL, &bd, 1, 0, full, 5, &tiny), "a 64-invocation workgroup on a 32-invocation device must be refused");
    lagfx_cx_pipe_t *cx = lagfx_cx_pipe_create_ex(g_dev, w, sn, NULL, &bd, 1, 0, full, 5, &pp.limits);
    CHECK(cx, "the common compute base must build the two-argument kernel");
    buf_t o; if (mkbuf(&o, 4 * 64 * 4)) return 1;
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
    VkCommandPool cp; VK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cb, &cbi));
    lagfx_cx_res_t res = { .binding = rf[0].binding, .buffer = o.b, .offset = 0, .range = VK_WHOLE_SIZE };
    CHECK(lagfx_cx_record(cx, cb, &res, 1, NULL, 0, 4, 1, 1), "record");
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkQueueSubmit(g_q, 1, &sub, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(g_q));
    int bad = 0;
    for (uint32_t g = 0; g < 4; g++) for (uint32_t l = 0; l < 64; l++) {
        uint32_t want = ((l + 1u) & 63u) + 1u + ((l + 2u) & 63u) * 10u;
        if (o.p[g * 64 + l] != want) bad++;
    }
    CHECK(bad == 0, "two threadgroup arguments through the compute base: %d mismatching words", bad);
    lagfx_cx_recycle(cx); lagfx_cx_pipe_destroy(cx); vkDestroyCommandPool(g_dev, cp, NULL); free(spv);
    printf("PASS: two threadgroup arguments (Metal indices 5 and 2 -> SpecIds 8 and 5) run through lagfx_cx_pipe_create_ex; missing length / duplicate SpecId / shared-memory and workgroup-size limits are refused\n");
    return 0;
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "air2spv-threadgroup-arg", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("SKIP: no Vulkan 1.3 instance"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL); if (!nd) { puts("SKIP: no Vulkan device"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds); g_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(g_pd, &f);
    if (!f.shaderInt64) { puts("SKIP: device lacks shaderInt64"); return 77; }
    float prio = 1.0f; uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    g_qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qf = i; break; }
    free(qp); CHECK(g_qf != ~0u, "no compute queue");
    for (int mode = 0; mode < 2; mode++) {   /* 0: a device WITH shaderInt64; 1: Int64 narrowing on and a device WITHOUT shaderInt64 (the S8 condition) */
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
        VkPhysicalDeviceFeatures en = { .shaderInt64 = mode ? VK_FALSE : VK_TRUE };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &en };
        VK(vkCreateDevice(g_pd, &dci, NULL, &g_dev)); vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
        lagfx_air2spv_set_narrow_int64(mode);
        printf("--- %s\n", mode ? "no shaderInt64, i64 narrowed to 32 bits" : "shaderInt64 enabled");
        if (check("threadgroup_arg_i32.air.bc", 0)) return 1;
        if (check("threadgroup_arg_f32.air.bc", 1)) return 1;
        if (check_two()) return 1;
        {   uint8_t *spv; size_t sn; lagfx_status_t st;
            if (translate("threadgroup_arg_mixed.air.bc", &spv, &sn, &st)) return 1;
            char why[160]; uint32_t n = lagfx_air2spv_last_unsupported_threadgroup_arg(why, sizeof why);
            CHECK(st != LAGFX_OK && !spv && n >= 1 && strstr(why, "disagree"), "a threadgroup argument accessed as i32 and float must be refused: st=%d n=%u '%s'", (int)st, n, why);
            printf("PASS: a threadgroup argument accessed with two element types is refused (%s)\n", why);
        }
        {   uint8_t *spv; size_t sn; lagfx_status_t st;
            if (translate("threadgroup_arg_direct.air.bc", &spv, &sn, &st)) return 1;
            char why[160]; uint32_t n = lagfx_air2spv_last_unsupported_threadgroup_arg(why, sizeof why);
            CHECK(st != LAGFX_OK && !spv, "a bare dereference of a threadgroup argument must not translate (st=%d)", (int)st);
            CHECK(n == 1 && strstr(why, "dereferenced directly"), "refusal reason: n=%u '%s'", n, why);
            printf("PASS: a bare dereference of a threadgroup argument is refused (%s)\n", why);
        }
        lagfx_air2spv_set_narrow_int64(0);
        vkDestroyDevice(g_dev, NULL);
    }
    return 0;
}
