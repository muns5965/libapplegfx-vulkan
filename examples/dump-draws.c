/* For every `draw` line of an arm64 GPU dump (index.txt: vbuf[idx:obj@off] fbuf[..] ftex[idx:obj] fsamp[idx:obj]) translates the PSO's vertex+fragment shaders, runs the
 * resolver's own binding map (lagfx_a64_map_stage_bindings) and reports per draw every binding the draw would leave unbound / unaligned and why. What the dump cannot say
 * (buffer lengths, whether a texture object is linear / resolvable, vertex-stage texture and sampler tables, which refs are in the exec's resource list) is printed as
 * "?" rather than guessed. usage: dump-draws <dump dir> [min storage buffer offset alignment, default 64]. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "protocol/arm64_pipeline.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t task, ref; char file[96]; } fnent_t;
static fnent_t g_fn[512]; static int g_nfn;
typedef struct { uint32_t task, ref, n, r[16]; } psoent_t;   /* function refs, first-seen order */
static psoent_t g_pso[512]; static int g_npso;
typedef struct { uint32_t task, ref; char surf[48]; } mapent_t;
static mapent_t g_map[512]; static int g_nmap;
typedef struct { uint32_t task, ref; int tag; } objent_t;
typedef struct { uint32_t task, ref, type, tag, lin_buf; } resent_t;   /* `res` lines (newer dumps): buffer / texture / linear texture entries of the resource list */
static resent_t g_res[1024]; static int g_nres;
static objent_t g_obj[512]; static int g_nobj;
typedef struct { bool have_pso; uint32_t nb; lagfx_a64_bind_t b[LAGFX_A64_MAX_BINDINGS]; int rc[2], why[2]; bool ok; } resolved_t;

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static const char *kname(int k) { static const char *n[] = { "?", "texture", "sampler", "buffer", "input", "storage_img" }; return k >= 0 && k < 6 ? n[k] : "?"; }
static const char *fnfile(const char *dir, uint32_t task, uint32_t ref, char *out, size_t cap) {
    for (int k = 0; k < g_nfn; k++) if (g_fn[k].ref == ref && g_fn[k].task == task) { snprintf(out, cap, "%s/%s", dir, g_fn[k].file); return out; }
    return NULL;
}
static const char *mapsurf(uint32_t task, uint32_t ref) { for (int i = 0; i < g_nmap; i++) if (g_map[i].task == task && g_map[i].ref == ref) return g_map[i].surf; return NULL; }
static const char *objtag(uint32_t task, uint32_t ref) { for (int i = 0; i < g_nobj; i++) if (g_obj[i].task == task && g_obj[i].ref == ref) return g_obj[i].tag == 3 ? "sampler-state" : "pso/other"; return NULL; }

