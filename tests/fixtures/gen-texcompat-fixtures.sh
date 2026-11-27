#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures t3_*.air.bc of tests/texture-compat-lavapipe.c (item #17, texture3d) with llvm-as. Not Apple output.
#   fragment function Smp3: arguments (position float4, texture3d, sampler); coordinate = (px / 8, 0.375, 0.25 * py + ZC) with px / py the fragment position (x + 0.5, y + 0.5)
#   of the 8 x 8 target, so every pixel column is a u (2 columns per texel of a 4-wide texture) and every pixel row is one depth coordinate z = 0.25 * y + ZC + 0.125.
#   air.sample_texture_3d.v4f32(tex, smp, coord, i1 has_offset, <3 x i32> offset, i1 lod_is_level, float lod_or_bias, float min_lod, i32 flags): offset, level form and level are per fixture.
# Usage: tests/fixtures/gen-texcompat-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
HDR='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"'
FTR='!llvm.module.flags = !{!0}
!air.version = !{!10}
!air.language_version = !{!11}
!0 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 2, i32 8, i32 0}
!11 = !{!"Metal", i32 4, i32 0, i32 0}
!16 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!17 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}'
t3() {   # $1 name  $2 ZC (float literal)  $3 offset "x, y, z" integers  $4 lod_is_level (true|false)  $5 lod float literal
  { echo "$HDR"
    cat <<EOT
define <4 x float> @Smp3(<4 x float> %0, ptr addrspace(1) %1, ptr addrspace(2) %2) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %u = fmul float %px, 1.250000e-01
  %z0 = fmul float %py, 2.500000e-01
  %w = fadd float %z0, $2
  %uv0 = insertelement <3 x float> zeroinitializer, float %u, i64 0
  %uv1 = insertelement <3 x float> %uv0, float 3.750000e-01, i64 1
  %uvw = insertelement <3 x float> %uv1, float %w, i64 2
  %r = call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) %1, ptr addrspace(2) %2, <3 x float> %uvw, i1 true, <3 x i32> <$3>, i1 $4, float $5, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %c
}
declare { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <3 x float>, i1, <3 x i32>, i1, float, float, i32)
EOT
    echo "$FTR"; echo '!air.fragment = !{!15}'
    echo '!15 = !{ptr @Smp3, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19, !20}'
    echo '!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture3d<float, sample>", !"air.arg_name", !"tex"}'
    echo '!20 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}'
  } | llvm-as -o "$1.air.bc" -
}
# ZC = -0.375: z = 0.25 y - 0.25 (every row on a slice BOUNDARY k / 4, k = y - 1);  ZC = -0.25: z = 0.25 y - 0.125 (every row on a slice CENTRE of slice y - 1 ... y)
t3 t3_zb       -3.750000e-01 "i32 0, i32 0, i32 0"   false 0.000000e+00
t3 t3_zc       -2.500000e-01 "i32 0, i32 0, i32 0"   false 0.000000e+00
t3 t3_offz     -2.500000e-01 "i32 0, i32 0, i32 1"   false 0.000000e+00
t3 t3_offxyz   -2.500000e-01 "i32 1, i32 -1, i32 1"  false 0.000000e+00
t3 t3_lvl0     -2.500000e-01 "i32 0, i32 0, i32 0"   true  0.000000e+00
t3 t3_lvl05    -2.500000e-01 "i32 0, i32 0, i32 0"   true  5.000000e-01
t3 t3_lvl1     -2.500000e-01 "i32 0, i32 0, i32 0"   true  1.000000e+00
t3 t3_lvl15    -2.500000e-01 "i32 0, i32 0, i32 0"   true  1.500000e+00
t3 t3_lvl2     -2.500000e-01 "i32 0, i32 0, i32 0"   true  2.000000e+00
t3 t3_lvlneg   -2.500000e-01 "i32 0, i32 0, i32 0"   true -1.000000e+00
t3 t3_lvl1_off -2.500000e-01 "i32 0, i32 0, i32 1"   true  1.000000e+00
