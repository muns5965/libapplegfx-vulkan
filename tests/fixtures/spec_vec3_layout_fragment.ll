; HAND-WRITTEN IR (not Apple output): struct { float3, float } under the AIR data layout (v96:128:128): LLVM puts member 1 at byte 16, size 32.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

%S = type { <3 x float>, float }
define <4 x float> @F(<4 x float> %0, ptr addrspace(2) nocapture noundef readonly align 16 dereferenceable(32) "air-buffer-no-alias" %1) {
entry:
  %p = getelementptr inbounds %S, ptr addrspace(2) %1, i32 0, i32 1
  %x = load float, ptr addrspace(2) %p, align 16
  %v = insertelement <4 x float> zeroinitializer, float %x, i64 0
  ret <4 x float> %v
}
!llvm.module.flags = !{!0}
!air.version = !{!9}
!air.language_version = !{!11}
!air.fragment = !{!15}
!0 = !{i32 1, !"wchar_size", i32 4}
!9 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 1, i32 2, i32 0}
!15 = !{ptr @F, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !20}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!20 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"S", !"air.arg_name", !"s"}
