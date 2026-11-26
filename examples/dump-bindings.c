/* For every render PSO of an arm64 GPU dump, runs the binding part of the pipeline resolver (translate vertex+fragment with the current translator, offset the
 * fragment bindings, reflect, walk the bindings) and prints per PSO what the resolver would do with it: the bindings per stage against LAGFX_A64_MAX_BINDINGS and,
 * for each input attachment, lagfx_a64_fbfetch_check()'s verdict. usage: dump-bindings <dump dir> [local_read 0|1, default 1]. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "protocol/arm64_pipeline.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXB LAGFX_A64_MAX_BINDINGS
typedef struct { uint32_t task, ref; char file[96]; } fnent_t;
static fnent_t g_fn[512]; static int g_nfn;

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static const char *kname(int k) { static const char *n[] = { "?", "image", "sampler", "buffer", "input", "storage_img" }; return k >= 0 && k < 6 ? n[k] : "?"; }

typedef struct { uint8_t *lib; size_t len; lagfx_metallib_function_t fn[8]; size_t nfn; } lib_t;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir> [local_read]\n", argv[0]); return 2; }
    int local_read = argc > 2 ? atoi(argv[2]) : 1;
    char path[1200]; snprintf(path, sizeof path, "%s/index.txt", argv[1]);
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 2; }
    static char line[8192]; int npso = 0, nstub = 0, nfetch = 0;
    static char psos[512][8192]; int np = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned ref, ftask = 0; char file[96];
        if (!strncmp(line, "fn ", 3) && sscanf(strstr(line, " ref=") + 5, "%u", &ref) == 1 && strstr(line, " file=") && g_nfn < 512) {
            sscanf(strstr(line, " file=") + 6, "%95s", file);
            sscanf(line + 3, "task=%u", &ftask); g_fn[g_nfn].task = ftask; g_fn[g_nfn].ref = ref; snprintf(g_fn[g_nfn].file, sizeof g_fn[g_nfn].file, "%s", file); g_nfn++;
        } else if (!strncmp(line, "pso ", 4) && np < 512) snprintf(psos[np++], sizeof psos[0], "%s", line);
    }
    fclose(f);
    for (int i = 0; i < np; i++) {
        unsigned task = 0, ref, a, b;
        char *pr = strstr(psos[i], " ref="), *fr = strstr(psos[i], "function_refs=[");
        if (!pr || !fr || sscanf(psos[i] + 4, "task=%u", &task) != 1) continue;
        ref = (unsigned)atoi(pr + 5);
        if (sscanf(fr + 15, "%u,%u", &a, &b) != 2) continue;
        unsigned refs[2] = { a, b };
        const lagfx_metallib_function_t *vf = NULL, *ff = NULL; lib_t libs[2]; int nl = 0;
        memset(libs, 0, sizeof libs);
        for (int q = 0; q < 2; q++) {
            const char *file = NULL;
            for (int k = 0; k < g_nfn; k++) if (g_fn[k].ref == refs[q] && g_fn[k].task == task) { file = g_fn[k].file; break; }
            if (!file) continue;
            snprintf(path, sizeof path, "%s/%s", argv[1], file);
            lib_t *l = &libs[nl]; l->lib = slurp(path, &l->len);
            if (!l->lib || lagfx_metallib_extract_functions(l->lib, l->len, l->fn, 8, &l->nfn) != LAGFX_OK) continue;
            nl++;
            for (size_t k = 0; k < l->nfn && k < 8; k++) {
                if (!l->fn[k].bitcode) continue;
                if (!vf && l->fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &l->fn[k];
                if (!ff && l->fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) ff = &l->fn[k];
            }
        }
        printf("pso ref=%u: ", ref); npso++;
        if (!vf || !ff) { printf("no vertex/fragment pair\n"); continue; }
        lagfx_air_module_t *vm = NULL, *fm = NULL; uint8_t *vspv = NULL, *fspv = NULL; size_t vsz = 0, fsz = 0;
        lagfx_stage_keys_t vk; memset(&vk, 0, sizeof vk);
        if (lagfx_air_module_open(vf->bitcode, vf->bitcode_len, &vm) != LAGFX_OK || lagfx_air_module_open(ff->bitcode, ff->bitcode_len, &fm) != LAGFX_OK) { printf("module_open failed\n"); continue; }
        if (!lagfx_air_stage_keys(vm, true, &vk)) memset(&vk, 0, sizeof vk);
        if (lagfx_air2spv_translate_module_ex(vm, NULL, &vspv, &vsz) != LAGFX_OK || lagfx_air2spv_translate_module_ex(fm, &vk, &fspv, &fsz) != LAGFX_OK) { printf("translate failed (%s / %s)\n", vf->name, ff->name); continue; }
        lagfx_spv_offset_bindings(fspv, fsz, LAGFX_A64_FRAG_BINDING_BASE);
        uint8_t *blobs[2] = { vspv, fspv }; size_t lens[2] = { vsz, fsz };
        printf("%s + %s\n", vf->name, ff->name);
        uint32_t n_bind = 0; int verdict = 0;
        for (int st = 0; st < 2; st++) {
            lagfx_spv_binding_t tmp[LAGFX_A64_MAX_REFLECT]; size_t nt = lagfx_spv_reflect_bindings(blobs[st], lens[st], tmp, LAGFX_A64_MAX_REFLECT);
            if (nt > LAGFX_A64_MAX_REFLECT) { printf("  %s: reflection hit the %u-binding cap: bindings stub\n", st ? "fragment" : "vertex", LAGFX_A64_MAX_REFLECT); if (!verdict) verdict = 98; }
            if (nt > LAGFX_A64_MAX_REFLECT) nt = LAGFX_A64_MAX_REFLECT;   /* only that many were written */
            printf("  %s: %zu bindings (limit %u total):", st ? "fragment" : "vertex", nt, MAXB);
            for (size_t t = 0; t < nt; t++) printf(" %u/%s%u", tmp[t].set, kname((int)tmp[t].kind), tmp[t].binding);
            printf("\n");
            for (size_t t = 0; t < nt; t++) {
                if (tmp[t].set == 0u && tmp[t].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT) {
                    int why = lagfx_a64_fbfetch_check(st, local_read, tmp[t].binding, n_bind);
                    printf("  input attachment at binding %u: %s\n", tmp[t].binding, why ? lagfx_a64_fbfetch_reason_name(why) : "ok");
                    if (why && !verdict) verdict = why;
                    nfetch++;
                }
                n_bind++;
            }
        }
        if (n_bind > MAXB) { printf("  total %u bindings > %u: bindings stub\n", n_bind, MAXB); if (!verdict) verdict = 99; }
        printf("  => %s\n", verdict ? "STUBBED" : "built");
        if (verdict) nstub++;
        free(vspv); free(fspv); lagfx_air_module_free(vm); lagfx_air_module_free(fm);
        for (int q = 0; q < nl; q++) free(libs[q].lib);
    }
    printf("%d PSOs, %d input-attachment bindings, %d would be stubbed\n", npso, nfetch, nstub);
    return 0;
}
