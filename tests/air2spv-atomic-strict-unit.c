/* AIR atomics: the STRICT operand grammar, the refusal reasons and LAGFX_ATOMIC_LENIENT. No Vulkan device needed.
 * SPDX-License-Identifier: MIT
 *
 * Only the operand combinations seen in real Apple modules are accepted [V, docs/spec-audit.md "Atomics"]:
 *     load(ptr, 0, 2, true)    store(ptr, v, 0, 2, true)    rmw(ptr, v, 0, 2, 0, true)
 * (memory order 0, scope 2, rmw extra 0, final i1 true). Every other literal in those positions, a different operand count, a non-constant operand, a float atomic and a 64-bit atomic
 * with a declared return value must FAIL the translation with a named reason (lagfx_air2spv_last_atomic_reason) and be counted per reason (lagfx_air2spv_atomic_totals), in the default
 * mode. LAGFX_ATOMIC_LENIENT=1 restores the earlier permissive grammar: the same fixtures translate, each counted as lenient_accepted. The memory-order operand and the
 * compare-exchange form guard are NOT relaxed by it. Fixtures are hand-written IR (tests/fixtures/gen-atomic-fixtures.sh); the [N] spellings (local, sub/xor/min/max, cmpxchg, i64, f32) are
 * accepted by grammar only with the observed operands.
 *
 * argv[1] == "env-lenient": run only the environment check (the meson test sets LAGFX_ATOMIC_LENIENT=1 for it).
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"

#include <stdint.h>
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

/* Translate `name` with lenient mode `mode` (-1 = environment, 0 / 1 forced). Returns the status; *reason / *nref = the refusal of this translation. */
static int run(const char *name, int mode, lagfx_status_t *st, unsigned *reason, uint32_t *nref) {
    size_t an = 0; uint8_t *air = slurp(name, &an);
    CHECK(air, "fixture %s not found", name);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK && m, "%s: module_open", name);
    uint8_t *spv = NULL; size_t sn = 0;
    lagfx_air2spv_set_atomic_lenient(mode);
    *st = lagfx_air2spv_translate_module(m, &spv, &sn);
    lagfx_air2spv_set_atomic_lenient(-1);
    *reason = lagfx_air2spv_last_atomic_reason();
    *nref = lagfx_air2spv_last_unsupported_atomic(NULL, 0);
    CHECK((*st == LAGFX_OK) == (spv != NULL), "%s: status %d vs blob %p", name, (int)*st, (void *)spv);
    free(spv); lagfx_air_module_free(m); free(air);
    return 0;
}

typedef struct {
    const char *fx;
    unsigned strict;    /* expected refusal reason in the default mode (LAGFX_ATOMIC_R_NONE = translates) */
    unsigned lenient;   /* expected result with lenient mode on */
} row_t;

