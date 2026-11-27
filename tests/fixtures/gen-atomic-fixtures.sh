#!/bin/sh
# Regenerates the HAND-WRITTEN AIR atomic fixtures of tests/air2spv-atomic-*.c with llvm-as (NOT Apple output). Sources: atomic_*.ll (written here from the heredocs).
#
# SPELLING STATUS (see docs/spec-audit.md, "Atomics"):
#   [V] verified in Apple-compiled modules:  air.atomic.global.load.i32, .store.i32, .xchg.i32, .add.u.i32, .and.u.i32, .or.u.i32 with the operand shapes used below.
#   [N] NOT seen in any Apple binary; written by extrapolating the verified grammar:  air.atomic.global.{sub,xor,min,max}.{s,u}.i32, add.s, air.atomic.local.*, cmpxchg.weak
#       (operand layout: ptr, ptr expected, i32 desired, i32 order, i32 order, i32 scope, i1), .i64 and .f32 forms.
# llvm-as writes OPAQUE pointers; Apple's AIR is typed. The kernels therefore mark device buffers with the "air.buffer" argument metadata (the translator classifies an
# opaque-pointer argument by that metadata) and the translator types the buffer from the accesses (GEP source type / atomic element type).
# Usage: tests/fixtures/gen-atomic-fixtures.sh   (needs llvm-as; the .air.bc files are committed)
set -eu
cd "$(dirname "$0")"
HDR='target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"'
KFTR='!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 7, i32 0}
!2 = !{!"Metal", i32 3, i32 2, i32 0}
!3 = !{}
!air.kernel = !{!20}
!20 = !{ptr @KERNEL, !3, !19}'
# metadata of the buffer arguments: !10 = buffer(0) "c", !11 = buffer(1) "o"; !12 = thread_position_in_grid "tid"
KMD='!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"c"}
!11 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!19 = !{!10, !11, !12}'
# only buffer 0 + tid
KMD1='!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"c"}
!12 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!19 = !{!10, !12}'
KMDTG='!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !12, !13}'
DECL_RMW='declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)
declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.xchg.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.add.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.sub.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.and.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.or.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.xor.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.min.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.max.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.min.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.max.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)'

build() {   # $1 = stem (also the kernel name), $2 = metadata block, stdin = declarations + function body; writes $1.ll and $1.air.bc
  { echo "$HDR"; cat; echo "$KFTR" | sed "s/KERNEL/$1/"; echo "$2"; } > "$1.ll"
  llvm-as "$1.ll" -o "$1.air.bc"
}

# word i of the counter buffer c, as a pointer
gep() { echo "  %$1 = getelementptr i32, ptr addrspace(1) %c, i64 $2"; }

