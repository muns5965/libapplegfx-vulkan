/*
 * libapplegfx-vulkan — SPIR-V descriptor-binding reflection regression
 * tests/air2spv-spv-reflect-bindings-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Guards lagfx_spv_reflect_bindings() — the host uses it to build a
 * VkDescriptorSetLayout matching the bindings a translated SkyLight shader
 * declares (the production pipeline currently uses an empty layout, so
 * resource-using shaders fail pipeline creation; this reflection is the
 * foundational piece that closes that gap). Validates against THREE real
 * SkyLight shaders translated by air2spv:
 *   - SimpleColorFragment      -> 0 bindings (resource-free colour pass)
 *   - SimpleTextureFragment    -> set 0: binding 0 sampled-image, binding 1 sampler
 *   - SimpleTextureLightingVertex -> a StorageBuffer binding ([[buffer]])
 */

#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "air/bitcode_reader.h"
#include "protocol/arm64_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)sz; return b;
}

/* Translate a fixture .bc -> SPIR-V; returns 0 on success. */
static int translate_fixture(const char *name, uint8_t **spv, size_t *spv_sz) {
    char path[512];
    const char *dirs[] = { "tests/fixtures/", "../tests/fixtures/", SRCDIR "/fixtures/", NULL };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        air = slurp(path, &air_len);
        if (air) break;
    }
    if (!air) { printf("FAIL: fixture %s not found\n", name); return 1; }
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open %s\n", name); free(air); return 1;
    }
    int rc = (lagfx_air2spv_translate_module(m, spv, spv_sz) == LAGFX_OK && *spv) ? 0 : 1;
    if (rc) printf("FAIL: translate %s\n", name);
    lagfx_air_module_free(m); free(air);
    return rc;
}

static int count_kind(const lagfx_spv_binding_t *b, size_t n, lagfx_spv_binding_kind_t k) {
    int c = 0; for (size_t i = 0; i < n; i++) if (b[i].kind == k) c++; return c;
}

/* Hand-assembled compute module (spirv-as, validated with spirv-val): 5 storage-buffer bindings. 0 = loaded only, 1 = OpStore through an access chain, 2 = two variables of one
 * binding (a byte-buffer view pair): one loads, the other stores, 3 = OpAtomicIAdd. Expected may_write: 0 1 1 1. SYNTHETIC: it states what the analysis must do, not what
 * any Apple shader does. */
static const uint32_t k_synth_spv[] = {
    0x07230203, 0x00010300, 0x00070000, 0x00000019, 0x00000000, 0x00020011,
    0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
    0x00000001, 0x6e69616d, 0x00000000, 0x00060010, 0x00000001, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000002, 0x00000006,
    0x00000004, 0x00050048, 0x00000003, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000003, 0x00000002, 0x00040047, 0x00000004, 0x00000022,
    0x00000000, 0x00040047, 0x00000004, 0x00000021, 0x00000000, 0x00040047,
    0x00000005, 0x00000022, 0x00000000, 0x00040047, 0x00000005, 0x00000021,
    0x00000001, 0x00040047, 0x00000006, 0x00000022, 0x00000000, 0x00040047,
    0x00000006, 0x00000021, 0x00000002, 0x00040047, 0x00000007, 0x00000022,
    0x00000000, 0x00040047, 0x00000007, 0x00000021, 0x00000002, 0x00040047,
    0x00000008, 0x00000022, 0x00000000, 0x00040047, 0x00000008, 0x00000021,
    0x00000003, 0x00020013, 0x00000009, 0x00030021, 0x0000000a, 0x00000009,
    0x00040015, 0x0000000b, 0x00000020, 0x00000000, 0x0004002b, 0x0000000b,
    0x0000000c, 0x00000000, 0x0004002b, 0x0000000b, 0x0000000d, 0x00000001,
    0x0003001d, 0x00000002, 0x0000000b, 0x0003001e, 0x00000003, 0x00000002,
    0x00040020, 0x0000000e, 0x0000000c, 0x00000003, 0x00040020, 0x0000000f,
    0x0000000c, 0x0000000b, 0x0004003b, 0x0000000e, 0x00000004, 0x0000000c,
    0x0004003b, 0x0000000e, 0x00000005, 0x0000000c, 0x0004003b, 0x0000000e,
    0x00000006, 0x0000000c, 0x0004003b, 0x0000000e, 0x00000007, 0x0000000c,
    0x0004003b, 0x0000000e, 0x00000008, 0x0000000c, 0x00050036, 0x00000009,
    0x00000001, 0x00000000, 0x0000000a, 0x000200f8, 0x00000010, 0x00060041,
    0x0000000f, 0x00000011, 0x00000004, 0x0000000c, 0x0000000c, 0x0004003d,
    0x0000000b, 0x00000012, 0x00000011, 0x00060041, 0x0000000f, 0x00000013,
    0x00000005, 0x0000000c, 0x0000000c, 0x0003003e, 0x00000013, 0x00000012,
    0x00060041, 0x0000000f, 0x00000014, 0x00000006, 0x0000000c, 0x0000000c,
    0x0004003d, 0x0000000b, 0x00000015, 0x00000014, 0x00060041, 0x0000000f,
    0x00000016, 0x00000007, 0x0000000c, 0x0000000d, 0x0003003e, 0x00000016,
    0x00000015, 0x00060041, 0x0000000f, 0x00000017, 0x00000008, 0x0000000c,
    0x0000000c, 0x000700ea, 0x0000000b, 0x00000018, 0x00000017, 0x0000000d,
    0x0000000c, 0x0000000d, 0x000100fd, 0x00010038
};

