; ModuleID = 'rt_bitsnarrow_fragment'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %xi = call i32 @air.convert.s.i32.f.f32(float %px)
  %yi = call i32 @air.convert.s.i32.f.f32(float %py)
  %xm = and i32 %xi, 127
  %ym = and i32 %yi, 127
  %a = zext i32 %xm to i64
  %b = zext i32 %ym to i64
  ; address-style arithmetic on small positive values: base + a * stride + b
  %m = mul i64 %a, %b
  %s = add i64 %m, 1000
  %r0 = trunc i64 %s to i32
  ; signed compare of a difference (negative for a < b) selecting between two products
  %am = sub i64 %a, 32
  %bm = sub i64 %b, 32
  %lt = icmp slt i64 %am, %bm
  %p5 = mul i64 %a, 5
  %p7 = mul i64 %b, 7
  %sel = select i1 %lt, i64 %p5, i64 %p7
  %r1 = trunc i64 %sel to i32
  ; unsigned compare + constant shifts of a packed index
  %hi = shl i64 %a, 14
  %lo = shl i64 %b, 8
  %pk = or i64 %hi, %lo
  %ult = icmp ult i64 %a, %b
  %sh = and i64 %pk, 65535
  %z = select i1 %ult, i64 %sh, i64 %pk
  %r2 = trunc i64 %z to i32
  ; same-width casts: i64 -> i64 value reused as an index, then masked
  %idx = add i64 %pk, %m
  %mk = and i64 %idx, 4095
  %r3 = trunc i64 %mk to i32
  %f0 = call float @air.convert.f.f32.u.i32(i32 %r0)
  %f1 = call float @air.convert.f.f32.u.i32(i32 %r1)
  %f2 = call float @air.convert.f.f32.u.i32(i32 %r2)
  %f3 = call float @air.convert.f.f32.u.i32(i32 %r3)
  %v0 = insertelement <4 x float> poison, float %f0, i64 0
  %v1 = insertelement <4 x float> %v0, float %f1, i64 1
  %v2 = insertelement <4 x float> %v1, float %f2, i64 2
  %v3 = insertelement <4 x float> %v2, float %f3, i64 3
  ret <4 x float> %v3
}

declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)

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
