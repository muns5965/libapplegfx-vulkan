; HAND-WRITTEN IR (not Apple output): integer -> integer air.convert; the spelling air.convert.<dk>.<dt>.<sk>.<st> with integer kinds is extrapolated from the float<->int names of the corpus [N].
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %h = call i16 @air.convert.s.i16.f.f32(float %px)
  %b = call i32 @air.convert.u.i32.s.i16(i16 %h)
  %c = call i32 @air.convert.s.i32.u.i16(i16 %h)
  %d = call i32 @air.convert.u.i32.u.i32(i32 %b)
  %e = add i32 %c, %d
  %f = call float @air.convert.f.f32.s.i32(i32 %e)
  %v = insertelement <4 x float> zeroinitializer, float %f, i64 0
  ret <4 x float> %v
}
declare i16 @air.convert.s.i16.f.f32(float)
declare i32 @air.convert.u.i32.s.i16(i16)
declare i32 @air.convert.s.i32.u.i16(i16)
declare i32 @air.convert.u.i32.u.i32(i32)
declare float @air.convert.f.f32.s.i32(i32)
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
