/* Per render PSO (tag 0xe) of an arm64 dump: the vertex attributes lagfx_a64_pdesc_decode() decodes from the serialized MTLVertexDescriptor versus the Location inputs the
 * translated vertex shader reflects (lagfx_spv_reflect_vertex_inputs) - the two counts and the locations/formats the vertex-input state builder
 * (lagfx_pipeline_vertex_input) works from. Prints one line per PSO and a distribution; exit 0.
 * usage: dump-vtxattrs <dump dir> [-v]. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "protocol/arm64_pipeline.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t ref; char file[96]; } fnent_t;
static fnent_t g_fn[512]; static int g_nfn;
static const char *g_dir;
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static bool is_fn(void *ctx, uint32_t ref) { (void)ctx; for (int i = 0; i < g_nfn; i++) if (g_fn[i].ref == ref) return true; return false; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir> [-v]\n", argv[0]); return 2; }
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    g_dir = argv[1];
    char path[1200]; snprintf(path, sizeof path, "%s/index.txt", g_dir);
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 2; }
    static char line[8192];
    static struct { uint32_t ref; char file[96]; } pso[256]; int np = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned ref, task; char file[96];
        if (!strncmp(line, "fn ", 3) && sscanf(line + 3, "task=%u ref=%u", &task, &ref) == 2 && strstr(line, " file=") && g_nfn < 512) {
            sscanf(strstr(line, " file=") + 6, "%95s", file); g_fn[g_nfn].ref = ref; snprintf(g_fn[g_nfn].file, sizeof g_fn[g_nfn].file, "%s", file); g_nfn++;
        } else if (!strncmp(line, "obj ", 4) && strstr(line, " type=7 tag=0xe ") && np < 256 && sscanf(line + 4, "task=%u ref=%u", &task, &ref) == 2 && strstr(line, " file=")) {
            sscanf(strstr(line, " file=") + 6, "%95s", file); pso[np].ref = ref; snprintf(pso[np].file, sizeof pso[np].file, "%s", file); np++;
        }
    }
    fclose(f);
    unsigned dist[9][9]; memset(dist, 0, sizeof dist);
    unsigned n_vi = 0, n_novi = 0, n_cover = 0, n_fmtmiss = 0, n_norm = 0, n_undec = 0;
    for (int i = 0; i < np; i++) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", g_dir, pso[i].file);
        size_t bn = 0; uint8_t *b = slurp(p, &bn); if (!b) continue;
        lagfx_a64_pdesc_t d;
        if (!lagfx_a64_pdesc_decode(b, (uint32_t)bn, is_fn, NULL, &d) || d.root_status != LAGFX_A64_ROOT_OK) { n_undec++; if (verbose) printf("pso %u: not decodable / root refused\n", pso[i].ref); free(b); continue; }
        size_t nvi = 0; lagfx_spv_vertex_input_t vin[8]; memset(vin, 0, sizeof vin);
        uint8_t *lib = NULL; size_t ll = 0; lagfx_metallib_function_t fn[8]; size_t nfn = 0; const lagfx_metallib_function_t *vf = NULL; uint8_t *vspv = NULL; size_t vs = 0;
        for (int k = 0; k < g_nfn; k++) if (g_fn[k].ref == d.refs[0]) {
            snprintf(p, sizeof p, "%s/%s", g_dir, g_fn[k].file); lib = slurp(p, &ll);
            if (lib && lagfx_metallib_extract_functions(lib, (uint32_t)ll, fn, 8, &nfn) == LAGFX_OK)
                for (size_t q = 0; q < nfn && q < 8; q++) if (!vf && fn[q].bitcode && fn[q].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &fn[q];
        }
        if (vf) {
            lagfx_air_module_t *m = NULL;
            if (lagfx_air_module_open(vf->bitcode, vf->bitcode_len, &m) == LAGFX_OK && m) {
                if (lagfx_air2spv_translate_module_ex(m, NULL, &vspv, &vs) == LAGFX_OK && vspv) { nvi = lagfx_spv_reflect_vertex_inputs(vspv, vs, vin, 8); if (nvi > 8) nvi = 8; }
                lagfx_air_module_free(m);
            }
        }
        if (!vspv) { n_undec++; if (verbose) printf("pso %u: vertex function not translated\n", pso[i].ref); free(lib); free(b); continue; }
        unsigned na = d.nattrs > 8 ? 8 : d.nattrs;
        dist[nvi][na]++;
        if (nvi) n_vi++; else n_novi++;
        int cover = nvi && na >= nvi; if (cover) n_cover++;
        printf("pso %-4u vs=%s reflected=%zu decoded=%u stride=%u %s | refl:", pso[i].ref, vf->name, nvi, d.nattrs, d.stride, nvi == 0 ? "no-stage-in" : (na >= nvi ? (na == nvi ? "EQUAL" : "decoded>reflected") : "UNDERCOUNT"));
        for (size_t v = 0; v < nvi; v++) printf(" loc%u:c%u", vin[v].location, vin[v].components);
        printf(" | decoded:");
        for (uint32_t a = 0; a < na; a++) printf(" fmt%u@%u/buf%u", d.attrs[a].fmt, d.attrs[a].off, d.attrs[a].bufidx);
        printf("\n");
        for (size_t v = 0; v < nvi; v++) if (vin[v].location >= 8) n_norm++;
        (void)n_fmtmiss;
        free(vspv); free(lib); free(b);
    }
    printf("\nrender PSOs=%d  analysed=%u not-analysed=%u  with stage-in=%u  without stage-in=%u  decoded>=reflected (use_pso_attrs true)=%u  UNDERCOUNT(fallback)=%u\n",
           np, n_vi + n_novi, n_undec, n_vi, n_novi, n_cover, n_vi - n_cover);
    printf("distribution (reflected inputs x decoded attrs -> number of PSOs):\n");
    for (int r = 0; r < 9; r++) for (int a = 0; a < 9; a++) if (dist[r][a]) printf("  reflected=%d decoded=%d : %u\n", r, a, dist[r][a]);
    return 0;
}
