/* Fragment [[position]] binding. A float4 "air.position" fragment argument becomes BuiltIn FragCoord (no Location) only when the pair keys are supplied, i.e. when
 * lagfx_air2spv_translate_module_ex() gets the paired vertex module's output keys (what lagfx_a64_pipeline_resolve passes, src/protocol/arm64_pipeline.c). Without
 * keys (lagfx_air2spv_translate_module(), as the x86 draw path compute_inner_ops.c and the X11 fixtures call it) the argument is a plain stage-in at the next free
 * Location. The fallback case documents that behaviour; it is NOT an endorsement of it.
 * Fixture tests/fixtures/frag_position_fragment.ll: HAND-WRITTEN AIR (llvm-as), not Apple output. Exit 77 when spirv-val is missing.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

/* 0 = valid, 1 = rejected, -1 = spirv-val not runnable */
static int spirv_val(const uint8_t *spv, size_t sz) {
    char tmpl[] = "/tmp/lagfx-fpos-XXXXXX";
    int fd = mkstemp(tmpl); if (fd < 0) return -1;
    if (write(fd, spv, sz) != (ssize_t)sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    pid_t pid = fork();
    if (pid == 0) { execlp("spirv-val", "spirv-val", tmpl, (char *)NULL); _exit(127); }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) == 127) { unlink(tmpl); return -1; }
    unlink(tmpl);
    return WEXITSTATUS(st) ? 1 : 0;
}

typedef struct { int n_fragcoord, n_loc_on_fragcoord, n_input_locs, locs[8], n_input_vec4; } info_t;

/* OpDecorate (71): BuiltIn = 11 (FragCoord = 15), Location = 30. Storage class Input = 1; only Input variables count. */
static void scan(const uint8_t *blob, size_t sz, info_t *r) {
    const uint32_t *w = (const uint32_t *)(const void *)blob; size_t nw = sz / 4u;
    memset(r, 0, sizeof *r);
    uint32_t bound = w[3];
    uint8_t *frag = calloc(bound + 1u, 1), *hasloc = calloc(bound + 1u, 1), *isin = calloc(bound + 1u, 1);
    uint32_t *loc = calloc(bound + 1u, 4);
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16; if (!wc) break;
        if (op == 71 && wc >= 4 && w[i + 2] == 11u && w[i + 3] == 15u) { frag[w[i + 1]] = 1; r->n_fragcoord++; }
        if (op == 71 && wc >= 4 && w[i + 2] == 30u) { hasloc[w[i + 1]] = 1; loc[w[i + 1]] = w[i + 3]; }
        if (op == 59 && w[i + 3] == 1u) isin[w[i + 2]] = 1;
        i += wc;
    }
    for (uint32_t id = 1; id <= bound; id++) {
        if (frag[id] && hasloc[id]) r->n_loc_on_fragcoord++;
        if (isin[id] && hasloc[id] && r->n_input_locs < 8) r->locs[r->n_input_locs++] = (int)loc[id];
    }
    free(frag); free(hasloc); free(isin); free(loc);
}

static uint8_t *translate(const lagfx_stage_keys_t *keys, size_t *sz) {
    size_t n = 0; uint8_t *bc = slurp("frag_position_fragment.air.bc", &n);
    CHECK(bc, "fixture missing");
    if (!bc) return NULL;
    lagfx_air_module_t *m = NULL; uint8_t *spv = NULL;
    CHECK(lagfx_air_module_open(bc, n, &m) == LAGFX_OK && m, "module_open");
    if (m) {
        lagfx_status_t st = keys ? lagfx_air2spv_translate_module_ex(m, keys, &spv, sz) : lagfx_air2spv_translate_module(m, &spv, sz);
        CHECK(st == LAGFX_OK && spv, "translate");
        lagfx_air_module_free(m);
    }
    free(bc);
    return spv;
}

int main(void) {
    int val_skipped = 0;

    /* vertex-output keys of a paired vertex shader: [[position]] then the colour varying (both float4) */
    lagfx_stage_keys_t vk; memset(&vk, 0, sizeof vk);
    vk.n = 2; vk.typed = 1;
    snprintf(vk.key[0], LAGFX_STAGE_KEY_LEN, "air.position");          vk.kind[0] = LAGFX_SHAPE_FLOAT; vk.lanes[0] = 4;
    snprintf(vk.key[1], LAGFX_STAGE_KEY_LEN, "generated(5colorDv4_f)"); vk.kind[1] = LAGFX_SHAPE_FLOAT; vk.lanes[1] = 4;

    size_t sz = 0; uint8_t *spv = translate(&vk, &sz);
    if (spv) {
        info_t in; scan(spv, sz, &in);
        CHECK(in.n_fragcoord == 1, "keys supplied: want exactly one BuiltIn FragCoord (OpDecorate BuiltIn 15), got %d", in.n_fragcoord);
        CHECK(in.n_loc_on_fragcoord == 0, "keys supplied: FragCoord must carry no Location");
        CHECK(in.n_input_locs == 1, "keys supplied: only the colour varying keeps a Location (got %d)", in.n_input_locs);
        int v = spirv_val(spv, sz); if (v < 0) val_skipped = 1; CHECK(v <= 0, "keys supplied: spirv-val rejected the module");
        free(spv);
    }

    /* no keys: documented fallback, both arguments are ordinary stage-ins at Locations 0 and 1 (no FragCoord) */
    spv = translate(NULL, &sz);
    if (spv) {
        info_t in; scan(spv, sz, &in);
        CHECK(in.n_fragcoord == 0, "no keys: fallback does not bind FragCoord (got %d)", in.n_fragcoord);
        CHECK(in.n_input_locs == 2 && in.locs[0] + in.locs[1] == 1, "no keys: position and colour take Locations 0 and 1 (got %d locs)", in.n_input_locs);
        int v = spirv_val(spv, sz); if (v < 0) val_skipped = 1; CHECK(v <= 0, "no keys: spirv-val rejected the module");
        free(spv);
    }

    printf("%s air2spv-fragment-position-unit%s\n", bad ? "FAIL" : "PASS", val_skipped ? " (spirv-val not available: validation skipped)" : "");
    return bad ? 1 : 0;
}
