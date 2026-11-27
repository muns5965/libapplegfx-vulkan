/* Vertex fetch wrappers (air.vertexFetchFunction): the translator records the buffer offset each stage-in input is loaded from (OpName "lagfx.vin.*" on the Input
 * variable, read back by lagfx_spv_reflect_vertex_inputs) and lagfx_pipeline_vertex_input pairs such an input with the decoded PSO attribute at the same buffer slot and
 * offset instead of the attribute at the same position. Regression for PSOs whose descriptor declares more attributes than the shader reads
 * ({Float4@0, Float2@16, Float2@24, Half4@32} vs a shader that reads offsets 0, 16, 32).
 * Fixtures tests/fixtures/vfetch_*.air.bc are DEVICE CAPTURES: the one metallib function module (real vertex function + the driver-generated wrapper) of three
 * shader libraries of a guest dump (lib_c0984019049069f0, lib_f75b16d9ae7adcc2, lib_87dacecf85d474ae); they are only translated, never executed.
 * vtxdesc_pso_12.bin is the device-captured PSO descriptor ({fmt31@0, fmt29@16, fmt29@24, fmt27@32}, stride 48, buffer 1). SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "protocol/arm64_pipeline.h"
#include "vulkan/pipeline_build.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static uint8_t *slurp(const char *name, size_t *n) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", LAGFX_TEST_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: missing fixture %s\n", path); bad++; return NULL; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static size_t reflect(const char *fixture, lagfx_spv_vertex_input_t *vin) {
    size_t bn = 0, sz = 0, nvi = 0; uint8_t *bc = slurp(fixture, &bn), *spv = NULL; lagfx_air_module_t *m = NULL;
    if (!bc) return 0;
    if (lagfx_air_module_open(bc, bn, &m) != LAGFX_OK || !m || lagfx_air2spv_translate_module_ex(m, NULL, &spv, &sz) != LAGFX_OK || !spv) { printf("FAIL: translate %s\n", fixture); bad++; }
    else nvi = lagfx_spv_reflect_vertex_inputs(spv, sz, vin, 8);
    lagfx_air_module_free(m); free(spv); free(bc);
    return nvi;
}
static bool load_pso(int ref, lagfx_a64_pdesc_t *d) {
    char name[64]; size_t n = 0; snprintf(name, sizeof name, "vtxdesc_pso_%d.bin", ref);
    uint8_t *b = slurp(name, &n); if (!b) return false;
    bool ok = lagfx_a64_pdesc_decode(b, (uint32_t)n, NULL, NULL, d); free(b);
    if (!ok) { printf("FAIL: decode %s\n", name); bad++; }
    return ok;
}
/* exactly what arm64_pipeline.c does with the reflection and the decoded descriptor */
static lagfx_pipeline_desc_t to_desc(const lagfx_spv_vertex_input_t *vin, size_t nvi, const lagfx_a64_pdesc_t *d) {
    lagfx_pipeline_desc_t p; memset(&p, 0, sizeof p);
    for (size_t v = 0; v < nvi; v++) {
        p.vtx_in_loc[v] = (uint8_t)vin[v].location, p.vtx_in_comp[v] = (uint8_t)vin[v].components;
        p.vtx_in_src_known[v] = vin[v].has_src_offset, p.vtx_in_src_unfed[v] = vin[v].src_unfed, p.vtx_in_src_off[v] = vin[v].src_offset;
        p.vtx_in_buf_known[v] = vin[v].has_src_buffer, p.vtx_in_buf[v] = vin[v].src_buffer;
    }
    p.n_vtx_inputs = (uint8_t)nvi; p.pso_attrs_serialized = 1; p.n_pso_attrs = (uint8_t)d->nattrs; p.pso_attr_buf_known = 1; p.vtx_stride = d->stride;
    for (uint32_t a = 0; a < d->nattrs; a++) p.pso_attr_fmt[a] = d->attrs[a].fmt, p.pso_attr_off[a] = d->attrs[a].off, p.pso_attr_buf[a] = d->attrs[a].bufidx;
    return p;
}

