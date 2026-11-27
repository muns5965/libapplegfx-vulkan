; HAND-WRITTEN IR (not Apple output): nested loops, three discards [I]. See gen-discard-robust-fixtures.sh
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

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
!llvm.module.flags = !{!0}
!air.version = !{!9}
!air.language_version = !{!11}
!air.fragment = !{!15}
!0 = !{i32 1, !"wchar_size", i32 4}
!9 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!15 = !{ptr @F, !16, !18}
!18 = !{!19}
