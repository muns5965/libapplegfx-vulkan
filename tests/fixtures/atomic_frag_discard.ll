; HAND-WRITTEN IR (not Apple output): MSL  fragment float4 F(float4 pos [[position]], device atomic_uint *buf [[buffer(0)]]) { if (pos.x < 4.0) discard_fragment(); atomic_fetch_add_explicit(buf, 1, relaxed); ((device uint *)buf)[1 + y*8 + x] = 7; return red; }
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare void @air.discard_fragment()
define <4 x float> @F(<4 x float> %pos, ptr addrspace(1) %buf) {
entry:
  %px = extractelement <4 x float> %pos, i64 0
  %py = extractelement <4 x float> %pos, i64 1
  %c = fcmp olt float %px, 4.0
  br i1 %c, label %d, label %k
d:
  call void @air.discard_fragment()
  br label %k
k:
  %p0 = getelementptr i32, ptr addrspace(1) %buf, i64 0
  %old = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 1, i32 0, i32 2, i32 0, i1 true)
  %xi = fptoui float %px to i32
  %yi = fptoui float %py to i32
  %y8 = mul i32 %yi, 8
  %i0 = add i32 %y8, %xi
  %i1 = add i32 %i0, 1
  %i64 = zext i32 %i1 to i64
  %pw = getelementptr i32, ptr addrspace(1) %buf, i64 %i64
  store i32 7, ptr addrspace(1) %pw
  ret <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0>
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
!20 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"buf"}