int main(void) {
    lagfx_spv_vertex_input_t vin[8]; lagfx_a64_pdesc_t d;
    VkVertexInputBindingDescription vb; VkVertexInputAttributeDescription va[8]; uint32_t n = 0, bi = 99; int why = 0;
    if (!load_pso(12, &d)) return 1;
    CHECK(d.nattrs == 4 && d.stride == 48, "vtxdesc_pso_12 decodes to 4 attributes, stride 48");

    /* --- 3 loads (offsets 0, 16, 32; float4, float2, half4): the colour input pairs with Half4@32, Float2@24 is unused --- */
    memset(vin, 0, sizeof vin);
    size_t nvi = reflect("vfetch_3load_std_vert1_lph.air.bc", vin);
    CHECK(nvi == 3, "3-load fetch VS reflects 3 stage-in inputs");
    if (nvi == 3) {
        CHECK(vin[0].has_src_offset && vin[0].src_offset == 0 && vin[1].has_src_offset && vin[1].src_offset == 16 && vin[2].has_src_offset && vin[2].src_offset == 32, "source offsets 0, 16, 32 (offset 0 is known)");
        CHECK(vin[0].has_src_bytes && vin[0].src_bytes == 16 && vin[1].src_bytes == 8 && vin[2].src_bytes == 8, "load sizes 16, 8, 8 bytes (float4, float2, half4)");
        CHECK(vin[0].has_src_buffer && vin[0].src_buffer == 1 && vin[2].src_buffer == 1, "source buffer slot 1 (the wrapper's air.location_index)");
        CHECK(vin[0].components == 4 && vin[1].components == 2 && vin[2].components == 4, "reflected components 4, 2, 4");
        lagfx_pipeline_desc_t p = to_desc(vin, nvi, &d);
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && n == 3, "3-load: vertex input ok");
        CHECK(va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[0].offset == 0, "loc0 Float4@0");
        CHECK(va[1].format == VK_FORMAT_R32G32_SFLOAT && va[1].offset == 16, "loc1 Float2@16");
        CHECK(va[2].location == 2 && va[2].format == VK_FORMAT_R16G16B16A16_SFLOAT && va[2].offset == 32, "loc2 (colour) pairs with R16G16B16A16_SFLOAT@32, not the Float2@24");
        CHECK(vb.stride == 48, "stride stays the PSO's 48");
        /* the old (positional) behaviour, for the record: every offset cleared */
        lagfx_pipeline_desc_t q = p; memset(q.vtx_in_src_known, 0, sizeof q.vtx_in_src_known);
        CHECK(lagfx_pipeline_vertex_input(&q, &vb, va, &n, &bi) && va[2].format == VK_FORMAT_R32G32_SFLOAT && va[2].offset == 24, "no known offsets: positional pairing unchanged (loc2 = Float2@24)");

        /* a different buffer slot on the decoded side: no attribute at that slot+offset */
        p = to_desc(vin, nvi, &d); for (uint32_t a = 0; a < d.nattrs; a++) p.pso_attr_buf[a] = 2;
        CHECK(!lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && why == LAGFX_VIN_NO_ATTR_AT_OFFSET && bi == 0, "buffer slot mismatch: fails (no attribute at slot+offset)");
        p = to_desc(vin, nvi, &d); p.pso_attr_buf_known = 0;
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why), "decoded buffer slot unknown: pairs by offset alone");
        /* no decoded attribute at offset 32 */
        p = to_desc(vin, nvi, &d); p.pso_attr_off[3] = 40;
        CHECK(!lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && why == LAGFX_VIN_NO_ATTR_AT_OFFSET && bi == 2 && va[2].format == VK_FORMAT_UNDEFINED, "no attribute at the offset: input 2 fails, not guessed");
        p.vertex_shader = (VkShaderModule)(uintptr_t)1; p.fragment_shader = (VkShaderModule)(uintptr_t)1; p.layout = (VkPipelineLayout)(uintptr_t)1;
        { VkPipeline pl = (VkPipeline)(uintptr_t)7; CHECK(lagfx_pipeline_build((VkDevice)(uintptr_t)1, &p, &pl) == LAGFX_ERR_PROTOCOL, "pipeline build fails with LAGFX_ERR_PROTOCOL"); }
        /* two decoded attributes at the same slot+offset, both fit: ambiguous */
        p = to_desc(vin, nvi, &d); p.pso_attr_off[2] = 32; p.pso_attr_fmt[2] = 27;
        CHECK(!lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && why == LAGFX_VIN_AMBIGUOUS_OFFSET && bi == 2, "duplicate decoded offset (both compatible): fails, never picks the first");
        /* duplicates disambiguated by component count: a Float (1 component) at the same offset cannot feed the 4 component input */
        p = to_desc(vin, nvi, &d); p.pso_attr_off[2] = 32; p.pso_attr_fmt[2] = 28;
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && va[2].format == VK_FORMAT_R16G16B16A16_SFLOAT && va[2].offset == 32, "duplicate decoded offset: the incompatible one is excluded, the compatible one is used");
        /* too few components at the offset */
        p = to_desc(vin, nvi, &d); p.pso_attr_fmt[3] = 29;
        CHECK(!lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && why == LAGFX_VIN_INCOMPATIBLE && bi == 2, "Float2 attribute for a 4 component input: incompatible");
        /* more components in the attribute than the shader loads is fine (half4 attribute -> float4 input is the Vulkan format conversion; here Float4 -> float2) */
        p = to_desc(vin, nvi, &d); p.pso_attr_fmt[1] = 31;
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && va[1].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[1].offset == 16, "attribute wider than the loaded value: accepted");
        /* an input with no known source while the others have one: no positional guess */
        p = to_desc(vin, nvi, &d); p.vtx_in_src_known[1] = 0;
        CHECK(!lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && why == LAGFX_VIN_UNKNOWN_SRC && bi == 1, "mixed known/unknown sources: the unknown input fails");
        /* not a serialized descriptor, fewer attributes than inputs: the old float tight pack, offsets not consulted */
        p = to_desc(vin, nvi, &d); p.pso_attrs_serialized = 0; p.n_pso_attrs = 2;
        CHECK(lagfx_pipeline_vertex_input(&p, &vb, va, &n, &bi) && va[2].format == VK_FORMAT_R32G32B32A32_SFLOAT && va[2].offset == 24, "x86-style descriptor: tight float pack unchanged");
    }

    /* --- 4 loads (offsets 0, 16, 24, 32): all four decoded attributes are kept, exactly as the positional pairing gave --- */
    memset(vin, 0, sizeof vin);
    nvi = reflect("vfetch_4load_VfxU11Xh.air.bc", vin);
    CHECK(nvi == 4, "4-load fetch VS reflects 4 stage-in inputs");
    if (nvi == 4) {
        CHECK(vin[0].src_offset == 0 && vin[1].src_offset == 16 && vin[2].src_offset == 24 && vin[3].src_offset == 32 && vin[2].has_src_offset, "source offsets 0, 16, 24, 32");
        lagfx_pipeline_desc_t p = to_desc(vin, nvi, &d), q = p; memset(q.vtx_in_src_known, 0, sizeof q.vtx_in_src_known);
        VkVertexInputAttributeDescription vo[8]; uint32_t no = 0;
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && n == 4 && lagfx_pipeline_vertex_input(&q, &vb, vo, &no, &bi) && no == 4, "4-load: ok with and without offsets");
        CHECK(!memcmp(va, vo, 4 * sizeof va[0]), "4-load: identical to the positional pairing");
        CHECK(va[2].format == VK_FORMAT_R32G32_SFLOAT && va[2].offset == 24 && va[3].format == VK_FORMAT_R16G16B16A16_SFLOAT && va[3].offset == 32, "4-load: Float2@24 and Half4@32 kept");
    }

    /* --- a wrapper that passes undef for an argument: that input has no source; it keeps its positional pairing, the others pair by offset --- */
    memset(vin, 0, sizeof vin);
    nvi = reflect("vfetch_undef_downsample_blur_vert_lph.air.bc", vin);
    CHECK(nvi == 3, "undef fetch VS reflects 3 stage-in inputs");
    if (nvi == 3) {
        CHECK(vin[0].has_src_offset && vin[0].src_offset == 0 && vin[1].has_src_offset && vin[1].src_offset == 16 && !vin[2].has_src_offset && vin[2].src_unfed, "inputs 0, 1 read offsets 0, 16; input 2 is fed undef");
        lagfx_pipeline_desc_t p = to_desc(vin, nvi, &d);
        CHECK(lagfx_pipeline_vertex_input_ex(&p, &vb, va, &n, &bi, &why) && n == 3, "undef input: vertex input ok");
        CHECK(va[1].format == VK_FORMAT_R32G32_SFLOAT && va[1].offset == 16 && va[2].format == VK_FORMAT_R32G32_SFLOAT && va[2].offset == 24, "undef input keeps its positional attribute (Float2@24)");
    }

    printf("%s vertex-fetch-offset-pairing-unit\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
#endif
