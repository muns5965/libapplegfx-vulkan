; ModuleID = 'rt_bitfield64_fragment'
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
  %m1 = mul i32 %xa, -1640531535
  %m2 = mul i32 %ya, -2048144789
  %mx = add i32 %m1, %m2
  %in = mul i32 %mx, 668265263
  %mxl = zext i32 %mx to i64
  %inl = zext i32 %in to i64
  %mxh = mul i64 %mxl, -7046029254386353131
  %inh = mul i64 %inl, -4658895280553007687
  %s64 = xor i64 %mxh, %mxl
  %t64 = xor i64 %inh, %inl
  %o64 = urem i32 %xa, 65
  %n640 = urem i32 %ya, 65
  %av64 = sub i32 64, %o64
  %lt64 = icmp ult i32 %n640, %av64
  %n64 = select i1 %lt64, i32 %n640, i32 %av64
  %r0 = call i64 @air.extract_bits.s.i64(i64 %s64, i32 %o64, i32 %n64)
  %r1 = call i64 @air.extract_bits.u.i64(i64 %s64, i32 %o64, i32 %n64)
  %r2 = call i64 @air.insert_bits.s.i64(i64 %s64, i64 %t64, i32 %o64, i32 %n64)
  %r3 = call i64 @air.insert_bits.u.i64(i64 %t64, i64 %s64, i32 %o64, i32 %n64)
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
  %f2_a = lshr i64 %r2, 16
  %f2_b = lshr i64 %r2, 32
  %f2_c = lshr i64 %r2, 48
  %f2_d = xor i64 %r2, %f2_a
  %f2_e = xor i64 %f2_d, %f2_b
  %f2_f = xor i64 %f2_e, %f2_c
  %f2_g = and i64 %f2_f, 65535
  %f2_z = trunc i64 %f2_g to i32
  %f2 = call float @air.convert.f.f32.u.i32(i32 %f2_z)
  %f3_a = lshr i64 %r3, 16
  %f3_b = lshr i64 %r3, 32
  %f3_c = lshr i64 %r3, 48
  %f3_d = xor i64 %r3, %f3_a
  %f3_e = xor i64 %f3_d, %f3_b
  %f3_f = xor i64 %f3_e, %f3_c
  %f3_g = and i64 %f3_f, 65535
  %f3_z = trunc i64 %f3_g to i32
  %f3 = call float @air.convert.f.f32.u.i32(i32 %f3_z)
  %v0 = insertelement <4 x float> poison, float %f0, i64 0
  %v1 = insertelement <4 x float> %v0, float %f1, i64 1
  %v2 = insertelement <4 x float> %v1, float %f2, i64 2
  %v3 = insertelement <4 x float> %v2, float %f3, i64 3
  ret <4 x float> %v3
}

declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare i64 @air.extract_bits.s.i64(i64, i32, i32)
declare i64 @air.extract_bits.u.i64(i64, i32, i32)
declare i64 @air.insert_bits.s.i64(i64, i64, i32, i32)
declare i64 @air.insert_bits.u.i64(i64, i64, i32, i32)


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
