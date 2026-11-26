/* AIR integer intrinsics -> SPIR-V (census item X11): translate hand-written AIR fixtures and check, per op, the emitted opcode / GLSL.std.450 instruction and
 * that spirv-val accepts the whole module.
 *
 * Fixtures tests/fixtures/int_*_fragment.air.bc come from the sibling .ll files, assembled with llvm-as: HAND-WRITTEN IR, NOT Apple output. The names follow the MIT
 * project vertex-language/air (intrinsic.go @3d277d16858a): air.<op>.<ty> for clz/ctz/popcount/reverse_bits, air.<op>.{s,u}.<ty> for min/max/clamp/abs/mul_hi/
 * add_sat/sub_sat/extract_bits/insert_bits. NONE of these names occurs in the device dumps of runs 231501 / 230109 (census X11 count: 0).
 *
 * Each case lists (a) GLSL.std.450 instructions that must appear N times, (b) core opcodes that must appear N times. Exit 77 when spirv-val is not installed.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air2spv/spv_builder.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int bad;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

/* SPIR-V core opcode numbers (spec), pinned against the builder's names */
_Static_assert(LAGFX_SPV_OP_UMUL_EXTENDED == 151 && LAGFX_SPV_OP_SMUL_EXTENDED == 152, "OpUMulExtended/OpSMulExtended");
_Static_assert(LAGFX_SPV_OP_BIT_FIELD_INSERT == 201 && LAGFX_SPV_OP_BIT_FIELD_S_EXTRACT == 202 && LAGFX_SPV_OP_BIT_FIELD_U_EXTRACT == 203, "OpBitField*");
_Static_assert(LAGFX_SPV_OP_BIT_REVERSE == 204 && LAGFX_SPV_OP_BIT_COUNT == 205, "OpBitReverse/OpBitCount");
_Static_assert(LAGFX_SPV_GLSL_FIND_ILSB == 73 && LAGFX_SPV_GLSL_FIND_UMSB == 75, "FindILsb/FindUMsb");

typedef struct { uint32_t glsl; int n; } want_glsl_t;
typedef struct { uint32_t op; int n; } want_op_t;
typedef struct {
    const char *fixture;
    const want_glsl_t *glsl;   /* terminated by n == 0 */
    const want_op_t *ops;      /* terminated by n == 0 */
} case_t;

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

static int count_op(const uint32_t *w, size_t nw, uint32_t opcode) {
    int n = 0;
    for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == opcode) n++; i += wc; }
    return n;
}
static int count_glsl(const uint32_t *w, size_t nw, uint32_t inst) {
    int n = 0;
    for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == 12u /* OpExtInst */ && wc >= 5u && w[i + 4] == inst) n++; i += wc; }
    return n;
}

/* 0 = valid, 1 = rejected, -1 = spirv-val not runnable */
static int spirv_val(const uint8_t *spv, size_t sz) {
    char tmpl[] = "/tmp/lagfx-int-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (write(fd, spv, sz) != (ssize_t)sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    pid_t pid = fork();
    if (pid == 0) { execlp("spirv-val", "spirv-val", tmpl, (char *)NULL); _exit(127); }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) { unlink(tmpl); return -1; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) == 127) { unlink(tmpl); return -1; }
    if (WEXITSTATUS(st) != 0) { char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val %s 2>&1 | head -5", tmpl); (void)system(cmd); }
    unlink(tmpl);
    return WEXITSTATUS(st) ? 1 : 0;
}

static const want_glsl_t g_minmax[] = { {39, 2}, {38, 2}, {42, 2}, {41, 2}, {45, 1}, {44, 2}, {5, 2}, {0, 0} };
static const want_op_t   o_none[]   = { {0, 0} };
static const want_glsl_t g_none[]   = { {0, 0} };

/* clz: i32 1 + i16 1 + i8 1 + i64 2 (halves) + v2i32 1 + v2i64 2 = 8 FindUMsb; ctz the same with FindILsb; popcount i32/i16/i64/v2i32 = 4; reverse_bits i32/i8/i64/v2i32 = 4 */
static const want_glsl_t g_bits[] = { {LAGFX_SPV_GLSL_FIND_UMSB, 8}, {LAGFX_SPV_GLSL_FIND_ILSB, 8}, {0, 0} };
static const want_op_t   o_bits[] = { {LAGFX_SPV_OP_BIT_COUNT, 4}, {LAGFX_SPV_OP_BIT_REVERSE, 4}, {0, 0} };

static const case_t cases[] = {
    { "int_bitcount_fragment.air.bc", g_bits, o_bits },
    /* the fixtures executed by the lavapipe test must at least be spirv-val clean */
    { "rt_bits32_fragment.air.bc", g_none, o_none },
    { "rt_bitsnarrow_fragment.air.bc", g_none, o_none },
    { "rt_bitsnarrow2_fragment.air.bc", g_none, o_none },
    { "rt_bits64_fragment.air.bc", g_none, o_none },
    { "rt_bitsvec_fragment.air.bc", g_none, o_none },
    { "int_minmax_fragment.air.bc", g_minmax, o_none },
};

int main(void) {
    int skipped = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const case_t *cs = &cases[k];
        size_t bn = 0; uint8_t *bc = slurp(cs->fixture, &bn);
        EXPECT(bc, "fixture %s not found", cs->fixture);
        if (!bc) continue;
        lagfx_air_module_t *m = NULL;
        uint8_t *spv = NULL; size_t sz = 0;
        EXPECT(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", cs->fixture);
        if (m && lagfx_air2spv_translate_module(m, &spv, &sz) != LAGFX_OK) spv = NULL;
        EXPECT(spv, "%s: translate", cs->fixture);
        if (spv) {
            const uint32_t *w = (const uint32_t *)(const void *)spv;
            size_t nw = sz / 4u;
            for (const want_glsl_t *g = cs->glsl; g->n; g++) {
                int got = count_glsl(w, nw, g->glsl);
                EXPECT(got == g->n, "%s: GLSL.std.450 inst %u appears %d times, want %d", cs->fixture, g->glsl, got, g->n);
            }
            for (const want_op_t *o = cs->ops; o->op || o->n; o++) {
                int got = count_op(w, nw, o->op);
                EXPECT(got == o->n, "%s: opcode %u appears %d times, want %d", cs->fixture, o->op, got, o->n);
            }
            if (getenv("LAGFX_INT_DUMP_DIR")) {   /* debugging aid: write the translated module for spirv-dis */
                char out[512]; snprintf(out, sizeof out, "%s/%s.spv", getenv("LAGFX_INT_DUMP_DIR"), cs->fixture);
                FILE *f = fopen(out, "wb"); if (f) { fwrite(spv, 1, sz, f); fclose(f); }
            }
            int v = spirv_val(spv, sz);
            if (v < 0) skipped = 1;
            else EXPECT(v == 0, "%s: spirv-val rejected the module", cs->fixture);
        }
        free(spv); if (m) lagfx_air_module_free(m); free(bc);
    }
    if (bad) { printf("FAIL air2spv-int-intrinsics-unit (%d)\n", bad); return 1; }
    if (skipped) { printf("SKIP: spirv-val not installed\n"); return 77; }
    printf("PASS air2spv-int-intrinsics-unit (%zu fixtures)\n", sizeof cases / sizeof cases[0]);
    return 0;
}