/* translates + maps the bindings of a PSO; same sequence as lagfx_a64_pipeline_resolve (vertex first for its stage keys, fragment offset by LAGFX_A64_FRAG_BINDING_BASE) */
static void resolve_pso(const char *dir, const psoent_t *ps, resolved_t *R) {
    memset(R, 0, sizeof *R);
    const lagfx_metallib_function_t *vf = NULL, *ff = NULL; uint8_t *libs[16] = { NULL }; lagfx_metallib_function_t fns[16][8]; size_t nfn[16] = { 0 };
    const uint32_t *refs = ps->r;
    for (uint32_t q = 0; q < ps->n && q < 16 && (!vf || !ff); q++) {   /* as the resolver: the first vertex and the first fragment function over the refs */
        char path[1300]; size_t len;
        if (!fnfile(dir, ps->task, refs[q], path, sizeof path)) continue;
        libs[q] = slurp(path, &len);
        if (!libs[q] || lagfx_metallib_extract_functions(libs[q], len, fns[q], 8, &nfn[q]) != LAGFX_OK) continue;
        for (size_t k = 0; k < nfn[q] && k < 8; k++) {
            if (!fns[q][k].bitcode) continue;
            if (!vf && fns[q][k].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &fns[q][k];
            if (!ff && fns[q][k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) ff = &fns[q][k];
        }
    }
    if (vf && ff) {
        lagfx_air_module_t *vm = NULL, *fm = NULL; uint8_t *vspv = NULL, *fspv = NULL; size_t vsz = 0, fsz = 0;
        lagfx_air_arg_binding_t vab[LAGFX_A64_MAX_ARGS], fab[LAGFX_A64_MAX_ARGS]; size_t nvab = 0, nfab = 0;
        lagfx_stage_keys_t vk; memset(&vk, 0, sizeof vk);
        if (lagfx_air_module_open(vf->bitcode, vf->bitcode_len, &vm) == LAGFX_OK && lagfx_air_module_open(ff->bitcode, ff->bitcode_len, &fm) == LAGFX_OK) {
            if (!lagfx_air_stage_keys(vm, true, &vk)) memset(&vk, 0, sizeof vk);
            if (lagfx_air2spv_translate_module_ex(vm, NULL, &vspv, &vsz) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &vk, &fspv, &fsz) == LAGFX_OK) {
                nvab = lagfx_air_arg_bindings(vm, vab, LAGFX_A64_MAX_ARGS), nfab = lagfx_air_arg_bindings(fm, fab, LAGFX_A64_MAX_ARGS);
                if (nvab > 16) nvab = 16;
                if (nfab > 16) nfab = 16;
                lagfx_spv_offset_bindings(fspv, fsz, LAGFX_A64_FRAG_BINDING_BASE);
                R->ok = true;
                R->rc[0] = lagfx_a64_map_stage_bindings(vspv, vsz, 0, vab, nvab, true, true, R->b, &R->nb, &R->why[0]);
                if (R->rc[0] == LAGFX_A64_MAP_OK) R->rc[1] = lagfx_a64_map_stage_bindings(fspv, fsz, 1, fab, nfab, true, true, R->b, &R->nb, &R->why[1]);
            }
        }
        free(vspv); free(fspv); if (vm) lagfx_air_module_free(vm); if (fm) lagfx_air_module_free(fm);
    }
    for (int q = 0; q < 16; q++) free(libs[q]);
}

/* finds "idx:ref[@off]" in the bracket list `name[...]` of a draw line */
static bool slot(const char *line, const char *name, uint32_t idx, uint32_t *ref, uint64_t *off) {
    char key[16]; snprintf(key, sizeof key, " %s[", name);
    const char *p = strstr(line, key); if (!p) return false;
    p += strlen(key);
    const char *e = strchr(p, ']'); if (!e) return false;
    while (p < e) {
        unsigned i, r; unsigned long long o = 0; int n = 0;
        if (sscanf(p, "%u:%u%n", &i, &r, &n) < 2) break;
        p += n;
        if (*p == '@') { sscanf(p + 1, "%llu%n", &o, &n); p += 1 + n; }
        if (i == idx) { *ref = r; if (off) *off = o; return true; }
        while (p < e && *p == ' ') p++;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir> [min storage buffer offset alignment]\n", argv[0]); return 2; }
    unsigned align = argc > 2 ? (unsigned)atoi(argv[2]) : 64u;
    if (!align) align = 1;
    char path[1200]; snprintf(path, sizeof path, "%s/index.txt", argv[1]);
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 2; }
    static char line[8192]; static char draws[256][8192]; int nd = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned task = 0, ref = 0, a, b, tag = 0; char file[96];
        if (!strncmp(line, "fn ", 3) && strstr(line, " ref=") && strstr(line, " file=") && g_nfn < 512) {
            sscanf(line + 3, "task=%u", &task); sscanf(strstr(line, " ref=") + 5, "%u", &ref); sscanf(strstr(line, " file=") + 6, "%95s", file);
            g_fn[g_nfn].task = task, g_fn[g_nfn].ref = ref; snprintf(g_fn[g_nfn].file, sizeof g_fn[g_nfn].file, "%s", file); g_nfn++;
        } else if (!strncmp(line, "pso ", 4) && g_npso < 512 && strstr(line, "function_refs=[")) {
            sscanf(line + 4, "task=%u", &task); sscanf(strstr(line, " ref=") + 5, "%u", &ref);
            psoent_t *pe = &g_pso[g_npso]; pe->task = task, pe->ref = ref, pe->n = 0;
            for (const char *q = strstr(line, "function_refs=[") + 15; *q && *q != ']' && pe->n < 16; ) { pe->r[pe->n++] = (uint32_t)strtoul(q, (char **)&q, 10); if (*q == ',') q++; }
            g_npso++; (void)a; (void)b;
        } else if (!strncmp(line, "mapped ", 7) && g_nmap < 512 && strstr(line, " mapping=")) {
            sscanf(line + 7, "task=%u", &task); sscanf(strstr(line, " ref=") + 5, "%u", &ref);
            char *s = strstr(line, "surface="); char sf[40] = "?"; if (s) sscanf(s + 8, "%39s", sf);
            g_map[g_nmap].task = task, g_map[g_nmap].ref = ref; snprintf(g_map[g_nmap].surf, sizeof g_map[g_nmap].surf, "%s", sf); g_nmap++;
        } else if (!strncmp(line, "obj ", 4) && g_nobj < 512 && strstr(line, " tag=0x")) {
            sscanf(line + 4, "task=%u", &task); sscanf(strstr(line, " ref=") + 5, "%u", &ref); sscanf(strstr(line, " tag=0x") + 7, "%x", &tag);
            g_obj[g_nobj++] = (objent_t){ task, ref, (int)tag };
        } else if (!strncmp(line, "res ", 4) && g_nres < 1024 && strstr(line, " type=")) {
            unsigned ty = 0, lb = 0; sscanf(line + 4, "task=%u", &task); sscanf(strstr(line, " ref=") + 5, "%u", &ref); sscanf(strstr(line, " type=") + 6, "%u", &ty);
            if (strstr(line, " tag=0x")) sscanf(strstr(line, " tag=0x") + 7, "%x", &tag);
            if (strstr(line, " lin_buf=")) sscanf(strstr(line, " lin_buf=") + 9, "%u", &lb);
            g_res[g_nres++] = (resent_t){ task, ref, ty, tag, lb };
        } else if (!strncmp(line, "draw ", 5) && nd < 256) snprintf(draws[nd++], sizeof draws[0], "%s", line);
    }
    fclose(f);
    int nlin = 0, tot_unbound = 0, tot_unaligned = 0, tot_ok = 0, tot_nopso = 0, tot_mapfail = 0;
    for (int d = 0; d < nd; d++) {
        unsigned n = 0, task = 0, prim = 0, pref = 0, dtag = 0;
        sscanf(draws[d], "draw n=%u task=%u draw_tag=%u prim=%u pipeline=%u", &n, &task, &dtag, &prim, &pref);
        printf("draw %u (task %u tag %u pipeline %u):", n, task, dtag, pref);
        const psoent_t *ps = NULL;
        for (int i = 0; i < g_npso; i++) if (g_pso[i].task == task && g_pso[i].ref == pref) ps = &g_pso[i];
        if (!ps) { printf(" PSO %u not in dump (no_pipeline cause: object never captured)\n", pref); tot_nopso++; continue; }
        resolved_t R; resolve_pso(argv[1], ps, &R);
        if (!R.ok) { printf(" shaders not translatable offline\n"); continue; }
        if (R.rc[0] != LAGFX_A64_MAP_OK || R.rc[1] != LAGFX_A64_MAP_OK) { printf(" resolver binding map FAILS (vertex rc=%d, fragment rc=%d, fbfetch why=%d/%d)\n", R.rc[0], R.rc[1], R.why[0], R.why[1]); tot_mapfail++; continue; }
        int bad = 0, unal = 0;
        printf(" %u bindings\n", R.nb);
        for (uint32_t k = 0; k < R.nb; k++) {
            const lagfx_a64_bind_t *bd = &R.b[k]; uint32_t ref = 0; uint64_t off = 0; char msg[160] = "";
            const char *tab = NULL;
            if (bd->kind == LAGFX_A64_BIND_BUFFER) {
                tab = bd->stage ? "fbuf" : "vbuf";
                if (bd->metal_index < 0) snprintf(msg, sizeof msg, "UNBOUND (no metal index)"), bad++;
                else if (!slot(draws[d], tab, (uint32_t)bd->metal_index, &ref, &off)) snprintf(msg, sizeof msg, "UNBOUND: %s slot %d never bound in the dump (desc_unbound[0])", tab, bd->metal_index), bad++;
                else if (off % align) snprintf(msg, sizeof msg, "obj %u off %llu: not a multiple of %u -> %s", ref, (unsigned long long)off, align, bd->writable ? "WRITABLE: draw skipped (desc_unaligned)" : "gathered copy (can fail: gather_failed)"), unal += bd->writable ? 1 : 0;
                else snprintf(msg, sizeof msg, "obj %u off %llu ok (length not in dump)", ref, (unsigned long long)off);
            } else if (bd->kind == LAGFX_A64_BIND_IMAGE || bd->kind == LAGFX_A64_BIND_STORAGE_IMAGE) {
                if (bd->stage == 0) snprintf(msg, sizeof msg, "? vertex-stage texture table (tag 129) is not in the dump");
                else if (bd->metal_index < 0 || !slot(draws[d], "ftex", (uint32_t)bd->metal_index, &ref, NULL)) snprintf(msg, sizeof msg, "UNBOUND: ftex slot %d never bound in the dump (desc_unbound[3])", bd->metal_index), bad++;
                else {
                    const char *ms = mapsurf(task, ref); const resent_t *re = NULL;
                    for (int i = 0; i < g_nres; i++) if (g_res[i].task == task && g_res[i].ref == ref) re = &g_res[i];
                    if (re && (re->type == 8u || re->type == 9u)) { snprintf(msg, sizeof msg, "obj %u LINEAR texture (type %u tag 0x%x, buffer ref %u)", ref, re->type, re->tag, re->lin_buf); nlin++; }
                    else if (re) snprintf(msg, sizeof msg, "obj %u type %u (not linear)", ref, re->type);
                    else snprintf(msg, sizeof msg, "obj %u %s%s (linear or not: no `res` line, dump predates it)", ref, ms ? "mapped surface " : "kind unknown", ms ? ms : "");
                }
            } else if (bd->kind == LAGFX_A64_BIND_SAMPLER) {
                if (bd->metal_index < 0) snprintf(msg, sizeof msg, "inline sampler: default sampler");
                else if (bd->stage == 0) snprintf(msg, sizeof msg, "? vertex sampler table not in the dump");
                else if (!slot(draws[d], "fsamp", (uint32_t)bd->metal_index, &ref, NULL)) snprintf(msg, sizeof msg, "UNBOUND: fsamp slot %d never bound in the dump (desc_unbound[5])", bd->metal_index), bad++;
                else { const char *o = objtag(task, ref); snprintf(msg, sizeof msg, "obj %u %s", ref, o ? o : "not captured in the dump"); }
            } else snprintf(msg, sizeof msg, "input attachment (colour %d)", bd->metal_index);
            printf("   %s b%-3u %s#%d: %s\n", kname(bd->kind), bd->binding, bd->stage ? "frag" : "vert", bd->metal_index, msg);
        }
        if (bad) tot_unbound++; else if (unal) tot_unaligned++; else tot_ok++;
    }
    printf("%d sampled textures are linear (buffer backed) per the dump\n", nlin);
    printf("%d draws: %d with an unbound binding, %d with an unaligned writable buffer, %d bind-complete, %d PSO missing, %d binding map failed (min storage alignment %u)\n", nd, tot_unbound, tot_unaligned, tot_ok, tot_nopso, tot_mapfail, align);
    return 0;
}