# ---- atomic_rmw: 256 threads; every thread applies one atomic of each kind to its own word of c (see tests/air2spv-atomic-lavapipe.c for the CPU model) ----
gen_rmw() {   # $1 = stem, $2 = opcode used for word 0 (add.u | sub.u)
{
  echo "$DECL_RMW"
  cat <<EOT
define void @$1(ptr addrspace(1) %c, ptr addrspace(1) %o, i32 %tid) {
entry:
  %t1 = add i32 %tid, 1
  %tid7 = add i32 %tid, 7
  %tidm100 = sub i32 %tid, 100
  %bit = and i32 %tid, 31
  %one = shl i32 1, %bit
  %nbit = xor i32 %one, -1
  %t64 = zext i32 %tid to i64
EOT
  gep p0 0; gep p1 1; gep p2 2; gep p3 3; gep p4 4; gep p5 5; gep p6 6; gep p7 7; gep p8 8; gep p9 9; gep p10 10; gep p11 11; gep p12 12; gep p13 13; gep p14 14
  cat <<EOT
  %r0 = call i32 @air.atomic.global.$2.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %r1 = call i32 @air.atomic.global.sub.u.i32(ptr addrspace(1) %p1, i32 1, i32 0, i32 2, i32 0, i1 true)
  %r2 = call i32 @air.atomic.global.and.u.i32(ptr addrspace(1) %p2, i32 %nbit, i32 0, i32 2, i32 0, i1 true)
  %r3 = call i32 @air.atomic.global.or.u.i32(ptr addrspace(1) %p3, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %r4 = call i32 @air.atomic.global.xor.u.i32(ptr addrspace(1) %p4, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %r5 = call i32 @air.atomic.global.min.u.i32(ptr addrspace(1) %p5, i32 %tid7, i32 0, i32 2, i32 0, i1 true)
  %r6 = call i32 @air.atomic.global.max.u.i32(ptr addrspace(1) %p6, i32 %tid7, i32 0, i32 2, i32 0, i1 true)
  %r7 = call i32 @air.atomic.global.min.s.i32(ptr addrspace(1) %p7, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  %r8 = call i32 @air.atomic.global.max.s.i32(ptr addrspace(1) %p8, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  ; exchange: the old value goes to o[tid]
  %r9 = call i32 @air.atomic.global.xchg.i32(ptr addrspace(1) %p9, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %o_x = getelementptr i32, ptr addrspace(1) %o, i64 %t64
  store i32 %r9, ptr addrspace(1) %o_x
  ; add.s of a negative value: c[10] += tid - 100 (signed, wraps as two's complement)
  %r10 = call i32 @air.atomic.global.add.s.i32(ptr addrspace(1) %p10, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  ; atomic store of a constant, then every thread loads c[11] back into o[256 + tid]
  call void @air.atomic.global.store.i32(ptr addrspace(1) %p11, i32 4660, i32 0, i32 2, i1 true)
  %r11 = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %p11, i32 0, i32 2, i1 true)
  %t64b = add i64 %t64, 256
  %o_l = getelementptr i32, ptr addrspace(1) %o, i64 %t64b
  store i32 %r11, ptr addrspace(1) %o_l
  ; ticket: c[14] += 1 returns a distinct old value per thread -> o[512 + tid]
  %r14 = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p14, i32 1, i32 0, i32 2, i32 0, i1 true)
  %t64c = add i64 %t64, 512
  %o_t = getelementptr i32, ptr addrspace(1) %o, i64 %t64c
  store i32 %r14, ptr addrspace(1) %o_t
  ret void
}
EOT
} | build "$1" "$KMD"
}
gen_rmw atomic_rmw add.u
gen_rmw atomic_rmw_neg sub.u    # negative control: word 0 uses sub instead of add

# ---- atomic_cas: every thread increments c[0] with the Metal CAS loop  { T e = atomic_load(c); while (!atomic_compare_exchange_weak(c, &e, e + 1)) {} }  (e is refreshed by the failed exchange) ----
{
  cat <<EOT
declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)
declare i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)
define void @atomic_cas(ptr addrspace(1) %c, i32 %tid) {
entry:
  %exp = alloca i32, align 4
  %p = getelementptr i32, ptr addrspace(1) %c, i64 0
  %init = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %p, i32 0, i32 2, i1 true)
  store i32 %init, ptr %exp, align 4
  br label %loop
loop:
  %e = load i32, ptr %exp, align 4
  %d = add i32 %e, 1
  %ok = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p, ptr %exp, i32 %d, i32 0, i32 0, i32 2, i1 true)
  br i1 %ok, label %done, label %loop
done:
  ret void
}
EOT
} | build atomic_cas "$KMD1"

# negative control for the CAS loop: the desired value is the expected value (no increment) -> the counter stays 0
sed 's/%d = add i32 %e, 1/%d = add i32 %e, 0/; s/@atomic_cas\b/@atomic_cas_neg/g' atomic_cas.ll > atomic_cas_neg.ll
llvm-as atomic_cas_neg.ll -o atomic_cas_neg.air.bc

# ---- atomic_cas_wb: single invocation, c[0] = 5 initially. 1st exchange expects 7 (fails): c[1] = *expected afterwards (must be the observed 5), c[2] = result (0).
#      2nd exchange expects that value (5), desires 9 (succeeds): c[3] = result (1), c[4] = *expected afterwards (5); c[0] ends as 9. ----
{
  cat <<EOT
declare i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)
define void @atomic_cas_wb(ptr addrspace(1) %c, i32 %tid) {
entry:
  %exp = alloca i32, align 4
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i32, ptr addrspace(1) %c, i64 1
  %p2 = getelementptr i32, ptr addrspace(1) %c, i64 2
  %p3 = getelementptr i32, ptr addrspace(1) %c, i64 3
  %p4 = getelementptr i32, ptr addrspace(1) %c, i64 4
  %zero = sub i32 %tid, %tid
  %one = add i32 %zero, 1
  store i32 7, ptr %exp, align 4
  %ok1 = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr %exp, i32 99, i32 0, i32 0, i32 2, i1 true)
  %e1 = load i32, ptr %exp, align 4
  store i32 %e1, ptr addrspace(1) %p1, align 4
  %r1 = select i1 %ok1, i32 %one, i32 %zero
  store i32 %r1, ptr addrspace(1) %p2, align 4
  %ok2 = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr %exp, i32 9, i32 0, i32 0, i32 2, i1 true)
  %r2 = select i1 %ok2, i32 %one, i32 %zero
  store i32 %r2, ptr addrspace(1) %p3, align 4
  %e2 = load i32, ptr %exp, align 4
  store i32 %e2, ptr addrspace(1) %p4, align 4
  ret void
}
EOT
} | build atomic_cas_wb "$KMD1"

