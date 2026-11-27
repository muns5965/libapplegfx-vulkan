; HAND-WRITTEN IR (not Apple output): odd x discards, the other pixels read the derivatives AFTER the branch (their quad neighbours are discarded lanes) [I]. See gen-discard-fixtures.sh
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %xi = fptosi float %px to i32
  %b = and i32 %xi, 1
  %c = icmp eq i32 %b, 1
  br i1 %c, label %d, label %k
d:
  call void @air.discard_fragment()
  br label %k
k:
  %dx = call float @air.dfdx.f32(float %px)
  %dy = call float @air.dfdy.f32(float %py)
  %o0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %dx, i64 0
  %o1 = insertelement <4 x float> %o0, float %dy, i64 1
  ret <4 x float> %o1
}
declare void @air.discard_fragment()
declare float @air.dfdx.f32(float)
declare float @air.dfdy.f32(float)
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
