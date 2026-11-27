/* air.sample_texture_{2d,3d} option operands (census X04), translated and checked at the SPIR-V level.
 *
 * Operand order (verified against the 317 sample calls of the device dumps of runs 231501 / 230109, and the declared signature
 * `{<4 x T>, i8} (tex, samp, <N x float> coord, i1, <N x i32>, i1, float, float, i32)`):
 *   (tex, samp, coord, [i32 array index,] i1 has_offset, <N x i32> offset, i1 lod_is_level, float lod_or_bias, float min_lod, i32 flags)
 * Expected lowering, per call, in call order:
 *   plain (level=false, bias 0.0)      -> OpImageSampleImplicitLod, no image operands (vertex stage: ExplicitLod, Lod 0.0)
 *   bias (level=false, bias != 0/var)  -> OpImageSampleImplicitLod Bias (vertex stage: bias ignored, Lod 0.0)
 *   explicit level (level=true)        -> OpImageSampleExplicitLod Lod (value = level; vertex stage too)
 *   constant offset in [-8, 7]         -> ConstOffset (OpConstantComposite of 2 ints; 3 ints for a Dim3D sample, z kept)
 *   non-constant / out-of-range offset -> ignored (logged), no operand
 *   min_lod                            -> ignored (logged); the MinLod capability / shaderResourceMinLod is never enabled
 * half-typed lod/bias operands are converted to float32 (OpFConvert). Fixtures tests/fixtures/tex_*.ll are HAND-WRITTEN IR (not Apple output) assembled with
 * llvm-as (the AIR reader accepts LLVM's opaque-pointer type record, see src/air/bitcode_reader.c). Exit 77 when spirv-val is not installed.
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
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

#define OP_SAMPLE_IMPLICIT 87u
#define OP_SAMPLE_EXPLICIT 88u
#define MASK_BIAS 0x1u
#define MASK_LOD 0x2u
#define MASK_CONSTOFFSET 0x8u

typedef enum { LOD_NONE, LOD_CONST, LOD_VAR } lodk_t;
typedef struct { uint32_t op, mask; lodk_t lodk; float lod; int has_off; int ox, oy, oz, nl; } want_t;   /* nl = ConstOffset lanes: 2 (Dim2D) or 3 (Dim3D) */
typedef struct { const char *fixture; const want_t *w; size_t n; } case_t;

#define PLAIN            { OP_SAMPLE_IMPLICIT, 0, LOD_NONE, 0.0f, 0, 0, 0, 0, 0 }
#define BIAS_C(v)        { OP_SAMPLE_IMPLICIT, MASK_BIAS, LOD_CONST, (v), 0, 0, 0, 0, 0 }
#define BIAS_V           { OP_SAMPLE_IMPLICIT, MASK_BIAS, LOD_VAR, 0.0f, 0, 0, 0, 0, 0 }
#define LEVEL_C(v)       { OP_SAMPLE_EXPLICIT, MASK_LOD, LOD_CONST, (v), 0, 0, 0, 0, 0 }
#define LEVEL_V          { OP_SAMPLE_EXPLICIT, MASK_LOD, LOD_VAR, 0.0f, 0, 0, 0, 0, 0 }
#define OFF(x, y)        { OP_SAMPLE_IMPLICIT, MASK_CONSTOFFSET, LOD_NONE, 0.0f, 1, (x), (y), 0, 2 }
#define BIAS_V_OFF(x, y) { OP_SAMPLE_IMPLICIT, MASK_BIAS | MASK_CONSTOFFSET, LOD_VAR, 0.0f, 1, (x), (y), 0, 2 }
#define BIAS_C_OFF(v, x, y) { OP_SAMPLE_IMPLICIT, MASK_BIAS | MASK_CONSTOFFSET, LOD_CONST, (v), 1, (x), (y), 0, 2 }
#define BIAS_V_OFF3(x, y, z) { OP_SAMPLE_IMPLICIT, MASK_BIAS | MASK_CONSTOFFSET, LOD_VAR, 0.0f, 1, (x), (y), (z), 3 }
#define LEVEL_C_OFF3(v, x, y, z) { OP_SAMPLE_EXPLICIT, MASK_LOD | MASK_CONSTOFFSET, LOD_CONST, (v), 1, (x), (y), (z), 3 }
#define LEVEL_C_OFF(v, x, y) { OP_SAMPLE_EXPLICIT, MASK_LOD | MASK_CONSTOFFSET, LOD_CONST, (v), 1, (x), (y), 0, 2 }