# ---- atomic_unknown: an atomic the translator does not implement (a fence) next to a supported one -> the translation must FAIL, never drop it ----
{
  cat <<EOT
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare void @air.atomic.fence(i32, i32)
define void @atomic_unknown(ptr addrspace(1) %c, i32 %tid) {
entry:
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %r0 = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 1, i32 0, i32 2, i32 0, i1 true)
  call void @air.atomic.fence(i32 1, i32 2)
  ret void
}
EOT
} | build atomic_unknown "$KMD1"

# ---- atomic_tg: threadgroup (air.atomic.local.*, addrspace 3) atomics. 64 threads x 4 groups. Statics: @tg = [8 x i32], @tga = [4 x atomic_uint] (Apple's struct.metal::_atomic wrapper).
#   every thread stores the initial values (relaxed atomic stores, identical values), barrier, one atomic of each kind on its group's words, barrier, then reads the words back:
#   o[g*16 + k] = word k (k < 8 from @tg, 8..11 from @tga) (every thread of the group writes the same value), o[64 + g*64 + lid] = the old value of a local fetch_add(1) ticket (a permutation of 0..63) ----
{
  cat <<EOT
%"struct.metal::_atomic" = type { i32 }
@tg = internal unnamed_addr addrspace(3) global [8 x i32] undef, align 4
@tga = internal unnamed_addr addrspace(3) global [4 x %"struct.metal::_atomic"] undef, align 4
declare void @air.wg.barrier(i32, i32)
declare i32 @air.atomic.local.load.i32(ptr addrspace(3), i32, i32, i1)
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.or.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.xor.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.max.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.min.s.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.and.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.xchg.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
define void @atomic_tg(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %lid1 = add i32 %lid, 1
  %lid7 = add i32 %lid, 7
  %lidm20 = sub i32 %lid, 20
  %bit = and i32 %lid, 31
  %one = shl i32 1, %bit
EOT
  for k in 0 1 2 3 4 5 6 7; do echo "  %w$k = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 $k"; done
  for k in 0 1 2 3; do echo "  %a$k = getelementptr inbounds [4 x %\"struct.metal::_atomic\"], ptr addrspace(3) @tga, i64 0, i64 $k, i32 0"; done
  cat <<EOT
  ; initial values: w0 = 0 (sum), w1 = 0 (or), w2 = 0 (xor), w3 = 0 (umax), w4 = 0x7fffffff (smin), w5 = 0xffffffff (and), w6 = 0 (ticket), w7 = 0;  a0 = 0 (sum), a1 = 0 (or), a2 = 5 (xchg), a3 = 0
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w0, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w1, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w2, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w3, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w4, i32 2147483647, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w5, i32 -1, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w6, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w7, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a0, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a1, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a2, i32 5, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a3, i32 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %r0 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w0, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  %r1 = call i32 @air.atomic.local.or.u.i32(ptr addrspace(3) %w1, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %r2 = call i32 @air.atomic.local.xor.u.i32(ptr addrspace(3) %w2, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  %r3 = call i32 @air.atomic.local.max.u.i32(ptr addrspace(3) %w3, i32 %lid7, i32 0, i32 2, i32 0, i1 true)
  %r4 = call i32 @air.atomic.local.min.s.i32(ptr addrspace(3) %w4, i32 %lidm20, i32 0, i32 2, i32 0, i1 true)
  %r5 = call i32 @air.atomic.local.and.u.i32(ptr addrspace(3) %w5, i32 -1, i32 0, i32 2, i32 0, i1 true)
  %tk = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w6, i32 1, i32 0, i32 2, i32 0, i1 true)
  %q0 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %a0, i32 2, i32 0, i32 2, i32 0, i1 true)
  %q1 = call i32 @air.atomic.local.or.u.i32(ptr addrspace(3) %a1, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %q2 = call i32 @air.atomic.local.xchg.i32(ptr addrspace(3) %a2, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %g64 = zext i32 %gid to i64
  %gb = mul i64 %g64, 16
  %l64 = zext i32 %lid to i64
EOT
  for k in 0 1 2 3 4 5 6 7; do
    echo "  %v$k = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w$k, i32 0, i32 2, i1 true)"
    echo "  %ob$k = add i64 %gb, $k"
    echo "  %op$k = getelementptr i32, ptr addrspace(1) %o, i64 %ob$k"
    echo "  store i32 %v$k, ptr addrspace(1) %op$k"
  done
  for k in 0 1 2 3; do
    echo "  %u$k = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %a$k, i32 0, i32 2, i1 true)"
    echo "  %oc$k = add i64 %gb, $((8 + k))"
    echo "  %oq$k = getelementptr i32, ptr addrspace(1) %o, i64 %oc$k"
    echo "  store i32 %u$k, ptr addrspace(1) %oq$k"
  done
  cat <<EOT
  %gt = mul i64 %g64, 64
  %tb = add i64 %gt, %l64
  %tc = add i64 %tb, 64
  %tp = getelementptr i32, ptr addrspace(1) %o, i64 %tc
  store i32 %tk, ptr addrspace(1) %tp
  ret void
}
EOT
} | build atomic_tg "$KMDTG"
# negative control for the threadgroup kernel: word 0 uses local.sub instead of local.add
sed 's/%r0 = call i32 @air.atomic.local.add.u.i32/%r0 = call i32 @air.atomic.local.sub.u.i32/; s/@atomic_tg\b/@atomic_tg_neg/g; s/^declare void @air.wg.barrier(i32, i32)$/&\ndeclare i32 @air.atomic.local.sub.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)/' atomic_tg.ll > atomic_tg_neg.ll
llvm-as atomic_tg_neg.ll -o atomic_tg_neg.air.bc

# ---- atomic_frag_discard (S06): FRAGMENT shader on an 8x8 target: pixels with x < 4 discard_fragment(); every surviving pixel then does atomic_fetch_add(buf[0], 1) and buf[1 + y*8 + x] = 7
#      (a discarded fragment's atomic must not reach memory: Vulkan "Helper Invocations" / SPV_EXT_demote_to_helper_invocation). Buffer 0 is the only resource. ----
cat > atomic_frag_discard.ll <<EOT
; HAND-WRITTEN IR (not Apple output): MSL  fragment float4 F(float4 pos [[position]], device atomic_uint *buf [[buffer(0)]]) { if (pos.x < 4.0) discard_fragment(); atomic_fetch_add_explicit(buf, 1, relaxed); ((device uint *)buf)[1 + y*8 + x] = 7; return red; }
$HDR
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
EOT
llvm-as atomic_frag_discard.ll -o atomic_frag_discard.air.bc

# ---- atomic_i64: 64-bit integer atomics [N spelling: .i64, void result as MSL's atomic_min/max on atomic_ulong, device memory only: MSL 6.16.4.6] -> OpCapability Int64Atomics
#      (LAGFX_VK_CAP_ATOMIC64_BUF). c[0] = umax(tid * 0x100000001), c[1] = umin((tid + 5) * 0x100000001 + 7) (c[1] starts at ~0) ----
{
  cat <<EOT
declare void @air.atomic.global.max.u.i64(ptr addrspace(1), i64, i32, i32, i32, i1)
declare void @air.atomic.global.min.u.i64(ptr addrspace(1), i64, i32, i32, i32, i1)
define void @atomic_i64(ptr addrspace(1) %c, i32 %tid) {
entry:
  %t = zext i32 %tid to i64
  %t5 = add i64 %t, 5
  %m = mul i64 %t, 4294967297
  %m5a = mul i64 %t5, 4294967297
  %m5 = add i64 %m5a, 7
  %p0 = getelementptr i64, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i64, ptr addrspace(1) %c, i64 1
  call void @air.atomic.global.max.u.i64(ptr addrspace(1) %p0, i64 %m, i32 0, i32 2, i32 0, i1 true)
  call void @air.atomic.global.min.u.i64(ptr addrspace(1) %p1, i64 %m5, i32 0, i32 2, i32 0, i1 true)
  ret void
}
EOT
} | build atomic_i64 "$KMD1"

# ---- atomic_order: an add with a NON-relaxed memory order operand (2). The AIR encoding of acquire / release is unknown [N]: the translator must refuse, not weaken it to relaxed ----
{
  cat <<EOT
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
define void @atomic_order(ptr addrspace(1) %c, i32 %tid) {
entry:
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %r0 = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 1, i32 2, i32 2, i32 0, i1 true)
  ret void
}
EOT
} | build atomic_order "$KMD1"

# ---- atomic_f32: 32-bit float add [N spelling: add.f32] -> OpAtomicFAddEXT + AtomicFloat32AddEXT (LAGFX_VK_CAP_ATOMIC_F32_ADD): c[0] += 1.0 per invocation ----
{
  cat <<EOT
declare float @air.atomic.global.add.f32(ptr addrspace(1), float, i32, i32, i32, i1)
define void @atomic_f32(ptr addrspace(1) %c, i32 %tid) {
entry:
  %p0 = getelementptr float, ptr addrspace(1) %c, i64 0
  %r0 = call float @air.atomic.global.add.f32(ptr addrspace(1) %p0, float 1.0, i32 0, i32 2, i32 0, i1 true)
  ret void
}
EOT
} | build atomic_f32 "$KMD1"

# ======================================================================================================================================================================
# Review fixtures (research/g7-atomic2). HAND-WRITTEN, NOT Apple output.
#
# (a) atomic_x_*: ONE atomic call whose operand differs from the combination seen in real Apple modules in exactly one position. The translator's strict grammar must refuse each
#     (named reason, counter); LAGFX_ATOMIC_LENIENT=1 accepts them (tests/air2spv-atomic-strict-unit.c). The observed combinations are load(ptr,0,2,true), store(ptr,v,0,2,true),
#     rmw(ptr,v,0,2,0,true) [V]; the compare-exchange layout (ptr, ptr expected, desired, order, order, scope, i1) is [N].
# ======================================================================================================================================================================
gen_x() {   # $1 = stem, $2 = declarations, $3 = body lines (may use %c %tid %t1 %p0 %p1), $4 = prologue (default: the two i32 element pointers %p0 %p1)
  local pro="${4-  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i32, ptr addrspace(1) %c, i64 1}"
  {
    printf '%s\n' "$2"
    cat <<EOT
define void @$1(ptr addrspace(1) %c, i32 %tid) {
entry:
  %t1 = add i32 %tid, 1
$pro
$3
  ret void
}
EOT
  } | build "$1" "$KMD1"
}
D_LOAD='declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)'
D_STORE='declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1)'
D_RMW='declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)'
D_CAS='declare i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)'
gen_x atomic_x_load_scope1  "$D_LOAD"  '  %r = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %p0, i32 0, i32 1, i1 true)'
gen_x atomic_x_load_flag0   "$D_LOAD"  '  %r = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %p0, i32 0, i32 2, i1 false)'
gen_x atomic_x_store_scope1 "$D_STORE" '  call void @air.atomic.global.store.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 1, i1 true)'
gen_x atomic_x_store_flag0  "$D_STORE" '  call void @air.atomic.global.store.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i1 false)'
gen_x atomic_x_rmw_scope1   "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 1, i32 0, i1 true)'
gen_x atomic_x_rmw_scope3   "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 3, i32 0, i1 true)'
gen_x atomic_x_rmw_scope0   "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 0, i32 0, i1 true)'
gen_x atomic_x_rmw_extra1   "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 1, i1 true)'
gen_x atomic_x_rmw_flag0    "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 0, i1 false)'
gen_x atomic_x_rmw_scopevar "$D_RMW"   '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 %tid, i32 0, i1 true)'
gen_x atomic_x_rmw_flagvar  "$D_RMW"   '  %b = icmp eq i32 %tid, 0
  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 0, i1 %b)'
