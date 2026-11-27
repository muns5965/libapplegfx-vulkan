; HAND-WRITTEN IR (not Apple output): vertex function with [[vertex_id]] and [[instance_id]] (both uint).
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <{ <4 x float> }> @F(i32 %0, i32 %1) {
entry:
  %f = uitofp i32 %1 to float
  %v = insertelement <4 x float> zeroinitializer, float %f, i64 0
  %r = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %r
}
!llvm.module.flags = !{!0}
!air.version = !{!9}
!air.language_version = !{!11}
!air.vertex = !{!15}
!0 = !{i32 1, !"wchar_size", i32 4}
!9 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!15 = !{ptr @F, !16, !18}
!16 = !{!17}
!17 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!18 = !{!19, !20}
!19 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!20 = !{i32 1, !"air.instance_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"iid"}
