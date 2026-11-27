; HAND-WRITTEN IR (not Apple output): p loop plan, discard in: merge [I]. See gen-discard-fixtures.sh
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  br label %H
H:
  %i = phi i32 [ 0, %entry ], [ %i2, %H ]
  %v = phi float [ 1.0, %entry ], [ %v2, %H ]
  %v2 = fmul float %v, 0.5
  %i2 = add i32 %i, 1
  %c = icmp slt i32 %i2, 3
  br i1 %c, label %H, label %M
M:
  %r = fadd float %v2, 0.0
  call void @air.discard_fragment()
  %o = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %r, i64 0
  ret <4 x float> %o
}
declare void @air.discard_fragment()
!llvm.module.flags = !{!0}
!air.version = !{!9}
!air.language_version = !{!11}
!air.fragment = !{!15}
!0 = !{i32 1, !"wchar_size", i32 4}
!9 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!15 = !{ptr @F, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
