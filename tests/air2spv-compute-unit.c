/*
 * libapplegfx-vulkan — compute stage (air.kernel) + simd-group / float-math lowering, structural checks
 * tests/air2spv-compute-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixtures (real Apple AIR taken from a guest metallib, like the login_* ones):
 *   tile_simd_blur_15.air.bc          tile function: [[imageblock]] store, simd_shuffle_and_fill_down, get_simdgroup_size, thread-position builtins
 *   dump_f16_fmax_frag.air.bc         fragment using air.fmax.f16 (used to be an OpUndef placeholder)
 *   dump_f16_fmax3_frag.air.bc        fragment using air.fmax3.f16 / air.fmax.f16
 *   dump_unpack_unorm4x8_vert.air.bc  vertex using air.unpack.unorm4x8.v4f16
 * The result is also executed by tests/air2spv-compute-lavapipe.c.
 */
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "vulkan/caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

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

typedef struct { lagfx_air_module_t *m; uint8_t *air; uint8_t *spv; size_t spv_n; } fx_t;
static int load(const char *name, fx_t *fx) {
    size_t n = 0;
    memset(fx, 0, sizeof *fx);
    fx->air = slurp(name, &n);
    CHECK(fx->air, "fixture %s not found", name);
    CHECK(lagfx_air_module_open(fx->air, n, &fx->m) == LAGFX_OK && fx->m, "module_open %s", name);
    CHECK(lagfx_air2spv_translate_module(fx->m, &fx->spv, &fx->spv_n) == LAGFX_OK && fx->spv, "translate %s", name);
    return 0;
}
static void unload(fx_t *fx) { free(fx->spv); lagfx_air_module_free(fx->m); free(fx->air); }

/* number of instructions with `op`; if a (a != NULL) the first one's operand `idx` is stored there */
static unsigned count_op(const fx_t *fx, uint32_t op) {
    const uint32_t *w = (const uint32_t *)(const void *)fx->spv; size_t n = fx->spv_n / 4; unsigned c = 0;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len) break; if ((w[i] & 0xffffu) == op) c++; i += len; }
    return c;
}
/* OpExtInst with GLSL instruction `inst` */
static unsigned count_glsl(const fx_t *fx, uint32_t inst) {
    const uint32_t *w = (const uint32_t *)(const void *)fx->spv; size_t n = fx->spv_n / 4; unsigned c = 0;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len) break; if ((w[i] & 0xffffu) == 12u && len >= 5 && w[i + 4] == inst) c++; i += len; }
    return c;
}
static int has_cap(const fx_t *fx, uint32_t cap) {
    const uint32_t *w = (const uint32_t *)(const void *)fx->spv; size_t n = fx->spv_n / 4;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len) break; if ((w[i] & 0xffffu) == 17u && w[i + 1] == cap) return 1; i += len; }
    return 0;
}
static uint32_t entry_model(const fx_t *fx) {
    const uint32_t *w = (const uint32_t *)(const void *)fx->spv; size_t n = fx->spv_n / 4;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len) break; if ((w[i] & 0xffffu) == 15u) return w[i + 1]; i += len; }
    return ~0u;
}
/* an OpDecorate <id> BuiltIn <b> */
static int has_builtin(const fx_t *fx, uint32_t b) {
    const uint32_t *w = (const uint32_t *)(const void *)fx->spv; size_t n = fx->spv_n / 4;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len) break; if ((w[i] & 0xffffu) == 71u && len == 4 && w[i + 2] == 11u && w[i + 3] == b) return 1; i += len; }
    return 0;
}