gen_x atomic_x_rmw_argc5 'declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i1)' '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i1 true)'
gen_x atomic_x_rmw_argc7 'declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1, i32)' '  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 0, i1 true, i32 0)'
gen_x atomic_x_cas_scope1 "$D_CAS" '  %exp = alloca i32, align 4
  store i32 0, ptr %exp, align 4
  %ok = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr %exp, i32 %t1, i32 0, i32 0, i32 1, i1 true)'
gen_x atomic_x_cas_flag0 "$D_CAS" '  %exp = alloca i32, align 4
  store i32 0, ptr %exp, align 4
  %ok = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr %exp, i32 %t1, i32 0, i32 0, i32 2, i1 false)'
# compare-exchange forms the translator does not implement: expected BY VALUE (result i32); i32 result with a pointer; i1 result with the expected pointer in DEVICE memory
gen_x atomic_x_cas_byval 'declare i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), i32, i32, i32, i32, i32, i1)' '  %r = call i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, i32 0, i32 %t1, i32 0, i32 0, i32 2, i1 true)'
gen_x atomic_x_cas_i32ret 'declare i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)' '  %exp = alloca i32, align 4
  store i32 0, ptr %exp, align 4
  %r = call i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr %exp, i32 %t1, i32 0, i32 0, i32 2, i1 true)'
