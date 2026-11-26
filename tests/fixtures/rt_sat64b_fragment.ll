; ModuleID = 'rt_sat64b_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %xa = call i32 @air.convert.s.i32.f.f32(float %px)
  %is0 = icmp eq i32 %xa, 0
  %sh = sub i32 %xa, 1
  %shc = select i1 %is0, i32 0, i32 %sh
  %shm = and i32 %shc, 31
  %p = shl i32 1, %shm
  %A = select i1 %is0, i32 0, i32 %p
  %A2 = lshr i32 %A, 2
  %A5 = lshr i32 %A, 5
  %B1 = or i32 %A, %A2
  %B = or i32 %B1, %A5
  %py = extractelement <4 x float> %0, i64 1
  %ya = call i32 @air.convert.s.i32.f.f32(float %py)
  %sh64 = zext i32 %shc to i64
  %p64 = shl i64 1, %sh64
  %L = select i1 %is0, i64 0, i64 %p64
  %L2 = lshr i64 %L, 2
  %L7 = lshr i64 %L, 7
  %LB1 = or i64 %L, %L2
  %LB = or i64 %LB1, %L7
  %a64_x = zext i32 %xa to i64
  %a64_t = shl i64 %a64_x, 58
  %a64_o = and i64 %a64_x, 1
  %a64_m = mul i64 %a64_o, 288230376151711743
  %a64 = or i64 %a64_t, %a64_m
  %b64_x = zext i32 %ya to i64
  %b64_t = shl i64 %b64_x, 58
  %b64_o = and i64 %b64_x, 1
  %b64_m = mul i64 %b64_o, 288230376151711743
  %b64 = or i64 %b64_t, %b64_m
  %a16_t = shl i32 %xa, 10
  %a16_o = and i32 %xa, 1
  %a16_m = mul i32 %a16_o, 1023
  %a16_32 = or i32 %a16_t, %a16_m
  %a16 = trunc i32 %a16_32 to i16
  %b16_t = shl i32 %ya, 10
  %b16_o = and i32 %ya, 1
  %b16_m = mul i32 %b16_o, 1023
  %b16_32 = or i32 %b16_t, %b16_m
  %b16 = trunc i32 %b16_32 to i16
  %a32_t = shl i32 %xa, 26
  %a32_o = and i32 %xa, 1
  %a32_m = mul i32 %a32_o, 67108863
  %a32 = or i32 %a32_t, %a32_m
  %b32_t = shl i32 %ya, 26
  %b32_o = and i32 %ya, 1
  %b32_m = mul i32 %b32_o, 67108863
  %b32 = or i32 %b32_t, %b32_m
  %r0 = call i64 @air.sub_sat.u.i64(i64 %a64, i64 %b64)
  %r1 = call i64 @air.sub_sat.s.i64(i64 %a64, i64 %b64)
  %f0_a = lshr i64 %r0, 16
  %f0_b = lshr i64 %r0, 32
  %f0_c = lshr i64 %r0, 48
  %f0_d = xor i64 %r0, %f0_a
  %f0_e = xor i64 %f0_d, %f0_b
  %f0_f = xor i64 %f0_e, %f0_c
  %f0_g = and i64 %f0_f, 65535
  %f0_z = trunc i64 %f0_g to i32
  %f0 = call float @air.convert.f.f32.u.i32(i32 %f0_z)
  %f1_a = lshr i64 %r1, 16
  %f1_b = lshr i64 %r1, 32
  %f1_c = lshr i64 %r1, 48
  %f1_d = xor i64 %r1, %f1_a
  %f1_e = xor i64 %f1_d, %f1_b
  %f1_f = xor i64 %f1_e, %f1_c
  %f1_g = and i64 %f1_f, 65535
  %f1_z = trunc i64 %f1_g to i32
  %f1 = call float @air.convert.f.f32.u.i32(i32 %f1_z)
  %p0 = insertelement <2 x i16> poison, i16 %a16, i64 0
  %p1 = insertelement <2 x i16> %p0, i16 %b16, i64 1
  %q0 = insertelement <2 x i16> poison, i16 %b16, i64 0
  %q1 = insertelement <2 x i16> %q0, i16 %a16, i64 1
  %vr = call <2 x i16> @air.add_sat.s.v2i16(<2 x i16> %p1, <2 x i16> %q1)
  %vr1 = extractelement <2 x i16> %vr, i64 1
  %f2_z = zext i16 %vr1 to i32
  %f2 = call float @air.convert.f.f32.u.i32(i32 %f2_z)
  %s0 = insertelement <2 x i32> poison, i32 %a32, i64 0
  %s1 = insertelement <2 x i32> %s0, i32 %b32, i64 1
  %t0 = insertelement <2 x i32> poison, i32 %b32, i64 0
  %t1 = insertelement <2 x i32> %t0, i32 %a32, i64 1
  %wr = call <2 x i32> @air.mul_hi.u.v2i32(<2 x i32> %s1, <2 x i32> %t1)
  %wr0 = extractelement <2 x i32> %wr, i64 0
  %f3_z = lshr i32 %wr0, 16
  %f3 = call float @air.convert.f.f32.u.i32(i32 %f3_z)
  %v0 = insertelement <4 x float> poison, float %f0, i64 0
  %v1 = insertelement <4 x float> %v0, float %f1, i64 1
  %v2 = insertelement <4 x float> %v1, float %f2, i64 2
  %v3 = insertelement <4 x float> %v2, float %f3, i64 3
  ret <4 x float> %v3
}

declare <2 x i16> @air.add_sat.s.v2i16(<2 x i16>, <2 x i16>)
declare <2 x i32> @air.mul_hi.u.v2i32(<2 x i32>, <2 x i32>)
declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare i64 @air.sub_sat.s.i64(i64, i64)
declare i64 @air.sub_sat.u.i64(i64, i64)


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
