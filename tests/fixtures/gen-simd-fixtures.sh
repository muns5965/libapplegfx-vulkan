#!/bin/sh
# Regenerates the HAND-WRITTEN AIR compute fixtures of tests/air2spv-simd-semantics-lavapipe.c with llvm-as (NOT Apple output):
#   simd_semantics.air.bc   tile kernel. Per thread (tid, lane, size, simdgroup index / count) and per workgroup g (threadgroup_position_in_grid.x, 0..47: it selects the
#                           operands p0 = 1 + 2*(g&1), d = {1, size/2, size-1, size}[(g>>1)&3], mask = {1, size/2, size-1}[(g>>3)%3], p3 = (g/24)&1) with v = tid*7 + 3:
#                             w0 lane  w1 size  w2 simdgroup index  w3 simdgroup count
#                             w4 simd_shuffle(v, (lane+p0)&(size-1))   w5 simd_shuffle_down(v, d)   w6 simd_shuffle_up(v, d)   w7 simd_shuffle_xor(v, mask)
#                             w8 simd_shuffle_and_fill_down(v, v+100, d, size>>p3)   w9 simd_shuffle_and_fill_up(v, v+100, d, size>>p3)
#                             w10 ushort simd_shuffle(v)   w11 half simd_shuffle_down(v & 1023, d)   w12/w13 uint2 simd_shuffle((v, v+100))   w14/w15 ulong simd_shuffle((v+100)<<32 | v)
#                           and stores word 4j+c as channel c of the Rgba16f imageblock texel (x, y + j*group_height) (every value < 2048: exact in half).
#   simd_unsup_sum.air.bc     tile kernel storing air.simd_sum.f32(float(tid)) (name / signature ASSUMED, not seen in Apple output): an operation the translator does not implement
#   simd_unsup_ballot.air.bc  tile kernel storing the low word of air.simd_ballot(tid & 1) (name / signature ASSUMED): ditto
#   simd_unsup_frag.air.bc    FRAGMENT function (i1const_fragment.ll's interface) returning air.quad_sum.f32(color.x) (name / signature ASSUMED): a graphics stage still builds, the call is counted
#   imageblock_rmw.air.bc     tile kernel: store (t, t+1, t+2, t+3) with t = tid into the imageblock, air.load.implicit_imageblock it back (signature ASSUMED as the translator reads it:
#                             rt, coord, sample, ?), add 1 to every channel, store again
# The kernels take only [[imageblock]] + thread-position attributes: llvm-as emits opaque pointers, which the translator's buffer handling (typed-pointer Apple AIR) cannot use.
# Usage: tests/fixtures/gen-simd-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
HDR='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }'
FTR='!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 7, i32 0}
!2 = !{!"Metal", i32 3, i32 2, i32 0}
!3 = !{}
!9 = !{i32 0, i32 8, i32 0, !"half4", !"color", !"air.render_target", i32 0}
!10 = !{i32 0, !"air.imageblock", !"implicit", !"air.struct_type_info", !9, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"imageblock<ColorData, layout_implicit>", !"air.arg_name", !"block"}
!11 = !{i32 1, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!12 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!13 = !{i32 3, !"air.thread_index_in_simdgroup", !"air.arg_type_name", !"ushort", !"air.arg_name", !"lane"}
!14 = !{i32 4, !"air.threads_per_simdgroup", !"air.arg_type_name", !"ushort", !"air.arg_name", !"size"}
!15 = !{i32 5, !"air.simdgroup_index_in_threadgroup", !"air.arg_type_name", !"ushort", !"air.arg_name", !"sgi"}
!16 = !{i32 6, !"air.simdgroups_per_threadgroup", !"air.arg_type_name", !"ushort", !"air.arg_name", !"nsg"}
!17 = !{i32 7, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"pos"}
!18 = !{i32 8, !"air.threads_per_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tgsize"}
!19 = !{!10, !11, !12, !13, !14, !15, !16, !17, !18}'
ARGS='%"struct.metal::_imageblock_base" %blk, i32 %gid, i32 %tid, i16 %lane, i16 %size, i16 %sgi, i16 %nsg, <2 x i16> %pos, <2 x i16> %tgsize'

emit() {   # $1 = file stem; stdin = declarations + function
  { echo "$HDR"; cat; echo "$FTR"; echo '!air.kernel = !{!20}'; echo "!20 = !{ptr @$1, !3, !19}"; } > /tmp/$1.$$.ll
  llvm-as /tmp/$1.$$.ll -o $1.air.bc
  rm -f /tmp/$1.$$.ll
}

# one texel store: $1 = j, $2..$5 = four half values
store_texel() {
  j=$1; shift
  echo "  %t${j}a = insertelement <4 x half> undef, half $1, i64 0"
  echo "  %t${j}b = insertelement <4 x half> %t${j}a, half $2, i64 1"
  echo "  %t${j}c = insertelement <4 x half> %t${j}b, half $3, i64 2"
  echo "  %t${j}d = insertelement <4 x half> %t${j}c, half $4, i64 3"
  echo "  %jy${j} = mul i16 %ly, $j"
  echo "  %yy${j} = add i16 %py, %jy${j}"
  echo "  %c${j}a = insertelement <2 x i16> undef, i16 %px, i64 0"
  echo "  %c${j} = insertelement <2 x i16> %c${j}a, i16 %yy${j}, i64 1"
  echo "  call void @air.store.implicit_imageblock.v4f16(<4 x half> %t${j}d, i32 0, <2 x i16> %c${j}, i32 0, i16 0)"
}

{
cat <<'EOT'
declare i32 @air.simd_shuffle.i32(i32, i16)
declare i32 @air.simd_shuffle_down.i32(i32, i16)
declare i32 @air.simd_shuffle_up.i32(i32, i16)
declare i32 @air.simd_shuffle_xor.i32(i32, i16)
declare i32 @air.simd_shuffle_and_fill_down.i32(i32, i32, i16, i16)
declare i32 @air.simd_shuffle_and_fill_up.i32(i32, i32, i16, i16)
declare i16 @air.simd_shuffle.i16(i16, i16)
declare half @air.simd_shuffle_down.f16(half, i16)
declare <2 x i32> @air.simd_shuffle.v2i32(<2 x i32>, i16)
declare i64 @air.simd_shuffle.i64(i64, i16)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16)

define void @simd_semantics(%"struct.metal::_imageblock_base" %blk, i32 %gid, i32 %tid, i16 %lane, i16 %size, i16 %sgi, i16 %nsg, <2 x i16> %pos, <2 x i16> %tgsize) {
entry:
  %v0 = mul i32 %tid, 7
  %v = add i32 %v0, 3
  %vf = add i32 %v, 100
  ; operands from the workgroup index
  %ga = and i32 %gid, 1
  %ga2 = mul i32 %ga, 2
  %p0 = add i32 %ga2, 1
  %gb = lshr i32 %gid, 1
  %dsel = and i32 %gb, 3
  %gc = lshr i32 %gid, 3
  %msel = urem i32 %gc, 3
  %gd = udiv i32 %gid, 24
  %p3 = and i32 %gd, 1
  %size32 = zext i16 %size to i32
  %half32 = lshr i32 %size32, 1
  %szm1_32 = sub i32 %size32, 1
  %dis0 = icmp eq i32 %dsel, 0
  %dis1 = icmp eq i32 %dsel, 1
  %dis2 = icmp eq i32 %dsel, 2
  %d2 = select i1 %dis2, i32 %szm1_32, i32 %size32
  %d1 = select i1 %dis1, i32 %half32, i32 %d2
  %d32 = select i1 %dis0, i32 1, i32 %d1
  %mis0 = icmp eq i32 %msel, 0
  %mis1 = icmp eq i32 %msel, 1
  %m1 = select i1 %mis1, i32 %half32, i32 %szm1_32
  %mask32 = select i1 %mis0, i32 1, i32 %m1
  %p0s = trunc i32 %p0 to i16
  %ds = trunc i32 %d32 to i16
  %masks = trunc i32 %mask32 to i16
  %p3s = trunc i32 %p3 to i16
  %lane32 = zext i16 %lane to i32
  %sgi32 = zext i16 %sgi to i32
  %nsg32 = zext i16 %nsg to i32
  %szm1 = sub i16 %size, 1
  %la = add i16 %lane, %p0s
  %idx = and i16 %la, %szm1
  %r4 = call i32 @air.simd_shuffle.i32(i32 %v, i16 %idx)
  %r5 = call i32 @air.simd_shuffle_down.i32(i32 %v, i16 %ds)
  %r6 = call i32 @air.simd_shuffle_up.i32(i32 %v, i16 %ds)
  %r7 = call i32 @air.simd_shuffle_xor.i32(i32 %v, i16 %masks)
  %mod = lshr i16 %size, %p3s
  %r8 = call i32 @air.simd_shuffle_and_fill_down.i32(i32 %v, i32 %vf, i16 %ds, i16 %mod)
  %r9 = call i32 @air.simd_shuffle_and_fill_up.i32(i32 %v, i32 %vf, i16 %ds, i16 %mod)
  %v16 = trunc i32 %v to i16
  %r10s = call i16 @air.simd_shuffle.i16(i16 %v16, i16 %idx)
  %r10 = zext i16 %r10s to i32
  %vm = and i32 %v, 1023
  %vh = uitofp i32 %vm to half
  %r11h = call half @air.simd_shuffle_down.f16(half %vh, i16 %ds)
  %u2a = insertelement <2 x i32> undef, i32 %v, i64 0
  %u2 = insertelement <2 x i32> %u2a, i32 %vf, i64 1
  %r12v = call <2 x i32> @air.simd_shuffle.v2i32(<2 x i32> %u2, i16 %idx)
  %r12 = extractelement <2 x i32> %r12v, i64 0
  %r13 = extractelement <2 x i32> %r12v, i64 1
  %vlo = zext i32 %v to i64
  %vhi = zext i32 %vf to i64
  %vhis = shl i64 %vhi, 32
  %v64 = or i64 %vhis, %vlo
  %r14v = call i64 @air.simd_shuffle.i64(i64 %v64, i16 %idx)
  %r14 = trunc i64 %r14v to i32
  %r15s = lshr i64 %r14v, 32
  %r15 = trunc i64 %r15s to i32
  %px = extractelement <2 x i16> %pos, i64 0
  %py = extractelement <2 x i16> %pos, i64 1
  %ly = extractelement <2 x i16> %tgsize, i64 1
EOT
for r in lane32 size32 sgi32 nsg32 r4 r5 r6 r7 r8 r9 r10 r12 r13 r14 r15; do
  echo "  %h_$r = uitofp i32 %$r to half"
done
store_texel 0 %h_lane32 %h_size32 %h_sgi32 %h_nsg32
store_texel 1 %h_r4 %h_r5 %h_r6 %h_r7
store_texel 2 %h_r8 %h_r9 %h_r10 %r11h
store_texel 3 %h_r12 %h_r13 %h_r14 %h_r15
echo "  ret void"
echo "}"
} | emit simd_semantics

emit simd_unsup_sum <<'EOT'
declare float @air.simd_sum.f32(float)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16)
define void @simd_unsup_sum(%"struct.metal::_imageblock_base" %blk, i32 %gid, i32 %tid, i16 %lane, i16 %size, i16 %sgi, i16 %nsg, <2 x i16> %pos, <2 x i16> %tgsize) {
entry:
  %f = uitofp i32 %tid to float
  %s = call float @air.simd_sum.f32(float %f)
  %h = fptrunc float %s to half
  %a = insertelement <4 x half> undef, half %h, i64 0
  %b = insertelement <4 x half> %a, half %h, i64 1
  %c = insertelement <4 x half> %b, half %h, i64 2
  %d = insertelement <4 x half> %c, half %h, i64 3
  call void @air.store.implicit_imageblock.v4f16(<4 x half> %d, i32 0, <2 x i16> %pos, i32 0, i16 0)
  ret void
}
EOT

