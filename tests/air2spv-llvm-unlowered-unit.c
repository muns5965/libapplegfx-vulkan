/* Unlowered-call ledger and the named refusal of unlowered non-void llvm.* calls (integ7b).
 *  - rt_llvm_umaxctl: llvm.umax.i32 steers a select: lowered, no OpUndef reaches control flow (the video-encode-kernel symptom), module valid.
 *  - rt_llvm_sinctl (llvm.sin.f32, NOT lowered): default = refused by name (no reference-stub fallback); LAGFX_ALLOW_UNLOWERED_LLVM=1 = typed undef that DOES reach the select (negative control).
 *  - undefctl (air.no_such_intrinsic.f32, non-void air.*): default unchanged (typed undef, translation proceeds); LAGFX_STRICT_INTRINSICS=1 refuses it by name.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

static uint8_t *slurp(const char *name, size_t *n) {
    char p[512]; const char *d[] = { SRCDIR "/fixtures/", "tests/fixtures/" };
    for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s%s", d[i], name); FILE *f = fopen(p, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc((size_t)l);
        if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; } fclose(f); *n = (size_t)l; return b; }
    return NULL;
}
static lagfx_status_t tr(const char *fix, uint8_t **spv, size_t *sz) {
    size_t bn = 0; uint8_t *bc = slurp(fix, &bn); if (!bc) return LAGFX_ERR_INVALID_ARG;
    lagfx_air_module_t *m = NULL; if (lagfx_air_module_open(bc, bn, &m) != LAGFX_OK) { free(bc); return LAGFX_ERR_INVALID_ARG; }
    *spv = NULL; *sz = 0; lagfx_status_t s = lagfx_air2spv_translate_module(m, spv, sz);
    lagfx_air_module_free(m); free(bc); return s;
}
static uint64_t sites_of(const char *name) {
    lagfx_unlowered_call_t r[256]; size_t n = lagfx_air2spv_unlowered_calls(r, 256); uint64_t t = 0;
    for (size_t i = 0; i < n && i < 256; i++) if (!strcmp(r[i].name, name)) t += r[i].sites;
    return t;
}
int main(void) {
    uint8_t *spv; size_t sz; char nm[96];
    unsetenv("LAGFX_ALLOW_UNLOWERED_LLVM"); unsetenv("LAGFX_STRICT_INTRINSICS"); unsetenv("LAGFX_ALLOW_UNDEF_CONTROL");
    lagfx_air2spv_unlowered_reset();
    /* 1. umax lowered */
    CHECK(tr("rt_llvm_umaxctl_fragment.air.bc", &spv, &sz) == LAGFX_OK && spv, "umaxctl must translate by default");
    lagfx_spv_undef_report_t ur; lagfx_spv_undef_analyze(spv, sz, &ur);
    CHECK(ur.reach_branch + ur.reach_switch + ur.reach_select_cond == 0, "umaxctl: an undef reaches control flow");
    CHECK(sites_of("llvm.umax.i32") == 0 && lagfx_air2spv_last_unlowered(NULL, 0) == 0, "llvm.umax.i32 must not be in the unlowered ledger");
    free(spv);
    /* 2. sin: refused by name, no stub */
    CHECK(tr("rt_llvm_sinctl_fragment.air.bc", &spv, &sz) != LAGFX_OK && !spv, "sinctl must be refused by default");
    CHECK(lagfx_air2spv_last_unlowered_refused() && lagfx_air2spv_last_unlowered(nm, sizeof nm) == 1 && !strcmp(nm, "llvm.sin.f32"), "refusal must name llvm.sin.f32 (got '%s')", nm);
    CHECK(sites_of("llvm.sin.f32") == 1 && lagfx_air2spv_unlowered_refused() >= 1, "ledger row / refused counter");
    /* 3. opt-out restores the typed undef; it reaches the select (negative control of the analysis) */
    setenv("LAGFX_ALLOW_UNLOWERED_LLVM", "1", 1); setenv("LAGFX_ALLOW_UNDEF_CONTROL", "1", 1);
    CHECK(tr("rt_llvm_sinctl_fragment.air.bc", &spv, &sz) == LAGFX_OK && spv, "sinctl with the opt-out must translate");
    lagfx_spv_undef_analyze(spv, sz, &ur);
    CHECK(ur.reach_select_cond > 0, "negative control: the typed undef must reach the select condition");
    free(spv); unsetenv("LAGFX_ALLOW_UNLOWERED_LLVM"); unsetenv("LAGFX_ALLOW_UNDEF_CONTROL");
    /* 4. non-void air.*: default proceeds, strict refuses by name */
    lagfx_air2spv_unlowered_reset();
    tr("undefctl_fragment.air.bc", &spv, &sz); free(spv);   /* its own undef-control guard may refuse; the ledger is what matters */
    CHECK(sites_of("air.no_such_intrinsic.f32") == 1, "air.* call must be in the ledger");
    CHECK(!lagfx_air2spv_last_unlowered_refused(), "air.* is not refused by default");
    setenv("LAGFX_STRICT_INTRINSICS", "1", 1);
    CHECK(tr("undefctl_fragment.air.bc", &spv, &sz) != LAGFX_OK && lagfx_air2spv_last_unlowered_refused(), "strict mode must refuse");
    CHECK(lagfx_air2spv_last_unlowered(nm, sizeof nm) == 1 && !strcmp(nm, "air.no_such_intrinsic.f32"), "strict refusal names the call (got '%s')", nm);
    unsetenv("LAGFX_STRICT_INTRINSICS");
    printf("PASS air2spv-llvm-unlowered-unit\n");
    return 0;
}
