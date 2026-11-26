/* For every render PSO of an arm64 GPU dump: translates the vertex + fragment function EXACTLY as lagfx_a64_pipeline_resolve() does (first vertex / first fragment
 * function of the PSO's libraries in function_refs order, vertex translated first, fragment translated with the vertex stage keys, then
 * lagfx_spv_offset_bindings(fragment, LAGFX_A64_FRAG_BINDING_BASE)) and prints the byte size and the FNV-1a-64 hash of both modules in the SAME form as the
 * "pipeline-build-fail:" device log line (src/vulkan/pipeline_build.c fnv64) - so a device log line can be matched against a dump offline.
 * usage: dump-pairhash <dump dir> [vs-size fs-size | vs-hash fs-hash]   (a filter: only print matching pairs). SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "protocol/arm64_pipeline.h"
#include "vulkan/pipeline_build.h"
#include "libapplegfx-vulkan.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t task, ref; char file[96]; } fnent_t;
static fnent_t g_fn[1024]; static int g_nfn;
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static uint64_t fnv64(const void *data, size_t n) { return lagfx_pipeline_spv_hash(data, n); }   /* the device log's hash= */
typedef struct { uint8_t *lib; size_t len; lagfx_metallib_function_t fn[8]; size_t nfn; } lib_t;

