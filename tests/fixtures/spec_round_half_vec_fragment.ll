; HAND-WRITTEN IR (not Apple output): air.round on <3 x half> (validity of the vector / half emulation).
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer

define <4 x float> @F(<4 x float> %0) {
entry:
  %s = shufflevector <4 x float> %0, <4 x float> undef, <3 x i32> <i32 0, i32 1, i32 2>
  %h = call <3 x half> @air.convert.f.v3f16.f.v3f32(<3 x float> %s)
  %r = call <3 x half> @air.round.v3f16(<3 x half> %h)
  %f = call <3 x float> @air.convert.f.v3f32.f.v3f16(<3 x half> %r)
  %o = shufflevector <3 x float> %f, <3 x float> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 2>
  ret <4 x float> %o
}
declare <3 x half> @air.convert.f.v3f16.f.v3f32(<3 x float>)
declare <3 x half> @air.round.v3f16(<3 x half>)
declare <3 x float> @air.convert.f.v3f32.f.v3f16(<3 x half>)
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
!18 = !{!19}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
