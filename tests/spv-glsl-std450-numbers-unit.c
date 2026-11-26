/* GLSL.std.450 extended-instruction numbers used by the AIR -> SPIR-V translator, pinned against the official
 * numbering in Khronos SPIRV-Headers (include/spirv/unified1/GLSL.std.450.h, GLSLstd450* enumerators).
 * SPDX-License-Identifier: MIT */
#include "air2spv/spv_builder.h"
#include <stdio.h>

static int bad;
#define PIN(sym, official, name) do { if ((int)(sym) != (official)) { printf("FAIL: %s = %d, GLSL.std.450.h %s = %d\n", #sym, (int)(sym), name, (official)); bad++; } } while (0)

int main(void) {
    PIN(LAGFX_SPV_GLSL_ROUND, 1, "GLSLstd450Round");
    PIN(LAGFX_SPV_GLSL_TRUNC, 3, "GLSLstd450Trunc");
    PIN(LAGFX_SPV_GLSL_FABS, 4, "GLSLstd450FAbs");
    PIN(LAGFX_SPV_GLSL_FSIGN, 6, "GLSLstd450FSign");
    PIN(LAGFX_SPV_GLSL_FLOOR, 8, "GLSLstd450Floor");
    PIN(LAGFX_SPV_GLSL_CEIL, 9, "GLSLstd450Ceil");
    PIN(LAGFX_SPV_GLSL_FRACT, 10, "GLSLstd450Fract");
    PIN(LAGFX_SPV_GLSL_RADIANS, 11, "GLSLstd450Radians");
    PIN(LAGFX_SPV_GLSL_DEGREES, 12, "GLSLstd450Degrees");
    PIN(LAGFX_SPV_GLSL_SIN, 13, "GLSLstd450Sin");
    PIN(LAGFX_SPV_GLSL_COS, 14, "GLSLstd450Cos");
    PIN(LAGFX_SPV_GLSL_TAN, 15, "GLSLstd450Tan");
    PIN(LAGFX_SPV_GLSL_ASIN, 16, "GLSLstd450Asin");
    PIN(LAGFX_SPV_GLSL_ACOS, 17, "GLSLstd450Acos");
    PIN(LAGFX_SPV_GLSL_ATAN, 18, "GLSLstd450Atan");
    PIN(LAGFX_SPV_GLSL_SINH, 19, "GLSLstd450Sinh");
    PIN(LAGFX_SPV_GLSL_COSH, 20, "GLSLstd450Cosh");
    PIN(LAGFX_SPV_GLSL_TANH, 21, "GLSLstd450Tanh");
    PIN(LAGFX_SPV_GLSL_ATAN2, 25, "GLSLstd450Atan2");
    PIN(LAGFX_SPV_GLSL_POW, 26, "GLSLstd450Pow");
    PIN(LAGFX_SPV_GLSL_EXP, 27, "GLSLstd450Exp");
    PIN(LAGFX_SPV_GLSL_LOG, 28, "GLSLstd450Log");
    PIN(LAGFX_SPV_GLSL_EXP2, 29, "GLSLstd450Exp2");
    PIN(LAGFX_SPV_GLSL_LOG2, 30, "GLSLstd450Log2");
    PIN(LAGFX_SPV_GLSL_SQRT, 31, "GLSLstd450Sqrt");
    PIN(LAGFX_SPV_GLSL_INVERSE_SQRT, 32, "GLSLstd450InverseSqrt");
    PIN(LAGFX_SPV_GLSL_FMIN, 37, "GLSLstd450FMin");
    PIN(LAGFX_SPV_GLSL_FMAX, 40, "GLSLstd450FMax");
    PIN(LAGFX_SPV_GLSL_FCLAMP, 43, "GLSLstd450FClamp");
    PIN(LAGFX_SPV_GLSL_FMIX, 46, "GLSLstd450FMix");
    PIN(LAGFX_SPV_GLSL_STEP, 48, "GLSLstd450Step");
    PIN(LAGFX_SPV_GLSL_SMOOTH_STEP, 49, "GLSLstd450SmoothStep");
    PIN(LAGFX_SPV_GLSL_FMA, 50, "GLSLstd450Fma");
    PIN(LAGFX_SPV_GLSL_LDEXP, 53, "GLSLstd450Ldexp");
    PIN(LAGFX_SPV_GLSL_LENGTH, 66, "GLSLstd450Length");
    PIN(LAGFX_SPV_GLSL_DISTANCE, 67, "GLSLstd450Distance");
    PIN(LAGFX_SPV_GLSL_CROSS, 68, "GLSLstd450Cross");
    PIN(LAGFX_SPV_GLSL_NORMALIZE, 69, "GLSLstd450Normalize");
    PIN(LAGFX_SPV_GLSL_REFLECT, 71, "GLSLstd450Reflect");
    PIN(LAGFX_SPV_GLSL_REFRACT, 72, "GLSLstd450Refract");
    printf("%s spv-glsl-std450-numbers-unit (40 enumerators)\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
