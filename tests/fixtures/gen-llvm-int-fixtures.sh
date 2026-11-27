#!/bin/sh
# Regenerates the HAND-WRITTEN AIR fixtures rt_llvm_*_fragment.{ll,air.bc} of tests/air2spv-int-intrinsics-lavapipe.c (LLVM integer / fused-multiply-add intrinsics, integ7b).
# NOT Apple output: the call names are the LLVM LangRef ones (`llvm.umax.i32`, `llvm.ctlz.i32(i32, i1)`, `llvm.fshl.i32`, `llvm.fma.f32` ...); the module skeleton and metadata are copied from
# rt_minmax_fragment.ll (Apple Metal 32023.622 style, fragment function IntOpsFragment, one float4 render target). Inputs per pixel (x, y) = the pixel coordinates (position - 0.5):
#   gen_in(X, w) = (X << (w-6)) | (X odd ? (1 << (w-6)) - 1 : 0), truncated to w bits  -> the 65 values per axis contain 0, MIN (X=32), MAX (X=31), -1 (X=63) of every width
# Usage: tests/fixtures/gen-llvm-int-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
FOOT=$(sed -n '/^!llvm.module.flags/,$p' rt_minmax_fragment.ll)
HEAD='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"

@llvm.global_ctors = appending global [0 x { i32, ptr, ptr }] zeroinitializer
'
PRE='define <4 x float> @IntOpsFragment(<4 x float> %0, <4 x float> %1) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %X = call i32 @air.convert.s.i32.f.f32(float %px)
  %py = extractelement <4 x float> %0, i64 1
  %Y = call i32 @air.convert.s.i32.f.f32(float %py)'
# genin NAME W SRC: %NAME = gen_in(SRC, W)
genin() { n=$1; w=$2; s=$3; sh=$((w-6)); m=$(( (1 << sh) - 1 ))
  if [ "$w" = 64 ]; then
    echo "  %$n.x = zext i32 $s to i64"; echo "  %$n.t = shl i64 %$n.x, $sh"; echo "  %$n.o = and i64 %$n.x, 1"; echo "  %$n.m = mul i64 %$n.o, $m"; echo "  %$n = or i64 %$n.t, %$n.m"
  else
    echo "  %$n.t = shl i32 $s, $sh"; echo "  %$n.o = and i32 $s, 1"; echo "  %$n.m = mul i32 %$n.o, $m"
    if [ "$w" = 32 ]; then echo "  %$n = or i32 %$n.t, %$n.m"; else echo "  %$n.w = or i32 %$n.t, %$n.m"; echo "  %$n = trunc i32 %$n.w to i$w"; fi
  fi; }
# tofl NAME W: %NAME.f = float(zext NAME) for W <= 16
tofl() { echo "  %$1.z = zext i$2 %$1 to i32"; echo "  %$1.f = call float @air.convert.f.f32.u.i32(i32 %$1.z)"; }
# tofl32 NAME: %NAME.hf / %NAME.lf = float(high 16 bits) / float(low 16 bits) of an i32
tofl32() { echo "  %$1.h = lshr i32 %$1, 16"; echo "  %$1.l = and i32 %$1, 65535"; echo "  %$1.hf = call float @air.convert.f.f32.u.i32(i32 %$1.h)"; echo "  %$1.lf = call float @air.convert.f.f32.u.i32(i32 %$1.l)"; }
# fold64 NAME: %NAME.f = float((r ^ r>>16 ^ r>>32 ^ r>>48) & 0xffff)
fold64() { n=$1; echo "  %$n.s1 = lshr i64 %$n, 16"; echo "  %$n.s2 = lshr i64 %$n, 32"; echo "  %$n.s3 = lshr i64 %$n, 48"; echo "  %$n.x1 = xor i64 %$n, %$n.s1"; echo "  %$n.x2 = xor i64 %$n.x1, %$n.s2"
  echo "  %$n.x3 = xor i64 %$n.x2, %$n.s3"; echo "  %$n.tt = trunc i64 %$n.x3 to i32"; echo "  %$n.fz = and i32 %$n.tt, 65535"; echo "  %$n.f = call float @air.convert.f.f32.u.i32(i32 %$n.fz)"; }
