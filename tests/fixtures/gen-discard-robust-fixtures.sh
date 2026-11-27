#!/bin/sh
# Regenerates the HAND-WRITTEN fixtures of the S06 robustness tests (tests/air2spv-discard-robust-lavapipe.c), NOT Apple output. `call void @air.discard_fragment()` is the [I] spelling.
# Every fragment function takes the AIR position (the lavapipe test's vertex shader passes fragment x + 0.5, y + 0.5 of an 8 x 8 target) and returns a float4 render target 0.
#   spec_discard_r_nested   nested loops (outer 3 x inner 3, general CFG -> block-dispatch lowering) with THREE discards: outer header (i == 2 && x < 2), inner body (i == 1 && j == 1 && x > 6), after the loops (y < 1)
#   spec_discard_r_diamond  if / else diamond whose merge block has an OpPhi, discard inside the then-arm (x < 4 && y < 4) before the phi's edge
# (No device-buffer store fixture: LLVM 18's llvm-as only writes OPAQUE pointers, and frag_resource_kind classifies a buffer argument by its typed STRUCT / data pointee [as in Apple's typed-pointer AIR], so a hand-written
#  `ptr addrspace(1)` buffer argument is taken for a texture. The buffer / atomic side effects are run from hand-written GLSL instead: discard_atomic.frag.)
#   spec_discard_r_imgstore discard (x < 4), then air.write_texture_2d of (1, 0.5, 0.25, 1) into a write-only texture (arg 1) at (x, y)
# Usage: tests/fixtures/gen-discard-robust-fixtures.sh   (needs llvm-as; the .ll and .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
HDR='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer
'
MD0='!llvm.module.flags = !{!0}
!air.version = !{!9}
!air.language_version = !{!11}
!air.fragment = !{!15}
!0 = !{i32 1, !"wchar_size", i32 4}
!9 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}'
emit() {   # name comment body-file extra-md   (stdin: the function text)
  { echo "; HAND-WRITTEN IR (not Apple output): $2 [I]. See gen-discard-robust-fixtures.sh"; echo "$HDR"; cat; echo "$MD0"; echo "$3"; } > "$1.ll"
  llvm-as "$1.ll" -o "$1.air.bc"
}

emit spec_discard_r_nested "nested loops, three discards" '!15 = !{ptr @F, !16, !18}
!18 = !{!19}' <<'EOF'
define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  br label %O
O:
  %i = phi i32 [ 0, %entry ], [ %i2, %L ]
  %acc = phi float [ 0.0, %entry ], [ %a2, %L ]
  %ci = icmp eq i32 %i, 2
  %cx = fcmp olt float %px, 2.0
  %ca = and i1 %ci, %cx
  br i1 %ca, label %DA, label %I0
DA:
  call void @air.discard_fragment()
  br label %I0
I0:
  br label %N
N:
  %j = phi i32 [ 0, %I0 ], [ %j2, %C ]
  %a = phi float [ %acc, %I0 ], [ %a1, %C ]
  %c1 = icmp eq i32 %i, 1
  %c2 = icmp eq i32 %j, 1
  %c3 = fcmp ogt float %px, 6.0
  %c4 = and i1 %c1, %c2
  %c5 = and i1 %c4, %c3
  br i1 %c5, label %DB, label %C
DB:
  call void @air.discard_fragment()
  br label %C
C:
  %a1 = fadd float %a, 1.0
  %j2 = add i32 %j, 1
  %cj = icmp slt i32 %j2, 3
  br i1 %cj, label %N, label %L
L:
  %a2 = phi float [ %a1, %C ]
  %i2 = add i32 %i, 1
  %cl = icmp slt i32 %i2, 3
  br i1 %cl, label %O, label %X
X:
  %cy = fcmp olt float %py, 1.0
  br i1 %cy, label %DC, label %R
DC:
  call void @air.discard_fragment()
  br label %R
R:
  %v = fmul float %a2, 0.0625
  %o0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %v, i64 0
  ret <4 x float> %o0
}
declare void @air.discard_fragment()
EOF

emit spec_discard_r_diamond "if/else diamond, merge OpPhi, discard in the then arm" '!15 = !{ptr @F, !16, !18}
!18 = !{!19}' <<'EOF'
define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %c = fcmp olt float %px, 4.0
  br i1 %c, label %T, label %E
T:
  %d = fcmp olt float %py, 4.0
  br i1 %d, label %TD, label %M
TD:
  call void @air.discard_fragment()
  br label %M
E:
  br label %M
M:
  %v = phi float [ 0.25, %T ], [ 0.25, %TD ], [ 0.5, %E ]
  %o = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %v, i64 0
  ret <4 x float> %o
}
declare void @air.discard_fragment()
EOF

emit spec_discard_r_imgstore "discard, then air.write_texture_2d" '!15 = !{ptr @F, !16, !18}
!18 = !{!19, !20}
!20 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"img"}' <<'EOF'
define <4 x float> @F(<4 x float> %0, ptr addrspace(1) %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %c = fcmp olt float %px, 4.0
  br i1 %c, label %D, label %K
D:
  call void @air.discard_fragment()
  br label %K
K:
  %xi = fptosi float %px to i32
  %yi = fptosi float %py to i32
  %c0 = insertelement <2 x i32> zeroinitializer, i32 %xi, i64 0
  %c1 = insertelement <2 x i32> %c0, i32 %yi, i64 1
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %1, <2 x i32> %c1, <4 x float> <float 1.0, float 0.5, float 0.25, float 1.0>, i32 0)
  ret <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0>
}
declare void @air.discard_fragment()
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32)
EOF
