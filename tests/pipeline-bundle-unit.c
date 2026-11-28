/*
 * libapplegfx-vulkan — pipeline failure bundle (lavapipe)
 * tests/pipeline-bundle-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * A failing vkCreateGraphicsPipelines is forced with a deliberately invalid state (the fragment entry point names a function the module does not have: the shader
 * MODULE is created fine, the pipeline is not -> the same VK_ERROR_UNKNOWN class as the device). With the option off nothing is written and the call behaves as
 * before; with LAGFX_PIPEFAIL_BUNDLE_DIR set the first 8 distinct failures write pipefail_<hash>/{state,vs.spv,fs.spv,vs.bc,fs.bc}, a repeat writes nothing, and
 * the state file carries the descriptor, layout, device and environment records the repro needs.
 */
#include "libapplegfx-vulkan.h"
#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { return 77; }
#else
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/pipeline_bundle.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define k_vs_spv k_vs_vin_spv
#define k_fs_spv k_fs_vin_spv
#include "arm64-exec-run-shaders.h"
#undef k_vs_spv
#undef k_fs_spv

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static int count_bundles(const char *dir, char *first, size_t cap) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, "pipefail_", 9)) { if (!n && first) snprintf(first, cap, "%s/%s", dir, e->d_name); n++; }
    if (d) closedir(d);
    return n;
}
static char *slurp(const char *dir, const char *name, size_t *n) {
    char path[1400]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)l + 1); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); b[l] = 0; *n = (size_t)l; return b;
}

