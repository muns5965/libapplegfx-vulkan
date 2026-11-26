/*
 * libapplegfx-vulkan — lagfx_pipeline_repro_run (lavapipe)
 * tests/pipeline-repro-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 *  1. A failure is FORCED (the fragment entry point names a function the module lacks: module creation works, pipeline creation does not), the bundle is written by the
 *     failing lagfx_pipeline_build(), and lagfx_pipeline_repro_run() on that bundle reproduces the SAME VkResult three times in a row. The variant matrix (opt-in) pins the
 *     failure on the fragment stage (real vertex + stub fragment builds, stub vertex + real fragment fails) and is absent without the flag.
 *  2. The UberComposite vertex + fragment fixtures (the CoreAnimation composite pair, translated as the resolver does: vertex first, fragment with the vertex keys,
 *     fragment bindings offset) are built directly, their bundle is written and the repro reproduces OK, with the descriptor layout and vertex input rebuilt from the bundle.
 *  3. Optional: LAGFX_TEST_DUMP_DIR=<real device dump> also rebuilds the two device failures (VfxXgh + A2Xghfc, vertexFetchFunction + UberCompositeFragment; colour formats
 *     97 and 44) and prints what lavapipe does with them (informational: the Adreno failure is a driver result).
 */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { return 77; }
#else
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/translate.h"
#include "protocol/arm64_pipeline.h"
#include "vulkan/descriptor_layout.h"
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/pipeline_bundle.h"
#include <dirent.h>
#include <unistd.h>
#define k_vs_spv k_vs_vin_spv
#define k_fs_spv k_fs_vin_spv
#include "arm64-exec-run-shaders.h"
#undef k_vs_spv
#undef k_fs_spv

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static uint8_t *fixture(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) { snprintf(path, sizeof path, "%s%s", dirs[d], name); uint8_t *b = slurp(path, n); if (b) return b; }
    return NULL;
}
static int count_bundles(const char *dir, char *first, size_t cap) {
    int n = 0; DIR *d = opendir(dir); struct dirent *e;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, "pipefail_", 9)) { if (first && (!n || strcmp(e->d_name, first + strlen(dir) + 1) > 0)) snprintf(first, cap, "%s/%s", dir, e->d_name); n++; }
    if (d) closedir(d);
    return n;
}
static void rmrf(const char *dir) { char cmd[1500]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {} }

/* resolver order: vertex (keys out), fragment (keys in), fragment bindings offset */
static int translate_pair(const uint8_t *vbc, size_t vn, const uint8_t *fbc, size_t fn, uint8_t **vs, size_t *vsn, uint8_t **fs, size_t *fsn) {
    lagfx_air_module_t *vm = NULL, *fm = NULL; lagfx_stage_keys_t k; memset(&k, 0, sizeof k);
    CHECK(lagfx_air_module_open(vbc, vn, &vm) == LAGFX_OK && lagfx_air_module_open(fbc, fn, &fm) == LAGFX_OK, "module_open");
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    CHECK(lagfx_air2spv_translate_module_ex(vm, NULL, vs, vsn) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &k, fs, fsn) == LAGFX_OK, "translate");
    lagfx_spv_offset_bindings(*fs, *fsn, LAGFX_A64_FRAG_BINDING_BASE);
    lagfx_air_module_free(vm); lagfx_air_module_free(fm);
    return 0;
}

