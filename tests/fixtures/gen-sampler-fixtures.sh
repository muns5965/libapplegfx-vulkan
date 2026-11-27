#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures of tests/air2spv-sampler-lavapipe.c (not Apple output) with llvm-as:
#   cxs_<word>.air.bc       fragment function, constexpr sampler `word` (the i64 of an air.sampler_states global), texture2d arg, coordinate = (fragment position - 1.75)
#                           (texel units: the 4x4 test texture is sampled 1.5 texels below / above its edges); normalized word: coordinate / 4; pixel word (bit 15): texel units
#   smp_norm / smp_pix      the same with a sampler ARGUMENT (runtime sampler descriptor)
#   tex3d_diag / tex3d_centers   texture3d + sampler argument; coordinate (px/8, py/8, (px+py)/16) / (px/8, py/8, 0.375)
# Usage: tests/fixtures/gen-sampler-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
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
body2d() {   # $1 = scale, $2 = sampler operand
cat <<EOT
define <4 x float> @Smp(<4 x float> %0, ptr addrspace(1) %1${3:-}) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %tx = fadd float %px, -1.750000e+00
  %ty = fadd float %py, -1.750000e+00
  %u = fmul float %tx, $1
  %v = fmul float %ty, $1
  %uv0 = insertelement <2 x float> zeroinitializer, float %u, i64 0
  %uv = insertelement <2 x float> %uv0, float %v, i64 1
  %r = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %1, ptr addrspace(2) $2, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %c
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
EOT
}
cxs() {   # $1 = word (hex), $2 = scale
  w=$(python3 -c "import sys; v=int(sys.argv[1],16); print(v-(1<<64) if v>>63 else v)" "$1")
  { echo "$HDR"; echo "@__air_sampler_state = internal addrspace(2) constant i64 $w, align 8"; body2d "$2" "@__air_sampler_state" "";
    echo "$FTR"; echo '!air.sampler_states = !{!18}'; echo '!air.fragment = !{!15}'
    echo '!15 = !{ptr @Smp, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19}'
    echo '!18 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}'
    echo '!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}'
  } | llvm-as -o "cxs_$1.air.bc" -
}
cxsh() {   # $1 = word (hex), $2 = scale: the same fragment as cxs() but the sample is made in an internal HELPER function that receives texture + sampler (+ uv) as parameters
  w=$(python3 -c "import sys; v=int(sys.argv[1],16); print(v-(1<<64) if v>>63 else v)" "$1")
  { echo "$HDR"; echo "@__air_sampler_state = internal addrspace(2) constant i64 $w, align 8"
    cat <<EOT
define <4 x float> @Smp(<4 x float> %0, ptr addrspace(1) %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %tx = fadd float %px, -1.750000e+00
  %ty = fadd float %py, -1.750000e+00
  %u = fmul float %tx, $2
  %v = fmul float %ty, $2
  %uv0 = insertelement <2 x float> zeroinitializer, float %u, i64 0
  %uv = insertelement <2 x float> %uv0, float %v, i64 1
  %c = call <4 x float> @SampH(ptr addrspace(1) %1, ptr addrspace(2) @__air_sampler_state, <2 x float> %uv)
  ret <4 x float> %c
}
define internal <4 x float> @SampH(ptr addrspace(1) %t, ptr addrspace(2) %s, <2 x float> %uv) {
entry:
  %r = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %s, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %c
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
EOT
    echo "$FTR"; echo '!air.sampler_states = !{!18}'; echo '!air.fragment = !{!15}'
    echo '!15 = !{ptr @Smp, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19}'
    echo '!18 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}'
    echo '!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}'
  } | llvm-as -o "cxsh_$1.air.bc" -
}
smp() {   # $1 = name, $2 = scale
  { echo "$HDR"; body2d "$2" "%2" ", ptr addrspace(2) %2"; echo "$FTR"; echo '!air.fragment = !{!15}'
    echo '!15 = !{ptr @Smp, !12, !13}'; echo '!12 = !{!16}'; echo '!13 = !{!17, !19, !20}'
    echo '!19 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}'
    echo '!20 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}'
  } | llvm-as -o "$1.air.bc" -
}
tex3d() {   # $1 = name, $2 = the w operand (a value or literal), $3 = instruction lines that define it
  { echo "$HDR"
    cat <<EOT
define <4 x float> @Smp3(<4 x float> %0, ptr addrspace(1) %1, ptr addrspace(2) %2) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %u = fmul float %px, 1.250000e-01
  %v = fmul float %py, 1.250000e-01
$3
  %uv0 = insertelement <3 x float> zeroinitializer, float %u, i64 0
  %uv1 = insertelement <3 x float> %uv0, float %v, i64 1
  %uvw = insertelement <3 x float> %uv1, float $2, i64 2
  %r = call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) %1, ptr addrspace(2) %2, <3 x float> %uvw, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
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
# word = 0x807bff0000080000 | address (3 bits s,t,r) | mag filter << 9 | min filter << 11 | pixel << 15   (layout: src/air2spv/constexpr_sampler.h)
mkword() { python3 -c "import sys; a,f,p=map(int,sys.argv[1:]); print('%016x' % (0x807bff0000080000 | a | a << 3 | a << 6 | f << 9 | f << 11 | p << 15))" "$1" "$2" "$3"; }
for addr in 0 1 2 3 4; do for filt in 0 1; do cxs "$(mkword $addr $filt 0)" 2.500000e-01; done; done
for addr in 0 1 4; do for filt in 0 1; do cxs "$(mkword $addr $filt 1)" 1.000000e+00; done; done
cxs "$(mkword 2 0 1)" 1.000000e+00
cxs "$(mkword 3 1 1)" 1.000000e+00
cxsh "$(mkword 1 1 1)" 1.000000e+00
cxsh "$(mkword 4 0 1)" 1.000000e+00
cxsh "$(mkword 2 0 0)" 2.500000e-01
cxsh "$(mkword 3 1 0)" 2.500000e-01
smp smp_norm 2.500000e-01
smp smp_pix 1.000000e+00
tex3d tex3d_diag %w '  %s = fadd float %px, %py
  %w = fmul float %s, 6.250000e-02'
tex3d tex3d_centers 3.750000e-01 ''
