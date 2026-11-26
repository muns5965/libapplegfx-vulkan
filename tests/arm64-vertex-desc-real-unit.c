/* Serialized MTLVertexDescriptor of real render-pipeline descriptors (tag 0xe) -> attributes / stride -> Vulkan vertex input.
 * Fixtures tests/fixtures/vtxdesc_pso_<ref>.bin are DEVICE CAPTURES (whole pipeline-descriptor objects, 180..368 bytes each, from a guest dump of
 * the Apple-originated CoreAnimation/WindowServer render pipelines); they are only parsed, never executed.
 * Regression for: the byte scan dropped the attribute-0 record (its default offset 0 is not serialized), attributes on buffer 0 omit tag 3, and the
 * 2-attribute composite pipelines (Float4@0 + Float2@16) have stride 32, not the 24 of the old tight-pack fallback.
 * SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include "protocol/arm64_pipeline.h"
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

static bool load(int ref, lagfx_a64_pdesc_t *d) {
    char path[512]; static uint8_t b[4096];
    snprintf(path, sizeof path, "%s/vtxdesc_pso_%d.bin", LAGFX_TEST_FIXTURE_DIR, ref);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: missing fixture %s\n", path); bad++; return false; }
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    if (!lagfx_a64_pdesc_decode(b, (uint32_t)n, NULL, NULL, d)) { printf("FAIL: decode ref %d\n", ref); bad++; return false; }
    return true;
}

/* expectation: n attributes {fmt, off} on buffer `buf`, stride */
static void expect(int ref, uint32_t buf, uint32_t stride, uint32_t n, const uint32_t (*fo)[2]) {
    lagfx_a64_pdesc_t d; char m[96];
    if (!load(ref, &d)) return;
    snprintf(m, sizeof m, "ref %d: attribute count %u == %u", ref, d.nattrs, n); CHECK(d.nattrs == n, m);
    snprintf(m, sizeof m, "ref %d: stride %u == %u", ref, d.stride, stride); CHECK(d.stride == stride, m);
    for (uint32_t a = 0; a < n && a < d.nattrs; a++) {
        snprintf(m, sizeof m, "ref %d attr %u: fmt/off %u@%u == %u@%u", ref, a, d.attrs[a].fmt, d.attrs[a].off, fo[a][0], fo[a][1]);
        CHECK(d.attrs[a].fmt == fo[a][0] && d.attrs[a].off == fo[a][1], m);
        snprintf(m, sizeof m, "ref %d attr %u: buffer %u == %u", ref, a, d.attrs[a].bufidx, buf); CHECK(d.attrs[a].bufidx == buf, m);
    }
}

/* decoded attributes -> desc as arm64_pipeline.c fills it (reflected component counts as in the device log: vtx_in=0:4,1:2,2:2,3:4) */
static lagfx_pipeline_desc_t to_desc(const lagfx_a64_pdesc_t *d, uint32_t nvi, const uint8_t *comp) {
    lagfx_pipeline_desc_t p; memset(&p, 0, sizeof p);
    p.n_vtx_inputs = (uint8_t)nvi;
    for (uint32_t i = 0; i < nvi; i++) p.vtx_in_loc[i] = (uint8_t)i, p.vtx_in_comp[i] = comp[i];
    p.n_pso_attrs = (uint8_t)d->nattrs;
    for (uint32_t a = 0; a < d->nattrs; a++) p.pso_attr_fmt[a] = d->attrs[a].fmt, p.pso_attr_off[a] = d->attrs[a].off;
    p.vtx_stride = d->stride;
    return p;
}

int main(void) {
    static const uint32_t quad4[4][2] = { {31, 0}, {29, 16}, {29, 24}, {27, 32} };
    expect(12, 1, 48, 4, quad4);                       /* CoreAnimation indexed quad, attribute 0 present although its offset 0 is not serialized */
    expect(23, 1, 48, 4, quad4);
    static const uint32_t p71[3][2] = { {29, 0}, {29, 8}, {28, 16} };
    expect(71, 0, 24, 3, p71);                          /* buffer 0: tag 3 omitted */
    static const uint32_t p32[2][2] = { {31, 0}, {29, 16} };
    expect(78, 0, 32, 2, p32);                          /* old fallback gave 24 */
    expect(45, 0, 32, 2, p32);
    expect(34, 0, 32, 2, p32);
    static const uint32_t p16[2][2] = { {29, 0}, {29, 8} };
    expect(61, 0, 16, 2, p16);
    expect(77, 0, 16, 2, p16);

    /* through lagfx_pipeline_vertex_input: PSO 12 (reflected comps 4,2,2,4: a float tight-pack would give offsets 0/16/24/32 as float4 and no Half4) */
    lagfx_a64_pdesc_t d; VkVertexInputBindingDescription vb; VkVertexInputAttributeDescription va[8]; uint32_t n = 0, badi = 99;
    if (load(12, &d)) {
        lagfx_pipeline_desc_t p = to_desc(&d, 4, (const uint8_t[]){4,2,2,4});
        CHECK(lagfx_pipeline_vertex_input(&p, &vb, va, &n, &badi) && n == 4, "pso 12: vertex input ok, 4 attributes");
        CHECK(va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[0].offset == 0, "pso 12 attr 0 Float4@0");
        CHECK(va[1].format == VK_FORMAT_R32G32_SFLOAT && va[1].offset == 16, "pso 12 attr 1 Float2@16");
        CHECK(va[2].format == VK_FORMAT_R32G32_SFLOAT && va[2].offset == 24, "pso 12 attr 2 Float2@24");
        CHECK(va[3].format == VK_FORMAT_R16G16B16A16_SFLOAT && va[3].offset == 32, "pso 12 attr 3 Half4 (27) -> R16G16B16A16_SFLOAT @32 (not float)");
        CHECK(vb.stride == 48, "pso 12 stride 48");
    }
    /* pso 78: 2 reflected inputs, stride 32 (old fallback: round8(16+8)=24) */
    if (load(78, &d)) {
        lagfx_pipeline_desc_t p = to_desc(&d, 2, (const uint8_t[]){4,2});
        CHECK(lagfx_pipeline_vertex_input(&p, &vb, va, &n, &badi) && n == 2, "pso 78: vertex input ok");
        CHECK(va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[0].offset == 0 && va[1].format == VK_FORMAT_R32G32_SFLOAT && va[1].offset == 16, "pso 78 Float4@0 + Float2@16 from the PSO, not the reflected comps");
        CHECK(vb.stride == 32, "pso 78 stride 32");
    }

    /* Synthetic: fewer decoded PSO attributes than reflected inputs. CURRENT behaviour (documented, not endorsed): use_pso_attrs is all-or-nothing, so the
     * valid decoded attributes are IGNORED and ALL inputs get the reflected float tight-pack (offsets 0/16, formats from the reflected component counts). */
    if (load(78, &d)) {
        lagfx_pipeline_desc_t p = to_desc(&d, 3, (const uint8_t[]){4,4,4});   /* 2 decoded attributes, 3 reflected inputs */
        CHECK(lagfx_pipeline_vertex_input(&p, &vb, va, &n, &badi) && n == 3, "undercount: succeeds");
        CHECK(va[1].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[1].offset == 16, "undercount (current behaviour): attribute 1 is float-packed from the reflection, its decoded Float2@16 is ignored");
        CHECK(va[2].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[2].offset == 32, "undercount (current behaviour): attribute 2 float-packed @32");
    }

    printf("%s arm64-vertex-desc-real-unit\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
#endif
