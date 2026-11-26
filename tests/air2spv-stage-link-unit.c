/*
 * libapplegfx-vulkan — fragment stage linker (VS output / FS input types)
 * tests/air2spv-stage-link-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixture: login_tvcmxc_isrc_frag (fragment inputs: air.position, texcoord0 float2, color float4). The paired vertex shader is a synthetic typed key set
 * (what lagfx_air_stage_keys(outputs=true) returns for a vertex module), so each problem kind of the Adreno pair can be built exactly:
 *   match      same shapes                         -> nothing changes
 *   mismatch   VS float4 / float2 vs FS float2/4   -> Input has the VS type, converted (truncate / zero-fill)
 *   missing    no VS output for texcoord0          -> zero-initialised Private, no Input
 *   unlinked   VS int2 vs FS float2                -> left alone and counted
 *   disabled   typed = 0                           -> identical to the keys-only behaviour, no counters
 * Every module is run through spirv-val (vulkan1.3) when the tool is on PATH.
 */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f); if (b) *n = (size_t)sz; return b;
}

typedef struct { int n_input, n_private, n_null_init, locs[16], n_locs; uint32_t in_lanes[16]; } scan_t;

/* Walks the module: Input/Private variables, the Location decorations of the Input ones and each Input variable's vector lane count (1 = scalar). */
static void scan(const uint8_t *blob, size_t sz, scan_t *r) {
    const uint32_t *w = (const uint32_t *)(const void *)blob; size_t nw = sz / 4u;
    memset(r, 0, sizeof *r);
    uint32_t bound = w[3]; uint32_t *lanes = calloc(bound + 1u, 4), *ptee = calloc(bound + 1u, 4), *loc = calloc(bound + 1u, 4);
    uint8_t *hasloc = calloc(bound + 1u, 1);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 5; i < nw;) {
            uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16; if (!wc) break;
            if (pass == 0) {
                if (op == 71 && wc >= 4 && w[i + 2] == 30u) { loc[w[i + 1]] = w[i + 3]; hasloc[w[i + 1]] = 1; }   /* OpDecorate Location */
                if (op == 22) lanes[w[i + 1]] = 1;                       /* OpTypeFloat */
                if (op == 21) lanes[w[i + 1]] = 1;                       /* OpTypeInt */
                if (op == 23) lanes[w[i + 1]] = w[i + 3];                /* OpTypeVector */
                if (op == 32) ptee[w[i + 1]] = w[i + 3];                 /* OpTypePointer */
            } else if (op == 59) {                                       /* OpVariable */
                uint32_t sc = w[i + 3];
                if (sc == 1u && r->n_input < 16) { r->n_input++; if (hasloc[w[i + 2]] && r->n_locs < 16) { r->locs[r->n_locs] = (int)loc[w[i + 2]]; r->in_lanes[r->n_locs++] = lanes[ptee[w[i + 1]]]; } }
                if (sc == 6u) { r->n_private++; if (wc >= 5u) r->n_null_init++; }
            }
            i += wc;
        }
    free(lanes); free(ptee); free(loc); free(hasloc);
}

static void validate(const uint8_t *spv, size_t n, const char *tag) {
    if (system("command -v spirv-val >/dev/null 2>&1") != 0) { printf("  (spirv-val not found, validation skipped)\n"); return; }
    char path[96]; snprintf(path, sizeof path, "/tmp/stage-link-unit-%d-%s.spv", (int)getpid(), tag);
    FILE *f = fopen(path, "wb"); if (!f) return; fwrite(spv, 1, n, f); fclose(f);
    char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", path);
    int rc = system(cmd); CHECK(rc == 0, "spirv-val failed for %s (%s)", tag, path);
    if (rc == 0) remove(path);
}

static void vs_set(lagfx_stage_keys_t *k, uint32_t i, const char *key, uint8_t kind, uint8_t lanes) {
    snprintf(k->key[i], LAGFX_STAGE_KEY_LEN, "%s", key); k->kind[i] = kind; k->lanes[i] = lanes; if (i + 1u > k->n) k->n = i + 1u;
}

static const char *TC = "generated(10texcoord0ADv2_f)", *COL = "generated(5colorDv4_f)";

static void run(lagfx_air_module_t *m, const lagfx_stage_keys_t *vs, const char *tag, scan_t *sc, lagfx_stage_link_totals_t *d) {
    lagfx_stage_link_totals_t a, b; uint8_t *spv = NULL; size_t n = 0;
    lagfx_air2spv_link_totals(&a);
    lagfx_status_t st = lagfx_air2spv_translate_module_ex(m, vs, &spv, &n);
    lagfx_air2spv_link_totals(&b);
    CHECK(st == LAGFX_OK && spv, "%s: translate failed (%d)", tag, (int)st);
    memset(sc, 0, sizeof *sc);
    if (spv) { scan(spv, n, sc); validate(spv, n, tag); }
    d->mismatch_fixed = b.mismatch_fixed - a.mismatch_fixed; d->missing_zeroed = b.missing_zeroed - a.missing_zeroed; d->unlinked = b.unlinked - a.unlinked;
    printf("%-9s inputs=%d private=%d(null-init %d) locs:", tag, sc->n_input, sc->n_private, sc->n_null_init);
    for (int i = 0; i < sc->n_locs; i++) printf(" %d(v%u)", sc->locs[i], sc->in_lanes[i]);
    printf("  link[mismatch=%llu missing=%llu unlinked=%llu]\n", (unsigned long long)d->mismatch_fixed, (unsigned long long)d->missing_zeroed, (unsigned long long)d->unlinked);
    free(spv);
}

