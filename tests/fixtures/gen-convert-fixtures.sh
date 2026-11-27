#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures of the integer air.convert matrix (S07) and of the D2 NaN min/max candidate, executed by tests/air2spv-int-intrinsics-lavapipe.c (NOT Apple output):
#   rt_convert_a_fragment   int8 -> int32, uint8 -> uint32, int16 -> uint32, uint16 -> int32          (same-sign pairs do not distinguish the old and the new mapping; the cross-sign ones do)
#   rt_convert_b_fragment   int32 -> uint32 (same width), int32 -> uint32 low half, int64 -> int16, uint64 -> int16 (narrowing)
#   rt_convert_c_fragment   <2 x i32> -> <2 x i16> -> <2 x i32> with a signed and an unsigned widening (vector spelling v<N>i<W>)
# Inputs: X = pixel x (0..64), D = X - 32. The spelling air.convert.<dk>.<dt>.<sk>.<st> with integer kinds is extrapolated from the float<->int names of the corpus [N].
# Usage: tests/fixtures/gen-convert-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
HDR='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer
'
MD='!llvm.module.flags = !{!0}
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
!20 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}'
PRE='define <4 x float> @F(<4 x float> %0, <4 x float> %1) {
entry:
  %x = extractelement <4 x float> %0, i64 0
  %xi = call i32 @air.convert.s.i32.f.f32(float %x)
  %d = sub i32 %xi, 32'
OUT='  %v0 = insertelement <4 x float> zeroinitializer, float %o0, i64 0
  %v1 = insertelement <4 x float> %v0, float %o1, i64 1
  %v2 = insertelement <4 x float> %v1, float %o2, i64 2
  %v3 = insertelement <4 x float> %v2, float %o3, i64 3
  ret <4 x float> %v3
}
declare i32 @air.convert.s.i32.f.f32(float)
declare float @air.convert.f.f32.s.i32(i32)
declare float @air.convert.f.f32.u.i32(i32)'

{ echo "; HAND-WRITTEN IR (not Apple output): integer air.convert matrix A, see gen-convert-fixtures.sh [N]"; echo "$HDR"; echo "$PRE"; cat <<'EOF2'
  %s8 = call i8 @air.convert.s.i8.s.i32(i32 %d)
  %w0 = call i32 @air.convert.s.i32.s.i8(i8 %s8)
  %o0 = call float @air.convert.f.f32.s.i32(i32 %w0)
  %m4 = mul i32 %xi, 4
  %u8 = call i8 @air.convert.u.i8.u.i32(i32 %m4)
  %w1 = call i32 @air.convert.u.i32.u.i8(i8 %u8)
  %o1 = call float @air.convert.f.f32.u.i32(i32 %w1)
  %s16 = call i16 @air.convert.s.i16.s.i32(i32 %d)
  %w2 = call i32 @air.convert.u.i32.s.i16(i16 %s16)
  %h2 = lshr i32 %w2, 16
  %o2 = call float @air.convert.f.f32.u.i32(i32 %h2)
  %b3 = add i32 %xi, 32768
  %u16 = call i16 @air.convert.u.i16.u.i32(i32 %b3)
  %w3 = call i32 @air.convert.s.i32.u.i16(i16 %u16)
  %o3 = call float @air.convert.f.f32.s.i32(i32 %w3)
EOF2
echo "$OUT"; cat <<'EOF2'
declare i8 @air.convert.s.i8.s.i32(i32)
declare i32 @air.convert.s.i32.s.i8(i8)
declare i8 @air.convert.u.i8.u.i32(i32)
declare i32 @air.convert.u.i32.u.i8(i8)
declare i16 @air.convert.s.i16.s.i32(i32)
declare i32 @air.convert.u.i32.s.i16(i16)
declare i16 @air.convert.u.i16.u.i32(i32)
declare i32 @air.convert.s.i32.u.i16(i16)
EOF2
echo "$MD"; } | llvm-as -o rt_convert_a_fragment.air.bc -

{ echo "; HAND-WRITTEN IR (not Apple output): integer air.convert matrix B, see gen-convert-fixtures.sh [N]"; echo "$HDR"; echo "$PRE"; cat <<'EOF2'
  %w0 = call i32 @air.convert.u.i32.s.i32(i32 %d)
  %h0 = lshr i32 %w0, 16
  %o0 = call float @air.convert.f.f32.u.i32(i32 %h0)
  %l1 = and i32 %w0, 65535
  %o1 = call float @air.convert.f.f32.u.i32(i32 %l1)
  %e2 = sext i32 %d to i64
  %l2 = add i64 %e2, 4294967296
  %s2 = call i16 @air.convert.s.i16.s.i64(i64 %l2)
  %w2 = call i32 @air.convert.s.i32.s.i16(i16 %s2)
  %o2 = call float @air.convert.f.f32.s.i32(i32 %w2)
  %e3 = zext i32 %xi to i64
  %l3 = or i64 %e3, 1099511627776
  %s3 = call i16 @air.convert.s.i16.u.i64(i64 %l3)
  %w3 = call i32 @air.convert.s.i32.s.i16(i16 %s3)
  %o3 = call float @air.convert.f.f32.s.i32(i32 %w3)
EOF2
echo "$OUT"; cat <<'EOF2'
declare i32 @air.convert.u.i32.s.i32(i32)
declare i16 @air.convert.s.i16.s.i64(i64)
declare i32 @air.convert.s.i32.s.i16(i16)
declare i16 @air.convert.s.i16.u.i64(i64)
EOF2
echo "$MD"; } | llvm-as -o rt_convert_b_fragment.air.bc -

