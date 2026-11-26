/*
 * libapplegfx-vulkan — the fragment-module reduction ladder (src/vulkan/pipeline_reduce.c, spv_reduce.c) on lavapipe
 * tests/pipeline-reduce-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * 0. engine, no Vulkan: every ladder row of a small test module (a helper that samples + takes a derivative, a helper with a loop and a switch, input attachment read, OpUndef consumers,
 *    constant and dynamic vector indices, a big SSBO array, a Location input) is written (DRY run) and checked with spirv-val (vulkan1.2) when the tool is installed, plus the library's own self-check.
 * 1. crash isolation (BEFORE this process creates any Vulkan object: fork + Vulkan must not meet a live parent device): rows run in forked children that open their own instance; a row
 *    that SIGSEGVs (LAGFX_PIPELINE_REDUCE_CRASH_ROW) is reported CRASH, a row that hangs (_HANG_ROW) TIMEOUT, and the rows after them still run; --from / --only resume.
 * 2. in-process ladder on lavapipe: every applicable row BUILT; with a simulated driver that rejects modules containing OpSwitch the adaptive search (ddmin) finds the one helper that must stay.
 * 3. the opt-in production hook LAGFX_PIPELINE_REDUCE.
 * lavapipe BUILT proves validity of the rows, it says nothing about Adreno.
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/pipeline_reduce.h"
#include "vulkan/spv_reduce.h"
#include "vulkan/spv_variants.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static const uint32_t k_fs[] = {
    0x07230203,0x00010300,0x00070000,0x0000005f,0x00000000,0x00020011,0x00000001,0x00020011,
    0x00000028,0x0003000e,0x00000000,0x00000001,0x0007000f,0x00000004,0x00000001,0x6e69616d,
    0x00000000,0x00000002,0x00000003,0x00030010,0x00000001,0x00000007,0x00040047,0x00000002,
    0x0000001e,0x00000000,0x00040047,0x00000003,0x0000001e,0x00000002,0x00040047,0x00000004,
    0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,0x00000010,0x00040047,0x00000005,
    0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,0x00000011,0x00040047,0x00000006,
    0x00000022,0x00000000,0x00040047,0x00000006,0x00000021,0x00000028,0x00040047,0x00000006,
    0x0000002b,0x00000000,0x00040047,0x00000007,0x00000022,0x00000000,0x00040047,0x00000007,
    0x00000021,0x00000003,0x00040047,0x00000008,0x00000006,0x00000004,0x00050048,0x00000009,
    0x00000000,0x00000023,0x00000000,0x00050048,0x00000009,0x00000001,0x00000023,0x00004000,
    0x00030047,0x00000009,0x00000003,0x00020013,0x0000000a,0x00030021,0x0000000b,0x0000000a,
    0x00030016,0x0000000c,0x00000020,0x00040015,0x0000000d,0x00000020,0x00000000,0x00040015,
    0x0000000e,0x00000020,0x00000001,0x00020014,0x0000000f,0x00040017,0x00000010,0x0000000c,
    0x00000002,0x00040017,0x00000011,0x0000000c,0x00000004,0x00040017,0x00000012,0x0000000e,
    0x00000002,0x00030021,0x00000013,0x00000011,0x00030021,0x00000014,0x0000000c,0x00040020,
    0x00000015,0x00000003,0x00000011,0x00040020,0x00000016,0x00000001,0x00000010,0x00040020,
    0x00000017,0x00000007,0x0000000d,0x00040020,0x00000018,0x00000007,0x0000000c,0x00090019,
    0x00000019,0x0000000c,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,
    0x00040020,0x0000001a,0x00000000,0x00000019,0x0002001a,0x0000001b,0x00040020,0x0000001c,
    0x00000000,0x0000001b,0x0003001b,0x0000001d,0x00000019,0x00090019,0x0000001e,0x0000000c,
    0x00000006,0x00000000,0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x0000001f,
    0x00000000,0x0000001e,0x0004002b,0x0000000d,0x00000020,0x00000000,0x0004002b,0x0000000d,
    0x00000021,0x00000001,0x0004002b,0x0000000d,0x00000022,0x00000002,0x0004002b,0x0000000d,
    0x00000023,0x00000004,0x0004002b,0x0000000d,0x00000024,0x00000800,0x0004002b,0x0000000d,
    0x00000025,0x00001000,0x0004002b,0x0000000c,0x00000026,0x00000000,0x0004002b,0x0000000c,
    0x00000027,0x3f800000,0x0004001c,0x00000008,0x0000000d,0x00000025,0x0004001e,0x00000009,
    0x0000000d,0x00000008,0x00040020,0x00000028,0x00000002,0x00000009,0x00040020,0x00000029,
    0x00000002,0x0000000d,0x0004003b,0x00000028,0x00000007,0x00000002,0x0004003b,0x00000015,
    0x00000002,0x00000003,0x0004003b,0x00000016,0x00000003,0x00000001,0x0004003b,0x0000001a,
    0x00000004,0x00000000,0x0004003b,0x0000001c,0x00000005,0x00000000,0x0004003b,0x0000001f,
    0x00000006,0x00000000,0x0004002b,0x0000000e,0x0000002a,0x00000000,0x0005002c,0x00000012,
    0x0000002b,0x0000002a,0x0000002a,0x00030001,0x00000011,0x0000002c,0x00050036,0x00000011,
    0x0000002d,0x00000000,0x00000013,0x000200f8,0x0000002e,0x0004003d,0x00000019,0x0000002f,
    0x00000004,0x0004003d,0x0000001b,0x00000030,0x00000005,0x00050056,0x0000001d,0x00000031,
    0x0000002f,0x00000030,0x0004003d,0x00000010,0x00000032,0x00000003,0x00050057,0x00000011,
    0x00000033,0x00000031,0x00000032,0x000400cf,0x00000010,0x00000034,0x00000032,0x00050051,
    0x0000000c,0x00000035,0x00000034,0x00000000,0x0005008e,0x00000011,0x00000036,0x00000033,
    0x00000035,0x000200fe,0x00000036,0x00010038,0x00050036,0x0000000c,0x00000037,0x00000000,
    0x00000014,0x000200f8,0x00000038,0x0004003b,0x00000017,0x00000039,0x00000007,0x0004003b,
    0x00000018,0x0000003a,0x00000007,0x0003003e,0x00000039,0x00000020,0x0003003e,0x0000003a,
    0x00000026,0x000200f9,0x0000003b,0x000200f8,0x0000003b,0x000400f6,0x0000003c,0x0000003d,
    0x00000000,0x000200f9,0x0000003e,0x000200f8,0x0000003e,0x0004003d,0x0000000d,0x0000003f,
    0x00000039,0x000500b0,0x0000000f,0x00000040,0x0000003f,0x00000023,0x000400fa,0x00000040,
    0x00000041,0x0000003c,0x000200f8,0x00000041,0x0004003d,0x0000000d,0x00000042,0x00000039,
    0x000300f7,0x00000043,0x00000000,0x000700fb,0x00000042,0x00000044,0x00000000,0x00000045,
    0x00000001,0x00000046,0x000200f8,0x00000045,0x0004003d,0x0000000c,0x00000047,0x0000003a,
    0x00050081,0x0000000c,0x00000048,0x00000047,0x00000027,0x0003003e,0x0000003a,0x00000048,
    0x000200f9,0x00000043,0x000200f8,0x00000046,0x0004003d,0x0000000c,0x00000049,0x0000003a,
    0x00050081,0x0000000c,0x0000004a,0x00000049,0x00000027,0x0003003e,0x0000003a,0x0000004a,
    0x000200f9,0x00000043,0x000200f8,0x00000044,0x000200f9,0x00000043,0x000200f8,0x00000043,
    0x000200f9,0x0000003d,0x000200f8,0x0000003d,0x0004003d,0x0000000d,0x0000004b,0x00000039,
    0x00050080,0x0000000d,0x0000004c,0x0000004b,0x00000021,0x0003003e,0x00000039,0x0000004c,
    0x000200f9,0x0000003b,0x000200f8,0x0000003c,0x0004003d,0x0000000c,0x0000004d,0x0000003a,
    0x000200fe,0x0000004d,0x00010038,0x00050036,0x0000000a,0x00000001,0x00000000,0x0000000b,
    0x000200f8,0x0000004e,0x00040039,0x00000011,0x0000004f,0x0000002d,0x00040039,0x0000000c,
    0x00000050,0x00000037,0x0004003d,0x0000001e,0x00000051,0x00000006,0x00050062,0x00000011,
    0x00000052,0x00000051,0x0000002b,0x0009004f,0x00000011,0x00000053,0x00000052,0x0000002c,
    0x00000000,0x00000005,0x00000002,0x00000007,0x0005008e,0x00000011,0x00000054,0x00000053,
    0x00000050,0x00050081,0x00000011,0x00000055,0x00000054,0x0000004f,0x00050041,0x00000029,
    0x00000056,0x00000007,0x00000020,0x0004003d,0x0000000d,0x00000057,0x00000056,0x0005004d,
    0x0000000c,0x00000058,0x00000055,0x00000057,0x0005004d,0x0000000c,0x00000059,0x00000055,
    0x00000022,0x0006004e,0x00000011,0x0000005a,0x00000055,0x00000058,0x00000057,0x0004003d,
    0x00000010,0x0000005b,0x00000003,0x00050051,0x0000000c,0x0000005c,0x0000005b,0x00000000,
    0x00050085,0x0000000c,0x0000005d,0x00000059,0x0000005c,0x0005008e,0x00000011,0x0000005e,
    0x0000005a,0x0000005d,0x0003003e,0x00000002,0x0000005e,0x000100fd,0x00010038 };

typedef struct { char buf[300000]; size_t n; unsigned lines; } cap_t;
static void emit(void *u, const char *l) { cap_t *c = u; c->lines++; c->n += (size_t)snprintf(c->buf + c->n, sizeof c->buf - c->n, "%s\n", l); }
static VkResult no_switch(void *u, const char *row, const uint32_t *fs, size_t bytes, VkResult real) {   /* a "driver" that rejects every module that still contains an OpSwitch */
    (void)u; (void)row; (void)real;
    for (size_t p = 5; p < bytes / 4; p += fs[p] >> 16) if ((fs[p] & 0xffffu) == 251u) return VK_ERROR_UNKNOWN;
    return VK_SUCCESS;
}
static const char *find_row(const char *buf, const char *id, const char *result) {
    char key[160];
    snprintf(key, sizeof key, "row=%s result=%s", id, result);
    return strstr(buf, key);
}