/* tex_lod_fragment: plain / bias var / bias 0.5 / level 1 / level var / level 0 / has_offset=false / min_lod 0.25 (ignored) / half bias var / half bias 2.0 / half level var */
static const want_t w_lod[] = { PLAIN, BIAS_V, BIAS_C(0.5f), LEVEL_C(1.0f), LEVEL_V, LEVEL_C(0.0f), PLAIN, PLAIN, BIAS_V, BIAS_C(2.0f), LEVEL_V };
/* tex_lod_vertex: plain (Lod 0) / level 1 / level var / bias var (ignored: Lod 0) / bias 0.5 (ignored: Lod 0) */
static const want_t w_vlod[] = { LEVEL_C(0.0f), LEVEL_C(1.0f), LEVEL_V, LEVEL_C(0.0f), LEVEL_C(0.0f) };
/* tex_offset_fragment: offset (1,-1) / bias var + (1,2) / level 1 + (-2,0) / (7,-8) / (9,0) out of range / non-constant offset / half-result bias 0.25 + (0,-3) */
static const want_t w_off[] = { OFF(1, -1), BIAS_V_OFF(1, 2), LEVEL_C_OFF(1.0f, -2, 0), OFF(7, -8), PLAIN, PLAIN, BIAS_C_OFF(0.25f, 0, -3) };
/* tex_offset3d_fragment: plain 3d / bias var + (1,2,3) / level 1 + (-1,1,0): a Dim3D sample takes a 3-lane ConstOffset (z kept) */
static const want_t w_off3[] = { PLAIN, BIAS_V_OFF3(1, 2, 3), LEVEL_C_OFF3(1.0f, -1, 1, 0) };

static const case_t cases[] = {
    { "tex_lod_fragment.air.bc", w_lod, sizeof w_lod / sizeof w_lod[0] },
    { "tex_lod_vertex.air.bc", w_vlod, sizeof w_vlod / sizeof w_vlod[0] },
    { "tex_offset_fragment.air.bc", w_off, sizeof w_off / sizeof w_off[0] },
    { "tex_offset3d_fragment.air.bc", w_off3, sizeof w_off3 / sizeof w_off3[0] },
};

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

/* 0 = valid, 1 = rejected, -1 = spirv-val not runnable */
static int spirv_val(const uint8_t *spv, size_t sz) {
    char tmpl[] = "/tmp/lagfx-tso-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (write(fd, spv, sz) != (ssize_t)sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    pid_t pid = fork();
    if (pid == 0) { execlp("spirv-val", "spirv-val", "--target-env", "vulkan1.3", tmpl, (char *)NULL); _exit(127); }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) { unlink(tmpl); return -1; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) == 127) { unlink(tmpl); return -1; }
    unlink(tmpl);
    return WEXITSTATUS(st) ? 1 : 0;
}

/* minimal SPIR-V constant tracker: id -> OpConstant literal / OpConstantComposite lanes; float type widths */
#define MAXID 4096u
typedef struct { uint8_t kind; uint32_t type, val, lanes[4], n; } kc_t;   /* kind 1 = OpConstant, 2 = composite */
static kc_t g_k[MAXID];
static uint8_t g_float32[MAXID];

