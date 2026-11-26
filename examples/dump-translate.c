/* Re-translates every function of the metallib blobs (lib_*.bin) of an arm64 dump with the CURRENT air2spv translator, so that translator fixes can be checked with
 * spirv-val against the guest's real shaders (no device needed). usage: dump-translate <dump dir> <out dir>
 * Writes <out>/<lib hash>_<function>_<vs|fs>.spv. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"
#include "vulkan/caps.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

/* vertex-output keys by library hash (filled by the first pass) */
typedef struct { char hash[24]; lagfx_stage_keys_t keys; int have; } vsk_t;
static vsk_t g_vsk[256]; static int g_nvsk;
static const lagfx_stage_keys_t *find_vsk(const char *h) {
    for (int i = 0; i < g_nvsk; i++) if (!strcmp(g_vsk[i].hash, h) && g_vsk[i].have) return &g_vsk[i].keys;
    return NULL;
}
/* pairs file: "<vs lib hash> <fs lib hash>" per line */
static char g_pairs[512][2][24]; static int g_npairs;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dump dir> <out dir> [pairs file]\n", argv[0]); return 2; }
    if (argc > 3) {
        FILE *pf = fopen(argv[3], "r");
        if (!pf) { perror("pairs"); return 2; }
        while (g_npairs < 512 && fscanf(pf, "%23s %23s", g_pairs[g_npairs][0], g_pairs[g_npairs][1]) == 2) g_npairs++;
        fclose(pf);
    }
    DIR *d = opendir(argv[1]); if (!d) { perror("opendir"); return 1; }
    struct dirent *e; int ok = 0, bad = 0;
    for (int pass = 0; pass < 2; pass++) {
    rewinddir(d);
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "lib_", 4) != 0) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        size_t n = 0; uint8_t *lib = slurp(path, &n); if (!lib) continue;
        lagfx_metallib_function_t fn[8]; size_t nf = 0;
        if (lagfx_metallib_extract_functions(lib, n, fn, 8, &nf) != LAGFX_OK) { free(lib); continue; }
        for (size_t k = 0; k < nf && k < 8; k++) {
            if (!fn[k].bitcode) continue;
            if (pass == 0 && fn[k].stage != LAGFX_METALLIB_STAGE_VERTEX) continue;
            if (pass == 1 && fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX && argc > 3) { /* vertex already keyed; still emit below */ }
            const char *st = fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX ? "vs" : fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT ? "fs" : "x";
            lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sz = 0;
            if (pass == 1 && getenv("DUMP_BC")) { /* DUMP_BC=<dir>: also write the extracted AIR bitcode (<dir>/<lib>_<fn>.air.bc) for air-bcdump */
                char bp[1400]; snprintf(bp, sizeof bp, "%s/%.*s_%s.air.bc", getenv("DUMP_BC"), (int)(strlen(e->d_name) - 8), e->d_name + 4, fn[k].name);
                FILE *bf = fopen(bp, "wb"); if (bf) { fwrite(fn[k].bitcode, 1, fn[k].bitcode_len, bf); fclose(bf); }
            }
            if (lagfx_air_module_open(fn[k].bitcode, fn[k].bitcode_len, &m) != LAGFX_OK || !m) { fprintf(stderr, "%s %s: module_open failed\n", e->d_name, fn[k].name); bad++; continue; }
            char libh[24]; snprintf(libh, sizeof libh, "%.*s", (int)(strlen(e->d_name) - 8), e->d_name + 4);
            if (pass == 1 && getenv("DUMP_KEYS")) {
                for (int o = 0; o < 2; o++) { lagfx_stage_keys_t kk;
                    if (lagfx_air_stage_keys(m, o, &kk)) for (uint32_t q = 0; q < kk.n; q++) printf("%s %s %s[%u]='%s'\n", libh, fn[k].name, o ? "out" : "in", q, kk.key[q]); }
            }
            if (pass == 0) {
                if (g_nvsk < 256 && lagfx_air_stage_keys(m, true, &g_vsk[g_nvsk].keys)) {
                    snprintf(g_vsk[g_nvsk].hash, sizeof g_vsk[g_nvsk].hash, "%s", libh); g_vsk[g_nvsk].have = 1; g_nvsk++;
                }
                lagfx_air_module_free(m); continue;
            }
            /* a fragment function is translated once per vertex function it is paired with (the Location assignment depends on the VS outputs):
             * "<fs>_<name>_fs__vs<vs hash>.spv"; an FS with no recorded pair, or a VS, is translated once without a suffix */
            int nvs = 0; const char *vsl[64];
            if (fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) for (int pi = 0; pi < g_npairs && nvs < 64; pi++) if (!strcmp(g_pairs[pi][1], libh)) { int dup = 0; for (int q = 0; q < nvs; q++) dup |= !strcmp(vsl[q], g_pairs[pi][0]); if (!dup) vsl[nvs++] = g_pairs[pi][0]; }
            int variants = nvs ? nvs : 1;
            for (int vi = 0; vi < variants; vi++) {
                const lagfx_stage_keys_t *vk = NULL;
                if (nvs) vk = find_vsk(vsl[vi]);
                if (nvs && !vk) { fprintf(stderr, "%s %s: translate failed (paired vertex shader %s not in the dump)\n", e->d_name, fn[k].name, vsl[vi]); bad++; continue; }
                lagfx_status_t s = lagfx_air2spv_translate_module_ex(m, vk, &spv, &sz);
                if (s != LAGFX_OK || !spv) { fprintf(stderr, "%s %s: translate failed\n", e->d_name, fn[k].name); bad++; continue; }
                char out[1400]; snprintf(out, sizeof out, "%s/%.*s_%s_%s", argv[2], (int)(strlen(e->d_name) - 8), e->d_name + 4, fn[k].name, st);
                for (char *c = out + strlen(argv[2]) + 1; *c; c++) if (*c == '.') *c = '_';
                size_t ol = strlen(out);
                if (nvs) snprintf(out + ol, sizeof out - ol, "__vs%.8s.spv", vsl[vi]); else snprintf(out + ol, sizeof out - ol, ".spv");
                FILE *o = fopen(out, "wb"); if (o) { fwrite(spv, 1, sz, o); fclose(o); ok++; } else { fprintf(stderr, "%s: cannot write\n", out); bad++; }
                if (getenv("DUMP_UNDEF")) { lagfx_spv_undef_report_t ur; lagfx_spv_undef_analyze(spv, sz, &ur);   /* DUMP_UNDEF=1: per-module OpUndef usage report (src/air2spv/spv_undef_report.c) */
                    printf("undef %s %s: defs=%u used=%u branch=%u switch=%u select_cond=%u store=%u\n", libh, fn[k].name, ur.undef_defs, ur.undef_used, ur.reach_branch, ur.reach_switch, ur.reach_select_cond, ur.reach_store); }
                if (getenv("DUMP_CAPS")) printf("caps %s %s: 0x%x\n", libh, fn[k].name, lagfx_spv_required_caps(spv, sz));
                free(spv); spv = NULL;
            }
            lagfx_air_module_free(m);
            continue;
        }
        free(lib);
    }
    }
    closedir(d);
    { lagfx_undef_totals_t t; lagfx_air2spv_undef_totals(&t);
      printf("undef summary: %llu modules, %llu with a possibly-undefined value in control flow (%llu control sinks), %llu undef values used, %llu stores of possibly-undefined values, %llu rejected (fail closed)\n",
             (unsigned long long)t.modules, (unsigned long long)t.modules_ctl, (unsigned long long)t.reach_ctl, (unsigned long long)t.undef_used, (unsigned long long)t.reach_store, (unsigned long long)t.rejected); }
    printf("translated %d functions, %d failed\n", ok, bad); return bad ? 1 : 0;
}
