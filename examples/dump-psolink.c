/* Offline replay of the pipeline resolver's stage selection + translation for every render PSO (tag 0xe) of an arm64 dump, with two selections:
 *   old = the function refs the dump recorded ("pso ... function_refs=[...]" = the decoder that RAN: first scanned `04 <u32>` token naming a function = vertex / fragment by
 *         the stage of the library, first of each kind wins; the pre-root resolver loop), and
 *   new = lagfx_a64_pdesc_decode(): vertex = root tag 1, fragment = root tag 2, each of the matching stage, a bad root is refused.
 * For each selection the vertex function is translated, then the fragment function with the vertex keys (as lagfx_a64_pipeline_resolve does) and the stage-link counters are
 * summed: the "missing zeroed" total is what g7-gpu-pipe's stage_link[missing_zeroed=] counts. A PSO that was never resolved in the real run (no draw) is counted here too.
 * usage: dump-psolink <dump dir> [-v]. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "protocol/arm64_pipeline.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t ref; char file[96]; } fnent_t;
static fnent_t g_fn[512]; static int g_nfn;
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static const char *g_dir;
static bool is_fn(void *ctx, uint32_t ref) { (void)ctx; for (int i = 0; i < g_nfn; i++) if (g_fn[i].ref == ref) return true; return false; }

typedef struct { uint8_t *lib; size_t len; lagfx_metallib_function_t fn[8]; size_t nfn; } lib_t;
/* the library of function object `ref` (NULL if the dump has none) */
static bool load_lib(uint32_t ref, lib_t *l) {
    memset(l, 0, sizeof *l);
    for (int i = 0; i < g_nfn; i++) if (g_fn[i].ref == ref) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", g_dir, g_fn[i].file);
        l->lib = slurp(p, &l->len);
        if (l->lib && lagfx_metallib_extract_functions(l->lib, (uint32_t)l->len, l->fn, 8, &l->nfn) == LAGFX_OK) return true;
        return false;
    }
    return false;
}
static const lagfx_metallib_function_t *stage_fn(const lib_t *l, int stage) {
    for (size_t k = 0; k < l->nfn && k < 8; k++) if (l->fn[k].bitcode && l->fn[k].bitcode_len && (int)l->fn[k].stage == stage) return &l->fn[k];
    return NULL;
}
typedef struct { char vs[128], fs[128]; uint32_t vref, fref; bool ok; uint64_t zeroed, fixed, unlinked; } res_t;

