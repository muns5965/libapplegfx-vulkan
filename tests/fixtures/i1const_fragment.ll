; ModuleID = 'bc/b32c82ce75a7dca3_SimpleColorFragment.air.bc'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @I1ConstFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %a = call i1 @air.any.v4i1(<4 x i1> zeroinitializer)
  %e = and i1 %a, true
  %r = select i1 %e, float 1.0, float 0.0
  %b = call i1 @air.all.v4i1(<4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  br i1 %b, label %gt, label %gf
gt:
  br label %join
gf:
  br label %join
join:
  %g = phi float [ 1.0, %gt ], [ 0.0, %gf ]
  %c = call i1 @air.any.v4i1(<4 x i1> <i1 false, i1 false, i1 true, i1 false>)
  %bl = select i1 %c, float 1.0, float 0.0
  %d = call i1 @air.all.v2i1(<2 x i1> <i1 true, i1 false>)
  %al = select i1 %d, float 0.0, float 1.0
  %v0 = insertelement <4 x float> poison, float %r, i64 0
  %v1 = insertelement <4 x float> %v0, float %g, i64 1
  %v2 = insertelement <4 x float> %v1, float %bl, i64 2
  %v3 = insertelement <4 x float> %v2, float %al, i64 3
  ret <4 x float> %v3
}

declare i1 @air.any.v4i1(<4 x i1>)
declare i1 @air.all.v4i1(<4 x i1>)
declare i1 @air.all.v2i1(<2 x i1>)


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
!15 = !{ptr @I1ConstFragment, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !20}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos", !"air.arg_unused"}
!20 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
