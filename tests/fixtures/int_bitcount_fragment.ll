; ModuleID = 'int_bitcount_fragment'
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
  %a1 = call i32 @air.clz.i32(i32 %xa, i1 false)
  %a2 = call i32 @air.ctz.i32(i32 %xb, i1 false)
  %a3 = call i32 @air.popcount.i32(i32 %xc)
  %a4 = call i32 @air.reverse_bits.i32(i32 %xa)
  %ha = trunc i32 %xa to i16
  %hb = trunc i32 %xb to i8
  %h1 = call i16 @air.clz.i16(i16 %ha, i1 false)
  %h2 = call i16 @air.ctz.i16(i16 %ha, i1 false)
  %h3 = call i8 @air.clz.i8(i8 %hb, i1 false)
  %h4 = call i8 @air.ctz.i8(i8 %hb, i1 false)
  %h5 = call i16 @air.popcount.i16(i16 %ha)
  %h6 = call i8 @air.reverse_bits.i8(i8 %hb)
  %h1w = zext i16 %h1 to i32
  %h2w = zext i16 %h2 to i32
  %h3w = zext i8 %h3 to i32
  %h4w = zext i8 %h4 to i32
  %h5w = zext i16 %h5 to i32
  %h6w = zext i8 %h6 to i32
  %la = zext i32 %xa to i64
  %lb = zext i32 %xb to i64
  %lc = shl i64 %lb, 32
  %ld = or i64 %la, %lc
  %l1 = call i64 @air.clz.i64(i64 %ld, i1 false)
  %l2 = call i64 @air.ctz.i64(i64 %ld, i1 false)
  %l3 = call i64 @air.popcount.i64(i64 %ld)
  %l4 = call i64 @air.reverse_bits.i64(i64 %ld)
  %l1n = trunc i64 %l1 to i32
  %l2n = trunc i64 %l2 to i32
  %l3n = trunc i64 %l3 to i32
  %l4n = trunc i64 %l4 to i32
  %va0 = insertelement <2 x i32> poison, i32 %xa, i64 0
  %va = insertelement <2 x i32> %va0, i32 %xb, i64 1
  %w1 = call <2 x i32> @air.clz.v2i32(<2 x i32> %va, i1 false)
  %w2 = call <2 x i32> @air.ctz.v2i32(<2 x i32> %va, i1 false)
  %w3 = call <2 x i32> @air.popcount.v2i32(<2 x i32> %va)
  %w4 = call <2 x i32> @air.reverse_bits.v2i32(<2 x i32> %va)
  %w1x = extractelement <2 x i32> %w1, i64 0
  %w2x = extractelement <2 x i32> %w2, i64 1
  %w3x = extractelement <2 x i32> %w3, i64 0
  %w4x = extractelement <2 x i32> %w4, i64 1
  %vl0 = insertelement <2 x i64> poison, i64 %ld, i64 0
  %vl = insertelement <2 x i64> %vl0, i64 %lc, i64 1
  %x1 = call <2 x i64> @air.clz.v2i64(<2 x i64> %vl, i1 false)
  %x2 = call <2 x i64> @air.ctz.v2i64(<2 x i64> %vl, i1 false)
  %x1e = extractelement <2 x i64> %x1, i64 1
  %x2e = extractelement <2 x i64> %x2, i64 0
  %x1n = trunc i64 %x1e to i32
  %x2n = trunc i64 %x2e to i32
  %c1 = call float @air.convert.f.f32.s.i32(i32 %a1)
  %c2 = call float @air.convert.f.f32.s.i32(i32 %a2)
  %c3 = call float @air.convert.f.f32.s.i32(i32 %a3)
  %c4 = call float @air.convert.f.f32.s.i32(i32 %a4)
  %c5 = call float @air.convert.f.f32.s.i32(i32 %h1w)
  %c6 = call float @air.convert.f.f32.s.i32(i32 %h2w)
  %c7 = call float @air.convert.f.f32.s.i32(i32 %h3w)
  %c8 = call float @air.convert.f.f32.s.i32(i32 %h4w)
  %c9 = call float @air.convert.f.f32.s.i32(i32 %h5w)
  %c10 = call float @air.convert.f.f32.s.i32(i32 %h6w)
  %c11 = call float @air.convert.f.f32.s.i32(i32 %l1n)
  %c12 = call float @air.convert.f.f32.s.i32(i32 %l2n)
  %c13 = call float @air.convert.f.f32.s.i32(i32 %l3n)
  %c14 = call float @air.convert.f.f32.s.i32(i32 %l4n)
  %c15 = call float @air.convert.f.f32.s.i32(i32 %w1x)
  %c16 = call float @air.convert.f.f32.s.i32(i32 %w2x)
  %c17 = call float @air.convert.f.f32.s.i32(i32 %w3x)
  %c18 = call float @air.convert.f.f32.s.i32(i32 %w4x)
  %c19 = call float @air.convert.f.f32.s.i32(i32 %x1n)
  %c20 = call float @air.convert.f.f32.s.i32(i32 %x2n)
  %s0 = fadd float %c1, %c2
  %s1 = fadd float %c3, %c4
  %s2 = fadd float %c5, %c6
  %s3 = fadd float %c7, %c8
  %s4 = fadd float %c9, %c10
  %s5 = fadd float %c11, %c12
  %s6 = fadd float %c13, %c14
  %s7 = fadd float %c15, %c16
  %s8 = fadd float %c17, %c18
  %s9 = fadd float %c19, %c20
  %f0 = fadd float %s0, %s1
  %f1 = fadd float %s2, %s3
  %f2 = fadd float %s4, %s5
  %f3 = fadd float %s6, %s7
  %v0 = insertelement <4 x float> poison, float %f0, i64 0
  %v1 = insertelement <4 x float> %v0, float %f1, i64 1
  %v2 = insertelement <4 x float> %v1, float %f2, i64 2
  %v3 = insertelement <4 x float> %v2, float %f3, i64 3
  ret <4 x float> %v3
}

declare <2 x i32> @air.clz.v2i32(<2 x i32>, i1)
declare <2 x i32> @air.ctz.v2i32(<2 x i32>, i1)
declare <2 x i32> @air.popcount.v2i32(<2 x i32>)
declare <2 x i32> @air.reverse_bits.v2i32(<2 x i32>)
declare <2 x i64> @air.clz.v2i64(<2 x i64>, i1)
declare <2 x i64> @air.ctz.v2i64(<2 x i64>, i1)
declare float @air.convert.f.f32.s.i32(i32)
declare i16 @air.clz.i16(i16, i1)
declare i16 @air.ctz.i16(i16, i1)
declare i16 @air.popcount.i16(i16)
declare i32 @air.clz.i32(i32, i1)
declare i32 @air.convert.s.i32.f.f32(float)
declare i32 @air.ctz.i32(i32, i1)
declare i32 @air.popcount.i32(i32)
declare i32 @air.reverse_bits.i32(i32)
declare i64 @air.clz.i64(i64, i1)
declare i64 @air.ctz.i64(i64, i1)
declare i64 @air.popcount.i64(i64)
declare i64 @air.reverse_bits.i64(i64)
declare i8 @air.clz.i8(i8, i1)
declare i8 @air.ctz.i8(i8, i1)
declare i8 @air.reverse_bits.i8(i8)


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
