; ModuleID = 'rt_llvm_bitsflag_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %X = call i32 @air.convert.s.i32.f.f32(float %px)
  %py = extractelement <4 x float> %0, i64 1
  %Y = call i32 @air.convert.s.i32.f.f32(float %py)
  %is0 = icmp eq i32 %X, 0
  %sh = sub i32 %X, 1
  %shc = select i1 %is0, i32 0, i32 %sh
  %shm = and i32 %shc, 31
  %p = shl i32 1, %shm
  %A = select i1 %is0, i32 0, i32 %p
  %N = or i32 %A, 2048
  %r0 = call i32 @llvm.ctlz.i32(i32 %N, i1 true)
  %r1 = call i32 @llvm.cttz.i32(i32 %N, i1 true)
  %Bv = or i32 %A, 256
  %vp0 = insertelement <2 x i32> poison, i32 %A, i64 0
  %vp = insertelement <2 x i32> %vp0, i32 %Bv, i64 1
  %vr = call <2 x i32> @llvm.ctpop.v2i32(<2 x i32> %vp)
  %r2 = extractelement <2 x i32> %vr, i64 1
  %A16 = trunc i32 %A to i16
  %r3 = call i16 @llvm.bitreverse.i16(i16 %A16)
  %r3z = zext i16 %r3 to i32
  %c0 = call float @air.convert.f.f32.u.i32(i32 %r0)
  %c1 = call float @air.convert.f.f32.u.i32(i32 %r1)
  %c2 = call float @air.convert.f.f32.u.i32(i32 %r2)
  %c3 = call float @air.convert.f.f32.u.i32(i32 %r3z)
  %v0 = insertelement <4 x float> poison, float %c0, i64 0
  %v1 = insertelement <4 x float> %v0, float %c1, i64 1
  %v2 = insertelement <4 x float> %v1, float %c2, i64 2
  %v3 = insertelement <4 x float> %v2, float %c3, i64 3
  ret <4 x float> %v3
}

declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare float @air.convert.f.f32.s.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare <2 x i32> @llvm.ctpop.v2i32(<2 x i32>)
declare i16 @llvm.bitreverse.i16(i16)


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