{ echo "; HAND-WRITTEN IR (not Apple output): integer air.convert matrix C (vectors), see gen-convert-fixtures.sh [N]"; echo "$HDR"; echo "$PRE"; cat <<'EOF2'
  %b = add i32 %xi, 32768
  %a0 = insertelement <2 x i32> zeroinitializer, i32 %d, i64 0
  %a = insertelement <2 x i32> %a0, i32 %b, i64 1
  %t = call <2 x i16> @air.convert.s.v2i16.s.v2i32(<2 x i32> %a)
  %r1 = call <2 x i32> @air.convert.u.v2i32.s.v2i16(<2 x i16> %t)
  %r2 = call <2 x i32> @air.convert.s.v2i32.u.v2i16(<2 x i16> %t)
  %r1a = extractelement <2 x i32> %r1, i64 0
  %r1b = extractelement <2 x i32> %r1, i64 1
  %r2a = extractelement <2 x i32> %r2, i64 0
  %r2b = extractelement <2 x i32> %r2, i64 1
  %h0 = lshr i32 %r1a, 16
  %h1 = lshr i32 %r1b, 16
  %o0 = call float @air.convert.f.f32.u.i32(i32 %h0)
  %o1 = call float @air.convert.f.f32.u.i32(i32 %h1)
  %o2 = call float @air.convert.f.f32.s.i32(i32 %r2a)
  %o3 = call float @air.convert.f.f32.s.i32(i32 %r2b)
EOF2
echo "$OUT"; cat <<'EOF2'
declare <2 x i16> @air.convert.s.v2i16.s.v2i32(<2 x i32>)
declare <2 x i32> @air.convert.u.v2i32.s.v2i16(<2 x i16>)
declare <2 x i32> @air.convert.s.v2i32.u.v2i16(<2 x i16>)
EOF2
echo "$MD"; } | llvm-as -o rt_convert_c_fragment.air.bc -

# ---- D2 NaN min / max candidate (tests/air2spv-int-intrinsics-lavapipe.c, mode "nanmax"): v = 0.25 * X (X = pixel x); NaN = the constant 0x7FF8000000000000 (o0 / o3 of a, o1 of b) or the runtime 0/0 (%nan = (x - x) / (x - x): LLVM cannot fold it without nnan) ----
#   rt_nanmax_a_fragment   fmax(NaN, v), fmax(v, NaN), fmin(NaN, v), fmin(v, NaN)
#   rt_nanmax_b_fragment   clamp(NaN, 0.5, 4), clamp(v, NaN, 4), clamp(v, 0.5, NaN), saturate(NaN)
NPRE='define <4 x float> @F(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %xf = fsub float %px, 0.5
  %v = fmul float %xf, 0.25
  %z = fsub float %xf, %xf
  %nan = fdiv float %z, %z'
{ echo "; HAND-WRITTEN IR (not Apple output): NaN operands of air.fmax / air.fmin [N: names as in the corpus, NaN semantics tested only on lavapipe]"; echo "$HDR"; echo "$NPRE"; cat <<'EOF2'
  %o0 = call float @air.fmax.f32(float 0x7FF8000000000000, float %v)
  %o1 = call float @air.fmax.f32(float %v, float %nan)
  %o2 = call float @air.fmin.f32(float %nan, float %v)
  %o3 = call float @air.fmin.f32(float %v, float 0x7FF8000000000000)
EOF2
echo "$OUT" | sed '/^declare/d'; cat <<'EOF2'
declare float @air.fmax.f32(float, float)
declare float @air.fmin.f32(float, float)
EOF2
echo "$MD"; } | llvm-as -o rt_nanmax_a_fragment.air.bc -
{ echo "; HAND-WRITTEN IR (not Apple output): NaN operands of air.clamp / air.saturate [N]"; echo "$HDR"; echo "$NPRE"; cat <<'EOF2'
  %o0 = call float @air.clamp.f32(float %nan, float 0.5, float 4.0)
  %o1 = call float @air.clamp.f32(float %v, float 0x7FF8000000000000, float 4.0)
  %o2 = call float @air.clamp.f32(float %v, float 0.5, float %nan)
  %o3 = call float @air.saturate.f32(float %nan)
EOF2
echo "$OUT" | sed '/^declare/d'; cat <<'EOF2'
declare float @air.clamp.f32(float, float, float)
declare float @air.saturate.f32(float)
EOF2
echo "$MD"; } | llvm-as -o rt_nanmax_b_fragment.air.bc -
