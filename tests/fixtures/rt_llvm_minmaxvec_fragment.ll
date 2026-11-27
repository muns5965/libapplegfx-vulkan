; ModuleID = 'rt_llvm_minmaxvec_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %X = call i32 @air.convert.s.i32.f.f32(float %px)
  %py = extractelement <4 x float> %0, i64 1
  %Y = call i32 @air.convert.s.i32.f.f32(float %py)
  %a8.t = shl i32 %X, 2
  %a8.o = and i32 %X, 1
  %a8.m = mul i32 %a8.o, 3
  %a8.w = or i32 %a8.t, %a8.m
  %a8 = trunc i32 %a8.w to i8
  %b8.t = shl i32 %Y, 2
  %b8.o = and i32 %Y, 1
  %b8.m = mul i32 %b8.o, 3
  %b8.w = or i32 %b8.t, %b8.m
  %b8 = trunc i32 %b8.w to i8
  %a16.t = shl i32 %X, 10
  %a16.o = and i32 %X, 1
  %a16.m = mul i32 %a16.o, 1023
  %a16.w = or i32 %a16.t, %a16.m
  %a16 = trunc i32 %a16.w to i16
  %b16.t = shl i32 %Y, 10
  %b16.o = and i32 %Y, 1
  %b16.m = mul i32 %b16.o, 1023
  %b16.w = or i32 %b16.t, %b16.m
  %b16 = trunc i32 %b16.w to i16
  %a32.t = shl i32 %X, 26
  %a32.o = and i32 %X, 1
  %a32.m = mul i32 %a32.o, 67108863
  %a32 = or i32 %a32.t, %a32.m
  %b32.t = shl i32 %Y, 26
  %b32.o = and i32 %Y, 1
  %b32.m = mul i32 %b32.o, 67108863
  %b32 = or i32 %b32.t, %b32.m
  %a64.x = zext i32 %X to i64
  %a64.t = shl i64 %a64.x, 58
  %a64.o = and i64 %a64.x, 1
  %a64.m = mul i64 %a64.o, 288230376151711743
  %a64 = or i64 %a64.t, %a64.m
  %b64.x = zext i32 %Y to i64
  %b64.t = shl i64 %b64.x, 58
  %b64.o = and i64 %b64.x, 1
  %b64.m = mul i64 %b64.o, 288230376151711743
  %b64 = or i64 %b64.t, %b64.m
  %a8x0 = xor i8 %a8, 0
  %b8x0 = add i8 %b8, 0
  %a8x1 = xor i8 %a8, 85
  %b8x1 = add i8 %b8, 1
  %a8x2 = xor i8 %a8, 170
  %b8x2 = add i8 %b8, 2
  %a8x3 = xor i8 %a8, 255
  %b8x3 = add i8 %b8, 3
  %va80 = insertelement <4 x i8> poison, i8 %a8x0, i64 0
  %va81 = insertelement <4 x i8> %va80, i8 %a8x1, i64 1
  %va82 = insertelement <4 x i8> %va81, i8 %a8x2, i64 2
  %va8 = insertelement <4 x i8> %va82, i8 %a8x3, i64 3
  %vb80 = insertelement <4 x i8> poison, i8 %b8x0, i64 0
  %vb81 = insertelement <4 x i8> %vb80, i8 %b8x1, i64 1
  %vb82 = insertelement <4 x i8> %vb81, i8 %b8x2, i64 2
  %vb8 = insertelement <4 x i8> %vb82, i8 %b8x3, i64 3
  %vr0 = call <4 x i8> @llvm.smax.v4i8(<4 x i8> %va8, <4 x i8> %vb8)
  %r0 = extractelement <4 x i8> %vr0, i64 2
  %a16x0 = xor i16 %a16, 0
  %b16x0 = add i16 %b16, 0
  %a16x1 = xor i16 %a16, 4369
  %b16x1 = add i16 %b16, 1
  %a16x2 = xor i16 %a16, 8738
  %b16x2 = add i16 %b16, 2
  %va160 = insertelement <3 x i16> poison, i16 %a16x0, i64 0
  %va161 = insertelement <3 x i16> %va160, i16 %a16x1, i64 1
  %va16 = insertelement <3 x i16> %va161, i16 %a16x2, i64 2
  %vb160 = insertelement <3 x i16> poison, i16 %b16x0, i64 0
  %vb161 = insertelement <3 x i16> %vb160, i16 %b16x1, i64 1
  %vb16 = insertelement <3 x i16> %vb161, i16 %b16x2, i64 2
  %vr1 = call <3 x i16> @llvm.umin.v3i16(<3 x i16> %va16, <3 x i16> %vb16)
  %r1 = extractelement <3 x i16> %vr1, i64 1
  %a32x0 = xor i32 %a32, 0
  %b32x0 = add i32 %b32, 0
  %a32x1 = xor i32 %a32, 1431655765
  %b32x1 = add i32 %b32, 1
  %a32x2 = xor i32 %a32, 2863311530
  %b32x2 = add i32 %b32, 2
  %a32x3 = xor i32 %a32, 4294967295
  %b32x3 = add i32 %b32, 3
  %va320 = insertelement <4 x i32> poison, i32 %a32x0, i64 0
  %va321 = insertelement <4 x i32> %va320, i32 %a32x1, i64 1
  %va322 = insertelement <4 x i32> %va321, i32 %a32x2, i64 2
  %va32 = insertelement <4 x i32> %va322, i32 %a32x3, i64 3
  %vb320 = insertelement <4 x i32> poison, i32 %b32x0, i64 0
  %vb321 = insertelement <4 x i32> %vb320, i32 %b32x1, i64 1
  %vb322 = insertelement <4 x i32> %vb321, i32 %b32x2, i64 2
  %vb32 = insertelement <4 x i32> %vb322, i32 %b32x3, i64 3
  %vr2 = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %va32, <4 x i32> %vb32)
  %r2 = extractelement <4 x i32> %vr2, i64 3
  %a64x = xor i64 %a64, 6148914691236517205
  %va640 = insertelement <2 x i64> poison, i64 %a64, i64 0
  %va64 = insertelement <2 x i64> %va640, i64 %a64x, i64 1
  %vb640 = insertelement <2 x i64> poison, i64 %b64, i64 0
  %vb64 = insertelement <2 x i64> %vb640, i64 %b64, i64 1
  %vr3 = call <2 x i64> @llvm.smin.v2i64(<2 x i64> %va64, <2 x i64> %vb64)
  %r3 = extractelement <2 x i64> %vr3, i64 1
  %r0.z = zext i8 %r0 to i32
  %r0.f = call float @air.convert.f.f32.u.i32(i32 %r0.z)
  %r1.z = zext i16 %r1 to i32
  %r1.f = call float @air.convert.f.f32.u.i32(i32 %r1.z)
  %r2.fh = lshr i32 %r2, 16
  %r2.fl = and i32 %r2, 65535
  %r2.fx = xor i32 %r2.fh, %r2.fl
  %r2.f = call float @air.convert.f.f32.u.i32(i32 %r2.fx)
  %r3.s1 = lshr i64 %r3, 16
  %r3.s2 = lshr i64 %r3, 32
  %r3.s3 = lshr i64 %r3, 48
  %r3.x1 = xor i64 %r3, %r3.s1
  %r3.x2 = xor i64 %r3.x1, %r3.s2
  %r3.x3 = xor i64 %r3.x2, %r3.s3
  %r3.tt = trunc i64 %r3.x3 to i32
  %r3.fz = and i32 %r3.tt, 65535
  %r3.f = call float @air.convert.f.f32.u.i32(i32 %r3.fz)
  %v0 = insertelement <4 x float> poison, float %r0.f, i64 0
  %v1 = insertelement <4 x float> %v0, float %r1.f, i64 1
  %v2 = insertelement <4 x float> %v1, float %r2.f, i64 2
  %v3 = insertelement <4 x float> %v2, float %r3.f, i64 3
  ret <4 x float> %v3
}

declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare float @air.convert.f.f32.s.i32(i32)
declare <4 x i8> @llvm.smax.v4i8(<4 x i8>, <4 x i8>)
declare <3 x i16> @llvm.umin.v3i16(<3 x i16>, <3 x i16>)
declare <4 x i32> @llvm.umax.v4i32(<4 x i32>, <4 x i32>)
declare <2 x i64> @llvm.smin.v2i64(<2 x i64>, <2 x i64>)


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
