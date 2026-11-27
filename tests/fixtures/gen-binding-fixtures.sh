#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures of the descriptor-binding tests (tests/air2spv-binding-translate-unit.c, tests/vk-binding-layout-lavapipe.c) with llvm-as (not Apple output):
#   bind_many_fs.air.bc   fragment function: 24 texture2d args + constexpr samplers (2 distinct words) + 2 framebuffer-fetch inputs (air.render_target:0 / :1)
#   bind_wide_fs.air.bc   the same shape with 60 textures, 4 constexpr samplers and 4 framebuffer-fetch inputs (growth of the resource counts)
#   bind_nofetch_fs.air.bc 24 textures + 2 constexpr samplers, no framebuffer fetch
#   (every texture is sampled, round-robin through the constexpr samplers)
# Usage: tests/fixtures/gen-binding-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
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
# constexpr sampler words (src/air2spv/constexpr_sampler.h): normalized clamp-to-edge nearest / linear / repeat / mirror
word() { python3 -c "import sys; v=int(sys.argv[1],16); print(v-(1<<64) if v>>63 else v)" "$1"; }
CXS_W="807bff0000080092 807bff0000080a49 807bff0000080049 807bff0000080124"

fs() {   # $1 = file, $2 = ntex, $3 = ncxs, $4 = nfetch
  name=$1; nt=$2; nc=$3; nf=$4
  {
    echo "$HDR"
    k=0; for w in $CXS_W; do [ $k -lt $nc ] && echo "@__air_sampler_state_$k = internal addrspace(2) constant i64 $(word $w), align 8"; k=$((k+1)); done
    args="<4 x float> %0"; i=0
    while [ $i -lt $nt ]; do args="$args, ptr addrspace(1) %t$i"; i=$((i+1)); done
    i=0; while [ $i -lt $nf ]; do args="$args, <4 x float> %d$i"; i=$((i+1)); done
    echo "define <4 x float> @Bind($args) {"
    echo "entry:"
    echo "  %px = extractelement <4 x float> %0, i64 0"
    echo "  %uv0 = insertelement <2 x float> zeroinitializer, float %px, i64 0"
    echo "  %uv = insertelement <2 x float> %uv0, float %px, i64 1"
    acc="%zero"; echo "  %zero = fadd <4 x float> zeroinitializer, zeroinitializer"
    j=0
    while [ $j -lt $nt ]; do   # every texture is sampled (so every binding is statically used by the entry point: validation layers check it against the layout)
      echo "  %r$j = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %t$j, ptr addrspace(2) @__air_sampler_state_$((j % nc)), <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)"
      echo "  %c$j = extractvalue { <4 x float>, i8 } %r$j, 0"
      echo "  %a$j = fadd <4 x float> $acc, %c$j"
      acc="%a$j"; j=$((j+1))
    done
    i=0; while [ $i -lt $nf ]; do echo "  %f$i = fadd <4 x float> $acc, %d$i"; acc="%f$i"; i=$((i+1)); done
    echo "  ret <4 x float> $acc"
    echo "}"
    echo 'declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)'
    echo "$FTR"
    sl=""; k=0; while [ $k -lt $nc ]; do sl="$sl, !$((200+k))"; echo "!$((200+k)) = !{!\"air.sampler_state\", ptr addrspace(2) @__air_sampler_state_$k}"; k=$((k+1)); done
    [ $nc -gt 0 ] && echo "!air.sampler_states = !{$(echo "$sl" | sed 's/^, //')}"
    echo '!air.fragment = !{!15}'
    nodes="!17"; i=0
    while [ $i -lt $nt ]; do
      echo "!$((100+i)) = !{i32 $((1+i)), !\"air.texture\", !\"air.location_index\", i32 $i, i32 1, !\"air.sample\", !\"air.arg_type_name\", !\"texture2d<float, sample>\", !\"air.arg_name\", !\"t$i\"}"
      nodes="$nodes, !$((100+i))"; i=$((i+1))
    done
    i=0
    while [ $i -lt $nf ]; do
      echo "!$((400+i)) = !{i32 $((1+nt+i)), !\"air.render_target\", i32 $i, i32 0, !\"air.arg_type_name\", !\"float4\", !\"air.arg_name\", !\"dest$i\"}"
      nodes="$nodes, !$((400+i))"; i=$((i+1))
    done
    echo '!15 = !{ptr @Bind, !12, !13}'; echo '!12 = !{!16}'; echo "!13 = !{$nodes}"
  } | llvm-as -o "$name" -
}
fs bind_many_fs.air.bc 24 2 2
fs bind_wide_fs.air.bc 60 4 4
fs bind_nofetch_fs.air.bc 24 2 0