/* Builds the pair directly (modules + layout from the SPIR-V + vertex input from reflection), writes its bundle (whatever the result) and returns the direct VkResult. */
static int direct_and_bundle(struct lagfx_vk_state *vk, const uint8_t *vs, size_t vsn, const uint8_t *fs, size_t fsn, VkFormat fmt, const uint32_t *attr_fmt, const uint32_t *attr_off, unsigned nattr, unsigned stride,
                             const char *origin, int32_t *direct, char *bundle, size_t bcap, const char *base) {
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    lagfx_spv_vertex_input_t vin[8];
    size_t nvi = lagfx_spv_reflect_vertex_inputs(vs, vsn, vin, 8);
    if (nvi > 8) nvi = 8;
    for (size_t i = 0; i < nvi; i++) d.vtx_in_loc[i] = (uint8_t)vin[i].location, d.vtx_in_comp[i] = (uint8_t)vin[i].components;
    d.n_vtx_inputs = (uint8_t)nvi;
    if (nvi && nattr >= nvi) { d.n_pso_attrs = (uint8_t)nattr; for (unsigned i = 0; i < nattr; i++) d.pso_attr_fmt[i] = attr_fmt[i], d.pso_attr_off[i] = attr_off[i]; d.vtx_stride = stride; }
    const uint8_t *blobs[2] = { vs, fs };
    const size_t lens[2] = { vsn, fsn };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    CHECK(lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 2, &dsl, &pl) == LAGFX_OK, "layout");
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsn, .pCode = (const uint32_t *)vs };
    CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &vm) == VK_SUCCESS, "vs module");
    ci.codeSize = fsn, ci.pCode = (const uint32_t *)fs;
    CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &fm) == VK_SUCCESS, "fs module");
    lagfx_pipeline_diag_t dg = { .vk = vk, .origin = origin };
    d.vertex_shader = vm, d.fragment_shader = fm, d.layout = pl ? pl : vk->empty_layout, d.color_format = fmt, d.caps_enabled = vk->caps_enabled, d.diag = &dg;
    d.vertex_spv = (const uint32_t *)vs, d.vertex_spv_bytes = vsn, d.fragment_spv = (const uint32_t *)fs, d.fragment_spv_bytes = fsn;
    VkPipeline p = VK_NULL_HANDLE;
    lagfx_pipeline_bundle_suppress(1);
    lagfx_status_t st = lagfx_pipeline_build(vk->device, &d, &p);
    lagfx_pipeline_bundle_suppress(0);
    *direct = st == LAGFX_OK ? 0 : lagfx_pipeline_build_last_result();
    if (p) vkDestroyPipeline(vk->device, p, NULL);
    setenv("LAGFX_PIPEFAIL_BUNDLE_DIR", base, 1);
    lagfx_pipeline_bundle_reset();
    CHECK(lagfx_pipeline_bundle_write(&d, (VkResult)*direct, "# test bundle", bundle, bcap), "bundle written");
    vkDestroyShaderModule(vk->device, vm, NULL), vkDestroyShaderModule(vk->device, fm, NULL);
    if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
    return 0;
}