static void scan(const uint32_t *w, size_t nw, const want_t *exp, size_t nexp, const char *fixture) {
    memset(g_k, 0, sizeof g_k); memset(g_float32, 0, sizeof g_float32);
    size_t seen = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc) break;
        if (op == 17u) {   /* OpCapability: nothing that needs a device feature we never enable may be declared by the sampling lowering */
            EXPECT(w[i + 1] != 42u /* MinLod */ && w[i + 1] != 50u /* ImageQuery */ && w[i + 1] != 25u /* ImageGatherExtended */, "%s: capability %u must not be declared", fixture, w[i + 1]);
        } else if (op == 22u && wc >= 3u && w[i + 2] == 32u && w[i + 1] < MAXID) g_float32[w[i + 1]] = 1;   /* OpTypeFloat 32 */
        else if (op == 43u && wc >= 4u && w[i + 2] < MAXID) { g_k[w[i + 2]].kind = 1; g_k[w[i + 2]].type = w[i + 1]; g_k[w[i + 2]].val = w[i + 3]; }
        else if (op == 44u && wc >= 3u && w[i + 2] < MAXID) {
            kc_t *k = &g_k[w[i + 2]]; k->kind = 2; k->type = w[i + 1]; k->n = wc - 3u > 4u ? 4u : wc - 3u;
            for (uint32_t j = 0; j < k->n; j++) k->lanes[j] = w[i + 3 + j];
        } else if (op == OP_SAMPLE_IMPLICIT || op == OP_SAMPLE_EXPLICIT) {
            if (seen < nexp) {
                const want_t *e = &exp[seen];
                uint32_t mask = wc > 5u ? w[i + 5] : 0u;
                EXPECT(op == e->op, "%s: sample #%zu opcode %u, want %u", fixture, seen, op, e->op);
                EXPECT(mask == e->mask, "%s: sample #%zu image operands 0x%x, want 0x%x", fixture, seen, mask, e->mask);
                uint32_t pos = 6u;
                if (mask & (MASK_BIAS | MASK_LOD)) {
                    uint32_t id = wc > pos ? w[i + pos] : 0u;
                    pos++;
                    int isc = id < MAXID && g_k[id].kind == 1 && g_k[id].type < MAXID && g_float32[g_k[id].type];
                    EXPECT((e->lodk == LOD_CONST) == isc, "%s: sample #%zu lod/bias operand constness (const=%d, want %d)", fixture, seen, isc, e->lodk == LOD_CONST);
                    if (isc && e->lodk == LOD_CONST) { float f; memcpy(&f, &g_k[id].val, sizeof f); EXPECT(f == e->lod, "%s: sample #%zu lod/bias %g, want %g", fixture, seen, (double)f, (double)e->lod); }
                }
                if (mask & MASK_CONSTOFFSET) {
                    uint32_t id = wc > pos ? w[i + pos] : 0u;
                    int ok = id < MAXID && g_k[id].kind == 2 && (int)g_k[id].n == e->nl;
                    EXPECT(ok, "%s: sample #%zu ConstOffset is not a %d-lane OpConstantComposite", fixture, seen, e->nl);
                    if (ok) {
                        int32_t ox = (int32_t)g_k[g_k[id].lanes[0]].val, oy = (int32_t)g_k[g_k[id].lanes[1]].val, oz = e->nl == 3 ? (int32_t)g_k[g_k[id].lanes[2]].val : 0;
                        EXPECT(ox == e->ox && oy == e->oy && oz == e->oz, "%s: sample #%zu offset (%d,%d,%d), want (%d,%d,%d)", fixture, seen, ox, oy, oz, e->ox, e->oy, e->oz);
                    }
                }
            }
            seen++;
        }
        i += wc;
    }
    EXPECT(seen == nexp, "%s: %zu OpImageSample* instructions, want %zu", fixture, seen, nexp);
}

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
            scan((const uint32_t *)(const void *)spv, sz / 4u, cs->w, cs->n, cs->fixture);
            int v = spirv_val(spv, sz);
            if (v < 0) skipped = 1;
            else EXPECT(v == 0, "%s: spirv-val (vulkan1.3) rejected the module", cs->fixture);
        }
        free(spv); if (m) lagfx_air_module_free(m); free(bc);
    }
    if (bad) { printf("FAIL air2spv-texture-sample-options-unit (%d)\n", bad); return 1; }
    if (skipped) { printf("SKIP: spirv-val not installed\n"); return 77; }
    printf("PASS air2spv-texture-sample-options-unit (%zu fixtures)\n", sizeof cases / sizeof cases[0]);
    return 0;
}
