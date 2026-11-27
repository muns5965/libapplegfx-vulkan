; ModuleID = 'rt_convert_c_fragment.air.bc'
source_filename = "<stdin>"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0, <4 x float> %1) {
entry:
  %x = extractelement <4 x float> %0, i64 0
  %xi = call i32 @air.convert.s.i32.f.f32(float %x)
  %d = sub i32 %xi, 32
  %b = add i32 %xi, 32768
  %a0 = insertelement <2 x i32> zeroinitializer, i32 %d, i64 0
  %a = insertelement <2 x i32> %a0, i32 %b, i64 1
  %t = call <2 x i16> @air.convert.s.v2i16.s.v2i32(<2 x i32> %a)
  %r1 = call <2 x i32> @air.convert.u.v2i32.s.v2i16(<2 x i16> %t)
  %r2 = call <2 x i32> @air.convert.s.v2i32.u.v2i16(<2 x i16> %t)
  %r1a = extractelement <2 x i32> %r1, i64 0
  %r1b = extractelement <2 x i32> %r1, i64 1
  %r2a = extractelement <2 x i32> %r2, i64 0
  %r2b = extractelement <2 x i32> %r2, i64 1
  %h0 = lshr i32 %r1a, 16
  %h1 = lshr i32 %r1b, 16
  %o0 = call float @air.convert.f.f32.u.i32(i32 %h0)
  %o1 = call float @air.convert.f.f32.u.i32(i32 %h1)
  %o2 = call float @air.convert.f.f32.s.i32(i32 %r2a)
  %o3 = call float @air.convert.f.f32.s.i32(i32 %r2b)
  %v0 = insertelement <4 x float> zeroinitializer, float %o0, i64 0
  %v1 = insertelement <4 x float> %v0, float %o1, i64 1
  %v2 = insertelement <4 x float> %v1, float %o2, i64 2
  %v3 = insertelement <4 x float> %v2, float %o3, i64 3
  ret <4 x float> %v3
}

declare i32 @air.convert.s.i32.f.f32(float)

declare float @air.convert.f.f32.s.i32(i32)

declare float @air.convert.f.f32.u.i32(i32)

declare <2 x i16> @air.convert.s.v2i16.s.v2i32(<2 x i32>)

declare <2 x i32> @air.convert.u.v2i32.s.v2i16(<2 x i16>)

declare <2 x i32> @air.convert.s.v2i32.u.v2i16(<2 x i16>)

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
