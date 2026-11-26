/*
 * libapplegfx-vulkan — the hash= of the "pipeline-build-fail:" device log line
 * tests/pipeline-spv-hash-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * lagfx_pipeline_spv_hash() is FNV-1a 64 with the historical offset basis (fixed vectors), and translating a vertex/fragment fixture pair the way lagfx_a64_pipeline_resolve() does (vertex first,
 * fragment with the vertex stage keys, then lagfx_spv_offset_bindings) gives the same bytes - hence the same hash - on every run, so a device log hash can be
 * matched against an offline translation (examples/dump-pairhash).
 */
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "protocol/arm64_pipeline.h"
#include "vulkan/pipeline_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

/* resolver order: vertex (keys out), fragment (keys in), offset the fragment bindings */
static int pair(uint8_t **vs, size_t *vn, uint8_t **fs, size_t *fn) {
    size_t n1, n2; uint8_t *vb = slurp("login_ubercomposite_vert.air.bc", &n1), *fb = slurp("login_ubercomposite_frag_a.air.bc", &n2);
    CHECK(vb && fb, "fixtures missing");
    lagfx_air_module_t *vm = NULL, *fm = NULL; lagfx_stage_keys_t k; memset(&k, 0, sizeof k);
    CHECK(lagfx_air_module_open(vb, n1, &vm) == LAGFX_OK && lagfx_air_module_open(fb, n2, &fm) == LAGFX_OK, "module_open");
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    CHECK(lagfx_air2spv_translate_module_ex(vm, NULL, vs, vn) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &k, fs, fn) == LAGFX_OK, "translate");
    lagfx_spv_offset_bindings(*fs, *fn, LAGFX_A64_FRAG_BINDING_BASE);
    lagfx_air_module_free(vm); lagfx_air_module_free(fm); free(vb); free(fb);
    return 0;
}

int main(void) {
    /* NOTE: this library's offset basis is 1469598103934665603 (the standard FNV one is 14695981039346656037: a digit was dropped when this was written). It is NOT
     * changed: the device logs and the spv_<hash>_*.spv dump names already carry these values. So these are not the textbook FNV-1a vectors. */
    CHECK(lagfx_pipeline_spv_hash("", 0) == 1469598103934665603ull, "empty string = the offset basis");
    CHECK(lagfx_pipeline_spv_hash("a", 1) == 0x44bd8ad473cd9906ull, "hash of \"a\"");
    CHECK(lagfx_pipeline_spv_hash("foobar", 6) == 0x88fad7c0a8ff07f2ull, "hash of \"foobar\"");
    uint8_t *v1 = NULL, *f1 = NULL, *v2 = NULL, *f2 = NULL; size_t vn1 = 0, fn1 = 0, vn2 = 0, fn2 = 0;
    if (pair(&v1, &vn1, &f1, &fn1) || pair(&v2, &vn2, &f2, &fn2)) return 1;
    CHECK(vn1 && fn1 && vn1 == vn2 && fn1 == fn2, "sizes differ between two translations");
    CHECK(lagfx_pipeline_spv_hash(v1, vn1) == lagfx_pipeline_spv_hash(v2, vn2) && lagfx_pipeline_spv_hash(f1, fn1) == lagfx_pipeline_spv_hash(f2, fn2), "translation is not deterministic");
    CHECK(!memcmp(f1, f2, fn1), "fragment bytes differ");
    printf("pipeline spv hash: ok (vs %zu bytes %016llx, fs %zu bytes %016llx)\n", vn1, (unsigned long long)lagfx_pipeline_spv_hash(v1, vn1), fn1, (unsigned long long)lagfx_pipeline_spv_hash(f1, fn1));
    free(v1); free(f1); free(v2); free(f2);
    return 0;
}