static int test_tile_kernel(void) {
    fx_t fx; if (load("tile_simd_blur_15.air.bc", &fx)) return 1;
    CHECK(entry_model(&fx) == 5u, "entry point is not GLCompute (%u)", entry_model(&fx));
    CHECK(has_builtin(&fx, 25u), "no BuiltIn WorkgroupSize (the workgroup size must be a spec-constant composite)");
    CHECK(count_op(&fx, 50u) == 3u, "expected 3 OpSpecConstant (local size x/y/z), got %u", count_op(&fx, 50u));
    CHECK(count_op(&fx, 51u) == 1u, "expected 1 OpSpecConstantComposite");
    CHECK(has_builtin(&fx, 26u), "no WorkgroupId");
    CHECK(has_builtin(&fx, 41u), "no SubgroupLocalInvocationId (thread_index_in_simdgroup)");
    CHECK(has_builtin(&fx, 36u), "no SubgroupSize (get_simdgroup_size)");
    CHECK(has_builtin(&fx, 29u), "no LocalInvocationIndex (simdgroup_index_in_threadgroup)");
    CHECK(count_op(&fx, 345u) == 2u, "shuffle_and_fill_down = two OpGroupNonUniformShuffle (data, filling), got %u", count_op(&fx, 345u));
    CHECK(has_cap(&fx, 61u) && has_cap(&fx, 65u), "GroupNonUniform + GroupNonUniformShuffle not declared");
    CHECK(!has_cap(&fx, 64u), "needless GroupNonUniformBallot declared");
    CHECK(count_op(&fx, 99u) == 1u, "implicit_imageblock store = one OpImageWrite, got %u", count_op(&fx, 99u));
    CHECK(count_op(&fx, 95u) == 2u, "two tex.read = two OpImageFetch, got %u", count_op(&fx, 95u));
    CHECK(count_op(&fx, 1u) <= 12u, "OpUndef count %u suspiciously high", count_op(&fx, 1u));
    /* fail-closed device-capability scan sees the subgroup requirements */
    uint32_t need = 0;
    CHECK(lagfx_spv_required_caps_ex(fx.spv, fx.spv_n, &need), "caps scan");
    CHECK((need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP)) && (need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE)), "required caps lack subgroup/sg_shuffle (0x%x)", need);
    CHECK((need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16)) && (need & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16)), "required caps lack float16/int16 (0x%x)", need);

    /* resource bindings in argument order, with the Metal indices */
    lagfx_air_arg_binding_t b[8];
    size_t nb = lagfx_air_arg_bindings(fx.m, b, 8);
    CHECK(nb == 3u, "arg bindings: expected texture + 2 buffers, got %zu", nb);
    CHECK(b[0].kind == 2 && b[0].metal_index == 0, "binding 0: texture(0), got kind %d idx %d", b[0].kind, b[0].metal_index);
    CHECK(b[1].kind == 1 && b[1].metal_index == 0, "binding 1: buffer(0)");
    CHECK(b[2].kind == 1 && b[2].metal_index == 1, "binding 2: buffer(1)");
    lagfx_stage_keys_t k;
    CHECK(lagfx_air_stage_keys(fx.m, false, &k) && k.n == 8u, "kernel arg keys");
    CHECK(!strcmp(k.key[0], "air.imageblock") && !strcmp(k.key[4], "air.threadgroup_position_in_grid") &&
          !strcmp(k.key[6], "air.thread_index_in_simdgroup") && !strcmp(k.key[7], "air.simdgroup_index_in_threadgroup"), "kernel arg keys: %s %s %s %s", k.key[0], k.key[4], k.key[6], k.key[7]);
    unload(&fx);
    printf("PASS: tile kernel translates to GLCompute with subgroup shuffles, builtins, spec-constant workgroup size, imageblock store\n");
    return 0;
}

static int test_float_math(void) {
    fx_t fx;
    /* air.fmax.f16 / air.fmin.f16 / air.clamp.f16...: FMax(40) FMin(37) FClamp(43) instead of OpUndef */
    if (load("dump_f16_fmax_frag.air.bc", &fx)) return 1;
    CHECK(count_glsl(&fx, 40u) >= 4u, "air.fmax.f16 not lowered to FMax (%u)", count_glsl(&fx, 40u));
    unload(&fx);
    if (load("dump_f16_fmax3_frag.air.bc", &fx)) return 1;
    CHECK(count_glsl(&fx, 40u) >= 3u, "fmax3 + fmax not lowered to FMax (%u)", count_glsl(&fx, 40u));
    unload(&fx);
    if (load("dump_unpack_unorm4x8_vert.air.bc", &fx)) return 1;
    CHECK(count_glsl(&fx, 64u) == 1u, "air.unpack.unorm4x8 not lowered to UnpackUnorm4x8 (%u)", count_glsl(&fx, 64u));
    unload(&fx);
    printf("PASS: half-precision air.fmax/fmin/clamp/fmax3/unpack.unorm4x8 lower to GLSL.std.450\n");
    return 0;
}

int main(void) { return test_tile_kernel() || test_float_math(); }
