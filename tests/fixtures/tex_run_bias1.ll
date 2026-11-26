; HAND-WRITTEN AIR (not Apple output; assembled with llvm-as): executed on lavapipe by air2spv-texture-sample-lavapipe: bias 1.0 (constant) on an implicit-LOD sample of footprint 1 texel (8x8 target, uv = fragment position / 8, 8x8 texture with 4 mip levels)
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define <4 x float> @Bias1(<4 x float> %0, ptr addrspace(1) %1, ptr addrspace(2) %2) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %u = fmul float %px, 1.250000e-01
  %v = fmul float %py, 1.250000e-01
  %uv0 = insertelement <2 x float> zeroinitializer, float %u, i64 0
  %uv = insertelement <2 x float> %uv0, float %v, i64 1
  %d = fsub float %px, %px
  %one = fadd float %d, 1.000000e+00
  %one2 = fadd float %d, 2.000000e+00
  %r = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %1, ptr addrspace(2) %2, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 1.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %r, 0
  ret <4 x float> %c
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!llvm.module.flags = !{!0}
!air.version = !{!10}
!air.language_version = !{!11}
!air.fragment = !{!15}

!0 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 2, i32 8, i32 0}
!11 = !{!"Metal", i32 4, i32 0, i32 0}
!15 = !{ptr @Bias1, !16, !18}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !21, !22}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!21 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!22 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
