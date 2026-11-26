/*
 * libapplegfx-vulkan — OpUndef usage report + fail-closed translation
 * tests/air2spv-undef-report-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Fixtures (hand-written IR assembled with llvm-as, see the .ll files; NOT Apple output):
 *   undefctl_fragment.air.bc  branches on an i1 returned by an intrinsic the translator does not know -> the call is replaced by OpUndef -> the (translated) control flow
 *                             depends on it. Must be REJECTED by default (LAGFX_ERR_PROTOCOL, no stub fallback), built with LAGFX_ALLOW_UNDEF_CONTROL=1, and the report
 *                             must show a control sink + a counter increment.
 *   i1const_fragment.air.bc   every operand is a resolved i1 constant: report must show 0 control sinks and the module must translate.
 */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

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

static lagfx_status_t xlate(const char *fixture, uint8_t **spv, size_t *sz) {
    size_t n = 0; uint8_t *bc = slurp(fixture, &n); if (!bc) return LAGFX_ERR_INVALID_ARG;
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(bc, n, &m) != LAGFX_OK || !m) return LAGFX_ERR_INVALID_ARG;
    return lagfx_air2spv_translate_module(m, spv, sz);
}

int main(void) {
    uint8_t *spv = NULL; size_t sz = 0; lagfx_undef_totals_t t0, t1;
    unsetenv("LAGFX_ALLOW_UNDEF_CONTROL");

    /* 1. clean module: no control sinks, counted as a module */
    lagfx_air2spv_undef_totals(&t0);
    CHECK(xlate("i1const_fragment.air.bc", &spv, &sz) == LAGFX_OK && spv, "i1const_fragment must translate");
    lagfx_spv_undef_report_t r; lagfx_spv_undef_analyze(spv, sz, &r);
    CHECK(r.reach_branch + r.reach_switch + r.reach_select_cond == 0, "i1const_fragment: %u/%u/%u control sinks", r.reach_branch, r.reach_switch, r.reach_select_cond);
    lagfx_air2spv_undef_totals(&t1);
    CHECK(t1.modules == t0.modules + 1 && t1.modules_ctl == t0.modules_ctl && t1.rejected == t0.rejected, "totals after a clean module");
    free(spv); spv = NULL;

    /* 2. undef-controlled branch: rejected by default */
    lagfx_air2spv_undef_totals(&t0);
    lagfx_status_t st = xlate("undefctl_fragment.air.bc", &spv, &sz);
    CHECK(st != LAGFX_OK && !spv, "undef-controlled control flow must be rejected (st=%d)", (int)st);
    lagfx_air2spv_undef_totals(&t1);
    CHECK(t1.rejected == t0.rejected + 1 && t1.modules_ctl == t0.modules_ctl + 1, "rejection counters (rejected %llu -> %llu)", (unsigned long long)t0.rejected, (unsigned long long)t1.rejected);

    /* 3. override: built, and the report names the control sink */
    setenv("LAGFX_ALLOW_UNDEF_CONTROL", "1", 1);
    st = xlate("undefctl_fragment.air.bc", &spv, &sz);
    unsetenv("LAGFX_ALLOW_UNDEF_CONTROL");
    CHECK(st == LAGFX_OK && spv, "override must build the module");
    lagfx_spv_undef_analyze(spv, sz, &r);
    CHECK(r.undef_defs > 0 && r.undef_used > 0 && r.reach_branch + r.reach_switch + r.reach_select_cond > 0, "report: defs=%u used=%u ctl=%u", r.undef_defs, r.undef_used, r.reach_branch + r.reach_switch + r.reach_select_cond);
    free(spv);
    printf("PASS: undef report (clean module 0 control sinks; undef-controlled module rejected, built with the override, %u control sink(s))\n", r.reach_branch + r.reach_switch + r.reach_select_cond);
    return 0;
}