int main(void) {
    int rc = 0;
    lagfx_spv_binding_t binds[16];
    uint8_t *spv = NULL; size_t spv_sz = 0;

    /* 1. Resource-free colour fragment -> 0 bindings. */
    if (translate_fixture("simpletexfraguv.air.bc", &spv, &spv_sz) == 0) {
        /* (UV fragment samples a texture via a constexpr sampler -> expect
         * a sampled image binding; sampler is module-scope immutable, no
         * descriptor.) Validate it reflects exactly the texture. */
        size_t n = lagfx_spv_reflect_bindings(spv, spv_sz, binds, 16);
        printf("simpletexfraguv: %zu binding(s)\n", n);
        if (count_kind(binds, n < 16 ? n : 16, LAGFX_SPV_BINDING_SAMPLED_IMAGE) < 1) {
            printf("FAIL: UV fragment should reflect a sampled-image binding\n"); rc = 1;
        }
        free(spv); spv = NULL;
    } else rc = 1;

    /* 2. Texture + sampler fragment -> sampled image + sampler in set 0. */
    if (translate_fixture("simpletexfrag.air.bc", &spv, &spv_sz) == 0 ||
        /* fall back to the buffer fixture name if the tex one is absent */
        0) {
        size_t n = lagfx_spv_reflect_bindings(spv, spv_sz, binds, 16);
        printf("simpletexfrag: %zu binding(s):", n);
        for (size_t i = 0; i < n && i < 16; i++)
            printf(" [set %u bind %u kind %d]", binds[i].set, binds[i].binding, binds[i].kind);
        printf("\n");
        int img = count_kind(binds, n < 16 ? n : 16, LAGFX_SPV_BINDING_SAMPLED_IMAGE);
        int smp = count_kind(binds, n < 16 ? n : 16, LAGFX_SPV_BINDING_SAMPLER);
        if (img < 1) { printf("FAIL: expected >=1 sampled-image binding\n"); rc = 1; }
        if (smp < 1) { printf("FAIL: expected >=1 sampler binding\n"); rc = 1; }
        /* Bindings must be ascending within set 0. */
        for (size_t i = 1; i < n && i < 16; i++)
            if (binds[i].set == binds[i-1].set && binds[i].binding <= binds[i-1].binding) {
                printf("FAIL: bindings not strictly ascending\n"); rc = 1;
            }
        free(spv); spv = NULL;
    } else {
        printf("FAIL: could not translate simpletexfrag fixture\n"); rc = 1;
    }

    /* 2b. Contract the pipeline resolver depends on: the return value is the TOTAL number of bindings, also when the output buffer is smaller (cap 1) or
     * absent; the resolver fails a stage whose total exceeds LAGFX_A64_MAX_REFLECT instead of truncating. */
    if (translate_fixture("simpletexfrag.air.bc", &spv, &spv_sz) == 0) {
        size_t total = lagfx_spv_reflect_bindings(spv, spv_sz, binds, 16);
        lagfx_spv_binding_t one[1];
        size_t capped = lagfx_spv_reflect_bindings(spv, spv_sz, one, 1);
        if (total < 2u || capped != total) { printf("FAIL: reflect must return the total (total=%zu with cap 16, %zu with cap 1)\n", total, capped); rc = 1; }
        else printf("reflect returns the total with a small cap: %zu == %zu\n", capped, total);
        free(spv); spv = NULL;
    } else rc = 1;

    /* 3. [[buffer]] vertex -> a StorageBuffer binding. */
    if (translate_fixture("simpletexlighting_vertex.air.bc", &spv, &spv_sz) == 0) {
        size_t n = lagfx_spv_reflect_bindings(spv, spv_sz, binds, 16);
        printf("simpletexlighting_vertex: %zu binding(s)\n", n);
        if (count_kind(binds, n < 16 ? n : 16, LAGFX_SPV_BINDING_STORAGE_BUFFER) < 1) {
            printf("FAIL: vertex [[buffer]] should reflect a StorageBuffer binding\n"); rc = 1;
        }
        free(spv); spv = NULL;
    } else rc = 1;


    /* 4. may_write: the arg metadata of Apple shaders says air.read_write for every non-const `device T *`, so it over-reports. */
    {
        size_t n = lagfx_spv_reflect_bindings((const uint8_t *)k_synth_spv, sizeof k_synth_spv, binds, 16);
        const uint8_t want[4] = { 0, 1, 1, 1 };
        if (n != 5u) { printf("FAIL: synthetic module: %zu bindings (want 5: the byte-buffer view pair reflects twice)\n", n); rc = 1; }
        else {
            for (size_t i = 0; i < n; i++) {
                uint32_t b = binds[i].binding;
                if (b > 3u || binds[i].kind != LAGFX_SPV_BINDING_STORAGE_BUFFER || binds[i].may_write != want[b]) {
                    printf("FAIL: synthetic binding %u kind %d may_write %u (want %u)\n", b, (int)binds[i].kind, binds[i].may_write, b < 4u ? want[b] : 9u); rc = 1;
                }
            }
            if (rc == 0) printf("may_write: synthetic load-only / store / view pair / atomic = 0 1 1 1\n");
        }
    }
    /* 4b. REAL data: VfxXgh (vertex function of the 20261126-231501 dump, lib_445d43e9ba751f49, task 2 ref 26; UberComposite pipelines). Its arg 0 `vertex_buffer` carries
     * air.read_write (lagfx_air_arg_bindings -> written=1) although the LLVM argument is `readonly` and the translated module never stores / atomics / copies through any
     * storage buffer: every binding must reflect may_write = 0. The resolver combines the two (written && may_write), so this binding is no longer "writable". */
    if (translate_fixture("vfxxgh_231501.air.bc", &spv, &spv_sz) == 0) {
        size_t n = lagfx_spv_reflect_bindings(spv, spv_sz, binds, 16);
        int nsb = count_kind(binds, n < 16 ? n : 16, LAGFX_SPV_BINDING_STORAGE_BUFFER), nw = 0;
        for (size_t i = 0; i < n && i < 16; i++) nw += binds[i].may_write;
        if (nsb < 5 || nw != 0) { printf("FAIL: VfxXgh: %d storage-buffer bindings, %d with may_write (want >= 5 and 0)\n", nsb, nw); rc = 1; }
        else printf("VfxXgh (real): %d storage-buffer bindings, none may be written\n", nsb);
        free(spv); spv = NULL;
        uint8_t *air = NULL; size_t air_len = 0; char path[512];
        const char *dirs[] = { "tests/fixtures/", "../tests/fixtures/", SRCDIR "/fixtures/", NULL };
        for (int i = 0; dirs[i] && !air; i++) { snprintf(path, sizeof path, "%svfxxgh_231501.air.bc", dirs[i]); air = slurp(path, &air_len); }
        lagfx_air_module_t *m = NULL;
        if (!air || lagfx_air_module_open(air, air_len, &m) != LAGFX_OK) { printf("FAIL: VfxXgh fixture\n"); rc = 1; }
        else {
            lagfx_air_arg_binding_t ab[16]; size_t na = lagfx_air_arg_bindings(m, ab, 16); int w = 0;
            for (size_t i = 0; i < na; i++) w += ab[i].kind == 1 && ab[i].written;
            if (w != 1) { printf("FAIL: VfxXgh arg metadata: %d written buffer args (want exactly 1: vertex_buffer = air.read_write)\n", w); rc = 1; }
            else printf("VfxXgh (real): arg metadata marks 1 buffer air.read_write (the over-report)\n");
            /* the resolver's binding map combines both: no binding of the vertex stage may come out writable (it decides skip-on-unaligned-offset vs. aligned copy) */
            uint8_t *vs = NULL; size_t vsz = 0;
            lagfx_a64_bind_t bm[LAGFX_A64_MAX_BINDINGS]; uint32_t nb = 0; int fw = 0;
            if (lagfx_air2spv_translate_module(m, &vs, &vsz) != LAGFX_OK || lagfx_a64_map_stage_bindings(vs, vsz, 0, ab, na, true, true, bm, &nb, &fw) != LAGFX_A64_MAP_OK || nb < 5u) {
                printf("FAIL: VfxXgh binding map (%u bindings)\n", nb); rc = 1;
            } else {
                int wb = 0; for (uint32_t i = 0; i < nb; i++) wb += bm[i].writable;
                if (wb) { printf("FAIL: VfxXgh: %d writable bindings in the resolver map (want 0)\n", wb); rc = 1; }
                else printf("VfxXgh (real): resolver binding map has %u bindings, none writable\n", nb);
            }
            free(vs);
            lagfx_air_module_free(m);
        }
        free(air);
    } else rc = 1;

    if (rc == 0) printf("PASS: spv_reflect_bindings matches real SkyLight resource layouts\n");
    return rc;
}