int main(void) {
    static cap_t c0, c1, c2, c3, c4;
    VkDescriptorSetLayoutBinding b[4] = {
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 16, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 17, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 40, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
    lagfx_reduce_target_t tg;
    memset(&tg, 0, sizeof tg);
    tg.d0.color_format = VK_FORMAT_R16G16B16A16_SFLOAT, tg.d0.n_color = 1, tg.d0.masks_valid = 1, tg.d0.color_write_masks[0] = 0xf, tg.d0.color_write_mask = 0xf, tg.d0.blend_set = 1;
    tg.d0.vertex_entry_point = "main", tg.d0.fragment_entry_point = "main";
    tg.bind = b, tg.nbind = 4, tg.fs_entry = "main";

    /* ---- 0. engine, DRY, spirv-val ---- */
    {
        char dir[] = "/tmp/pipereduce-unit-XXXXXX";
        CHECK(mkdtemp(dir) != NULL, "mkdtemp");
        lagfx_reduce_cfg_t cfg = { .fs = k_fs, .fs_bytes = sizeof k_fs, .vs = lagfx_stub_vs_spv, .vs_bytes = sizeof lagfx_stub_vs_spv, .dump_dir = dir, .dry = 1, .emit = emit, .user = &c0, .target = &tg };
        lagfx_reduce_result_t r;
        lagfx_pipeline_reduce_run(&cfg, &r);
        CHECK(r.rows >= 60 && r.invalid == 0, "dry rows %u invalid %u\n%s", r.rows, r.invalid, c0.buf);
        const char *must[] = { "row=R0 ", "row=HALL ", "row=SMPN ", "row=LOD0 ", "row=DERIV ", "row=IADROP ", "row=SW-H ", "row=U1 ", "row=LP-H ", "row=UN ", "row=UD0 ", "row=UU-SHUF ", "row=DYN-C ", "row=DYN-S ", "row=S1 ", "row=S4 ",
                               "row=IFA ", "row=IFC ", "row=IFF ", "row=FI-ALL ", "row=FL-NOOPT ", "row=CH1:1 ", "row=RI0:", "row=GR0:1 ", "pipeline-reduce: stats ", "pipeline-reduce: audit " };
        for (unsigned i = 0; i < sizeof must / sizeof must[0]; i++) CHECK(strstr(c0.buf, must[i]), "missing '%s'\n%s", must[i], c0.buf);
        CHECK(strstr(c0.buf, "pipeline-reduce: smallest-failing row="), "smallest failing report");
        CHECK(strstr(c0.buf, "sha=") && strstr(c0.buf, "val=offline") && strstr(c0.buf, "[meaning-preserving]") && strstr(c0.buf, "[diagnostic: meaning changed]"), "row line fields");
        /* spirv-val every row file */
        int have_val = system("command -v spirv-val >/dev/null 2>&1") == 0;
        DIR *d = opendir(dir);
        unsigned nfile = 0, nbad = 0;
        for (struct dirent *e; d && (e = readdir(d));) {
            size_t l = strlen(e->d_name);
            if (l < 8 || strcmp(e->d_name + l - 7, ".fs.spv")) continue;
            char cmd[600];
            snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.2 %s/%s >/dev/null 2>&1", dir, e->d_name);
            nfile++;
            if (have_val && system(cmd) != 0) { printf("spirv-val FAILED: %s\n", e->d_name); nbad++; }
        }
        if (d) closedir(d);
        CHECK(nfile >= 60 && nbad == 0, "row files %u, spirv-val failures %u", nfile, nbad);
        printf("pipeline reduce: %u row files, spirv-val %s\n", nfile, have_val ? "all valid" : "not installed (skipped)");
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -r %s", dir);
        if (system(cmd)) {}
    }

    /* ---- 1. crash isolation: nothing of Vulkan exists in this process yet ---- */
    {
        setenv("LAGFX_PIPELINE_REDUCE_CRASH_ROW", "SMPN", 1);
        setenv("LAGFX_PIPELINE_REDUCE_HANG_ROW", "IFC", 1);
        lagfx_reduce_cfg_t cfg = { .fs = k_fs, .fs_bytes = sizeof k_fs, .vs = lagfx_stub_vs_spv, .vs_bytes = sizeof lagfx_stub_vs_spv, .only = "R0,HALL,SMPN,LOD0,IFC,IFA", .isolate = 1, .timeout_ms = 3000,
                                   .emit = emit, .user = &c1, .target = &tg };
        lagfx_reduce_result_t r;
        lagfx_pipeline_reduce_run(&cfg, &r);
        if (strstr(c1.buf, "result=BUILT") == NULL && strstr(c1.buf, "NO_DEVICE_IN_CHILD")) { printf("no usable Vulkan device in a forked child: skip\n"); return 77; }
        CHECK(find_row(c1.buf, "R0", "BUILT") && find_row(c1.buf, "HALL", "BUILT"), "rows before the crash ran in children:\n%s", c1.buf);
        CHECK(find_row(c1.buf, "SMPN", "CRASH") && strstr(c1.buf, "signal=11"), "the crashing row is CRASH(signal=11):\n%s", c1.buf);
        CHECK(find_row(c1.buf, "LOD0", "BUILT"), "the row AFTER the crash still ran:\n%s", c1.buf);
        CHECK(find_row(c1.buf, "IFC", "TIMEOUT"), "the hanging row is TIMEOUT:\n%s", c1.buf);
        CHECK(find_row(c1.buf, "IFA", "BUILT"), "the row after the timeout still ran:\n%s", c1.buf);
        CHECK(r.crash == 1 && r.timeout == 1 && strstr(c1.buf, "CRASH=[SMPN]") && strstr(c1.buf, "TIMEOUT=[IFC]"), "summary lists CRASH / TIMEOUT:\n%s", c1.buf);
        unsetenv("LAGFX_PIPELINE_REDUCE_CRASH_ROW"), unsetenv("LAGFX_PIPELINE_REDUCE_HANG_ROW");
        /* resume: --from SMPN skips R0 / HALL */
        lagfx_reduce_cfg_t cf2 = cfg;
        cf2.only = NULL, cf2.from = "LOD0", cf2.groups = "s", cf2.user = &c2;
        lagfx_pipeline_reduce_run(&cf2, &r);
        CHECK(!strstr(c2.buf, "row=R0 ") && !strstr(c2.buf, "row=SMPN ") && find_row(c2.buf, "LOD0", "BUILT") && find_row(c2.buf, "IADROP", "BUILT"), "--from LOD0 resumes at LOD0:\n%s", c2.buf);
        printf("pipeline reduce: crash isolation ok (CRASH, TIMEOUT, resume)\n");
    }

    /* ---- 2. in-process on lavapipe ---- */
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    char logpath[] = "/tmp/pipereduce-unit-log-XXXXXX";
    int lfd = mkstemp(logpath);
    CHECK(lfd >= 0, "mkstemp");
    close(lfd);
    setenv("LAGFX_LOG_FILE", logpath, 1);
    setenv("LAGFX_PIPELINE_REDUCE", "1", 1);
    setenv("LAGFX_PIPELINE_REDUCE_MAX", "40", 1);
    unsetenv("LAGFX_PIPELINE_MATRIX_DIR"), unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR"), unsetenv("LAGFX_PIPELINE_MATRIX");
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    VkDevice dev = vk->device;
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b };
    CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl) == VK_SUCCESS, "dsl");
    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    CHECK(vkCreatePipelineLayout(dev, &pi, NULL, &pl) == VK_SUCCESS, "pl");
    {
        lagfx_reduce_cfg_t cfg = { .fs = k_fs, .fs_bytes = sizeof k_fs, .vs = lagfx_stub_vs_spv, .vs_bytes = sizeof lagfx_stub_vs_spv, .max_rows = 400, .dev = dev, .layout = pl, .emit = emit, .user = &c3, .target = &tg };
        lagfx_reduce_result_t r;
        lagfx_pipeline_reduce_run(&cfg, &r);
        CHECK(r.fail == 0 && r.crash == 0 && r.timeout == 0 && r.invalid == 0 && r.built >= 40, "lavapipe: every applicable row BUILT (built %u fail %u invalid %u)\n%s", r.built, r.fail, r.invalid, c3.buf);
        CHECK(strstr(c3.buf, "R0=BUILT") && strstr(c3.buf, "FAIL=[]"), "summary:\n%s", c3.buf);
        lagfx_reduce_cfg_t c4cfg = cfg;
        c4cfg.test_override = no_switch, c4cfg.user = &c4;
        lagfx_pipeline_reduce_run(&c4cfg, &r);
        CHECK(find_row(c4.buf, "R0", "FAIL") && find_row(c4.buf, "HALL", "BUILT") && find_row(c4.buf, "SW-H", "BUILT") && find_row(c4.buf, "U1", "BUILT") && find_row(c4.buf, "LP-H", "FAIL"), "simulated driver: switch-dependent rows:\n%s", c4.buf);
        CHECK(strstr(c4.buf, "ddmin result: helpers that must stay for the failure (everything else stubbed): 1:") , "ddmin finds the one helper:\n%s", c4.buf);
        CHECK(strstr(c4.buf, "smallest-failing row=") && strstr(c4.buf, "greedy chain result mask="), "smallest failing + greedy chain:\n%s", c4.buf);
        printf("pipeline reduce: in-process ladder ok (%u rows, %u built)\n", r.rows, r.built);
    }
    /* ---- 3. production hook ---- */
    {
        lagfx_pipeline_desc_t f;
        memset(&f, 0, sizeof f);
        f.layout = pl, f.color_format = VK_FORMAT_R16G16B16A16_SFLOAT, f.n_color = 1, f.masks_valid = 1, f.color_write_masks[0] = 0xf, f.color_write_mask = 0xf, f.blend_set = 1;
        f.vertex_spv = lagfx_stub_vs_spv, f.vertex_spv_bytes = sizeof lagfx_stub_vs_spv, f.fragment_spv = k_fs, f.fragment_spv_bytes = sizeof k_fs, f.vertex_entry_point = "main", f.fragment_entry_point = "no_such_entry";
        for (int pass = 0; pass < 2; pass++) {
            VkShaderModule vm, fm;
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof lagfx_stub_vs_spv, .pCode = lagfx_stub_vs_spv };
            CHECK(vkCreateShaderModule(dev, &ci, NULL, &vm) == VK_SUCCESS, "vm");
            ci.codeSize = sizeof k_fs, ci.pCode = k_fs;
            CHECK(vkCreateShaderModule(dev, &ci, NULL, &fm) == VK_SUCCESS, "fm");
            f.vertex_shader = vm, f.fragment_shader = fm;
            VkPipeline p = VK_NULL_HANDLE;
            CHECK(lagfx_pipeline_build(dev, &f, &p) == LAGFX_ERR_BACKEND, "forced failure %d", pass);
            vkDestroyShaderModule(dev, vm, NULL), vkDestroyShaderModule(dev, fm, NULL);
        }
        FILE *fl = fopen(logpath, "r");
        static char all[400000];
        size_t n = fl ? fread(all, 1, sizeof all - 1, fl) : 0;
        all[n] = 0;
        if (fl) fclose(fl);
        unlink(logpath);
        unsigned begins = 0;
        for (const char *q = all; (q = strstr(q, "pipeline-reduce: begin fs=")); q++) begins++;
        CHECK(strstr(all, "pipeline-reduce: ENABLED") && begins == 1, "hook enabled, the repeated module is silent: begins %u", begins);
        CHECK(strstr(all, "pipeline-reduce: row=R0 result=FAIL"), "R0 reproduces the forced failure");
        printf("pipeline reduce: production hook ok\n");
    }
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    lagfx_vk_shutdown(vk);
    printf("pipeline reduce: ok\n");
    return 0;
}