emit simd_unsup_ballot <<'EOT'
declare i64 @air.simd_ballot(i1)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16)
define void @simd_unsup_ballot(%"struct.metal::_imageblock_base" %blk, i32 %gid, i32 %tid, i16 %lane, i16 %size, i16 %sgi, i16 %nsg, <2 x i16> %pos, <2 x i16> %tgsize) {
entry:
  %b0 = and i32 %tid, 1
  %b = icmp ne i32 %b0, 0
  %m = call i64 @air.simd_ballot(i1 %b)
  %r = trunc i64 %m to i32
  %f = uitofp i32 %r to half
  %a = insertelement <4 x half> undef, half %f, i64 0
  %b2 = insertelement <4 x half> %a, half %f, i64 1
  %c = insertelement <4 x half> %b2, half %f, i64 2
  %d = insertelement <4 x half> %c, half %f, i64 3
  call void @air.store.implicit_imageblock.v4f16(<4 x half> %d, i32 0, <2 x i16> %pos, i32 0, i16 0)
  ret void
}
EOT
emit imageblock_rmw <<'EOT'
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16)
declare <4 x half> @air.load.implicit_imageblock.v4f16(i32, <2 x i16>, i32, i16)
define void @imageblock_rmw(%"struct.metal::_imageblock_base" %blk, i32 %gid, i32 %tid, i16 %lane, i16 %size, i16 %sgi, i16 %nsg, <2 x i16> %pos, <2 x i16> %tgsize) {
entry:
  %t0 = uitofp i32 %tid to half
  %t1 = fadd half %t0, 1.0
  %t2 = fadd half %t0, 2.0
  %t3 = fadd half %t0, 3.0
  %a = insertelement <4 x half> undef, half %t0, i64 0
  %b = insertelement <4 x half> %a, half %t1, i64 1
  %c = insertelement <4 x half> %b, half %t2, i64 2
  %d = insertelement <4 x half> %c, half %t3, i64 3
  call void @air.store.implicit_imageblock.v4f16(<4 x half> %d, i32 0, <2 x i16> %pos, i32 0, i16 0)
  %ld = call <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %pos, i32 0, i16 0)
  %r = fadd <4 x half> %ld, <half 1.0, half 1.0, half 1.0, half 1.0>
  call void @air.store.implicit_imageblock.v4f16(<4 x half> %r, i32 0, <2 x i16> %pos, i32 0, i16 0)
  ret void
}
EOT