gen_x atomic_x_cas_devptr 'declare i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr addrspace(1), i32, i32, i32, i32, i1)' '  %ok = call i1 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %p0, ptr addrspace(1) %p1, i32 %t1, i32 0, i32 0, i32 2, i1 true)'
# 64-bit: declared return value (the real AIR return type is unobserved); 64-bit load
gen_x atomic_x_i64_ret 'declare i64 @air.atomic.global.max.u.i64(ptr addrspace(1), i64, i32, i32, i32, i1)' '  %q = getelementptr i64, ptr addrspace(1) %c, i64 0
  %r = call i64 @air.atomic.global.max.u.i64(ptr addrspace(1) %q, i64 5, i32 0, i32 2, i32 0, i1 true)' ''
gen_x atomic_x_i64_load 'declare i64 @air.atomic.global.load.i64(ptr addrspace(1), i32, i32, i1)' '  %q = getelementptr i64, ptr addrspace(1) %c, i64 0
  %r = call i64 @air.atomic.global.load.i64(ptr addrspace(1) %q, i32 0, i32 2, i1 true)' ''
# threadgroup add with scope operand 1 (the scope of threadgroup atomics in real AIR is unobserved: strict accepts only 2)
{
  cat <<EOT
@tgx = internal unnamed_addr addrspace(3) global [1 x i32] undef, align 4
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
define void @atomic_x_tg_scope1(ptr addrspace(1) %c, i32 %tid) {
entry:
  %w = getelementptr inbounds [1 x i32], ptr addrspace(3) @tgx, i64 0, i64 0
  %r = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w, i32 1, i32 0, i32 1, i32 0, i1 true)
  ret void
}
EOT
} | build atomic_x_tg_scope1 "$KMD1"

