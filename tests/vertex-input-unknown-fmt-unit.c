/* P06: an unknown Metal vertex format affects only its own attribute and fails the pipeline build with a defined status; it no longer re-packs every attribute as
 * float. Hardening only (no unknown format occurs in the device dumps). SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include "vulkan/pipeline_build.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { return 77; }
#else
void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

static int bad;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); bad++; } } while (0)

static lagfx_pipeline_desc_t base(void) {
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.n_vtx_inputs = 3;
    for (int i = 0; i < 3; i++) d.vtx_in_loc[i] = (uint8_t)i, d.vtx_in_comp[i] = 4;
    d.pso_attr_fmt[0] = 31; d.pso_attr_off[0] = 0;    /* Float4 */
    d.pso_attr_fmt[1] = 29; d.pso_attr_off[1] = 16;   /* Float2 */
    d.pso_attr_fmt[2] = 9;  d.pso_attr_off[2] = 24;   /* UChar4Normalized */
    d.n_pso_attrs = 3; d.vtx_stride = 48;
    return d;
}

int main(void) {
    VkVertexInputBindingDescription vb; VkVertexInputAttributeDescription va[8]; uint32_t n = 0, badi = 99;

    lagfx_pipeline_desc_t d = base();
    CHECK(lagfx_pipeline_vertex_input(&d, &vb, va, &n, &badi), "all-known formats succeed");
    CHECK(n == 3 && va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[1].format == VK_FORMAT_R32G32_SFLOAT && va[2].format == VK_FORMAT_B8G8R8A8_UNORM, "known formats map");
    CHECK(va[1].offset == 16 && va[2].offset == 24 && vb.stride == 48, "known offsets and stride kept");

    /* middle attribute unknown: only it is affected, the others keep their format and offset */
    d = base(); d.pso_attr_fmt[1] = 999;
    badi = 99;
    CHECK(!lagfx_pipeline_vertex_input(&d, &vb, va, &n, &badi), "unknown format is reported");
    CHECK(badi == 1, "the unknown attribute's index is reported");
    CHECK(va[1].format == VK_FORMAT_UNDEFINED, "the unknown attribute is not guessed");
    CHECK(va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[0].offset == 0, "attribute 0 untouched");
    CHECK(va[2].format == VK_FORMAT_B8G8R8A8_UNORM && va[2].offset == 24, "attribute 2 keeps its format (old code re-packed it as R32G32B32A32_SFLOAT @8)");

    /* the first of two unknown attributes is the one reported */
    d = base(); d.pso_attr_fmt[0] = 0; d.pso_attr_fmt[2] = 777; badi = 99;
    CHECK(!lagfx_pipeline_vertex_input(&d, &vb, va, &n, &badi) && badi == 0, "first unknown attribute reported");

    /* PSO attributes not decoded: reflected tight-pack, unchanged */
    d = base(); d.n_pso_attrs = 0; d.pso_attr_fmt[1] = 999;
    CHECK(lagfx_pipeline_vertex_input(&d, &vb, va, &n, &badi) && va[1].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[1].offset == 16, "undecoded PSO keeps the tight float pack");

    /* the builder fails the PSO with a defined status before touching the device (dummy handles: the old code went on to call into Vulkan) */
    d = base(); d.pso_attr_fmt[2] = 999;
    d.vertex_shader = (VkShaderModule)(uintptr_t)1; d.fragment_shader = (VkShaderModule)(uintptr_t)1; d.layout = (VkPipelineLayout)(uintptr_t)1;
    VkPipeline p = (VkPipeline)(uintptr_t)7;
    CHECK(lagfx_pipeline_build((VkDevice)(uintptr_t)1, &d, &p) == LAGFX_ERR_PROTOCOL, "pipeline build fails with LAGFX_ERR_PROTOCOL");

    printf("%s vertex-input-unknown-fmt-unit\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
#endif