int main(void) {
    const char *cands[] = { "tests/fixtures/login_tvcmxc_isrc_frag.air.bc", SRCDIR "/fixtures/login_tvcmxc_isrc_frag.air.bc", NULL };
    uint8_t *air = NULL; size_t an = 0;
    for (int i = 0; cands[i] && !air; i++) air = slurp(cands[i], &an);
    if (!air) { printf("FAIL: fixture not found\n"); return 1; }
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, an, &m) != LAGFX_OK) { printf("FAIL: open\n"); return 1; }

    scan_t s; lagfx_stage_link_totals_t d;
    lagfx_stage_keys_t vs; memset(&vs, 0, sizeof vs); vs.typed = 1;

    /* match: position float4, texcoord0 float2, color float4 */
    vs_set(&vs, 0, "air.position", LAGFX_SHAPE_FLOAT, 4); vs_set(&vs, 1, TC, LAGFX_SHAPE_FLOAT, 2); vs_set(&vs, 2, COL, LAGFX_SHAPE_FLOAT, 4);
    run(m, &vs, "match", &s, &d);
    CHECK(d.mismatch_fixed == 0 && d.missing_zeroed == 0 && d.unlinked == 0, "match: no counters expected");
    CHECK(s.n_private == 0 && s.n_locs == 2 && s.locs[0] == 1 && s.locs[1] == 2 && s.in_lanes[0] == 2 && s.in_lanes[1] == 4, "match: unchanged inputs");
    int base_inputs = s.n_input;

    /* mismatch: texcoord0 VS float4 (FS float2: truncate), color VS float2 (FS float4: zero-fill the zw lanes), plus a scalar VS float for texcoord in a second run */
    vs_set(&vs, 1, TC, LAGFX_SHAPE_FLOAT, 4); vs_set(&vs, 2, COL, LAGFX_SHAPE_FLOAT, 2);
    run(m, &vs, "mismatch", &s, &d);
    CHECK(d.mismatch_fixed == 2 && d.missing_zeroed == 0 && d.unlinked == 0, "mismatch: expected 2 fixed");
    CHECK(s.n_locs == 2 && s.in_lanes[0] == 4 && s.in_lanes[1] == 2, "mismatch: the Inputs must carry the VS types (v4 at loc1, v2 at loc2)");
    vs_set(&vs, 1, TC, LAGFX_SHAPE_FLOAT, 1); vs_set(&vs, 2, COL, LAGFX_SHAPE_FLOAT, 4);
    run(m, &vs, "scalar", &s, &d);
    CHECK(d.mismatch_fixed == 1 && s.in_lanes[0] == 1, "scalar VS float -> float2 FS: fixed, Input is a scalar");

    /* missing: the vertex shader has no texcoord0 output */
    memset(&vs, 0, sizeof vs); vs.typed = 1;
    vs_set(&vs, 0, "air.position", LAGFX_SHAPE_FLOAT, 4); vs_set(&vs, 1, COL, LAGFX_SHAPE_FLOAT, 4);
    run(m, &vs, "missing", &s, &d);
    CHECK(d.missing_zeroed == 1 && d.mismatch_fixed == 0, "missing: expected 1 zeroed");
    CHECK(s.n_private == 1 && s.n_null_init == 1 && s.n_input == base_inputs - 1 && s.n_locs == 1 && s.locs[0] == 1 && s.in_lanes[0] == 4, "missing: Private with initialiser, colour keeps Location 1");

    /* unlinked: integer VS output for a float FS input */
    memset(&vs, 0, sizeof vs); vs.typed = 1;
    vs_set(&vs, 0, "air.position", LAGFX_SHAPE_FLOAT, 4); vs_set(&vs, 1, TC, LAGFX_SHAPE_INT, 2); vs_set(&vs, 2, COL, LAGFX_SHAPE_FLOAT, 4);
    run(m, &vs, "unlinked", &s, &d);
    CHECK(d.unlinked == 1 && d.mismatch_fixed == 0 && d.missing_zeroed == 0 && s.n_private == 0, "unlinked: counted, left as an Input");

    /* disabled (LAGFX_A64_NO_STAGE_LINK): untyped keys behave exactly as before */
    memset(&vs, 0, sizeof vs);
    vs_set(&vs, 0, "air.position", LAGFX_SHAPE_FLOAT, 4); vs_set(&vs, 1, COL, LAGFX_SHAPE_FLOAT, 4);
    run(m, &vs, "disabled", &s, &d);
    CHECK(d.mismatch_fixed == 0 && d.missing_zeroed == 0 && d.unlinked == 0 && s.n_private == 0, "disabled: nothing linked");

    lagfx_air_module_free(m); free(air);
    printf(g_fail ? "FAILED\n" : "PASS\n");
    return g_fail;
}
