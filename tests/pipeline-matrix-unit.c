/*
 * libapplegfx-vulkan — the -13 pipeline matrix (src/vulkan/pipeline_matrix.c) on lavapipe
 * tests/pipeline-matrix-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * 1. The engine on a small framebuffer-fetch fragment module (SPIR-V 1.4, an SSBO and two input attachments at bindings 40 / 41) with a TEST OVERRIDE that makes the original (M0) fail with
 *    VK_ERROR_UNKNOWN and lets every other row report what lavapipe really did: every row prints one bounded line, the rows that "changed the outcome" feed M10, the summary lists them.
 * 2. The opt-in production hook: LAGFX_PIPELINE_MATRIX=1 (set below before the first failure) expands the first 2 distinct failing pairs, repeats are silent, later pairs cost one line each.
 * lavapipe builds every valid variant, so this proves the rows are VALID and well-formed, not that Adreno accepts them.
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/pipeline_matrix.h"
#include "vulkan/spv_variants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static const uint32_t k_fs[] = { 0x07230203,0x00010400,0x00070000,0x00000024,0x00000000,0x00020011,0x00000001,0x00020011,0x00000028,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,0x00000005,0x00030010,0x00000001,0x00000007,0x00040047,0x00000002,0x0000001e,0x00000000,0x00030047,0x00000006,0x00000002,0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,0x00040047,0x00000003,0x00000022,0x00000000,0x00040047,0x00000003,0x00000021,0x00000003,0x00040047,0x00000004,0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,0x00000028,0x00040047,0x00000004,0x0000002b,0x00000000,0x00040047,0x00000005,0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,0x00000029,0x00040047,0x00000005,0x0000002b,0x00000001,0x00020013,0x00000007,0x00030021,0x00000008,0x00000007,0x00030016,0x00000009,0x00000020,0x00020014,0x0000000a,0x00040017,0x0000000b,0x00000009,0x00000004,0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,0x0000000c,0x00000002,0x00000003,0x0003001e,0x00000006,0x00000009,0x00040020,0x0000000d,0x0000000c,0x00000006,0x0004003b,0x0000000d,0x00000003,0x0000000c,0x00040015,0x0000000e,0x00000020,0x00000001,0x00040017,0x0000000f,0x0000000e,0x00000002,0x00090019,0x00000010,0x00000009,0x00000006,0x00000000,0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x00000011,0x00000000,0x00000010,0x0004003b,0x00000011,0x00000004,0x00000000,0x0004003b,0x00000011,0x00000005,0x00000000,0x0004002b,0x0000000e,0x00000012,0x00000000,0x0005002c,0x0000000f,0x00000013,0x00000012,0x00000012,0x0004002b,0x00000009,0x00000014,0x3f800000,0x0004002b,0x00000009,0x00000015,0x00000000,0x0007002c,0x0000000b,0x00000016,0x00000014,0x00000015,0x00000015,0x00000014,0x00030001,0x0000000b,0x00000017,0x00030021,0x00000018,0x0000000b,0x00050036,0x0000000b,0x00000019,0x00000000,0x00000018,0x000200f8,0x0000001a,0x000200fe,0x00000017,0x00010038,0x00050036,0x00000007,0x00000001,0x00000000,0x00000008,0x000200f8,0x0000001b,0x0004003d,0x00000010,0x0000001c,0x00000004,0x0004003d,0x00000010,0x0000001d,0x00000005,0x00050062,0x0000000b,0x0000001e,0x0000001c,0x00000013,0x00050062,0x0000000b,0x0000001f,0x0000001d,0x00000013,0x000500b8,0x0000000a,0x00000020,0x00000014,0x00000015,0x000600a9,0x0000000b,0x00000021,0x00000020,0x0000001e,0x0000001f,0x00040039,0x0000000b,0x00000022,0x00000019,0x00050081,0x0000000b,0x00000023,0x00000021,0x00000022,0x0003003e,0x00000002,0x00000023,0x000100fd,0x00010038 };

typedef struct { char buf[40000]; size_t n; unsigned lines; } cap_t;
static void emit(void *u, const char *l) { cap_t *c = u; c->lines++; c->n += (size_t)snprintf(c->buf + c->n, sizeof c->buf - c->n, "%s\n", l); }
static VkResult over(void *u, uint32_t recipe, VkResult real) { (void)u; return recipe == 0 ? VK_ERROR_UNKNOWN : real; }

int main(void) {
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    char logpath[] = "/tmp/pipematrix-unit-XXXXXX";
    int lfd = mkstemp(logpath);
    CHECK(lfd >= 0, "mkstemp");
    close(lfd);
    setenv("LAGFX_LOG_FILE", logpath, 1);       /* the library's log sink (LAGFX_WARN lines) */
    setenv("LAGFX_PIPELINE_MATRIX", "1", 1);   /* before the first failing build: the production hook reads it once */
    unsetenv("LAGFX_PIPELINE_MATRIX_DIR"), unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR");
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    VkDevice dev = vk->device;
    VkDescriptorSetLayoutBinding b[3] = {
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 40, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 41, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b };
    CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl) == VK_SUCCESS, "dsl");
    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    CHECK(vkCreatePipelineLayout(dev, &pi, NULL, &pl) == VK_SUCCESS, "pl");
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    d.layout = pl, d.color_format = VK_FORMAT_R16G16B16A16_SFLOAT, d.n_color = 2, d.color_formats[0] = d.color_formats[1] = VK_FORMAT_R16G16B16A16_SFLOAT, d.independent_blend = vk->have_independent_blend ? 1 : 0;
    d.masks_valid = 1, d.color_write_masks[0] = d.color_write_masks[1] = 0xf, d.color_write_mask = 0xf, d.blend_set = 1, d.raster_order_access = vk->have_raster_order ? 1 : 0, d.caps_enabled = vk->caps_enabled;
    d.vertex_spv = lagfx_stub_vs_spv, d.vertex_spv_bytes = sizeof lagfx_stub_vs_spv, d.fragment_spv = k_fs, d.fragment_spv_bytes = sizeof k_fs, d.vertex_entry_point = "main", d.fragment_entry_point = "main";

    /* ---- 1. the engine ---- */
    {
        static cap_t c;
        lagfx_matrix_env_t env = { .have_local_read = vk->have_local_read, .have_raster_order = vk->have_raster_order, .max_color_att = vk->phys_props.limits.maxColorAttachments,
                                   .max_per_stage_input_att = vk->phys_props.limits.maxPerStageDescriptorInputAttachments, .emit = emit, .user = &c, .test_override = over };
        lagfx_matrix_result_t r;
        lagfx_pipeline_matrix_run(dev, &d, b, 3, &env, &r);
        CHECK(strstr(c.buf, "pipeline-matrix: begin pair") && strstr(c.buf, "row=M0 result=FAIL vk=-13(VK_ERROR_UNKNOWN)"), "header + M0 forced failure:\n%s", c.buf);
        CHECK(c.lines <= 80, "bounded output: %u lines", c.lines);
        CHECK(r.m0_result == VK_ERROR_UNKNOWN, "m0 result");
        const char *must_build[] = { "row=M2 result=BUILT", "row=M3a result=BUILT", "row=M3b result=BUILT", "row=M4a result=BUILT", "row=M4b result=BUILT", "row=M9k result=BUILT", "row=M8 result=n/a", "row=M8c result=n/a" };
        for (unsigned i = 0; i < sizeof must_build / sizeof must_build[0]; i++) CHECK(strstr(c.buf, must_build[i]), "expected '%s':\n%s", must_build[i], c.buf);
        if (vk->have_raster_order) CHECK(strstr(c.buf, "row=M1 result=BUILT") && strstr(c.buf, "row=M6a result=BUILT") && strstr(c.buf, "row=M6b result=BUILT"), "M1 / M6 built (lavapipe accepts every valid variant):\n%s", c.buf);
        if (vk->have_local_read) CHECK(strstr(c.buf, "row=M5a result=BUILT") && strstr(c.buf, "row=M5b result=BUILT") && strstr(c.buf, "row=M5c result=BUILT"), "M5 built:\n%s", c.buf);
        CHECK(strstr(c.buf, "row=M10:M1+M2+M3a") && strstr(c.buf, "row=M10:M1+M2 "), "combinations of the rows that changed the outcome:\n%s", c.buf);
        CHECK(strstr(c.buf, "pipeline-matrix: summary pair") && strstr(c.buf, "FAIL=[M0]") && r.built >= 8 && r.failed == 1 && r.combos >= 2, "summary: built %u failed %u combos %u\n%s", r.built, r.failed, r.combos, c.buf);
        /* no layout / pipeline object leaked into the next build: M0 without the override builds */
        lagfx_matrix_env_t env2 = env;
        env2.test_override = NULL, env2.emit = emit;
        static cap_t c2;
        env2.user = &c2;
        lagfx_pipeline_matrix_run(dev, &d, b, 3, &env2, &r);
        CHECK(strstr(c2.buf, "row=M0 result=BUILT") && strstr(c2.buf, "note M0 BUILT here") && strstr(c2.buf, "row=M10 result=n/a"), "without the override nothing changes the outcome: M10 n/a:\n%s", c2.buf);
        /* rows that cannot apply say so: no layout bindings -> M3 / M4 are n/a */
        static cap_t c3;
        env2.user = &c3;
        lagfx_pipeline_matrix_run(dev, &d, NULL, 0, &env2, &r);
        CHECK(strstr(c3.buf, "row=M3a result=n/a") && strstr(c3.buf, "row=M4a result=n/a") && strstr(c3.buf, "layout bindings are unknown"), "layout rows n/a without bindings:\n%s", c3.buf);
        /* a device without local read: M5 n/a */
        static cap_t c4;
        env2.user = &c4, env2.have_local_read = false;
        lagfx_pipeline_matrix_run(dev, &d, b, 3, &env2, &r);
        CHECK(strstr(c4.buf, "row=M5a result=n/a") && strstr(c4.buf, "dynamicRenderingLocalRead not enabled"), "M5 n/a without local read:\n%s", c4.buf);
    }
    /* ---- the fetch barrier helper records ---- */
    {
        VkCommandPool cp;
        VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = vk->graphics_queue_family };
        CHECK(vkCreateCommandPool(dev, &ci, NULL, &cp) == VK_SUCCESS, "pool");
        VkCommandBuffer cb;
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        CHECK(vkAllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "cb");
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
        lagfx_matrix_cmd_fetch_barrier(cb);
        CHECK(vkEndCommandBuffer(cb) == VK_SUCCESS, "end");
        vkDestroyCommandPool(dev, cp, NULL);
    }
    /* ---- 2. the production hook: forced failures (fragment entry point the module lacks) ---- */
    {
        for (int pass = 0; pass < 4; pass++) {   /* pairs A, A (repeat), B, C */
            VkShaderModule vm, fm;
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof lagfx_stub_vs_spv, .pCode = lagfx_stub_vs_spv };
            vkCreateShaderModule(dev, &ci, NULL, &vm);
            static uint32_t fsv[sizeof k_fs / 4];
            memcpy(fsv, k_fs, sizeof k_fs);
            if (pass >= 2) fsv[3] += (uint32_t)pass;   /* a different module (bigger id bound is still valid): distinct hash */
            ci.codeSize = sizeof k_fs, ci.pCode = fsv;
            vkCreateShaderModule(dev, &ci, NULL, &fm);
            lagfx_pipeline_desc_t f = d;
            f.vertex_shader = vm, f.fragment_shader = fm, f.fragment_entry_point = "no_such_entry", f.fragment_spv = fsv, f.diag = NULL;
            VkPipeline p = VK_NULL_HANDLE;
            CHECK(lagfx_pipeline_build(dev, &f, &p) == LAGFX_ERR_BACKEND, "forced failure %d", pass);
            vkDestroyShaderModule(dev, vm, NULL), vkDestroyShaderModule(dev, fm, NULL);
        }
        FILE *f = fopen(logpath, "r");
        static char all[200000];
        size_t n = f ? fread(all, 1, sizeof all - 1, f) : 0;
        all[n] = 0;
        if (f) fclose(f);
        unlink(logpath);
        unsigned begins = 0, rows = 0, notexp = 0;
        for (const char *q = all; (q = strstr(q, "pipeline-matrix: begin pair")); q++) begins++;
        for (const char *q = all; (q = strstr(q, "pipeline-matrix: row=")); q++) rows++;
        for (const char *q = all; (q = strstr(q, "NOT expanded")); q++) notexp++;
        CHECK(strstr(all, "LAGFX [WARN] pipeline-matrix: ENABLED"), "enable line; log (%zu bytes) head:\n%.2000s", n, all);
        CHECK(begins == 2, "two pairs expanded (limit 2), the repeat is silent: %u\n%s", begins, all);
        CHECK(notexp == 1, "the third distinct pair costs one 'NOT expanded' line: %u", notexp);
        CHECK(rows >= 20 && rows < 160, "bounded rows: %u", rows);
        CHECK(strstr(all, "row=M0 result=FAIL"), "M0 reproduces the forced failure");
    }
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    lagfx_vk_shutdown(vk);
    printf("pipeline matrix: ok\n");
    return 0;
}
