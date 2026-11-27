; ModuleID = 'rt_nanmax_b_fragment.air.bc'
source_filename = "<stdin>"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %xf = fsub float %px, 5.000000e-01
  %v = fmul float %xf, 2.500000e-01
  %z = fsub float %xf, %xf
  %nan = fdiv float %z, %z
  %o0 = call float @air.clamp.f32(float %nan, float 5.000000e-01, float 4.000000e+00)
  %o1 = call float @air.clamp.f32(float %v, float 0x7FF8000000000000, float 4.000000e+00)
  %o2 = call float @air.clamp.f32(float %v, float 5.000000e-01, float %nan)
  %o3 = call float @air.saturate.f32(float %nan)
  %v0 = insertelement <4 x float> zeroinitializer, float %o0, i64 0
  %v1 = insertelement <4 x float> %v0, float %o1, i64 1
  %v2 = insertelement <4 x float> %v1, float %o2, i64 2
  %v3 = insertelement <4 x float> %v2, float %o3, i64 3
  ret <4 x float> %v3
}

declare float @air.clamp.f32(float, float, float)

declare float @air.saturate.f32(float)

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 7, i32 0}
!2 = !{!"Metal", i32 1, i32 2, i32 0}
!3 = !{ptr @F, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!8 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
