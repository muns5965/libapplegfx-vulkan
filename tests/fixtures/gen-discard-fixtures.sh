#!/bin/sh
# Regenerates the HAND-WRITTEN fixtures of the S06 loop-plan discard tests (tests/arm64-render-compat-unit.c test_discard_loop_plans, tests/air2spv-spec-audit-unit.c), NOT Apple output.
# `call void @air.discard_fragment()` is the [I] spelling (not in the corpus). The two recognised loop plans of translate_function.c are 3-block shapes:
#   plain rotated loop   entry: ... br %H | H: phi...; ...; br i1 %c, %H, %M | M: ...; ret
#   guarded loop         entry: ...; br i1 %g, %H, %M | H: phi...; ...; br i1 %c, %H, %M | M: phi (entry, H) ...; ret
# spec_discard_<shape>_<where>: shape g (guarded) / p (plain), where = none (control) / entry / loop / merge (the block that holds the discard).
# The guarded fixtures enter the loop only for pos.x > 8, so a discard in the loop body is CONDITIONAL per pixel: x < 8 draws red = 0.75 (the merge phi's entry edge), x >= 8 is discarded.
# Unconditional discards (entry / merge / plain loop) discard every pixel. Controls: guarded x < 8 -> 0.75, x >= 8 -> 0.25 (two halvings); plain -> 0.125 (three halvings).
# Usage: tests/fixtures/gen-discard-fixtures.sh   (needs llvm-as; the .ll and .air.bc files are committed)
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
!18 = !{!19}
!19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}'
D='  call void @air.discard_fragment()'
gen() {   # shape where
  shape=$1; where=$2
  e=''; l=''; m=''
  case $where in entry) e=$D;; loop) l=$D;; merge) m=$D;; none) ;; esac
  out=spec_discard_${shape}_${where}
  {
    echo "; HAND-WRITTEN IR (not Apple output): $shape loop plan, discard in: $where [I]. See gen-discard-fixtures.sh"
    echo "$HDR"
    echo 'define <4 x float> @F(<4 x float> %0) {'
    echo 'entry:'
    echo '  %px = extractelement <4 x float> %0, i64 0'
    [ -n "$e" ] && echo "$e"
    if [ $shape = g ]; then
      echo '  %g = fcmp ogt float %px, 8.0'
      echo '  br i1 %g, label %H, label %M'
      echo 'H:'
      echo '  %v = phi float [ 1.0, %entry ], [ %v2, %H ]'
      [ -n "$l" ] && echo "$l"
      echo '  %v2 = fmul float %v, 0.5'
      echo '  %c = fcmp ogt float %v2, 0.375'
      echo '  br i1 %c, label %H, label %M'
      echo 'M:'
      echo '  %r = phi float [ 0.75, %entry ], [ %v2, %H ]'
    else
      echo '  br label %H'
      echo 'H:'
      echo '  %i = phi i32 [ 0, %entry ], [ %i2, %H ]'
      echo '  %v = phi float [ 1.0, %entry ], [ %v2, %H ]'
      [ -n "$l" ] && echo "$l"
      echo '  %v2 = fmul float %v, 0.5'
      echo '  %i2 = add i32 %i, 1'
      echo '  %c = icmp slt i32 %i2, 3'
      echo '  br i1 %c, label %H, label %M'
      echo 'M:'
      echo '  %r = fadd float %v2, 0.0'
    fi
    [ -n "$m" ] && echo "$m"
    echo '  %o = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %r, i64 0'
    echo '  ret <4 x float> %o'
    echo '}'
    echo 'declare void @air.discard_fragment()'
    echo "$MD"
  } > $out.ll
  llvm-as -o $out.air.bc $out.ll
}
for s in g p; do for w in none entry loop merge; do gen $s $w; done; done

# derivative after a discard (if / else diamond: the dispatch lowering, not a loop plan): odd pixels discard, even pixels return dFdx(pos.x) = 1.0 in red and dFdy(pos.y) = 1.0 in green
cat > spec_discard_deriv.ll <<'EOF2'
; HAND-WRITTEN IR (not Apple output): odd x discards, the other pixels read the derivatives AFTER the branch (their quad neighbours are discarded lanes) [I]. See gen-discard-fixtures.sh
EOF2
echo "$HDR" >> spec_discard_deriv.ll
cat >> spec_discard_deriv.ll <<'EOF2'
define <4 x float> @F(<4 x float> %0) {
entry:
  %px = extractelement <4 x float> %0, i64 0
  %py = extractelement <4 x float> %0, i64 1
  %xi = fptosi float %px to i32
  %b = and i32 %xi, 1
  %c = icmp eq i32 %b, 1
  br i1 %c, label %d, label %k
d:
  call void @air.discard_fragment()
  br label %k
k:
  %dx = call float @air.dfdx.f32(float %px)
  %dy = call float @air.dfdy.f32(float %py)
  %o0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %dx, i64 0
  %o1 = insertelement <4 x float> %o0, float %dy, i64 1
  ret <4 x float> %o1
}
declare void @air.discard_fragment()
declare float @air.dfdx.f32(float)
declare float @air.dfdy.f32(float)
EOF2
echo "$MD" >> spec_discard_deriv.ll
llvm-as -o spec_discard_deriv.air.bc spec_discard_deriv.ll
