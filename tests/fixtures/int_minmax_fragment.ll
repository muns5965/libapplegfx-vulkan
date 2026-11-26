; ModuleID = 'int_minmax_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %x = extractelement <4 x float> %1, i64 0
  %y = extractelement <4 x float> %1, i64 1
  %z = extractelement <4 x float> %1, i64 2
  %xa = call i32 @air.convert.s.i32.f.f32(float %x)
  %xb = call i32 @air.convert.s.i32.f.f32(float %y)
  %xc = call i32 @air.convert.s.i32.f.f32(float %z)
  %a1 = call i32 @air.min.s.i32(i32 %xa, i32 %xb)
  %a2 = call i32 @air.max.u.i32(i32 %xa, i32 %xb)
  %a3 = call i32 @air.clamp.s.i32(i32 %xa, i32 %xb, i32 %xc)
  %a4 = call i32 @air.clamp.u.i32(i32 %xa, i32 %xb, i32 %xc)
  %a5 = call i32 @air.abs.s.i32(i32 %xa, i1 false)
  %a6 = call i32 @air.min.u.i32(i32 %xa, i32 %xc)
  %a7 = call i32 @air.max.s.i32(i32 %xb, i32 %xc)
  %ha = trunc i32 %xa to i16
  %hb = trunc i32 %xb to i16
  %h1 = call i16 @air.min.u.i16(i16 %ha, i16 %hb)
  %h2 = call i16 @air.max.s.i16(i16 %ha, i16 %hb)
  %h1w = zext i16 %h1 to i32
  %h2w = zext i16 %h2 to i32
  %va0 = insertelement <2 x i32> poison, i32 %xa, i64 0
  %va = insertelement <2 x i32> %va0, i32 %xb, i64 1
  %vb0 = insertelement <2 x i32> poison, i32 %xc, i64 0
  %vb = insertelement <2 x i32> %vb0, i32 %xa, i64 1
  %w1 = call <2 x i32> @air.min.s.v2i32(<2 x i32> %va, <2 x i32> %vb)
  %w2 = call <2 x i32> @air.max.u.v2i32(<2 x i32> %va, <2 x i32> %vb)
  %w3 = call <2 x i32> @air.clamp.u.v2i32(<2 x i32> %va, <2 x i32> %vb, <2 x i32> %va)
  %w4 = call <2 x i32> @air.abs.s.v2i32(<2 x i32> %va)
  %w1x = extractelement <2 x i32> %w1, i64 0
  %w2x = extractelement <2 x i32> %w2, i64 1
  %w3x = extractelement <2 x i32> %w3, i64 0
  %w4x = extractelement <2 x i32> %w4, i64 1
  %c1 = call float @air.convert.f.f32.s.i32(i32 %a1)
  %c2 = call float @air.convert.f.f32.s.i32(i32 %a2)
  %c3 = call float @air.convert.f.f32.s.i32(i32 %a3)
  %c4 = call float @air.convert.f.f32.s.i32(i32 %a4)
  %c5 = call float @air.convert.f.f32.s.i32(i32 %a5)
  %c6 = call float @air.convert.f.f32.s.i32(i32 %a6)
  %c7 = call float @air.convert.f.f32.s.i32(i32 %a7)
  %c8 = call float @air.convert.f.f32.s.i32(i32 %h1w)
  %c9 = call float @air.convert.f.f32.s.i32(i32 %h2w)
  %c10 = call float @air.convert.f.f32.s.i32(i32 %w1x)
  %c11 = call float @air.convert.f.f32.s.i32(i32 %w2x)
  %c12 = call float @air.convert.f.f32.s.i32(i32 %w3x)
  %c13 = call float @air.convert.f.f32.s.i32(i32 %w4x)
  %s0 = fadd float %c1, %c2
  %s1 = fadd float %c3, %c4
  %s2 = fadd float %c5, %c6
  %s3 = fadd float %c7, %c8
  %s4 = fadd float %c9, %c10
  %s5 = fadd float %c11, %c12
  %f0 = fadd float %s0, %s1
  %f1 = fadd float %s2, %s3
  %f2 = fadd float %s4, %s5
  %f3 = fadd float %c13, %c1
  %v0 = insertelement <4 x float> poison, float %f0, i64 0
  %v1 = insertelement <4 x float> %v0, float %f1, i64 1
  %v2 = insertelement <4 x float> %v1, float %f2, i64 2
  %v3 = insertelement <4 x float> %v2, float %f3, i64 3
  ret <4 x float> %v3
}

declare <2 x i32> @air.abs.s.v2i32(<2 x i32>)
declare <2 x i32> @air.clamp.u.v2i32(<2 x i32>, <2 x i32>, <2 x i32>)
declare <2 x i32> @air.max.u.v2i32(<2 x i32>, <2 x i32>)
declare <2 x i32> @air.min.s.v2i32(<2 x i32>, <2 x i32>)
declare float @air.convert.f.f32.s.i32(i32)
declare i16 @air.max.s.i16(i16, i16)
declare i16 @air.min.u.i16(i16, i16)
declare i32 @air.abs.s.i32(i32, i1)
declare i32 @air.clamp.s.i32(i32, i32, i32)
declare i32 @air.clamp.u.i32(i32, i32, i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare i32 @air.max.s.i32(i32, i32)
declare i32 @air.max.u.i32(i32, i32)
declare i32 @air.min.s.i32(i32, i32)
declare i32 @air.min.u.i32(i32, i32)


!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.version = !{!9}
!llvm.ident = !{!10}
!air.language_version = !{!11}
!air.compile_options = !{!12, !13, !14}
!air.fragment = !{!15}

!0 = !{i32 7, !"air.max_device_buffers", i32 31}
!1 = !{i32 7, !"air.max_constant_buffers", i32 31}
!2 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!3 = !{i32 7, !"air.max_textures", i32 128}
!4 = !{i32 7, !"air.max_read_write_textures", i32 8}
!5 = !{i32 7, !"air.max_samplers", i32 16}
!6 = !{i32 2, !"SDK Version", [2 x i32] [i32 15, i32 6]}
!7 = !{i32 1, !"wchar_size", i32 4}
!8 = !{i32 7, !"frame-pointer", i32 2}
!9 = !{i32 2, i32 7, i32 0}
!10 = !{!"Apple metal version 32023.622 (metalfe-32023.622)"}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!12 = !{!"air.compile.denorms_disable"}
!13 = !{!"air.compile.fast_math_enable"}
!14 = !{!"air.compile.framebuffer_fetch_enable"}
!15 = !{ptr @IntOpsFragment, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !20}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos", !"air.arg_unused"}
!20 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