/* translate vertex, then fragment with the vertex keys; returns the link counter deltas */
static void translate_pair(const lagfx_metallib_function_t *vf, const lagfx_metallib_function_t *ff, res_t *r) {
    lagfx_air_module_t *vm = NULL, *fm = NULL; uint8_t *vspv = NULL, *fspv = NULL; size_t vs = 0, fs = 0; lagfx_stage_keys_t keys; memset(&keys, 0, sizeof keys);
    lagfx_stage_link_totals_t a, b;
    if (lagfx_air_module_open(vf->bitcode, vf->bitcode_len, &vm) != LAGFX_OK || !vm) return;
    if (!lagfx_air_stage_keys(vm, true, &keys)) memset(&keys, 0, sizeof keys);
    lagfx_air2spv_translate_module_ex(vm, NULL, &vspv, &vs);
    lagfx_air_module_free(vm);
    if (lagfx_air_module_open(ff->bitcode, ff->bitcode_len, &fm) != LAGFX_OK || !fm) { free(vspv); return; }
    lagfx_air2spv_link_totals(&a);
    lagfx_status_t st = lagfx_air2spv_translate_module_ex(fm, &keys, &fspv, &fs);
    lagfx_air2spv_link_totals(&b);
    lagfx_air_module_free(fm);
    r->ok = vspv && st == LAGFX_OK && fspv;
    r->zeroed = b.missing_zeroed - a.missing_zeroed; r->fixed = b.mismatch_fixed - a.mismatch_fixed; r->unlinked = b.unlinked - a.unlinked;
    free(vspv); free(fspv);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir> [-v]\n", argv[0]); return 2; }
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    g_dir = argv[1];
    char path[1200]; snprintf(path, sizeof path, "%s/index.txt", g_dir);
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 2; }
    static char line[8192];
    static struct { uint32_t ref; char file[96]; uint32_t old[16]; int nold; } pso[256]; int np = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned ref, task; char file[96];
        if (!strncmp(line, "fn ", 3) && sscanf(line + 3, "task=%u ref=%u", &task, &ref) == 2 && strstr(line, " file=") && g_nfn < 512) {
            sscanf(strstr(line, " file=") + 6, "%95s", file); g_fn[g_nfn].ref = ref; snprintf(g_fn[g_nfn].file, sizeof g_fn[g_nfn].file, "%s", file); g_nfn++;
        } else if (!strncmp(line, "obj ", 4) && strstr(line, " type=7 tag=0xe ") && np < 256 && sscanf(line + 4, "task=%u ref=%u", &task, &ref) == 2 && strstr(line, " file=")) {
            sscanf(strstr(line, " file=") + 6, "%95s", file); pso[np].ref = ref; snprintf(pso[np].file, sizeof pso[np].file, "%s", file); pso[np].nold = -1; np++;
        } else if (!strncmp(line, "pso ", 4) && sscanf(line + 4, "task=%u ref=%u", &task, &ref) == 2 && strstr(line, "function_refs=[")) {
            for (int i = 0; i < np; i++) if (pso[i].ref == ref) {
                const char *s = strstr(line, "function_refs=[") + 15; pso[i].nold = 0;
                while (*s && *s != ']' && pso[i].nold < 16) { pso[i].old[pso[i].nold++] = (uint32_t)strtoul(s, (char **)&s, 10); if (*s == ',') s++; }
            }
        }
    }
    fclose(f);
    unsigned long long tz_old = 0, tz_new = 0, fixed_old = 0, fixed_new = 0, ok_old = 0, ok_new = 0, refused = 0, differ = 0, fs_differs = 0;
    for (int i = 0; i < np; i++) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", g_dir, pso[i].file);
        size_t bn = 0; uint8_t *b = slurp(p, &bn); if (!b) continue;
        res_t o, n; memset(&o, 0, sizeof o); memset(&n, 0, sizeof n);
        /* old: the recorded scan order; first vertex-stage and first fragment-stage function among those libraries */
        const lagfx_metallib_function_t *ovf = NULL, *off = NULL; lib_t olibs[16]; int nol = 0;
        for (int k = 0; k < pso[i].nold && (!ovf || !off); k++) {
            if (!load_lib(pso[i].old[k], &olibs[nol])) continue;
            const lib_t *l = &olibs[nol++];
            const lagfx_metallib_function_t *v = stage_fn(l, LAGFX_METALLIB_STAGE_VERTEX), *fg = stage_fn(l, LAGFX_METALLIB_STAGE_FRAGMENT);
            if (!ovf && v) ovf = v, o.vref = pso[i].old[k];
            if (!off && fg) off = fg, o.fref = pso[i].old[k];
        }
        if (ovf && off) { snprintf(o.vs, sizeof o.vs, "%s", ovf->name); snprintf(o.fs, sizeof o.fs, "%s", off->name); translate_pair(ovf, off, &o); }
        /* new: root tags 1 / 2 only */
        lagfx_a64_pdesc_t d;
        const char *why = "ok";
        const lagfx_metallib_function_t *nvf = NULL, *nff = NULL; lib_t nl[2]; memset(nl, 0, sizeof nl);
        if (!lagfx_a64_pdesc_decode(b, (uint32_t)bn, is_fn, NULL, &d)) why = "decode";
        else if (d.root_status != LAGFX_A64_ROOT_OK) why = lagfx_a64_root_status_name((int)d.root_status);
        else {
            bool lv = load_lib(d.refs[0], &nl[0]), lf = load_lib(d.refs[1], &nl[1]);
            if (lv) nvf = stage_fn(&nl[0], LAGFX_METALLIB_STAGE_VERTEX);
            if (lf) nff = stage_fn(&nl[1], LAGFX_METALLIB_STAGE_FRAGMENT);
            if (!nvf || !nff) why = "wrong_stage", nvf = nff = NULL;
        }
        if (nvf && nff) { n.vref = d.refs[0], n.fref = d.refs[1]; snprintf(n.vs, sizeof n.vs, "%s", nvf->name); snprintf(n.fs, sizeof n.fs, "%s", nff->name); translate_pair(nvf, nff, &n); }
        else refused++;
        ok_old += o.ok; ok_new += n.ok; tz_old += o.zeroed; tz_new += n.zeroed; fixed_old += o.fixed; fixed_new += n.fixed;
        int diff = strcmp(o.fs, n.fs) || strcmp(o.vs, n.vs) || o.fref != n.fref || o.vref != n.vref; differ += diff; fs_differs += o.fref != n.fref;
        if (verbose || diff) printf("pso %-4u old: vs=%u(%s) fs=%u(%s) zeroed=%llu | new: vs=%u(%s) fs=%u(%s) zeroed=%llu [%s]%s\n", pso[i].ref, o.vref, o.vs, o.fref, o.fs, (unsigned long long)o.zeroed,
                              n.vref, n.vs, n.fref, n.fs, (unsigned long long)n.zeroed, why, diff ? " DIFFERS" : "");
        for (int k = 0; k < nol; k++) free(olibs[k].lib);
        free(nl[0].lib); free(nl[1].lib); free(b);
    }
    printf("render PSOs: %d  translated old=%llu new=%llu  refused(new)=%llu  PSOs whose stage functions differ=%llu (fragment ref differs=%llu)\n", np, ok_old, ok_new, refused, differ, fs_differs);
    printf("stage_link missing_zeroed: old selection=%llu  new selection=%llu   (mismatch_fixed old=%llu new=%llu)\n", tz_old, tz_new, fixed_old, fixed_new);
    return 0;
}
