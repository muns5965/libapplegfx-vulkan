; ModuleID = 'rt_llvm_funnel2_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %X = call i32 @air.convert.s.i32.f.f32(float %px)
  %py = extractelement <4 x float> %0, i64 1
  %Y = call i32 @air.convert.s.i32.f.f32(float %py)
  %a32.t = shl i32 %X, 26
  %a32.o = and i32 %X, 1
  %a32.m = mul i32 %a32.o, 67108863
  %a32 = or i32 %a32.t, %a32.m
  %b32.t = shl i32 %Y, 26
  %b32.o = and i32 %Y, 1
  %b32.m = mul i32 %b32.o, 67108863
  %b32 = or i32 %b32.t, %b32.m
  %a64.x = zext i32 %Y to i64
  %a64.t = shl i64 %a64.x, 58
  %a64.o = and i64 %a64.x, 1
  %a64.m = mul i64 %a64.o, 288230376151711743
  %a64 = or i64 %a64.t, %a64.m
  %b64.x = zext i32 %X to i64
  %b64.t = shl i64 %b64.x, 58
  %b64.o = and i64 %b64.x, 1
  %b64.m = mul i64 %b64.o, 288230376151711743
  %b64 = or i64 %b64.t, %b64.m
  %y3 = mul i32 %Y, 3
  %s = add i32 %X, %y3
  %s64 = zext i32 %s to i64
  %r0 = call i32 @llvm.fshr.i32(i32 %a32, i32 %b32, i32 %s)
  %r1 = call i64 @llvm.fshl.i64(i64 %a64, i64 %b64, i64 %s64)
  %r2 = call i32 @llvm.fshl.i32(i32 %a32, i32 %a32, i32 %s)
  %r0.h = lshr i32 %r0, 16
  %r0.l = and i32 %r0, 65535
  %r0.hf = call float @air.convert.f.f32.u.i32(i32 %r0.h)
  %r0.lf = call float @air.convert.f.f32.u.i32(i32 %r0.l)
  %r1.s1 = lshr i64 %r1, 16
  %r1.s2 = lshr i64 %r1, 32
  %r1.s3 = lshr i64 %r1, 48
  %r1.x1 = xor i64 %r1, %r1.s1
  %r1.x2 = xor i64 %r1.x1, %r1.s2
  %r1.x3 = xor i64 %r1.x2, %r1.s3
  %r1.tt = trunc i64 %r1.x3 to i32
  %r1.fz = and i32 %r1.tt, 65535
  %r1.f = call float @air.convert.f.f32.u.i32(i32 %r1.fz)
  %r2.fh = lshr i32 %r2, 16
  %r2.fl = and i32 %r2, 65535
  %r2.fx = xor i32 %r2.fh, %r2.fl
  %r2.f = call float @air.convert.f.f32.u.i32(i32 %r2.fx)
  %v0 = insertelement <4 x float> poison, float %r0.hf, i64 0
  %v1 = insertelement <4 x float> %v0, float %r0.lf, i64 1
  %v2 = insertelement <4 x float> %v1, float %r1.f, i64 2
  %v3 = insertelement <4 x float> %v2, float %r2.f, i64 3
  ret <4 x float> %v3
}

declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare float @air.convert.f.f32.s.i32(i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)
declare i64 @llvm.fshl.i64(i64, i64, i64)
declare i32 @llvm.fshl.i32(i32, i32, i32)


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
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!20 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