/* --cross <fs lib hash prefix>: pairs EVERY vertex function of the dump with the fragment function of that library (a device pair whose PSO is not in the dump) */
static int cross(const char *dir, const char *fsprefix) {
    char path[1200]; size_t fn_n = 0; lagfx_metallib_function_t ffn[8]; size_t nff = 0;
    snprintf(path, sizeof path, "%s/lib_%s", dir, fsprefix);
    DIR *d = opendir(dir); struct dirent *e; uint8_t *fl = NULL; size_t fln = 0;
    if (!d) return 2;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "lib_", 4) && !strncmp(e->d_name + 4, fsprefix, strlen(fsprefix)) && strstr(e->d_name, ".bin")) { snprintf(path, sizeof path, "%s/%s", dir, e->d_name); fl = slurp(path, &fln); }
    if (!fl || lagfx_metallib_extract_functions(fl, fln, ffn, 8, &nff) != LAGFX_OK) { fprintf(stderr, "no fragment lib %s\n", fsprefix); return 2; }
    const lagfx_metallib_function_t *ff = NULL;
    for (size_t k = 0; k < nff; k++) if (ffn[k].bitcode && ffn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) { ff = &ffn[k]; break; }
    if (!ff) return 2;
    (void)fn_n;
    rewinddir(d);
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "lib_", 4) || !strstr(e->d_name, ".bin")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t n; uint8_t *l = slurp(path, &n); lagfx_metallib_function_t fn[8]; size_t nf = 0;
        if (!l || lagfx_metallib_extract_functions(l, n, fn, 8, &nf) != LAGFX_OK) { free(l); continue; }
        for (size_t k = 0; k < nf; k++) {
            if (!fn[k].bitcode || fn[k].stage != LAGFX_METALLIB_STAGE_VERTEX) continue;
            lagfx_air_module_t *vm = NULL, *fm = NULL; uint8_t *vspv = NULL, *fspv = NULL; size_t vsz = 0, fsz = 0; lagfx_stage_keys_t vk; memset(&vk, 0, sizeof vk);
            if (lagfx_air_module_open(fn[k].bitcode, fn[k].bitcode_len, &vm) != LAGFX_OK || lagfx_air_module_open(ff->bitcode, ff->bitcode_len, &fm) != LAGFX_OK) continue;
            if (!lagfx_air_stage_keys(vm, true, &vk)) memset(&vk, 0, sizeof vk);
            if (lagfx_air2spv_translate_module_ex(vm, NULL, &vspv, &vsz) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &vk, &fspv, &fsz) == LAGFX_OK) {
                lagfx_spv_offset_bindings(fspv, fsz, LAGFX_A64_FRAG_BINDING_BASE);
                printf("vs lib %.*s %s + fs %s: vs[bytes=%zu hash=%016llx] fs[bytes=%zu hash=%016llx]\n", (int)strlen(e->d_name) - 8, e->d_name + 4, fn[k].name, ff->name, vsz, (unsigned long long)fnv64(vspv, vsz), fsz, (unsigned long long)fnv64(fspv, fsz));
            }
            free(vspv); free(fspv); lagfx_air_module_free(vm); lagfx_air_module_free(fm);
        }
        free(l);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir> [vs-size fs-size | vs-hash fs-hash | --cross <fs lib hash prefix>]\n", argv[0]); return 2; }
    if (argc >= 4 && !strcmp(argv[2], "--cross")) return cross(argv[1], argv[3]);
    char path[1200]; snprintf(path, sizeof path, "%s/index.txt", argv[1]);
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 2; }
    static char line[8192], psos[1024][2048]; int np = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned ref, ftask = 0; char file[96];
        if (!strncmp(line, "fn ", 3) && strstr(line, " ref=") && strstr(line, " file=") && g_nfn < 1024) {
            ref = (unsigned)atoi(strstr(line, " ref=") + 5); sscanf(strstr(line, " file=") + 6, "%95s", file); sscanf(line + 3, "task=%u", &ftask);
            g_fn[g_nfn].task = ftask, g_fn[g_nfn].ref = ref; snprintf(g_fn[g_nfn].file, sizeof g_fn[g_nfn].file, "%s", file); g_nfn++;
        } else if (!strncmp(line, "pso ", 4) && np < 1024) snprintf(psos[np++], sizeof psos[0], "%s", line);
    }
    fclose(f);
    int nmatch = 0;
    for (int i = 0; i < np; i++) {
        unsigned task = 0, ref, refs[8], nr = 0;
        char *pr = strstr(psos[i], " ref="), *fr = strstr(psos[i], "function_refs=[");
        if (!pr || !fr || sscanf(psos[i] + 4, "task=%u", &task) != 1) continue;
        ref = (unsigned)atoi(pr + 5);
        for (const char *c = fr + 15; *c && *c != ']' && nr < 8; ) { refs[nr++] = (unsigned)strtoul(c, (char **)&c, 10); if (*c == ',') c++; }
        const lagfx_metallib_function_t *vf = NULL, *ff = NULL; lib_t libs[8]; int nl = 0;
        memset(libs, 0, sizeof libs);
        for (unsigned q = 0; q < nr && (!vf || !ff); q++) {
            const char *file = NULL;
            for (int k = 0; k < g_nfn; k++) if (g_fn[k].ref == refs[q] && g_fn[k].task == task) { file = g_fn[k].file; break; }
            if (!file) continue;
            snprintf(path, sizeof path, "%s/%s", argv[1], file);
            lib_t *l = &libs[nl]; l->lib = slurp(path, &l->len);
            if (!l->lib || lagfx_metallib_extract_functions(l->lib, l->len, l->fn, 8, &l->nfn) != LAGFX_OK) continue;
            nl++;
            for (size_t k = 0; k < l->nfn && k < 8; k++) {
                if (!l->fn[k].bitcode || !l->fn[k].bitcode_len) continue;
                if (!vf && l->fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &l->fn[k];
                if (!ff && l->fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) ff = &l->fn[k];
            }
        }
        if (!vf || !ff) continue;
        lagfx_air_module_t *vm = NULL, *fm = NULL; uint8_t *vspv = NULL, *fspv = NULL; size_t vsz = 0, fsz = 0;
        lagfx_stage_keys_t vk; memset(&vk, 0, sizeof vk);
        if (lagfx_air_module_open(vf->bitcode, vf->bitcode_len, &vm) != LAGFX_OK || lagfx_air_module_open(ff->bitcode, ff->bitcode_len, &fm) != LAGFX_OK) continue;
        if (!lagfx_air_stage_keys(vm, true, &vk)) memset(&vk, 0, sizeof vk);
        if (lagfx_air2spv_translate_module_ex(vm, NULL, &vspv, &vsz) != LAGFX_OK || lagfx_air2spv_translate_module_ex(fm, &vk, &fspv, &fsz) != LAGFX_OK) { printf("pso task=%u ref=%u %s+%s: translate failed\n", task, ref, vf->name, ff->name); continue; }
        size_t fsz_raw = fsz; uint64_t fh_raw = fnv64(fspv, fsz);
        lagfx_spv_offset_bindings(fspv, fsz, LAGFX_A64_FRAG_BINDING_BASE);
        uint64_t vh = fnv64(vspv, vsz), fh = fnv64(fspv, fsz);
        int show = 1;
        if (argc >= 4) {
            char a[40], b[40]; snprintf(a, sizeof a, "%zu", vsz), snprintf(b, sizeof b, "%zu", fsz);
            char ha[40], hb[40]; snprintf(ha, sizeof ha, "%016llx", (unsigned long long)vh), snprintf(hb, sizeof hb, "%016llx", (unsigned long long)fh);
            show = (!strcmp(argv[2], a) && !strcmp(argv[3], b)) || (!strcmp(argv[2], ha) && !strcmp(argv[3], hb));
        }
        if (show) { nmatch++; printf("pso task=%u ref=%u %s + %s: vs[bytes=%zu hash=%016llx] fs[bytes=%zu hash=%016llx] (fs before offset_bindings: bytes=%zu hash=%016llx)\n", task, ref, vf->name, ff->name, vsz, (unsigned long long)vh, fsz, (unsigned long long)fh, fsz_raw, (unsigned long long)fh_raw); }
        free(vspv); free(fspv); lagfx_air_module_free(vm); lagfx_air_module_free(fm);
        for (int q = 0; q < nl; q++) free(libs[q].lib);
    }
    fprintf(stderr, "%d PSOs listed\n", nmatch);
    return 0;
}