int main(void) {
    {   /* selective capture: LAGFX_AIR_CAPTURE_DIR alone selects the bundle directory (no full dump needed); LAGFX_PIPEFAIL_BUNDLE_DIR still wins */
        unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR"); unsetenv("LAGFX_AIR_CAPTURE_DIR");
        const char *none = lagfx_pipeline_bundle_dir();
        setenv("LAGFX_AIR_CAPTURE_DIR", "/tmp/air-cap-unit", 1);
        const char *cap = lagfx_pipeline_bundle_dir();
        setenv("LAGFX_PIPEFAIL_BUNDLE_DIR", "/tmp/bundle-unit", 1);
        const char *both = lagfx_pipeline_bundle_dir();
        CHECK(none == NULL, "no env -> no bundle dir");
        CHECK(cap && strcmp(cap, "/tmp/air-cap-unit") == 0, "LAGFX_AIR_CAPTURE_DIR selects the dir");
        CHECK(both && strcmp(both, "/tmp/bundle-unit") == 0, "explicit bundle dir wins");
        unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR"); unsetenv("LAGFX_AIR_CAPTURE_DIR");
    }
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t desc;
    memset(&desc, 0, sizeof desc);
    if (lagfx_vk_init(&vk, &desc) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    VkDevice dev = vk->device;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_vin_spv, .pCode = k_vs_vin_spv };
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &vs) == VK_SUCCESS, "vs module");
    ci.codeSize = sizeof k_fs_vin_spv, ci.pCode = k_fs_vin_spv;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &fs) == VK_SUCCESS, "fs module");
    const uint8_t bc[16] = "AIRBITCODE-STUB";
    lagfx_pipeline_diag_t dg = { .vk = vk, .vs_bc = bc, .vs_bc_bytes = sizeof bc, .fs_bc = bc, .fs_bc_bytes = sizeof bc, .vs_fn = "vsfn", .fs_fn = "fsfn", .origin = "unit test" };
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    d.vertex_spv = k_vs_vin_spv, d.vertex_spv_bytes = sizeof k_vs_vin_spv, d.fragment_spv = k_fs_vin_spv, d.fragment_spv_bytes = sizeof k_fs_vin_spv;
    d.caps_enabled = vk->caps_enabled, d.diag = &dg;

    char tmpl[] = "/tmp/pipefail-unit-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir, "mkdtemp");
    unsetenv("LAGFX_PIPEFAIL_BUNDLE_DIR"), unsetenv("LAGFX_ARM64_DUMP"), unsetenv("LAGFX_ARM64_DUMP_DIR");
    lagfx_pipeline_bundle_reset();

    /* a valid state builds and writes nothing, option on or off */
    VkPipeline pl = VK_NULL_HANDLE;
    CHECK(lagfx_pipeline_build(dev, &d, &pl) == LAGFX_OK && pl, "valid build");
    vkDestroyPipeline(dev, pl, NULL);

    lagfx_pipeline_desc_t bad = d;
    bad.fragment_entry_point = "no_such_entry";   /* the deliberately invalid state */
    pl = VK_NULL_HANDLE;
    CHECK(lagfx_pipeline_build(dev, &bad, &pl) == LAGFX_ERR_BACKEND && !pl, "invalid state must fail (option off)");
    CHECK(count_bundles(dir, NULL, 0) == 0 && !lagfx_pipeline_bundle_dir(), "option off: nothing written");

    setenv("LAGFX_PIPEFAIL_BUNDLE_DIR", dir, 1);
    CHECK(lagfx_pipeline_bundle_dir() && !strcmp(lagfx_pipeline_bundle_dir(), dir), "option on");
    CHECK(lagfx_pipeline_build(dev, &d, &pl) == LAGFX_OK && pl, "valid build, option on");
    vkDestroyPipeline(dev, pl, NULL);
    CHECK(count_bundles(dir, NULL, 0) == 0, "a successful build writes no bundle");
    pl = VK_NULL_HANDLE;
    CHECK(lagfx_pipeline_build(dev, &bad, &pl) == LAGFX_ERR_BACKEND && !pl, "invalid state must still fail (option on): behaviour unchanged");
    char b1[1400];
    CHECK(count_bundles(dir, b1, sizeof b1) == 1, "one bundle after one failure");
    CHECK(lagfx_pipeline_build(dev, &bad, &pl) == LAGFX_ERR_BACKEND, "repeat fails");
    CHECK(count_bundles(dir, NULL, 0) == 1, "a repeat of the same failure writes no second bundle");

    /* bundle contents */
    size_t n = 0;
    char *vsb = slurp(b1, "vs.spv", &n);
    CHECK(vsb && n == sizeof k_vs_vin_spv && !memcmp(vsb, k_vs_vin_spv, n), "vs.spv is the module as handed to the driver");
    free(vsb);
    char *fsb = slurp(b1, "fs.spv", &n);
    CHECK(fsb && n == sizeof k_fs_vin_spv && !memcmp(fsb, k_fs_vin_spv, n), "fs.spv is the module as handed to the driver");
    free(fsb);
    char *bcb = slurp(b1, "vs.bc", &n);
    CHECK(bcb && n == sizeof bc && !memcmp(bcb, bc, n), "vs.bc (AIR bitcode) present");
    free(bcb);
    char *st = slurp(b1, "state", &n);
    CHECK(st && n < 65536, "state file, bounded");
    static const char *const need[] = { "lagfx-pipefail-bundle 1\n", "result ", "translator ", "origin unit test\n", "fs_entry no_such_entry\n", "vs_hash ", "desc.color_format 44\n", "desc.vtx_stride 8\n", "desc.n_vtx_inputs 1\n",
        "desc.dyn_state 0\n", "desc.slot_blend 3 ", "layout.sets 0\n", "spec_constants 0\n", "dev.name ", "dev.driver_version ", "dev.api_version ", "dev.extensions ", "dev.features shaderInt64=", "dev.caps_enabled ",
        "env.LAGFX_VTX_STRIDE ", "# vertex_attr loc=0 binding=0 format=", "# rendering viewMask=0 colorAttachments=1", "# dynamic_states ", "# stage 16 module=handle entry=no_such_entry" };
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) CHECK(strstr(st, need[i]), "state lacks \"%s\"", need[i]);
    free(st);

    /* the loader parses what the writer wrote */
    lagfx_bundle_t bl;
    char err[200] = "";
    CHECK(lagfx_bundle_load(b1, &bl, err, sizeof err), "load: %s", err);
    CHECK(bl.result == (int)VK_ERROR_UNKNOWN || bl.result < 0, "recorded result %d", bl.result);
    CHECK(!strcmp(bl.fs_entry, "no_such_entry") && !strcmp(bl.vs_entry, "main") && bl.desc.color_format == VK_FORMAT_B8G8R8A8_UNORM && bl.desc.vtx_stride == 8 && bl.desc.n_vtx_inputs == 1 && bl.vs_bytes == sizeof k_vs_vin_spv, "loaded fields");
    CHECK(bl.have_dev && bl.dev_name[0] && bl.dev_caps == vk->caps_enabled && bl.vs_hash == lagfx_pipeline_spv_hash(k_vs_vin_spv, sizeof k_vs_vin_spv), "loaded device record / hashes");
    lagfx_bundle_free(&bl);

    /* bounded: at most 8 distinct failures per process */
    for (unsigned i = 0; i < 12; i++) { lagfx_pipeline_desc_t v = bad; v.vtx_stride = 8 + 4 * i; pl = VK_NULL_HANDLE; CHECK(lagfx_pipeline_build(dev, &v, &pl) == LAGFX_ERR_BACKEND, "variant %u fails", i); }
    int nb = count_bundles(dir, NULL, 0);
    CHECK(nb == (int)LAGFX_BUNDLE_MAX_FAILURES, "bundles bounded to %u (got %d)", LAGFX_BUNDLE_MAX_FAILURES, nb);

    vkDestroyShaderModule(dev, vs, NULL), vkDestroyShaderModule(dev, fs, NULL);
    lagfx_vk_shutdown(vk);
    printf("pipeline bundle: ok (%s)\n", b1);
    char cmd[1500]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    return system(cmd) ? 0 : 0;
}
#endif