int main(void) {
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t desc;
    memset(&desc, 0, sizeof desc);
    if (lagfx_vk_init(&vk, &desc) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    char tmpl[] = "/tmp/piperepro-unit-XXXXXX";
    char *base = mkdtemp(tmpl);
    CHECK(base, "mkdtemp");
    unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR"), unsetenv("LAGFX_ARM64_DUMP"), unsetenv("LAGFX_ARM64_DUMP_DIR");

    /* ---- 1. forced failure -> bundle -> repro ---- */
    {
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_vin_spv, .pCode = k_vs_vin_spv };
        CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &vs) == VK_SUCCESS, "vs");
        ci.codeSize = sizeof k_fs_vin_spv, ci.pCode = k_fs_vin_spv;
        CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &fs) == VK_SUCCESS, "fs");
        lagfx_pipeline_diag_t dg = { .vk = vk, .origin = "repro unit: forced failure" };
        lagfx_pipeline_desc_t d;
        memset(&d, 0, sizeof d);
        d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = VK_FORMAT_R16G16B16A16_SFLOAT, d.fragment_entry_point = "no_such_entry";
        d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
        d.vertex_spv = k_vs_vin_spv, d.vertex_spv_bytes = sizeof k_vs_vin_spv, d.fragment_spv = k_fs_vin_spv, d.fragment_spv_bytes = sizeof k_fs_vin_spv, d.caps_enabled = vk->caps_enabled, d.diag = &dg;
        setenv("LAGFX_PIPEFAIL_BUNDLE_DIR", base, 1);
        lagfx_pipeline_bundle_reset();
        VkPipeline pl = VK_NULL_HANDLE;
        CHECK(lagfx_pipeline_build(vk->device, &d, &pl) == LAGFX_ERR_BACKEND, "forced failure");
        int32_t direct = lagfx_pipeline_build_last_result();
        CHECK(direct != 0, "the driver result of the forced failure is an error (%d)", direct);
        unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR");
        vkDestroyShaderModule(vk->device, vs, NULL), vkDestroyShaderModule(vk->device, fs, NULL);
        char b1[1400] = "";
        CHECK(count_bundles(base, b1, sizeof b1) == 1, "bundle written by the failing build");

        lagfx_pipeline_repro_result_t r;
        char *rep = NULL;
        lagfx_pipeline_repro_opts_t o = { .struct_size = sizeof o, .flags = 0 };
        CHECK(lagfx_pipeline_repro_run(b1, &o, &r, &rep) == LAGFX_OK && rep, "repro ran");
        CHECK(r.recorded_result == direct && r.vk_result == direct && r.as_is_consistent && r.reproduced, "repro reproduces the recorded result %d: got %d consistent=%u reproduced=%u\n%s", direct, r.vk_result, r.as_is_consistent, r.reproduced, rep);
        CHECK(strstr(rep, "the recorded failure REPRODUCES") && strstr(rep, "AS-IS REBUILD x3") && strstr(rep, "same device and driver") && !strstr(rep, "variant matrix"), "report content:\n%s", rep);
        CHECK(!strstr(rep, "ENV DIFFERENCE"), "no env difference:\n%s", rep);
        lagfx_pipeline_repro_free_report(rep);
        CHECK(count_bundles(base, NULL, 0) == 1, "the repro writes no bundle of its own");
        /* again, same answer */
        CHECK(lagfx_pipeline_repro_run(b1, &o, &r, NULL) == LAGFX_OK && r.reproduced && r.vk_result == direct, "second repro run, no report requested");
        /* with the option on in the environment the repro still writes no bundle */
        setenv("LAGFX_PIPEFAIL_BUNDLE_DIR", base, 1);
        CHECK(lagfx_pipeline_repro_run(b1, &o, &r, NULL) == LAGFX_OK && r.reproduced && count_bundles(base, NULL, 0) == 1, "option on: repro stays quiet");
        unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR");

        /* the matrix */
        o.flags = LAGFX_PIPELINE_REPRO_MATRIX;
        CHECK(lagfx_pipeline_repro_run(b1, &o, &r, &rep) == LAGFX_OK && rep, "matrix ran");
        CHECK(strstr(rep, "variant matrix (DIAGNOSTIC ONLY") && strstr(rep, "DIAGNOSTIC: changes meaning"), "matrix header and labels:\n%s", rep);
        { char *d0 = strstr(rep, "d0  "), *e0 = d0 ? strchr(d0, '\n') : NULL; CHECK(d0 && e0 && memmem(d0, (size_t)(e0 - d0), "OK", 2), "d0 (both stubs) builds: the state alone is fine:\n%s", rep); }
        { char *d1 = strstr(rep, "d1  "), *e1 = d1 ? strchr(d1, '\n') : NULL; CHECK(d1 && e1 && memmem(d1, (size_t)(e1 - d1), "OK", 2), "d1 (real vertex + stub fragment) builds: the vertex stage is fine:\n%s", rep); }
        { char *d2 = strstr(rep, "d2  "), *e2 = d2 ? strchr(d2, '\n') : NULL; CHECK(d2 && e2 && memmem(d2, (size_t)(e2 - d2), "FAIL(", 5), "d2 (stub vertex + real fragment) fails: the fragment stage is the one:\n%s", rep); }
        CHECK(strstr(rep, "colour format 97 alone (recorded)") && strstr(rep, "colour format 44 alone (extra)") && strstr(rep, "c1 ") && strstr(rep, "c2 ") && strstr(rep, "c3 "), "b / c variants:\n%s", rep);
        CHECK(r.variants_ok > 0 && r.variants_failed > 0 && r.reproduced, "matrix counters %u/%u/%u", r.variants_ok, r.variants_failed, r.variants_na);
        lagfx_pipeline_repro_free_report(rep);
        CHECK(lagfx_pipeline_repro_run("/nonexistent/bundle", &o, &r, &rep) == LAGFX_ERR_NOT_FOUND && rep && strstr(rep, "cannot load the bundle"), "missing bundle");
        lagfx_pipeline_repro_free_report(rep);
        CHECK(lagfx_pipeline_repro_run(NULL, NULL, NULL, NULL) == LAGFX_ERR_INVALID_ARG, "NULL dir");
    }
    rmrf(base);
    char tmpl2[] = "/tmp/piperepro-unit-XXXXXX";
    base = mkdtemp(tmpl2);
    CHECK(base, "mkdtemp 2");

    /* ---- 2. the UberComposite fixture pair: the bundle of a pair that BUILDS reproduces OK ---- */
    {
        size_t vn, fn;
        uint8_t *vbc = fixture("login_ubercomposite_vert.air.bc", &vn), *fbc = fixture("login_ubercomposite_frag_a.air.bc", &fn);
        CHECK(vbc && fbc, "UberComposite fixtures");
        uint8_t *vs = NULL, *fs = NULL; size_t vsn = 0, fsn = 0;
        if (translate_pair(vbc, vn, fbc, fn, &vs, &vsn, &fs, &fsn)) return 1;
        const uint32_t af[4] = { 31, 29, 29, 31 }, ao[4] = { 0, 16, 24, 32 };   /* Float4 @0, Float2 @16, Float2 @24, Float4 @32 of a 48 byte vertex (the device pair-2 layout) */
        int32_t direct = -99;
        char bundle[1400] = "";
        if (direct_and_bundle(vk, vs, vsn, fs, fsn, VK_FORMAT_R16G16B16A16_SFLOAT, af, ao, 4, 48, "unit: UberComposite fixture pair", &direct, bundle, sizeof bundle, base)) return 1;
        unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR");
        printf("UberComposite fixture pair: vs %zu bytes %016llx, fs %zu bytes %016llx, direct build result %d\n", vsn, (unsigned long long)lagfx_pipeline_spv_hash(vs, vsn), fsn, (unsigned long long)lagfx_pipeline_spv_hash(fs, fsn), direct);
        CHECK(direct == 0, "the UberComposite pair builds on lavapipe (got %d)", direct);
        lagfx_pipeline_repro_result_t r;
        char *rep = NULL;
        lagfx_pipeline_repro_opts_t o = { .struct_size = sizeof o, .flags = LAGFX_PIPELINE_REPRO_MATRIX };
        CHECK(lagfx_pipeline_repro_run(bundle, &o, &r, &rep) == LAGFX_OK && rep, "repro of the real pair ran");
        CHECK(r.vk_result == 0 && r.recorded_result == 0 && r.as_is_consistent && r.reproduced && strstr(rep, "the recorded SUCCESS reproduces"), "the real pair reproduces OK:\n%s", rep);
        CHECK(strstr(rep, "layout: 1 set(s)") && strstr(rep, "created"), "layout rebuilt from the bundle:\n%s", rep);
        CHECK(strstr(rep, "a    as-is") && r.variants_ok >= 5, "matrix on the real pair:\n%s", rep);
        printf("%s", rep);
        lagfx_pipeline_repro_free_report(rep);
        free(vs); free(fs); free(vbc); free(fbc);
    }

    /* ---- 3. optional: the real device dump ---- */
    const char *dump = getenv("LAGFX_TEST_DUMP_DIR");
    if (dump) {
        static const struct { const char *name, *vlib, *flib; unsigned nattr; uint32_t af[4], ao[4]; unsigned stride; uint64_t vh, fh; } pairs[2] = {
            { "VfxXgh + A2Xghfc", "445d43e9ba751f49", "35024e60e414abf4", 0, {0}, {0}, 0, 0x22942f5e92793ddeull, 0xce5c70c189b74076ull },   /* the hash= of the device log line (vs 10268 / fs 346780 bytes) */
            { "vertexFetchFunction(fa13d506) + UberCompositeFragment", "fa13d506019b4303", "a2cf8265420e7293", 4, { 31, 29, 29, 31 }, { 0, 16, 24, 32 }, 48, 0x5fbcd5a03563d1eaull, 0x21043becea32bddcull } };   /* device log pair 2 (vs 2252 / fs 1556 bytes) */
        for (int p = 0; p < 2; p++) {
            uint8_t *bc[2] = { NULL, NULL }; size_t bn[2] = { 0, 0 };
            for (int s = 0; s < 2; s++) {
                char path[1400]; size_t ln = 0;
                snprintf(path, sizeof path, "%s/lib_%s.bin", dump, s ? pairs[p].flib : pairs[p].vlib);
                uint8_t *lib = slurp(path, &ln);
                lagfx_metallib_function_t fnv[8]; size_t nf = 0;
                if (lib && lagfx_metallib_extract_functions(lib, ln, fnv, 8, &nf) == LAGFX_OK) for (size_t k = 0; k < nf; k++) if (fnv[k].bitcode && fnv[k].stage == (s ? LAGFX_METALLIB_STAGE_FRAGMENT : LAGFX_METALLIB_STAGE_VERTEX)) {
                    bc[s] = malloc(fnv[k].bitcode_len); memcpy(bc[s], fnv[k].bitcode, fnv[k].bitcode_len); bn[s] = fnv[k].bitcode_len; break; }
                free(lib);
            }
            if (!bc[0] || !bc[1]) { printf("dump pair %s: libraries not found in %s, skipped\n", pairs[p].name, dump); free(bc[0]); free(bc[1]); continue; }
            uint8_t *vs = NULL, *fs = NULL; size_t vsn = 0, fsn = 0;
            if (translate_pair(bc[0], bn[0], bc[1], bn[1], &vs, &vsn, &fs, &fsn)) return 1;
            CHECK(lagfx_pipeline_spv_hash(vs, vsn) == pairs[p].vh && lagfx_pipeline_spv_hash(fs, fsn) == pairs[p].fh, "dump pair %s: the translation is byte-identical to the device's modules (hash=)", pairs[p].name);
            static const VkFormat fmts[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM };
            for (int f = 0; f < 2; f++) {
                int32_t direct = -99; char bundle[1400] = "";
                if (direct_and_bundle(vk, vs, vsn, fs, fsn, fmts[f], pairs[p].af, pairs[p].ao, pairs[p].nattr, pairs[p].stride, pairs[p].name, &direct, bundle, sizeof bundle, base)) return 1;
                unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR");
                lagfx_pipeline_repro_result_t r; char *rep = NULL;
                lagfx_pipeline_repro_opts_t o = { .struct_size = sizeof o, .flags = 0 };
                CHECK(lagfx_pipeline_repro_run(bundle, &o, &r, &rep) == LAGFX_OK, "dump pair repro");
                printf("dump pair %s, colour format %d: vs %zu bytes %016llx, fs %zu bytes %016llx: lavapipe result %d (device log: -13); repro consistent=%u reproduced=%u\n", pairs[p].name, (int)fmts[f], vsn, (unsigned long long)lagfx_pipeline_spv_hash(vs, vsn), fsn, (unsigned long long)lagfx_pipeline_spv_hash(fs, fsn), r.vk_result, r.as_is_consistent, r.reproduced);
                CHECK(r.as_is_consistent && r.reproduced, "dump pair repro is consistent with the direct build");
                lagfx_pipeline_repro_free_report(rep);
                rmrf(bundle);
            }
            free(vs); free(fs); free(bc[0]); free(bc[1]);
        }
    }
    lagfx_vk_shutdown(vk);
    rmrf(base);
    printf("pipeline repro: ok\n");
    return 0;
}
#endif