# fold32 NAME: %NAME.f = float((r >> 16) ^ (r & 0xffff))
fold32() { n=$1; echo "  %$n.fh = lshr i32 %$n, 16"; echo "  %$n.fl = and i32 %$n, 65535"; echo "  %$n.fx = xor i32 %$n.fh, %$n.fl"; echo "  %$n.f = call float @air.convert.f.f32.u.i32(i32 %$n.fx)"; }
# vec2 NAME A B: <2 x iW> %NAME = <A, B> ; vecN is written inline where needed
RET() { echo "  %v0 = insertelement <4 x float> poison, float $1, i64 0"; echo "  %v1 = insertelement <4 x float> %v0, float $2, i64 1"; echo "  %v2 = insertelement <4 x float> %v1, float $3, i64 2"
  echo "  %v3 = insertelement <4 x float> %v2, float $4, i64 3"; echo "  ret <4 x float> %v3"; echo "}"; }
BASEDECL='declare float @air.convert.f.f32.u.i32(i32)
declare i32 @air.convert.s.i32.f.f32(float)
declare float @air.convert.f.f32.s.i32(i32)'
# finish NAME BODY DECLS
finish() { { printf '; ModuleID = '"'"'rt_llvm_%s_fragment'"'"'\n%s\n%s\n%s\n' "$1" "$HEAD" "$PRE" "$2"; printf '\n%s\n%s\n\n\n%s\n' "$BASEDECL" "$3" "$FOOT"; } > "rt_llvm_$1_fragment.ll"
  llvm-as "rt_llvm_$1_fragment.ll" -o "rt_llvm_$1_fragment.air.bc"; }
