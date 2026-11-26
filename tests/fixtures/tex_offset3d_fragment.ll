; HAND-WRITTEN AIR (not Apple output; assembled with llvm-as): 3D forms (2D-slice approximation with offset z dropped); operand order (tex, samp, coord, [array idx,] i1 has_offset, <N x i32> offset, i1 lod_is_level, float lod_or_bias, float min_lod, i32 flags)
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define <4 x float> @Tex3dFragment(<4 x float> %0, <2 x float> %1, ptr addrspace(1) %2, ptr addrspace(2) %3) {
entry:
  %ux = extractelement <2 x float> %1, i64 0
  %uy = extractelement <2 x float> %1, i64 1
  %uv30 = insertelement <3 x float> zeroinitializer, float %ux, i64 0
  %uv31 = insertelement <3 x float> %uv30, float %uy, i64 1
  %uv3 = insertelement <3 x float> %uv31, float 5.000000e-01, i64 2
  %c1 = call { <4 x half>, i8 } @air.sample_texture_3d.v4f16(ptr addrspace(1) %2, ptr addrspace(2) %3, <3 x float> %uv3, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c1v = extractvalue { <4 x half>, i8 } %c1, 0
  %c1c = fpext <4 x half> %c1v to <4 x float>
  %c2 = call { <4 x half>, i8 } @air.sample_texture_3d.v4f16(ptr addrspace(1) %2, ptr addrspace(2) %3, <3 x float> %uv3, i1 true, <3 x i32> <i32 1, i32 2, i32 3>, i1 false, float %ux, float 0.000000e+00, i32 0)
  %c2v = extractvalue { <4 x half>, i8 } %c2, 0
  %c2c = fpext <4 x half> %c2v to <4 x float>
  %c3 = call { <4 x half>, i8 } @air.sample_texture_3d.v4f16(ptr addrspace(1) %2, ptr addrspace(2) %3, <3 x float> %uv3, i1 true, <3 x i32> <i32 -1, i32 1, i32 0>, i1 true, float 1.000000e+00, float 0.000000e+00, i32 0)
  %c3v = extractvalue { <4 x half>, i8 } %c3, 0
  %c3c = fpext <4 x half> %c3v to <4 x float>
  %s1 = fadd <4 x float> %c1c, %c2c
  %s2 = fadd <4 x float> %s1, %c3c
  ret <4 x float> %s2
}

declare { <4 x half>, i8 } @air.sample_texture_3d.v4f16(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <3 x float>, i1, <3 x i32>, i1, float, float, i32)

!llvm.module.flags = !{!0}
!air.version = !{!10}
!air.language_version = !{!11}
!air.fragment = !{!15}

!0 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 2, i32 8, i32 0}
!11 = !{!"Metal", i32 4, i32 0, i32 0}
!15 = !{ptr @Tex3dFragment, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !20, !21, !22}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!20 = !{i32 1, !"air.fragment_input", !"generated(2uvDv2_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!21 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture3d<float, sample>", !"air.arg_name", !"tex"}
!22 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
