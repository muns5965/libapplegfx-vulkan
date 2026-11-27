/*
 * libapplegfx-vulkan — descriptor bindings of translated modules never share a set+binding between two descriptor types (unit)
 * tests/air2spv-binding-translate-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The defect (device pair VfxXgh + A2Xghfc): the fragment translator numbered resources, the default sampler and the constexpr samplers sequentially but put the input
 * attachments at the FIXED bindings 24 + n, so a module with more than ~21 resources had a sampler and an input attachment on one (set, binding) -- spirv-val accepts that,
 * a descriptor-set layout cannot. These tests translate hand-written AIR fixtures (tests/fixtures/gen-binding-fixtures.sh, not Apple output) that reach that shape
 * and check, on the reflected SPIR-V:
 *   - no (set, binding) carries two different descriptor kinds, in either stage and in the merged vertex + fragment table;
 *   - every variable has its own binding (no sharing but byte-buffer views of one buffer, which are one storage-buffer descriptor);
 *   - the number of bindings follows the number of resources (growth: 24 -> 60 textures, up to 4 constexpr samplers, 4 framebuffer-fetch inputs).
 */
#include "air/bitcode_reader.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#define FRAG_BASE 16u
#define MAXB 256u

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static uint8_t *fixture(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) { snprintf(path, sizeof path, "%s%s", dirs[d], name); uint8_t *b = slurp(path, n); if (b) return b; }
    return NULL;
}

/* VS (stage keys out) then FS with the keys, fragment bindings offset like the resolver does */
static bool translate_pair(const char *vs_name, const char *fs_name, uint8_t **vs, size_t *vsn, uint8_t **fs, size_t *fsn) {
    size_t vn = 0, fn = 0;
    uint8_t *vbc = fixture(vs_name, &vn), *fbc = fixture(fs_name, &fn);
    lagfx_air_module_t *vm = NULL, *fm = NULL;
    lagfx_stage_keys_t k;
    memset(&k, 0, sizeof k);
    if (!vbc || !fbc || lagfx_air_module_open(vbc, vn, &vm) != LAGFX_OK || lagfx_air_module_open(fbc, fn, &fm) != LAGFX_OK) { fprintf(stderr, "cannot open %s / %s\n", vs_name, fs_name); return false; }
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    bool ok = lagfx_air2spv_translate_module_ex(vm, NULL, vs, vsn) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &k, fs, fsn) == LAGFX_OK;
    if (ok) lagfx_spv_offset_bindings(*fs, *fsn, FRAG_BASE);
    lagfx_air_module_free(vm); lagfx_air_module_free(fm); free(vbc); free(fbc);
    return ok;
}

/* collisions (two kinds on one set+binding) across the given reflected tables; same-kind repeats are counted in *same */
static unsigned collisions(const lagfx_spv_binding_t *t, size_t n, unsigned *same) {
    unsigned c = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (t[i].set == t[j].set && t[i].binding == t[j].binding) {
                if (t[i].kind != t[j].kind) { c++; fprintf(stderr, "  collision: set %u binding %u kinds %d vs %d\n", t[i].set, t[i].binding, (int)t[i].kind, (int)t[j].kind); }
                else if (same) (*same)++;
            }
    return c;
}

static size_t count_kind(const lagfx_spv_binding_t *t, size_t n, lagfx_spv_binding_kind_t k) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += t[i].kind == k;
    return c;
}

static void run_case(const char *fs_name, unsigned ntex, unsigned ncxs, unsigned nfetch) {
    uint8_t *vs = NULL, *fs = NULL;
    size_t vsn = 0, fsn = 0;
    fprintf(stderr, "case %s: %u textures, %u constexpr samplers, %u input attachments\n", fs_name, ntex, ncxs, nfetch);
    if (!translate_pair("login_ubercomposite_vert.air.bc", fs_name, &vs, &vsn, &fs, &fsn)) { CHECK(!"translate_pair"); return; }
    lagfx_spv_binding_t v[MAXB], f[MAXB], all[2 * MAXB];
    size_t nv = lagfx_spv_reflect_bindings(vs, vsn, v, MAXB), nf = lagfx_spv_reflect_bindings(fs, fsn, f, MAXB);
    CHECK(nv <= MAXB && nf <= MAXB);
    CHECK(count_kind(f, nf, LAGFX_SPV_BINDING_SAMPLED_IMAGE) == ntex);
    CHECK(count_kind(f, nf, LAGFX_SPV_BINDING_INPUT_ATTACHMENT) == nfetch);
    CHECK(count_kind(f, nf, LAGFX_SPV_BINDING_SAMPLER) == ncxs + 1u);   /* the default sampler + one binding per constexpr sampler */
    unsigned same_f = 0, same_all = 0;
    CHECK(collisions(f, nf, &same_f) == 0);
    CHECK(same_f == 0);   /* a fragment module has no byte-buffer views: every variable has its own binding */
    memcpy(all, v, nv * sizeof *v);
    memcpy(all + nv, f, nf * sizeof *f);
    CHECK(collisions(all, nv + nf, &same_all) == 0);   /* the merged vertex + fragment table */
    for (size_t i = 0; i < nf; i++) CHECK(f[i].binding >= FRAG_BASE);   /* the fragment range is disjoint from the vertex range */
    free(vs); free(fs);
}

int main(void) {
    run_case("bind_nofetch_fs.air.bc", 24, 2, 0);
    run_case("bind_many_fs.air.bc", 24, 2, 2);   /* the shape of the device failure: > 21 resources + constexpr samplers + 2 input attachments */
    run_case("bind_wide_fs.air.bc", 60, 4, 4);   /* growth: nothing is limited to the old 24..27 attachment window or to 32 bindings */
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    printf("air2spv binding translate: ok\n");
    return 0;
}
