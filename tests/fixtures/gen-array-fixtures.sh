#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures of section D of tests/air2spv-sampler-lavapipe.c (texture2d_array) with llvm-as. NOT Apple output: no captured or
# vendor module carries an array texture, so the operand order of air.sample_texture_2d_array is the INFERRED one of aee8ded
#   (tex, samp, <2 x float> coord, i32 array_index, i1 has_offset, <2 x i32> offset, i1 lod_is_level, float lod_or_bias, float min_lod, i32 flags)
# and the argument type name "texture2d_array<float, sample>" is by analogy with the captured "texture3d<half, sample>".
#   arr_lod        texture2d_array + sampler argument; coordinate (px / 8, py / 8), array index = floor(py / 2), explicit level = floor(px / 2)
#   arr_plainarg   the same call but the texture argument is typed "texture2d<float, sample>" (metadata does not say array): the index is dropped, layer 0 of a 2D image
#   arr_cxs_pix    constexpr PIXEL sampler (clamp to edge, nearest), coordinate (px, py) in texels, array index = floor(py / 2), level 0
# Usage: tests/fixtures/gen-array-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
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
DECL='declare { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i32, i1, <2 x i32>, i1, float, float, i32)'
body() {   # $1 = scale of the coordinate, $2 = sampler operand, $3 = extra params, $4 = level instruction value ("%lodf" or a literal)
cat <<EOT
define <4 x float> @Arr(<4 x float> %0, ptr addrspace(1) %1${3:-}) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %u = fmul float %px, $1
  %v = fmul float %py, $1
  %uv0 = insertelement <2 x float> zeroinitializer, float %u, i64 0
  %uv = insertelement <2 x float> %uv0, float %v, i64 1
  %hx = fmul float %px, 5.000000e-01
  %hy = fmul float %py, 5.000000e-01
  %lx = fptosi float %hx to i32
  %lodf = sitofp i32 %lx to float
  %layer = fptosi float %hy to i32
  %r = call { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) %1, ptr addrspace(2) $2, <2 x float> %uv, i32 %layer, i1 true, <2 x i32> zeroinitializer, i1 true, float $4, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %c
}
$DECL
EOT
}
TEX_ARR='!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d_array<float, sample>", !"air.arg_name", !"tex"}'
TEX_2D='!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}'
SMP='!20 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}'
rt() {   # $1 = file, $2 = texture metadata line
  { echo "$HDR"; body 1.250000e-01 "%2" ", ptr addrspace(2) %2" "%lodf"; echo "$FTR"; echo '!air.fragment = !{!15}'
    echo '!15 = !{ptr @Arr, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19, !20}'; echo "$2"; echo "$SMP"
  } | llvm-as -o "$1.air.bc" -
}
rt arr_lod "$TEX_ARR"
rt arr_plainarg "$TEX_2D"
# pixel constexpr sampler: word = 0x807bff0000080000 | clamp-to-edge (1 in each address field) | pixel bit 15, nearest filters
W=$(python3 -c "v=0x807bff0000080000 | (1 | 1<<3 | 1<<6) | 1<<15; print(v-(1<<64) if v>>63 else v)")
{ echo "$HDR"; echo "@__air_sampler_state = internal addrspace(2) constant i64 $W, align 8"
  body 1.000000e+00 "@__air_sampler_state" "" "0.000000e+00"; echo "$FTR"; echo '!air.sampler_states = !{!18}'; echo '!air.fragment = !{!15}'
  echo '!15 = !{ptr @Arr, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19}'; echo '!18 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}'; echo "$TEX_ARR"
} | llvm-as -o arr_cxs_pix.air.bc -