# fixture list ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# umax: i8, i16, i32 (high / low half)
B="$(genin a8 8 %X; genin b8 8 %Y; genin a16 16 %X; genin b16 16 %Y; genin a32 32 %X; genin b32 32 %Y
echo '  %r0 = call i8 @llvm.umax.i8(i8 %a8, i8 %b8)'; echo '  %r1 = call i16 @llvm.umax.i16(i16 %a16, i16 %b16)'; echo '  %r2 = call i32 @llvm.umax.i32(i32 %a32, i32 %b32)'
tofl r0 8; tofl r1 16; tofl32 r2; RET %r0.f %r1.f %r2.hf %r2.lf)"
finish umax "$B" 'declare i8 @llvm.umax.i8(i8, i8)
declare i16 @llvm.umax.i16(i16, i16)
declare i32 @llvm.umax.i32(i32, i32)'
# umin i8, smin i16, smin i32 (hi / lo)
B="$(genin a8 8 %X; genin b8 8 %Y; genin a16 16 %X; genin b16 16 %Y; genin a32 32 %X; genin b32 32 %Y
echo '  %r0 = call i8 @llvm.umin.i8(i8 %a8, i8 %b8)'; echo '  %r1 = call i16 @llvm.smin.i16(i16 %a16, i16 %b16)'; echo '  %r2 = call i32 @llvm.smin.i32(i32 %a32, i32 %b32)'
tofl r0 8; tofl r1 16; tofl32 r2; RET %r0.f %r1.f %r2.hf %r2.lf)"
finish uminsmin "$B" 'declare i8 @llvm.umin.i8(i8, i8)
declare i16 @llvm.smin.i16(i16, i16)
declare i32 @llvm.smin.i32(i32, i32)'
# smax i8, umax i64, smin i64, smax i64 folded
B="$(genin a8 8 %X; genin b8 8 %Y; genin a64 64 %X; genin b64 64 %Y
echo '  %r0 = call i8 @llvm.smax.i8(i8 %a8, i8 %b8)'; echo '  %r1 = call i64 @llvm.umax.i64(i64 %a64, i64 %b64)'; echo '  %r2 = call i64 @llvm.smin.i64(i64 %a64, i64 %b64)'; echo '  %r3 = call i64 @llvm.smax.i64(i64 %a64, i64 %b64)'
tofl r0 8; fold64 r1; fold64 r2; fold64 r3; RET %r0.f %r1.f %r2.f %r3.f)"
finish minmax64 "$B" 'declare i8 @llvm.smax.i8(i8, i8)
declare i64 @llvm.umax.i64(i64, i64)
declare i64 @llvm.smin.i64(i64, i64)
declare i64 @llvm.smax.i64(i64, i64)'
# vectors: smax.v4i8 lane 2, umin.v3i16 lane 1, umax.v4i32 lane 3 (fold), smin.v2i64 lane 1 (fold). Operand vectors mix x and y per lane: lane k of A = a ^ (k * 0x55), of B = b + k
B="$(genin a8 8 %X; genin b8 8 %Y; genin a16 16 %X; genin b16 16 %Y; genin a32 32 %X; genin b32 32 %Y; genin a64 64 %X; genin b64 64 %Y
for k in 0 1 2 3; do echo "  %a8x$k = xor i8 %a8, $((k * 85))"; echo "  %b8x$k = add i8 %b8, $k"; done
echo '  %va80 = insertelement <4 x i8> poison, i8 %a8x0, i64 0'; echo '  %va81 = insertelement <4 x i8> %va80, i8 %a8x1, i64 1'; echo '  %va82 = insertelement <4 x i8> %va81, i8 %a8x2, i64 2'; echo '  %va8 = insertelement <4 x i8> %va82, i8 %a8x3, i64 3'
echo '  %vb80 = insertelement <4 x i8> poison, i8 %b8x0, i64 0'; echo '  %vb81 = insertelement <4 x i8> %vb80, i8 %b8x1, i64 1'; echo '  %vb82 = insertelement <4 x i8> %vb81, i8 %b8x2, i64 2'; echo '  %vb8 = insertelement <4 x i8> %vb82, i8 %b8x3, i64 3'
echo '  %vr0 = call <4 x i8> @llvm.smax.v4i8(<4 x i8> %va8, <4 x i8> %vb8)'; echo '  %r0 = extractelement <4 x i8> %vr0, i64 2'
for k in 0 1 2; do echo "  %a16x$k = xor i16 %a16, $((k * 4369))"; echo "  %b16x$k = add i16 %b16, $k"; done
echo '  %va160 = insertelement <3 x i16> poison, i16 %a16x0, i64 0'; echo '  %va161 = insertelement <3 x i16> %va160, i16 %a16x1, i64 1'; echo '  %va16 = insertelement <3 x i16> %va161, i16 %a16x2, i64 2'
echo '  %vb160 = insertelement <3 x i16> poison, i16 %b16x0, i64 0'; echo '  %vb161 = insertelement <3 x i16> %vb160, i16 %b16x1, i64 1'; echo '  %vb16 = insertelement <3 x i16> %vb161, i16 %b16x2, i64 2'
echo '  %vr1 = call <3 x i16> @llvm.umin.v3i16(<3 x i16> %va16, <3 x i16> %vb16)'; echo '  %r1 = extractelement <3 x i16> %vr1, i64 1'
for k in 0 1 2 3; do echo "  %a32x$k = xor i32 %a32, $((k * 1431655765))"; echo "  %b32x$k = add i32 %b32, $k"; done
echo '  %va320 = insertelement <4 x i32> poison, i32 %a32x0, i64 0'; echo '  %va321 = insertelement <4 x i32> %va320, i32 %a32x1, i64 1'; echo '  %va322 = insertelement <4 x i32> %va321, i32 %a32x2, i64 2'; echo '  %va32 = insertelement <4 x i32> %va322, i32 %a32x3, i64 3'
echo '  %vb320 = insertelement <4 x i32> poison, i32 %b32x0, i64 0'; echo '  %vb321 = insertelement <4 x i32> %vb320, i32 %b32x1, i64 1'; echo '  %vb322 = insertelement <4 x i32> %vb321, i32 %b32x2, i64 2'; echo '  %vb32 = insertelement <4 x i32> %vb322, i32 %b32x3, i64 3'
echo '  %vr2 = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %va32, <4 x i32> %vb32)'; echo '  %r2 = extractelement <4 x i32> %vr2, i64 3'
echo '  %a64x = xor i64 %a64, 6148914691236517205'; echo '  %va640 = insertelement <2 x i64> poison, i64 %a64, i64 0'; echo '  %va64 = insertelement <2 x i64> %va640, i64 %a64x, i64 1'
echo '  %vb640 = insertelement <2 x i64> poison, i64 %b64, i64 0'; echo '  %vb64 = insertelement <2 x i64> %vb640, i64 %b64, i64 1'
echo '  %vr3 = call <2 x i64> @llvm.smin.v2i64(<2 x i64> %va64, <2 x i64> %vb64)'; echo '  %r3 = extractelement <2 x i64> %vr3, i64 1'
tofl r0 8; tofl r1 16; fold32 r2; fold64 r3; RET %r0.f %r1.f %r2.f %r3.f)"
finish minmaxvec "$B" 'declare <4 x i8> @llvm.smax.v4i8(<4 x i8>, <4 x i8>)
declare <3 x i16> @llvm.umin.v3i16(<3 x i16>, <3 x i16>)
declare <4 x i32> @llvm.umax.v4i32(<4 x i32>, <4 x i32>)
declare <2 x i64> @llvm.smin.v2i64(<2 x i64>, <2 x i64>)'
# abs (flag false): i8, i16, i32 hi / lo of the signed inputs (MIN stays MIN)
B="$(genin a8 8 %Y; genin a16 16 %X; genin a32 32 %X
echo '  %r0 = call i8 @llvm.abs.i8(i8 %a8, i1 false)'; echo '  %r1 = call i16 @llvm.abs.i16(i16 %a16, i1 false)'; echo '  %r2 = call i32 @llvm.abs.i32(i32 %a32, i1 false)'
tofl r0 8; tofl r1 16; tofl32 r2; RET %r0.f %r1.f %r2.hf %r2.lf)"
finish abs "$B" 'declare i8 @llvm.abs.i8(i8, i1)
declare i16 @llvm.abs.i16(i16, i1)
declare i32 @llvm.abs.i32(i32, i1)'
# bit counting on single-bit / three-bit patterns (as rt_bits32): ctlz(A,false), cttz(A,false), ctpop(B), bitreverse(A)
B="$(echo '  %is0 = icmp eq i32 %X, 0'; echo '  %sh = sub i32 %X, 1'; echo '  %shc = select i1 %is0, i32 0, i32 %sh'; echo '  %shm = and i32 %shc, 31'; echo '  %p = shl i32 1, %shm'; echo '  %A = select i1 %is0, i32 0, i32 %p'
echo '  %A2 = lshr i32 %A, 2'; echo '  %A5 = lshr i32 %A, 5'; echo '  %B1 = or i32 %A, %A2'; echo '  %B = or i32 %B1, %A5'
echo '  %r0 = call i32 @llvm.ctlz.i32(i32 %A, i1 false)'; echo '  %r1 = call i32 @llvm.cttz.i32(i32 %A, i1 false)'; echo '  %r2 = call i32 @llvm.ctpop.i32(i32 %B)'; echo '  %r3 = call i32 @llvm.bitreverse.i32(i32 %A)'
for k in 0 1 2 3; do echo "  %c$k = call float @air.convert.f.f32.u.i32(i32 %r$k)"; done; RET %c0 %c1 %c2 %c3)"
finish bits32 "$B" 'declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.bitreverse.i32(i32)'
# narrow and wide counting: ctlz.i16 / cttz.i8 of the low bits, ctlz.i64 / cttz.i64 of the single-bit 64-bit pattern (X = 0 gives the width)
B="$(echo '  %is0 = icmp eq i32 %X, 0'; echo '  %sh = sub i32 %X, 1'; echo '  %shc = select i1 %is0, i32 0, i32 %sh'; echo '  %shm = and i32 %shc, 31'; echo '  %p = shl i32 1, %shm'; echo '  %A = select i1 %is0, i32 0, i32 %p'
echo '  %A16 = trunc i32 %A to i16'; echo '  %A8 = trunc i32 %A to i8'; echo '  %sh64 = zext i32 %shc to i64'; echo '  %p64 = shl i64 1, %sh64'; echo '  %L = select i1 %is0, i64 0, i64 %p64'
echo '  %r0 = call i16 @llvm.ctlz.i16(i16 %A16, i1 false)'; echo '  %r1 = call i8 @llvm.cttz.i8(i8 %A8, i1 false)'; echo '  %r2 = call i64 @llvm.ctlz.i64(i64 %L, i1 false)'; echo '  %r3 = call i64 @llvm.cttz.i64(i64 %L, i1 false)'
tofl r0 16; tofl r1 8; fold64 r2; fold64 r3; RET %r0.f %r1.f %r2.f %r3.f)"
finish bitsnw "$B" 'declare i16 @llvm.ctlz.i16(i16, i1)
declare i8 @llvm.cttz.i8(i8, i1)
declare i64 @llvm.ctlz.i64(i64, i1)
declare i64 @llvm.cttz.i64(i64, i1)'
# is_zero_poison = true on non-zero inputs only (N = A | 0x8000_0001-style mix keeps it non-zero), ctpop.v2i32 lane 1, bitreverse.i8 / i16
B="$(echo '  %is0 = icmp eq i32 %X, 0'; echo '  %sh = sub i32 %X, 1'; echo '  %shc = select i1 %is0, i32 0, i32 %sh'; echo '  %shm = and i32 %shc, 31'; echo '  %p = shl i32 1, %shm'; echo '  %A = select i1 %is0, i32 0, i32 %p'
echo '  %N = or i32 %A, 2048'; echo '  %r0 = call i32 @llvm.ctlz.i32(i32 %N, i1 true)'; echo '  %r1 = call i32 @llvm.cttz.i32(i32 %N, i1 true)'
echo '  %Bv = or i32 %A, 256'; echo '  %vp0 = insertelement <2 x i32> poison, i32 %A, i64 0'; echo '  %vp = insertelement <2 x i32> %vp0, i32 %Bv, i64 1'; echo '  %vr = call <2 x i32> @llvm.ctpop.v2i32(<2 x i32> %vp)'; echo '  %r2 = extractelement <2 x i32> %vr, i64 1'
echo '  %A16 = trunc i32 %A to i16'; echo '  %r3 = call i16 @llvm.bitreverse.i16(i16 %A16)'; echo '  %r3z = zext i16 %r3 to i32'
for k in 0 1 2; do echo "  %c$k = call float @air.convert.f.f32.u.i32(i32 %r$k)"; done; echo '  %c3 = call float @air.convert.f.f32.u.i32(i32 %r3z)'; RET %c0 %c1 %c2 %c3)"
finish bitsflag "$B" 'declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare <2 x i32> @llvm.ctpop.v2i32(<2 x i32>)
declare i16 @llvm.bitreverse.i16(i16)'
# bswap: i16, i32 (hi / lo), i64 folded
B="$(genin a16 16 %X; genin a32 32 %Y; genin a64 64 %X
echo '  %r0 = call i16 @llvm.bswap.i16(i16 %a16)'; echo '  %r1 = call i32 @llvm.bswap.i32(i32 %a32)'; echo '  %r2 = call i64 @llvm.bswap.i64(i64 %a64)'
tofl r0 16; tofl32 r1; fold64 r2; RET %r0.f %r1.hf %r1.lf %r2.f)"
finish bswap "$B" 'declare i16 @llvm.bswap.i16(i16)
declare i32 @llvm.bswap.i32(i32)
declare i64 @llvm.bswap.i64(i64)'
# funnel shifts: shift amount s = x + 3*y (0..260: past the width, so the modulo is exercised). fshl.i8, fshr.i16, fshl.i32 (hi / lo)
B="$(genin a8 8 %X; genin b8 8 %Y; genin a16 16 %Y; genin b16 16 %X; genin a32 32 %X; genin b32 32 %Y
echo '  %y3 = mul i32 %Y, 3'; echo '  %s = add i32 %X, %y3'; echo '  %s8 = trunc i32 %s to i8'; echo '  %s16 = trunc i32 %s to i16'
echo '  %r0 = call i8 @llvm.fshl.i8(i8 %a8, i8 %b8, i8 %s8)'; echo '  %r1 = call i16 @llvm.fshr.i16(i16 %a16, i16 %b16, i16 %s16)'; echo '  %r2 = call i32 @llvm.fshl.i32(i32 %a32, i32 %b32, i32 %s)'
tofl r0 8; tofl r1 16; tofl32 r2; RET %r0.f %r1.f %r2.hf %r2.lf)"
finish funnel "$B" 'declare i8 @llvm.fshl.i8(i8, i8, i8)
declare i16 @llvm.fshr.i16(i16, i16, i16)
declare i32 @llvm.fshl.i32(i32, i32, i32)'
# fshr.i32 (hi / lo), fshl.i64 folded, rotate (fshl with equal operands) i32 folded
B="$(genin a32 32 %X; genin b32 32 %Y; genin a64 64 %Y; genin b64 64 %X
echo '  %y3 = mul i32 %Y, 3'; echo '  %s = add i32 %X, %y3'; echo '  %s64 = zext i32 %s to i64'
echo '  %r0 = call i32 @llvm.fshr.i32(i32 %a32, i32 %b32, i32 %s)'; echo '  %r1 = call i64 @llvm.fshl.i64(i64 %a64, i64 %b64, i64 %s64)'; echo '  %r2 = call i32 @llvm.fshl.i32(i32 %a32, i32 %a32, i32 %s)'
tofl32 r0; fold64 r1; fold32 r2; RET %r0.hf %r0.lf %r1.f %r2.f)"
finish funnel2 "$B" 'declare i32 @llvm.fshr.i32(i32, i32, i32)
declare i64 @llvm.fshl.i64(i64, i64, i64)
declare i32 @llvm.fshl.i32(i32, i32, i32)'
# fma / fmuladd: a = 1 + x/256, c = -(1 + 2x/256): a*a + c = (x/256)^2 EXACTLY (every product / sum has at most 16 fraction bits), so a fused and an unfused device agree and the pixels test the operand routing and the
# operation, NOT the single rounding of a fused multiply-add (lavapipe does not fuse GLSL Fma; whether Adreno does cannot be told here). fmuladd, v2f32 lane 1, and llvm.fma of other operands
B="$(echo '  %xf = call float @air.convert.f.f32.s.i32(i32 %X)'; echo '  %e = fmul float %xf, 0x3F70000000000000'; echo '  %a = fadd float %e, 1.0'; echo '  %e2 = fmul float %e, 2.0'; echo '  %c0 = fadd float %e2, 1.0'; echo '  %c = fsub float -0.0, %c0'
echo '  %r0 = call float @llvm.fma.f32(float %a, float %a, float %c)'; echo '  %r1 = call float @llvm.fmuladd.f32(float %a, float %a, float %c)'
echo '  %va0 = insertelement <2 x float> poison, float %c, i64 0'; echo '  %va = insertelement <2 x float> %va0, float %a, i64 1'
echo '  %vc0 = insertelement <2 x float> poison, float %c, i64 0'; echo '  %vc = insertelement <2 x float> %vc0, float %c, i64 1'
echo '  %vr = call <2 x float> @llvm.fmuladd.v2f32(<2 x float> %va, <2 x float> %va, <2 x float> %vc)'; echo '  %r2 = extractelement <2 x float> %vr, i64 1'
echo '  %yf = call float @air.convert.f.f32.s.i32(i32 %Y)'; echo '  %d = fmul float %yf, 0x3F70000000000000'; echo '  %r3 = call float @llvm.fma.f32(float %d, float 3.0, float %a)'
RET %r0 %r1 %r2 %r3)"
finish fma "$B" 'declare float @llvm.fma.f32(float, float, float)
declare float @llvm.fmuladd.f32(float, float, float)
declare <2 x float> @llvm.fmuladd.v2f32(<2 x float>, <2 x float>, <2 x float>)'
# control-flow fixtures for tests/air2spv-llvm-unlowered-unit.c: the result of an LLVM intrinsic is the condition of a select (the shape of the video-encode kernel's block dispatch).
#   umaxctl: llvm.umax.i32 (lowered, no undef anywhere);  sinctl: llvm.sin.f32 (NOT lowered: refused by name, or with LAGFX_ALLOW_UNLOWERED_LLVM=1 an undef that reaches the select condition)
B="$(echo '  %m = call i32 @llvm.umax.i32(i32 %X, i32 1)'; echo '  %cnd = icmp ugt i32 %m, 31'; echo '  %o = select i1 %cnd, float 1.0, float 0.25'; RET %o %o %o 1.0)"
finish umaxctl "$B" 'declare i32 @llvm.umax.i32(i32, i32)'
B="$(echo '  %sn = call float @llvm.sin.f32(float %px)'; echo '  %cnd = fcmp ogt float %sn, 0.0'; echo '  %o = select i1 %cnd, float 1.0, float 0.25'; RET %o %o %o 1.0)"
finish sinctl "$B" 'declare float @llvm.sin.f32(float)'
echo "generated $(ls rt_llvm_*_fragment.air.bc | wc -l) fixtures"
