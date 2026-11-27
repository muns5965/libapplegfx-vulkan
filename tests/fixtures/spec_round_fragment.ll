; HAND-WRITTEN IR (not Apple output): MSL round() = halves away from zero; GLSL.std.450 Round leaves halves unspecified. Colour = (round(2.5)/4, -round(-2.5)/4, round(0.5)/2, -round(-0.5)/2) = (0.75, 0.75, 0.5, 0.5).
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0) {
entry:
  %a = call float @air.round.f32(float 2.5)
  %b = call float @air.round.f32(float -2.5)
  %c = call float @air.round.f32(float 0.5)
  %d = call float @air.round.f32(float -0.5)
  %x = fmul float %a, 0.25
  %y = fmul float %b, -0.25
  %z = fmul float %c, 0.5
  %w = fmul float %d, -0.5
  %v0 = insertelement <4 x float> zeroinitializer, float %x, i64 0
  %v1 = insertelement <4 x float> %v0, float %y, i64 1
  %v2 = insertelement <4 x float> %v1, float %z, i64 2
  %v3 = insertelement <4 x float> %v2, float %w, i64 3
  ret <4 x float> %v3
}
declare float @air.round.f32(float)
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