# (b) 64-bit threadgroup atomics (void result, MSL-style), 64-bit device + threadgroup in one kernel, and a threadgroup float add. They exist to prove the CAPABILITY attribution
#     (Int64Atomics on StorageBuffer -> shaderBufferInt64Atomics, on Workgroup -> shaderSharedInt64Atomics; AtomicFloat32AddEXT per storage class) and to run on lavapipe.
#     atomic_i64_tg: 64 threads: local.max.u.i64 of tid * 0x100000001 on @tg64[0], barrier, thread 0 copies @tg64[0] (plain load) to c[0].
{
  cat <<EOT
@tg64 = internal unnamed_addr addrspace(3) global [1 x i64] undef, align 8
declare void @air.wg.barrier(i32, i32)
declare void @air.atomic.local.store.i64(ptr addrspace(3), i64, i32, i32, i1)
declare void @air.atomic.local.max.u.i64(ptr addrspace(3), i64, i32, i32, i32, i1)
define void @atomic_i64_tg(ptr addrspace(1) %c, i32 %tid) {
entry:
  %w = getelementptr inbounds [1 x i64], ptr addrspace(3) @tg64, i64 0, i64 0
  call void @air.atomic.local.store.i64(ptr addrspace(3) %w, i64 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %t = zext i32 %tid to i64
  %m = mul i64 %t, 4294967297
  call void @air.atomic.local.max.u.i64(ptr addrspace(3) %w, i64 %m, i32 0, i32 2, i32 0, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %v = load i64, ptr addrspace(3) %w, align 8
  %p0 = getelementptr i64, ptr addrspace(1) %c, i64 0
  store i64 %v, ptr addrspace(1) %p0
  ret void
}
EOT
} | build atomic_i64_tg "$KMD1"
# atomic_i64_both: device AND threadgroup 64-bit atomics in one kernel (needs both caps)
{
  cat <<EOT
@tg64 = internal unnamed_addr addrspace(3) global [1 x i64] undef, align 8
declare void @air.atomic.global.max.u.i64(ptr addrspace(1), i64, i32, i32, i32, i1)
declare void @air.atomic.local.max.u.i64(ptr addrspace(3), i64, i32, i32, i32, i1)
define void @atomic_i64_both(ptr addrspace(1) %c, i32 %tid) {
entry:
  %w = getelementptr inbounds [1 x i64], ptr addrspace(3) @tg64, i64 0, i64 0
  %t = zext i32 %tid to i64
  %p0 = getelementptr i64, ptr addrspace(1) %c, i64 0
  call void @air.atomic.global.max.u.i64(ptr addrspace(1) %p0, i64 %t, i32 0, i32 2, i32 0, i1 true)
  call void @air.atomic.local.max.u.i64(ptr addrspace(3) %w, i64 %t, i32 0, i32 2, i32 0, i1 true)
  ret void
}
EOT
} | build atomic_i64_both "$KMD1"
# atomic_f32_tg: local.add.f32 on a threadgroup float (needs shaderSharedFloat32AtomicAdd); thread 0 copies the sum (plain load after a barrier) to c[0]
{
  cat <<EOT
@tgf = internal unnamed_addr addrspace(3) global [1 x float] undef, align 4
declare void @air.wg.barrier(i32, i32)
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare float @air.atomic.local.add.f32(ptr addrspace(3), float, i32, i32, i32, i1)
define void @atomic_f32_tg(ptr addrspace(1) %c, i32 %tid) {
entry:
  %w = getelementptr inbounds [1 x float], ptr addrspace(3) @tgf, i64 0, i64 0
  store float 0.0, ptr addrspace(3) %w, align 4
  call void @air.wg.barrier(i32 2, i32 1)
  %r = call float @air.atomic.local.add.f32(ptr addrspace(3) %w, float 1.0, i32 0, i32 2, i32 0, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %v = load float, ptr addrspace(3) %w, align 4
  %p0 = getelementptr float, ptr addrspace(1) %c, i64 0
  store float %v, ptr addrspace(1) %p0
  ret void
}
EOT
} | build atomic_f32_tg "$KMD1"

# (c) helper functions (non-entry) containing atomics: the pointer is a parameter; two call sites; one nested call (a helper calling a helper which holds the atomic).
#     256 invocations: c[0] += 1 (site A) and += 4 (nested, 2 x +2), c[1] += tid + 1 (site B), c[2] ticket (site C) -> o[tid].
#     The ENTRY function must come first in the module (the translator picks the first non-prototype function, as clang emits the kernel before the static helpers it calls).
{
  cat <<EOT
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
define void @atomic_helper(ptr addrspace(1) %c, ptr addrspace(1) %o, i32 %tid) {
entry:
  %t1 = add i32 %tid, 1
  %t64 = zext i32 %tid to i64
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i32, ptr addrspace(1) %c, i64 1
  %p2 = getelementptr i32, ptr addrspace(1) %c, i64 2
  %ra = call i32 @bump(ptr addrspace(1) %p0, i32 1)
  %rb = call i32 @bump(ptr addrspace(1) %p1, i32 %t1)
  %rn = call i32 @bump_twice(ptr addrspace(1) %p0, i32 2)
  %rc = call i32 @bump(ptr addrspace(1) %p2, i32 1)
  %op = getelementptr i32, ptr addrspace(1) %o, i64 %t64
  store i32 %rc, ptr addrspace(1) %op
  ret void
}
define internal i32 @bump(ptr addrspace(1) %p, i32 %v) {
entry:
  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p, i32 %v, i32 0, i32 2, i32 0, i1 true)
  ret i32 %r
}
define internal i32 @bump_twice(ptr addrspace(1) %p, i32 %v) {
entry:
  %a = call i32 @bump(ptr addrspace(1) %p, i32 %v)
  %b = call i32 @bump(ptr addrspace(1) %p, i32 %v)
  %s = add i32 %a, %b
  ret i32 %s
}
EOT
} | build atomic_helper "$KMD"

# (d) helper functions holding THREADGROUP atomics on a module-level threadgroup static (no pointer parameter): value parameters only, so the helpers are registered and emitted.
#     4 groups x 64: every thread does tg_add(1) (site A), tg_add(lid + 1) (site B, a different call site), tg_twice(2) (nested: tg_twice -> tg_add x2 = +4); after a barrier o[gid] = the word.
#     Expected per group: 64 * 1 + (64 * 65 / 2) + 64 * 4 = 2400.
{
  cat <<EOT
@tgh = internal unnamed_addr addrspace(3) global [1 x i32] undef, align 4
declare void @air.wg.barrier(i32, i32)
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
define void @atomic_helper_tg(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %w = getelementptr inbounds [1 x i32], ptr addrspace(3) @tgh, i64 0, i64 0
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w, i32 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %l1 = add i32 %lid, 1
  %ra = call i32 @tg_add(i32 1)
  %rb = call i32 @tg_add(i32 %l1)
  %rn = call i32 @tg_twice(i32 2)
  call void @air.wg.barrier(i32 2, i32 1)
  %v = load i32, ptr addrspace(3) %w, align 4
  %g64 = zext i32 %gid to i64
  %op = getelementptr i32, ptr addrspace(1) %o, i64 %g64
  store i32 %v, ptr addrspace(1) %op
  ret void
}
define internal i32 @tg_add(i32 %v) {
entry:
  %w = getelementptr inbounds [1 x i32], ptr addrspace(3) @tgh, i64 0, i64 0
  %r = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w, i32 %v, i32 0, i32 2, i32 0, i1 true)
  ret i32 %r
}
define internal i32 @tg_twice(i32 %v) {
entry:
  %a = call i32 @tg_add(i32 %v)
  %b = call i32 @tg_add(i32 %v)
  %s = add i32 %a, %b
  ret i32 %s
}
EOT
} | build atomic_helper_tg "$KMDTG"
echo "generated"
