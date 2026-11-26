/* Audit of the AIR -> GLSL.std.450 intrinsic table (lagfx_air2spv_glsl_intrinsic_lookup).
 *
 * Every row is pinned as (callee prefix, official GLSL.std.450 number from Khronos SPIRV-Headers GLSL.std.450.h, operand count, 3-way flag), and each row must
 * be reachable: looking up its own concrete name must return that row, not an earlier row whose prefix is a prefix of it (air.fast.sin vs air.fast.sinh).
 * Column 5 records the provenance of the AIR name: 'V' = the name (without the type suffix) occurs in the device dumps of runs 231501 / 230109, 'U' = unverified
 * (no dump evidence; the source comment says the dotted air.fast.<op> / air.precise.<op> spellings are not what Apple emits). 'U' rows are kept, not deleted.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate_function.h"
#include <stdio.h>
#include <string.h>

static int bad;
static const struct { const char *prefix; uint32_t glsl; uint32_t nargs; int three_way; char prov; } rows[] = {
    {"air.fast_fmax3.", 40, 3, 1, 'V'},
    {"air.fast_fmin3.", 37, 3, 1, 'U'},
    {"air.precise_fmax3.", 40, 3, 1, 'U'},
    {"air.precise_fmin3.", 37, 3, 1, 'U'},
    {"air.fmax3.", 40, 3, 1, 'V'},
    {"air.fmin3.", 37, 3, 1, 'U'},
    {"air.fmax.", 40, 2, 0, 'V'},
    {"air.fmin.", 37, 2, 0, 'V'},
    {"air.clamp.", 43, 3, 0, 'V'},
    {"air.fabs.", 4, 1, 0, 'V'},
    {"air.sqrt.", 31, 1, 0, 'V'},
    {"air.rsqrt.", 32, 1, 0, 'U'},
    {"air.fract.", 10, 1, 0, 'V'},
    {"air.floor.", 8, 1, 0, 'U'},
    {"air.ceil.", 9, 1, 0, 'U'},
    {"air.trunc.", 3, 1, 0, 'U'},
    {"air.exp.", 27, 1, 0, 'U'},
    {"air.exp2.", 29, 1, 0, 'U'},
    {"air.log.", 28, 1, 0, 'U'},
    {"air.log2.", 30, 1, 0, 'U'},
    {"air.sin.", 13, 1, 0, 'U'},
    {"air.cos.", 14, 1, 0, 'U'},
    {"air.powr.", 26, 2, 0, 'V'},
    {"air.fast_powr.", 26, 2, 0, 'V'},
    {"air.precise_powr.", 26, 2, 0, 'U'},
    {"air.fast.sqrt", 31, 1, 0, 'U'},
    {"air.fast.rsqrt", 32, 1, 0, 'U'},
    {"air.fast.exp", 27, 1, 0, 'U'},
    {"air.fast.log", 28, 1, 0, 'U'},
    {"air.fast.sin", 13, 1, 0, 'U'},
    {"air.fast.cos", 14, 1, 0, 'U'},
    {"air.fast.normalize", 69, 1, 0, 'U'},
    {"air.fast.length", 66, 1, 0, 'U'},
    {"air.fast.fabs", 4, 1, 0, 'U'},
    {"air.fast.floor", 8, 1, 0, 'U'},
    {"air.fast.ceil", 9, 1, 0, 'U'},
    {"air.fast.round", 1, 1, 0, 'U'},
    {"air.fast.fract", 10, 1, 0, 'U'},
    {"air.precise.sqrt", 31, 1, 0, 'U'},
    {"air.precise.rsqrt", 32, 1, 0, 'U'},
    {"air.precise.exp", 27, 1, 0, 'U'},
    {"air.precise.log", 28, 1, 0, 'U'},
    {"air.precise.sin", 13, 1, 0, 'U'},
    {"air.precise.cos", 14, 1, 0, 'U'},
    {"air.precise.normalize", 69, 1, 0, 'U'},
    {"air.precise.length", 66, 1, 0, 'U'},
    {"air.fast.fmin", 37, 2, 0, 'U'},
    {"air.fast.fmax", 40, 2, 0, 'U'},
    {"air.fast.pow", 26, 2, 0, 'U'},
    {"air.fast.distance", 67, 2, 0, 'U'},
    {"air.fast.reflect", 71, 2, 0, 'U'},
    {"air.fast.cross", 68, 2, 0, 'U'},
    {"air.fast.fclamp", 43, 3, 0, 'U'},
    {"air.fast.fmix", 46, 3, 0, 'U'},
    {"air.fast.step", 48, 2, 0, 'U'},
    {"air.fast.smoothstep", 49, 3, 0, 'U'},
    {"air.precise.fmin", 37, 2, 0, 'U'},
    {"air.precise.fmax", 40, 2, 0, 'U'},
    {"air.precise.pow", 26, 2, 0, 'U'},
    {"air.precise.distance", 67, 2, 0, 'U'},
    {"air.precise.reflect", 71, 2, 0, 'U'},
    {"air.precise.cross", 68, 2, 0, 'U'},
    {"air.precise.fclamp", 43, 3, 0, 'U'},
    {"air.precise.fmix", 46, 3, 0, 'U'},
    {"air.precise.step", 48, 2, 0, 'U'},
    {"air.precise.smoothstep", 49, 3, 0, 'U'},
    {"air.fast.tan", 15, 1, 0, 'U'},
    {"air.fast.asin", 16, 1, 0, 'U'},
    {"air.fast.acos", 17, 1, 0, 'U'},
    {"air.fast.atan", 18, 1, 0, 'U'},
    {"air.fast.sinh", 19, 1, 0, 'U'},
    {"air.fast.cosh", 20, 1, 0, 'U'},
    {"air.fast.tanh", 21, 1, 0, 'U'},
    {"air.fast.exp2", 29, 1, 0, 'U'},
    {"air.fast.log2", 30, 1, 0, 'U'},
    {"air.fast.trunc", 3, 1, 0, 'U'},
    {"air.fast.sign", 6, 1, 0, 'U'},
    {"air.fast.radians", 11, 1, 0, 'U'},
    {"air.fast.degrees", 12, 1, 0, 'U'},
    {"air.fast.atan2", 25, 2, 0, 'U'},
    {"air.fast_atan2", 25, 2, 0, 'V'},
    {"air.fast.ldexp", 53, 2, 0, 'U'},
    {"air.fast.refract", 72, 3, 0, 'U'},
    {"air.precise.tan", 15, 1, 0, 'U'},
    {"air.precise.asin", 16, 1, 0, 'U'},
    {"air.precise.acos", 17, 1, 0, 'U'},
    {"air.precise.atan", 18, 1, 0, 'U'},
    {"air.precise.sinh", 19, 1, 0, 'U'},
    {"air.precise.cosh", 20, 1, 0, 'U'},
    {"air.precise.tanh", 21, 1, 0, 'U'},
    {"air.precise.exp2", 29, 1, 0, 'U'},
    {"air.precise.log2", 30, 1, 0, 'U'},
    {"air.precise.trunc", 3, 1, 0, 'U'},
    {"air.precise.sign", 6, 1, 0, 'U'},
    {"air.precise.radians", 11, 1, 0, 'U'},
    {"air.precise.degrees", 12, 1, 0, 'U'},
    {"air.precise.atan2", 25, 2, 0, 'U'},
    {"air.precise.ldexp", 53, 2, 0, 'U'},
    {"air.precise.refract", 72, 3, 0, 'U'},
    {"air.mix", 46, 3, 0, 'V'},
    {"air.fast_clamp", 43, 3, 0, 'V'},
    {"air.precise_clamp", 43, 3, 0, 'U'},
    {"air.fast_pow", 26, 2, 0, 'V'},
    {"air.precise_pow", 26, 2, 0, 'U'},
    {"air.fast_rsqrt", 32, 1, 0, 'V'},
    {"air.precise_rsqrt", 32, 1, 0, 'U'},
    {"air.fast_sqrt", 31, 1, 0, 'V'},
    {"air.precise_sqrt", 31, 1, 0, 'U'},
    {"air.fast_fmin", 37, 2, 0, 'V'},
    {"air.fast_fmax", 40, 2, 0, 'V'},
    {"air.fast_floor", 8, 1, 0, 'V'},
    {"air.fast_ceil", 9, 1, 0, 'V'},
    {"air.fast_fract", 10, 1, 0, 'V'},
    {"air.fast_normalize", 69, 1, 0, 'U'},
    {"air.fast_length", 66, 1, 0, 'U'},
    {"air.fast_fabs", 4, 1, 0, 'V'},
    {"air.fast_log2", 30, 1, 0, 'V'},
    {"air.fast_exp2", 29, 1, 0, 'U'},
    {"air.fast_log", 28, 1, 0, 'V'},
    {"air.fast_exp", 27, 1, 0, 'V'},
    {"air.fma.", 50, 3, 0, 'V'},
    {"air.fast_fma", 50, 3, 0, 'U'},
    {"air.sign.", 6, 1, 0, 'V'},
    {"air.fast_sign", 6, 1, 0, 'U'},
    {"air.fast_sin.", 13, 1, 0, 'U'},
    {"air.precise_sin.", 13, 1, 0, 'U'},
    {"air.fast_cos.", 14, 1, 0, 'U'},
    {"air.precise_cos.", 14, 1, 0, 'U'},
    {"air.fast_tan.", 15, 1, 0, 'U'},
    {"air.precise_tan.", 15, 1, 0, 'U'},
    {"air.tan.", 15, 1, 0, 'U'},
    {"air.fast_asin.", 16, 1, 0, 'U'},
    {"air.precise_asin.", 16, 1, 0, 'U'},
    {"air.asin.", 16, 1, 0, 'U'},
    {"air.fast_acos.", 17, 1, 0, 'U'},
    {"air.precise_acos.", 17, 1, 0, 'U'},
    {"air.acos.", 17, 1, 0, 'U'},
    {"air.fast_atan.", 18, 1, 0, 'U'},
    {"air.precise_atan.", 18, 1, 0, 'U'},
    {"air.atan.", 18, 1, 0, 'U'},
    {"air.fast_sinh.", 19, 1, 0, 'U'},
    {"air.precise_sinh.", 19, 1, 0, 'U'},
    {"air.sinh.", 19, 1, 0, 'U'},
    {"air.fast_cosh.", 20, 1, 0, 'U'},
    {"air.precise_cosh.", 20, 1, 0, 'U'},
    {"air.cosh.", 20, 1, 0, 'U'},
    {"air.fast_tanh.", 21, 1, 0, 'U'},
    {"air.precise_tanh.", 21, 1, 0, 'U'},
    {"air.tanh.", 21, 1, 0, 'U'},
    {"air.fast_trunc.", 3, 1, 0, 'U'},
    {"air.precise_trunc.", 3, 1, 0, 'U'},
    {"air.fast_round.", 1, 1, 0, 'U'},
    {"air.precise_round.", 1, 1, 0, 'U'},
    {"air.round.", 1, 1, 0, 'U'},
    {"air.fast_step.", 48, 2, 0, 'U'},
    {"air.precise_step.", 48, 2, 0, 'U'},
    {"air.step.", 48, 2, 0, 'U'},
    {"air.fast_smoothstep.", 49, 3, 0, 'U'},
    {"air.precise_smoothstep.", 49, 3, 0, 'U'},
    {"air.smoothstep.", 49, 3, 0, 'U'},
    {"air.fast_distance.", 67, 2, 0, 'U'},
    {"air.precise_distance.", 67, 2, 0, 'U'},
    {"air.distance.", 67, 2, 0, 'U'},
    {"air.fast_cross.", 68, 2, 0, 'U'},
    {"air.precise_cross.", 68, 2, 0, 'U'},
    {"air.cross.", 68, 2, 0, 'U'},
    {"air.fast_reflect.", 71, 2, 0, 'U'},
    {"air.precise_reflect.", 71, 2, 0, 'U'},
    {"air.reflect.", 71, 2, 0, 'U'},
    {"air.fast_refract.", 72, 3, 0, 'U'},
    {"air.precise_refract.", 72, 3, 0, 'U'},
    {"air.refract.", 72, 3, 0, 'U'},
    {"air.fast_radians.", 11, 1, 0, 'U'},
    {"air.precise_radians.", 11, 1, 0, 'U'},
    {"air.radians.", 11, 1, 0, 'U'},
    {"air.fast_degrees.", 12, 1, 0, 'U'},
    {"air.precise_degrees.", 12, 1, 0, 'U'},
    {"air.degrees.", 12, 1, 0, 'U'},
    {"air.fast_ldexp.", 53, 2, 0, 'U'},
    {"air.precise_ldexp.", 53, 2, 0, 'U'},
    {"air.ldexp.", 53, 2, 0, 'U'},
};

static void check(const char *name, int want_hit, uint32_t glsl, uint32_t nargs, int three_way) {
    uint32_t g = 0, n = 0; bool t = false;
    bool hit = lagfx_air2spv_glsl_intrinsic_lookup(name, &g, &n, &t);
    if (hit != (want_hit != 0)) { printf("FAIL: %s hit=%d want %d\n", name, hit, want_hit); bad++; return; }
    if (!hit) return;
    if (g != glsl || n != nargs || t != (three_way != 0)) {
        printf("FAIL: %s -> inst %u nargs %u 3way %d, want inst %u nargs %u 3way %d\n", name, g, n, t, glsl, nargs, three_way);
        bad++;
    }
}

int main(void) {
    int nreal = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char name[96];
        size_t pl = strlen(rows[i].prefix);
        /* rows ending in '.' take the type suffix directly; the others are bare stems followed by ".<type>" */
        snprintf(name, sizeof(name), "%s%sf32", rows[i].prefix, rows[i].prefix[pl - 1] == '.' ? "" : ".");
        check(name, 1, rows[i].glsl, rows[i].nargs, rows[i].three_way);
        snprintf(name, sizeof(name), "%s%sv3f32", rows[i].prefix, rows[i].prefix[pl - 1] == '.' ? "" : ".");
        check(name, 1, rows[i].glsl, rows[i].nargs, rows[i].three_way);
        nreal += rows[i].prov == 'V';
    }
    /* Integer forms: air.<op>.s.<ty> / air.<op>.u.<ty> (sign marker before the type; MIT project vertex-language/air intrinsic.go @3d277d16858a; 'U' = not in the
     * device dumps). The unsigned forms must not fall into the float rows ("air.clamp." has IXF_FLOATSFX) nor into each other. Official GLSL.std.450 numbers:
     * UMin 38, SMin 39, UMax 41, SMax 42, UClamp 44, SClamp 45, SAbs 5. */
    {
        static const struct { const char *stem; uint32_t glsl; uint32_t nargs; } irows[] = {
            {"air.min.s.", 39, 2}, {"air.min.u.", 38, 2}, {"air.max.s.", 42, 2}, {"air.max.u.", 41, 2},
            {"air.clamp.s.", 45, 3}, {"air.clamp.u.", 44, 3}, {"air.abs.s.", 5, 1},
        };
        static const char *const tys[] = { "i8", "i16", "i32", "v2i32", "v3i16", "v4i8" };
        for (size_t i = 0; i < sizeof irows / sizeof irows[0]; i++)
            for (size_t t = 0; t < sizeof tys / sizeof tys[0]; t++) {
                char name[64];
                snprintf(name, sizeof name, "%s%s", irows[i].stem, tys[t]);
                check(name, 1, irows[i].glsl, irows[i].nargs, 0);
            }
    }
    /* The other integer intrinsics (clz, ctz, popcount, reverse_bits, mul_hi, add_sat, sub_sat, extract_bits, insert_bits, abs.u) are not GLSL.std.450 rows: they are
     * lowered by their own code (tests/air2spv-int-intrinsics-unit.c), so the table must not claim them. */
    {
        static const char *const own[] = { "air.clz.i32", "air.ctz.v2i32", "air.popcount.i16", "air.reverse_bits.i8", "air.mul_hi.s.i32", "air.mul_hi.u.i64", "air.add_sat.s.i8",
            "air.add_sat.u.v2i32", "air.sub_sat.s.i16", "air.sub_sat.u.i32", "air.extract_bits.s.i32", "air.insert_bits.u.v4i32", "air.abs.u.i32" };
        for (size_t i = 0; i < sizeof own / sizeof own[0]; i++) check(own[i], 0, 0, 0, 0);
    }
    /* Not integer-min/max names: no sign marker (ambiguous) or float suffix after a sign marker. */
    check("air.min.i32", 0, 0, 0, 0);
    check("air.max.i32", 0, 0, 0, 0);
    check("air.abs.i32", 0, 0, 0, 0);
    check("air.clamp.i32", 0, 0, 0, 0);
    check("air.fabs.i32", 0, 0, 0, 0);
    check("air.step.i32", 0, 0, 0, 0);
    check("air.sin.i32", 0, 0, 0, 0);
    /* Non-math AIR names never match. */
    check("air.sample_texture_2d.v4f32", 0, 0, 0, 0);
    check("air.dot.v3f32", 0, 0, 0, 0);
    check("air.convert.f.f32.u.i32", 0, 0, 0, 0);
    check("air.fast_saturate.f32", 0, 0, 0, 0);
    /* Names seen verbatim in the device dumps, with their expected numbers. */
    check("air.fast_atan2.f32", 1, 25, 2, 0);
    check("air.fast_fmax3.f32", 1, 40, 3, 1);
    check("air.fmax3.f16", 1, 40, 3, 1);
    check("air.fast_log2.f32", 1, 30, 1, 0);
    check("air.fast_exp.f32", 1, 27, 1, 0);
    check("air.fast_pow.f32", 1, 26, 2, 0);
    check("air.fast_powr.v4f32", 1, 26, 2, 0);
    check("air.powr.f16", 1, 26, 2, 0);
    check("air.fma.v4f16", 1, 50, 3, 0);
    check("air.mix.v4f16", 1, 46, 3, 0);
    check("air.sign.v4f32", 1, 6, 1, 0);
    check("air.fract.v3f16", 1, 10, 1, 0);
    check("air.sqrt.v3f16", 1, 31, 1, 0);
    check("air.clamp.v3f16", 1, 43, 3, 0);
    check("air.fabs.v2f16", 1, 4, 1, 0);
    printf("%s air2spv-glsl-intrinsic-table-unit (%zu rows, %d with dump evidence)\n", bad ? "FAIL" : "PASS", sizeof(rows) / sizeof(rows[0]), nreal);
    return bad ? 1 : 0;
}