{
  sed -n '1,/^define <4 x float> @I1ConstFragment/{/^define <4 x float> @I1ConstFragment/!p}' i1const_fragment.ll
  cat <<'EOT'
define <4 x float> @SimdUnsupFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %x = extractelement <4 x float> %1, i64 0
  %q = call float @air.quad_sum.f32(float %x)
  %v0 = insertelement <4 x float> poison, float %q, i64 0
  %v1 = insertelement <4 x float> %v0, float %q, i64 1
  %v2 = insertelement <4 x float> %v1, float %q, i64 2
  %v3 = insertelement <4 x float> %v2, float 1.0, i64 3
  ret <4 x float> %v3
}

declare float @air.quad_sum.f32(float)

EOT
  sed -n '/^!llvm.module.flags/,$p' i1const_fragment.ll | sed 's/@I1ConstFragment/@SimdUnsupFragment/'
} > /tmp/simd_unsup_frag.$$.ll
llvm-as /tmp/simd_unsup_frag.$$.ll -o simd_unsup_frag.air.bc
rm -f /tmp/simd_unsup_frag.$$.ll
echo "wrote simd_semantics.air.bc simd_unsup_sum.air.bc simd_unsup_ballot.air.bc simd_unsup_frag.air.bc imageblock_rmw.air.bc"
