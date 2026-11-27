/*
 * libapplegfx-vulkan — offline vertex + fragment AIR -> SPIR-V pair translation, as the resolver does it
 * examples/air-pair-translate.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Translates <vs.air.bc> (stage keys out) and <fs.air.bc> (translated with the vertex keys), moves the fragment bindings to their own range exactly like
 * lagfx_a64_pipeline_resolve(), writes <prefix>.vs.spv / <prefix>.fs.spv and prints the descriptor binding table of each stage and of the merged set-0 layout
 * (kind, stages, input-attachment index) plus every set+binding that carries two incompatible descriptor types. Used to produce before/after tables of a translator
 * change on a captured pair (the bundle directories of the pipeline-failure dumps hold vs.bc / fs.bc).
 *
 *   usage: air-pair-translate <vs.air.bc> <fs.air.bc> <out-prefix>
 *   exit:  0 = translated, no collision; 1 = translate / I/O failure; 3 = translated but the table has a collision (the SPIR-V is still written)
 */
#include "air/bitcode_reader.h"
#include "air2spv/binding_alloc.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"
#include "protocol/arm64_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static const char *kind_name(lagfx_spv_binding_kind_t k) {
    switch (k) {
    case LAGFX_SPV_BINDING_SAMPLED_IMAGE: return "SAMPLED_IMAGE";
    case LAGFX_SPV_BINDING_SAMPLER: return "SAMPLER";
    case LAGFX_SPV_BINDING_STORAGE_BUFFER: return "STORAGE_BUFFER";
    case LAGFX_SPV_BINDING_INPUT_ATTACHMENT: return "INPUT_ATTACHMENT";
    case LAGFX_SPV_BINDING_STORAGE_IMAGE: return "STORAGE_IMAGE";
    default: return "?";
    }
}

static int write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n) { if (f) fclose(f); return 1; }
    return fclose(f) != 0;
}

#define MAXB 256u
static int table(const char *label, const uint8_t *spv, size_t n, lagfx_spv_binding_t *out, size_t *nout) {
    size_t t = lagfx_spv_reflect_bindings(spv, n, out, MAXB);
    if (t > MAXB) { printf("%s: %zu bindings (more than the %u this tool prints)\n", label, t, MAXB); t = MAXB; }
    *nout = t;
    printf("== %s: %zu variable(s) with set+binding\n", label, t);
    for (size_t i = 0; i < t; i++) printf("  set %u binding %-3u %-16s%s\n", out[i].set, out[i].binding, kind_name(out[i].kind), "");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s <vs.air.bc> <fs.air.bc> <out-prefix>\n", argv[0]); return 2; }
    size_t vn = 0, fn = 0;
    uint8_t *vbc = slurp(argv[1], &vn), *fbc = slurp(argv[2], &fn);
    if (!vbc || !fbc) { fprintf(stderr, "error: cannot read the inputs\n"); return 1; }
    lagfx_air_module_t *vm = NULL, *fm = NULL;
    lagfx_stage_keys_t k;
    memset(&k, 0, sizeof k);
    if (lagfx_air_module_open(vbc, vn, &vm) != LAGFX_OK || lagfx_air_module_open(fbc, fn, &fm) != LAGFX_OK) { fprintf(stderr, "error: module open\n"); return 1; }
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    uint8_t *vs = NULL, *fs = NULL;
    size_t vsn = 0, fsn = 0;
    if (lagfx_air2spv_translate_module_ex(vm, NULL, &vs, &vsn) != LAGFX_OK || lagfx_air2spv_translate_module_ex(fm, &k, &fs, &fsn) != LAGFX_OK) { fprintf(stderr, "error: translate\n"); return 1; }
    lagfx_spv_offset_bindings(fs, fsn, lagfx_a64_frag_base(vs, vsn));
    char path[1024];
    snprintf(path, sizeof path, "%s.vs.spv", argv[3]);
    int rc = write_file(path, vs, vsn);
    snprintf(path, sizeof path, "%s.fs.spv", argv[3]);
    rc |= write_file(path, fs, fsn);
    if (rc) { fprintf(stderr, "error: cannot write the outputs\n"); return 1; }
    printf("vs %zu B, fs %zu B\n", vsn, fsn);
    lagfx_spv_binding_t vt[MAXB], ft[MAXB];
    size_t nv = 0, nf = 0;
    table("vertex", vs, vsn, vt, &nv);
    table("fragment", fs, fsn, ft, &nf);
    /* merged table through the SAME code the layout builder uses: stages, input-attachment indices and every set+binding holding two incompatible descriptors */
    int ncol = 0;
    {
        lagfx_ba_t *ba = malloc(sizeof *ba);
        const uint8_t *bl[2] = { vs, fs };
        const size_t ln[2] = { vsn, fsn };
        char txt[8192];
        if (!ba) return 1;
        lagfx_ba_init(ba);
        lagfx_ba_merge_modules(ba, bl, ln, 2);
        lagfx_ba_describe(ba, txt, sizeof txt);
        printf("== merged set-0 table (layout key %016llx)\n%s", (unsigned long long)lagfx_ba_key(ba), txt);
        ncol = (int)(ba->nconflict + ba->noverflow);
        free(ba);
    }
    printf("collisions: %d\n", ncol);
    free(vs); free(fs);
    lagfx_air_module_free(vm); lagfx_air_module_free(fm);
    free(vbc); free(fbc);
    return ncol ? 3 : 0;
}