#define OK LAGFX_ATOMIC_R_NONE
static const row_t rows[] = {
    /* ---- the observed combinations (and the [N] spellings carrying exactly the observed operands): accepted in both modes ---- */
    { "atomic_rmw.air.bc",              OK, OK },   /* load store xchg add.u/add.s and or (observed spellings) + sub xor min max [N spellings] */
    { "atomic_tg.air.bc",               OK, OK },   /* air.atomic.local.* [N] with scope 2 */
    { "atomic_cas.air.bc",              OK, OK },   /* cmpxchg.weak [N] */
    { "atomic_cas_wb.air.bc",           OK, OK },
    { "atomic_i64.air.bc",              OK, OK },   /* 64-bit min / max, void declared return */
    { "atomic_i64_tg.air.bc",           OK, OK },
    { "atomic_helper_tg.air.bc",        OK, OK },
    /* ---- each unobserved operand value in each position ---- */
    { "atomic_x_load_scope1.air.bc",    LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_load_flag0.air.bc",     LAGFX_ATOMIC_R_FLAG,          OK },
    { "atomic_x_store_scope1.air.bc",   LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_store_flag0.air.bc",    LAGFX_ATOMIC_R_FLAG,          OK },
    { "atomic_x_rmw_scope0.air.bc",     LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_rmw_scope1.air.bc",     LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_rmw_scope3.air.bc",     LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_rmw_scopevar.air.bc",   LAGFX_ATOMIC_R_SCOPE,         OK },   /* a runtime value in the scope position */
    { "atomic_x_rmw_extra1.air.bc",     LAGFX_ATOMIC_R_RMW_EXTRA,     OK },
    { "atomic_x_rmw_flag0.air.bc",      LAGFX_ATOMIC_R_FLAG,          OK },
    { "atomic_x_rmw_flagvar.air.bc",    LAGFX_ATOMIC_R_FLAG,          OK },   /* a runtime i1 in the flag position */
    { "atomic_x_rmw_argc5.air.bc",      LAGFX_ATOMIC_R_OPERAND_COUNT, LAGFX_ATOMIC_R_OPERAND_COUNT },   /* a missing operand is never tolerated */
    { "atomic_x_rmw_argc7.air.bc",      LAGFX_ATOMIC_R_OPERAND_COUNT, OK },                              /* an extra trailing operand was tolerated before */
    { "atomic_x_tg_scope1.air.bc",      LAGFX_ATOMIC_R_SCOPE,         OK },   /* threadgroup: only the observed scope value 2 */
    { "atomic_x_cas_scope1.air.bc",     LAGFX_ATOMIC_R_SCOPE,         OK },
    { "atomic_x_cas_flag0.air.bc",      LAGFX_ATOMIC_R_FLAG,          OK },
    /* ---- 64-bit return, float: unobserved ---- */
    { "atomic_x_i64_ret.air.bc",        LAGFX_ATOMIC_R_I64_RETURN,    OK },
    { "atomic_x_i64_load.air.bc",       LAGFX_ATOMIC_R_I64_RETURN,    OK },
    { "atomic_f32.air.bc",              LAGFX_ATOMIC_R_FLOAT,         OK },
    { "atomic_f32_tg.air.bc",           LAGFX_ATOMIC_R_FLOAT,         OK },
    /* ---- never relaxed: memory order, compare-exchange forms, unknown spellings, atomics in helpers that cannot be wired ---- */
    { "atomic_order.air.bc",            LAGFX_ATOMIC_R_ORDER,         LAGFX_ATOMIC_R_ORDER },
    { "atomic_x_cas_byval.air.bc",      LAGFX_ATOMIC_R_CAS_FORM,      LAGFX_ATOMIC_R_CAS_FORM },
    { "atomic_x_cas_i32ret.air.bc",     LAGFX_ATOMIC_R_CAS_FORM,      LAGFX_ATOMIC_R_CAS_FORM },
    { "atomic_x_cas_devptr.air.bc",     LAGFX_ATOMIC_R_CAS_FORM,      LAGFX_ATOMIC_R_CAS_FORM },
    { "atomic_unknown.air.bc",          LAGFX_ATOMIC_R_SPELLING,      LAGFX_ATOMIC_R_SPELLING },
    { "atomic_helper.air.bc",           LAGFX_ATOMIC_R_HELPER,        LAGFX_ATOMIC_R_HELPER },
};

int main(int argc, char **argv) {
    lagfx_atomic_totals_t t0, t1;
    if (argc > 1 && !strcmp(argv[1], "env-lenient")) {
        /* LAGFX_ATOMIC_LENIENT=1 in the environment (set by the meson test): the default mode (-1) behaves as lenient */
        lagfx_status_t st; unsigned why; uint32_t n;
        lagfx_air2spv_atomic_totals(&t0);
        CHECK(!run("atomic_x_rmw_scope1.air.bc", -1, &st, &why, &n) && st == LAGFX_OK, "LAGFX_ATOMIC_LENIENT=1 must accept the scope-1 rmw (st %d, reason %s)", (int)st, lagfx_air2spv_atomic_reason_name(why));
        CHECK(!run("atomic_f32.air.bc", -1, &st, &why, &n) && st == LAGFX_OK, "LAGFX_ATOMIC_LENIENT=1 must accept the float add");
        lagfx_air2spv_atomic_totals(&t1);
        CHECK(t1.lenient_accepted == t0.lenient_accepted + 2u, "lenient_accepted %llu -> %llu, expected +2", (unsigned long long)t0.lenient_accepted, (unsigned long long)t1.lenient_accepted);
        CHECK(!run("atomic_order.air.bc", -1, &st, &why, &n) && st != LAGFX_OK && why == LAGFX_ATOMIC_R_ORDER, "a non-relaxed order stays refused in lenient mode");
        printf("PASS: LAGFX_ATOMIC_LENIENT=1 in the environment restores the permissive grammar (scope-1 rmw, float add accepted, 2 counted); the memory order is still refused\n");
        return 0;
    }
    CHECK(!getenv("LAGFX_ATOMIC_LENIENT") || strcmp(getenv("LAGFX_ATOMIC_LENIENT"), "1"), "run this test without LAGFX_ATOMIC_LENIENT=1 (the default must be off)");
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const row_t *r = &rows[i];
        lagfx_status_t st; unsigned why; uint32_t n;
        /* default mode (the environment says off): strict */
        lagfx_air2spv_atomic_totals(&t0);
        CHECK(!run(r->fx, -1, &st, &why, &n), "%s: run", r->fx);
        lagfx_air2spv_atomic_totals(&t1);
        if (r->strict == OK) {
            CHECK(st == LAGFX_OK && why == OK && n == 0u, "%s: must translate in the default mode (st %d, reason %s)", r->fx, (int)st, lagfx_air2spv_atomic_reason_name(why));
            CHECK(t1.lenient_accepted == t0.lenient_accepted, "%s: strict translation counted a lenient acceptance", r->fx);
        } else {
            CHECK(st != LAGFX_OK && why == r->strict && n >= 1u, "%s: must be refused with reason '%s' in the default mode (st %d, reason '%s', %u call(s))", r->fx, lagfx_air2spv_atomic_reason_name(r->strict), (int)st,
                  lagfx_air2spv_atomic_reason_name(why), n);
            CHECK(t1.reason[r->strict] >= t0.reason[r->strict] + n, "%s: reason counter '%s' did not grow by %u", r->fx, lagfx_air2spv_atomic_reason_name(r->strict), n);
            CHECK(t1.modules_rejected == t0.modules_rejected + 1u && t1.refused >= t0.refused + n, "%s: module / call refusal counters", r->fx);
        }
        /* lenient mode */
        lagfx_air2spv_atomic_totals(&t0);
        CHECK(!run(r->fx, 1, &st, &why, &n), "%s: run (lenient)", r->fx);
        lagfx_air2spv_atomic_totals(&t1);
        if (r->lenient == OK) {
            CHECK(st == LAGFX_OK, "%s: lenient mode must accept it (st %d, reason '%s')", r->fx, (int)st, lagfx_air2spv_atomic_reason_name(why));
            if (r->strict != OK) CHECK(t1.lenient_accepted > t0.lenient_accepted, "%s: a call strict mode refuses was accepted without being counted (lenient_accepted)", r->fx);
        } else {
            CHECK(st != LAGFX_OK && why == r->lenient, "%s: must stay refused in lenient mode with reason '%s' (st %d, reason '%s')", r->fx, lagfx_air2spv_atomic_reason_name(r->lenient), (int)st, lagfx_air2spv_atomic_reason_name(why));
        }
        printf("PASS: %-32s strict: %-14s lenient: %s\n", r->fx, r->strict == OK ? "translates" : lagfx_air2spv_atomic_reason_name(r->strict), r->lenient == OK ? "translates" : lagfx_air2spv_atomic_reason_name(r->lenient));
    }
    /* the synthetic compare-exchange form is counted when translated (a real Apple compare-exchange has never been seen) */
    {
        lagfx_status_t st; unsigned why; uint32_t n;
        lagfx_air2spv_atomic_totals(&t0);
        CHECK(!run("atomic_cas.air.bc", -1, &st, &why, &n) && st == LAGFX_OK, "cas run");
        lagfx_air2spv_atomic_totals(&t1);
        CHECK(t1.cas_synthetic == t0.cas_synthetic + 1u, "cas_synthetic %llu -> %llu", (unsigned long long)t0.cas_synthetic, (unsigned long long)t1.cas_synthetic);
        printf("PASS: the synthetic compare-exchange form is counted (cas_synthetic +1); every other compare-exchange form is refused\n");
    }
    printf("PASS air2spv-atomic-strict-unit\n");
    return 0;
}
